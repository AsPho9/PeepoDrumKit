#include "chart_editor_playtest.h"
#include "chart_editor_context.h"
#include "chart_editor_settings.h"
#include "imgui/extension/imgui_input_binding.h"
#include "audio/audio_engine.h"
#include <windows.h>
#undef PlaySound
#pragma comment(lib, "winmm.lib") // timeBeginPeriod / timeEndPeriod

namespace PeepoDrumKit
{
	namespace
	{
		// NOTE: Reverse of the backend's `ImGui_ImplWin32_KeyEventToImGuiKey`, covering the keys that can be bound to playtest hits.
		u32 ImGuiKeyToVirtualKey(ImGuiKey key)
		{
			if (key >= ImGuiKey_0 && key <= ImGuiKey_9) return u32('0') + (key - ImGuiKey_0);
			if (key >= ImGuiKey_A && key <= ImGuiKey_Z) return u32('A') + (key - ImGuiKey_A);
			if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24) return 0x70 + (key - ImGuiKey_F1); // VK_F1
			if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9) return 0x60 + (key - ImGuiKey_Keypad0); // VK_NUMPAD0

			switch (key)
			{
				case ImGuiKey_Tab: return 0x09; case ImGuiKey_LeftArrow: return 0x25; case ImGuiKey_RightArrow: return 0x27;
				case ImGuiKey_UpArrow: return 0x26; case ImGuiKey_DownArrow: return 0x28; case ImGuiKey_PageUp: return 0x21;
				case ImGuiKey_PageDown: return 0x22; case ImGuiKey_Home: return 0x24; case ImGuiKey_End: return 0x23;
				case ImGuiKey_Insert: return 0x2D; case ImGuiKey_Delete: return 0x2E; case ImGuiKey_Backspace: return 0x08;
				case ImGuiKey_Space: return 0x20; case ImGuiKey_Enter: return 0x0D; case ImGuiKey_Escape: return 0x1B;
				case ImGuiKey_LeftCtrl: return 0xA2; case ImGuiKey_LeftShift: return 0xA0; case ImGuiKey_LeftAlt: return 0xA4;
				case ImGuiKey_LeftSuper: return 0x5B; case ImGuiKey_RightCtrl: return 0xA3; case ImGuiKey_RightShift: return 0xA1;
				case ImGuiKey_RightAlt: return 0xA5; case ImGuiKey_RightSuper: return 0x5C; case ImGuiKey_Menu: return 0x5D;
				case ImGuiKey_KeypadDecimal: return 0x6E; case ImGuiKey_KeypadDivide: return 0x6F; case ImGuiKey_KeypadMultiply: return 0x6A;
				case ImGuiKey_KeypadSubtract: return 0x6D; case ImGuiKey_KeypadAdd: return 0x6B; case ImGuiKey_KeypadEnter: return 0x0D;
				case ImGuiKey_Apostrophe: return 0xDE; case ImGuiKey_Comma: return 0xBC; case ImGuiKey_Minus: return 0xBD;
				case ImGuiKey_Period: return 0xBE; case ImGuiKey_Slash: return 0xBF; case ImGuiKey_Semicolon: return 0xBA;
				case ImGuiKey_Equal: return 0xBB; case ImGuiKey_LeftBracket: return 0xDB; case ImGuiKey_Backslash: return 0xDC;
				case ImGuiKey_RightBracket: return 0xDD; case ImGuiKey_GraveAccent: return 0xC0;
				default: return 0;
			}
		}

		u32 ImGuiModifiersToMask(ImGuiKeyChord mods)
		{
			u32 mask = 0;
			if (mods & ImGuiMod_Ctrl) mask |= 1u << 0;
			if (mods & ImGuiMod_Shift) mask |= 1u << 1;
			if (mods & ImGuiMod_Alt) mask |= 1u << 2;
			if (mods & ImGuiMod_Super) mask |= 1u << 3;
			return mask;
		}

		// NOTE: Matches InputModifierBehavior::Relaxed: all required modifiers must be down, extra modifiers are fine.
		b8 ArePollModifiersDown(u32 modifierMask)
		{
			if ((modifierMask & (1u << 0)) && !(GetAsyncKeyState(0x11) & 0x8000)) return false; // VK_CONTROL
			if ((modifierMask & (1u << 1)) && !(GetAsyncKeyState(0x10) & 0x8000)) return false; // VK_SHIFT
			if ((modifierMask & (1u << 2)) && !(GetAsyncKeyState(0x12) & 0x8000)) return false; // VK_MENU
			if ((modifierMask & (1u << 3)) && !(GetAsyncKeyState(0x5B) & 0x8000) && !(GetAsyncKeyState(0x5C) & 0x8000)) return false; // VK_LWIN / VK_RWIN
			return true;
		}
	}
	void ChartPlaytest::Start(ChartContext& context)
	{
		Start(context, context.GetCursorBeat());
	}

	void ChartPlaytest::Start(ChartContext& context, Beat startBeat)
	{
		Stop(context);

		IsActive = true;
		IsPaused = false;
		IsFinished = false;

		Combo = 0;
		MaxCombo = 0;
		GoodCount = 0;
		OkCount = 0;
		BadCount = 0;
		TotalNotes = 0;
		LastJudgment = Judgment::None;
		LastJudgmentTime = Time::Zero();

		NoteStates.clear();
		ChartCourse& course = *context.ChartSelectedCourse;
		for (const Note& note : course.GetNotes(context.ChartSelectedBranch))
		{
			NoteStates[&note] = {};
			TotalNotes++;
		}

		OriginalStartBeat = startBeat;

		// NOTE: Start a full measure before the cursor as a lead-in (instead of a countdown), fading the audio in
		//		 and hiding the lead-in measure's notes so the player has time to get ready.
		Beat leadInStartBeat, realStartBeat;
		context.ComputePlaybackLeadInBeats(startBeat, leadInStartBeat, realStartBeat);
		const Time realStartTime = course.TempoMap.BeatToTime(realStartBeat);
		// NOTE: Pre-mark all notes before the real start as completed so they don't register misses or appear in the game preview
		for (auto& [note, state] : NoteStates)
		{
			const Time noteTime = course.TempoMap.BeatToTime(note->BeatTime) + note->TimeOffset;
			if (noteTime < realStartTime)
				state.IsCompleted = true;
		}
		context.PlaybackLeadInActive = true;
		context.PlaybackLeadInStartTime = course.TempoMap.BeatToTime(leadInStartBeat);
		context.PlaybackLeadInEndTime = realStartTime;

		context.SetCursorBeat(leadInStartBeat);
		BeginPlayback(context);
		StartInputPollThread(context);
	}

	void ChartPlaytest::Restart(ChartContext& context)
	{
		if (IsActive)
			Start(context, OriginalStartBeat);
		else
			Start(context);
	}

	void ChartPlaytest::BeginPlayback(ChartContext& context)
	{
		context.SetCursorTime(context.GetCursorTime());
		Audio::Engine.EnsureStreamRunning();
		context.SetIsPlayback(true);
	}

	void ChartPlaytest::Stop(ChartContext& context)
	{
		if (IsActive)
		{
			// NOTE: Only stop the playback that the playtest started
			if (context.GetIsPlayback())
				context.SetIsPlayback(false);
		}
		context.PlaybackLeadInActive = false;
		IsActive = false;
		IsPaused = false;
		InputPollEnabled.store(false, std::memory_order_release);
		StopInputPollThread();
		// NOTE: Keep the score data (combo, hits, etc.) visible after the playtest ends
		IsFinished = true;
		LastJudgment = Judgment::None;
		NoteStates.clear();
	}

	void ChartPlaytest::Pause(ChartContext& context)
	{
		if (!IsActive || IsPaused)
			return;

		if (context.GetIsPlayback())
			context.SetIsPlayback(false);
		IsPaused = true;
		InputPollEnabled.store(false, std::memory_order_release);
		CurrentTime = context.GetCursorTime();
	}

	void ChartPlaytest::Resume(ChartContext& context)
	{
		if (!IsActive || !IsPaused)
			return;

		IsPaused = false;

		ChartCourse& course = *context.ChartSelectedCourse;

		// NOTE: Reset the judgement score so a retry from the lead-in starts with a clean slate
		Combo = 0;
		MaxCombo = 0;
		GoodCount = 0;
		OkCount = 0;
		BadCount = 0;
		LastJudgment = Judgment::None;
		LastJudgmentTime = Time::Zero();

		// NOTE: Re-apply the same lead-in / fade-in as a fresh playtest start: rewind half a measure before the current
		//		 position so the player gets the same fade-in and time to get ready again.
		Beat leadInStartBeat, realStartBeat;
		context.ComputePlaybackLeadInBeats(context.GetCursorBeat(), leadInStartBeat, realStartBeat, 0.5f);
		const Time realStartTime = course.TempoMap.BeatToTime(realStartBeat);
		// NOTE: Notes may have been added/removed while paused, so rebuild the playtest state from the current chart
		RefreshNoteStates(context, realStartTime);
		context.PlaybackLeadInActive = true;
		context.PlaybackLeadInStartTime = course.TempoMap.BeatToTime(leadInStartBeat);
		context.PlaybackLeadInEndTime = realStartTime;

		context.SetCursorBeat(leadInStartBeat);
		CurrentTime = context.GetCursorTime();
		BeginPlayback(context);
	}

	void ChartPlaytest::RefreshNoteStates(ChartContext& context, Time completedBeforeTime)
	{
		NoteStates.clear();
		TotalNotes = 0;
		ChartCourse& course = *context.ChartSelectedCourse;
		for (const Note& note : course.GetNotes(context.ChartSelectedBranch))
		{
			NotePlayState& state = NoteStates[&note];
			TotalNotes++;
			// NOTE: Notes before the real start are marked as completed so they don't register as misses or appear in the lead-in measure
			const Time noteTime = course.TempoMap.BeatToTime(note.BeatTime) + note.TimeOffset;
			if (noteTime < completedBeforeTime)
				state.IsCompleted = true;
		}
	}

	void ChartPlaytest::RegisterNoteHit(ChartContext& context, Judgment judgment, Time hitTime)
	{
		LastJudgment = judgment;
		LastJudgmentTime = hitTime;

		if (judgment == Judgment::Bad)
		{
			Combo = 0;
			BadCount++;
			return;
		}

		Combo++;
		MaxCombo = Max(MaxCombo, Combo);
		if (judgment == Judgment::Good) GoodCount++; else OkCount++;
	}

	void ChartPlaytest::RegisterMiss(ChartContext& context)
	{
		RegisterNoteHit(context, Judgment::Bad, CurrentTime);
	}

	void ChartPlaytest::Update(ChartContext& context, const f32 deltaTimeSec)
	{
		if (!IsActive)
			return;
		(void)deltaTimeSec;

		if (IsPaused)
			return;

		CurrentTime = context.GetCursorTime();

		// NOTE: Apply judgement for hits detected by the high-frequency input polling thread (the sound for these
		//		 was already played immediately by the polling thread, so don't play it again here).
		{
			std::scoped_lock lock(PendingHitsMutex);
			for (const b8 isKa : PendingHits)
				OnHit(context, isKa, false);
			PendingHits.clear();
		}

		ChartCourse& course = *context.ChartSelectedCourse;
		const SortedNotesList& notes = course.GetNotes(context.ChartSelectedBranch);
		const f64 okWindowSec = Time::FromMS(*Settings.General.PlaytestJudgementWindowOkMS).Seconds;

		for (const Note& note : notes)
		{
			auto stateIt = NoteStates.find(&note);
			if (stateIt == cend(NoteStates)) continue;
			NotePlayState& state = stateIt->second;
			if (state.IsMissed || state.IsCompleted) continue;

			const Time noteTime = course.TempoMap.BeatToTime(note.BeatTime) + note.TimeOffset;

			if (IsLongNote(note.Type))
			{
				// NOTE: A hit long note must still be checked for completion once its tail passes,
				//		 otherwise a hit drumroll/balloon would stay on screen forever.
				if (state.IsHit)
				{
					const Time tailTime = (note.BeatDuration > Beat::Zero())
						? course.TempoMap.BeatToTime(note.BeatTime + note.BeatDuration) + note.TimeOffset
						: noteTime;
					if (CurrentTime >= tailTime)
					{
						state.IsCompleted = true;
						// NOTE: Drumrolls simply finish when their tail passes, balloons require enough pops
						if (IsBalloonNote(note.Type) && state.BalloonHits < note.BalloonPopCount)
						{
							state.IsMissed = true;
							RegisterMiss(context);
						}
					}
				}
				else if ((CurrentTime - noteTime).Seconds > okWindowSec)
				{
					state.IsMissed = true;
					RegisterMiss(context);
				}
			}
			else
			{
				// NOTE: Don't miss notes that have already been hit (their state is set in OnHit)
				if (!state.IsHit && (CurrentTime - noteTime).Seconds > okWindowSec)
				{
					state.IsMissed = true;
					RegisterMiss(context);
				}
			}
		}

		if (CurrentTime >= context.Chart.GetDurationOrDefault())
			Stop(context);
	}

	void ChartPlaytest::OnHit(ChartContext& context, b8 isKa, b8 playSound)
	{
		if (!IsActive || IsPaused)
			return;

		const Time now = CurrentTime;
		const f64 goodWindowSec = Time::FromMS(*Settings.General.PlaytestJudgementWindowGoodMS).Seconds;
		const f64 okWindowSec = Time::FromMS(*Settings.General.PlaytestJudgementWindowOkMS).Seconds;

		ChartCourse& course = *context.ChartSelectedCourse;
		const SortedNotesList& notes = course.GetNotes(context.ChartSelectedBranch);

		// NOTE: 1. If a long note is currently active, count this as a drumroll/balloon hit
		for (const Note& note : notes)
		{
			auto stateIt = NoteStates.find(&note);
			if (stateIt == cend(NoteStates)) continue;
			NotePlayState& state = stateIt->second;
			if (!IsLongNote(note.Type) || !state.IsHit || state.IsCompleted || state.IsMissed) continue;

			const Time headTime = course.TempoMap.BeatToTime(note.BeatTime) + note.TimeOffset;
			const Time tailTime = (note.BeatDuration > Beat::Zero())
				? course.TempoMap.BeatToTime(note.BeatTime + note.BeatDuration) + note.TimeOffset
				: headTime;
			if (now < headTime || now > tailTime) continue;

			if (playSound)
				context.SfxVoicePool.PlaySound(isKa ? SoundEffectType::TaikoKa : SoundEffectType::TaikoDon);
			state.LastHitTime = now;

			if (IsDrumrollNote(note.Type))
			{
				state.DrumrollHits++;
				LastJudgment = Judgment::Good;
				LastJudgmentTime = now;
			}
			else if (IsBalloonNote(note.Type))
			{
				state.BalloonHits++;
				if (state.BalloonHits >= note.BalloonPopCount)
				{
					state.IsCompleted = true;
					RegisterNoteHit(context, Judgment::Good, now);
				}
				else
				{
					LastJudgment = Judgment::Ok;
					LastJudgmentTime = now;
				}
			}
			return;
		}

		// NOTE: 2. Find the nearest unjudged matching-type note within the ok window
		const Note* bestNote = nullptr;
		f64 bestDeltaSec = F32Max;
		for (const Note& note : notes)
		{
			auto stateIt = NoteStates.find(&note);
			if (stateIt == cend(NoteStates)) continue;
			const NotePlayState& state = stateIt->second;
			if (state.IsHit || state.IsMissed || state.IsCompleted) continue;

			const b8 isLong = IsLongNote(note.Type);
			const b8 matches = isLong
				? true
				: (isKa ? (IsKaNote(note.Type) || IsKaDonNote(note.Type)) : (IsDonNote(note.Type) || IsKaDonNote(note.Type)));
			if (!matches) continue;

			const Time noteTime = course.TempoMap.BeatToTime(note.BeatTime) + note.TimeOffset;
			const f64 deltaSec = Absolute((noteTime - now).Seconds);
			if (deltaSec <= okWindowSec && deltaSec < bestDeltaSec)
			{
				bestDeltaSec = deltaSec;
				bestNote = &note;
			}
		}

		if (bestNote == nullptr)
		{
			// NOTE: Ghost hit, play the sound but don't change judgement
			if (playSound)
				context.SfxVoicePool.PlaySound(isKa ? SoundEffectType::TaikoKa : SoundEffectType::TaikoDon);
			return;
		}

		auto& bestState = NoteStates[bestNote];
		const Time noteTime = course.TempoMap.BeatToTime(bestNote->BeatTime) + bestNote->TimeOffset;
		if (playSound)
			context.SfxVoicePool.PlaySound(isKa ? SoundEffectType::TaikoKa : SoundEffectType::TaikoDon);

		const Judgment judgment = (bestDeltaSec <= goodWindowSec) ? Judgment::Good : Judgment::Ok;
		RegisterNoteHit(context, judgment, now);

		bestState.IsHit = true;
		bestState.HitTime = now;
		bestState.LastHitTime = now;
	}

	ChartPlaytest::~ChartPlaytest()
	{
		StopInputPollThread();
	}

	void ChartPlaytest::StartInputPollThread(ChartContext& context)
	{
		StopInputPollThread();

		// NOTE: Only the plain-keyboard slots are polled. Any slot that isn't a mappable keyboard key (i.e. mouse
		//		 buttons) means the whole binding falls back to the frame-rate `IsAnyPressed` input path instead.
		auto collectSlots = [](const MultiInputBinding& binding, std::vector<InputPollSlot>& outSlots) -> b8
		{
			outSlots.clear();
			b8 allCovered = true;
			for (const InputBinding& slot : binding)
			{
				if (slot.Type != InputBindingType::Keyboard)
				{
					allCovered = false;
					continue;
				}
				const u32 virtualKey = ImGuiKeyToVirtualKey(static_cast<ImGuiKey>(slot.KeyOrButton));
				if (virtualKey == 0)
				{
					allCovered = false;
					continue;
				}
				InputPollSlot pollSlot;
				pollSlot.VirtualKey = virtualKey;
				pollSlot.ModifierMask = ImGuiModifiersToMask(slot.KeyModifiers());
				// NOTE: Don't fire on keys already held down when the playtest starts
				pollSlot.PrevDown = (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
				outSlots.push_back(pollSlot);
			}
			return allCovered && !outSlots.empty();
		};

		InputPollDonCovered = collectSlots(*Settings.Input.Playtest_HitDon, InputPollDonSlots);
		InputPollKaCovered = collectSlots(*Settings.Input.Playtest_HitKa, InputPollKaSlots);

		if (InputPollDonSlots.empty() && InputPollKaSlots.empty())
			return;

		InputPollRunning.store(true, std::memory_order_release);
		InputPollThread = std::thread(&ChartPlaytest::InputPollThreadMain, this, &context.SfxVoicePool);
	}

	void ChartPlaytest::StopInputPollThread()
	{
		InputPollRunning.store(false, std::memory_order_release);
		if (InputPollThread.joinable())
			InputPollThread.join();
		{
			std::scoped_lock lock(PendingHitsMutex);
			PendingHits.clear();
		}
	}

	void ChartPlaytest::InputPollThreadMain(SoundEffectsVoicePool* sfxPool)
	{
		// NOTE: Raise the global timer resolution so the ~1ms sleep below actually sleeps ~1ms instead of ~15.6ms
		::timeBeginPeriod(1);
		::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
		defer { ::timeEndPeriod(1); };

		while (InputPollRunning.load(std::memory_order_acquire))
		{
			const b8 enabled = InputPollEnabled.load(std::memory_order_acquire);
			auto updateSlot = [&](InputPollSlot& slot, b8 isKa)
			{
				const b8 isDown = ((GetAsyncKeyState(slot.VirtualKey) & 0x8000) != 0) && ArePollModifiersDown(slot.ModifierMask);
				if (enabled && isDown && !slot.PrevDown)
				{
					if (sfxPool != nullptr)
						sfxPool->PlaySound(isKa ? SoundEffectType::TaikoKa : SoundEffectType::TaikoDon);
					{
						std::scoped_lock lock(PendingHitsMutex);
						PendingHits.push_back(isKa);
					}
				}
				slot.PrevDown = isDown;
			};

			for (auto& slot : InputPollDonSlots) updateSlot(slot, false);
			for (auto& slot : InputPollKaSlots) updateSlot(slot, true);
			::Sleep(1);
		}
	}
}
