/* Copyright (c) 2026 Davey Hughes
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "util/test/suite.h"

#include <mgba-util/audio-speed-filter.h>

#include <math.h>

#define SAMPLE_RATE 48000.0
#define BLOCK 256

static void fillSine(int16_t* out, int frames, double freq, double* phase) {
	int i;
	for (i = 0; i < frames; ++i) {
		double v = sin(*phase) * 12000.0;
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

// Sign changes over the block, zeros skipped: a channel's tone, not its level.
static int zeroCrossings(const int16_t* frames, int count, int ch) {
	int n = 0;
	int prev = 0;
	int i;
	for (i = 0; i < count; ++i) {
		int v = frames[(i * 2) + ch];
		if (v == 0) {
			continue;
		}
		if (prev && ((v < 0) != (prev < 0))) {
			++n;
		}
		prev = v;
	}
	return n;
}

static void pump(struct mAudioSpeedFilter* f, double speed, int callbacks, double* phase) {
	static int16_t in[8192 * 2];
	int16_t out[BLOCK * 2];
	int i;
	for (i = 0; i < callbacks; ++i) {
		int arriving = (int) (BLOCK * speed);
		fillSine(in, arriving, 440.0, phase);
		mAudioSpeedFilterWrite(f, in, arriving);
		mAudioSpeedFilterRead(f, out, BLOCK);
	}
}

// Like pump(), but at a caller-chosen tone; hands back the last block read.
static void pumpFreqInto(struct mAudioSpeedFilter* f, double speed, int callbacks, double freq,
                         double* phase, int16_t* lastOut) {
	static int16_t in[8192 * 2];
	int i;
	for (i = 0; i < callbacks; ++i) {
		int arriving = (int) (BLOCK * speed);
		fillSine(in, arriving, freq, phase);
		mAudioSpeedFilterWrite(f, in, arriving);
		mAudioSpeedFilterRead(f, lastOut, BLOCK);
	}
}

M_TEST_DEFINE(initSucceedsAndStartsDisengaged) {
	struct mAudioSpeedFilter f;
	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	assert_false(mAudioSpeedFilterEngaged(&f));
	assert_true(mAudioSpeedFilterEnabled(&f));
	mAudioSpeedFilterDeinit(&f);
}

M_TEST_DEFINE(staysDisengagedAtNormalSpeed) {
	struct mAudioSpeedFilter f;
	double phase = 0.0;
	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	pump(&f, 1.0, 60, &phase);
	assert_false(mAudioSpeedFilterEngaged(&f));
	mAudioSpeedFilterDeinit(&f);
}

M_TEST_DEFINE(engagesOnSustainedFastForward) {
	struct mAudioSpeedFilter f;
	double phase = 0.0;
	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	pump(&f, 3.0, 120, &phase);
	assert_true(mAudioSpeedFilterEngaged(&f));
	assert_true(mAudioSpeedFilterSpeed(&f) > 2.0);
	mAudioSpeedFilterDeinit(&f);
}

/* Engaging moves only the low-pass target, so the output must run on unbroken.
 * Resyncing the stretcher here instead strands it with no input, taking the
 * stream down on the tail: a dropout at the start of every fast-forward. */
M_TEST_DEFINE(engageEdgeDoesNotBreakTheOutput) {
	struct mAudioSpeedFilter f;
	static int16_t in[8192 * 2];
	static int16_t out[BLOCK * 60 * 2];
	const double level = 12000.0 / sqrt(2.0);
	double phase = 0.0;
	int worst = 0;
	int block;
	int i;

	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	// Off, or the cutoff sliding down over the edge would mask the splice.
	mAudioSpeedFilterSetLowPass(&f, M_AUDIO_LOW_PASS_OFF);
	pump(&f, 1.0, 40, &phase);
	assert_false(mAudioSpeedFilterEngaged(&f));

	for (block = 0; block < 60; ++block) {
		fillSine(in, BLOCK * 3, 440.0, &phase);
		mAudioSpeedFilterWrite(&f, in, BLOCK * 3);
		mAudioSpeedFilterRead(&f, &out[block * BLOCK * 2], BLOCK);
		assert_false(mAudioSpeedFilterStreamEnded(&f));
		assert_true(rms(out, block * BLOCK, BLOCK) > 0.7 * level);
	}
	assert_true(mAudioSpeedFilterEngaged(&f));

	/* A 440 Hz tone at 12000 moves ~700 per frame, and a WSOLA splice adds a
	 * little; a resync would restart the overlap-add at an unrelated phase. */
	for (i = 1; i < BLOCK * 60; ++i) {
		int d = abs(out[i * 2] - out[(i - 1) * 2]);
		if (d > worst) {
			worst = d;
		}
	}
	assert_true(worst < 3000);
	mAudioSpeedFilterDeinit(&f);
}

