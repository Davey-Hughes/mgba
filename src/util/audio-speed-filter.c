/* Copyright (c) 2026 Davey Hughes
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba-util/audio-speed-filter.h>

bool mAudioSpeedFilterInit(struct mAudioSpeedFilter* f, double sampleRate) {
	memset(f, 0, sizeof(*f));
	f->lowPassReferenceHz = M_AUDIO_LOW_PASS_OFF;
	f->enabled = true;
	f->engaged = false;
	f->arrivalAvg = 0.0;
	f->speed = 1.0;

	if (!(sampleRate > 0.0) || !isfinite(sampleRate)) {
		// A bad rate divides to infinity/NaN in _finish and _design.
		f->enabled = false;
		return false;
	}
	f->sampleRate = sampleRate;

	if (!mAudioTimeStretchInit(&f->stretch)) {
		// Disable rather than go silent with a half-initialised struct.
		f->enabled = false;
		return false;
	}
	mAudioLowPassInit(&f->lowPass, sampleRate);
	mAudioStreamRampInit(&f->ramp);

	// Or the first Read reads any pre-existing backlog as a single delivery.
	f->lastWritten = mAudioTimeStretchTotalWritten(&f->stretch);
	return true;
}

void mAudioSpeedFilterDeinit(struct mAudioSpeedFilter* f) {
	mAudioTimeStretchDeinit(&f->stretch);
}

void mAudioSpeedFilterSetSampleRate(struct mAudioSpeedFilter* f, double sampleRate) {
	if (!(sampleRate > 0.0) || !isfinite(sampleRate)) {
		return;
	}
	if (sampleRate == f->sampleRate) {
		return;
	}

	f->sampleRate = sampleRate;
	mAudioLowPassInit(&f->lowPass, sampleRate);
	mAudioStreamRampReset(&f->ramp);

	mAudioTimeStretchBeginSession(&f->stretch);

	// `engaged` is left alone: a rate change doesn't end fast-forward.
	f->arrivalAvg = 0.0;
	f->lastWritten = mAudioTimeStretchTotalWritten(&f->stretch);
	f->speed = 1.0;
	f->offSpeedRuns = 0;
	f->onSpeedRuns = 0;
	f->windowArrival = 0;
	f->windowOutput = 0;
	f->windowOpen = false;
}

void mAudioSpeedFilterSetEnabled(struct mAudioSpeedFilter* f, bool enabled) {
	if (f->enabled == enabled) {
		return;
	}
	if (!enabled) {
		f->enabled = false;
		f->engaged = false;
		f->offSpeedRuns = 0;
		f->onSpeedRuns = 0;
		// Write and Read early-return from here on, so nothing reaches the rings.
		mAudioTimeStretchDeinit(&f->stretch);
		return;
	}

	// A failed Init leaves no usable rate to design the low-pass against.
	if (!(f->sampleRate > 0.0)) {
		return;
	}
	if (!mAudioTimeStretchInit(&f->stretch)) {
		return;
	}
	f->enabled = true;
	// And the ramp, or a toggle mid-tail resumes a stale one.
	mAudioStreamRampReset(&f->ramp);
	f->suspended = false;
	f->arrivalAvg = 0.0;
	f->lastWritten = mAudioTimeStretchTotalWritten(&f->stretch);
	f->speed = 1.0;
	f->windowArrival = 0;
	f->windowOutput = 0;
	f->windowOpen = false;
}

bool mAudioSpeedFilterEnabled(const struct mAudioSpeedFilter* f) {
	return f->enabled;
}

void mAudioSpeedFilterSetLowPass(struct mAudioSpeedFilter* f, int referenceHz) {
	// The Qt spinbox already clamps to this range; a hand-edited config doesn't.
	if (referenceHz <= 0 || referenceHz > M_AUDIO_LOW_PASS_OFF) {
		referenceHz = M_AUDIO_LOW_PASS_OFF;
	} else if (referenceHz < M_AUDIO_LOW_PASS_MIN_REFERENCE) {
		referenceHz = M_AUDIO_LOW_PASS_MIN_REFERENCE;
	}
	f->lowPassReferenceHz = referenceHz;
}

bool mAudioSpeedFilterEngaged(const struct mAudioSpeedFilter* f) {
	return f->engaged;
}

double mAudioSpeedFilterSpeed(const struct mAudioSpeedFilter* f) {
	return f->speed;
}


int mAudioSpeedFilterWrite(struct mAudioSpeedFilter* f, const int16_t* frames, int numFrames) {
	int accepted;
	if (!f->enabled) {
		return numFrames;
	}
	accepted = mAudioTimeStretchWrite(&f->stretch, frames, numFrames);
	if (accepted < numFrames) {
		// Frames were already lost; resync rather than splice over the gap.
		mAudioTimeStretchBeginSession(&f->stretch);
	}
	return accepted;
}

// The target steps at the engage edge; mAudioLowPassProcess smooths curCutoff to it.
static void _finish(struct mAudioSpeedFilter* f, int16_t* frames, int numFrames, double speed) {
	double cutoff = mAudioLowPassCutoff(f->engaged, speed, f->lowPassReferenceHz,
	                                    mAudioLowPassWideOpen(&f->lowPass));
	mAudioLowPassProcess(&f->lowPass, frames, numFrames, cutoff, numFrames / f->sampleRate);
}

// The tail is synthesised from history _finish already filtered. Step the filter
// over it rather than filtering twice, or the tail is duller than what it continues.
static void _finishTail(struct mAudioSpeedFilter* f, int16_t* frames, int numFrames, double speed) {
	double cutoff = mAudioLowPassCutoff(f->engaged, speed, f->lowPassReferenceHz,
	                                    mAudioLowPassWideOpen(&f->lowPass));
	mAudioLowPassAdvance(&f->lowPass, frames, numFrames, cutoff, numFrames / f->sampleRate);
}

/* Arrival and output accumulate over a window closed by a delivery, not per
 * Read: no frontend does one Read per Write. The first delivery after a reset
 * only opens a window, since the output before it can't be attributed. */
