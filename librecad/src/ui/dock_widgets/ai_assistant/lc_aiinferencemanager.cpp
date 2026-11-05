/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aiinferencemanager.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <exception>

#include "lc_aicredentials.h"

namespace {
    constexpr int TRANSFER_TIMEOUT_MS = 10 * 60 * 1000;
    constexpr int MAX_RAW_BODY = 64 * 1024;

    // Anthropic Messages API version and the Claude models that take the server-side refusal fallback
    const QByteArray ANTHROPIC_VERSION = "2026-10-21";
    const QByteArray ANTHROPIC_FALLBACK_BETA = "server-side-fallback-2026-07-01";
    const QStringList FALLBACK_MODELS = {"claude-fable-5-1", "claude-opus-5-5", "claude-opus-5", "claude-sonnet-5-5"};

    // tool call ids we invent (Gemini may omit them) are not sent back to Gemini
    const QString GENERATED_ID_PREFIX = QStringLiteral("gen_");
    // Gemini 3 accepts this in place of a thought signature for calls it did not produce itself
    const QString SKIP_SIGNATURE = QStringLiteral("skip_thought_signature_validator");

    QString joinUrl(QString base, const QString& path) {
        while (base.endsWith('/')) {
            base.chop(1);
        }
        return base + path;
    }

    QString compactJson(const QJsonObject& o) {
        return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
    }

    QString base64(const QByteArray& data) {
        return QString::fromLatin1(data.toBase64());
    }

    QJsonObject parseArguments(const QString& json, QString& error) {
        if (json.trimmed().isEmpty()) {
            return {};
        }
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            error = QStringLiteral("The tool arguments were not a valid JSON object: %1").arg(parseError.errorString());
            return {};
        }
        return doc.object();
    }

    /**
     * Gemini takes an OpenAPI subset of JSON schema: inline $ref (from $defs/definitions), turn
     * ["x", "null"] types into nullable, const into a one-value enum, and keep only the keywords it knows.
     * (Python MCP servers such as UniCAD generate schemas with $defs and nullable unions.)
     */
    QJsonValue geminiSchema(const QJsonValue& value, const QJsonObject& defs, const int depth = 0) {
        static const QStringList KEEP = {"type", "format", "title", "description", "nullable", "enum", "maxItems",
                                         "minItems", "properties", "required", "minProperties", "maxProperties",
                                         "minLength", "maxLength", "pattern", "anyOf", "items", "minimum", "maximum"};
        if (value.isArray()) {
            QJsonArray out;
            for (const QJsonValue& v : value.toArray()) {
                out.append(geminiSchema(v, defs, depth));
            }
            return out;
        }
        if (!value.isObject() || depth > 12) {
            return depth > 12 ? QJsonValue(QJsonObject{{"type", "object"}}) : value;
        }
        QJsonObject in = value.toObject();
        if (in.contains("$ref")) { // "#/$defs/Name" -> the definition, merged with sibling keywords
            const QString name = in.value("$ref").toString().section('/', -1);
            QJsonObject resolved = defs.value(name).toObject();
            for (auto it = in.begin(); it != in.end(); ++it) {
                if (it.key() != "$ref") {
                    resolved.insert(it.key(), it.value());
                }
            }
            return geminiSchema(resolved, defs, depth + 1);
        }
        if (in.value("type").isArray()) {
            QJsonArray types = in.value("type").toArray();
            bool nullable = false;
            for (int i = types.size() - 1; i >= 0; --i) {
                if (types[i].toString() == "null") {
                    types.removeAt(i);
                    nullable = true;
                }
            }
            in.insert("type", types.isEmpty() ? QJsonValue("string") : types.first());
            if (nullable) {
                in.insert("nullable", true);
            }
        }
        if (in.contains("const") && !in.contains("enum")) {
            in.insert("enum", QJsonArray{in.value("const")});
        }
        if (in.contains("anyOf")) { // anyOf [X, {type: null}] -> X, nullable
            QJsonArray options = in.value("anyOf").toArray();
            for (int i = options.size() - 1; i >= 0; --i) {
                if (options[i].toObject().value("type").toString() == "null") {
                    options.removeAt(i);
                    in.insert("nullable", true);
                }
            }
            if (options.size() == 1) {
                QJsonObject only = options.first().toObject();
                in.remove("anyOf");
                for (auto it = only.begin(); it != only.end(); ++it) {
                    if (!in.contains(it.key())) {
                        in.insert(it.key(), it.value());
                    }
                }
                return geminiSchema(in, defs, depth + 1);
            }
            in.insert("anyOf", options);
        }
        if (in.value("type").toString() == "array" && !in.value("items").isObject()) {
            // a tuple (pydantic: prefixItems, one schema per position): Gemini needs a single item schema
            const QJsonArray positions = in.value("prefixItems").toArray();
            in.insert("items", positions.isEmpty() ? QJsonObject{{"type", "string"}} : positions.first().toObject());
            if (positions.size() > 1 && positions.first() != positions.last()) {
                in.insert("description", QString(in.value("description").toString() + " Items by position: "
                                                  + QString::fromUtf8(QJsonDocument(positions).toJson(QJsonDocument::Compact))).trimmed());
            }
        }
        if (in.contains("enum")) {
            // Gemini enums are non-empty strings on string fields: null becomes nullable, anything else
            // it cannot express moves into the description so the model still knows the choices
            QJsonArray values;
            bool expressible = in.value("type").toString("string") == "string";
            QStringList shown;
            for (const QJsonValue& v : in.value("enum").toArray()) {
                if (v.isNull()) {
                    in.insert("nullable", true);
                    continue;
                }
                expressible = expressible && v.isString() && !v.toString().isEmpty();
                values.append(v);
                shown << (v.isString() ? (v.toString().isEmpty() ? QStringLiteral("\"\" (empty)") : v.toString())
                                       : v.toVariant().toString());
            }
            if (expressible && !values.isEmpty()) {
                in.insert("enum", values);
            }
            else {
                in.remove("enum");
                if (!shown.isEmpty()) {
                    in.insert("description", QString(in.value("description").toString() + " Allowed values: " + shown.join(", ") + ".").trimmed());
                }
            }
        }
        QJsonObject out;
        for (auto it = in.begin(); it != in.end(); ++it) {
            if (!KEEP.contains(it.key())) {
                continue;
            }
            if (it.key() == "properties") {
                QJsonObject props;
                const QJsonObject p = it.value().toObject();
                for (auto pit = p.begin(); pit != p.end(); ++pit) {
                    props.insert(pit.key(), geminiSchema(pit.value(), defs, depth + 1));
                }
                out.insert("properties", props);
            }
            else if (it.key() == "items" || it.key() == "anyOf") {
                out.insert(it.key(), geminiSchema(it.value(), defs, depth + 1));
            }
            else if (it.key() == "enum") {
                QJsonArray values; // Gemini enums are strings
                for (const QJsonValue& v : it.value().toArray()) {
                    values.append(v.isString() ? v : QJsonValue(v.toVariant().toString()));
                }
                out.insert("enum", values);
            }
            else {
                out.insert(it.key(), it.value());
            }
        }
        if (out.value("type").toString() == "object" && !out.contains("properties")) {
            out.insert("properties", QJsonObject());
        }
        return out;
    }

    QJsonValue geminiSchema(const QJsonObject& root) {
        QJsonObject defs = root.value("$defs").toObject();
        const QJsonObject legacy = root.value("definitions").toObject();
        for (auto it = legacy.begin(); it != legacy.end(); ++it) {
            defs.insert(it.key(), it.value());
        }
        return geminiSchema(root, defs, 0);
    }

    QString resultText(const LC_AIToolResult& result) {
        const QString text = result.joinedText();
        if (!text.trimmed().isEmpty()) {
            return text;
        }
        for (const LC_AIContent& c : result.content) {
            if (c.type == LC_AIContent::Type::Image) {
                return QStringLiteral("(see attached image)");
            }
        }
        return QStringLiteral("(no output)");
    }
}

