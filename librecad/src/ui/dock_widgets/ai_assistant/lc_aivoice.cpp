/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aivoice.h"

#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QtEndian>
#include <cmath>

#include "lc_aicredentials.h"
#include "lc_aiprovider.h"

namespace {
    const QString SETTING = QStringLiteral("voice");
    const QString OPENAI_BASE = QStringLiteral("https://api.openai.com/v1");
    const QString GEMINI_BASE = QStringLiteral("https://generativelanguage.googleapis.com/v1beta");
    constexpr int TRANSFER_TIMEOUT_MS = 90000; // a long dictation uploads a few MB and the model then listens to all of it

    QString trimSlash(QString url) {
        while (url.endsWith('/')) {
            url.chop(1);
        }
        return url;
    }

    LC_AIVoice::Endpoint endpoint(const LC_AIVoice::Endpoint::Kind kind, const QString& baseUrl, const QString& model,
                                  const QString& keyProvider) {
        LC_AIVoice::Endpoint e;
        e.kind = kind;
        e.baseUrl = baseUrl;
        e.model = model;
        e.keyProvider = keyProvider;
        return e;
    }

    /** The error's message ({"error": {"message": ...}} from OpenAI, Groq and Gemini), else the start of the body. */
    QString snippet(const QByteArray& body) {
        const QJsonValue error = QJsonDocument::fromJson(body).object().value("error");
        QString text = error.isObject() ? error.toObject().value("message").toString() : error.toString();
        if (text.trimmed().isEmpty()) {
            text = QString::fromUtf8(body);
        }
        text = text.simplified();
        return text.size() > 300 ? text.left(297) + "..." : text;
    }

    void writeText(QByteArray& out, const int offset, const char* text) {
        for (int i = 0; text[i] != '\0'; ++i) {
            out[offset + i] = text[i];
        }
    }

    template <typename T>
    void writeLe(QByteArray& out, const int offset, const T value) {
        qToLittleEndian(value, out.data() + offset);
    }
}

// ------------------------------------------------------------------------------------------------ settings

QJsonObject LC_AIVoice::Endpoint::toJson() const {
    QJsonObject json{{"kind", kind == Kind::Gemini ? "gemini" : "openai"}, {"baseUrl", baseUrl}, {"model", model},
                     {"keyProvider", keyProvider}};
    if (!language.isEmpty()) {
        json.insert("language", language);
    }
    return json;
}

LC_AIVoice::Endpoint LC_AIVoice::Endpoint::fromJson(const QJsonObject& json) {
    Endpoint e;
    e.kind = json.value("kind").toString() == "gemini" ? Kind::Gemini : Kind::OpenAI;
    e.baseUrl = json.value("baseUrl").toString();
    e.model = json.value("model").toString();
    e.keyProvider = json.value("keyProvider").toString();
    e.language = json.value("language").toString();
    return e;
}

QList<LC_AIVoice::Preset> LC_AIVoice::presets() {
    using Kind = Endpoint::Kind;
    return {
        {"Gemini", endpoint(Kind::Gemini, GEMINI_BASE, "gemini-2.5-flash", "google")},
        {"OpenAI", endpoint(Kind::OpenAI, OPENAI_BASE, "gpt-4o-mini-transcribe", "openai")},
        {"Groq", endpoint(Kind::OpenAI, "https://api.groq.com/openai/v1", "whisper-large-v3-turbo", "groq")},
        {"Local whisper.cpp", endpoint(Kind::OpenAI, "http://127.0.0.1:8080/v1", "whisper-1", QString())},
    };
}

LC_AIVoice::Settings LC_AIVoice::settings() {
    const QJsonObject json = LC_AICredentials::instance().setting(SETTING).toObject();
    Settings s;
    s.autoSend = json.value("autoSend").toBool(false);
    if (json.value("stt").isObject()) {
        s.endpoint = Endpoint::fromJson(json.value("stt").toObject());
        return s;
    }
    // nothing chosen yet: the first preset that already has its key, so the mic works without a visit to the menu
    const QList<Preset> all = presets();
    s.endpoint = all.first().endpoint;
    for (const Preset& p : all) {
        if (!p.endpoint.keyProvider.isEmpty() && ready(p.endpoint)) {
            s.endpoint = p.endpoint;
            break;
        }
    }
    return s;
}

