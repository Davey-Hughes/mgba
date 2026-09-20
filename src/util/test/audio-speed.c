/* Copyright (c) 2026 Davey Hughes
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "util/test/suite.h"

#include <mgba-util/audio-speed.h>

M_TEST_DEFINE(stretchRatioTracksArrival) {
	double ratio = mAudioStretchRatio(512.0, 256, 4096, 4096);
	assert_true(ratio > 1.999 && ratio < 2.001);
}

M_TEST_DEFINE(stretchRatioZeroOutputIsUnity) {
	assert_true(mAudioStretchRatio(512.0, 0, 4096, 4096) == 1.0);
}

M_TEST_DEFINE(stretchRatioFullFifoRaisesRatio) {
	double atTarget = mAudioStretchRatio(256.0, 256, 4096, 4096);
	double full = mAudioStretchRatio(256.0, 256, 8192, 4096);
	assert_true(full > atTarget);
}

M_TEST_DEFINE(stretchRatioEmptyFifoLowersRatio) {
	double atTarget = mAudioStretchRatio(256.0, 256, 4096, 4096);
	double empty = mAudioStretchRatio(256.0, 256, 0, 4096);
	assert_true(empty < atTarget);
}

M_TEST_DEFINE(stretchRatioIsClamped) {
	assert_true(mAudioStretchRatio(1000000.0, 1, 4096, 4096) <= 32.0);
	assert_true(mAudioStretchRatio(0.0, 256, 0, 4096) >= 0.25);
}

M_TEST_DEFINE(lowPassOffSentinelIsTransparent) {
	double wideOpen = 21600.0;
	assert_true(mAudioLowPassCutoff(true, 3.0, M_AUDIO_LOW_PASS_OFF, wideOpen) == wideOpen);
}

M_TEST_DEFINE(lowPassTransparentAtOrBelowNormalSpeed) {
	double wideOpen = 21600.0;
	assert_true(mAudioLowPassCutoff(true, 1.0, 12000, wideOpen) == wideOpen);
	assert_true(mAudioLowPassCutoff(true, 0.5, 12000, wideOpen) == wideOpen);
}

M_TEST_DEFINE(lowPassTransparentWhenNotEngaged) {
	// 1.015 is inside the dead band: the case that fires at ordinary speeds.
	double wideOpen = 21600.0;
	assert_true(mAudioLowPassCutoff(false, 1.015, 12000, wideOpen) == wideOpen);
	// Not engaged must win even at a speed that would otherwise be dulled.
	assert_true(mAudioLowPassCutoff(false, 4.0, 12000, wideOpen) == wideOpen);
}

M_TEST_DEFINE(lowPassDullsWithSpeed) {
	double wideOpen = 21600.0;
	double at2x = mAudioLowPassCutoff(true, 2.0, 12000, wideOpen);
	double at4x = mAudioLowPassCutoff(true, 4.0, 12000, wideOpen);
	assert_true(at2x > at4x);
	assert_true(at2x > 5999.0 && at2x < 6001.0);
}

M_TEST_DEFINE(lowPassHasFloor) {
	assert_true(mAudioLowPassCutoff(true, 1000.0, 12000, 21600.0) >= 200.0);
}

M_TEST_DEFINE(engageHysteresisBandsDoNotOverlap) {
	/* Midway between the thresholds, derived from the constants so retuning
	 * them can't silently move this out of the dead band. */
	double dead = 1.0 + ((M_AUDIO_DISENGAGE_DEVIATION + M_AUDIO_ENGAGE_DEVIATION) * 0.5);
	assert_false(mAudioIsOffSpeed(dead));
	assert_false(mAudioIsOnSpeed(dead));
	assert_true(mAudioIsOffSpeed(1.0 + (M_AUDIO_ENGAGE_DEVIATION * 2.0)));
	assert_true(mAudioIsOnSpeed(1.0 + (M_AUDIO_DISENGAGE_DEVIATION * 0.5)));
}

M_TEST_DEFINE(engageHysteresisBoundariesAreExact) {
	// nextafter: a decimal literal like 1.01 doesn't land on the threshold exactly.
	double atDisengage = 1.0 + M_AUDIO_DISENGAGE_DEVIATION;
	double justInsideDisengage = nextafter(atDisengage, 1.0);
	double atEngage = 1.0 + M_AUDIO_ENGAGE_DEVIATION;
	double justInsideEngage = nextafter(atEngage, 1.0);

	/* Step off the threshold rather than testing the tie: whether `1.0 + K`
	 * lands above or below K tests the literal's rounding, not the predicate. */
	double justOutsideDisengage = nextafter(atDisengage, 2.0);
	double justOutsideEngage = nextafter(atEngage, 2.0);

	assert_true(mAudioIsOnSpeed(justInsideDisengage));
	assert_false(mAudioIsOnSpeed(justOutsideDisengage));

	// Symmetric below 1.0.
	assert_true(mAudioIsOnSpeed(2.0 - justInsideDisengage));
	assert_false(mAudioIsOnSpeed(2.0 - justOutsideDisengage));

	assert_false(mAudioIsOffSpeed(justInsideEngage));
	assert_true(mAudioIsOffSpeed(justOutsideEngage));

	// Symmetric below 1.0.
	assert_false(mAudioIsOffSpeed(2.0 - justInsideEngage));
	assert_true(mAudioIsOffSpeed(2.0 - justOutsideEngage));
}

M_TEST_DEFINE(lowPassZeroReadsAsOff) {
	// A hand-edited config's 0 must not clamp to the strongest setting.
	double wideOpen = 21600.0;
	assert_true(mAudioLowPassCutoff(true, 3.0, 0, wideOpen) == wideOpen);
}

M_TEST_SUITE_DEFINE(mAudioSpeed,
	cmocka_unit_test(stretchRatioTracksArrival),
	cmocka_unit_test(stretchRatioZeroOutputIsUnity),
	cmocka_unit_test(stretchRatioFullFifoRaisesRatio),
	cmocka_unit_test(stretchRatioEmptyFifoLowersRatio),
	cmocka_unit_test(stretchRatioIsClamped),
	cmocka_unit_test(lowPassOffSentinelIsTransparent),
	cmocka_unit_test(lowPassTransparentAtOrBelowNormalSpeed),
	cmocka_unit_test(lowPassTransparentWhenNotEngaged),
	cmocka_unit_test(lowPassDullsWithSpeed),
	cmocka_unit_test(lowPassHasFloor),
	cmocka_unit_test(engageHysteresisBandsDoNotOverlap),
	cmocka_unit_test(engageHysteresisBoundariesAreExact),
	cmocka_unit_test(lowPassZeroReadsAsOff),
)