LC_AIInferenceManager::LC_AIInferenceManager(QObject* parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)), m_watchdog(new QTimer(this)) {
    // the server has not answered at all (e.g. a network that drops one IP version): give up and retry
    m_watchdog->setSingleShot(true);
    connect(m_watchdog, &QTimer::timeout, this, [this] {
        if (m_reply && !m_gotHeaders) {
            m_timedOut = true;
            m_reply->abort(); // onFinished() retries
        }
    });
}

QString LC_AIInferenceManager::systemPrompt() {
    return QStringLiteral(
        "You are the AI assistant built into OpenCAD, a 2D CAD application based on LibreCAD. "
        "Help the user with drafting, geometry, CAD workflows and the features of OpenCAD.\n\n"
        "You have tools that act on the drawing that is open in OpenCAD:\n"
        "- capture_view returns a screenshot of the drawing view. Use it to look at the design before you "
        "comment on it, and to check your work after every change.\n"
        "- get_view, zoom_to_fit, zoom_to_area, zoom and pan move around the drawing. The view tools can "
        "return a screenshot in the same call (capture: true).\n"
        "- get_drawing_info and list_entities read the drawing precisely: layers, extents, entities and "
        "their coordinates. Prefer them over guessing from a screenshot when you need exact numbers.\n"
        "- browser_search, browser_navigate, browser_read and the other browser_ tools look things up on the web "
        "(standards, datasheets, documentation) in OpenCAD's own browser window. Use them only for lookups; cite the "
        "page you used.\n"
        "- request_clarification asks the user one question (with choices when they help) and waits for the answer. "
        "Use it when the request is ambiguous and you cannot safely proceed (which project, units, sizes), instead of "
        "guessing; never ask what the other tools can tell you.\n"
        "- add_text places text labels (names, notes, numbers, titles); never use the text or mtext commands, they open a dialog.\n"
        "- run_command types into OpenCAD's command line, exactly like the user would, to draw or edit "
        "(for example: line, 0,0, 100,0, @0,50, ESC). list_commands shows what the command line accepts.\n"
        "- open_drawing opens a DXF/DWG file in OpenCAD; view_image_file shows you an image file.\n"
        "- Every toolbar button is yours to use like a person: list_toolbar_buttons lists them (and shows one in detail), "
        "press_toolbar_button presses one and answers its prompts with steps (typed points or values, clicks, drags, "
        "keys); tool_options sets the running tool's settings (angle, factor, radius, count, modes) shown in the Tool "
        "Options bar; select_entities selects what the editing buttons work on; click_drawing clicks or drags in the "
        "drawing (pick entities, window zoom); handle_dialog fills in and closes a dialog a button opened; "
        "set_toolbar_value picks the pen colour, width, line type, a named view or a UCS; test_toolbar_buttons checks "
        "buttons on a scratch drawing. Read each result: it says what changed, which tool is running, its prompt and "
        "options.\n"
        "- While you work, the user can look around the drawing but not edit it, unless they take over; then your next "
        "tool call waits until they hand it back, and its result starts with a NOTE of what they changed. Respect those "
        "changes: do not undo the user's work unless asked.\n"
        "- Tools named mcp__<server>__<tool> come from MCP servers set up in OpenCAD. For example UniCAD designs "
        "solar EPC drawing sets: after its drawings_render, open the returned DXF paths with open_drawing and "
        "check them with capture_view.\n\n"
        "All coordinates are drawing units of the open drawing. Make changes only when the user asks for "
        "them, and say what you changed. Keep answers concise and use Markdown. Never use LaTeX or math markup ($...$, \\text, \\times): the chat cannot render it. Write values as plain text with Unicode symbols, for example 7,000 to 10,000 kVA, ⌀750 mm, 11 kV ± 5%, 75 °C, 2 × 10⁶.");
}

