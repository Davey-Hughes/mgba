/* Copyright (c) 2026 Davey Hughes
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef M_AUDIO_TIME_STRETCH_H
#define M_AUDIO_TIME_STRETCH_H

#include <mgba-util/common.h>

CXX_GUARD_START

/* Tuned against pumping at ratio 3; below 256 is under one bass cycle. */
#define M_AUDIO_STRETCH_FRAME_SIZE 256
#define M_AUDIO_STRETCH_SYNTHESIS_HOP (M_AUDIO_STRETCH_FRAME_SIZE / 2)
#define M_AUDIO_STRETCH_SEARCH_RADIUS 1024
#define M_AUDIO_STRETCH_COARSE_STRIDE 4
#define M_AUDIO_STRETCH_FINE_RADIUS 3
#define M_AUDIO_STRETCH_INPUT_CAPACITY 32768
#define M_AUDIO_STRETCH_OUTPUT_CAPACITY 8192
#define M_AUDIO_STRETCH_MIN_TARGET_FILL 4096
// Per-call cap only; nothing bounds how many Writes land between two Reads.
#define M_AUDIO_STRETCH_MAX_WRITE 4096

struct mAudioTimeStretch {
	float window[M_AUDIO_STRETCH_FRAME_SIZE];

	int16_t* inL;
	int16_t* inR;
	float* inMono;

	int64_t writePos;      /* producer writes, consumer reads */
	uint32_t generation;   /* written by either side */
	int64_t consumerFloor; /* consumer writes, producer reads */

	int64_t seenWrite;     /* consumer's snapshot of writePos */
	uint32_t seenGeneration;
	int64_t analysisPos;
	int64_t naturalPos;
	bool primed;

	float accL[M_AUDIO_STRETCH_FRAME_SIZE];
	float accR[M_AUDIO_STRETCH_FRAME_SIZE];

	int16_t* outL;
	int16_t* outR;
	int64_t outReadPos;
	int64_t outWritePos;
};

/* Lock-free for one producer and one consumer: Write and BeginSession on the
 * producer side, Read and the fill queries on the consumer side. Both frontends
 * happen to drive it from the audio thread alone, which is also allowed. */
bool mAudioTimeStretchInit(struct mAudioTimeStretch*);
void mAudioTimeStretchDeinit(struct mAudioTimeStretch*);

// PRODUCER. Returns frames accepted; a short accept means the ring saturated.
int mAudioTimeStretchWrite(struct mAudioTimeStretch*, const int16_t* frames, int numFrames);

void mAudioTimeStretchBeginSession(struct mAudioTimeStretch*);

// CONSUMER. Capped at M_AUDIO_STRETCH_OUTPUT_CAPACITY regardless of numFrames.
int mAudioTimeStretchRead(struct mAudioTimeStretch*, int16_t* frames, int numFrames, double ratio);

// Not const: the MSVC 64-bit atomic load needs a non-const LONG64*.
int mAudioTimeStretchInputFill(struct mAudioTimeStretch*);
int mAudioTimeStretchOutputFill(const struct mAudioTimeStretch*);
int64_t mAudioTimeStretchTotalWritten(struct mAudioTimeStretch*);

// Floors at M_AUDIO_STRETCH_MIN_TARGET_FILL: ~85 ms of latency at 48 kHz.
int mAudioTimeStretchTargetInputFill(double arrivalPerCallback);

CXX_GUARD_END

#endif