M_TEST_DEFINE(engagesOnSustainedSlowMotion) {
	struct mAudioSpeedFilter f;
	double phase = 0.0;
	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	pump(&f, 0.5, 120, &phase);
	assert_true(mAudioSpeedFilterEngaged(&f));
	assert_true(mAudioSpeedFilterSpeed(&f) < 0.8);
	mAudioSpeedFilterDeinit(&f);
}

M_TEST_DEFINE(disengagesWhenSpeedReturns) {
	struct mAudioSpeedFilter f;
	double phase = 0.0;
	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	pump(&f, 3.0, 120, &phase);
	assert_true(mAudioSpeedFilterEngaged(&f));
	pump(&f, 1.0, 200, &phase);
	assert_false(mAudioSpeedFilterEngaged(&f));
	mAudioSpeedFilterDeinit(&f);
}

M_TEST_DEFINE(doesNotFlapInsideDeadBand) {
	struct mAudioSpeedFilter f;
	double phase = 0.0;
	// Midway between the thresholds, so retuning them keeps this in the dead band.
	double dead = 1.0 + ((M_AUDIO_DISENGAGE_DEVIATION + M_AUDIO_ENGAGE_DEVIATION) * 0.5);
	double deviation;
	int chunk;

	// Already engaged, then fed dead-band input: must hold state, not drop it.
	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	pump(&f, 3.0, 120, &phase);
	assert_true(mAudioSpeedFilterEngaged(&f));
	for (chunk = 0; chunk < 5; ++chunk) {
		pump(&f, dead, 40, &phase);
		assert_true(mAudioSpeedFilterEngaged(&f));
	}
	deviation = fabs(mAudioSpeedFilterSpeed(&f) - 1.0);
	assert_true(deviation > M_AUDIO_DISENGAGE_DEVIATION);
	assert_true(deviation <= M_AUDIO_ENGAGE_DEVIATION);
	mAudioSpeedFilterDeinit(&f);

	// Converse: from a cold start, dead-band input must not read as off-speed.
	phase = 0.0;
	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	assert_false(mAudioSpeedFilterEngaged(&f));
	for (chunk = 0; chunk < 5; ++chunk) {
		pump(&f, dead, 40, &phase);
		assert_false(mAudioSpeedFilterEngaged(&f));
	}
	deviation = fabs(mAudioSpeedFilterSpeed(&f) - 1.0);
	assert_true(deviation > M_AUDIO_DISENGAGE_DEVIATION);
	assert_true(deviation <= M_AUDIO_ENGAGE_DEVIATION);
	mAudioSpeedFilterDeinit(&f);
}

M_TEST_DEFINE(deadBandDoesNotBlockDisengage) {
	/* Structural overspeed can land in the dead band, so a dead-band reading
	 * must not reset an otherwise-complete disengage run, or a return to
	 * normal play could latch engaged forever. */
	struct mAudioSpeedFilter f;
	double phase = 0.0;
	double dead = 1.0 + ((M_AUDIO_DISENGAGE_DEVIATION + M_AUDIO_ENGAGE_DEVIATION) * 0.5);
	int chunk;

	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	pump(&f, 3.0, 120, &phase);
	assert_true(mAudioSpeedFilterEngaged(&f));

	for (chunk = 0; chunk < 20 && mAudioSpeedFilterEngaged(&f); ++chunk) {
		pump(&f, 1.0, 1, &phase);
		pump(&f, dead, 1, &phase);
	}
	assert_false(mAudioSpeedFilterEngaged(&f));
	mAudioSpeedFilterDeinit(&f);
}

M_TEST_DEFINE(lowPassStaysWideOpenInsideDeadBand) {
	/* At reference 8000 Hz the raw formula gives ~7877 Hz for a speed of
	 * 1.0156, which never engages. The low-pass must stay wide open. */
	struct mAudioSpeedFilter f;
	double phase = 0.0;
	int chunk;
	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	mAudioSpeedFilterSetLowPass(&f, 8000);
	for (chunk = 0; chunk < 5; ++chunk) {
		pump(&f, 1.0156, 40, &phase);
		assert_false(mAudioSpeedFilterEngaged(&f));
		assert_true(mAudioLowPassBypassed(&f.lowPass));
	}
	mAudioSpeedFilterDeinit(&f);
}

