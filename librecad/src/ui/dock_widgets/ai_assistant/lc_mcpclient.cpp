/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_mcpclient.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

namespace {
    const QString PROTOCOL_VERSION = QStringLiteral("2025-06-18");
    constexpr int MAX_STDERR = 4000;

    /** "${NAME}" -> value of the environment variable NAME (as in Cypher's mcp.json). */
    QString expand(QString value) {
        static const QRegularExpression var(QStringLiteral(R"(\$\{([A-Za-z_][A-Za-z0-9_]*)\})"));
        QRegularExpressionMatch m;
        int from = 0;
        while ((m = var.match(value, from)).hasMatch()) {
            const QString replacement = qEnvironmentVariable(m.captured(1).toUtf8().constData());
            value.replace(m.capturedStart(), m.capturedLength(), replacement);
            from = m.capturedStart() + replacement.size();
        }
        return value;
    }

    QString findProgram(const QString& command) {
        if (command.contains('/') || command.contains('\\')) {
            return QDir::fromNativeSeparators(command);
        }
        QString found = QStandardPaths::findExecutable(command);
        if (found.isEmpty()) {
            // apps started from a desktop launcher often miss the user's tool directories (uvx, npx, ...)
            const QString home = QDir::homePath();
            found = QStandardPaths::findExecutable(command, {home + "/.local/bin", home + "/.cargo/bin", "/usr/local/bin",
                                                             home + "/.local/share/mise/shims", home + "/bin"});
        }
        return found.isEmpty() ? command : found;
    }
}

// ------------------------------------------------------------------------------------------------ config

LC_MCPServerConfig LC_MCPServerConfig::fromJson(const QString& name, const QJsonObject& json) {
    LC_MCPServerConfig c;
    c.name = name;
    const QString type = json.value("type").toString();
    c.url = json.value("url").toString();
    c.http = type == "http" || type == "streamable-http" || (type.isEmpty() && !c.url.isEmpty());
    c.command = json.value("command").toString();
    for (const QJsonValue& v : json.value("args").toArray()) {
        c.args << v.toString();
    }
    const QJsonObject env = json.value("env").toObject();
    for (auto it = env.begin(); it != env.end(); ++it) {
        c.env.insert(it.key(), it.value().toString());
    }
    c.cwd = json.value("cwd").toString();
    const QJsonObject headers = json.value("headers").toObject();
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        c.headers.insert(it.key(), it.value().toString());
    }
    c.enabled = json.value("enabled").toBool(true);
    c.description = json.value("description").toString();
    if (json.value("timeout").toDouble() > 0) {
        c.timeoutSeconds = qRound(json.value("timeout").toDouble());
    }
    return c;
}

QJsonObject LC_MCPServerConfig::toJson() const {
    QJsonObject json;
    if (http) {
        json.insert("type", "http");
        json.insert("url", url);
        if (!headers.isEmpty()) {
            QJsonObject h;
            for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
                h.insert(it.key(), it.value());
            }
            json.insert("headers", h);
        }
    }
    else {
        json.insert("command", command);
        if (!args.isEmpty()) {
            json.insert("args", QJsonArray::fromStringList(args));
        }
        if (!env.isEmpty()) {
            QJsonObject e;
            for (auto it = env.cbegin(); it != env.cend(); ++it) {
                e.insert(it.key(), it.value());
            }
            json.insert("env", e);
        }
        if (!cwd.isEmpty()) {
            json.insert("cwd", cwd);
        }
    }
    if (!enabled) {
        json.insert("enabled", false);
    }
    if (!description.isEmpty()) {
        json.insert("description", description);
    }
    if (timeoutSeconds != 300) {
        json.insert("timeout", timeoutSeconds);
    }
    return json;
}

QString LC_MCPServerConfig::target() const {
    if (http) {
        return url;
    }
    QStringList parts{command};
    for (const QString& a : args) {
        parts << (a.contains(' ') ? '"' + a + '"' : a);
    }
    return parts.join(' ');
}

// ------------------------------------------------------------------------------------------------ client

LC_MCPClient::LC_MCPClient(const LC_MCPServerConfig& config, QObject* parent)
    : QObject(parent), m_config(config) {
}

LC_MCPClient::~LC_MCPClient() {
    disconnectFromServer();
}

