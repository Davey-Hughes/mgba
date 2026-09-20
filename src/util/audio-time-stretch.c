/* Copyright (c) 2026 Davey Hughes
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba-util/audio-time-stretch.h>

#include <math.h>

#define IN_MASK (M_AUDIO_STRETCH_INPUT_CAPACITY - 1)
#define OUT_MASK (M_AUDIO_STRETCH_OUTPUT_CAPACITY - 1)

/* writePos and consumerFloor cross the producer/consumer boundary at 64 bits,
 * and common.h's ATOMIC_LOAD/ATOMIC_STORE are 32-bit on its MSVC branches.
 * Mirror all three of its branches here at int64_t width. */
#if !defined(_MSC_VER) && (defined(__llvm__) || (__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 7))
#define M_ATOMIC_LOAD64(DST, SRC) ((DST) = __atomic_load_n(&(SRC), __ATOMIC_ACQUIRE))
#define M_ATOMIC_STORE64(DST, SRC) __atomic_store_n(&(DST), (SRC), __ATOMIC_RELEASE)
#elif defined(_MSC_VER) && defined(_M_IX86)
/* The Interlocked*64 family is documented ARM/x64/ARM64-only apart from
 * CompareExchange64, so build both ops from that: the load is common.h's own
 * CAS-as-a-load trick, the store a retry loop, safe because each of these has
 * exactly one writer. */
static __inline int64_t _mAtomicLoad64(LONG64 volatile* p) {
	return InterlockedCompareExchange64(p, 0, 0);
}
static __inline void _mAtomicStore64(LONG64 volatile* p, LONG64 v) {
	LONG64 old = *p;
	while (InterlockedCompareExchange64(p, v, old) != old) {
		old = *p;
	}
}
#define M_ATOMIC_LOAD64(DST, SRC) ((DST) = _mAtomicLoad64((LONG64 volatile*) &(SRC)))
#define M_ATOMIC_STORE64(DST, SRC) _mAtomicStore64((LONG64 volatile*) &(DST), (SRC))
#elif defined(_MSC_VER)
#define M_ATOMIC_LOAD64(DST, SRC) ((DST) = InterlockedOr64Acquire((LONG64 volatile*) &(SRC), 0))
#define M_ATOMIC_STORE64(DST, SRC) InterlockedExchange64((LONG64 volatile*) &(DST), (SRC))
#else
// Matches common.h's own non-atomic fallback, which can tear at 64 bits.
#define M_ATOMIC_LOAD64(DST, SRC) ((DST) = (SRC))
#define M_ATOMIC_STORE64(DST, SRC) ((DST) = (SRC))
#endif

static int16_t _saturate(float v) {
	long s = lroundf(v);
	if (s > 32767) {
		s = 32767;
	}
	if (s < -32768) {
		s = -32768;
	}
	return (int16_t) s;
}

static int64_t _min64(int64_t a, int64_t b) {
	return a < b ? a : b;
}

static int64_t _max64(int64_t a, int64_t b) {
	return a > b ? a : b;
}

static void _reset(struct mAudioTimeStretch* ts);

bool mAudioTimeStretchInit(struct mAudioTimeStretch* ts) {
	int i;
	memset(ts, 0, sizeof(*ts));

	ts->inL = calloc(M_AUDIO_STRETCH_INPUT_CAPACITY, sizeof(int16_t));
	ts->inR = calloc(M_AUDIO_STRETCH_INPUT_CAPACITY, sizeof(int16_t));
	ts->inMono = calloc(M_AUDIO_STRETCH_INPUT_CAPACITY, sizeof(float));
	ts->outL = calloc(M_AUDIO_STRETCH_OUTPUT_CAPACITY, sizeof(int16_t));
	ts->outR = calloc(M_AUDIO_STRETCH_OUTPUT_CAPACITY, sizeof(int16_t));
	if (!ts->inL || !ts->inR || !ts->inMono || !ts->outL || !ts->outR) {
		mAudioTimeStretchDeinit(ts);
		return false;
	}

	for (i = 0; i < M_AUDIO_STRETCH_FRAME_SIZE; ++i) {
		ts->window[i] = (float) (0.5 * (1.0 - cos((2.0 * M_PI * i) / M_AUDIO_STRETCH_FRAME_SIZE)));
	}
	_reset(ts);
	return true;
}

void mAudioTimeStretchDeinit(struct mAudioTimeStretch* ts) {
	free(ts->inL);
	free(ts->inR);
	free(ts->inMono);
	free(ts->outL);
	free(ts->outR);
	ts->inL = 0;
	ts->inR = 0;
	ts->inMono = 0;
	ts->outL = 0;
	ts->outR = 0;
}