M_TEST_DEFINE(disabledNeverEngages) {
	struct mAudioSpeedFilter f;
	double phase = 0.0;
	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));

	// Engage for real first, so disabling has something to undo.
	pump(&f, 3.0, 120, &phase);
	assert_true(mAudioSpeedFilterEngaged(&f));

	mAudioSpeedFilterSetEnabled(&f, false);
	assert_false(mAudioSpeedFilterEngaged(&f));

	// Write and Read short-circuit while disabled, so this must not re-engage.
	pump(&f, 3.0, 120, &phase);
	assert_false(mAudioSpeedFilterEngaged(&f));

	mAudioSpeedFilterDeinit(&f);
}

M_TEST_DEFINE(reenablingAfterDisableStillWorks) {
	struct mAudioSpeedFilter f;
	double phase = 0.0;
	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));

	pump(&f, 3.0, 120, &phase);
	assert_true(mAudioSpeedFilterEngaged(&f));

	// Releases the stretcher's rings; re-enabling has to bring them back.
	mAudioSpeedFilterSetEnabled(&f, false);
	assert_false(mAudioSpeedFilterEnabled(&f));

	mAudioSpeedFilterSetEnabled(&f, true);
	assert_true(mAudioSpeedFilterEnabled(&f));
	assert_false(mAudioSpeedFilterEngaged(&f));

	// Fully functional again, from a clean estimator rather than stale state.
	pump(&f, 3.0, 120, &phase);
	assert_true(mAudioSpeedFilterEngaged(&f));

	mAudioSpeedFilterDeinit(&f);
}

M_TEST_DEFINE(readAlwaysFillsTheBuffer) {
	struct mAudioSpeedFilter f;
	int16_t out[BLOCK * 2];
	int i;
	int nonZero = 0;
	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	// Poison first, so only a callee that overwrites every frame clears this.
	for (i = 0; i < BLOCK * 2; ++i) {
		out[i] = (int16_t) 0x5A5A;
	}
	assert_int_equal(mAudioSpeedFilterRead(&f, out, BLOCK), BLOCK);
	for (i = 0; i < BLOCK * 2; ++i) {
		if (out[i] != 0) {
			++nonZero;
		}
	}
	assert_int_equal(nonZero, 0);
	mAudioSpeedFilterDeinit(&f);
}

M_TEST_DEFINE(underrunFadesRatherThanCutting) {
	struct mAudioSpeedFilter f;
	static int16_t out[BLOCK * 3 * 2];
	double phase = 0.0;
	bool found = false;
	int block;
	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	pump(&f, 3.0, 120, &phase);

	/* The backlog delays the underrun by several reads. The read that comes
	 * up short takes the stream down wherever in the block that happens, so
	 * the two blocks after it lie wholly inside the tail and its mute. */
	for (block = 0; block < 40 && !found; ++block) {
		assert_int_equal(mAudioSpeedFilterRead(&f, out, BLOCK), BLOCK);
		found = mAudioSpeedFilterStreamEnded(&f);
	}
	assert_true(found);
	assert_int_equal(mAudioSpeedFilterRead(&f, &out[BLOCK * 2], BLOCK), BLOCK);
	assert_int_equal(mAudioSpeedFilterRead(&f, &out[BLOCK * 4], BLOCK), BLOCK);

	/* A hard cut zeroes the head and a hold keeps it flat; the tail's level only
	 * ever falls, and by more across 128-frame windows than a partial cycle's RMS
	 * wobbles. Only the first three windows are sure to sit inside the tail. */
	assert_true(abs(out[BLOCK * 2] - out[(BLOCK - 1) * 2]) <= 0.05 * 32767);
	assert_true(rms(out, BLOCK, 128) > rms(out, BLOCK + 128, 128));
	assert_true(rms(out, BLOCK + 128, 128) > rms(out, BLOCK + 256, 128));
	assert_true(rms(out, BLOCK + 384, 128) < 0.5 * rms(out, BLOCK, 128));
	mAudioSpeedFilterDeinit(&f);
}

