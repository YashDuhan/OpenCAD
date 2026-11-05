/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aiprovider.h"

namespace {
    using Api = LC_AIProvider::Api;

    LC_AIProvider make(const QString& id, const QString& name, const Api api, const QString& baseUrl,
                       const QString& envVar, const QStringList& models, const int maxTokens = 8192) {
        LC_AIProvider p;
        p.id = id;
        p.name = name;
        p.api = api;
        p.baseUrl = baseUrl;
        p.envVar = envVar;
        p.defaultModel = models.isEmpty() ? QString() : models.first();
        p.models = models;
        p.maxTokens = maxTokens;
        return p;
    }

    QList<LC_AIProvider> buildProviders() {
        QList<LC_AIProvider> list;
        list << make("anthropic", "Anthropic", Api::AnthropicMessages, "https://api.anthropic.com", "ANTHROPIC_API_KEY",
                     {"claude-opus-5-5", "claude-sonnet-5-5", "claude-haiku-5-5", "claude-fable-5-1"}, 64000);
        list << make("openai", "OpenAI", Api::OpenAIChat, "https://api.openai.com/v1", "OPENAI_API_KEY",
                     {"gpt-5.5", "gpt-5.4"});
        list << make("google", "Google Gemini", Api::GoogleGenerativeAI, "https://generativelanguage.googleapis.com/v1beta",
                     "GEMINI_API_KEY", {"gemini-3.1-pro-preview"});
        list << make("openrouter", "OpenRouter", Api::OpenAIChat, "https://openrouter.ai/api/v1", "OPENROUTER_API_KEY",
                     {"moonshotai/kimi-k2.6", "anthropic/claude-opus-5-5", "openai/gpt-5.5"});
        list << make("xai", "xAI", Api::OpenAIChat, "https://api.x.ai/v1", "XAI_API_KEY", {"grok-4.7"});
        list << make("deepseek", "DeepSeek", Api::OpenAIChat, "https://api.deepseek.com", "DEEPSEEK_API_KEY",
                     {"deepseek-v4-pro"});
        list << make("mistral", "Mistral", Api::OpenAIChat, "https://api.mistral.ai/v1", "MISTRAL_API_KEY",
                     {"devstral-medium-latest", "mistral-large-latest"});
        list << make("groq", "Groq", Api::OpenAIChat, "https://api.groq.com/openai/v1", "GROQ_API_KEY",
                     {"openai/gpt-oss-120b"});
        list << make("cerebras", "Cerebras", Api::OpenAIChat, "https://api.cerebras.ai/v1", "CEREBRAS_API_KEY",
                     {"gpt-oss-120b"});
        list << make("together", "Together AI", Api::OpenAIChat, "https://api.together.ai/v1", "TOGETHER_API_KEY",
                     {"moonshotai/Kimi-K3"});
        list << make("fireworks", "Fireworks", Api::OpenAIChat, "https://api.fireworks.ai/inference/v1", "FIREWORKS_API_KEY",
                     {"accounts/fireworks/models/kimi-k3"});
        list << make("nvidia", "NVIDIA NIM", Api::OpenAIChat, "https://integrate.api.nvidia.com/v1", "NVIDIA_API_KEY",
                     {"nvidia/nemotron-3.5-super-vl-120b-a12b"});
        list << make("huggingface", "Hugging Face", Api::OpenAIChat, "https://router.huggingface.co/v1", "HF_TOKEN",
                     {"moonshotai/Kimi-K2.6"});
        list << make("moonshotai", "Moonshot AI", Api::OpenAIChat, "https://api.moonshot.ai/v1", "MOONSHOT_API_KEY",
                     {"kimi-k2.6"});
        list << make("zai", "Z.AI", Api::OpenAIChat, "https://api.z.ai/api/coding/paas/v4", "ZAI_API_KEY", {"glm-5.3"});
        list << make("minimax", "MiniMax", Api::AnthropicMessages, "https://api.minimax.io/anthropic", "MINIMAX_API_KEY",
                     {"MiniMax-M2.7"});
        list << make("vercel-ai-gateway", "Vercel AI Gateway", Api::AnthropicMessages, "https://ai-gateway.vercel.sh",
                     "AI_GATEWAY_API_KEY", {"zai/glm-5.1"});

        // Any OpenAI-compatible endpoint: Ollama (http://localhost:11434/v1), LM Studio (http://localhost:1234/v1), vLLM, ...
        LC_AIProvider custom = make("custom", "Custom (OpenAI-compatible)", Api::OpenAIChat, "http://localhost:11434/v1",
                                    "OPENAI_COMPATIBLE_API_KEY", {});
        custom.keyOptional = true;
        custom.customBaseUrl = true;
        list << custom;
        return list;
    }
}

const QList<LC_AIProvider>& LC_AIProvider::all() {
    static const QList<LC_AIProvider> providers = buildProviders();
    return providers;
}

const LC_AIProvider* LC_AIProvider::find(const QString& id) {
    for (const LC_AIProvider& p : all()) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

QList<const LC_AIGenerated::ModelEntry*> LC_AIProvider::catalog(const QString& providerId) {
    QList<const LC_AIGenerated::ModelEntry*> models;
    const QByteArray id = providerId.toUtf8();
    for (int i = 0; i < LC_AIGenerated::MODEL_COUNT; ++i) {
        if (id == LC_AIGenerated::MODELS[i].provider) {
            models << &LC_AIGenerated::MODELS[i];
        }
    }
    return models;
}

QString LC_AIProvider::compactNumber(const qint64 value) {
    if (value < 1000) {
        return QString::number(value);
    }
    if (value < 1000000) {
        return QString::number(value / 1000.0, 'f', value < 10000 ? 1 : 0) + "k";
    }
    return QString::number(value / 1000000.0, 'f', 1) + "M";
}