bool LC_AIInferenceManager::resolveTarget(Target& target) {
    LC_AICredentials& creds = LC_AICredentials::instance();
    const QString providerId = creds.selectedProvider();
    target.provider = LC_AIProvider::find(providerId);
    if (target.provider == nullptr) {
        emit failed(tr("No AI provider is selected."));
        return false;
    }
    target.key = creds.apiKey(providerId);
    if (target.key.isEmpty() && !target.provider->keyOptional) {
        emit failed(tr("No API key for %1. Open Providers (the key button) and add one, or set %2.")
                        .arg(target.provider->name, target.provider->envVar));
        return false;
    }
    target.model = creds.model(providerId);
    if (target.model.isEmpty()) {
        emit failed(tr("No model is selected for %1. Pick one with the model button.").arg(target.provider->name));
        return false;
    }
    target.baseUrl = creds.baseUrl(providerId);
    return true;
}

bool LC_AIInferenceManager::send(const QList<LC_AIChatMessage>& history) {
    if (m_running) {
        return false;
    }
    Target target;
    if (!resolveTarget(target)) {
        return false;
    }
    m_target = target;
    m_history = history;
    m_round = 0;
    m_attempt = 0;
    m_cancelled = false;
    m_running = true;
    ++m_generation;
    startTurn();
    return true;
}

void LC_AIInferenceManager::cancel() {
    if (!m_running) {
        return;
    }
    m_cancelled = true;
    if (m_reply) {
        m_reply->abort(); // onFinished() ends the run
    }
    else {
        // waiting for a tool (e.g. an MCP server): stop now, its late answer is ignored
        ++m_generation;
        m_running = false;
        emit finished();
    }
}

// ------------------------------------------------------------------------------------------------ requests

QJsonObject LC_AIInferenceManager::buildOpenAI() const {
    QJsonArray messages;
    messages.append(QJsonObject{{"role", "system"}, {"content", systemPrompt()}});
    QJsonArray pendingImages; // OpenAI chat tool messages are text only: images follow in a user message
    const auto flushImages = [&messages, &pendingImages] {
        if (pendingImages.isEmpty()) {
            return;
        }
        QJsonArray content{QJsonObject{{"type", "text"}, {"text", "Attached image(s) from tool result:"}}};
        for (const QJsonValue& image : std::as_const(pendingImages)) {
            content.append(image);
        }
        messages.append(QJsonObject{{"role", "user"}, {"content", content}});
        pendingImages = QJsonArray();
    };
    for (const LC_AIChatMessage& m : m_history) {
        if (m.role != LC_AIChatMessage::Role::ToolResult) {
            flushImages();
        }
        switch (m.role) {
            case LC_AIChatMessage::Role::User:
                messages.append(QJsonObject{{"role", "user"}, {"content", m.text}});
                break;
            case LC_AIChatMessage::Role::Assistant: {
                if (m.text.isEmpty() && m.toolCalls.isEmpty()) {
                    break;
                }
                QJsonObject msg{{"role", "assistant"}};
                msg.insert("content", m.text.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(m.text));
                if (!m.toolCalls.isEmpty()) {
                    QJsonArray calls;
                    for (const LC_AIToolCall& c : m.toolCalls) {
                        calls.append(QJsonObject{{"id", c.id}, {"type", "function"},
                                                 {"function", QJsonObject{{"name", c.name}, {"arguments", compactJson(c.arguments)}}}});
                    }
                    msg.insert("tool_calls", calls);
                }
                messages.append(msg);
                break;
            }
            case LC_AIChatMessage::Role::ToolResult:
                messages.append(QJsonObject{{"role", "tool"}, {"tool_call_id", m.toolCallId}, {"content", resultText(m.result)}});
                for (const LC_AIContent& c : m.result.content) {
                    if (c.type == LC_AIContent::Type::Image) {
                        pendingImages.append(QJsonObject{
                            {"type", "image_url"},
                            {"image_url", QJsonObject{{"url", QString("data:" + c.mimeType + ";base64," + base64(c.data))}}}});
                    }
                }
                break;
        }
    }
    flushImages();

    QJsonObject body{{"model", m_target.model}, {"messages", messages}, {"stream", true}};
    if (!m_tools.isEmpty()) {
        QJsonArray tools;
        for (const LC_AITool& t : m_tools) {
            tools.append(QJsonObject{{"type", "function"},
                                     {"function", QJsonObject{{"name", t.name}, {"description", t.description},
                                                              {"parameters", t.parameters}}}});
        }
        body.insert("tools", tools);
    }
    return body;
}

