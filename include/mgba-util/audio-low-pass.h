/* Copyright (c) 2026 Davey Hughes
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef M_AUDIO_LOW_PASS_H
#define M_AUDIO_LOW_PASS_H

#include <mgba-util/common.h>

CXX_GUARD_START

#define M_AUDIO_LOW_PASS_TAU 0.05
#define M_AUDIO_LOW_PASS_BYPASS 0.995
#define M_AUDIO_LOW_PASS_MIN_CUTOFF 20.0

static inline double mAudioClampDouble(double v, double lo, double hi) {
	if (v < lo) {
		return lo;
	}
	if (v > hi) {
		return hi;
	}
	return v;
}

struct mAudioBiquad {
	double b0;
	double b1;
	double b2;
	double a1;
	double a2;
	double z1[2];
	double z2[2];
};

struct mAudioLowPass {
	double sampleRate;
	double wideOpen;
	double curCutoff;
	struct mAudioBiquad stages[2];
};

void mAudioLowPassInit(struct mAudioLowPass*, double sampleRate);

void mAudioLowPassProcess(struct mAudioLowPass*, int16_t* frames, int numFrames,
                          double targetHz, double blockSeconds);

// Steps the smoother and the biquad state over already-filtered audio without
// writing to it. Leaving the filter still would splice stale state on re-entry.
void mAudioLowPassAdvance(struct mAudioLowPass*, int16_t* frames, int numFrames,
                          double targetHz, double blockSeconds);

double mAudioLowPassWideOpen(const struct mAudioLowPass*);
double mAudioLowPassCurrent(const struct mAudioLowPass*);
bool mAudioLowPassBypassed(const struct mAudioLowPass*);

CXX_GUARD_END

#endif
