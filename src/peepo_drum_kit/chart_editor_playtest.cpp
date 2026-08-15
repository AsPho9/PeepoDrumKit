#include "chart_editor_playtest.h"
#include "chart_editor_context.h"
#include "chart_editor_settings.h"
#include "audio/audio_engine.h"

namespace PeepoDrumKit
{
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
		CurrentTime = context.GetCursorTime();
	}

	void ChartPlaytest::Resume(ChartContext& context)
	{
		if (!IsActive || !IsPaused)
			return;

		IsPaused = false;

		ChartCourse& course = *context.ChartSelectedCourse;

		// NOTE: Re-apply the same lead-in / fade-in as a fresh playtest start: rewind one measure before the current
		//		 position so the player gets the same fade-in and time to get ready again.
		Beat leadInStartBeat, realStartBeat;
		context.ComputePlaybackLeadInBeats(context.GetCursorBeat(), leadInStartBeat, realStartBeat);
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

	void ChartPlaytest::OnHit(ChartContext& context, b8 isKa)
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
			context.SfxVoicePool.PlaySound(isKa ? SoundEffectType::TaikoKa : SoundEffectType::TaikoDon);
			return;
		}

		auto& bestState = NoteStates[bestNote];
		const Time noteTime = course.TempoMap.BeatToTime(bestNote->BeatTime) + bestNote->TimeOffset;
		context.SfxVoicePool.PlaySound(isKa ? SoundEffectType::TaikoKa : SoundEffectType::TaikoDon);

		const Judgment judgment = (bestDeltaSec <= goodWindowSec) ? Judgment::Good : Judgment::Ok;
		RegisterNoteHit(context, judgment, now);

		bestState.IsHit = true;
		bestState.HitTime = now;
		bestState.LastHitTime = now;
	}
}
