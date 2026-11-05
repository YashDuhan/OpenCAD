/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIVOICERECORDER_H
#define LC_AIVOICERECORDER_H

#include <QByteArray>
#include <QList>
#include <QObject>
#include <memory>

#ifdef LC_AI_VOICE
#include <QAudioFormat>
#endif

/**
 * Records the default microphone for the AI composer, like Cypher's Mic in "dictation" mode: whatever format
 * the device gives is turned into 16 kHz mono 16-bit PCM while it arrives (LC_AIVoice::Resampler), and stop()
 * returns it as a WAV. Needs Qt Multimedia: built without it (no LC_AI_VOICE), available() is false and
 * start() only reports that voice input is missing.
 */
class LC_AIVoiceRecorder : public QObject {
    Q_OBJECT
public:
    /** Built with Qt Multimedia. */
    static bool available();
    /** Name of the microphone a recording would use, empty when there is none. */
    static QString deviceName();

    explicit LC_AIVoiceRecorder(QObject* parent = nullptr);
    ~LC_AIVoiceRecorder() override;

    /** Opens the default microphone: started() or failed() follows (later, when the system first asks for permission). */
    void start();
    /** Stops and returns the recording as a 16 kHz mono 16-bit WAV (empty when nothing was recorded). */
    QByteArray stop();
    /** Stops and drops the recording. */
    void cancel();
    bool isRecording() const;
    /** Length of the recording so far. */
    qreal seconds() const;

#ifdef LC_AI_VOICE
    /** Interleaved frames in any sample format and channel count, averaged to mono samples in [-1, 1]. */
    static QList<float> toMono(const QByteArray& frames, const QAudioFormat& format);
#endif

signals:
    void started();
    /** Loudness of the latest chunk (RMS, 0..1), many times a second while recording. */
    void level(qreal rms);
    void failed(const QString& error);

private:
    void read();
    void release();

    struct Private;
    std::unique_ptr<Private> d;
};

#endif