M_TEST_DEFINE(underrunFadePreservesChannelIdentity) {
	/* pump() drives both channels identically elsewhere, so only this catches
	 * L/R crossed in the ramp's history. */
	struct mAudioSpeedFilter f;
	static int16_t in[8192 * 2];
	int16_t out[BLOCK * 2];
	double phaseL = 0.0;
	double phaseR = 0.0;
	int16_t prevL = 0;
	int16_t prevR = 0;
	bool foundFade = false;
	int block;
	int i;

	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	mAudioSpeedFilterSetLowPass(&f, M_AUDIO_LOW_PASS_OFF);

	for (block = 0; block < 120; ++block) {
		int arriving = (int) (BLOCK * 3.0);
		for (i = 0; i < arriving; ++i) {
			double l = sin(phaseL) * 12000.0;
			double r = sin(phaseR) * 12000.0;
			in[(i * 2) + 0] = (int16_t) l;
			in[(i * 2) + 1] = (int16_t) r;
			phaseL += (2.0 * M_PI * 440.0) / SAMPLE_RATE;
			phaseR += (2.0 * M_PI * 880.0) / SAMPLE_RATE;
		}
		mAudioSpeedFilterWrite(&f, in, arriving);
		mAudioSpeedFilterRead(&f, out, BLOCK);
	}
	assert_true(mAudioSpeedFilterEngaged(&f));
	prevL = out[(BLOCK - 1) * 2 + 0];
	prevR = out[(BLOCK - 1) * 2 + 1];

	/* Find the read that took the stream down; the block after it lies wholly
	 * inside the tail, so its boundary and its content are both the tail's. */
	for (block = 0; block < 40 && !foundFade; ++block) {
		mAudioSpeedFilterRead(&f, out, BLOCK);
		foundFade = mAudioSpeedFilterStreamEnded(&f);
	}
	assert_true(foundFade);
	prevL = out[(BLOCK - 1) * 2 + 0];
	prevR = out[(BLOCK - 1) * 2 + 1];
	mAudioSpeedFilterRead(&f, out, BLOCK);

	/* Each channel continues its own waveform across the boundary, and the
	 * repeat keeps each channel's tone: the 880 Hz side still crosses zero
	 * about twice as often as the 440 Hz side, which a swap would invert. */
	assert_true(abs(out[0] - prevL) <= 0.05 * 32767);
	assert_true(abs(out[1] - prevR) <= 0.05 * 32767);
	assert_true(zeroCrossings(out, BLOCK, 1) > zeroCrossings(out, BLOCK, 0) * 3 / 2);
	mAudioSpeedFilterDeinit(&f);
}

M_TEST_DEFINE(lowPassOffLeavesToneUntouched) {
	struct mAudioSpeedFilter f;
	static int16_t in[8192 * 2];
	int16_t out[BLOCK * 2];
	double phase = 0.0;
	double peak = 0.0;
	int i;
	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	mAudioSpeedFilterSetLowPass(&f, M_AUDIO_LOW_PASS_OFF);
	pump(&f, 3.0, 120, &phase);
	fillSine(in, 768, 440.0, &phase);
	mAudioSpeedFilterWrite(&f, in, 768);
	mAudioSpeedFilterRead(&f, out, BLOCK);
	for (i = 0; i < BLOCK * 2; ++i) {
		if (fabs((double) out[i]) > peak) {
			peak = fabs((double) out[i]);
		}
	}
	assert_true(peak > 6000.0);
	mAudioSpeedFilterDeinit(&f);
}

M_TEST_DEFINE(lowPassAttenuatesToneAboveEngagedCutoff) {
	/* The only test that exercises the low-pass actually engaged: a real
	 * reference against OFF, at a tone above the resulting cutoff. */
	struct mAudioSpeedFilter on;
	struct mAudioSpeedFilter off;
	int16_t outOn[BLOCK * 2];
	int16_t outOff[BLOCK * 2];
	double phaseOn = 0.0;
	double phaseOff = 0.0;
	double peakOn = 0.0;
	double peakOff = 0.0;
	int i;

	assert_true(mAudioSpeedFilterInit(&on, SAMPLE_RATE));
	assert_true(mAudioSpeedFilterInit(&off, SAMPLE_RATE));
	mAudioSpeedFilterSetLowPass(&on, 8000);
	mAudioSpeedFilterSetLowPass(&off, M_AUDIO_LOW_PASS_OFF);

	// Long enough to converge; at speed 3, reference 8000 Hz settles near 2667 Hz.
	pump(&on, 3.0, 120, &phaseOn);
	pump(&off, 3.0, 120, &phaseOff);
	assert_true(mAudioSpeedFilterEngaged(&on));
	assert_true(mAudioSpeedFilterEngaged(&off));

	// Above the engaged cutoff, and long enough to flush the backlog.
	pumpFreqInto(&on, 3.0, 40, 6000.0, &phaseOn, outOn);
	pumpFreqInto(&off, 3.0, 40, 6000.0, &phaseOff, outOff);

	for (i = 0; i < BLOCK * 2; ++i) {
		if (fabs((double) outOn[i]) > peakOn) {
			peakOn = fabs((double) outOn[i]);
		}
		if (fabs((double) outOff[i]) > peakOff) {
			peakOff = fabs((double) outOff[i]);
		}
	}

	assert_true(peakOff > 6000.0);
	// Substantially attenuated, not merely different: search noise nudges the peak.
	assert_true(peakOn < peakOff * 0.5);
	mAudioSpeedFilterDeinit(&on);
	mAudioSpeedFilterDeinit(&off);
}

