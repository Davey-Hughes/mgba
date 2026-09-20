/* Copyright (c) 2026 Davey Hughes
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "util/test/suite.h"

#include <mgba-util/audio-low-pass.h>

#include <math.h>
#include <string.h>

#define SAMPLE_RATE 48000.0
#define BLOCK 256

static void fillSine(int16_t* out, int frames, double freq, double* phase) {
	int i;
	for (i = 0; i < frames; ++i) {
		double v = sin(*phase) * 16000.0;
		out[(i * 2) + 0] = (int16_t) v;
		out[(i * 2) + 1] = (int16_t) v;
		*phase += (2.0 * M_PI * freq) / SAMPLE_RATE;
	}
}

static double peakAbs(const int16_t* frames, int n) {
	double peak = 0.0;
	int i;
	for (i = 0; i < n * 2; ++i) {
		double v = fabs((double) frames[i]);
		if (v > peak) {
			peak = v;
		}
	}
	return peak;
}

M_TEST_DEFINE(initIsWideOpenAndBypassed) {
	struct mAudioLowPass lp;
	mAudioLowPassInit(&lp, SAMPLE_RATE);
	assert_true(mAudioLowPassBypassed(&lp));
	assert_true(mAudioLowPassWideOpen(&lp) > 21000.0);
}

M_TEST_DEFINE(wideOpenLeavesSignalUntouched) {
	struct mAudioLowPass lp;
	int16_t buf[BLOCK * 2];
	int16_t copy[BLOCK * 2];
	double phase = 0.0;
	mAudioLowPassInit(&lp, SAMPLE_RATE);
	fillSine(buf, BLOCK, 1000.0, &phase);
	memcpy(copy, buf, sizeof(buf));
	mAudioLowPassProcess(&lp, buf, BLOCK, mAudioLowPassWideOpen(&lp), BLOCK / SAMPLE_RATE);
	assert_memory_equal(buf, copy, sizeof(buf));
}

M_TEST_DEFINE(attenuatesAboveCutoff) {
	struct mAudioLowPass lp;
	int16_t buf[BLOCK * 2];
	double phase = 0.0;
	int block;
	mAudioLowPassInit(&lp, SAMPLE_RATE);
	// Drive the smoother to 1 kHz, then measure a 10 kHz tone.
	for (block = 0; block < 200; ++block) {
		fillSine(buf, BLOCK, 10000.0, &phase);
		mAudioLowPassProcess(&lp, buf, BLOCK, 1000.0, BLOCK / SAMPLE_RATE);
	}
	// Measured ~1 for the 4th-order filter; a first-order RC would peak ~1600.
	assert_true(peakAbs(buf, BLOCK) < 10.0);
}

M_TEST_DEFINE(passesBelowCutoff) {
	struct mAudioLowPass lp;
	int16_t buf[BLOCK * 2];
	double phase = 0.0;
	int block;
	mAudioLowPassInit(&lp, SAMPLE_RATE);
	for (block = 0; block < 200; ++block) {
		fillSine(buf, BLOCK, 200.0, &phase);
		mAudioLowPassProcess(&lp, buf, BLOCK, 4000.0, BLOCK / SAMPLE_RATE);
	}
	assert_true(peakAbs(buf, BLOCK) > 12000.0);
}

M_TEST_DEFINE(cutoffIsSmoothedNotStepped) {
	struct mAudioLowPass lp;
	int16_t buf[BLOCK * 2];
	double phase = 0.0;
	double before;
	double blockSeconds;
	double a;
	double expected;
	mAudioLowPassInit(&lp, SAMPLE_RATE);
	before = mAudioLowPassCurrent(&lp);
	fillSine(buf, BLOCK, 1000.0, &phase);
	mAudioLowPassProcess(&lp, buf, BLOCK, 1000.0, BLOCK / SAMPLE_RATE);
	// Against the exact one-pole formula: a loose bound passes a 5x-fast smoother.
	blockSeconds = BLOCK / SAMPLE_RATE;
	a = 1.0 - exp(-blockSeconds / M_AUDIO_LOW_PASS_TAU);
	expected = before + ((1000.0 - before) * a);
	assert_true(fabs(mAudioLowPassCurrent(&lp) - expected) < 1.0);
}

M_TEST_DEFINE(reengagingDoesNotClick) {
	struct mAudioLowPass lp;
	int16_t buf[BLOCK * 2];
	double phase = 0.0;
	double firstSample;
	int block;
	mAudioLowPassInit(&lp, SAMPLE_RATE);
	// Run wide open (bypassed) so filter state still tracks the signal.
	for (block = 0; block < 50; ++block) {
		fillSine(buf, BLOCK, 500.0, &phase);
		mAudioLowPassProcess(&lp, buf, BLOCK, mAudioLowPassWideOpen(&lp), BLOCK / SAMPLE_RATE);
	}
	// Now engage: the first filtered sample must not jump from zeroed state.
	fillSine(buf, BLOCK, 500.0, &phase);
	firstSample = buf[0];
	mAudioLowPassProcess(&lp, buf, BLOCK, 2000.0, BLOCK / SAMPLE_RATE);
	assert_true(fabs((double) buf[0] - firstSample) < 4000.0);
}

M_TEST_DEFINE(channelsFilterIndependently) {
	// The only test that would catch both channels sharing one biquad's state.
	struct mAudioLowPass lp;
	int16_t buf[BLOCK * 2];
	double phaseL = 0.0;
	double phaseR = 0.0;
	double peakL = 0.0;
	double peakR = 0.0;
	int block;
	int i;
	mAudioLowPassInit(&lp, SAMPLE_RATE);
	for (block = 0; block < 200; ++block) {
		for (i = 0; i < BLOCK; ++i) {
			double l = sin(phaseL) * 16000.0;
			double r = sin(phaseR) * 16000.0;
			buf[(i * 2) + 0] = (int16_t) l;
			buf[(i * 2) + 1] = (int16_t) r;
			phaseL += (2.0 * M_PI * 200.0) / SAMPLE_RATE;
			phaseR += (2.0 * M_PI * 10000.0) / SAMPLE_RATE;
		}
		mAudioLowPassProcess(&lp, buf, BLOCK, 4000.0, BLOCK / SAMPLE_RATE);
	}
	for (i = 0; i < BLOCK; ++i) {
		double l = fabs((double) buf[(i * 2) + 0]);
		double r = fabs((double) buf[(i * 2) + 1]);
		if (l > peakL) {
			peakL = l;
		}
		if (r > peakR) {
			peakR = r;
		}
	}
	assert_true(peakL > 12000.0);
	assert_true(peakR < 3000.0);
}

static double maxStep(const int16_t* frames, int first, int count) {
	double peak = 0.0;
	int i;
	for (i = first + 1; i < first + count; ++i) {
		double d = fabs((double) frames[i * 2] - (double) frames[(i - 1) * 2]);
		if (d > peak) {
			peak = d;
		}
	}
	return peak;
}

M_TEST_DEFINE(cutoffStepDoesNotRing) {
	/* A transposed direct-form II biquad's state was produced under the
	 * previous coefficients, so redesigning them once per block leaves the
	 * two inconsistent for a sample and the filter rings. */
	struct mAudioLowPass lp;
	static int16_t open[4096 * 2];
	static int16_t closing[4096 * 2];
	double phase = 0.0;
	double steady;
	double transient;
	int block;
	mAudioLowPassInit(&lp, SAMPLE_RATE);
	fillSine(open, 4096, 200.0, &phase);
	for (block = 0; block < 4096 / BLOCK; ++block) {
		mAudioLowPassProcess(&lp, open + (block * BLOCK * 2), BLOCK, mAudioLowPassWideOpen(&lp), BLOCK / SAMPLE_RATE);
	}
	fillSine(closing, 4096, 200.0, &phase);
	for (block = 0; block < 4096 / BLOCK; ++block) {
		mAudioLowPassProcess(&lp, closing + (block * BLOCK * 2), BLOCK, 2000.0, BLOCK / SAMPLE_RATE);
	}
	steady = maxStep(open, 4096 - 1024, 1024);
	transient = maxStep(closing, 0, 4096);
	assert_true(transient <= 1.5 * steady);
}

