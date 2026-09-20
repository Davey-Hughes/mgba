/* Copyright (c) 2026 Davey Hughes
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "util/test/suite.h"

#include <mgba-util/audio-time-stretch.h>

#include <math.h>

#define SAMPLE_RATE 48000.0

static void fillSine(int16_t* out, int frames, double freq, double* phase) {
	int i;
	for (i = 0; i < frames; ++i) {
		double v = sin(*phase) * 12000.0;
		out[(i * 2) + 0] = (int16_t) v;
		out[(i * 2) + 1] = (int16_t) v;
		*phase += (2.0 * M_PI * freq) / SAMPLE_RATE;
	}
}

/* Loops because Write caps at M_AUDIO_STRETCH_MAX_WRITE per call, and asserts
 * everything landed: a silent short write here once hid a cap bug. */
static void writeAll(struct mAudioTimeStretch* ts, const int16_t* frames, int numFrames) {
	int total = 0;
	while (total < numFrames) {
		int accepted = mAudioTimeStretchWrite(ts, frames + (total * 2), numFrames - total);
		assert_true(accepted > 0);
		total += accepted;
	}
}

// Goertzel power at one bin; only meaningful against another call with the same `n`.
static double goertzelPower(const int16_t* frames, int n, int channelOffset, double freq, double sampleRate) {
	double w = 2.0 * M_PI * freq / sampleRate;
	double coeff = 2.0 * cos(w);
	double s1 = 0.0;
	double s2 = 0.0;
	int i;
	for (i = 0; i < n; ++i) {
		double x = (double) frames[(i * 2) + channelOffset];
		double s0 = x + (coeff * s1) - s2;
		s2 = s1;
		s1 = s0;
	}
	return (s1 * s1) + (s2 * s2) - (coeff * s1 * s2);
}

M_TEST_DEFINE(initSucceedsAndStartsEmpty) {
	struct mAudioTimeStretch ts;
	assert_true(mAudioTimeStretchInit(&ts));
	assert_int_equal(mAudioTimeStretchInputFill(&ts), 0);
	assert_int_equal(mAudioTimeStretchOutputFill(&ts), 0);
	assert_true(mAudioTimeStretchTotalWritten(&ts) == 0);
	mAudioTimeStretchDeinit(&ts);
}

M_TEST_DEFINE(writeAdvancesTotalWritten) {
	struct mAudioTimeStretch ts;
	int16_t in[512 * 2];
	double phase = 0.0;
	assert_true(mAudioTimeStretchInit(&ts));
	fillSine(in, 512, 440.0, &phase);
	assert_int_equal(mAudioTimeStretchWrite(&ts, in, 512), 512);
	assert_true(mAudioTimeStretchTotalWritten(&ts) == 512);
	assert_int_equal(mAudioTimeStretchInputFill(&ts), 512);
	mAudioTimeStretchDeinit(&ts);
}

M_TEST_DEFINE(writeRefusesRatherThanLappingConsumer) {
	struct mAudioTimeStretch ts;
	static int16_t in[M_AUDIO_STRETCH_MAX_WRITE * 2];
	double phase = 0.0;
	int accepted = 1;
	int total = 0;
	assert_true(mAudioTimeStretchInit(&ts));
	// Never read, so the ring must eventually refuse.
	while (accepted > 0 && total < 1000000) {
		fillSine(in, M_AUDIO_STRETCH_MAX_WRITE, 440.0, &phase);
		accepted = mAudioTimeStretchWrite(&ts, in, M_AUDIO_STRETCH_MAX_WRITE);
		total += accepted;
	}
	assert_int_equal(accepted, 0);
	assert_true(total <= M_AUDIO_STRETCH_INPUT_CAPACITY);
	mAudioTimeStretchDeinit(&ts);
}

