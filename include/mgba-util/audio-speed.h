/* Copyright (c) 2026 Davey Hughes
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef M_AUDIO_SPEED_H
#define M_AUDIO_SPEED_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba-util/audio-low-pass.h>

#include <math.h>

// "Do not filter", at the top of the range; zero reads as off too. The range
// runs past Nyquist because the reference is divided by the emulation speed.
#define M_AUDIO_LOW_PASS_OFF 48000
#define M_AUDIO_LOW_PASS_DEFAULT 20000

// Matches the Qt spinbox minimum; lower brick-walls at M_AUDIO_LOW_PASS_FLOOR.
#define M_AUDIO_LOW_PASS_MIN_REFERENCE 1000

#define M_AUDIO_STRETCH_TRIM_GAIN 0.25

#define M_AUDIO_MIN_STRETCH_RATIO 0.25
#define M_AUDIO_MAX_STRETCH_RATIO 32.0

#define M_AUDIO_LOW_PASS_FLOOR 200.0

// Non-overlapping, and clear of the frontends' steady-state overspeed (up to
// 26.6% at the smallest buffer) so on-speed can always be confirmed.
#define M_AUDIO_ENGAGE_DEVIATION 0.45
#define M_AUDIO_DISENGAGE_DEVIATION 0.30

static inline bool mAudioIsOffSpeed(double speed) {
	return fabs(speed - 1.0) > M_AUDIO_ENGAGE_DEVIATION;
}

static inline bool mAudioIsOnSpeed(double speed) {
	return fabs(speed - 1.0) <= M_AUDIO_DISENGAGE_DEVIATION;
}

static inline double mAudioStretchRatio(double arrivalPerCallback, int outputPerCallback,
                                        int inputFill, int targetFill) {
	double ratio;
	if (outputPerCallback <= 0) {
		return 1.0;
	}
	ratio = arrivalPerCallback / (double) outputPerCallback;
	if (targetFill > 0) {
		double err = (inputFill - (double) targetFill) / (double) targetFill;
		err = mAudioClampDouble(err, -1.0, 1.0);
		ratio *= 1.0 + (M_AUDIO_STRETCH_TRIM_GAIN * err);
	}
	return mAudioClampDouble(ratio, M_AUDIO_MIN_STRETCH_RATIO, M_AUDIO_MAX_STRETCH_RATIO);
}

// Gated on `engaged` so the dead band stays undulled and noise around 1.0
// cannot flap the target every callback.
static inline double mAudioLowPassCutoff(bool engaged, double speed, int referenceHz, double wideOpen) {
	if (wideOpen <= M_AUDIO_LOW_PASS_FLOOR) {
		return wideOpen;
	}
	if (!engaged || speed <= 1.0) {
		return wideOpen;
	}
	if (referenceHz <= 0 || referenceHz >= M_AUDIO_LOW_PASS_OFF) {
		return wideOpen;
	}
	return mAudioClampDouble(referenceHz / speed, M_AUDIO_LOW_PASS_FLOOR, wideOpen);
}

CXX_GUARD_END

#endif
