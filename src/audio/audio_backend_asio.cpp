#include "audio_backend.h"
#include "audio_common.h"
#include "core_string.h"

#include <stdio.h>
#include <stdarg.h>
#include <atomic>
#include <vector>

#include <Windows.h>

namespace Audio
{
	// NOTE: The app is a GUI application (no visible console), so ASIO errors are also
	//		 appended to a log file next to the executable to help diagnose driver issues.
	static void LogAsioDebug(const char* fmt, ...)
	{
		char buffer[1024];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buffer, sizeof(buffer), fmt, args);
		va_end(args);

		printf("%s\n", buffer);

		FILE* file = nullptr;
		if (fopen_s(&file, "asio_debug.log", "a") == 0 && file != nullptr)
		{
			fputs(buffer, file);
			fputc('\n', file);
			fclose(file);
		}
	}

	// ---------------------------------------------------------------------------------
	// NOTE: ASIO (Audio Stream Input/Output) driver interface.
	//		 The Steinberg ASIO SDK is a proprietary download, so instead of requiring
	//		 its headers we declare the public IASIO interface (as documented in the
	//		 ASIO 2.3 specification) ourselves and load the driver DLL at runtime.
	//		 See https://www.steinberg.net/developers/
	//
	//		 Modern ASIO drivers (Voicemeeter, FlexASIO, FL Studio ASIO, trgkASIO, ...)
	//		 are in-process COM servers: the driver CLSID is stored under
	//		 HKLM\SOFTWARE\ASIO\{driver name}\CLSID, and the driver object is created
	//		 with CoCreateInstance(clsid, ..., CLSCTX_INPROC_SERVER, clsid, ...) - the
	//		 same GUID is used as both the CLSID and the interface IID (this is also how
	//		 JUCE loads ASIO drivers). Because of that the IASIO interface derives from
	//		 IUnknown, i.e. its vtable starts with QueryInterface/AddRef/Release.
	// ---------------------------------------------------------------------------------

	using ASIOSampleType = long;
	using ASIOBool = long;
	using ASIOSampleRate = double;
	using ASIOError = long;

	static constexpr ASIOError ASE_OK = 0;
	static constexpr ASIOError ASE_SUCCESS = 0x3f4847a0;
	static constexpr ASIOError ASE_NotPresent = -1000;
	static constexpr ASIOError ASE_HWMalfunction = -1001;
	static constexpr ASIOError ASE_InvalidParameter = -1002;
	static constexpr ASIOError ASE_InvalidMode = -1003;
	static constexpr ASIOError ASE_SPNotAdvancing = -1004;
	static constexpr ASIOError ASE_NoClock = -1005;
	static constexpr ASIOError ASE_NoMemory = -1006;

	static constexpr ASIOBool ASIOTrue = 1;
	static constexpr ASIOBool ASIOFalse = 0;

	// NOTE: ASIO 2.3 sample type identifiers (LSB = little endian, which is what x86 uses)
	static constexpr ASIOSampleType ASIOSTInt16LSB = 16;
	static constexpr ASIOSampleType ASIOSTInt24LSB = 17;
	static constexpr ASIOSampleType ASIOSTInt32LSB = 18;
	static constexpr ASIOSampleType ASIOSTFloat32LSB = 19;
	static constexpr ASIOSampleType ASIOSTFloat64LSB = 20;

	struct ASIOTimeStamp { long double the1; long double the2; };
	struct ASIOSamples { long double hi; long double lo; };

	struct ASIOTime; // NOTE: Only used by the optional bufferSwitchTimeInfo callback which we don't use

	struct ASIOBufferInfo
	{
		ASIOBool isInput;
		long channelNum;
		void* buffers[2];
	};

	struct ASIOChannelInfo
	{
		long channel;
		ASIOBool isInput;
		ASIOBool isActive;
		long channelGroup;
		ASIOSampleType type;
		char name[32];
	};

	struct ASIOClockSource
	{
		long index;
		long associatedChannel;
		long associatedGroup;
		ASIOBool isCurrentSource;
		char name[32];
	};

	struct ASIOCallbacks
	{
		// NOTE: `doublePrecision` is a long (32-bit) buffer index (0 or 1), NOT a long double.
		//		 Using long double here misreads the packed argument on x64, always yielding
		//		 index 0 which breaks the driver's double-buffering and causes crackling.
		void (*bufferSwitch)(long doublePrecision, ASIOBool process);
		void (*sampleRateDidChange)(ASIOSampleRate sRate);
		long (*asioMessage)(long selector, long value, void* message, double* opt);
		ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doublePrecision, ASIOBool process);
	};

	// NOTE: The public ASIO driver interface, as specified in the ASIO 2.3 specification.
	//		 Modern drivers expose it as a COM interface deriving from IUnknown, so the
	//		 vtable is: QueryInterface, AddRef, Release, init, getDriverName, ..., outputReady.
	class IASIO : public IUnknown
	{
	public:
		virtual ASIOBool init(void* sysHandle) = 0;
		virtual void getDriverName(char* name) = 0;
		virtual long getDriverVersion() = 0;
		virtual void getErrorMessage(char* string) = 0;
		virtual ASIOError start() = 0;
		virtual ASIOError stop() = 0;
		virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) = 0;
		virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) = 0;
		virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
		virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
		virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) = 0;
		virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
		virtual ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
		virtual ASIOError setClockSource(long reference) = 0;
		virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
		virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
		virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) = 0;
		virtual ASIOError disposeBuffers() = 0;
		virtual ASIOError controlPanel() = 0;
		virtual ASIOError future(long selector, void* opt) = 0;
		virtual ASIOError outputReady() = 0;
	};

	// NOTE: ASIO buffers are double-buffered, the driver alternates between buffer index 0 and 1
	static constexpr long AsioDoubleBufferCount = 2;

	// NOTE: Sanity cap for the requested buffer size, ASIO drivers typically use 256-2048 frames
	static constexpr long AsioMaxBufferFrameCount = 4096;

	static b8 ReadRegistryStringValue(HKEY parentKey, const std::wstring& subKey, const std::wstring& valueName, std::wstring& outValue)
	{
		HKEY key = nullptr;
		if (::RegOpenKeyExW(parentKey, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
			return false;

		DWORD size = 0;
		if (::RegQueryValueExW(key, valueName.c_str(), nullptr, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0)
		{
			::RegCloseKey(key);
			return false;
		}

		outValue.resize((size / sizeof(wchar_t)) - 1);
		::RegQueryValueExW(key, valueName.c_str(), nullptr, nullptr, reinterpret_cast<LPBYTE>(outValue.data()), &size);
		::RegCloseKey(key);
		return true;
	}

	// NOTE: Some drivers misbehave when they fail, so like JUCE we guard the COM interaction
	//		 with SEH. These helpers must stay free of C++ objects (no destructors), which is
	//		 also required because this project is compiled with exceptions disabled.
	static b8 TryCreateAsioDriver(const CLSID clsid, IASIO*& outDriver)
	{
		__try
		{
			outDriver = nullptr;
			const HRESULT hr = ::CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&outDriver));
			return SUCCEEDED(hr) && outDriver != nullptr;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			outDriver = nullptr;
			return false;
		}
	}

	static void SafeReleaseAsioDriver(IASIO* driver)
	{
		if (driver == nullptr)
			return;
		__try { driver->Release(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { }
	}

	// NOTE: The ASIOCallbacks struct has no user-data field, so like the ASIO SDK sample host
	//		 we use a global pointer to the currently active backend instance. The engine
	//		 only ever has a single open audio stream at a time.
	static ASIOBackend::Impl* g_CurrentASIOBackendImpl = nullptr;

	struct ASIOBackend::Impl
	{
	public:
		b8 OpenStartStream(const BackendStreamParam& param, BackendRenderCallback callback)
		{
			if (isOpenRunning)
				return false;

			streamParam = param;
			renderCallback = std::move(callback);
			asioDriverName = (param.ASIODriverName != nullptr) ? param.ASIODriverName : "";

			if (!LoadDriver(asioDriverName))
			{
				LogAsioDebug(__FUNCTION__"(): Unable to load ASIO driver '%s'. Make sure it's installed and selected in the settings.", asioDriverName.c_str());
				return false;
			}

			if (!driver->init(::GetModuleHandle(nullptr)))
			{
				LogAsioDebug(__FUNCTION__"(): ASIO driver '%s' failed to initialize.", asioDriverName.c_str());
				UnloadDriver();
				return false;
			}

			long inputChannelCount = 0, outputChannelCount = 0;
			if (driver->getChannels(&inputChannelCount, &outputChannelCount) != ASE_OK)
			{
				LogAsioDebug(__FUNCTION__"(): ASIO driver '%s' failed to report its channels.", asioDriverName.c_str());
				UnloadDriver();
				return false;
			}

			const long nOutputBuffers = static_cast<long>(streamParam.ChannelCount);
			if (outputChannelCount < nOutputBuffers)
			{
				LogAsioDebug(__FUNCTION__"(): ASIO driver '%s' only has %d output channels, %d are required.", asioDriverName.c_str(), outputChannelCount, nOutputBuffers);
				UnloadDriver();
				return false;
			}

			long minBufferSize = 0, maxBufferSize = 0, preferredBufferSize = 0, bufferSizeGranularity = 0;
			if (driver->getBufferSize(&minBufferSize, &maxBufferSize, &preferredBufferSize, &bufferSizeGranularity) != ASE_OK)
			{
				LogAsioDebug(__FUNCTION__"(): ASIO driver '%s' failed to report its buffer sizes.", asioDriverName.c_str());
				UnloadDriver();
				return false;
			}

			long bufferFrameCount = (preferredBufferSize > 0) ? preferredBufferSize : static_cast<long>(streamParam.DesiredFrameCount);
			if (minBufferSize > 0)
				bufferFrameCount = Max(bufferFrameCount, minBufferSize);
			if (maxBufferSize > 0)
				bufferFrameCount = Min(bufferFrameCount, maxBufferSize);
			bufferFrameCount = Min(bufferFrameCount, AsioMaxBufferFrameCount);

			// NOTE: Try to use the requested sample rate, fall back to the driver's current rate if unsupported
			ASIOSampleRate actualSampleRate = static_cast<ASIOSampleRate>(streamParam.SampleRate);
			if (driver->canSampleRate(actualSampleRate) == ASE_OK)
			{
				if (driver->setSampleRate(actualSampleRate) != ASE_OK)
					LogAsioDebug(__FUNCTION__"(): ASIO driver '%s' failed to set the sample rate to %u.", asioDriverName.c_str(), streamParam.SampleRate);
			}
			else
			{
				if (driver->getSampleRate(&actualSampleRate) != ASE_OK)
				{
					LogAsioDebug(__FUNCTION__"(): ASIO driver '%s' neither supports the requested sample rate nor reports its own.", asioDriverName.c_str());
					UnloadDriver();
					return false;
				}
				LogAsioDebug(__FUNCTION__"(): ASIO driver '%s' doesn't support %u Hz, using its native rate %.0f Hz.", asioDriverName.c_str(), streamParam.SampleRate, actualSampleRate);
			}

			bufferFrameCount_ = bufferFrameCount;
			outputBufferCount = nOutputBuffers;

			bufferInfos = std::make_unique<ASIOBufferInfo[]>(nOutputBuffers);
			for (long c = 0; c < nOutputBuffers; c++)
			{
				bufferInfos[c] = {};
				bufferInfos[c].isInput = false;
				bufferInfos[c].channelNum = c;
			}

			outputChannelTypes.clear();
			outputChannelTypes.reserve(nOutputBuffers);
			for (long c = 0; c < nOutputBuffers; c++)
			{
				ASIOChannelInfo channelInfo = {};
				channelInfo.channel = c;
				channelInfo.isInput = false;
				if (driver->getChannelInfo(&channelInfo) == ASE_OK)
					outputChannelTypes.push_back(channelInfo.type);
				else
					outputChannelTypes.push_back(ASIOSTInt16LSB);
			}

			// NOTE: The ASIOCallbacks struct must outlive the stream: Voicemeeter (and likely other
			//		 drivers) keep a pointer to it and read it from their callback thread on every
			//		 buffer switch, so it must NOT be a local variable here. It is a member instead.
			asioCallbacks = {};
			asioCallbacks.bufferSwitch = &Impl::ASIOBufferSwitchCallback;
			asioCallbacks.sampleRateDidChange = &Impl::ASIOSampleRateDidChangeCallback;
			asioCallbacks.asioMessage = &Impl::ASIOAsioMessageCallback;
			asioCallbacks.bufferSwitchTimeInfo = nullptr;

			if (driver->createBuffers(bufferInfos.get(), nOutputBuffers, bufferFrameCount, &asioCallbacks) != ASE_OK)
			{
				LogAsioDebug(__FUNCTION__"(): ASIO driver '%s' failed to create the output buffers.", asioDriverName.c_str());
				UnloadDriver();
				return false;
			}

			scratchInterleavedBuffer.resize(static_cast<size_t>(bufferFrameCount) * streamParam.ChannelCount);

			// NOTE: Only now can the driver invoke the callbacks, so expose the instance to them
			g_CurrentASIOBackendImpl = this;
			isOpenRunning = true;

			if (driver->start() != ASE_OK)
			{
				isOpenRunning = false;
				g_CurrentASIOBackendImpl = nullptr;
				LogAsioDebug(__FUNCTION__"(): ASIO driver '%s' failed to start.", asioDriverName.c_str());
				driver->disposeBuffers();
				UnloadDriver();
				return false;
			}

			LogAsioDebug(__FUNCTION__"(): ASIO driver '%s' started with %d output buffer(s) of %d frames.", asioDriverName.c_str(), outputBufferCount, bufferFrameCount);
			return true;
		}

		b8 StopCloseStream()
		{
			if (!isOpenRunning)
				return false;

			isOpenRunning = false;
			g_CurrentASIOBackendImpl = nullptr;

			if (driver != nullptr)
			{
				driver->stop();
				// NOTE: Give the driver a moment to finish any in-flight buffer switch before tearing down
				::Sleep(5);
				driver->disposeBuffers();
			}

			UnloadDriver();
			bufferInfos.reset();
			outputChannelTypes.clear();
			scratchInterleavedBuffer.clear();
			return true;
		}

		b8 IsOpenRunning() const
		{
			return isOpenRunning;
		}

	private:
		void RenderBuffers(const long bufferIndex)
		{
			const u32 channelCount = streamParam.ChannelCount;
			renderCallback(scratchInterleavedBuffer.data(), bufferFrameCount_, channelCount);

			for (long c = 0; c < outputBufferCount; c++)
			{
				void* asioBuffer = bufferInfos[c].buffers[bufferIndex];
				if (asioBuffer == nullptr)
					continue;

				WriteChannelSamples(asioBuffer, outputChannelTypes[c], c, channelCount);
			}

			// NOTE: Tells the driver the output buffers are ready to be read (not used in direct-access mode)
			driver->outputReady();
		}

		void WriteChannelSamples(void* asioBuffer, const ASIOSampleType sampleType, const long channelIndex, const u32 channelCount)
		{
			const i16* src = scratchInterleavedBuffer.data() + channelIndex;
			const size_t frameCount = static_cast<size_t>(bufferFrameCount_);

			switch (sampleType)
			{
			case ASIOSTInt16LSB:
			{
				i16* dst = static_cast<i16*>(asioBuffer);
				for (size_t f = 0; f < frameCount; f++)
					dst[f] = src[f * channelCount];
			} break;
			case ASIOSTInt24LSB:
			{
				u8* dst = static_cast<u8*>(asioBuffer);
				for (size_t f = 0; f < frameCount; f++)
				{
					const i32 v = static_cast<i32>(src[f * channelCount]) << 8;
					dst[f * 3 + 0] = static_cast<u8>(v & 0xFF);
					dst[f * 3 + 1] = static_cast<u8>((v >> 8) & 0xFF);
					dst[f * 3 + 2] = static_cast<u8>((v >> 16) & 0xFF);
				}
			} break;
			case ASIOSTInt32LSB:
			{
				i32* dst = static_cast<i32*>(asioBuffer);
				for (size_t f = 0; f < frameCount; f++)
					dst[f] = static_cast<i32>(src[f * channelCount]) << 16;
			} break;
			case ASIOSTFloat32LSB:
			{
				f32* dst = static_cast<f32*>(asioBuffer);
				for (size_t f = 0; f < frameCount; f++)
					dst[f] = ConvertSampleI16ToF32(src[f * channelCount]);
			} break;
			case ASIOSTFloat64LSB:
			{
				f64* dst = static_cast<f64*>(asioBuffer);
				for (size_t f = 0; f < frameCount; f++)
					dst[f] = static_cast<f64>(ConvertSampleI16ToF32(src[f * channelCount]));
			} break;
			default:
			{
				// NOTE: Unsupported sample format, write the raw 16-bit samples anyway rather than outputting silence
				i16* dst = static_cast<i16*>(asioBuffer);
				for (size_t f = 0; f < frameCount; f++)
					dst[f] = src[f * channelCount];
			} break;
			}
		}

		b8 LoadDriver(const std::string& driverName)
		{
			if (driverName.empty())
				return false;
			// NOTE: The driver is a COM server. Its CLSID is registered under
			//		 HKLM\SOFTWARE\ASIO\{name}\CLSID, and CoCreateInstance resolves the DLL
			//		 itself through HKCR\CLSID\{clsid}\InprocServer32.
			std::wstring clsidW;
			{
				const std::wstring subKey = L"SOFTWARE\\ASIO\\" + UTF8::Widen(driverName);
				if (!ReadRegistryStringValue(HKEY_LOCAL_MACHINE, subKey, L"CLSID", clsidW))
				{
					LogAsioDebug(__FUNCTION__"(): No CLSID found in the registry for ASIO driver '%s'.", driverName.c_str());
					return false;
				}
			}

			CLSID clsid = {};
			if (::CLSIDFromString(const_cast<wchar_t*>(clsidW.c_str()), &clsid) != S_OK)
			{
				LogAsioDebug(__FUNCTION__"(): Failed to parse the CLSID '%S' of ASIO driver '%s'.", clsidW.c_str(), driverName.c_str());
				return false;
			}

			// NOTE: COM must be initialized on this thread before creating the driver object.
			//		 CoInitializeEx returning S_OK means we own one initialization, which we
			//		 match with a CoUninitialize in UnloadDriver. S_FALSE means it was already
			//		 initialized here by something else (or a previous run), so we don't pair it.
			if (!comInitialized)
			{
				const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
				if (hr == RPC_E_CHANGED_MODE)
				{
					// NOTE: Thread is already in the multi-threaded apartment, keep using it as-is
					comInitialized = false;
				}
				else if (FAILED(hr))
				{
					LogAsioDebug(__FUNCTION__"(): CoInitializeEx failed with 0x%08X.", static_cast<unsigned>(hr));
					return false;
				}
				else
				{
					comInitialized = (hr == S_OK);
				}
			}

			IASIO* loadedDriver = nullptr;
			if (!TryCreateAsioDriver(clsid, loadedDriver))
			{
				LogAsioDebug(__FUNCTION__"(): Failed to create the ASIO driver object '%s' (is the driver DLL registered and loadable?).", driverName.c_str());
				return false;
			}

			driver = loadedDriver;
			LogAsioDebug(__FUNCTION__"(): Loaded ASIO driver '%s'.", driverName.c_str());
			return true;
		}

		void UnloadDriver()
		{
			if (driver != nullptr)
			{
				SafeReleaseAsioDriver(driver);
				driver = nullptr;
			}
			if (comInitialized)
			{
				::CoUninitialize();
				comInitialized = false;
			}
		}

		static void ASIOBufferSwitchCallback(long doublePrecision, ASIOBool process)
		{
			if (g_CurrentASIOBackendImpl == nullptr)
				return;

			// NOTE: The bufferSwitch callback runs on the driver's own real-time thread. Raise its
			//		 priority on the first callback so the render work is less likely to be preempted,
			//		 which would cause buffer underruns and crackling (especially when minimized).
			if (!g_CurrentASIOBackendImpl->callbackThreadPrioritySet.exchange(true))
				::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

			const long bufferIndex = doublePrecision % AsioDoubleBufferCount;
			g_CurrentASIOBackendImpl->RenderBuffers(bufferIndex);
		}

		static void ASIOSampleRateDidChangeCallback(ASIOSampleRate sampleRate)
		{
		}

		static long ASIOAsioMessageCallback(long selector, long value, void* message, double* opt)
		{
			return 0;
		}

	private:
		BackendStreamParam streamParam = {};
		BackendRenderCallback renderCallback;
		std::string asioDriverName;

		std::atomic<b8> isOpenRunning = false;
		b8 comInitialized = false;
		std::atomic<b8> callbackThreadPrioritySet = false;

		IASIO* driver = nullptr;

		// NOTE: Kept as a member so it lives as long as the stream (the driver holds a pointer to it)
		ASIOCallbacks asioCallbacks = {};

		long bufferFrameCount_ = 0;
		long outputBufferCount = 0;
		std::vector<ASIOSampleType> outputChannelTypes;

		std::unique_ptr<ASIOBufferInfo[]> bufferInfos;
		std::vector<i16> scratchInterleavedBuffer;
	};

	ASIOBackend::ASIOBackend() : impl(std::make_unique<Impl>()) {}
	ASIOBackend::~ASIOBackend() = default;
	b8 ASIOBackend::OpenStartStream(const BackendStreamParam& param, BackendRenderCallback callback) { return impl->OpenStartStream(param, std::move(callback)); }
	b8 ASIOBackend::StopCloseStream() { return impl->StopCloseStream(); }
	b8 ASIOBackend::IsOpenRunning() const { return impl->IsOpenRunning(); }

	std::vector<ASIODriverInfo> ASIOEnumerateDrivers()
	{
		std::vector<ASIODriverInfo> drivers;

		HKEY asioKey = nullptr;
		if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &asioKey) != ERROR_SUCCESS)
			return drivers;

		wchar_t subKeyNameBuffer[256];
		for (DWORD index = 0; ; index++)
		{
			DWORD nameLength = static_cast<DWORD>(ArrayCount(subKeyNameBuffer));
			const LSTATUS result = ::RegEnumKeyExW(asioKey, index, subKeyNameBuffer, &nameLength, nullptr, nullptr, nullptr, nullptr);
			if (result == ERROR_NO_MORE_ITEMS)
				break;
			if (result != ERROR_SUCCESS)
				continue;

			const std::wstring nameW(subKeyNameBuffer, nameLength);
			const std::string name = UTF8::Narrow(nameW);

			// NOTE: Prefer the "Description" registry value for a nicer display name
			std::string displayName = name;
			{
				const std::wstring subKey = L"SOFTWARE\\ASIO\\" + nameW;
				std::wstring descriptionW;
				if (ReadRegistryStringValue(HKEY_LOCAL_MACHINE, subKey, L"Description", descriptionW))
					displayName = UTF8::Narrow(descriptionW);
			}

			drivers.push_back({ name, displayName });
		}

		::RegCloseKey(asioKey);
		return drivers;
	}
}
