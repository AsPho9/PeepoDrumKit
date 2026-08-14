#include "chart_editor_playtest.h"
#include "chart_editor_context.h"
#include "chart_editor_settings.h"
#include "audio/audio_engine.h"

namespace PeepoDrumKit
{
	void ChartPlaytest::Start(ChartContext& context)
	{
		Stop(context);

		IsActive = true;

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

		// NOTE: Playtest uses real song playback so the timeline follows along and the audio plays.
		// Sync the song voice to the cursor position before starting playback.
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
		IsActive = false;
		NoteStates.clear();
		LastJudgment = Judgment::None;
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

	void ChartPlaytest::Update(ChartContext& context)
	{
		if (!IsActive)
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
		if (!IsActive)
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
