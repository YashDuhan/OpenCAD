/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIINFERENCEMANAGER_H
#define LC_AIINFERENCEMANAGER_H

#include <QJsonArray>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>

#include "lc_aiattachments.h"
#include "lc_aiprovider.h"
#include "lc_aitool.h"

#include <QNetworkReply>

class QNetworkAccessManager;
class QTimer;

struct LC_AIToolCall {
    QString id;
    QString name;
    QJsonObject arguments;
    QString argumentsError; // set when the model sent arguments that are not valid JSON
};

/**
 * One message of the conversation. Assistant messages may carry tool calls; each tool call is
 * answered by a ToolResult message. Provider-specific blocks (Anthropic thinking blocks, Gemini
 * thought signatures) are kept verbatim in `raw` and replayed to the same provider and model.
 */
struct LC_AIChatMessage {
    enum class Role { User, Assistant, ToolResult };
    Role role {Role::User};
    QString text;

    // User: files sent with the text, and the drawing context its @-mentions stood for when it was sent
    QList<LC_AIAttachment> attachments;
    QString context;

    // Assistant
    QList<LC_AIToolCall> toolCalls;
    QString origin;   // "provider/model" that produced it
    QJsonArray raw;   // Anthropic content blocks or Gemini parts, exactly as received

    // ToolResult
    QString toolCallId;
    QString toolName;
    LC_AIToolResult result;
};

/**
 * Agent loop over the selected provider: sends the conversation and the tool list, streams the
 * reply, runs the tools the model asks for, sends their results back, and repeats until the model
 * answers without tool calls (or MAX_TOOL_ROUNDS is reached). Provider, key, model and base URL
 * come from LC_AICredentials.
 */
class LC_AIInferenceManager : public QObject {
    Q_OBJECT
public:
    // only a safety net against a model stuck in a loop: like Cypher, a run goes on until the job is done or Stop
    static constexpr int MAX_TOOL_ROUNDS = 250;
    static constexpr int MAX_RETRIES = 3;            // per model turn, for failures that may pass
    static constexpr int CONNECT_TIMEOUT_MS = 30000; // no answer from the server at all: retry

    explicit LC_AIInferenceManager(QObject* parent = nullptr);

    void setTools(const QList<LC_AITool>& tools) { m_tools = tools; }
    /**
     * Asked before every tool call (Cypher's tool_call hook): answer with allowed, or not allowed and the
     * reason the model gets. The answer may come later (the user clicks in the approval box).
     */
    using Approver = std::function<void(const LC_AIToolCall& call, std::function<void(bool allowed, const QString& reason)> answer)>;
    void setApprover(Approver approver) { m_approver = std::move(approver); }
    /** Starts a run on the conversation; returns false (and emits failed) when it cannot be started. */
    bool send(const QList<LC_AIChatMessage>& history);
    void cancel();
    bool isBusy() const { return m_running; }

    static QString systemPrompt();
    /** Instructions after the system prompt for the runs from now on (plan mode, the debugger's pass); empty for none. */
    void setPromptAddition(const QString& text) { m_promptAddition = text; }
    /** The last run was stopped (Stop, New Chat) rather than ended by the model. */
    bool wasCancelled() const { return m_cancelled; }

    /** The result a tool call gets when the user stopped the run before it ran (or while it ran): every call is answered. */
    static bool isStoppedResult(const LC_AIChatMessage& message);
    /**
     * The conversation as the providers accept it: every tool call followed by one result. Calls left without one
     * (chats saved before stopped calls were answered) get a "stopped" result; results without their call are left out.
     */
    static QList<LC_AIChatMessage> paired(const QList<LC_AIChatMessage>& history);

signals:
    void delta(const QString& text);                         // assistant text as it streams
    void messageAdded(const LC_AIChatMessage& message);       // completed assistant or tool-result message
    void toolStarted(const LC_AIToolCall& call);
    /** A turn failed in a way that may pass (network, rate limit, overload); it is tried again after `seconds`. */
    void retrying(int attempt, int maxAttempts, int seconds, const QString& reason);
    void finished();
    void failed(const QString& error);

private:
    struct Target {
        const LC_AIProvider* provider {nullptr};
        QString key;
        QString model;
        QString baseUrl;
        QString origin() const { return provider->id + "/" + model; }
    };

    bool resolveTarget(Target& target);
    void startTurn();
    QJsonObject buildOpenAI() const;
    QJsonObject buildAnthropic() const;
    QJsonObject buildGoogle() const;
    bool canReplay(const LC_AIChatMessage& m) const { return m.origin == m_target.origin() && !m.raw.isEmpty(); }

    void onReadyRead();
    /** Network errors, timeouts, 408/429/5xx and overload are worth another try; wrong keys or requests are not. */
    bool retryable(int status, QNetworkReply::NetworkError error) const;
    bool scheduleRetry(const QString& reason, int retryAfterSeconds);
    void onFinished();
    void processLine(const QByteArray& line);
    void processEvent(const QJsonObject& event);
    void appendText(const QString& text);
    void finishTurn();
    void runTools(int index);
    void finishTool(int index, const LC_AIToolResult& result);
    void executeTool(int index, const LC_AITool* tool);
    /** The run was stopped: the calls of the turn that got no result get a "stopped" one, so the next request is valid. */
    void answerUnrunCalls();
    void fail(const QString& error);
    QString errorFromBody(const QByteArray& body) const;

    QNetworkAccessManager* m_network {nullptr};
    QPointer<QNetworkReply> m_reply;
    QList<LC_AITool> m_tools;
    Approver m_approver;
    QList<LC_AIChatMessage> m_history;
    Target m_target;
    bool m_running {false};
    bool m_cancelled {false};
    int m_round {0};
    int m_generation {0};  // bumped per run, so late answers of async tools from an old run are ignored
    int m_attempt {0};     // retries of the current turn
    bool m_gotHeaders {false};
    bool m_timedOut {false};
    QTimer* m_watchdog {nullptr};

    // state of the turn being streamed
    QByteArray m_buffer;
    QByteArray m_rawBody;
    QString m_streamError;
    QString m_stopNote;
    LC_AIChatMessage m_turn;
    QMap<int, QJsonObject> m_blocks;     // Anthropic: content blocks by index
    QMap<int, QString> m_partialJson;    // Anthropic tool_use input / OpenAI tool arguments, by index
    QMap<int, LC_AIToolCall> m_openAICalls;
    // the tool calls of m_turn once it is in the history: how many have a result, and whether the next one is running
    bool m_callsInHistory {false};
    int m_answered {0};
    bool m_executing {false};

    QString runSystemPrompt() const; // systemPrompt() and m_promptAddition
    QString m_promptAddition;
};

#endif