QString LC_MCPClient::stateText() const {
    switch (m_state) {
        case State::Disconnected:
            return m_config.enabled ? tr("Not connected") : tr("Disabled");
        case State::Connecting:
            return tr("Connecting...");
        case State::Connected:
            return tr("Connected");
        case State::Failed:
            return tr("Failed");
    }
    return {};
}

void LC_MCPClient::setState(const State state, const QString& error) {
    m_state = state;
    m_error = error;
    emit changed();
}

void LC_MCPClient::connectToServer() {
    disconnectFromServer();
    ++m_connection;
    m_stderr.clear();
    m_serverInfo.clear();
    m_tools.clear();
    setState(State::Connecting);
    if (m_config.http) {
        if (!QUrl(expand(m_config.url)).isValid() || !expand(m_config.url).startsWith("http")) {
            setState(State::Failed, tr("The URL must start with http:// or https://"));
            return;
        }
        m_network = new QNetworkAccessManager(this);
        initialize();
    }
    else {
        startProcess();
    }
}

void LC_MCPClient::disconnectFromServer() {
    ++m_connection;
    failAll(tr("Disconnected"));
    if (m_process != nullptr) {
        QProcess* process = m_process;
        m_process = nullptr;
        process->disconnect(this);
        process->closeWriteChannel(); // stdio servers exit when their input closes
        if (!process->waitForFinished(1500)) {
            process->terminate();
            if (!process->waitForFinished(1500)) {
                process->kill();
                process->waitForFinished(500);
            }
        }
        process->deleteLater();
    }
    if (m_network != nullptr) {
        m_network->deleteLater();
        m_network = nullptr;
    }
    m_sessionId.clear();
    m_protocolVersion.clear();
    m_stdoutBuffer.clear();
    if (m_state != State::Disconnected) {
        m_tools.clear();
        setState(State::Disconnected);
    }
}

void LC_MCPClient::startProcess() {
    m_process = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (auto it = m_config.env.cbegin(); it != m_config.env.cend(); ++it) {
        env.insert(it.key(), expand(it.value()));
    }
    m_process->setProcessEnvironment(env);
    if (!m_config.cwd.isEmpty()) {
        m_process->setWorkingDirectory(expand(m_config.cwd));
    }
    QStringList args;
    for (const QString& a : std::as_const(m_config.args)) {
        args << expand(a);
    }
    const int connection = m_connection;
    connect(m_process, &QProcess::readyReadStandardOutput, this, &LC_MCPClient::onStdout);
    connect(m_process, &QProcess::readyReadStandardError, this, [this] {
        m_stderr += QString::fromUtf8(m_process->readAllStandardError());
        if (m_stderr.size() > MAX_STDERR) {
            m_stderr = m_stderr.right(MAX_STDERR);
        }
    });
    connect(m_process, &QProcess::started, this, [this, connection] {
        if (connection == m_connection) {
            initialize();
        }
    });
    connect(m_process, &QProcess::errorOccurred, this, [this, connection](const QProcess::ProcessError error) {
        if (connection != m_connection || error != QProcess::FailedToStart) {
            return;
        }
        failAll(tr("The server could not start"));
        setState(State::Failed, tr("Could not start \"%1\": %2").arg(m_config.command, m_process->errorString()));
    });
    connect(m_process, &QProcess::finished, this, [this, connection](const int exitCode, QProcess::ExitStatus) {
        if (connection != m_connection) {
            return;
        }
        const QString tail = m_stderr.trimmed().right(600);
        const QString message = tr("The server exited (code %1).").arg(exitCode) + (tail.isEmpty() ? QString() : "\n" + tail);
        failAll(message);
        m_tools.clear();
        setState(State::Failed, message);
    });
    m_process->start(findProgram(expand(m_config.command)), args);
}

void LC_MCPClient::onStdout() {
    m_stdoutBuffer.append(m_process->readAllStandardOutput());
    qsizetype newline;
    while ((newline = m_stdoutBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdoutBuffer.left(newline).trimmed();
        m_stdoutBuffer.remove(0, newline + 1);
        if (line.isEmpty()) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject()) {
            handleMessage(doc.object());
        }
        else if (doc.isArray()) {
            for (const QJsonValue& v : doc.array()) {
                handleMessage(v.toObject());
            }
        }
        // anything else is log output a server printed on stdout: ignored
    }
}

void LC_MCPClient::send(const QJsonObject& message) {
    if (m_config.http) {
        post(message);
    }
    else if (m_process != nullptr && m_process->state() == QProcess::Running) {
        m_process->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + "\n");
    }
}