static double _updateSpeed(struct mAudioSpeedFilter* f, int numFrames) {
	int64_t written = mAudioTimeStretchTotalWritten(&f->stretch);
	int64_t delta = written - f->lastWritten;
	bool fire = false;

	if (delta < 0) {
		delta = 0;
	}
	f->lastWritten = written;
	f->windowArrival += delta;

	if (numFrames > 0) {
		f->windowOutput += numFrames;
		if (delta > 0) {
			if (!f->windowOpen) {
				f->windowOpen = true;
				f->windowOutput = 0;
				f->windowArrival = 0;
			} else {
				fire = true;
			}
		} else if (f->windowOutput >= M_AUDIO_SPEED_ARRIVAL_WINDOW_CAP) {
			fire = true;
			f->windowOpen = true;
		}
	}

	if (fire) {
		double target = (f->windowArrival / (double) f->windowOutput) * numFrames;
		f->windowArrival = 0;
		f->windowOutput = 0;

		if (f->arrivalAvg <= 0.0) {
			f->arrivalAvg = target;
		} else {
			double err = target - f->arrivalAvg;
			double gain = M_AUDIO_SPEED_ARRIVAL_GAIN + (fabs(err) / f->arrivalAvg) * M_AUDIO_SPEED_ARRIVAL_TRACK;
			if (gain > M_AUDIO_SPEED_ARRIVAL_MAX_GAIN) {
				gain = M_AUDIO_SPEED_ARRIVAL_MAX_GAIN;
			}
			f->arrivalAvg += err * gain;
		}

		f->speed = f->arrivalAvg / (double) numFrames;

		if (mAudioIsOffSpeed(f->speed)) {
			++f->offSpeedRuns;
			f->onSpeedRuns = 0;
		} else if (mAudioIsOnSpeed(f->speed)) {
			++f->onSpeedRuns;
			f->offSpeedRuns = 0;
		}
		// A reading in the dead band leaves both counters alone rather than
		// restarting a run. Only the low-pass target moves on either edge: the
		// ratio comes from arrivalAvg on both sides, so there is nothing to resync.
		if (!f->engaged && f->offSpeedRuns >= M_AUDIO_SPEED_RUNS_TO_SWITCH) {
			f->engaged = true;
		} else if (f->engaged && f->onSpeedRuns >= M_AUDIO_SPEED_RUNS_TO_SWITCH) {
			f->engaged = false;
		}
	}

	return f->speed;
}

void mAudioSpeedFilterStreamEnd(struct mAudioSpeedFilter* f) {
	// Serialised against Read by the frontend's own audio lock.
	f->suspended = true;
	mAudioStreamRampEnd(&f->ramp);
}

/* Both the held audio and the arrival average describe the speed before the
 * break: resuming at 1x from a 3x hold would consume fresh input three times
 * too fast and underrun inside the ramp-in. Re-seed from the first delivery. */