M_TEST_DEFINE(readAtUnityPreservesFrameCount) {
	struct mAudioTimeStretch ts;
	static int16_t in[8192 * 2];
	int16_t out[256 * 2];
	double phase = 0.0;
	int block;
	int got = 0;
	assert_true(mAudioTimeStretchInit(&ts));
	fillSine(in, 8192, 440.0, &phase);
	writeAll(&ts, in, 8192);
	for (block = 0; block < 4; ++block) {
		got = mAudioTimeStretchRead(&ts, out, 256, 1.0);
	}
	assert_int_equal(got, 256);
	mAudioTimeStretchDeinit(&ts);
}

M_TEST_DEFINE(compressionConsumesFasterThanItEmits) {
	/* analysisPos advances by lround(hop * ratio), so 8 reads of 256 at ratio 3
	 * consume ~6144, plus one silent hop that charges the accumulator on the
	 * first read. Bounded on both sides: a one-sided bound passes a half hop. */
	struct mAudioTimeStretch ts;
	static int16_t in[16384 * 2];
	int16_t out[256 * 2];
	double phase = 0.0;
	int64_t before;
	int fillBefore;
	int fillAfter;
	int consumed;
	int hops = (8 * 256) / M_AUDIO_STRETCH_SYNTHESIS_HOP;
	int expected = (hops + 1) * (int) lround(M_AUDIO_STRETCH_SYNTHESIS_HOP * 3.0);
	int block;
	assert_true(mAudioTimeStretchInit(&ts));
	fillSine(in, 16384, 440.0, &phase);
	writeAll(&ts, in, 16384);
	before = mAudioTimeStretchTotalWritten(&ts);
	fillBefore = mAudioTimeStretchInputFill(&ts);
	for (block = 0; block < 8; ++block) {
		mAudioTimeStretchRead(&ts, out, 256, 3.0);
	}
	fillAfter = mAudioTimeStretchInputFill(&ts);
	consumed = fillBefore - fillAfter;
	// Nothing new written; consumption should be ~3x what was emitted.
	assert_true(mAudioTimeStretchTotalWritten(&ts) == before);
	assert_true(consumed > expected - M_AUDIO_STRETCH_SYNTHESIS_HOP);
	assert_true(consumed < expected + M_AUDIO_STRETCH_SYNTHESIS_HOP);
	mAudioTimeStretchDeinit(&ts);
}

M_TEST_DEFINE(expansionConsumesSlowerThanItEmits) {
	// Same at ratio 0.5, where a bound with no lower edge passes zero consumption.
	struct mAudioTimeStretch ts;
	static int16_t in[16384 * 2];
	int16_t out[256 * 2];
	double phase = 0.0;
	int fillBefore;
	int fillAfter;
	int consumed;
	int hops = (8 * 256) / M_AUDIO_STRETCH_SYNTHESIS_HOP;
	int expected = hops * (int) lround(M_AUDIO_STRETCH_SYNTHESIS_HOP * 0.5);
	int block;
	assert_true(mAudioTimeStretchInit(&ts));
	fillSine(in, 16384, 440.0, &phase);
	writeAll(&ts, in, 16384);
	fillBefore = mAudioTimeStretchInputFill(&ts);
	for (block = 0; block < 8; ++block) {
		mAudioTimeStretchRead(&ts, out, 256, 0.5);
	}
	fillAfter = mAudioTimeStretchInputFill(&ts);
	consumed = fillBefore - fillAfter;
	// 2048 frames emitted at ratio 0.5 consumes about 1024.
	assert_true(consumed > expected - M_AUDIO_STRETCH_SYNTHESIS_HOP);
	assert_true(consumed < expected + M_AUDIO_STRETCH_SYNTHESIS_HOP);
	mAudioTimeStretchDeinit(&ts);
}