void LC_MCPClient::post(const QJsonObject& message) {
    if (m_network == nullptr) {
        return;
    }
    QNetworkRequest req(QUrl(expand(m_config.url)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Accept", "application/json, text/event-stream");
    for (auto it = m_config.headers.cbegin(); it != m_config.headers.cend(); ++it) {
        req.setRawHeader(it.key().toUtf8(), expand(it.value()).toUtf8());
    }
    if (!m_sessionId.isEmpty()) {
        req.setRawHeader("Mcp-Session-Id", m_sessionId.toUtf8());
    }
    if (!m_protocolVersion.isEmpty()) {
        req.setRawHeader("MCP-Protocol-Version", m_protocolVersion.toUtf8());
    }
    req.setTransferTimeout(m_config.timeoutSeconds * 1000);
    // HTTP/1.1 lets Qt fall back from a broken IPv6 route to IPv4 (it does not over HTTP/2)
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    QNetworkReply* reply = m_network->post(req, QJsonDocument(message).toJson(QJsonDocument::Compact));
    const QJsonValue id = message.value("id");
    const int connection = m_connection;
    auto buffer = std::make_shared<QByteArray>();
    auto sse = std::make_shared<QByteArray>(); // data lines of the SSE event being read
    const auto parseSse = [this, buffer, sse](const bool flush) {
        qsizetype newline;
        while ((newline = buffer->indexOf('\n')) >= 0 || (flush && !buffer->isEmpty())) {
            QByteArray line = newline >= 0 ? buffer->left(newline) : *buffer;
            buffer->remove(0, newline >= 0 ? newline + 1 : buffer->size());
            if (line.endsWith('\r')) {
                line.chop(1);
            }
            if (line.startsWith("data:")) {
                sse->append(line.mid(5).trimmed());
            }
            else if (line.isEmpty() && !sse->isEmpty()) { // end of an event
                const QJsonDocument doc = QJsonDocument::fromJson(*sse);
                sse->clear();
                if (doc.isObject()) {
                    handleMessage(doc.object());
                }
            }
        }
        if (flush && !sse->isEmpty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(*sse);
            sse->clear();
            if (doc.isObject()) {
                handleMessage(doc.object());
            }
        }
    };
    connect(reply, &QNetworkReply::readyRead, this, [reply, buffer, parseSse] {
        buffer->append(reply->readAll());
        if (reply->header(QNetworkRequest::ContentTypeHeader).toString().contains("text/event-stream")) {
            parseSse(false);
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, buffer, parseSse, id, connection] {
        reply->deleteLater();
        if (connection != m_connection) {
            return;
        }
        buffer->append(reply->readAll());
        const QString sessionId = QString::fromUtf8(reply->rawHeader("Mcp-Session-Id"));
        if (!sessionId.isEmpty()) {
            m_sessionId = sessionId;
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 400 || (status == 0 && reply->error() != QNetworkReply::NoError)) {
            QString detail = QString::fromUtf8(buffer->left(400)).trimmed();
            if (detail.isEmpty()) {
                detail = reply->errorString();
            }
            const QString error = status > 0 ? tr("HTTP %1: %2").arg(status).arg(detail) : detail;
            if (!id.isUndefined()) {
                resolve(id, {}, error);
            }
            return;
        }
        if (reply->header(QNetworkRequest::ContentTypeHeader).toString().contains("text/event-stream")) {
            parseSse(true);
        }
        else if (!buffer->trimmed().isEmpty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(*buffer);
            if (doc.isObject()) {
                handleMessage(doc.object());
            }
            else if (doc.isArray()) {
                for (const QJsonValue& v : doc.array()) {
                    handleMessage(v.toObject());
                }
            }
        }
        if (!id.isUndefined() && m_pending.contains(id.toInteger())) {
            resolve(id, {}, tr("The server closed the response without an answer."));
        }
    });
}

void LC_MCPClient::request(const QString& method, const QJsonObject& params, const Callback& callback) {
    const qint64 id = m_nextId++;
    Pending pending;
    pending.callback = callback;
    pending.timer = new QTimer(this);
    pending.timer->setSingleShot(true);
    connect(pending.timer, &QTimer::timeout, this, [this, id] {
        resolve(QJsonValue(id), {}, tr("No answer within %1 s.").arg(m_config.timeoutSeconds));
    });
    pending.timer->start(m_config.timeoutSeconds * 1000);
    m_pending.insert(id, pending);
    QJsonObject message{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params.isEmpty()) {
        message.insert("params", params);
    }
    send(message);
}

void LC_MCPClient::notify(const QString& method, const QJsonObject& params) {
    QJsonObject message{{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.isEmpty()) {
        message.insert("params", params);
    }
    send(message);
}

void LC_MCPClient::resolve(const QJsonValue& id, const QJsonObject& result, const QString& error) {
    const qint64 key = id.toInteger(-1);
    if (!m_pending.contains(key)) {
        return;
    }
    const Pending pending = m_pending.take(key);
    if (pending.timer != nullptr) {
        pending.timer->stop();
        pending.timer->deleteLater();
    }
    if (pending.callback) {
        pending.callback(result, error);
    }
}

void LC_MCPClient::failAll(const QString& error) {
    const QList<qint64> ids = m_pending.keys();
    for (const qint64 id : ids) {
        resolve(QJsonValue(id), {}, error);
    }
}

void LC_MCPClient::handleMessage(const QJsonObject& message) {
    const bool hasId = message.contains("id") && !message.value("id").isNull();
    const QString method = message.value("method").toString();
    if (method.isEmpty()) { // a response to one of our requests
        if (hasId) {
            const QJsonObject error = message.value("error").toObject();
            resolve(message.value("id"), message.value("result").toObject(),
                    message.contains("error") ? error.value("message").toString(tr("Unknown error")) : QString());
        }
        return;
    }
    if (hasId) { // a request from the server
        QJsonObject response{{"jsonrpc", "2.0"}, {"id", message.value("id")}};
        if (method == "ping") {
            response.insert("result", QJsonObject());
        }
        else if (method == "roots/list") {
            response.insert("result", QJsonObject{{"roots", QJsonArray()}});
        }
        else {
            response.insert("error", QJsonObject{{"code", -32601}, {"message", QString("Method not supported by OpenCAD: " + method)}});
        }
        send(response);
        return;
    }
    if (method == "notifications/tools/list_changed" && m_state == State::Connected) {
        listTools();
    }
}

void LC_MCPClient::initialize() {
    const int connection = m_connection;
    const QJsonObject params{{"protocolVersion", PROTOCOL_VERSION},
                             {"capabilities", QJsonObject()},
                             {"clientInfo", QJsonObject{{"name", "OpenCAD"}, {"version", "1.0"}}}};
    request("initialize", params, [this, connection](const QJsonObject& result, const QString& error) {
        if (connection != m_connection) {
            return;
        }
        if (!error.isEmpty()) {
            const QString tail = m_stderr.trimmed().right(600);
            setState(State::Failed, tr("initialize failed: %1").arg(error) + (tail.isEmpty() ? QString() : "\n" + tail));
            return;
        }
        m_protocolVersion = result.value("protocolVersion").toString(PROTOCOL_VERSION);
        const QJsonObject info = result.value("serverInfo").toObject();
        m_serverInfo = QString(info.value("name").toString() + " " + info.value("version").toString()).trimmed();
        notify("notifications/initialized");
        listTools();
    });
}

void LC_MCPClient::listTools(const QString& cursor, QList<Tool> collected) {
    const int connection = m_connection;
    QJsonObject params;
    if (!cursor.isEmpty()) {
        params.insert("cursor", cursor);
    }
    request("tools/list", params, [this, connection, collected](const QJsonObject& result, const QString& error) mutable {
        if (connection != m_connection) {
            return;
        }
        if (!error.isEmpty()) {
            setState(State::Failed, tr("tools/list failed: %1").arg(error));
            return;
        }
        for (const QJsonValue& v : result.value("tools").toArray()) {
            const QJsonObject t = v.toObject();
            collected.append({t.value("name").toString(), t.value("description").toString(), t.value("inputSchema").toObject()});
        }
        const QString next = result.value("nextCursor").toString();
        if (!next.isEmpty()) {
            listTools(next, collected);
            return;
        }
        m_tools = collected;
        setState(State::Connected);
    });
}

void LC_MCPClient::callTool(const QString& name, const QJsonObject& arguments, const Callback& callback) {
    if (m_state != State::Connected) {
        callback({}, tr("The MCP server \"%1\" is not connected (%2).").arg(m_config.name, stateText()));
        return;
    }
    request("tools/call", QJsonObject{{"name", name}, {"arguments", arguments}}, callback);
}