M_TEST_DEFINE(setSampleRateIsNoOpWhenUnchanged) {
	struct mAudioSpeedFilter f;
	double phase = 0.0;
	double arrivalAvg;
	double speed;
	int offSpeedRuns;
	int onSpeedRuns;
	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	pump(&f, 3.0, 120, &phase);
	assert_true(mAudioSpeedFilterEngaged(&f));
	arrivalAvg = f.arrivalAvg;
	speed = f.speed;
	offSpeedRuns = f.offSpeedRuns;
	onSpeedRuns = f.onSpeedRuns;

	// Same rate passed back in: arrival and hysteresis state may not move.
	mAudioSpeedFilterSetSampleRate(&f, SAMPLE_RATE);

	assert_true(fabs(f.arrivalAvg - arrivalAvg) < 1e-9);
	assert_true(fabs(f.speed - speed) < 1e-9);
	assert_int_equal(f.offSpeedRuns, offSpeedRuns);
	assert_int_equal(f.onSpeedRuns, onSpeedRuns);
	assert_true(mAudioSpeedFilterEngaged(&f));
	mAudioSpeedFilterDeinit(&f);
}

M_TEST_DEFINE(setSampleRateRejectsNonPositiveValues) {
	struct mAudioSpeedFilter f;
	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	mAudioSpeedFilterSetSampleRate(&f, 0.0);
	assert_true(fabs(f.sampleRate - SAMPLE_RATE) < 1e-9);
	mAudioSpeedFilterSetSampleRate(&f, -48000.0);
	assert_true(fabs(f.sampleRate - SAMPLE_RATE) < 1e-9);
	mAudioSpeedFilterSetSampleRate(&f, NAN);
	assert_true(fabs(f.sampleRate - SAMPLE_RATE) < 1e-9);
	mAudioSpeedFilterDeinit(&f);
}

M_TEST_DEFINE(setSampleRateResetsArrivalStateOnChange) {
	struct mAudioSpeedFilter f;
	double phase = 0.0;
	double newRate = SAMPLE_RATE * 2.0;
	double oldWideOpen;
	double newWideOpen;
	int16_t in[BLOCK * 2];
	int16_t out[BLOCK * 2];

	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	mAudioSpeedFilterSetLowPass(&f, 8000);

	// Drive to engaged first, or a no-op SetSampleRate would pass by accident.
	pump(&f, 3.0, 120, &phase);
	assert_true(mAudioSpeedFilterEngaged(&f));
	assert_true(f.arrivalAvg > 0.0);
	assert_true(f.offSpeedRuns > 0);

	oldWideOpen = mAudioLowPassWideOpen(&f.lowPass);

	mAudioSpeedFilterSetSampleRate(&f, newRate);

	assert_true(fabs(f.sampleRate - newRate) < 1e-9);

	// Wide-open cutoff (0.45 * sampleRate) must track the new rate.
	newWideOpen = mAudioLowPassWideOpen(&f.lowPass);
	assert_true(newWideOpen > oldWideOpen * 1.9);
	assert_true(newWideOpen < oldWideOpen * 2.1);

	/* Arrival estimate and run counters must clear, but engaged must survive:
	 * a rate change doesn't itself end fast-forward. */
	assert_true(fabs(f.arrivalAvg) < 1e-9);
	assert_true(fabs(f.speed - 1.0) < 1e-9);
	assert_int_equal(f.offSpeedRuns, 0);
	assert_int_equal(f.onSpeedRuns, 0);
	assert_true(f.windowArrival == 0);
	assert_int_equal(f.windowOutput, 0);
	assert_false(f.windowOpen);
	assert_true(mAudioSpeedFilterEnabled(&f));
	assert_int_equal(f.lowPassReferenceHz, 8000);
	assert_true(mAudioSpeedFilterEngaged(&f));

	// lastWritten resyncs to the running total, not zero, or this reads as a burst.
	fillSine(in, BLOCK, 440.0, &phase);
	mAudioSpeedFilterWrite(&f, in, BLOCK);
	mAudioSpeedFilterRead(&f, out, BLOCK);
	assert_true(mAudioSpeedFilterSpeed(&f) < 5.0);

	mAudioSpeedFilterDeinit(&f);
}

M_TEST_DEFINE(tracksASpeedStepWithinAFewCallbacks) {
	struct mAudioSpeedFilter f;
	static int16_t in[8192 * 2];
	int16_t out[BLOCK * 2];
	double phase = 0.0;
	int block;
	int settled = -1;

	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	pump(&f, 1.0, 40, &phase);

	// A fixed 0.05 gain needs ~70 callbacks for a 1x->3x step; this must take a few.
	for (block = 0; block < 20; ++block) {
		int arriving = BLOCK * 3;
		fillSine(in, arriving, 440.0, &phase);
		mAudioSpeedFilterWrite(&f, in, arriving);
		mAudioSpeedFilterRead(&f, out, BLOCK);
		if (settled < 0 && fabs(mAudioSpeedFilterSpeed(&f) - 3.0) < 0.15) {
			settled = block;
		}
	}

	assert_true(settled >= 0);
	assert_true(settled < 10);
	mAudioSpeedFilterDeinit(&f);
}