static void _reset(struct mAudioTimeStretch* ts) {
	ts->writePos = 0;
	ts->generation = 0;
	ts->consumerFloor = 0;
	ts->seenWrite = 0;
	ts->seenGeneration = 0;
	ts->analysisPos = 0;
	ts->naturalPos = 0;
	ts->outReadPos = 0;
	ts->outWritePos = 0;
	ts->primed = false;
	memset(ts->accL, 0, sizeof(ts->accL));
	memset(ts->accR, 0, sizeof(ts->accR));

	memset(ts->inL, 0, M_AUDIO_STRETCH_INPUT_CAPACITY * sizeof(int16_t));
	memset(ts->inR, 0, M_AUDIO_STRETCH_INPUT_CAPACITY * sizeof(int16_t));
	memset(ts->inMono, 0, M_AUDIO_STRETCH_INPUT_CAPACITY * sizeof(float));
}

int mAudioTimeStretchInputFill(struct mAudioTimeStretch* ts) {
	int64_t writePos;
	int64_t pending;
	M_ATOMIC_LOAD64(writePos, ts->writePos);
	pending = writePos - ts->analysisPos;
	if (pending < 0) {
		return 0;
	}
	if (pending > M_AUDIO_STRETCH_INPUT_CAPACITY) {
		return M_AUDIO_STRETCH_INPUT_CAPACITY;
	}
	return (int) pending;
}

int mAudioTimeStretchOutputFill(const struct mAudioTimeStretch* ts) {
	return (int) (ts->outWritePos - ts->outReadPos);
}

int64_t mAudioTimeStretchTotalWritten(struct mAudioTimeStretch* ts) {
	int64_t writePos;
	M_ATOMIC_LOAD64(writePos, ts->writePos);
	return writePos;
}

int mAudioTimeStretchTargetInputFill(double arrivalPerCallback) {
	// Half the ring, so a high ratio's floor can't starve Write.
	const double cap = M_AUDIO_STRETCH_INPUT_CAPACITY / 2;
	double need = (2.0 * arrivalPerCallback) + M_AUDIO_STRETCH_SEARCH_RADIUS
	              + M_AUDIO_STRETCH_FRAME_SIZE;
	// Compared as double: casting an absurd estimate to int first is UB.
	if (!(need > M_AUDIO_STRETCH_MIN_TARGET_FILL)) {
		need = M_AUDIO_STRETCH_MIN_TARGET_FILL;
	}
	if (need > cap) {
		need = cap;
	}
	return (int) need;
}

int mAudioTimeStretchWrite(struct mAudioTimeStretch* ts, const int16_t* frames, int numFrames) {
	int64_t w;
	int64_t floor;
	int64_t space;
	int i;

	M_ATOMIC_LOAD64(w, ts->writePos);
	M_ATOMIC_LOAD64(floor, ts->consumerFloor);
	space = M_AUDIO_STRETCH_INPUT_CAPACITY - (w - floor);

	if (numFrames > M_AUDIO_STRETCH_MAX_WRITE) {
		numFrames = M_AUDIO_STRETCH_MAX_WRITE;
	}
	if (space < 0) {
		space = 0;
	}
	if (numFrames > space) {
		numFrames = (int) space;
	}
	if (numFrames <= 0) {
		return 0;
	}

	for (i = 0; i < numFrames; ++i) {
		int idx = (int) ((w + i) & IN_MASK);
		int16_t l = frames[(i * 2) + 0];
		int16_t r = frames[(i * 2) + 1];
		ts->inL[idx] = l;
		ts->inR[idx] = r;
		ts->inMono[idx] = 0.5f * ((float) l + (float) r);
	}

	// Release: frames must be visible before writePos advertises them.
	M_ATOMIC_STORE64(ts->writePos, w + numFrames);
	return numFrames;
}

void mAudioTimeStretchBeginSession(struct mAudioTimeStretch* ts) {
	ATOMIC_ADD(ts->generation, 1);
}

// CONSUMER. naturalPos trails below the search floor at a large hop.
static void _publishFloor(struct mAudioTimeStretch* ts) {
	int64_t floorNow = _min64(ts->analysisPos - M_AUDIO_STRETCH_SEARCH_RADIUS, ts->naturalPos);
	if (floorNow < 0) {
		floorNow = 0;
	}
	M_ATOMIC_STORE64(ts->consumerFloor, floorNow);
}

