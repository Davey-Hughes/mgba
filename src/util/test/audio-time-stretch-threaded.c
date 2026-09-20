/* Copyright (c) 2026 Davey Hughes
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "util/test/suite.h"

#include <mgba-util/audio-time-stretch.h>
#include <mgba-util/threading.h>

#include <math.h>
#include <time.h>

/* Drives the stretcher from a real producer thread, giving ThreadSanitizer an
 * actual interleaving to catch. Compiled out where threading isn't available. */
#ifndef DISABLE_THREADING

#define PRODUCER_BLOCKS 2000
#define BLOCK 512
#define SESSION_AT_BLOCK (PRODUCER_BLOCKS / 2)

// Wall-clock: spin counts vary wildly with host speed and TSan instrumentation.
#define TEST_TIMEOUT_SECONDS 30

#define SIGNAL_FREQ 440.0
#define SAMPLE_RATE 48000.0
#define MAX_LOGGED_FRAMES (PRODUCER_BLOCKS * BLOCK)

static struct mAudioTimeStretch s_ts;
static bool s_stop;

static THREAD_ENTRY _producer(void* context) {
	int16_t in[BLOCK * 2];
	double phase = 0.0;
	int block;
	UNUSED(context);
	for (block = 0; block < PRODUCER_BLOCKS; ++block) {
		int i;
		for (i = 0; i < BLOCK; ++i) {
			double v = sin(phase) * 12000.0;
			in[(i * 2) + 0] = (int16_t) v;
			in[(i * 2) + 1] = (int16_t) v;
			phase += (2.0 * M_PI * SIGNAL_FREQ) / SAMPLE_RATE;
		}
		mAudioTimeStretchWrite(&s_ts, in, BLOCK);

		if (block == SESSION_AT_BLOCK) {
			// A mid-stream discontinuity the consumer must pick up next Read.
			mAudioTimeStretchBeginSession(&s_ts);
		}
	}
	// Atomic: a plain store here would itself be the race TSan is meant to catch.
	ATOMIC_STORE(s_stop, true);
	THREAD_EXIT(0);
}

// Goertzel power at one bin, left channel only; not normalised.
static double _goertzelPower(const int16_t* frames, int n, double freq, double sampleRate) {
	double w = 2.0 * M_PI * freq / sampleRate;
	double coeff = 2.0 * cos(w);
	double s1 = 0.0;
	double s2 = 0.0;
	int i;
	for (i = 0; i < n; ++i) {
		double x = (double) frames[i * 2];
		double s0 = x + (coeff * s1) - s2;
		s2 = s1;
		s1 = s0;
	}
	return (s1 * s1) + (s2 * s2) - (coeff * s1 * s2);
}

M_TEST_DEFINE(concurrentProducerDoesNotTear) {
	Thread thread;
	int16_t out[256 * 2];
	int16_t* logged;
	int64_t loggedFrames = 0;
	int reads = 0;
	int inputFill;
	int outputFill = 0;
	int64_t totalWritten = 0;
	bool stop;
	bool timedOut = false;
	bool sawOutOfRange = false;
	time_t deadline;
	int i;
	double power440;
	double powerControl;

	logged = malloc(sizeof(int16_t) * 2 * MAX_LOGGED_FRAMES);
	assert_non_null(logged);

	assert_true(mAudioTimeStretchInit(&s_ts));
	ATOMIC_STORE(s_stop, false);
	assert_int_equal(ThreadCreate(&thread, _producer, NULL), 0);

	deadline = time(NULL) + TEST_TIMEOUT_SECONDS;
	ATOMIC_LOAD(stop, s_stop);
	inputFill = mAudioTimeStretchInputFill(&s_ts);
	outputFill = mAudioTimeStretchOutputFill(&s_ts);
	/* A remainder under one frame can never be synthesised, so waiting for
	 * inputFill == 0 could spin forever. */
	while (!stop || inputFill >= M_AUDIO_STRETCH_FRAME_SIZE || outputFill > 0) {
		int got = mAudioTimeStretchRead(&s_ts, out, 256, 2.0);
		++reads;

		// Keep these accessors under the same concurrent pressure as Read.
		outputFill = mAudioTimeStretchOutputFill(&s_ts);
		totalWritten = mAudioTimeStretchTotalWritten(&s_ts);
		assert_true(outputFill >= 0);
		assert_true(totalWritten >= 0);

		// Record rather than assert, so one bad sample doesn't hide the rest.
		for (i = 0; i < got; ++i) {
			if (abs(out[(i * 2) + 0]) > 20000 || abs(out[(i * 2) + 1]) > 20000) {
				sawOutOfRange = true;
			}
		}
		if (got > 0 && loggedFrames + got <= MAX_LOGGED_FRAMES) {
			memcpy(logged + (loggedFrames * 2), out, (size_t) got * 2 * sizeof(int16_t));
			loggedFrames += got;
		}

		// time() is a syscall; only pay for it occasionally.
		if ((reads & 0xfff) == 0 && time(NULL) > deadline) {
			timedOut = true;
			break;
		}

		ATOMIC_LOAD(stop, s_stop);
		inputFill = mAudioTimeStretchInputFill(&s_ts);
	}

	ThreadJoin(&thread);
	assert_false(timedOut);
	assert_true(reads > 0);
	assert_true(totalWritten > 0);
	assert_false(sawOutOfRange);

	/* The counters above pass even under a torn read; that shows up instead as
	 * broadband noise the Goertzel bin won't pick up. */
	assert_true(loggedFrames > 10000);
	power440 = _goertzelPower(logged, (int) loggedFrames, SIGNAL_FREQ, SAMPLE_RATE);
	powerControl = _goertzelPower(logged, (int) loggedFrames, SIGNAL_FREQ * 1.5, SAMPLE_RATE);
	assert_true(power440 > powerControl * 50.0);

	free(logged);
	mAudioTimeStretchDeinit(&s_ts);
}

M_TEST_SUITE_DEFINE(mAudioTimeStretchThreaded,
	cmocka_unit_test(concurrentProducerDoesNotTear),
)

#else

M_TEST_DEFINE(threadingDisabledNothingToTest) {
	// Threading is compiled out on this target; no boundary to exercise.
}

M_TEST_SUITE_DEFINE(mAudioTimeStretchThreaded,
	cmocka_unit_test(threadingDisabledNothingToTest),
)

#endif
