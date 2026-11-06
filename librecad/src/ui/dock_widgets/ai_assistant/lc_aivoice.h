/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIVOICE_H
#define LC_AIVOICE_H

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QObject;

/**
 * Speech to text for the AI composer, ported from Cypher (apps/desktop/src/voice.ts and server/voice.ts):
 * the microphone is recorded as 16 kHz mono 16-bit PCM, wrapped in a WAV header, and sent either to an
 * OpenAI-compatible /audio/transcriptions endpoint (OpenAI, Groq, a local whisper.cpp server) or to Gemini's
 * generateContent with a JSON schema that asks for the transcript. Nothing here needs Qt Multimedia: the
 * recording itself is LC_AIVoiceRecorder.
 */
namespace LC_AIVoice {
    /** 16 kHz mono 16-bit: accepted by OpenAI, Groq, whisper.cpp and Gemini alike, and small to upload. */
    constexpr int SAMPLE_RATE = 16000;

    /** A speech endpoint (Cypher's SpeechEndpoint). */
    struct Endpoint {
        enum class Kind { OpenAI, Gemini };
        Kind kind {Kind::Gemini};
        QString baseUrl;
        QString model;
        QString keyProvider; // provider whose stored key is sent ("openai", "groq", "google"); empty for none
        QString language;    // optional ISO-639-1 hint for OpenAI-compatible servers, e.g. "en"

        QJsonObject toJson() const;
        static Endpoint fromJson(const QJsonObject& json);
        bool sameServer(const Endpoint& other) const { return kind == other.kind && baseUrl == other.baseUrl; }
    };

    /** Cypher's speech-to-text presets: Gemini, OpenAI, Groq and a local whisper.cpp server. */
    struct Preset {
        QString name;
        Endpoint endpoint;
    };
    QList<Preset> presets();

    struct Settings {
        Endpoint endpoint;
        bool autoSend {false}; // send the message as soon as the transcript is in the box
    };
    /** Kept as "voice" in ai-auth.json. Until it is set, the first preset whose key is stored, else Gemini. */
    Settings settings();
    void setSettings(const Settings& settings);

    /** Can transcribe: a model is set and the key it needs is stored (or in the environment). */
    bool ready(const Endpoint& endpoint, QString* why = nullptr);
    /** The provider id that supplies the key: Gemini falls back to "google", as in Cypher. */
    QString keyProviderOf(const Endpoint& endpoint);
    /** "an OpenAI key", "a Groq key": the English article before a provider's name (translations have both texts). */
    bool startsWithVowel(const QString& name);

    /**
     * Turns microphone samples (mono, at the device's rate) into 16 kHz 16-bit PCM while they arrive.
     * Each output sample is the average of the input samples that fall into it, like Cypher's encodeWav
     * (a simple low-pass before the decimation); rates below 16 kHz repeat the nearest sample.
     */
    class Resampler {
    public:
        explicit Resampler(int inputRate);
        void push(const float* samples, qsizetype count);
        const QByteArray& pcm() const { return m_pcm; }
        qreal seconds() const { return m_pcm.size() / (2.0 * SAMPLE_RATE); }

    private:
        void emitSample(float value);

        double m_ratio;       // input samples per output sample
        qint64 m_inputCount {0};
        qint64 m_outputCount {0};
        qint64 m_boundary {0}; // first input sample of the next output sample
        double m_sum {0};
        int m_summed {0};
        QByteArray m_pcm;
    };

    /** Wraps raw 16-bit little-endian mono PCM in a WAV header (Cypher's pcmToWav). */
    QByteArray wavFromPcm(const QByteArray& pcm, int sampleRate = SAMPLE_RATE);
    /** Cypher's encodeWav: mono float samples in [-1, 1] at any rate to a 16 kHz mono 16-bit WAV. */
    QByteArray encodeWav(const QList<float>& samples, int sampleRate);

    /**
     * Cypher's transcribe(): posts the WAV to the endpoint and calls done with the trimmed transcript, or
     * with an error message. done runs on context's thread and never after context is gone. The returned
     * reply can be aborted (done then reports the abort as an error).
     */
    QNetworkReply* transcribe(QNetworkAccessManager* network, const Endpoint& endpoint, const QByteArray& wav,
                              QObject* context, const std::function<void(const QString& text, const QString& error)>& done);
}

#endif
