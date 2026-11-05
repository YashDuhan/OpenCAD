/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIPROVIDER_H
#define LC_AIPROVIDER_H

#include <QList>
#include <QString>
#include <QStringList>

#include "lc_aigenerated.h"

/**
 * Registry of AI providers the assistant can talk to (ported from the Sorvex/Cypher harness:
 * packages/ai/src/providers). Almost every provider speaks one of three wire formats.
 */
struct LC_AIProvider {
    enum class Api {
        OpenAIChat,        // POST {base}/chat/completions, Authorization: Bearer
        AnthropicMessages, // POST {base}/v1/messages, x-api-key + anthropic-version
        GoogleGenerativeAI // POST {base}/models/{model}:streamGenerateContent?alt=sse, x-goog-api-key
    };

    QString id;
    QString name;
    Api api {Api::OpenAIChat};
    QString baseUrl;
    QString envVar;           // environment variable checked when no key is saved
    QString defaultModel;
    QStringList models;       // suggestions; any model id can be typed
    int maxTokens {8192};     // output cap sent where the API requires one (Anthropic Messages)
    bool keyOptional {false}; // local servers (Ollama, LM Studio) need no key
    bool customBaseUrl {false};

    static const QList<LC_AIProvider>& all();
    static const LC_AIProvider* find(const QString& id);
    /** Models of a provider in the catalog copied from Cypher (lc_aigenerated.cpp). */
    static QList<const LC_AIGenerated::ModelEntry*> catalog(const QString& providerId);
    /** compactNumber() from Cypher's format.ts: 8192 -> "8.2k", 200000 -> "200k", 1000000 -> "1.0M". */
    static QString compactNumber(qint64 value);
};

#endif
