/* Copyright (c) 2026 Davey Hughes
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba-util/audio-stream-ramp.h>

#include <math.h>

static int16_t _saturate(double v) {
	long s = lround(v);
	if (s > 32767) {
		s = 32767;
	}
	if (s < -32768) {
		s = -32768;
	}
	return (int16_t) s;
}

void mAudioStreamRampInit(struct mAudioStreamRamp* r) {
	mAudioStreamRampReset(r);
}

void mAudioStreamRampReset(struct mAudioStreamRamp* r) {
	memset(r, 0, sizeof(*r));
}

bool mAudioStreamRampEnded(const struct mAudioStreamRamp* r) {
	return r->tailFrames > 0 || r->muteFrames > 0;
}

/* Index of the frame `age` back from the most recent in the frozen snapshot,
 * age 1 being the most recent. _findPeriod walks the ring with _tailPrev
 * instead, so the modulo is paid once per call rather than once per candidate. */
static unsigned _tailIdx(const struct mAudioStreamRamp* r, unsigned age) {
	return (r->tailSrcPos + M_AUDIO_RAMP_HIST - age) % M_AUDIO_RAMP_HIST;
}

static const int16_t* _tailAt(const struct mAudioStreamRamp* r, unsigned age) {
	return &r->tailSrc[_tailIdx(r, age) * 2];
}

// One frame older, wrapping without a modulo.
static unsigned _tailPrev(unsigned idx) {
	return idx ? (idx - 1) : (M_AUDIO_RAMP_HIST - 1);
}

/* Period whose lag matches the last M_AUDIO_RAMP_CORR frames best, 0 when there
 * is no periodic content. Scored by squared distance rather than correlation,
 * which is blind to level and would repeat a phrase at the wrong volume. */
static unsigned _findPeriod(const struct mAudioStreamRamp* r) {
	// The reference window is the same for every candidate; sum it once.
	double ref[M_AUDIO_RAMP_CORR];
	unsigned p;
	unsigned maxP = M_AUDIO_RAMP_MAX_PERIOD;
	unsigned k;
	unsigned idx;
	double eRef = 0.0;
	double bestDiff = -1.0;
	unsigned best = 0;

	if (r->tailSrcFill < M_AUDIO_RAMP_MIN_PERIOD + M_AUDIO_RAMP_CORR) {
		return 0;
	}
	if (maxP > r->tailSrcFill - M_AUDIO_RAMP_CORR) {
		maxP = r->tailSrcFill - M_AUDIO_RAMP_CORR;
	}

	idx = _tailIdx(r, 1);
	for (k = 0; k < M_AUDIO_RAMP_CORR; ++k) {
		const int16_t* f = &r->tailSrc[idx * 2];
		double v = (double) f[0] + (double) f[1];
		ref[k] = v;
		eRef += v * v;
		idx = _tailPrev(idx);
	}
	if (eRef <= 0.0) {
		return 0;
	}

	for (p = M_AUDIO_RAMP_MIN_PERIOD; p <= maxP; ++p) {
		double diff = 0.0;
		unsigned ib = _tailIdx(r, 1 + p);
		for (k = 0; k < M_AUDIO_RAMP_CORR; ++k) {
			const int16_t* b = &r->tailSrc[ib * 2];
			double d = ref[k] - ((double) b[0] + b[1]);
			diff += d * d;
			ib = _tailPrev(ib);
		}
		if (bestDiff < 0.0 || diff < bestDiff) {
			bestDiff = diff;
			best = p;
		}
	}
	return best;
}

void mAudioStreamRampEnd(struct mAudioStreamRamp* r) {
	if (mAudioStreamRampEnded(r)) {
		return;
	}
	if (r->lastOut[0] == 0 && r->lastOut[1] == 0) {
		return;
	}

	// Frozen so recording can resume without the tail reading its own output back.
	memcpy(r->tailSrc, r->hist, sizeof(r->tailSrc));
	r->tailSrcPos = r->histPos;
	r->tailSrcFill = r->histFill;

	r->tailPeriod = _findPeriod(r);
	r->tailFrames = M_AUDIO_RAMP_TAIL;
	r->muteFrames = 0;
	r->fadeInFrames = 0;
	// Covers both an explicit resume and an underrun healing itself.
	r->rampInLatched = true;

	if (r->tailPeriod) {
		const int16_t* f = _tailAt(r, r->tailPeriod + 1);
		r->tailJoin[0] = (int32_t) r->lastOut[0] - f[0];
		r->tailJoin[1] = (int32_t) r->lastOut[1] - f[1];
	} else {
		r->tailJoin[0] = 0;
		r->tailJoin[1] = 0;
	}
}