M_TEST_DEFINE(structuralOverspeedDoesNotEngage) {
	/* mCoreSync's high-water mark carries a samples/64 cushion, so a core at
	 * nominal 1x delivers fast by (16 + samples/64) / samples. That is normal
	 * play and must not engage, at every buffer size SettingsView.ui offers. */
	static const double overspeed[] = {
		1.0 + (17.0 / 64.0),  /* buffer  64: (16 + 64/64)/64   ~= 26.6% */
		1.0 + (17.0 / 96.0),  /* buffer  96: (16 + 96/64)/96   ~= 17.7% */
		1.078,                /* buffer 256: (16 + 256/64)/256 ~= 7.8% */
	};
	int i;
	for (i = 0; i < (int) (sizeof(overspeed) / sizeof(overspeed[0])); ++i) {
		struct mAudioSpeedFilter f;
		double phase = 0.0;
		int chunk;
		assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
		mAudioSpeedFilterSetLowPass(&f, 8000);
		for (chunk = 0; chunk < 10; ++chunk) {
			pump(&f, overspeed[i], 40, &phase);
			assert_false(mAudioSpeedFilterEngaged(&f));
		}
		assert_true(mAudioLowPassBypassed(&f.lowPass));
		mAudioSpeedFilterDeinit(&f);
	}
}

M_TEST_DEFINE(isolatedArrivalStallsDoNotEngage) {
	/* Real Qt playback crosses M_AUDIO_ENGAGE_DEVIATION on under 1% of callbacks,
	 * always in isolation, so RUNS_TO_SWITCH is what holds it disengaged. Stall
	 * ~25x more often than measured: the dips must happen and must not engage. */
	struct mAudioSpeedFilter f;
	double phase = 0.0;
	double overspeed = 1.0 + (17.0 / 64.0);
	int offSpeedReadings = 0;
	int cycle;
	int i;

	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	for (cycle = 0; cycle < 40; ++cycle) {
		// Two callbacks starve, then delivery resumes at structural overspeed.
		for (i = 0; i < 22; ++i) {
			pump(&f, i < 2 ? 0.05 : overspeed, 1, &phase);
			if (mAudioIsOffSpeed(mAudioSpeedFilterSpeed(&f))) {
				++offSpeedReadings;
			}
			assert_false(mAudioSpeedFilterEngaged(&f));
		}
	}
	assert_true(offSpeedReadings > 0);
	mAudioSpeedFilterDeinit(&f);
}

M_TEST_DEFINE(staysDisengagedWithKReadsPerDelivery) {
	/* pump() writes before every Read, but neither frontend does: both chunk
	 * one delivery across several Reads. Model that directly. */
	static int16_t in[8192 * 2];
	int16_t out[BLOCK * 2];
	struct mAudioSpeedFilter f;
	static const int ks[] = {2, 3, 5};
	int ki;

	for (ki = 0; ki < (int) (sizeof(ks) / sizeof(ks[0])); ++ki) {
		int k = ks[ki];
		double phase = 0.0;
		int delivery;

		assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
		for (delivery = 0; delivery < 60; ++delivery) {
			int arriving = BLOCK * k;
			int read;
			fillSine(in, arriving, 440.0, &phase);
			mAudioSpeedFilterWrite(&f, in, arriving);
			for (read = 0; read < k; ++read) {
				mAudioSpeedFilterRead(&f, out, BLOCK);
				assert_false(mAudioSpeedFilterEngaged(&f));
				assert_true(fabs(mAudioSpeedFilterSpeed(&f) - 1.0) <= M_AUDIO_DISENGAGE_DEVIATION);
			}
		}
		mAudioSpeedFilterDeinit(&f);
	}
}

M_TEST_DEFINE(setLowPassZeroReadsAsOff) {
	struct mAudioSpeedFilter f;
	assert_true(mAudioSpeedFilterInit(&f, 48000.0));
	mAudioSpeedFilterSetLowPass(&f, 0);
	assert_int_equal(f.lowPassReferenceHz, M_AUDIO_LOW_PASS_OFF);
	mAudioSpeedFilterDeinit(&f);
}

/* A pause takes the stream down while the stretcher still holds pre-pause
 * audio. Once the tail and its mute are spent nothing may bring that backlog
 * back; the stream only returns on StreamBegin, on the ramp. */
