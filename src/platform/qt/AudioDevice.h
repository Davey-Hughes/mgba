/* Copyright (c) 2013-2014 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QAudioFormat>
#include <QIODevice>
#include <QMutex>
#include <QTimer>

#include <atomic>

#include <mgba-util/audio-buffer.h>
#include <mgba-util/audio-resampler.h>
#include <mgba-util/audio-speed-filter.h>

struct mCoreThread;

namespace QGBA {

class AudioDevice : public QIODevice {
Q_OBJECT

public:
	AudioDevice(QObject* parent = nullptr);
	virtual ~AudioDevice();

	void setInput(mCoreThread* input);
	void setFormat(const QAudioFormat& format);
	void setBufferSamples(int samples);
	void streamEnd();
	void streamBegin();
	bool streamFilterActive() const;
	bool jumpBegin();
	void jumpEnd(bool ramped);
	bool atEnd() const override;
	qint64 bytesAvailable() const override;
	qint64 bytesAvailable();
	bool isSequential() const override { return true; }

protected:
	virtual qint64 readData(char* data, qint64 maxSize) override;
	virtual qint64 writeData(const char* data, qint64 maxSize) override;

private slots:
	void update();

private:
	size_t m_samples = 512;
	QAudioFormat m_format;
	mCoreThread* m_context;
	mAudioBuffer m_buffer;
	mAudioResampler m_resampler;
	mAudioSpeedFilter m_speedFilter;
	int16_t m_filterBuffer[M_AUDIO_STRETCH_MAX_WRITE * 2];
	// Staging buffer for readData(), refilled one quantum at a time.
	int16_t m_filterOutput[M_AUDIO_STRETCH_OUTPUT_CAPACITY * 2];
	size_t m_filterOutputPos = 0;
	size_t m_filterOutputLen = 0;
	bool m_filterReady = false;
	/* Guards m_speedFilter against readData() on the audio thread. Never nested
	 * inside the audio sync mutex, in either order. jumpBegin()/jumpEnd() do
	 * block the core thread on it for the length of one WSOLA pass. */
	QMutex m_filterMutex;
	/* Mirrors m_speedFilter's enabled flag for adjustResampler(), which holds
	 * mCoreSyncLockAudio and so can't take m_filterMutex. */
	std::atomic<bool> m_filterEnabledCache{false};
	mutable QTimer m_updateTimer;

	void adjustResampler();
};

}
