/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "AudioProcessorSDL.h"
#include "moc_AudioProcessorSDL.cpp"

#include "LogController.h"

#include <mgba/core/core.h>
#include <mgba/core/thread.h>

using namespace QGBA;

AudioProcessorSDL::AudioProcessorSDL(QObject* parent)
	: AudioProcessor(parent)
{
}

void AudioProcessorSDL::setInput(std::shared_ptr<CoreController> controller) {
	AudioProcessor::setInput(std::move(controller));
	if (m_audio.core && input()->core != m_audio.core) {
		// As in setBufferSamples(): conceal the tear-down before it happens. The
		// outgoing core's audio ends here either way; it should not end on a cut.
		mSDLPauseAudio(&m_audio);
		mSDLDeinitAudio(&m_audio);
		mSDLInitAudio(&m_audio, input());
	}
}

void AudioProcessorSDL::stop() {
	mSDLDeinitAudio(&m_audio);
	AudioProcessor::stop();
}

bool AudioProcessorSDL::start() {
	if (!input()) {
		LOG(QT, WARN) << tr("Can't start an audio processor without input");
		return false;
	}

	if (m_audio.core) {
		mSDLResumeAudio(&m_audio);
		return true;
	} else {
		if (!m_audio.samples) {
			m_audio.samples = 2048; // TODO?
		}
		return mSDLInitAudio(&m_audio, input());
	}
}

void AudioProcessorSDL::pause() {
	mSDLPauseAudio(&m_audio);
}

void AudioProcessorSDL::jumpBegin() {
	m_jumpRamped = mSDLAudioJumpBegin(&m_audio);
}

void AudioProcessorSDL::jumpEnd() {
	mSDLAudioJumpEnd(&m_audio, m_jumpRamped);
}

void AudioProcessorSDL::setBufferSamples(int samples) {
	AudioProcessor::setBufferSamples(samples);
	if (m_audio.samples != static_cast<size_t>(samples)) {
		m_audio.samples = samples;
		if (m_audio.core) {
			// Or the stream is destroyed mid-waveform, with no tail over the cut.
			mSDLPauseAudio(&m_audio);
			mSDLDeinitAudio(&m_audio);
			mSDLInitAudio(&m_audio, input());
		}
	}
}

void AudioProcessorSDL::inputParametersChanged() {
	/* Toggling these changes neither the buffer size nor the sample rate, so
	 * nothing else would re-read them until the next mSDLInitAudio(). */
	if (!m_audio.core || !m_audio.filterReady) {
		return;
	}
	int enabled = 1;
	int lowPass = M_AUDIO_LOW_PASS_DEFAULT;
	mCoreConfigGetIntValue(&m_audio.core->config, "audioSpeedFilter", &enabled);
	mCoreConfigGetIntValue(&m_audio.core->config, "audioSpeedLowPass", &lowPass);
	// _mSDLAudioCallback touches speedFilter on SDL's own audio thread.
	mSDLLockAudio(&m_audio);
	mAudioSpeedFilterSetEnabled(&m_audio.speedFilter, enabled != 0);
	mAudioSpeedFilterSetLowPass(&m_audio.speedFilter, lowPass);
	mSDLUnlockAudio(&m_audio);
}

void AudioProcessorSDL::configure(ConfigController* config) {
	AudioProcessor::configure(config);
	inputParametersChanged();
}

void AudioProcessorSDL::requestSampleRate(unsigned rate) {
	if (m_audio.sampleRate != rate) {
		m_audio.sampleRate = rate;
		if (m_audio.core) {
			// As in setBufferSamples(): conceal the tear-down before it happens.
			mSDLPauseAudio(&m_audio);
			mSDLDeinitAudio(&m_audio);
			mSDLInitAudio(&m_audio, input());
		}
	}
}

unsigned AudioProcessorSDL::sampleRate() const {
	if (m_audio.core) {
		return m_audio.obtainedSpec.freq;
	} else {
		return 0;
	}
}