M_TEST_DEFINE(suspendedStreamStaysSilentUntilBegin) {
	struct mAudioSpeedFilter f;
	static int16_t out[BLOCK * 12 * 2];
	static int16_t in[BLOCK * 2];
	double phase = 0.0;
	int rampStart = -1;
	int block;
	int i;
	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	// A 3x run leaves the stretcher holding thousands of frames of reserve.
	pump(&f, 3.0, 120, &phase);
	assert_true(mAudioTimeStretchInputFill(&f.stretch) > 1024);

	mAudioSpeedFilterStreamEnd(&f);
	// The tail and the mute are spent inside four device blocks.
	for (block = 0; block < 4; ++block) {
		assert_int_equal(mAudioSpeedFilterRead(&f, out, BLOCK), BLOCK);
	}
	// From here on every frame is silent, however much the stretcher holds.
	for (block = 0; block < 8; ++block) {
		assert_int_equal(mAudioSpeedFilterRead(&f, out, BLOCK), BLOCK);
		for (i = 0; i < BLOCK * 2; ++i) {
			assert_int_equal(out[i], 0);
		}
	}

	mAudioSpeedFilterStreamBegin(&f);
	for (block = 0; block < 12; ++block) {
		fillSine(in, BLOCK, 440.0, &phase);
		mAudioSpeedFilterWrite(&f, in, BLOCK);
		assert_int_equal(mAudioSpeedFilterRead(&f, &out[block * BLOCK * 2], BLOCK), BLOCK);
	}
	for (i = 0; i < BLOCK * 12 && rampStart < 0; ++i) {
		if (out[i * 2]) {
			rampStart = i;
		}
	}
	assert_true(rampStart >= 0);
	assert_true(rampStart + 512 + 1024 <= BLOCK * 12);
	// The resume opens from silence and climbs to the tone over the ramp;
	// nothing plays at level before it.
	assert_true(abs(out[rampStart * 2]) < 0.02 * 12000.0);
	assert_true(rms(out, rampStart, 128) < 0.3 * (12000.0 / sqrt(2.0)));
	assert_true(fabs(rms(out, rampStart + 512, 1024) - (12000.0 / sqrt(2.0))) < 0.02 * (12000.0 / sqrt(2.0)));
	mAudioSpeedFilterDeinit(&f);
}

/* AudioProcessorQt::start() and mSDLResumeAudio() both reach StreamBegin from
 * an unpaused() that never had a matching paused() - the Qt scripting engine
 * emits one mid-play. Nothing took the stream down, so nothing may be brought
 * back: the resync would drop a live reserve and the ramp would fade audio
 * that never stopped. */
M_TEST_DEFINE(streamBeginOnALiveStreamIsANoOp) {
	struct mAudioSpeedFilter f;
	static int16_t in[BLOCK * 2];
	static int16_t out[BLOCK * 2];
	const double level = 12000.0 / sqrt(2.0);
	double phase = 0.0;
	int block;

	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	mAudioSpeedFilterSetLowPass(&f, M_AUDIO_LOW_PASS_OFF);
	pump(&f, 1.0, 120, &phase);
	assert_false(mAudioSpeedFilterStreamEnded(&f));

	mAudioSpeedFilterStreamBegin(&f);

	for (block = 0; block < 4; ++block) {
		fillSine(in, BLOCK, 440.0, &phase);
		mAudioSpeedFilterWrite(&f, in, BLOCK);
		mAudioSpeedFilterRead(&f, out, BLOCK);
		assert_false(mAudioSpeedFilterStreamEnded(&f));
		assert_true(rms(out, 0, BLOCK) > 0.8 * level);
	}
	mAudioSpeedFilterDeinit(&f);
}

/* A state load landing during a stutter finds the stream already down on an
 * underrun tail, so JumpBegin declines. That tail heals itself through the
 * ramp's own latch, with no resync, so JumpEnd has to do the resync or the
 * pre-jump reserve splices onto post-jump audio behind the tail. */
M_TEST_DEFINE(jumpEndResyncsWhenBeginDeclined) {
	struct mAudioSpeedFilter f;
	static int16_t out[BLOCK * 2];
	double phase = 0.0;
	uint32_t generation;
	bool down = false;
	bool ramped;
	int block;

	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	pump(&f, 3.0, 120, &phase);
	// Read on without writing until the backlog runs out and the tail arms.
	for (block = 0; block < 40 && !down; ++block) {
		mAudioSpeedFilterRead(&f, out, BLOCK);
		down = mAudioSpeedFilterStreamEnded(&f);
	}
	assert_true(down);
	assert_false(f.suspended);

	generation = f.stretch.generation;
	ramped = mAudioSpeedFilterJumpBegin(&f);
	assert_false(ramped);
	mAudioSpeedFilterJumpEnd(&f, ramped);

	assert_true(f.stretch.generation != generation);
	// The tail is still healing itself; nothing here may lift the suspend.
	assert_false(f.suspended);
	mAudioSpeedFilterDeinit(&f);
}