void mAudioStreamRampFillTail(struct mAudioStreamRamp* r, int16_t* frames, int numFrames) {
	int i;
	for (i = 0; i < numFrames; ++i) {
		if (r->tailFrames > 0) {
			unsigned pos = M_AUDIO_RAMP_TAIL - r->tailFrames;
			/* Raised cosine: a linear ramp still corners at both ends. */
			double g = 0.5 * (1.0 + cos(M_PI * (double) (pos + 1) / (double) M_AUDIO_RAMP_TAIL));
			if (r->tailPeriod) {
				unsigned u = pos % r->tailPeriod;
				const int16_t* f = _tailAt(r, r->tailPeriod - u);
				unsigned jn = (M_AUDIO_RAMP_JOIN < r->tailPeriod) ? M_AUDIO_RAMP_JOIN : r->tailPeriod - 1;
				// The join step recurs at every wrap, so decay it within the period.
				double jw = (u < jn) ? 1.0 - (double) u / (double) jn : 0.0;
				frames[(i * 2) + 0] = _saturate((f[0] + r->tailJoin[0] * jw) * g);
				frames[(i * 2) + 1] = _saturate((f[1] + r->tailJoin[1] * jw) * g);
			} else {
				frames[(i * 2) + 0] = _saturate(r->lastOut[0] * g);
				frames[(i * 2) + 1] = _saturate(r->lastOut[1] * g);
			}
			--r->tailFrames;
			if (r->tailFrames == 0) {
				r->muteFrames = M_AUDIO_RAMP_MUTE;
				r->lastOut[0] = 0;
				r->lastOut[1] = 0;
			}
		} else if (r->muteFrames > 0) {
			frames[(i * 2) + 0] = 0;
			frames[(i * 2) + 1] = 0;
			--r->muteFrames;
		} else {
			frames[(i * 2) + 0] = 0;
			frames[(i * 2) + 1] = 0;
		}
	}
}

void mAudioStreamRampBegin(struct mAudioStreamRamp* r) {
	// Always latched: Track decides where the ramp actually opens.
	r->rampInLatched = true;
}

static void _pushHistory(struct mAudioStreamRamp* r, const int16_t* frames, int numFrames) {
	int i;
	for (i = 0; i < numFrames; ++i) {
		r->hist[(r->histPos * 2) + 0] = frames[(i * 2) + 0];
		r->hist[(r->histPos * 2) + 1] = frames[(i * 2) + 1];
		r->histPos = (r->histPos + 1) % M_AUDIO_RAMP_HIST;
	}
	r->histFill += numFrames;
	if (r->histFill > M_AUDIO_RAMP_HIST) {
		r->histFill = M_AUDIO_RAMP_HIST;
	}
	if (numFrames > 0) {
		r->lastOut[0] = frames[((numFrames - 1) * 2) + 0];
		r->lastOut[1] = frames[((numFrames - 1) * 2) + 1];
	}
}

void mAudioStreamRampTrack(struct mAudioStreamRamp* r, int16_t* frames, int numFrames) {
	int first = 0;

	if (r->rampInLatched && !mAudioStreamRampEnded(r)) {
		// Open the ramp where the audio does: a load into a quiet moment would
		// spend it on zeros and splice the first real frames in at full level.
		while (first < numFrames && !frames[(first * 2) + 0] && !frames[(first * 2) + 1]) {
			++first;
		}
		if (first < numFrames) {
			r->fadeInFrames = M_AUDIO_RAMP_IN;
			r->rampInLatched = false;
		} else {
			first = 0;
		}
	}

	if (r->fadeInFrames > 0) {
		int avail = numFrames - first;
		int n = (r->fadeInFrames < (unsigned) avail) ? (int) r->fadeInFrames : avail;
		int j;
		for (j = 0; j < n; ++j) {
			// Against the whole ramp, or the gain restarts near zero every buffer.
			unsigned done = M_AUDIO_RAMP_IN - r->fadeInFrames + (unsigned) j + 1;
			double g = 0.5 * (1.0 - cos(M_PI * (double) done / (double) M_AUDIO_RAMP_IN));
			frames[((first + j) * 2) + 0] = _saturate(frames[((first + j) * 2) + 0] * g);
			frames[((first + j) * 2) + 1] = _saturate(frames[((first + j) * 2) + 1] * g);
		}
		r->fadeInFrames -= (unsigned) n;
	}

	_pushHistory(r, frames, numFrames);
}
