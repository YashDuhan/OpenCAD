/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aivoicerecorder.h"

#include <optional>

#include "lc_aivoice.h"

#ifndef LC_AI_VOICE

// built without Qt Multimedia: the composer shows no mic button, and nothing can record

struct LC_AIVoiceRecorder::Private {};

bool LC_AIVoiceRecorder::available() {
    return false;
}

QString LC_AIVoiceRecorder::deviceName() {
    return {};
}

LC_AIVoiceRecorder::LC_AIVoiceRecorder(QObject* parent)
    : QObject(parent), d(std::make_unique<Private>()) {
}

LC_AIVoiceRecorder::~LC_AIVoiceRecorder() = default;

void LC_AIVoiceRecorder::start() {
    emit failed(tr("Voice input needs Qt Multimedia, and this OpenCAD was built without it."));
}

QByteArray LC_AIVoiceRecorder::stop() {
    return {};
}

void LC_AIVoiceRecorder::cancel() {
}

bool LC_AIVoiceRecorder::isRecording() const {
    return false;
}

qreal LC_AIVoiceRecorder::seconds() const {
    return 0;
}

void LC_AIVoiceRecorder::read() {
}

void LC_AIVoiceRecorder::release() {
}

#else

#include <QAudioDevice>
#include <QAudioSource>
#include <QCoreApplication>
#include <QMediaDevices>
#include <QPointer>
#include <cmath>
#if QT_CONFIG(permissions)
#include <QPermissions>
#endif

struct LC_AIVoiceRecorder::Private {
    QPointer<QAudioSource> source;
    QPointer<QIODevice> io;
    QAudioFormat format;
    std::optional<LC_AIVoice::Resampler> resampler;
    QByteArray partial;          // the start of a frame that arrived without its end
    bool recording {false};
    bool awaitingPermission {false};
};

bool LC_AIVoiceRecorder::available() {
    return true;
}

QString LC_AIVoiceRecorder::deviceName() {
    return QMediaDevices::defaultAudioInput().description();
}

LC_AIVoiceRecorder::LC_AIVoiceRecorder(QObject* parent)
    : QObject(parent), d(std::make_unique<Private>()) {
}

LC_AIVoiceRecorder::~LC_AIVoiceRecorder() {
    release();
}

void LC_AIVoiceRecorder::start() {
    if (d->recording || d->awaitingPermission) {
        return;
    }
#if QT_CONFIG(permissions)
    // macOS (and sandboxed builds) ask the user once; elsewhere this is granted at once
    const QMicrophonePermission permission;
    switch (qApp->checkPermission(permission)) {
        case Qt::PermissionStatus::Undetermined:
            d->awaitingPermission = true;
            qApp->requestPermission(permission, this, [this](const QPermission& answer) {
                if (!d->awaitingPermission) {
                    return; // cancelled while the system asked
                }
                d->awaitingPermission = false;
                if (answer.status() == Qt::PermissionStatus::Granted) {
                    start();
                }
                else {
                    emit failed(tr("OpenCAD may not use the microphone. Allow it in the system's privacy settings."));
                }
            });
            return;
        case Qt::PermissionStatus::Denied:
            emit failed(tr("OpenCAD may not use the microphone. Allow it in the system's privacy settings."));
            return;
        case Qt::PermissionStatus::Granted:
            break;
    }
#endif
    const QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (device.isNull()) {
        emit failed(tr("No microphone was found."));
        return;
    }
    // 16 kHz mono 16-bit straight from the device when it can; otherwise its own format, converted as it arrives
    QAudioFormat format;
    format.setSampleRate(LC_AIVoice::SAMPLE_RATE);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    if (!device.isFormatSupported(format)) {
        format = device.preferredFormat();
    }
    d->format = format;
    d->resampler.emplace(format.sampleRate());
    d->partial.clear();
    d->source = new QAudioSource(device, format, this);
    connect(d->source, &QAudioSource::stateChanged, this, [this](const QAudio::State state) {
        // the device went away (unplugged headset) or the sound server stopped
        if (state == QAudio::StoppedState && d->recording && d->source != nullptr && d->source->error() != QAudio::NoError) {
            release();
            emit failed(tr("The microphone stopped working."));
        }
    });
    d->io = d->source->start();
    if (d->io == nullptr || d->source->error() != QAudio::NoError) {
        release();
        emit failed(tr("The microphone %1 could not be opened.").arg(device.description()));
        return;
    }
    connect(d->io, &QIODevice::readyRead, this, &LC_AIVoiceRecorder::read);
    d->recording = true;
    emit started();
}

QList<float> LC_AIVoiceRecorder::toMono(const QByteArray& frames, const QAudioFormat& format) {
    const int frameBytes = format.bytesPerFrame();
    const int sampleBytes = format.bytesPerSample();
    const int channels = format.channelCount();
    QList<float> mono;
    if (frameBytes <= 0 || channels <= 0) {
        return mono;
    }
    const qsizetype count = frames.size() / frameBytes;
    mono.reserve(count);
    const char* data = frames.constData();
    for (qsizetype i = 0; i < count; ++i) {
        const char* frame = data + i * frameBytes;
        float sum = 0;
        for (int c = 0; c < channels; ++c) {
            sum += format.normalizedSampleValue(frame + c * sampleBytes);
        }
        mono.append(sum / channels);
    }
    return mono;
}

void LC_AIVoiceRecorder::read() {
    if (d->io == nullptr || !d->resampler) {
        return;
    }
    d->partial += d->io->readAll();
    const int frameBytes = qMax(1, d->format.bytesPerFrame());
    const qsizetype whole = d->partial.size() - d->partial.size() % frameBytes;
    if (whole <= 0) {
        return;
    }
    const QList<float> mono = toMono(d->partial.left(whole), d->format);
    d->partial.remove(0, whole);
    d->resampler->push(mono.constData(), mono.size());
    double energy = 0;
    for (const float v : mono) {
        energy += double(v) * v;
    }
    emit level(mono.isEmpty() ? 0.0 : std::sqrt(energy / mono.size()));
}

QByteArray LC_AIVoiceRecorder::stop() {
    if (!d->recording) {
        cancel();
        return {};
    }
    read(); // what is still buffered
    const QByteArray pcm = d->resampler ? d->resampler->pcm() : QByteArray();
    release();
    return pcm.isEmpty() ? QByteArray() : LC_AIVoice::wavFromPcm(pcm);
}

void LC_AIVoiceRecorder::cancel() {
    d->awaitingPermission = false;
    release();
}

void LC_AIVoiceRecorder::release() {
    d->recording = false;
    if (d->source != nullptr) {
        d->source->disconnect(this);
        if (d->io != nullptr) {
            d->io->disconnect(this);
        }
        d->source->stop();
        d->source->deleteLater(); // may be inside its own stateChanged
    }
    d->source = nullptr;
    d->io = nullptr;
    d->resampler.reset();
    d->partial.clear();
}

bool LC_AIVoiceRecorder::isRecording() const {
    return d->recording;
}

qreal LC_AIVoiceRecorder::seconds() const {
    return d->resampler ? d->resampler->seconds() : 0.0;
}

#endif