QJsonObject LC_AIInferenceManager::buildAnthropic() const {
    QJsonArray messages;
    QJsonArray pendingResults; // all tool results of a turn go back in a single user message
    const auto flushResults = [&messages, &pendingResults] {
        if (!pendingResults.isEmpty()) {
            messages.append(QJsonObject{{"role", "user"}, {"content", pendingResults}});
            pendingResults = QJsonArray();
        }
    };
    for (const LC_AIChatMessage& m : m_history) {
        if (m.role != LC_AIChatMessage::Role::ToolResult) {
            flushResults();
        }
        switch (m.role) {
            case LC_AIChatMessage::Role::User:
                messages.append(QJsonObject{{"role", "user"}, {"content", m.text}});
                break;
            case LC_AIChatMessage::Role::Assistant: {
                QJsonArray content;
                if (canReplay(m)) {
                    content = m.raw; // thinking blocks go back unchanged, in their original order
                }
                else {
                    if (!m.text.isEmpty()) {
                        content.append(QJsonObject{{"type", "text"}, {"text", m.text}});
                    }
                    for (const LC_AIToolCall& c : m.toolCalls) {
                        content.append(QJsonObject{{"type", "tool_use"}, {"id", c.id}, {"name", c.name}, {"input", c.arguments}});
                    }
                }
                if (!content.isEmpty()) {
                    messages.append(QJsonObject{{"role", "assistant"}, {"content", content}});
                }
                break;
            }
            case LC_AIChatMessage::Role::ToolResult: {
                QJsonArray content;
                for (const LC_AIContent& c : m.result.content) {
                    if (c.type == LC_AIContent::Type::Text) {
                        content.append(QJsonObject{{"type", "text"}, {"text", c.text.isEmpty() ? QStringLiteral("(empty)") : c.text}});
                    }
                    else {
                        content.append(QJsonObject{{"type", "image"},
                                                   {"source", QJsonObject{{"type", "base64"}, {"media_type", c.mimeType},
                                                                          {"data", base64(c.data)}}}});
                    }
                }
                QJsonObject block{{"type", "tool_result"}, {"tool_use_id", m.toolCallId}, {"content", content}};
                if (m.result.isError) {
                    block.insert("is_error", true);
                }
                pendingResults.append(block);
                break;
            }
        }
    }
    flushResults();

    QJsonObject body{{"model", m_target.model}, {"max_tokens", m_target.provider->maxTokens},
                     {"system", systemPrompt()}, {"messages", messages}, {"stream", true}};
    if (!m_tools.isEmpty()) {
        const bool firstParty = m_target.provider->id == "anthropic"; // gateways may reject the field
        QJsonArray tools;
        for (const LC_AITool& t : m_tools) {
            QJsonObject tool{{"name", t.name}, {"description", t.description}, {"input_schema", t.parameters}};
            if (firstParty) {
                tool.insert("eager_input_streaming", true); // inputs are validated in finishTurn()
            }
            tools.append(tool);
        }
        body.insert("tools", tools);
    }
    if (m_target.provider->id == "anthropic" && FALLBACK_MODELS.contains(m_target.model)) {
        body.insert("fallbacks", "default"); // retried on a fallback model instead of refusing
    }
    return body;
}

QJsonObject LC_AIInferenceManager::buildGoogle() const {
    const bool gemini3 = m_target.model.startsWith("gemini-3");
    QJsonArray contents;
    QJsonArray pendingParts;  // function responses of a turn: one user turn
    QJsonArray pendingImages; // older Gemini: images after the function responses
    const auto flush = [&contents, &pendingParts, &pendingImages] {
        if (!pendingParts.isEmpty()) {
            contents.append(QJsonObject{{"role", "user"}, {"parts", pendingParts}});
            pendingParts = QJsonArray();
        }
        if (!pendingImages.isEmpty()) {
            QJsonArray parts{QJsonObject{{"text", "Attached image(s) from tool result:"}}};
            for (const QJsonValue& image : std::as_const(pendingImages)) {
                parts.append(image);
            }
            contents.append(QJsonObject{{"role", "user"}, {"parts", parts}});
            pendingImages = QJsonArray();
        }
    };
    for (const LC_AIChatMessage& m : m_history) {
        if (m.role != LC_AIChatMessage::Role::ToolResult) {
            flush();
        }
        switch (m.role) {
            case LC_AIChatMessage::Role::User:
                contents.append(QJsonObject{{"role", "user"}, {"parts", QJsonArray{QJsonObject{{"text", m.text}}}}});
                break;
            case LC_AIChatMessage::Role::Assistant: {
                QJsonArray parts;
                if (canReplay(m)) {
                    parts = m.raw; // keeps the thought signatures Gemini 3 requires
                }
                else {
                    if (!m.text.isEmpty()) {
                        parts.append(QJsonObject{{"text", m.text}});
                    }
                    for (const LC_AIToolCall& c : m.toolCalls) {
                        QJsonObject part{{"functionCall", QJsonObject{{"name", c.name}, {"args", c.arguments}}}};
                        if (gemini3) {
                            part.insert("thoughtSignature", SKIP_SIGNATURE);
                        }
                        parts.append(part);
                    }
                }
                if (!parts.isEmpty()) {
                    contents.append(QJsonObject{{"role", "model"}, {"parts", parts}});
                }
                break;
            }
            case LC_AIChatMessage::Role::ToolResult: {
                const QString text = resultText(m.result);
                QJsonObject response{{"name", m.toolName},
                                     {"response", m.result.isError ? QJsonObject{{"error", text}} : QJsonObject{{"output", text}}}};
                if (!m.toolCallId.startsWith(GENERATED_ID_PREFIX)) {
                    response.insert("id", m.toolCallId);
                }
                QJsonArray imageParts;
                for (const LC_AIContent& c : m.result.content) {
                    if (c.type == LC_AIContent::Type::Image) {
                        imageParts.append(QJsonObject{{"inlineData", QJsonObject{{"mimeType", c.mimeType}, {"data", base64(c.data)}}}});
                    }
                }
                if (!imageParts.isEmpty()) {
                    if (gemini3) {
                        response.insert("parts", imageParts); // multimodal function response
                    }
                    else {
                        for (const QJsonValue& v : std::as_const(imageParts)) {
                            pendingImages.append(v);
                        }
                    }
                }
                pendingParts.append(QJsonObject{{"functionResponse", response}});
                break;
            }
        }
    }
    flush();

    QJsonObject body{{"systemInstruction", QJsonObject{{"parts", QJsonArray{QJsonObject{{"text", systemPrompt()}}}}}},
                     {"contents", contents}};
    if (!m_tools.isEmpty()) {
        QJsonArray declarations;
        for (const LC_AITool& t : m_tools) {
            declarations.append(QJsonObject{{"name", t.name}, {"description", t.description},
                                            {"parameters", geminiSchema(t.parameters)}});
        }
        body.insert("tools", QJsonArray{QJsonObject{{"functionDeclarations", declarations}}});
    }
    return body;
}