M_TEST_DEFINE(outputStaysInRangeOnLoudInput) {
	struct mAudioTimeStretch ts;
	static int16_t in[8192 * 2];
	int16_t out[256 * 2];
	bool sawMaxPositive = false;
	bool sawMaxNegative = false;
	int i;
	int block;
	assert_true(mAudioTimeStretchInit(&ts));
	// Per frame, not per sample, or each channel just gets constant DC.
	for (i = 0; i < 8192; ++i) {
		int16_t v = (i & 1) ? 32767 : -32768;
		in[(i * 2) + 0] = v;
		in[(i * 2) + 1] = v;
	}
	writeAll(&ts, in, 8192);
	for (block = 0; block < 4; ++block) {
		int got = mAudioTimeStretchRead(&ts, out, 256, 2.0);
		for (i = 0; i < got; ++i) {
			int16_t l = out[(i * 2) + 0];
			int16_t r = out[(i * 2) + 1];
			if (l == 32767 || r == 32767) {
				sawMaxPositive = true;
			}
			if (l == -32768 || r == -32768) {
				sawMaxNegative = true;
			}
		}
	}
	// Overlap-add of full-scale windows can overshoot, so both rails must hit.
	assert_true(sawMaxPositive);
	assert_true(sawMaxNegative);
	mAudioTimeStretchDeinit(&ts);
}

M_TEST_DEFINE(stretchPreservesDominantFrequency) {
	// WSOLA preserves the input frequency; a naive resampler would shift it 3x.
	struct mAudioTimeStretch ts;
	static int16_t in[24000 * 2];
	int16_t out[M_AUDIO_STRETCH_SYNTHESIS_HOP * 2];
	static int16_t measured[40 * M_AUDIO_STRETCH_SYNTHESIS_HOP * 2];
	double phase = 0.0;
	const double inputFreq = 440.0;
	const double resampledFreq = inputFreq * 3.0;
	const int skipHops = 4; /* first hop ramps up from silence; skip it */
	const int keepHops = 40;
	double powerAtInput;
	double powerAtResampled;
	int hop;
	int got;

	assert_true(mAudioTimeStretchInit(&ts));
	fillSine(in, 24000, inputFreq, &phase);
	writeAll(&ts, in, 24000);

	for (hop = 0; hop < skipHops; ++hop) {
		got = mAudioTimeStretchRead(&ts, out, M_AUDIO_STRETCH_SYNTHESIS_HOP, 3.0);
		assert_int_equal(got, M_AUDIO_STRETCH_SYNTHESIS_HOP);
	}
	for (hop = 0; hop < keepHops; ++hop) {
		got = mAudioTimeStretchRead(&ts, out, M_AUDIO_STRETCH_SYNTHESIS_HOP, 3.0);
		assert_int_equal(got, M_AUDIO_STRETCH_SYNTHESIS_HOP);
		memcpy(measured + (hop * M_AUDIO_STRETCH_SYNTHESIS_HOP * 2), out, sizeof(out));
	}

	powerAtInput = goertzelPower(measured, keepHops * M_AUDIO_STRETCH_SYNTHESIS_HOP, 0, inputFreq, SAMPLE_RATE);
	powerAtResampled = goertzelPower(measured, keepHops * M_AUDIO_STRETCH_SYNTHESIS_HOP, 0, resampledFreq, SAMPLE_RATE);

	// Measured: WSOLA ~2e5, naive resampling ~3e-7, a constant stage ~1.
	assert_true(powerAtInput > powerAtResampled * 50.0);
	mAudioTimeStretchDeinit(&ts);
}

M_TEST_DEFINE(beginSessionResyncsToLiveData) {
	struct mAudioTimeStretch ts;
	static int16_t in[8192 * 2];
	int16_t out[256 * 2];
	double phase = 0.0;
	assert_true(mAudioTimeStretchInit(&ts));
	fillSine(in, 8192, 440.0, &phase);
	writeAll(&ts, in, 8192);
	mAudioTimeStretchRead(&ts, out, 256, 1.0);

	mAudioTimeStretchBeginSession(&ts);
	fillSine(in, 8192, 880.0, &phase);
	writeAll(&ts, in, 8192);
	mAudioTimeStretchRead(&ts, out, 256, 1.0);
	// Resynced near the live write head, so the backlog is small, not 8k.
	assert_true(mAudioTimeStretchInputFill(&ts) < 8192);
	mAudioTimeStretchDeinit(&ts);
}

