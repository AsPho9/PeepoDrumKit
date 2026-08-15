#pragma once
#include "core_types.h"
#include "chart.h"
#include <unordered_map>

namespace PeepoDrumKit
{
	struct ChartContext;

	// NOTE: Playtest feature, lets the user play the chart from the current cursor position using the same input bindings used for placing notes (Don / Ka),
	// with taiko-style judgement and combo shown inside the game preview window. Uses real song playback so the timeline follows along and audio plays.
	struct ChartPlaytest
	{
		enum class Judgment : u8
		{
			None,
			Good,
			Ok,
			Bad,
			Count
		};

		struct NotePlayState
		{
			b8 IsHit = false;
			b8 IsMissed = false;
			b8 IsCompleted = false;
			Time HitTime = Time::Zero();
			Time LastHitTime = Time::Zero();
			i32 DrumrollHits = 0;
			i32 BalloonHits = 0;
		};

		b8 IsActive = false;
		b8 IsPaused = false;
		b8 IsFinished = false;		// NOTE: A playtest has run and ended; score data is kept visible until the next start
		Time CurrentTime = Time::Zero();

		i32 Combo = 0;
		i32 MaxCombo = 0;
		i32 GoodCount = 0;
		i32 OkCount = 0;
		i32 BadCount = 0;
		i32 TotalNotes = 0;

		// NOTE: Keyed by note pointer. Notes can be edited freely while the playtest is paused; the states are rebuilt on resume.
		std::unordered_map<const Note*, NotePlayState> NoteStates;

		Judgment LastJudgment = Judgment::None;
		Time LastJudgmentTime = Time::Zero();

		void Start(ChartContext& context);
		void Restart(ChartContext& context);
		void Stop(ChartContext& context);
		void Update(ChartContext& context, f32 deltaTimeSec);
		void OnHit(ChartContext& context, b8 isKa);
		void Pause(ChartContext& context);
		void Resume(ChartContext& context);
		void RefreshNoteStates(ChartContext& context);

		inline const NotePlayState* TryGetNoteState(const Note* note) const
		{
			auto it = NoteStates.find(note);
			return (it != cend(NoteStates)) ? &it->second : nullptr;
		}

	private:
		void Start(ChartContext& context, Beat startBeat);
		Beat OriginalStartBeat = Beat::Zero();
		void RegisterNoteHit(ChartContext& context, Judgment judgment, Time hitTime);
		void RegisterMiss(ChartContext& context);
		void BeginPlayback(ChartContext& context);
	};
}