void LC_AIInferenceManager::startTurn() {
    m_buffer.clear();
    m_rawBody.clear();
    m_streamError.clear();
    m_stopNote.clear();
    m_blocks.clear();
    m_partialJson.clear();
    m_openAICalls.clear();
    m_turn = LC_AIChatMessage();
    m_turn.role = LC_AIChatMessage::Role::Assistant;
    m_turn.origin = m_target.origin();

    QNetworkRequest request;
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "text/event-stream");
    request.setTransferTimeout(TRANSFER_TIMEOUT_MS);
    // HTTP/1.1: Qt then tries IPv6 and IPv4 side by side (happy eyeballs). Over HTTP/2 it waits on IPv6 alone,
    // which hangs on networks whose IPv6 is broken. Streaming works the same either way.
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    const QByteArray key = m_target.key.toUtf8();
    QJsonObject body;
    switch (m_target.provider->api) {
        case LC_AIProvider::Api::OpenAIChat:
            request.setUrl(QUrl(joinUrl(m_target.baseUrl, "/chat/completions")));
            if (!key.isEmpty()) {
                request.setRawHeader("Authorization", "Bearer " + key);
            }
            body = buildOpenAI();
            break;
        case LC_AIProvider::Api::AnthropicMessages:
            request.setUrl(QUrl(joinUrl(m_target.baseUrl, "/v1/messages")));
            request.setRawHeader("x-api-key", key);
            request.setRawHeader("anthropic-version", ANTHROPIC_VERSION);
            if (m_target.provider->id == "anthropic" && FALLBACK_MODELS.contains(m_target.model)) {
                request.setRawHeader("anthropic-beta", ANTHROPIC_FALLBACK_BETA);
            }
            body = buildAnthropic();
            break;
        case LC_AIProvider::Api::GoogleGenerativeAI:
            request.setUrl(QUrl(joinUrl(m_target.baseUrl, "/models/" + m_target.model + ":streamGenerateContent?alt=sse")));
            request.setRawHeader("x-goog-api-key", key);
            body = buildGoogle();
            break;
    }
    m_gotHeaders = false;
    m_timedOut = false;
    m_reply = m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::metaDataChanged, this, [this] {
        m_gotHeaders = true;
        m_watchdog->stop();
    });
    connect(m_reply, &QNetworkReply::readyRead, this, &LC_AIInferenceManager::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &LC_AIInferenceManager::onFinished);
    m_watchdog->start(CONNECT_TIMEOUT_MS);
}

// ------------------------------------------------------------------------------------------------ streaming

void LC_AIInferenceManager::onReadyRead() {
    const QByteArray chunk = m_reply->readAll();
    if (m_rawBody.size() < MAX_RAW_BODY) {
        m_rawBody.append(chunk.left(MAX_RAW_BODY - m_rawBody.size()));
    }
    m_buffer.append(chunk);
    qsizetype newline;
    while ((newline = m_buffer.indexOf('\n')) >= 0) {
        QByteArray line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        processLine(line);
    }
}

void LC_AIInferenceManager::processLine(const QByteArray& line) {
    // server-sent events: payloads are on "data:" lines (event names are repeated in the JSON "type")
    if (!line.startsWith("data:")) {
        return;
    }
    const QByteArray data = line.mid(5).trimmed();
    if (data.isEmpty() || data == "[DONE]") {
        return;
    }
    const QJsonObject event = QJsonDocument::fromJson(data).object();
    if (!event.isEmpty()) {
        processEvent(event);
    }
}