M_TEST_DEFINE(readWithNoInputEmitsNothing) {
	struct mAudioTimeStretch ts;
	int16_t out[256 * 2];
	assert_true(mAudioTimeStretchInit(&ts));
	assert_int_equal(mAudioTimeStretchRead(&ts, out, 256, 1.0), 0);
	mAudioTimeStretchDeinit(&ts);
}

M_TEST_DEFINE(searchReducesAmplitudePumping) {
	// Measured spread of per-hop RMS: 0.036 with the search on, 0.169 forced off.
	struct mAudioTimeStretch ts;
	static int16_t in[16384 * 2];
	int16_t out[M_AUDIO_STRETCH_SYNTHESIS_HOP * 2];
	double phase = 0.0;
	double rms[64];
	double mean = 0.0;
	double spread = 0.0;
	int hops = 0;
	int i;

	assert_true(mAudioTimeStretchInit(&ts));
	// Noisy material: a pure tone would understate worst-case pumping.
	uint32_t rng = 1;
	for (i = 0; i < 16384; ++i) {
		double v;
		// Unsigned: signed overflow here would be undefined behaviour.
		rng = (rng * 1103515245u) + 12345u;
		v = (sin(phase) * 6000.0) + ((double) (rng % 8000u) - 4000.0);
		in[(i * 2) + 0] = (int16_t) v;
		in[(i * 2) + 1] = (int16_t) v;
		phase += (2.0 * M_PI * 440.0) / SAMPLE_RATE;
	}
	writeAll(&ts, in, 16384);

	while (hops < 64) {
		double sum = 0.0;
		int got = mAudioTimeStretchRead(&ts, out, M_AUDIO_STRETCH_SYNTHESIS_HOP, 3.0);
		if (got < M_AUDIO_STRETCH_SYNTHESIS_HOP) {
			break;
		}
		for (i = 0; i < got; ++i) {
			double v = out[i * 2];
			sum += v * v;
		}
		rms[hops] = sqrt(sum / got);
		mean += rms[hops];
		++hops;
	}
	assert_true(hops > 16);
	mean /= hops;
	assert_true(mean > 0.0);
	for (i = 0; i < hops; ++i) {
		double d = (rms[i] - mean) / mean;
		spread += d * d;
	}
	spread = sqrt(spread / hops);

	// Measured is 0.036 and no-search is 0.169, so 0.12 proves the search works.
	assert_true(spread < 0.12);
	mAudioTimeStretchDeinit(&ts);
}

M_TEST_DEFINE(targetInputFillGrowsWithArrival) {
	int small = mAudioTimeStretchTargetInputFill(256.0);
	int large = mAudioTimeStretchTargetInputFill(8192.0);
	assert_true(small >= M_AUDIO_STRETCH_MIN_TARGET_FILL);
	assert_true(large > small);
	assert_true(large <= M_AUDIO_STRETCH_INPUT_CAPACITY / 2);
}

M_TEST_DEFINE(targetInputFillSurvivesAbsurdArrival) {
	// Casting straight to int would be UB; must clamp as a double first.
	int fill = mAudioTimeStretchTargetInputFill(2.5e9);
	assert_true(fill > 0);
	assert_true(fill <= M_AUDIO_STRETCH_INPUT_CAPACITY / 2);
}