static void _resyncToFreshInput(struct mAudioSpeedFilter* f) {
	mAudioTimeStretchBeginSession(&f->stretch);
	f->arrivalAvg = 0.0;
	f->speed = 1.0;
	f->windowArrival = 0;
	f->windowOutput = 0;
	f->windowOpen = false;
}

void mAudioSpeedFilterStreamBegin(struct mAudioSpeedFilter* f) {
	// A resume with no matching pause would discard a live reserve and ramp
	// continuously-playing audio down to silence and back up.
	if (!f->suspended && !mAudioStreamRampEnded(&f->ramp)) {
		return;
	}
	_resyncToFreshInput(f);
	f->suspended = false;
	mAudioStreamRampBegin(&f->ramp);
}

bool mAudioSpeedFilterJumpBegin(struct mAudioSpeedFilter* f) {
	// Already down, or the resume that lifts the suspend brings it back itself.
	if (f->suspended || mAudioStreamRampEnded(&f->ramp)) {
		return false;
	}
	// Same hold as a pause, so a jump outlasting the tail sits in silence.
	f->suspended = true;
	mAudioStreamRampEnd(&f->ramp);
	return true;
}

void mAudioSpeedFilterJumpEnd(struct mAudioSpeedFilter* f, bool ramped) {
	// Unconditional: an underrun tail heals through the ramp latch without a
	// resync, leaving the pre-jump reserve to splice onto what follows.
	_resyncToFreshInput(f);
	// Only if this jump took the stream down; one made while paused stays down.
	if (ramped) {
		f->suspended = false;
		mAudioStreamRampBegin(&f->ramp);
	}
}

bool mAudioSpeedFilterStreamEnded(const struct mAudioSpeedFilter* f) {
	return mAudioStreamRampEnded(&f->ramp);
}

int mAudioSpeedFilterRead(struct mAudioSpeedFilter* f, int16_t* frames, int numFrames) {
	double speed;
	double ratio;
	int got;

	// Past this, mAudioTimeStretchRead can't fill a call and the pad below takes over.
	assert(numFrames <= M_AUDIO_STRETCH_OUTPUT_CAPACITY);

	/* Nothing asked for, so nothing to conceal: falling through would take the
	 * stream down on a full tail and cost a dropout for a no-op call. */
	if (numFrames < 1) {
		return 0;
	}

	if (!f->enabled) {
		memset(frames, 0, numFrames * 2 * sizeof(int16_t));
		return numFrames;
	}

	speed = _updateSpeed(f, numFrames);

	// Down for a pause, a jump, or an unhealed underrun. Keep the low-pass
	// stepping over the tail so re-engaging does not click.
	if (mAudioStreamRampEnded(&f->ramp)) {
		mAudioStreamRampFillTail(&f->ramp, frames, numFrames);
		_finishTail(f, frames, numFrames, speed);
		return numFrames;
	}

	/* Tail spent and not yet resumed. Hold silence rather than fall through to
	 * the stretcher, whose reserve StreamBegin discards anyway. Not tracked, or
	 * the latched ramp-in would be spent on the silence. */
	if (f->suspended) {
		memset(frames, 0, numFrames * 2 * sizeof(int16_t));
		_finishTail(f, frames, numFrames, speed);
		return numFrames;
	}

	ratio = mAudioStretchRatio(f->arrivalAvg, numFrames,
	                           mAudioTimeStretchInputFill(&f->stretch),
	                           mAudioTimeStretchTargetInputFill(f->arrivalAvg));
	got = mAudioTimeStretchRead(&f->stretch, frames, numFrames, ratio);

	if (got < 1) {
		// Nothing arrived: end on the tail rather than fade a held sample as a
		// decaying DC offset. End() no-ops if nothing was playing.
		mAudioStreamRampEnd(&f->ramp);
		mAudioStreamRampFillTail(&f->ramp, frames, numFrames);
		_finishTail(f, frames, numFrames, speed);
		return numFrames;
	}

	// Low-pass first, then the ramp records and ramps in what actually plays.
	_finish(f, frames, got, speed);
	mAudioStreamRampTrack(&f->ramp, frames, got);

	if (got < numFrames) {
		// Ran short mid-block: the remainder is tail, not a fade of a held sample.
		mAudioStreamRampEnd(&f->ramp);
		mAudioStreamRampFillTail(&f->ramp, &frames[got * 2], numFrames - got);
		_finishTail(f, &frames[got * 2], numFrames - got, speed);
	}
	return numFrames;
}