/* CONSUMER. Resync to live data on a discontinuity. Never below the floor
 * already published plus the search radius: the producer may have sized a
 * write against that floor, so the frames under it can be gone. Where the
 * consumer had run past the live edge this holds position instead of moving
 * back, and the next Write brings it forward again. */
static void _resyncToLive(struct mAudioTimeStretch* ts) {
	int64_t lowest = ts->consumerFloor + M_AUDIO_STRETCH_SEARCH_RADIUS;
	ts->analysisPos = _max64(lowest, _max64(0, ts->seenWrite - M_AUDIO_STRETCH_FRAME_SIZE));
	ts->naturalPos = ts->analysisPos;
	ts->primed = false;
	_publishFloor(ts);
	ts->outReadPos = 0;
	ts->outWritePos = 0;
	memset(ts->accL, 0, sizeof(ts->accL));
	memset(ts->accR, 0, sizeof(ts->accR));
}

static bool _canSynthesise(const struct mAudioTimeStretch* ts) {
	int64_t frameEnd;
	int64_t naturalEnd;
	if ((M_AUDIO_STRETCH_OUTPUT_CAPACITY - mAudioTimeStretchOutputFill(ts))
	    < M_AUDIO_STRETCH_SYNTHESIS_HOP) {
		return false;
	}
	// Only the frame and natural reference, or a short FIFO emits nothing.
	frameEnd = ts->analysisPos + M_AUDIO_STRETCH_FRAME_SIZE;
	naturalEnd = ts->naturalPos + M_AUDIO_STRETCH_SYNTHESIS_HOP;
	return (ts->seenWrite >= frameEnd) && (ts->seenWrite >= naturalEnd);
}

/* Normalised so the search doesn't just latch onto the loudest candidate. `ref`
 * is the natural-continuation window copied out linearly, so the masked lookup
 * is paid once per search rather than once per candidate. */
static double _score(const struct mAudioTimeStretch* ts, int64_t pos, const double* ref,
                     double refEnergy) {
	double dot = 0.0;
	double energy = 0.0;
	int i;
	for (i = 0; i < M_AUDIO_STRETCH_SYNTHESIS_HOP; ++i) {
		double a = ts->inMono[(int) ((pos + i) & IN_MASK)];
		dot += a * ref[i];
		energy += a * a;
	}
	return dot / sqrt((energy * refEnergy) + 1.0e-9);
}

static int64_t _findBestOffset(const struct mAudioTimeStretch* ts) {
	int64_t oldest = _max64(0, ts->seenWrite - M_AUDIO_STRETCH_INPUT_CAPACITY);
	int64_t latest;
	double ref[M_AUDIO_STRETCH_SYNTHESIS_HOP];
	double refEnergy = 0.0;
	double bestScore = -1.0e30;
	int lowestK = -M_AUDIO_STRETCH_SEARCH_RADIUS;
	int highestK = M_AUDIO_STRETCH_SEARCH_RADIUS;
	int bestK;
	int lo;
	int hi;
	int i;
	int k;

	// naturalPos can fall off the back of the ring at a high ratio.
	if (ts->naturalPos < oldest) {
		return ts->analysisPos;
	}

	for (i = 0; i < M_AUDIO_STRETCH_SYNTHESIS_HOP; ++i) {
		double v = ts->inMono[(int) ((ts->naturalPos + i) & IN_MASK)];
		ref[i] = v;
		refEnergy += v * v;
	}

	// Masking a negative position wraps it into frames we never wrote.
	if ((ts->analysisPos + lowestK) < oldest) {
		lowestK = (int) (oldest - ts->analysisPos);
	}

	// Clamp to what has arrived, so a short FIFO narrows the search.
	latest = ts->seenWrite - M_AUDIO_STRETCH_FRAME_SIZE;
	if ((ts->analysisPos + highestK) > latest) {
		highestK = (int) (latest - ts->analysisPos);
	}
	if (highestK < lowestK) {
		highestK = lowestK;
	}

	bestK = lowestK > 0 ? lowestK : 0;
	if (bestK > highestK) {
		bestK = highestK;
	}

	for (k = lowestK; k <= highestK; k += M_AUDIO_STRETCH_COARSE_STRIDE) {
		double s = _score(ts, ts->analysisPos + k, ref, refEnergy);
		if (s > bestScore) {
			bestScore = s;
			bestK = k;
		}
	}

	lo = bestK - M_AUDIO_STRETCH_FINE_RADIUS;
	hi = bestK + M_AUDIO_STRETCH_FINE_RADIUS;
	if (lo < lowestK) {
		lo = lowestK;
	}
	if (hi > highestK) {
		hi = highestK;
	}
	for (k = lo; k <= hi; ++k) {
		double s = _score(ts, ts->analysisPos + k, ref, refEnergy);
		if (s > bestScore) {
			bestScore = s;
			bestK = k;
		}
	}

	return ts->analysisPos + bestK;
}

