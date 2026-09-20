/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "AudioDevice.h"
#include "moc_AudioDevice.cpp"

#include "GBAApp.h"
#include "LogController.h"

#include <mgba/core/core.h>
#include <mgba/core/thread.h>
#include <mgba/internal/gba/audio.h>

#include <QDebug>
#include <QMutexLocker>

#include <cstring>

using namespace QGBA;

AudioDevice::AudioDevice(QObject* parent)
	: QIODevice(parent)
	, m_context(nullptr)
{
	setOpenMode(ReadOnly);
	mAudioBufferInit(&m_buffer, 0x4000, 2);
	mAudioResamplerInit(&m_resampler, mINTERPOLATOR_SINC);
	m_filterReady = mAudioSpeedFilterInit(&m_speedFilter, 48000.0);
	m_filterEnabledCache = mAudioSpeedFilterEnabled(&m_speedFilter);
	m_updateTimer.setSingleShot(false);
	m_updateTimer.setInterval(1);
	connect(&m_updateTimer, &QTimer::timeout, this, &AudioDevice::update);
}

AudioDevice::~AudioDevice() {
	mAudioResamplerDeinit(&m_resampler);
	mAudioSpeedFilterDeinit(&m_speedFilter);
	mAudioBufferDeinit(&m_buffer);
}

void AudioDevice::streamEnd() {
	if (!m_filterReady) {
		return;
	}
	// Serialise against readData() on the audio thread, as setFormat() does.
	QMutexLocker filterLock(&m_filterMutex);
	if (mAudioSpeedFilterEnabled(&m_speedFilter)) {
		mAudioSpeedFilterStreamEnd(&m_speedFilter);
	}
}

void AudioDevice::streamBegin() {
	if (!m_filterReady) {
		return;
	}
	QMutexLocker filterLock(&m_filterMutex);
	if (mAudioSpeedFilterEnabled(&m_speedFilter)) {
		mAudioSpeedFilterStreamBegin(&m_speedFilter);
	}
}

bool AudioDevice::streamFilterActive() const {
	return m_filterReady && m_filterEnabledCache.load(std::memory_order_relaxed);
}

bool AudioDevice::jumpBegin() {
	if (!m_filterReady) {
		return false;
	}
	QMutexLocker filterLock(&m_filterMutex);
	if (!mAudioSpeedFilterEnabled(&m_speedFilter)) {
		return false;
	}
	return mAudioSpeedFilterJumpBegin(&m_speedFilter);
}

void AudioDevice::jumpEnd(bool ramped) {
	if (!m_filterReady) {
		return;
	}
	QMutexLocker filterLock(&m_filterMutex);
	mAudioSpeedFilterJumpEnd(&m_speedFilter, ramped);
}

void AudioDevice::setFormat(const QAudioFormat& format) {
	if (!m_context) {
		LOG(QT, INFO) << tr("Can't set format of context-less audio device");
		return;
	}
	if (m_filterReady && m_context->core) {
		/* Applied even with the core thread inactive, or a toggle while paused is
		 * dropped. Taken before the sync lock rather than nested inside it, so this
		 * thread never blocks on a WSOLA pass while holding the emulation lock. */
		QMutexLocker filterLock(&m_filterMutex);
		double wasRate = m_speedFilter.sampleRate;
		bool wasEnabled = mAudioSpeedFilterEnabled(&m_speedFilter);
		mAudioSpeedFilterSetSampleRate(&m_speedFilter, format.sampleRate());
		int enabled = 1;
		int lowPass = M_AUDIO_LOW_PASS_DEFAULT;
		mCoreConfigGetIntValue(&m_context->core->config, "audioSpeedFilter", &enabled);
		mCoreConfigGetIntValue(&m_context->core->config, "audioSpeedLowPass", &lowPass);
		mAudioSpeedFilterSetEnabled(&m_speedFilter, enabled != 0);
		// Not `enabled`: SetEnabled stays off if it can't reallocate its rings.
		m_filterEnabledCache = mAudioSpeedFilterEnabled(&m_speedFilter);
		mAudioSpeedFilterSetLowPass(&m_speedFilter, lowPass);
		// Only on a real reconfigure: this also runs on every fast-forward toggle,
		// where dropping staged output skips at the transition it exists to smooth.
		if (m_speedFilter.sampleRate != wasRate || mAudioSpeedFilterEnabled(&m_speedFilter) != wasEnabled) {
			// readData() refills the staging buffer on its next pass.
			m_filterOutputPos = 0;
			m_filterOutputLen = 0;
		}
	}

	if (!mCoreThreadIsActive(m_context)) {
		LOG(QT, INFO) << tr("Can't set format of inactive audio device");
		return;
	}

	mCoreSyncLockAudio(&m_context->impl->sync);
	mCore* core = m_context->core;
	mAudioResamplerSetSource(&m_resampler, core->getAudioBuffer(core), core->audioSampleRate(core), true);
	m_format = format;
	adjustResampler();
	mCoreSyncUnlockAudio(&m_context->impl->sync);
}

void AudioDevice::setBufferSamples(int samples) {
	m_samples = samples;
}

void AudioDevice::setInput(mCoreThread* input) {
	m_context = input;
}