void LC_AIVoice::setSettings(const Settings& settings) {
    LC_AICredentials::instance().setSetting(SETTING, QJsonObject{{"stt", settings.endpoint.toJson()}, {"autoSend", settings.autoSend}});
}

QString LC_AIVoice::keyProviderOf(const Endpoint& endpoint) {
    if (endpoint.kind == Endpoint::Kind::Gemini && endpoint.keyProvider.isEmpty()) {
        return QStringLiteral("google");
    }
    return endpoint.keyProvider;
}

bool LC_AIVoice::ready(const Endpoint& endpoint, QString* why) {
    if (endpoint.model.trimmed().isEmpty()) {
        if (why != nullptr) {
            *why = QObject::tr("Choose a speech model in the voice menu.");
        }
        return false;
    }
    const QString provider = keyProviderOf(endpoint);
    if (!provider.isEmpty() && LC_AICredentials::instance().apiKey(provider).isEmpty()) {
        if (why != nullptr) {
            const LC_AIProvider* p = LC_AIProvider::find(provider);
            *why = QObject::tr("Add a %1 key in Providers to transcribe speech.").arg(p != nullptr ? p->name : provider);
        }
        return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ audio

LC_AIVoice::Resampler::Resampler(const int inputRate)
    : m_ratio(inputRate > 0 ? double(inputRate) / SAMPLE_RATE : 1.0) {
    m_boundary = qint64(std::floor(m_ratio));
}

void LC_AIVoice::Resampler::emitSample(const float value) {
    const float v = qBound(-1.0f, value, 1.0f);
    const qint16 sample = qToLittleEndian(qint16(qRound(v < 0 ? v * 32768.0f : v * 32767.0f)));
    m_pcm.append(reinterpret_cast<const char*>(&sample), 2); // grows geometrically: a long dictation stays cheap
}

void LC_AIVoice::Resampler::push(const float* samples, const qsizetype count) {
    for (qsizetype i = 0; i < count; ++i) {
        ++m_inputCount;
        if (m_ratio < 1.0) { // a slower microphone: repeat each sample for the output samples it covers
            while (m_outputCount * m_ratio < m_inputCount) {
                emitSample(samples[i]);
                ++m_outputCount;
            }
            continue;
        }
        m_sum += samples[i];
        ++m_summed;
        if (m_inputCount >= m_boundary) {
            emitSample(float(m_sum / qMax(1, m_summed)));
            m_sum = 0;
            m_summed = 0;
            ++m_outputCount;
            m_boundary = qint64(std::floor((m_outputCount + 1) * m_ratio));
        }
    }
}

QByteArray LC_AIVoice::wavFromPcm(const QByteArray& pcm, const int sampleRate) {
    QByteArray wav(44, '\0');
    writeText(wav, 0, "RIFF");
    writeLe<quint32>(wav, 4, quint32(36 + pcm.size()));
    writeText(wav, 8, "WAVE");
    writeText(wav, 12, "fmt ");
    writeLe<quint32>(wav, 16, 16);                       // size of the fmt chunk
    writeLe<quint16>(wav, 20, 1);                        // PCM
    writeLe<quint16>(wav, 22, 1);                        // mono
    writeLe<quint32>(wav, 24, quint32(sampleRate));
    writeLe<quint32>(wav, 28, quint32(sampleRate * 2));  // bytes per second
    writeLe<quint16>(wav, 32, 2);                        // bytes per frame
    writeLe<quint16>(wav, 34, 16);                       // bits per sample
    writeText(wav, 36, "data");
    writeLe<quint32>(wav, 40, quint32(pcm.size()));
    return wav + pcm;
}

QByteArray LC_AIVoice::encodeWav(const QList<float>& samples, const int sampleRate) {
    Resampler resampler(sampleRate);
    resampler.push(samples.constData(), samples.size());
    return wavFromPcm(resampler.pcm());
}

// ------------------------------------------------------------------------------------------------ transcription

QNetworkReply* LC_AIVoice::transcribe(QNetworkAccessManager* network, const Endpoint& endpoint, const QByteArray& wav,
                                      QObject* context, const std::function<void(const QString&, const QString&)>& done) {
    const QString provider = keyProviderOf(endpoint);
    const QByteArray key = provider.isEmpty() ? QByteArray() : LC_AICredentials::instance().apiKey(provider).toUtf8();
    const bool gemini = endpoint.kind == Endpoint::Kind::Gemini;

    QNetworkRequest request;
    request.setTransferTimeout(TRANSFER_TIMEOUT_MS);
    // HTTP/1.1, as for the chat requests: Qt then tries IPv6 and IPv4 side by side
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    QNetworkReply* reply = nullptr;
    if (gemini) {
        const QString base = trimSlash(endpoint.baseUrl.isEmpty() ? GEMINI_BASE : endpoint.baseUrl);
        request.setUrl(QUrl(base + "/models/" + QString::fromUtf8(QUrl::toPercentEncoding(endpoint.model)) + ":generateContent"));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("x-goog-api-key", key);
        const QJsonObject body{
            {"contents", QJsonArray{QJsonObject{{"parts", QJsonArray{
                QJsonObject{{"text", "Transcribe the speech in this audio verbatim. Put only the spoken words in `transcript`. "
                                     "If there is no intelligible speech, return an empty transcript."}},
                QJsonObject{{"inline_data", QJsonObject{{"mime_type", "audio/wav"}, {"data", QString::fromLatin1(wav.toBase64())}}}}}}}}},
            // a JSON schema keeps commentary such as "I cannot hear anything" out of the transcript
            {"generationConfig", QJsonObject{
                {"temperature", 0},
                {"responseMimeType", "application/json"},
                {"responseSchema", QJsonObject{{"type", "OBJECT"},
                                               {"properties", QJsonObject{{"transcript", QJsonObject{{"type", "STRING"}}}}},
                                               {"required", QJsonArray{"transcript"}}}}}}};
        reply = network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    }
    else {
        auto* form = new QHttpMultiPart(QHttpMultiPart::FormDataType);
        const auto field = [form](const QString& name, const QByteArray& value) {
            QHttpPart part;
            part.setHeader(QNetworkRequest::ContentDispositionHeader, QStringLiteral("form-data; name=\"%1\"").arg(name));
            part.setBody(value);
            form->append(part);
        };
        QHttpPart file;
        file.setHeader(QNetworkRequest::ContentDispositionHeader, QStringLiteral("form-data; name=\"file\"; filename=\"audio.wav\""));
        file.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("audio/wav"));
        file.setBody(wav);
        form->append(file);
        field("model", endpoint.model.toUtf8());
        field("response_format", "json");
        if (!endpoint.language.isEmpty()) {
            field("language", endpoint.language.toUtf8());
        }
        const QString base = trimSlash(endpoint.baseUrl.isEmpty() ? OPENAI_BASE : endpoint.baseUrl);
        request.setUrl(QUrl(base + "/audio/transcriptions"));
        if (!key.isEmpty()) {
            request.setRawHeader("Authorization", "Bearer " + key);
        }
        reply = network->post(request, form);
        form->setParent(reply);
    }

    QObject::connect(reply, &QNetworkReply::finished, context, [reply, gemini, done] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->readAll();
        const bool ok = status >= 200 && status < 300;
        if (status == 0 || (ok && reply->error() != QNetworkReply::NoError)) { // no answer, or cut off
            done({}, QObject::tr("Transcription failed: %1").arg(reply->errorString()));
            return;
        }
        if (!ok) {
            done({}, (gemini ? QObject::tr("Gemini transcription failed (%1): %2") : QObject::tr("Transcription failed (%1): %2")).arg(status).arg(snippet(data)));
            return;
        }
        const QJsonObject json = QJsonDocument::fromJson(data).object();
        if (!gemini) {
            done(json.value("text").toString().trimmed(), {});
            return;
        }
        QString raw;
        const QJsonArray candidates = json.value("candidates").toArray();
        if (!candidates.isEmpty()) {
            for (const QJsonValue& part : candidates.first().toObject().value("content").toObject().value("parts").toArray()) {
                raw += part.toObject().value("text").toString();
            }
        }
        // anything but the asked-for JSON counts as no speech, as in Cypher
        done(QJsonDocument::fromJson(raw.toUtf8()).object().value("transcript").toString().trimmed(), {});
    });
    return reply;
}
