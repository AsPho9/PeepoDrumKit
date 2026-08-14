#pragma once
#include "core_types.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Audio
{
	enum class StreamShareMode : u8
	{
		Shared,
		Exclusive,
		Count
	};

	struct BackendStreamParam
	{
		u32 SampleRate;
		u32 ChannelCount;
		u32 DesiredFrameCount;
		StreamShareMode ShareMode;

		// NOTE: Name of the ASIO driver to use (ignored by all non-ASIO backends)
		const char* ASIODriverName = nullptr;
	};

	using BackendRenderCallback = std::function<void(i16* outputBuffer, const u32 bufferFrameCount, const u32 bufferChannelCount)>;

	struct IAudioBackend
	{
		virtual ~IAudioBackend() = default;
		virtual b8 OpenStartStream(const BackendStreamParam& param, BackendRenderCallback callback) = 0;
		virtual b8 StopCloseStream() = 0;
		virtual b8 IsOpenRunning() const = 0;
	};

	class WASAPIBackend : public IAudioBackend
	{
	public:
		WASAPIBackend();
		~WASAPIBackend();

	public:
		b8 OpenStartStream(const BackendStreamParam& param, BackendRenderCallback callback) override;
		b8 StopCloseStream() override;
		b8 IsOpenRunning() const override;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};

	class ASIOBackend : public IAudioBackend
	{
	public:
		ASIOBackend();
		~ASIOBackend();

		// NOTE: Public so the ASIO backend implementation source can reference it
		struct Impl;

	public:
		b8 OpenStartStream(const BackendStreamParam& param, BackendRenderCallback callback) override;
		b8 StopCloseStream() override;
		b8 IsOpenRunning() const override;

	private:
		std::unique_ptr<Impl> impl;
	};

	// NOTE: Info about an installed ASIO driver. "Name" is the canonical registry driver name used to load it,
	// "DisplayName" is the human readable name meant to be shown in the settings UI.
	struct ASIODriverInfo
	{
		std::string Name;
		std::string DisplayName;
	};

	// NOTE: Returns all ASIO drivers installed and registered on this system (used by the settings UI)
	std::vector<ASIODriverInfo> ASIOEnumerateDrivers();
}
