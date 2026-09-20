/* Copyright (c) 2013-2014 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef SDL_AUDIO_H
#define SDL_AUDIO_H

#include "sdl-common.h"

CXX_GUARD_START

#include <mgba/core/log.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/audio-resampler.h>
#include <mgba-util/audio-speed-filter.h>

mLOG_DECLARE_CATEGORY(SDL_AUDIO);

struct mSDLAudio {
	// Input
	size_t samples;
	unsigned sampleRate;

	// State
	struct mAudioBuffer buffer; // Resampler destination while not filtering; exactly `samples`
	struct mAudioBuffer filterBuffer; // Resampler destination while filtering
	struct mAudioResampler resampler;
	struct mAudioSpeedFilter speedFilter;
	bool filterReady;
	bool devicePlaying;
	int16_t speedFilterBuffer[M_AUDIO_STRETCH_MAX_WRITE * 2]; // Interleaved stereo
	SDL_AudioSpec desiredSpec;
	SDL_AudioSpec obtainedSpec;
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_AudioStream* stream;
#elif SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_AudioDeviceID deviceId;
#endif

	struct mCore* core;
	struct mCoreSync* sync;
};

struct mCoreThread;
bool mSDLInitAudio(struct mSDLAudio* context, struct mCoreThread*);
void mSDLDeinitAudio(struct mSDLAudio* context);
void mSDLPauseAudio(struct mSDLAudio* context);
void mSDLResumeAudio(struct mSDLAudio* context);

// Guards context->speedFilter against a concurrent write from another thread.
void mSDLLockAudio(struct mSDLAudio* context);
void mSDLUnlockAudio(struct mSDLAudio* context);

// Bracket a state load or reset with the concealment tail; JumpBegin returns
// whether it took the stream down, which JumpEnd needs to decide to ramp back.
bool mSDLAudioJumpBegin(struct mSDLAudio* context);
void mSDLAudioJumpEnd(struct mSDLAudio* context, bool ramped);

CXX_GUARD_END

#endif