void LC_AIInferenceManager::appendText(const QString& text) {
    if (!text.isEmpty()) {
        m_turn.text += text;
        emit delta(text);
    }
}

void LC_AIInferenceManager::processEvent(const QJsonObject& event) {
    if (event.contains("error")) {
        const QJsonValue error = event.value("error");
        m_streamError = error.isObject() ? error.toObject().value("message").toString() : error.toString();
        return;
    }
    switch (m_target.provider->api) {
        case LC_AIProvider::Api::OpenAIChat: {
            const QJsonArray choices = event.value("choices").toArray();
            if (choices.isEmpty()) {
                return;
            }
            const QJsonObject d = choices.first().toObject().value("delta").toObject();
            appendText(d.value("content").toString());
            const QJsonArray calls = d.value("tool_calls").toArray();
            for (int i = 0; i < calls.size(); ++i) {
                const QJsonObject call = calls[i].toObject();
                const int index = call.contains("index") ? call.value("index").toInt() : i;
                LC_AIToolCall& c = m_openAICalls[index];
                if (!call.value("id").toString().isEmpty()) {
                    c.id = call.value("id").toString();
                }
                const QJsonObject fn = call.value("function").toObject();
                if (!fn.value("name").toString().isEmpty()) {
                    c.name = fn.value("name").toString();
                }
                m_partialJson[index] += fn.value("arguments").toString();
            }
            break;
        }
        case LC_AIProvider::Api::AnthropicMessages: {
            const QString type = event.value("type").toString();
            const int index = event.value("index").toInt();
            if (type == "content_block_start") {
                QJsonObject block = event.value("content_block").toObject();
                m_blocks[index] = block;
                if (block.value("type").toString() == "text") {
                    appendText(block.value("text").toString());
                }
            }
            else if (type == "content_block_delta") {
                const QJsonObject d = event.value("delta").toObject();
                const QString deltaType = d.value("type").toString();
                QJsonObject& block = m_blocks[index];
                if (deltaType == "text_delta") {
                    block.insert("text", QString(block.value("text").toString() + d.value("text").toString()));
                    appendText(d.value("text").toString());
                }
                else if (deltaType == "thinking_delta") {
                    block.insert("thinking", QString(block.value("thinking").toString() + d.value("thinking").toString()));
                }
                else if (deltaType == "signature_delta") {
                    block.insert("signature", QString(block.value("signature").toString() + d.value("signature").toString()));
                }
                else if (deltaType == "input_json_delta") {
                    m_partialJson[index] += d.value("partial_json").toString();
                }
            }
            else if (type == "message_delta") {
                const QString stop = event.value("delta").toObject().value("stop_reason").toString();
                if (stop == "refusal") {
                    m_stopNote = tr("The model declined to answer this request.");
                }
                else if (stop == "max_tokens") {
                    m_stopNote = tr("The reply was cut off at the output limit.");
                }
            }
            break;
        }
        case LC_AIProvider::Api::GoogleGenerativeAI: {
            const QJsonArray candidates = event.value("candidates").toArray();
            if (candidates.isEmpty()) {
                return;
            }
            const QJsonObject candidate = candidates.first().toObject();
            for (const QJsonValue& value : candidate.value("content").toObject().value("parts").toArray()) {
                const QJsonObject part = value.toObject();
                if (part.contains("functionCall")) {
                    const QJsonObject fc = part.value("functionCall").toObject();
                    QString id = fc.value("id").toString();
                    if (id.isEmpty()) {
                        id = GENERATED_ID_PREFIX + QString::number(m_turn.toolCalls.size() + 1);
                    }
                    m_turn.toolCalls.append({id, fc.value("name").toString(), fc.value("args").toObject(), {}});
                    m_turn.raw.append(part);
                    continue;
                }
                const bool thought = part.value("thought").toBool();
                const QString text = part.value("text").toString();
                if (!thought) {
                    appendText(text);
                }
                // merge streamed text pieces into one part; keep any signature on it
                QJsonObject last = m_turn.raw.isEmpty() ? QJsonObject() : m_turn.raw.last().toObject();
                const bool mergeable = !m_turn.raw.isEmpty() && last.contains("text") && !last.contains("functionCall")
                                       && last.value("thought").toBool() == thought;
                if (mergeable) {
                    last.insert("text", QString(last.value("text").toString() + text));
                    if (part.contains("thoughtSignature")) {
                        last.insert("thoughtSignature", part.value("thoughtSignature"));
                    }
                    m_turn.raw.replace(m_turn.raw.size() - 1, last);
                }
                else if (!text.isEmpty() || part.contains("thoughtSignature")) {
                    m_turn.raw.append(part);
                }
            }
            const QString finish = candidate.value("finishReason").toString();
            if (finish == "MALFORMED_FUNCTION_CALL" || finish == "UNEXPECTED_TOOL_CALL") {
                m_stopNote = tr("The model produced a tool call it could not complete (%1).").arg(finish);
            }
            else if (finish == "MAX_TOKENS") {
                m_stopNote = tr("The reply was cut off at the output limit.");
            }
            else if (finish == "SAFETY" || finish == "PROHIBITED_CONTENT") {
                m_stopNote = tr("The model declined to answer this request.");
            }
            break;
        }
    }
}

