/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "sdl-audio.h"

#include <mgba/core/core.h>
#include <mgba/core/thread.h>

mLOG_DEFINE_CATEGORY(SDL_AUDIO, "SDL Audio", "platform.sdl.audio");

/* Depth, in device buffers, of filterBuffer. The filter needs headroom to
 * observe fast-forward arrival before mAudioSpeedFilterWrite absorbs it: at
 * depth 1 the resampler stops filling once full, pinning the measured rate at
 * 1.0. 16 covers Qt's 10x fast-forward UI. */
#define SDL_AUDIO_SPEED_FILTER_DEPTH 16

#if SDL_VERSION_ATLEAST(3, 0, 0)
static void _mSDLAudioCallback(void* context, SDL_AudioStream* stream, int additionalLen, int totalLen);
#else
static void _mSDLAudioCallback(void* context, Uint8* data, int len);
#endif

bool mSDLInitAudio(struct mSDLAudio* context, struct mCoreThread* threadContext) {
#if defined(_WIN32) && SDL_VERSION_ATLEAST(2, 0, 8) && !SDL_VERSION_ATLEAST(3, 0, 0)
	if (!getenv("SDL_AUDIODRIVER")) {
		_putenv_s("SDL_AUDIODRIVER", "directsound");
	}
#endif
	if (!SDL_OK(SDL_InitSubSystem(SDL_INIT_AUDIO))) {
		mLOG(SDL_AUDIO, ERROR, "Could not initialize SDL sound system: %s", SDL_GetError());
		return false;
	}

	memset(&context->desiredSpec, 0, sizeof(context->desiredSpec));
	memset(&context->obtainedSpec, 0, sizeof(context->obtainedSpec));
	context->desiredSpec.freq = context->sampleRate;
	context->desiredSpec.channels = 2;
#if SDL_VERSION_ATLEAST(3, 0, 0)
	context->desiredSpec.format = SDL_AUDIO_S16;
	char hint[21];
	snprintf(hint, sizeof(hint), "%" PRIz "u", context->samples);
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, hint);
	context->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &context->desiredSpec, _mSDLAudioCallback, context);
	if (!context->stream) {
#else
	context->desiredSpec.callback = _mSDLAudioCallback;
	context->desiredSpec.userdata = context;
	context->desiredSpec.samples = context->samples;
	context->desiredSpec.format = AUDIO_S16SYS;
#if SDL_VERSION_ATLEAST(2, 0, 0)
	context->deviceId = SDL_OpenAudioDevice(0, 0, &context->desiredSpec, &context->obtainedSpec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if (context->deviceId == 0) {
#else
	if (SDL_OpenAudio(&context->desiredSpec, &context->obtainedSpec) < 0) {
#endif
#endif
		mLOG(SDL_AUDIO, ERROR, "Could not open SDL sound system");
		return false;
	}
	context->core = NULL;

#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_GetAudioStreamFormat(context->stream, &context->obtainedSpec, NULL);
#endif
	if (context->obtainedSpec.channels == 0) {
		// This should never happen, but let's make sure
#if SDL_VERSION_ATLEAST(3, 0, 0)
		SDL_DestroyAudioStream(context->stream);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
		SDL_PauseAudioDevice(context->deviceId, 1);
		SDL_CloseAudioDevice(context->deviceId);
#else
		SDL_PauseAudio(1);
		SDL_CloseAudio();
#endif
		return false;
	}

	mAudioResamplerInit(&context->resampler, mINTERPOLATOR_SINC);
	if (context->obtainedSpec.channels == 2) {
		context->filterReady = mAudioSpeedFilterInit(&context->speedFilter, context->obtainedSpec.freq);
	} else {
		// mAudioSpeedFilter and speedFilterBuffer are hardcoded to 2 channels.
		context->filterReady = false;
		mLOG(SDL_AUDIO, WARN, "Speed filter unavailable: device has %u channels, not 2", context->obtainedSpec.channels);
	}

	if (threadContext) {
		context->core = threadContext->core;
		context->sync = &threadContext->impl->sync;

		// Defaults on, matching Qt; see mAudioSpeedFilterInit for the latency cost.
		int filterEnabled = 1;
		int lowPass = M_AUDIO_LOW_PASS_DEFAULT;
		mCoreConfigGetIntValue(&context->core->config, "audioSpeedFilter", &filterEnabled);
		mCoreConfigGetIntValue(&context->core->config, "audioSpeedLowPass", &lowPass);
		if (context->filterReady) {
			mAudioSpeedFilterSetEnabled(&context->speedFilter, filterEnabled != 0);
			mAudioSpeedFilterSetLowPass(&context->speedFilter, lowPass);
		}
	}
	/* buffer stays exactly `samples` whether filtering or not, so its
	 * mAudioBufferFull backpressure still pins the core to device rate. */
	mAudioBufferInit(&context->buffer, context->samples, context->obtainedSpec.channels);
	if (context->filterReady) {
		mAudioBufferInit(&context->filterBuffer, context->samples * SDL_AUDIO_SPEED_FILTER_DEPTH, context->obtainedSpec.channels);
	}
	mAudioResamplerSetDestination(&context->resampler, &context->buffer, context->obtainedSpec.freq);

	// Not while the core is paused: the frontend resumes on its next unpause.
	if (threadContext && !mCoreThreadIsPaused(threadContext)) {
		mSDLResumeAudio(context);
	}

	return true;
}

void mSDLDeinitAudio(struct mSDLAudio* context) {
	UNUSED(context);
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_DestroyAudioStream(context->stream);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_PauseAudioDevice(context->deviceId, 1);
	SDL_CloseAudioDevice(context->deviceId);
#else
	SDL_PauseAudio(1);
	SDL_CloseAudio();
#endif
	mAudioBufferDeinit(&context->buffer);
	mAudioBufferDeinit(&context->filterBuffer);
	mAudioResamplerDeinit(&context->resampler);
	mAudioSpeedFilterDeinit(&context->speedFilter);
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

static void _sdlPauseDevice(struct mSDLAudio* context) {
	context->devicePlaying = false;
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_PauseAudioStreamDevice(context->stream);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_PauseAudioDevice(context->deviceId, 1);
#else
	UNUSED(context);
	SDL_PauseAudio(1);
#endif
}

void mSDLPauseAudio(struct mSDLAudio* context) {
	/* Conceal the stop before killing the callback: arm the tail, then let the
	 * still-running callback play it out before the device is paused, or it
	 * cuts mid-waveform. The tail and its mute fit in one callback, so four
	 * periods is ample: about 185 ms at the 2048-frame, 44.1 kHz default,
	 * and the caller only blocks that long if the callback has stalled. */
	if (context->devicePlaying && context->filterReady && mAudioSpeedFilterEnabled(&context->speedFilter)) {
		int rate = context->obtainedSpec.freq > 0 ? context->obtainedSpec.freq : 48000;
		int bound = (int) ((4000 * context->samples) / (size_t) rate);
		int waited = 0;
		mSDLLockAudio(context);
		mAudioSpeedFilterStreamEnd(&context->speedFilter);
		mSDLUnlockAudio(context);
		while (waited < bound) {
			bool ended;
			mSDLLockAudio(context);
			ended = mAudioSpeedFilterStreamEnded(&context->speedFilter);
			mSDLUnlockAudio(context);
			if (!ended) {
				break;
			}
			SDL_Delay(2);
			waited += 2;
		}
	}
	_sdlPauseDevice(context);
}

bool mSDLAudioJumpBegin(struct mSDLAudio* context) {
	bool ramped = false;
	if (context->filterReady && mAudioSpeedFilterEnabled(&context->speedFilter)) {
		mSDLLockAudio(context);
		ramped = mAudioSpeedFilterJumpBegin(&context->speedFilter);
		mSDLUnlockAudio(context);
	}
	return ramped;
}

void mSDLAudioJumpEnd(struct mSDLAudio* context, bool ramped) {
	if (context->filterReady && mAudioSpeedFilterEnabled(&context->speedFilter)) {
		mSDLLockAudio(context);
		mAudioSpeedFilterJumpEnd(&context->speedFilter, ramped);
		mSDLUnlockAudio(context);
	}
}

void mSDLResumeAudio(struct mSDLAudio* context) {
	context->devicePlaying = true;
	if (context->filterReady && mAudioSpeedFilterEnabled(&context->speedFilter)) {
		mSDLLockAudio(context);
		mAudioSpeedFilterStreamBegin(&context->speedFilter);
		mSDLUnlockAudio(context);
	}
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_ResumeAudioStreamDevice(context->stream);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_PauseAudioDevice(context->deviceId, 0);
#else
	SDL_PauseAudio(0);
#endif
}

/* Blocks the callback until mSDLUnlockAudio, so another thread can safely
 * touch context->speedFilter while a callback could be in flight. */
void mSDLLockAudio(struct mSDLAudio* context) {
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_LockAudioStream(context->stream);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_LockAudioDevice(context->deviceId);
#else
	UNUSED(context);
	SDL_LockAudio();
#endif
}

void mSDLUnlockAudio(struct mSDLAudio* context) {
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_UnlockAudioStream(context->stream);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_UnlockAudioDevice(context->deviceId);
#else
	UNUSED(context);
	SDL_UnlockAudio();
#endif
}

#if SDL_VERSION_ATLEAST(3, 0, 0)
static void _mSDLAudioCallback(void* context, SDL_AudioStream* stream, int additionalLen, int len) {
	UNUSED(additionalLen);
#else
static void _mSDLAudioCallback(void* context, Uint8* data, int len) {
#endif
	struct mSDLAudio* audioContext = context;
	if (!context || !audioContext->core) {
#if SDL_VERSION_ATLEAST(3, 0, 0)
		return;
#else
		memset(data, 0, len);
		return;
#endif
	}
	struct mAudioBuffer* buffer = audioContext->core->getAudioBuffer(audioContext->core);
	unsigned sampleRate = audioContext->core->audioSampleRate(audioContext->core);
	bool filtering = audioContext->filterReady && mAudioSpeedFilterEnabled(&audioContext->speedFilter);
	double fauxClock = 1;
	double highWaterClock = 1;
	if (audioContext->sync) {
		/* Two separate clocks: skewing fauxClock would flatten the arrival
		 * rate the filter measures speed from, while pinning highWaterClock
		 * would cap how far mCoreSyncProduceAudio lets the core run ahead. */
		if (!filtering && audioContext->sync->fpsTarget > 0 && audioContext->core) {
			fauxClock = mCoreCalculateFramerateRatio(audioContext->core, audioContext->sync->fpsTarget);
		}
		if (audioContext->sync->fpsTarget > 0 && audioContext->core) {
			highWaterClock = mCoreCalculateFramerateRatio(audioContext->core, audioContext->sync->fpsTarget);
		}
		mCoreSyncLockAudio(audioContext->sync);
		audioContext->sync->audioHighWater = audioContext->samples + audioContext->resampler.highWaterMark + audioContext->resampler.lowWaterMark + (audioContext->samples >> 6);
		audioContext->sync->audioHighWater *= sampleRate / (highWaterClock * audioContext->obtainedSpec.freq);
	}
	// Chosen fresh every call, so the runtime toggle needs no re-init.
	mAudioResamplerSetDestination(&audioContext->resampler, filtering ? &audioContext->filterBuffer : &audioContext->buffer, audioContext->obtainedSpec.freq);
	mAudioResamplerSetSource(&audioContext->resampler, buffer, sampleRate / fauxClock, true);
	mAudioResamplerProcess(&audioContext->resampler);
	if (audioContext->sync) {
		mCoreSyncConsumeAudio(audioContext->sync);
	}

	if (filtering) {
		/* A short accept means the ring saturated. The frames already pulled
		 * out of filterBuffer that didn't fit are lost, not retried: stop
		 * rather than compound the loss against an already-full ring. */
		int pending = mAudioBufferAvailable(&audioContext->filterBuffer);
		while (pending > 0) {
			int chunk = pending;
			if (chunk > M_AUDIO_STRETCH_MAX_WRITE) {
				chunk = M_AUDIO_STRETCH_MAX_WRITE;
			}
			chunk = mAudioBufferRead(&audioContext->filterBuffer, audioContext->speedFilterBuffer, chunk);
			if (chunk <= 0) {
				break;
			}
			int accepted = mAudioSpeedFilterWrite(&audioContext->speedFilter, audioContext->speedFilterBuffer, chunk);
			pending -= chunk;
			if (accepted < chunk) {
				break;
			}
		}
	}
#if SDL_VERSION_ATLEAST(3, 0, 0)
	int channels = (int) audioContext->obtainedSpec.channels;
	// speedFilterBuffer is sized for M_AUDIO_STRETCH_MAX_WRITE stereo frames.
	int maxChunk = (int) (sizeof(audioContext->speedFilterBuffer) / sizeof(int16_t)) / channels;
	int totalFrames = len / (int) (sizeof(int16_t) * (size_t) channels);
	if (maxChunk > M_AUDIO_STRETCH_MAX_WRITE) {
		maxChunk = M_AUDIO_STRETCH_MAX_WRITE;
	}
	while (totalFrames > 0) {
		int frameChunk = totalFrames;
		if (frameChunk > maxChunk) {
			frameChunk = maxChunk;
		}
		int got;
		if (filtering) {
			got = mAudioSpeedFilterRead(&audioContext->speedFilter, audioContext->speedFilterBuffer, frameChunk);
		} else {
			got = mAudioBufferRead(&audioContext->buffer, audioContext->speedFilterBuffer, frameChunk);
		}
		if (got <= 0) {
			break;
		}
		SDL_PutAudioStreamData(stream, audioContext->speedFilterBuffer, got * (int) (sizeof(int16_t) * (size_t) channels));
		totalFrames -= got;
	}
#else
	len /= 2 * audioContext->obtainedSpec.channels;
	if (filtering) {
		// Chunked like the SDL 3 path above; filtering implies 2 channels.
		int16_t* out = (int16_t*) data;
		while (len > 0) {
			int frameChunk = len;
			if (frameChunk > M_AUDIO_STRETCH_MAX_WRITE) {
				frameChunk = M_AUDIO_STRETCH_MAX_WRITE;
			}
			mAudioSpeedFilterRead(&audioContext->speedFilter, out, frameChunk);
			out += frameChunk * 2;
			len -= frameChunk;
		}
	} else {
		int available = mAudioBufferRead(&audioContext->buffer, (int16_t*) data, len);
		if (available < len) {
			memset(((short*) data) + audioContext->obtainedSpec.channels * available, 0, (len - available) * audioContext->obtainedSpec.channels * sizeof(short));
		}
	}
#endif
}