M_TEST_DEFINE(readSnapshotsGenerationBeforeWritePos) {
	struct mAudioTimeStretch ts;
	static int16_t in[8192 * 2];
	int16_t out[256 * 2];
	double phase = 0.0;
	assert_true(mAudioTimeStretchInit(&ts));
	fillSine(in, 8192, 440.0, &phase);
	writeAll(&ts, in, 8192);
	mAudioTimeStretchRead(&ts, out, 256, 1.0);

	// A new session must be picked up next read, not read against a stale snapshot.
	mAudioTimeStretchBeginSession(&ts);
	fillSine(in, 4096, 880.0, &phase);
	mAudioTimeStretchWrite(&ts, in, 4096);
	mAudioTimeStretchRead(&ts, out, 256, 1.0);
	assert_true(mAudioTimeStretchInputFill(&ts) < 4096);
	mAudioTimeStretchDeinit(&ts);
}

M_TEST_DEFINE(consumerFloorStopsProducerNotConsumer) {
	struct mAudioTimeStretch ts;
	static int16_t in[M_AUDIO_STRETCH_MAX_WRITE * 2];
	int16_t out[256 * 2];
	double phase = 0.0;
	int accepted;
	int guard = 0;
	assert_true(mAudioTimeStretchInit(&ts));

	// Fill until refused, then confirm one read frees space again.
	do {
		fillSine(in, M_AUDIO_STRETCH_MAX_WRITE, 440.0, &phase);
		accepted = mAudioTimeStretchWrite(&ts, in, M_AUDIO_STRETCH_MAX_WRITE);
		++guard;
	} while (accepted > 0 && guard < 64);
	assert_int_equal(accepted, 0);

	mAudioTimeStretchRead(&ts, out, 256, 4.0);
	fillSine(in, M_AUDIO_STRETCH_MAX_WRITE, 440.0, &phase);
	assert_true(mAudioTimeStretchWrite(&ts, in, M_AUDIO_STRETCH_MAX_WRITE) > 0);
	mAudioTimeStretchDeinit(&ts);
}

M_TEST_DEFINE(channelsStayDistinctThroughStretch) {
	// The only test that would catch inL/inR or accL/accR getting crossed.
	struct mAudioTimeStretch ts;
	static int16_t in[24000 * 2];
	int16_t out[M_AUDIO_STRETCH_SYNTHESIS_HOP * 2];
	static int16_t measured[40 * M_AUDIO_STRETCH_SYNTHESIS_HOP * 2];
	double phaseL = 0.0;
	double phaseR = 0.0;
	const double freqL = 440.0;
	const double freqR = 1000.0;
	const int skipHops = 4;
	const int keepHops = 40;
	double powerL_atL;
	double powerL_atR;
	double powerR_atL;
	double powerR_atR;
	int hop;
	int got;
	int i;

	assert_true(mAudioTimeStretchInit(&ts));
	for (i = 0; i < 24000; ++i) {
		double l = sin(phaseL) * 12000.0;
		double r = sin(phaseR) * 12000.0;
		in[(i * 2) + 0] = (int16_t) l;
		in[(i * 2) + 1] = (int16_t) r;
		phaseL += (2.0 * M_PI * freqL) / SAMPLE_RATE;
		phaseR += (2.0 * M_PI * freqR) / SAMPLE_RATE;
	}
	writeAll(&ts, in, 24000);

	for (hop = 0; hop < skipHops; ++hop) {
		got = mAudioTimeStretchRead(&ts, out, M_AUDIO_STRETCH_SYNTHESIS_HOP, 3.0);
		assert_int_equal(got, M_AUDIO_STRETCH_SYNTHESIS_HOP);
	}
	for (hop = 0; hop < keepHops; ++hop) {
		got = mAudioTimeStretchRead(&ts, out, M_AUDIO_STRETCH_SYNTHESIS_HOP, 3.0);
		assert_int_equal(got, M_AUDIO_STRETCH_SYNTHESIS_HOP);
		memcpy(measured + (hop * M_AUDIO_STRETCH_SYNTHESIS_HOP * 2), out, sizeof(out));
	}

	powerL_atL = goertzelPower(measured, keepHops * M_AUDIO_STRETCH_SYNTHESIS_HOP, 0, freqL, SAMPLE_RATE);
	powerL_atR = goertzelPower(measured, keepHops * M_AUDIO_STRETCH_SYNTHESIS_HOP, 0, freqR, SAMPLE_RATE);
	powerR_atL = goertzelPower(measured, keepHops * M_AUDIO_STRETCH_SYNTHESIS_HOP, 1, freqL, SAMPLE_RATE);
	powerR_atR = goertzelPower(measured, keepHops * M_AUDIO_STRETCH_SYNTHESIS_HOP, 1, freqR, SAMPLE_RATE);

	// A channel swap or mono-averaging would fail this in either direction.
	assert_true(powerL_atL > powerL_atR * 50.0);
	assert_true(powerR_atR > powerR_atL * 50.0);
	mAudioTimeStretchDeinit(&ts);
}