QString LC_AIInferenceManager::errorFromBody(const QByteArray& body) const {
    QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isArray() && !doc.array().isEmpty()) {
        doc = QJsonDocument(doc.array().first().toObject()); // Gemini wraps errors in an array
    }
    const QJsonValue error = doc.object().value("error");
    if (error.isObject() && !error.toObject().value("message").toString().isEmpty()) {
        return error.toObject().value("message").toString();
    }
    if (error.isString()) {
        return error.toString();
    }
    if (!doc.object().value("message").toString().isEmpty()) {
        return doc.object().value("message").toString();
    }
    return QString::fromUtf8(body.left(500)).trimmed();
}

void LC_AIInferenceManager::fail(const QString& error) {
    m_running = false;
    emit failed(error);
}

void LC_AIInferenceManager::onFinished() {
    QNetworkReply* reply = m_reply;
    if (reply == nullptr) {
        return;
    }
    if (!m_buffer.isEmpty()) {
        processLine(m_buffer.trimmed());
        m_buffer.clear();
    }
    m_watchdog->stop();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError netError = reply->error();
    const QString netErrorString = reply->errorString();
    const int retryAfter = reply->rawHeader("Retry-After").toInt();
    m_reply = nullptr;
    reply->deleteLater();

    if (m_cancelled) {
        if (!m_turn.text.isEmpty()) {
            m_turn.toolCalls.clear(); // half-streamed calls are not run
            m_turn.raw = QJsonArray();
            m_history.append(m_turn);
            emit messageAdded(m_turn);
        }
        m_running = false;
        emit finished();
        return;
    }
    // a failure before any of the reply streamed in can simply be tried again
    const bool nothingShown = m_turn.text.isEmpty() && m_turn.toolCalls.isEmpty() && m_openAICalls.isEmpty() && m_blocks.isEmpty();
    QString error;
    bool mayPass = false;
    if (m_timedOut) {
        error = tr("No answer from %1 within %2 s.").arg(m_target.provider->name).arg(CONNECT_TIMEOUT_MS / 1000);
        mayPass = true;
    }
    else if (status >= 400) {
        const QString detail = errorFromBody(m_rawBody);
        error = tr("HTTP %1: %2").arg(status).arg(detail.isEmpty() ? netErrorString : detail);
        mayPass = retryable(status, netError);
    }
    else if (netError != QNetworkReply::NoError) {
        error = netErrorString;
        mayPass = retryable(status, netError);
    }
    else if (!m_streamError.isEmpty()) {
        error = m_streamError;
        mayPass = m_streamError.contains("overloaded", Qt::CaseInsensitive) || m_streamError.contains("rate limit", Qt::CaseInsensitive);
    }
    if (error.isEmpty()) {
        m_attempt = 0; // this turn went through
        finishTurn();
        return;
    }
    if (mayPass && nothingShown && scheduleRetry(error, retryAfter)) {
        return;
    }
    fail(m_attempt > 0 ? tr("%1 (gave up after %2 retries)").arg(error).arg(m_attempt) : error);
}

bool LC_AIInferenceManager::retryable(const int status, const QNetworkReply::NetworkError error) const {
    if (status > 0) {
        return status == 408 || status == 425 || status == 429 || (status >= 500 && status <= 599);
    }
    switch (error) {
        case QNetworkReply::ConnectionRefusedError:
        case QNetworkReply::RemoteHostClosedError:
        case QNetworkReply::HostNotFoundError:
        case QNetworkReply::TimeoutError:
        case QNetworkReply::TemporaryNetworkFailureError:
        case QNetworkReply::NetworkSessionFailedError:
        case QNetworkReply::UnknownNetworkError:
        case QNetworkReply::ProxyConnectionClosedError:
        case QNetworkReply::ProxyTimeoutError:
        case QNetworkReply::InternalServerError:
        case QNetworkReply::ServiceUnavailableError:
        case QNetworkReply::UnknownServerError:
            return true;
        default:
            return false; // e.g. SSL or authentication errors do not pass by themselves
    }
}

bool LC_AIInferenceManager::scheduleRetry(const QString& reason, const int retryAfterSeconds) {
    if (m_attempt >= MAX_RETRIES) {
        return false;
    }
    static const int BACKOFF[] = {2, 5, 10};
    const int seconds = retryAfterSeconds > 0 ? qMin(retryAfterSeconds, 60) : BACKOFF[qMin(m_attempt, 2)];
    ++m_attempt;
    emit retrying(m_attempt, MAX_RETRIES, seconds, reason);
    const int generation = m_generation;
    QTimer::singleShot(seconds * 1000, this, [this, generation] {
        if (m_running && !m_cancelled && m_generation == generation) {
            startTurn();
        }
    });
    return true;
}

