/* Copyright (c) 2026 Davey Hughes
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef M_AUDIO_STREAM_RAMP_H
#define M_AUDIO_STREAM_RAMP_H

#include <mgba-util/common.h>

CXX_GUARD_START

/* Packet-loss concealment for the ends of a stream: ending repeats the last
 * periodic stretch under a raised cosine rather than cutting to silence or
 * holding a sample, and resuming ramps back up. Interleaved stereo s16, owned
 * by the audio thread. */

/* All in frames at the output rate. */
#define M_AUDIO_RAMP_HIST 2048
#define M_AUDIO_RAMP_TAIL 512
#define M_AUDIO_RAMP_MIN_PERIOD 96
#define M_AUDIO_RAMP_MAX_PERIOD 1024
#define M_AUDIO_RAMP_CORR 128
#define M_AUDIO_RAMP_JOIN 128
#define M_AUDIO_RAMP_MUTE 512
#define M_AUDIO_RAMP_IN 512

struct mAudioStreamRamp {
	int16_t hist[M_AUDIO_RAMP_HIST * 2];
	unsigned histPos;  /* next write slot */
	unsigned histFill;

	int16_t lastOut[2];

	/* Frozen at End(), so the tail is not read back as its own history. */
	int16_t tailSrc[M_AUDIO_RAMP_HIST * 2];
	unsigned tailSrcPos;
	unsigned tailSrcFill;

	unsigned tailFrames;   /* still to emit; 0 = no tail */
	unsigned tailPeriod;   /* 0 = DC ramp */
	int32_t tailJoin[2];   /* lastOut minus the repeat's first frame: twice the s16 range */

	unsigned muteFrames;   /* silence still to emit after the tail */
	unsigned fadeInFrames; /* resume ramp still to apply */
	bool rampInLatched;    /* a resume is owed, waiting for real audio to land on */
};

void mAudioStreamRampInit(struct mAudioStreamRamp*);
void mAudioStreamRampReset(struct mAudioStreamRamp*);

// True while the stream is down: a tail or its trailing mute is still owed.
bool mAudioStreamRampEnded(const struct mAudioStreamRamp*);

// A no-op if nothing was playing or the stream is already down.
void mAudioStreamRampEnd(struct mAudioStreamRamp*);

// Tail, then mute, then zeros. Call only while Ended().
void mAudioStreamRampFillTail(struct mAudioStreamRamp*, int16_t* frames, int numFrames);

// The ramp is latched until Track sees a non-silent frame, so a playing tail
// or a quiet core cannot spend it before the first real frames arrive.
void mAudioStreamRampBegin(struct mAudioStreamRamp*);

// Applies any latched resume ramp in place, then records the frames as history
// for the next End(). Call only with real audio, never the tail.
void mAudioStreamRampTrack(struct mAudioStreamRamp*, int16_t* frames, int numFrames);

CXX_GUARD_END

#endif