qint64 AudioDevice::readData(char* data, qint64 maxSize) {
	if (!m_context->core) {
		LOG(QT, WARN) << tr("Audio device is missing its core");
		return 0;
	}

	if (!maxSize) {
		return 0;
	}

	mCoreSyncLockAudio(&m_context->impl->sync);
	mAudioResamplerSetSource(&m_resampler, m_context->core->getAudioBuffer(m_context->core), m_context->core->audioSampleRate(m_context->core), true);
	mAudioResamplerProcess(&m_resampler);
	mCoreSyncConsumeAudio(&m_context->impl->sync);
	if (mAudioBufferAvailable(&m_buffer) < 32) {
		// Audio is running slow...let's wait a tiny bit for more to come in
		QThread::usleep(100);
		mCoreSyncLockAudio(&m_context->impl->sync);
		mAudioResamplerProcess(&m_resampler);
		mCoreSyncConsumeAudio(&m_context->impl->sync);
	}
	quint64 wanted = std::min<quint64>({
		static_cast<quint64>(maxSize / sizeof(mStereoSample)),
		std::numeric_limits<int>::max()
	});

	if (m_filterReady) {
		// Held across the whole pass, so a setFormat() reinit can't land mid-block.
		QMutexLocker filterLock(&m_filterMutex);
		if (mAudioSpeedFilterEnabled(&m_speedFilter)) {
			/* Refilled only once it runs dry: draining m_buffer on every call
			 * would outpace the core and defeat its backpressure. */
			quint64 produced = 0;
			size_t quantum = std::min<size_t>(m_samples ? m_samples : 1, M_AUDIO_STRETCH_OUTPUT_CAPACITY);
			while (produced < wanted) {
				if (m_filterOutputPos >= m_filterOutputLen) {
					int pending = mAudioBufferAvailable(&m_buffer);
					while (pending > 0) {
						int chunk = std::min(pending, M_AUDIO_STRETCH_MAX_WRITE);
						chunk = mAudioBufferRead(&m_buffer, m_filterBuffer, chunk);
						if (chunk <= 0) {
							break;
						}
						int accepted = mAudioSpeedFilterWrite(&m_speedFilter, m_filterBuffer, chunk);
						pending -= chunk;
						if (accepted < chunk) {
							// Ring saturated; see sdl-audio.c's callback.
							break;
						}
					}
					mAudioSpeedFilterRead(&m_speedFilter, m_filterOutput, quantum);
					m_filterOutputPos = 0;
					m_filterOutputLen = quantum;
				}
				quint64 take = std::min<quint64>(m_filterOutputLen - m_filterOutputPos, wanted - produced);
				memcpy(data + produced * sizeof(mStereoSample), &m_filterOutput[m_filterOutputPos * 2], take * sizeof(mStereoSample));
				m_filterOutputPos += take;
				produced += take;
			}
			filterLock.unlock();
			m_updateTimer.start();
			return produced * sizeof(mStereoSample);
		}
	}

	quint64 available = std::min<quint64>({
		mAudioBufferAvailable(&m_buffer),
		wanted
	});
	mAudioBufferRead(&m_buffer, reinterpret_cast<int16_t*>(data), available);
	m_updateTimer.start();
	return available * sizeof(mStereoSample);
}

qint64 AudioDevice::writeData(const char*, qint64) {
	LOG(QT, WARN) << tr("Writing data to read-only audio device");
	return 0;
}

bool AudioDevice::atEnd() const {
	return false;
}

qint64 AudioDevice::bytesAvailable() const {
	if (!m_context->core) {
		return true;
	}
	m_updateTimer.start();
	int available = mAudioBufferAvailable(&m_buffer);
	return available * sizeof(mStereoSample);
}

qint64 AudioDevice::bytesAvailable() {
	if (!m_context->core) {
		return true;
	}
	mCoreSyncLockAudio(&m_context->impl->sync);
	adjustResampler();
	mAudioResamplerProcess(&m_resampler);
	int available = mAudioBufferAvailable(&m_buffer);
	mCoreSyncUnlockAudio(&m_context->impl->sync);
	m_updateTimer.start();
	return available * sizeof(mStereoSample);
}

void AudioDevice::update() {
	if (!m_context->core) {
		return;
	}

	bool wasAvailable = mAudioBufferAvailable(&m_buffer);

	mCoreSyncLockAudio(&m_context->impl->sync);
	mAudioResamplerProcess(&m_resampler);
	mCoreSyncConsumeAudio(&m_context->impl->sync);

	if (!wasAvailable && mAudioBufferAvailable(&m_buffer)) {
		emit readyRead();
	}

	if (mAudioBufferFull(&m_buffer)) {
		m_updateTimer.stop();
	}
}

void AudioDevice::adjustResampler() {
	mCore* core = m_context->core;
	// Two separate clocks; same split as sdl-audio.c's _mSDLAudioCallback.
	double fauxClock = 1;
	/* m_filterEnabledCache, since every caller here already holds
	 * mCoreSyncLockAudio and must not also take m_filterMutex. */
	if (!(m_filterReady && m_filterEnabledCache.load(std::memory_order_relaxed))) {
		fauxClock = mCoreCalculateFramerateRatio(m_context->core, m_context->impl->sync.fpsTarget);
	}
	mAudioResamplerSetDestination(&m_resampler, &m_buffer, m_format.sampleRate() * fauxClock);

	double highWaterClock = mCoreCalculateFramerateRatio(m_context->core, m_context->impl->sync.fpsTarget);
	m_context->impl->sync.audioHighWater = m_samples + m_resampler.highWaterMark + m_resampler.lowWaterMark;
	m_context->impl->sync.audioHighWater *= core->audioSampleRate(core) / (m_format.sampleRate() * highWaterClock);
}
