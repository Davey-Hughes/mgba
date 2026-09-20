/* Copyright (c) 2026 Davey Hughes
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef M_AUDIO_SPEED_FILTER_H
#define M_AUDIO_SPEED_FILTER_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba-util/audio-low-pass.h>
#include <mgba-util/audio-speed.h>
#include <mgba-util/audio-stream-ramp.h>
#include <mgba-util/audio-time-stretch.h>

// The base gain rejects per-callback lumpiness; the track term follows a speed change.
#define M_AUDIO_SPEED_ARRIVAL_GAIN 0.05
#define M_AUDIO_SPEED_ARRIVAL_TRACK 1.5
#define M_AUDIO_SPEED_ARRIVAL_MAX_GAIN 0.6

// Isolated stalls cross ENGAGE, so requiring a run is what keeps normal play
// disengaged at the smaller buffer sizes. Not safe below 3.
#define M_AUDIO_SPEED_RUNS_TO_SWITCH 3

// Closes the arrival window when arrival stops entirely. Above any real delivery.
#define M_AUDIO_SPEED_ARRIVAL_WINDOW_CAP 16384

struct mAudioSpeedFilter {
	struct mAudioTimeStretch stretch;
	struct mAudioLowPass lowPass;
	struct mAudioStreamRamp ramp;

	double sampleRate;
	double arrivalAvg;   /* smoothed frames arriving per Read call */
	double speed;        /* arrivalAvg divided by the Read size: the ratio */
	int64_t lastWritten;

	int64_t windowArrival; /* frames arrived since the current window opened */
	int windowOutput;      /* frames read (output) over the same window */
	bool windowOpen;       /* true once a window has a real start point to measure from */

	int lowPassReferenceHz;
	int offSpeedRuns;
	int onSpeedRuns;
	bool enabled;
	bool engaged;
	bool suspended; /* StreamEnd seen with no StreamBegin since */
};

// Costs ~85 ms of latency at 48 kHz, more at larger buffers, plus a ~0.3 s
// slowed refill at every session start, rate change and engage transition.
bool mAudioSpeedFilterInit(struct mAudioSpeedFilter*, double sampleRate);
void mAudioSpeedFilterDeinit(struct mAudioSpeedFilter*);

// The rate frames are written and read at, not the core's where the caller resamples.
void mAudioSpeedFilterSetSampleRate(struct mAudioSpeedFilter*, double sampleRate);

// Enabling reallocates the stretcher's rings and stays disabled if that fails.
void mAudioSpeedFilterSetEnabled(struct mAudioSpeedFilter*, bool enabled);
bool mAudioSpeedFilterEnabled(const struct mAudioSpeedFilter*);
void mAudioSpeedFilterSetLowPass(struct mAudioSpeedFilter*, int referenceHz);

// PRODUCER. Interleaved stereo, hardcoded to 2 channels. To see fast-forward at
// all, the caller's buffer and mCoreSync::audioHighWater must scale with maxSpeed.
int mAudioSpeedFilterWrite(struct mAudioSpeedFilter*, const int16_t* frames, int numFrames);

// Armed from another thread; Read acts on them on the audio thread. JumpBegin
// returns false if the stream was already down, which is what JumpEnd ramps back on.
void mAudioSpeedFilterStreamEnd(struct mAudioSpeedFilter*);
void mAudioSpeedFilterStreamBegin(struct mAudioSpeedFilter*);
bool mAudioSpeedFilterJumpBegin(struct mAudioSpeedFilter*);
void mAudioSpeedFilterJumpEnd(struct mAudioSpeedFilter*, bool ramped);

// True while a concealment tail or its trailing mute is still owed.
bool mAudioSpeedFilterStreamEnded(const struct mAudioSpeedFilter*);

// CONSUMER. Emits numFrames, concealing an underrun until real frames return,
// or 0 if none were asked for. numFrames must not exceed
// M_AUDIO_STRETCH_OUTPUT_CAPACITY.
int mAudioSpeedFilterRead(struct mAudioSpeedFilter*, int16_t* frames, int numFrames);

bool mAudioSpeedFilterEngaged(const struct mAudioSpeedFilter*);
double mAudioSpeedFilterSpeed(const struct mAudioSpeedFilter*);

CXX_GUARD_END

#endif
