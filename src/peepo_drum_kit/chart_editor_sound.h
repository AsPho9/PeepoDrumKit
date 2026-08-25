#pragma once
#include "core_types.h"
#include "audio/audio_engine.h"
#include <mutex>
#include <optional>
#include <string>

namespace PeepoDrumKit
{
	enum class SoundEffectType : u8
	{
		TaikoDon,
		TaikoKa,
		MetronomeBar,
		MetronomeBeat,
		Count
	};

	// NOTE: Extension-less paths. At load time each of the supported audio format extensions (.ogg/.wav/.flac/.mp3)
	//		 is probed and the first matching file gets loaded - so every sound effect can use any supported format.
	static constexpr cstr SoundEffectTypeFileBaseNames[] =
	{
		u8"assets/audio/taiko_don",
		u8"assets/audio/taiko_ka",
		u8"assets/audio/metronome_bar",
		u8"assets/audio/metronome_beat",
	};

	static_assert(ArrayCount(SoundEffectTypeFileBaseNames) == EnumCount<SoundEffectType>);

	struct AsyncLoadSoundEffectsResult
	{
		std::string FilePaths[EnumCount<SoundEffectType>];
		Audio::PCMSampleBuffer SampleBuffers[EnumCount<SoundEffectType>];
	};

	enum class SoundGroup : i32
	{
		Master = 0,
		Metronome = 1,
		SoundEffects = 2,
		Count,
	};

	struct SoundEffectsVoicePool
	{
		SoundEffectsVoicePool() { for (auto& handle : LoadedSources) handle = Audio::SourceHandle::Invalid; }
		void StartAsyncLoadingAndAddVoices();
		void UpdateAsyncLoading();
		void UnloadAllSourcesAndVoices();

		void PlaySound(SoundEffectType type, Time startTime = Time::Zero(), std::optional<Time> externalClock = {}, f32 pan = 0);
		void PauseAllFutureVoices();
		Audio::SourceHandle TryGetSourceForType(SoundEffectType type) const;

		inline void SetSoundGroupVolume(SoundGroup soundGroup, f32 value) { Audio::Engine.SetSoundGroupVolume(EnumToIndex(soundGroup), value); }
		inline f32 GetSoundGroupVolume(SoundGroup soundGroup) { return Audio::Engine.GetSoundGroupVolume(EnumToIndex(soundGroup)); }

		i32 VoicePoolRingIndex = 0;
		static constexpr size_t VoicePoolSize = 64;
		Audio::Voice VoicePool[VoicePoolSize] = {};

		// NOTE: Serializes PlaySound/PauseAllFutureVoices so the high-frequency playtest input polling thread
		//		 can safely play sounds concurrently with the main thread.
		std::mutex PlayMutex;

		Audio::SourceHandle LoadedSources[EnumCount<SoundEffectType>] = {};
		std::future<AsyncLoadSoundEffectsResult> LoadSoundEffectFuture = {};
	};
}