void LC_AIInferenceManager::finishTurn() {
    switch (m_target.provider->api) {
        case LC_AIProvider::Api::OpenAIChat:
            for (auto it = m_openAICalls.begin(); it != m_openAICalls.end(); ++it) {
                LC_AIToolCall call = it.value();
                if (call.name.isEmpty()) {
                    continue;
                }
                if (call.id.isEmpty()) {
                    call.id = QStringLiteral("call_%1_%2").arg(m_round).arg(it.key());
                }
                call.arguments = parseArguments(m_partialJson.value(it.key()), call.argumentsError);
                m_turn.toolCalls.append(call);
            }
            break;
        case LC_AIProvider::Api::AnthropicMessages:
            // keep each block as the API expects it back, in order
            for (auto it = m_blocks.begin(); it != m_blocks.end(); ++it) {
                const QJsonObject b = it.value();
                const QString type = b.value("type").toString();
                if (type == "text") {
                    if (!b.value("text").toString().isEmpty()) {
                        m_turn.raw.append(QJsonObject{{"type", "text"}, {"text", b.value("text")}});
                    }
                }
                else if (type == "thinking") {
                    m_turn.raw.append(QJsonObject{{"type", "thinking"}, {"thinking", b.value("thinking").toString()},
                                                  {"signature", b.value("signature").toString()}});
                }
                else if (type == "tool_use") {
                    LC_AIToolCall call{b.value("id").toString(), b.value("name").toString(), {}, {}};
                    call.arguments = parseArguments(m_partialJson.value(it.key()), call.argumentsError);
                    m_turn.toolCalls.append(call);
                    m_turn.raw.append(QJsonObject{{"type", "tool_use"}, {"id", call.id}, {"name", call.name},
                                                  {"input", call.arguments}});
                }
                else {
                    m_turn.raw.append(b); // redacted_thinking and anything else: back verbatim
                }
            }
            break;
        case LC_AIProvider::Api::GoogleGenerativeAI:
            break; // tool calls and parts were collected while streaming
    }

    if (!m_stopNote.isEmpty()) {
        const QString note = (m_turn.text.isEmpty() ? QString() : QStringLiteral("\n\n")) + "_" + m_stopNote + "_";
        m_turn.text += note;
        emit delta(note);
    }
    if (m_turn.text.isEmpty() && m_turn.toolCalls.isEmpty()) {
        if (m_round == 0) {
            const QString detail = errorFromBody(m_rawBody);
            fail(detail.isEmpty() || detail.startsWith("data:") ? tr("The provider returned an empty reply.") : detail);
            return;
        }
        m_running = false; // a run may end quietly after its tool results
        emit finished();
        return;
    }
    m_history.append(m_turn);
    emit messageAdded(m_turn);

    if (m_turn.toolCalls.isEmpty()) {
        m_running = false;
        emit finished();
        return;
    }
    QTimer::singleShot(0, this, [this] { runTools(0); });
}

void LC_AIInferenceManager::runTools(const int index) {
    if (!m_running) {
        return; // the run was stopped meanwhile
    }
    const QList<LC_AIToolCall> calls = m_turn.toolCalls;
    if (m_cancelled || index >= calls.size()) {
        if (m_cancelled) {
            m_running = false;
            emit finished();
            return;
        }
        if (m_round + 1 >= MAX_TOOL_ROUNDS) {
            // the calls of this last round have run, so the history stays valid for "continue"
            LC_AIChatMessage note;
            note.role = LC_AIChatMessage::Role::Assistant;
            note.text = tr("_Paused after %1 rounds of tool calls. Say \"continue\" to go on._").arg(MAX_TOOL_ROUNDS);
            m_history.append(note);
            emit messageAdded(note);
            m_running = false;
            emit finished();
            return;
        }
        ++m_round;
        startTurn();
        return;
    }
    const LC_AIToolCall& call = calls.at(index);
    emit toolStarted(call);

    const LC_AITool* tool = nullptr;
    for (const LC_AITool& t : std::as_const(m_tools)) {
        if (t.name == call.name) {
            tool = &t;
            break;
        }
    }
    if (tool == nullptr) {
        finishTool(index, LC_AIToolResult::error(tr("Unknown tool: %1").arg(call.name)));
        return;
    }
    if (!call.argumentsError.isEmpty()) {
        finishTool(index, LC_AIToolResult::error(call.argumentsError + tr(" Send the call again with valid JSON.")));
        return;
    }
    if (m_approver) {
        const int generation = m_generation;
        QPointer<LC_AIInferenceManager> self(this);
        m_approver(call, [self, generation, index, tool](const bool allowed, const QString& reason) {
            if (!self || !self->m_running || self->m_generation != generation) {
                return; // the run was stopped while waiting for the user
            }
            if (allowed) {
                self->executeTool(index, tool);
            }
            else {
                self->finishTool(index, LC_AIToolResult::error(reason));
            }
        });
        return;
    }
    executeTool(index, tool);
}

void LC_AIInferenceManager::executeTool(const int index, const LC_AITool* tool) {
    const LC_AIToolCall call = m_turn.toolCalls.value(index);
    if (tool->executeAsync) {
        const int generation = m_generation;
        QPointer<LC_AIInferenceManager> self(this);
        tool->executeAsync(call.arguments, [self, generation, index](const LC_AIToolResult& result) {
            if (self && self->m_running && self->m_generation == generation) {
                self->finishTool(index, result);
            }
        });
        return;
    }
    LC_AIToolResult result;
    try {
        result = tool->execute(call.arguments);
    }
    catch (const std::exception& e) {
        result = LC_AIToolResult::error(tr("The tool failed: %1").arg(QString::fromUtf8(e.what())));
    }
    finishTool(index, result);
}

void LC_AIInferenceManager::finishTool(const int index, const LC_AIToolResult& result) {
    const LC_AIToolCall call = m_turn.toolCalls.value(index);
    LC_AIChatMessage message;
    message.role = LC_AIChatMessage::Role::ToolResult;
    message.toolCallId = call.id;
    message.toolName = call.name;
    message.result = result;
    m_history.append(message);
    emit messageAdded(message);
    // let the UI repaint between tools (screenshots need an up-to-date view)
    QTimer::singleShot(0, this, [this, index] { runTools(index + 1); });
}