/* JumpBegin also declines while the device is suspended. The resume that lifts
 * the suspend owns the ramp-in, so JumpEnd must leave the stream down. */
M_TEST_DEFINE(jumpEndDuringSuspendLeavesTheStreamDown) {
	struct mAudioSpeedFilter f;
	static int16_t out[BLOCK * 2];
	double phase = 0.0;
	bool ramped;
	int block;
	int i;

	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));
	pump(&f, 1.0, 120, &phase);
	mAudioSpeedFilterStreamEnd(&f);
	// Spend the tail and its mute, so only `suspended` still holds the stream.
	for (block = 0; block < 4; ++block) {
		mAudioSpeedFilterRead(&f, out, BLOCK);
	}
	assert_true(f.suspended);
	assert_false(mAudioSpeedFilterStreamEnded(&f));

	ramped = mAudioSpeedFilterJumpBegin(&f);
	assert_false(ramped);
	mAudioSpeedFilterJumpEnd(&f, ramped);
	assert_true(f.suspended);

	mAudioSpeedFilterRead(&f, out, BLOCK);
	for (i = 0; i < BLOCK * 2; ++i) {
		assert_int_equal(out[i], 0);
	}
	mAudioSpeedFilterDeinit(&f);
}

M_TEST_DEFINE(zeroLengthReadLeavesTheStreamUp) {
	struct mAudioSpeedFilter f;
	int16_t out[BLOCK * 2];
	double phase = 0.0;
	assert_true(mAudioSpeedFilterInit(&f, SAMPLE_RATE));

	pump(&f, 1.0, 40, &phase);
	assert_false(mAudioSpeedFilterStreamEnded(&f));

	// A no-op call must not arm the concealment tail.
	assert_int_equal(mAudioSpeedFilterRead(&f, out, 0), 0);
	assert_false(mAudioSpeedFilterStreamEnded(&f));

	// And the next real read still carries audio rather than a tail.
	pump(&f, 1.0, 1, &phase);
	mAudioSpeedFilterRead(&f, out, BLOCK);
	assert_true(rms(out, 0, BLOCK) > 1000.0);
	mAudioSpeedFilterDeinit(&f);
}

M_TEST_SUITE_DEFINE(mAudioSpeedFilter,
	cmocka_unit_test(initSucceedsAndStartsDisengaged),
	cmocka_unit_test(tracksASpeedStepWithinAFewCallbacks),
	cmocka_unit_test(staysDisengagedAtNormalSpeed),
	cmocka_unit_test(engagesOnSustainedFastForward),
	cmocka_unit_test(engageEdgeDoesNotBreakTheOutput),
	cmocka_unit_test(engagesOnSustainedSlowMotion),
	cmocka_unit_test(disengagesWhenSpeedReturns),
	cmocka_unit_test(doesNotFlapInsideDeadBand),
	cmocka_unit_test(deadBandDoesNotBlockDisengage),
	cmocka_unit_test(structuralOverspeedDoesNotEngage),
	cmocka_unit_test(isolatedArrivalStallsDoNotEngage),
	cmocka_unit_test(staysDisengagedWithKReadsPerDelivery),
	cmocka_unit_test(lowPassStaysWideOpenInsideDeadBand),
	cmocka_unit_test(disabledNeverEngages),
	cmocka_unit_test(reenablingAfterDisableStillWorks),
	cmocka_unit_test(readAlwaysFillsTheBuffer),
	cmocka_unit_test(underrunFadesRatherThanCutting),
	cmocka_unit_test(underrunFadePreservesChannelIdentity),
	cmocka_unit_test(suspendedStreamStaysSilentUntilBegin),
	cmocka_unit_test(streamBeginOnALiveStreamIsANoOp),
	cmocka_unit_test(jumpEndResyncsWhenBeginDeclined),
	cmocka_unit_test(jumpEndDuringSuspendLeavesTheStreamDown),
	cmocka_unit_test(lowPassOffLeavesToneUntouched),
	cmocka_unit_test(lowPassAttenuatesToneAboveEngagedCutoff),
	cmocka_unit_test(setSampleRateIsNoOpWhenUnchanged),
	cmocka_unit_test(setSampleRateRejectsNonPositiveValues),
	cmocka_unit_test(setSampleRateResetsArrivalStateOnChange),
	cmocka_unit_test(setLowPassZeroReadsAsOff),
	cmocka_unit_test(zeroLengthReadLeavesTheStreamUp),
)