/* Reads until `numFrames` have been collected, topping the input up whenever
 * the stretcher comes up short, so the first frames it ever emits after a
 * session begins are what land at the front of `out`. */
static void collectAfterSession(struct mAudioTimeStretch* ts, int16_t* out, int numFrames, int16_t* scratch, double* phase) {
	int got = 0;
	int guard = 0;
	while (got < numFrames && guard++ < 64) {
		int want = numFrames - got;
		int n = mAudioTimeStretchRead(ts, out + (got * 2), want, 1.0);
		got += n;
		if (n < want) {
			fillSine(scratch, 4096, 440.0, phase);
			writeAll(ts, scratch, 4096);
		}
	}
	assert_int_equal(got, numFrames);
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

M_TEST_DEFINE(firstHopAfterSessionIsAtFullLevel) {
	struct mAudioTimeStretch ts;
	static int16_t in[8192 * 2];
	static int16_t out[1024 * 2];
	double phase = 0.0;
	assert_true(mAudioTimeStretchInit(&ts));
	fillSine(in, 8192, 440.0, &phase);
	writeAll(&ts, in, 8192);
	mAudioTimeStretchRead(&ts, out, 256, 1.0);

	mAudioTimeStretchBeginSession(&ts);
	fillSine(in, 4096, 440.0, &phase);
	writeAll(&ts, in, 4096);
	collectAfterSession(&ts, out, 1024, in, &phase);
	/* The first hop after a resync used to carry only the rising half of the
	 * window: a fade-in from silence at every engage. */
	assert_true(rms(out, 0, 64) >= 0.8 * rms(out, 512, 512));
	mAudioTimeStretchDeinit(&ts);
}

M_TEST_SUITE_DEFINE(mAudioTimeStretch,
	cmocka_unit_test(initSucceedsAndStartsEmpty),
	cmocka_unit_test(writeAdvancesTotalWritten),
	cmocka_unit_test(writeRefusesRatherThanLappingConsumer),
	cmocka_unit_test(readAtUnityPreservesFrameCount),
	cmocka_unit_test(compressionConsumesFasterThanItEmits),
	cmocka_unit_test(expansionConsumesSlowerThanItEmits),
	cmocka_unit_test(outputStaysInRangeOnLoudInput),
	cmocka_unit_test(stretchPreservesDominantFrequency),
	cmocka_unit_test(beginSessionResyncsToLiveData),
	cmocka_unit_test(readWithNoInputEmitsNothing),
	cmocka_unit_test(searchReducesAmplitudePumping),
	cmocka_unit_test(targetInputFillGrowsWithArrival),
	cmocka_unit_test(targetInputFillSurvivesAbsurdArrival),
	cmocka_unit_test(readSnapshotsGenerationBeforeWritePos),
	cmocka_unit_test(consumerFloorStopsProducerNotConsumer),
	cmocka_unit_test(channelsStayDistinctThroughStretch),
	cmocka_unit_test(firstHopAfterSessionIsAtFullLevel),
)
