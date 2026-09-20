/* Copyright (c) 2026 Davey Hughes
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "util/test/suite.h"

#include <mgba-util/audio-stream-ramp.h>

#include <math.h>

#define SAMPLE_RATE 48000.0
#define BLOCK 256

static void fillSine(int16_t* out, int frames, double freq, double* phase) {
	int i;
	for (i = 0; i < frames; ++i) {
		double v = sin(*phase) * 8000.0;
		out[(i * 2) + 0] = (int16_t) v;
		out[(i * 2) + 1] = (int16_t) v;
		*phase += (2.0 * M_PI * freq) / SAMPLE_RATE;
	}
}

static double rms(const int16_t* frames, int first, int count) {
	double e = 0.0;
	int i;
	for (i = first; i < first + count; ++i) {
		double v = (double) frames[i * 2];
		e += v * v;
	}
	return sqrt(e / count);
}

M_TEST_DEFINE(tailContinuesAndDecays) {
	struct mAudioStreamRamp r;
	int16_t buf[BLOCK * 2];
	static int16_t tail[1024 * 2];
	double phase = 0.0;
	int16_t lastPlayed;
	int block;
	int i;

	mAudioStreamRampInit(&r);
	for (block = 0; block < 4096 / BLOCK; ++block) {
		fillSine(buf, BLOCK, 220.0, &phase);
		mAudioStreamRampTrack(&r, buf, BLOCK);
	}
	lastPlayed = buf[(BLOCK - 1) * 2];

	mAudioStreamRampEnd(&r);
	assert_true(mAudioStreamRampEnded(&r));
	mAudioStreamRampFillTail(&r, tail, 1024);

	/* The tail opens where the stream stopped, not on some louder cycle. */
	assert_true(fabs((double) tail[0] - lastPlayed) <= 0.05 * 32767.0);

	/* The raised cosine only takes level away. Compared over 128-frame
	 * windows across the tail's thirds, where the envelope drops by far more
	 * than a partial-cycle sine's own windowed RMS can wobble: g is ~0.96,
	 * ~0.31 and ~0.04 at their centres. */
	assert_true(rms(tail, 0, 128) > rms(tail, 128, 128));
	assert_true(rms(tail, 128, 128) > rms(tail, 256, 128));
	assert_true(rms(tail, 256, 128) > rms(tail, 384, 128));
	assert_true(rms(tail, 384, 128) < 0.25 * rms(tail, 0, 128));

	/* Past the tail it is exactly silent. */
	for (i = M_AUDIO_RAMP_TAIL; i < 1024; ++i) {
		assert_int_equal(tail[i * 2], 0);
		assert_int_equal(tail[(i * 2) + 1], 0);
	}
	mAudioStreamRampReset(&r);
}

M_TEST_DEFINE(rampInStartsSilentAndReachesLevel) {
	struct mAudioStreamRamp r;
	static int16_t buf[1024 * 2];
	static int16_t ref[1024 * 2];
	double phase = 0.0;
	double refPhase = 0.0;
	double target;

	fillSine(ref, 1024, 220.0, &refPhase);
	target = rms(ref, 512, 512);

	mAudioStreamRampInit(&r);
	mAudioStreamRampBegin(&r);
	fillSine(buf, 1024, 220.0, &phase);
	mAudioStreamRampTrack(&r, buf, 1024);

	/* First frame near silence; past the RAMP_IN window the level matches
	 * the same source untouched, so the frames beyond it are unscaled. */
	assert_true(fabs((double) buf[0]) <= 0.02 * 8000.0);
	assert_true(fabs(rms(buf, 512, 512) - target) <= 0.02 * target);
	mAudioStreamRampReset(&r);
}

/* A reset or a state load into a quiet moment leaves the core emitting silence
 * for far longer than the ramp. Spending the ramp on those zeros would splice
 * the first real frames in at full level. */
M_TEST_DEFINE(rampInWaitsForRealAudio) {
	struct mAudioStreamRamp r;
	static int16_t buf[1024 * 2];
	static int16_t tail[1024 * 2];
	double phase = 0.0;
	double level;

	mAudioStreamRampInit(&r);
	// End() no-ops unless something was playing.
	fillSine(buf, 1024, 220.0, &phase);
	mAudioStreamRampTrack(&r, buf, 1024);
	mAudioStreamRampEnd(&r);
	mAudioStreamRampFillTail(&r, tail, M_AUDIO_RAMP_TAIL + M_AUDIO_RAMP_MUTE);
	assert_false(mAudioStreamRampEnded(&r));

	mAudioStreamRampBegin(&r);
	memset(buf, 0, sizeof(buf));
	mAudioStreamRampTrack(&r, buf, 1024);

	fillSine(buf, 1024, 220.0, &phase);
	mAudioStreamRampTrack(&r, buf, 1024);

	level = rms(buf, 512, 512);
	assert_true(level > 0.9 * (8000.0 / sqrt(2.0)));
	assert_true(fabs((double) buf[0]) <= 0.02 * 8000.0);
	assert_true(rms(buf, 0, 128) < 0.3 * level);
	mAudioStreamRampReset(&r);
}

M_TEST_SUITE_DEFINE(mAudioStreamRamp,
	cmocka_unit_test(tailContinuesAndDecays),
	cmocka_unit_test(rampInStartsSilentAndReachesLevel),
	cmocka_unit_test(rampInWaitsForRealAudio),
)