static void _synthesiseHop(struct mAudioTimeStretch* ts, double ratio, bool emit) {
	int hop = (int) lround(M_AUDIO_STRETCH_SYNTHESIS_HOP * ratio);
	int64_t chosen;
	int i;

	if (hop < 1) {
		hop = 1;
	}

	chosen = ts->primed ? _findBestOffset(ts) : ts->analysisPos;
	ts->primed = true;

	for (i = 0; i < M_AUDIO_STRETCH_FRAME_SIZE; ++i) {
		int idx = (int) ((chosen + i) & IN_MASK);
		float w = ts->window[i];
		ts->accL[i] += w * (float) ts->inL[idx];
		ts->accR[i] += w * (float) ts->inR[idx];
	}

	if (emit) {
		for (i = 0; i < M_AUDIO_STRETCH_SYNTHESIS_HOP; ++i) {
			int idx = (int) (ts->outWritePos & OUT_MASK);
			ts->outL[idx] = _saturate(ts->accL[i]);
			ts->outR[idx] = _saturate(ts->accR[i]);
			++ts->outWritePos;
		}
	}

	memmove(ts->accL, ts->accL + M_AUDIO_STRETCH_SYNTHESIS_HOP,
	        M_AUDIO_STRETCH_SYNTHESIS_HOP * sizeof(float));
	memmove(ts->accR, ts->accR + M_AUDIO_STRETCH_SYNTHESIS_HOP,
	        M_AUDIO_STRETCH_SYNTHESIS_HOP * sizeof(float));
	memset(ts->accL + M_AUDIO_STRETCH_SYNTHESIS_HOP, 0,
	       M_AUDIO_STRETCH_SYNTHESIS_HOP * sizeof(float));
	memset(ts->accR + M_AUDIO_STRETCH_SYNTHESIS_HOP, 0,
	       M_AUDIO_STRETCH_SYNTHESIS_HOP * sizeof(float));

	// By hop alone: advancing from chosen would hide consumption from the caller.
	ts->naturalPos = chosen + M_AUDIO_STRETCH_SYNTHESIS_HOP;
	ts->analysisPos += hop;
}

int mAudioTimeStretchRead(struct mAudioTimeStretch* ts, int16_t* frames, int numFrames,
                          double ratio) {
	int64_t floor;
	int n;
	int i;

	// Before writePos: the reverse order can consume a new session and never resync.
	uint32_t gen;
	ATOMIC_LOAD(gen, ts->generation);
	M_ATOMIC_LOAD64(ts->seenWrite, ts->writePos);

	if (gen != ts->seenGeneration) {
		ts->seenGeneration = gen;
		_resyncToLive(ts);
	}

	// Producer overwrote frames we wanted: skip forward rather than read over them.
	floor = (ts->seenWrite - M_AUDIO_STRETCH_INPUT_CAPACITY)
	        + M_AUDIO_STRETCH_SEARCH_RADIUS + M_AUDIO_STRETCH_FRAME_SIZE;
	if (ts->analysisPos < floor) {
		ts->analysisPos = floor;
		ts->naturalPos = floor;
		ts->primed = false;
	}

	/* Overlap-add reaches full amplitude only once the accumulator carries a
	 * whole window, so the first hop after a reposition would ramp up from
	 * silence. Run one hop to charge the accumulator and drop its output. */
	if (!ts->primed && _canSynthesise(ts)) {
		_synthesiseHop(ts, ratio, false);
	}

	while ((mAudioTimeStretchOutputFill(ts) < numFrames) && _canSynthesise(ts)) {
		_synthesiseHop(ts, ratio, true);
	}

	n = mAudioTimeStretchOutputFill(ts);
	if (n > numFrames) {
		n = numFrames;
	}
	for (i = 0; i < n; ++i) {
		int idx = (int) (ts->outReadPos & OUT_MASK);
		frames[(i * 2) + 0] = ts->outL[idx];
		frames[(i * 2) + 1] = ts->outR[idx];
		++ts->outReadPos;
	}

	_publishFloor(ts);
	return n;
}