M_TEST_DEFINE(advanceLeavesSamplesButMovesState) {
	struct mAudioLowPass a;
	struct mAudioLowPass b;
	int16_t viaProcess[BLOCK * 2];
	int16_t viaAdvance[BLOCK * 2];
	int16_t nextA[BLOCK * 2];
	int16_t nextB[BLOCK * 2];
	int16_t untouched[BLOCK * 2];
	double phase = 0.0;
	int i;

	mAudioLowPassInit(&a, SAMPLE_RATE);
	mAudioLowPassInit(&b, SAMPLE_RATE);

	fillSine(viaProcess, BLOCK, 4000.0, &phase);
	memcpy(viaAdvance, viaProcess, sizeof(viaProcess));
	memcpy(untouched, viaProcess, sizeof(viaProcess));

	mAudioLowPassProcess(&a, viaProcess, BLOCK, 2000.0, BLOCK / SAMPLE_RATE);
	mAudioLowPassAdvance(&b, viaAdvance, BLOCK, 2000.0, BLOCK / SAMPLE_RATE);

	// Advance writes nothing back...
	assert_memory_equal(viaAdvance, untouched, sizeof(untouched));
	// ...but it did filter something, so the two buffers must differ.
	assert_memory_not_equal(viaProcess, untouched, sizeof(untouched));

	// Both filters carry the same state forward: the next block matches.
	phase = 0.0;
	fillSine(nextA, BLOCK, 4000.0, &phase);
	memcpy(nextB, nextA, sizeof(nextA));
	mAudioLowPassProcess(&a, nextA, BLOCK, 2000.0, BLOCK / SAMPLE_RATE);
	mAudioLowPassProcess(&b, nextB, BLOCK, 2000.0, BLOCK / SAMPLE_RATE);
	for (i = 0; i < BLOCK * 2; ++i) {
		assert_int_equal(nextA[i], nextB[i]);
	}
}

M_TEST_SUITE_DEFINE(mAudioLowPass,
	cmocka_unit_test(initIsWideOpenAndBypassed),
	cmocka_unit_test(wideOpenLeavesSignalUntouched),
	cmocka_unit_test(attenuatesAboveCutoff),
	cmocka_unit_test(passesBelowCutoff),
	cmocka_unit_test(cutoffIsSmoothedNotStepped),
	cmocka_unit_test(reengagingDoesNotClick),
	cmocka_unit_test(channelsFilterIndependently),
	cmocka_unit_test(cutoffStepDoesNotRing),
	cmocka_unit_test(advanceLeavesSamplesButMovesState),
)
