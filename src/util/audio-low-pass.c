/* Copyright (c) 2026 Davey Hughes
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba-util/audio-low-pass.h>

#include <math.h>

// Section Q's for a fourth-order Butterworth cascade.
static const double _sectionQ[2] = { 0.54119610014619698, 1.3065629648763766 };

static int16_t _saturate(double y) {
	long v = lround(y);
	if (v > 32767) {
		v = 32767;
	}
	if (v < -32768) {
		v = -32768;
	}
	return (int16_t) v;
}

static void _design(struct mAudioBiquad* bq, double cutoffHz, double sampleRate, double q) {
	double w0 = 2.0 * M_PI * (cutoffHz / sampleRate);
	double cw = cos(w0);
	double alpha = sin(w0) / (2.0 * q);
	double a0 = 1.0 + alpha;

	bq->b0 = ((1.0 - cw) * 0.5) / a0;
	bq->b1 = (1.0 - cw) / a0;
	bq->b2 = bq->b0;
	bq->a1 = (-2.0 * cw) / a0;
	bq->a2 = (1.0 - alpha) / a0;
}

// Transposed direct form II
static double _run(struct mAudioBiquad* bq, double x, int ch) {
	double y = (bq->b0 * x) + bq->z1[ch];
	bq->z1[ch] = (bq->b1 * x) - (bq->a1 * y) + bq->z2[ch];
	bq->z2[ch] = (bq->b2 * x) - (bq->a2 * y);
	return y;
}

static double _processSample(struct mAudioLowPass* lp, double x, int ch) {
	double y = x;
	int s;
	for (s = 0; s < 2; ++s) {
		y = _run(&lp->stages[s], y, ch);
	}
	return y;
}

static void _setCutoffNow(struct mAudioLowPass* lp, double cutoffHz) {
	int s;
	lp->curCutoff = mAudioClampDouble(cutoffHz, M_AUDIO_LOW_PASS_MIN_CUTOFF, lp->wideOpen);
	for (s = 0; s < 2; ++s) {
		_design(&lp->stages[s], lp->curCutoff, lp->sampleRate, _sectionQ[s]);
	}
}

void mAudioLowPassInit(struct mAudioLowPass* lp, double sampleRate) {
	int s;
	memset(lp, 0, sizeof(*lp));
	lp->sampleRate = sampleRate;
	lp->wideOpen = 0.45 * sampleRate;
	if (lp->wideOpen < M_AUDIO_LOW_PASS_MIN_CUTOFF) {
		lp->wideOpen = M_AUDIO_LOW_PASS_MIN_CUTOFF;
	}
	for (s = 0; s < 2; ++s) {
		lp->stages[s].z1[0] = 0.0;
		lp->stages[s].z1[1] = 0.0;
		lp->stages[s].z2[0] = 0.0;
		lp->stages[s].z2[1] = 0.0;
	}
	_setCutoffNow(lp, lp->wideOpen);
}

double mAudioLowPassWideOpen(const struct mAudioLowPass* lp) {
	return lp->wideOpen;
}

double mAudioLowPassCurrent(const struct mAudioLowPass* lp) {
	return lp->curCutoff;
}

bool mAudioLowPassBypassed(const struct mAudioLowPass* lp) {
	return lp->curCutoff >= (lp->wideOpen * M_AUDIO_LOW_PASS_BYPASS);
}

static void _smooth(struct mAudioLowPass* lp, double targetHz, double blockSeconds) {
	double a;
	targetHz = mAudioClampDouble(targetHz, M_AUDIO_LOW_PASS_MIN_CUTOFF, lp->wideOpen);
	a = 1.0 - exp(-blockSeconds / M_AUDIO_LOW_PASS_TAU);
	_setCutoffNow(lp, lp->curCutoff + ((targetHz - lp->curCutoff) * a));
}

static void _snapshot(const struct mAudioLowPass* lp, double out[2][5]) {
	int s;
	for (s = 0; s < 2; ++s) {
		out[s][0] = lp->stages[s].b0;
		out[s][1] = lp->stages[s].b1;
		out[s][2] = lp->stages[s].b2;
		out[s][3] = lp->stages[s].a1;
		out[s][4] = lp->stages[s].a2;
	}
}

static void _restore(struct mAudioLowPass* lp, const double in[2][5]) {
	int s;
	for (s = 0; s < 2; ++s) {
		lp->stages[s].b0 = in[s][0];
		lp->stages[s].b1 = in[s][1];
		lp->stages[s].b2 = in[s][2];
		lp->stages[s].a1 = in[s][3];
		lp->stages[s].a2 = in[s][4];
	}
}

static void _runBlock(struct mAudioLowPass* lp, int16_t* frames, int numFrames,
                 double targetHz, double blockSeconds, bool emit) {
	bool bypass;
	double from[2][5];
	double to[2][5];
	int i;
	int ch;
	int s;

	if (numFrames < 1) {
		return;
	}

	/* A transposed direct-form II biquad's state encodes its past under the
	 * coefficients that produced it, so replacing them in one go leaves the
	 * two inconsistent and the filter rings once per block. Spread the move
	 * to the new cutoff across the block instead. */
	_snapshot(lp, from);
	_smooth(lp, targetHz, blockSeconds);
	_snapshot(lp, to);
	bypass = mAudioLowPassBypassed(lp);

	for (i = 0; i < numFrames; ++i) {
		double t = (double) (i + 1) / (double) numFrames;
		for (s = 0; s < 2; ++s) {
			lp->stages[s].b0 = from[s][0] + ((to[s][0] - from[s][0]) * t);
			lp->stages[s].b1 = from[s][1] + ((to[s][1] - from[s][1]) * t);
			lp->stages[s].b2 = from[s][2] + ((to[s][2] - from[s][2]) * t);
			lp->stages[s].a1 = from[s][3] + ((to[s][3] - from[s][3]) * t);
			lp->stages[s].a2 = from[s][4] + ((to[s][4] - from[s][4]) * t);
		}
		for (ch = 0; ch < 2; ++ch) {
			// Runs even when bypassed, or re-engaging would click.
			double y = _processSample(lp, frames[(i * 2) + ch], ch);
			if (emit && !bypass) {
				frames[(i * 2) + ch] = _saturate(y);
			}
		}
	}

	// Land exactly on the designed set, so rounding cannot accumulate across blocks.
	_restore(lp, to);
}

void mAudioLowPassProcess(struct mAudioLowPass* lp, int16_t* frames, int numFrames,
                          double targetHz, double blockSeconds) {
	_runBlock(lp, frames, numFrames, targetHz, blockSeconds, true);
}

void mAudioLowPassAdvance(struct mAudioLowPass* lp, int16_t* frames, int numFrames,
                          double targetHz, double blockSeconds) {
	_runBlock(lp, frames, numFrames, targetHz, blockSeconds, false);
}
