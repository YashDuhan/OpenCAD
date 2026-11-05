/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_mcpmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QPointer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace {
    const QString MCP_SERVERS = QStringLiteral("mcpServers");
    constexpr int MAX_TOOL_NAME = 64; // OpenAI and Gemini limit

    bool validName(const QString& name) {
        static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,40}$"));
        return re.match(name).hasMatch();
    }

    /** MCP content blocks -> what the model receives. */
    LC_AIToolResult convertResult(const QJsonObject& result) {
        LC_AIToolResult out;
        out.isError = result.value("isError").toBool();
        for (const QJsonValue& v : result.value("content").toArray()) {
            const QJsonObject c = v.toObject();
            const QString type = c.value("type").toString();
            if (type == "text") {
                out.content << LC_AIContent::makeText(c.value("text").toString());
            }
            else if (type == "image") {
                out.content << LC_AIContent::makeImage(QByteArray::fromBase64(c.value("data").toString().toLatin1()),
                                                       c.value("mimeType").toString("image/png"));
            }
            else if (type == "resource") {
                const QJsonObject r = c.value("resource").toObject();
                const QString text = r.value("text").toString();
                out.content << LC_AIContent::makeText(QStringLiteral("[resource %1]%2").arg(
                    r.value("uri").toString(), text.isEmpty() ? QStringLiteral(" (binary, not shown)") : "\n" + text));
            }
            else if (type == "resource_link") {
                out.content << LC_AIContent::makeText(QStringLiteral("[link] %1 %2").arg(c.value("name").toString(), c.value("uri").toString()));
            }
            else if (!type.isEmpty()) {
                out.content << LC_AIContent::makeText(QStringLiteral("[%1 content, not shown]").arg(type));
            }
        }
        const QJsonValue structured = result.value("structuredContent");
        if (out.joinedText().trimmed().isEmpty() && (structured.isObject() || structured.isArray())) {
            const QJsonDocument doc = structured.isObject() ? QJsonDocument(structured.toObject()) : QJsonDocument(structured.toArray());
            out.content.prepend(LC_AIContent::makeText(QString::fromUtf8(doc.toJson(QJsonDocument::Compact))));
        }
        if (out.content.isEmpty()) {
            out.content << LC_AIContent::makeText(QStringLiteral("(no output)"));
        }
        return out;
    }
}

LC_MCPManager& LC_MCPManager::instance() {
    static auto* manager = new LC_MCPManager(); // lives as long as the application (servers are stopped at exit)
    return *manager;
}

LC_MCPManager::LC_MCPManager() {
    load();
    // stop the servers OpenCAD started when it quits
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        for (LC_MCPClient* c : std::as_const(m_clients)) {
            c->disconnectFromServer();
        }
    });
}

QString LC_MCPManager::configPath() const {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)).filePath("mcp.json");
}

void LC_MCPManager::load() {
    QFile file(configPath());
    m_servers = QJsonObject();
    if (file.open(QIODevice::ReadOnly)) {
        m_servers = QJsonDocument::fromJson(file.readAll()).object().value(MCP_SERVERS).toObject();
    }
    syncClients();
}

QString LC_MCPManager::write(const QJsonObject& servers) {
    const QString path = configPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return tr("Could not write %1: %2").arg(path, file.errorString());
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner); // headers and env can hold tokens
    file.write(QJsonDocument(QJsonObject{{MCP_SERVERS, servers}}).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        return tr("Could not write %1: %2").arg(path, file.errorString());
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    m_servers = servers;
    syncClients();
    return {};
}

void LC_MCPManager::syncClients() {
    // drop clients whose server was removed or changed; keep the others connected
    for (int i = m_clients.size() - 1; i >= 0; --i) {
        LC_MCPClient* c = m_clients[i];
        const QJsonValue entry = m_servers.value(c->config().name);
        if (!entry.isObject() || LC_MCPServerConfig::fromJson(c->config().name, entry.toObject()).toJson() != c->config().toJson()) {
            m_clients.removeAt(i);
            c->disconnect(this);
            c->deleteLater(); // its destructor stops the server
        }
    }
    for (auto it = m_servers.begin(); it != m_servers.end(); ++it) {
        if (client(it.key()) == nullptr) {
            auto* c = new LC_MCPClient(LC_MCPServerConfig::fromJson(it.key(), it.value().toObject()), this);
            connect(c, &LC_MCPClient::changed, this, &LC_MCPManager::changed);
            m_clients << c;
        }
    }
    emit changed();
}

QList<LC_MCPServerConfig> LC_MCPManager::servers() const {
    QList<LC_MCPServerConfig> list;
    for (auto it = m_servers.begin(); it != m_servers.end(); ++it) {
        list << LC_MCPServerConfig::fromJson(it.key(), it.value().toObject());
    }
    return list;
}

LC_MCPClient* LC_MCPManager::client(const QString& name) const {
    for (LC_MCPClient* c : m_clients) {
        if (c->config().name == name) {
            return c;
        }
    }
    return nullptr;
}

QString LC_MCPManager::configText() const {
    return QString::fromUtf8(QJsonDocument(QJsonObject{{MCP_SERVERS, m_servers}}).toJson(QJsonDocument::Indented));
}

QString LC_MCPManager::setConfigText(const QString& text) {
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return tr("Not valid JSON: %1").arg(error.errorString());
    }
    const QJsonValue servers = doc.object().value(MCP_SERVERS);
    if (!servers.isObject()) {
        return tr("The file needs an \"mcpServers\" object.");
    }
    const QJsonObject serverObjects = servers.toObject();
    for (auto it = serverObjects.begin(); it != serverObjects.end(); ++it) {
        if (!validName(it.key())) {
            return tr("Invalid server name \"%1\": use letters, digits, \"_\" and \"-\".").arg(it.key());
        }
        const QJsonObject s = it.value().toObject();
        if (s.value("command").toString().isEmpty() && s.value("url").toString().isEmpty()) {
            return tr("Server \"%1\" needs a \"command\" or a \"url\".").arg(it.key());
        }
    }
    const QString result = write(servers.toObject());
    if (result.isEmpty()) {
        connectAll();
    }
    return result;
}

QString LC_MCPManager::saveServer(const LC_MCPServerConfig& config, const QString& previousName) {
    if (!validName(config.name)) {
        return tr("Use a name of letters, digits, \"_\" and \"-\" (up to 40).");
    }
    if (config.http ? !config.url.startsWith("http") : config.command.trimmed().isEmpty()) {
        return config.http ? tr("Enter a URL starting with http:// or https://") : tr("Enter the command to start the server.");
    }
    QJsonObject servers = m_servers;
    if (!previousName.isEmpty() && previousName != config.name) {
        servers.remove(previousName);
    }
    else if (previousName.isEmpty() && servers.contains(config.name)) {
        return tr("A server named \"%1\" exists already.").arg(config.name);
    }
    servers.insert(config.name, config.toJson());
    const QString result = write(servers);
    if (result.isEmpty() && config.enabled) {
        reconnect(config.name);
    }
    return result;
}

void LC_MCPManager::removeServer(const QString& name) {
    QJsonObject servers = m_servers;
    servers.remove(name);
    write(servers);
}

void LC_MCPManager::setEnabled(const QString& name, const bool enabled) {
    QJsonObject servers = m_servers;
    QJsonObject entry = servers.value(name).toObject();
    if (enabled) {
        entry.remove("enabled");
    }
    else {
        entry.insert("enabled", false);
    }
    servers.insert(name, entry);
    write(servers);
    if (enabled) {
        reconnect(name);
    }
}

QString LC_MCPManager::importFile(const QString& path, QStringList& added) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return tr("Could not read %1: %2").arg(path, file.errorString());
    }
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return tr("%1 is not valid JSON: %2").arg(QFileInfo(path).fileName(), error.errorString());
    }
    QJsonObject incoming = doc.object().value(MCP_SERVERS).toObject();
    if (incoming.isEmpty() && (doc.object().contains("command") || doc.object().contains("url"))) {
        incoming.insert(QFileInfo(path).completeBaseName(), doc.object()); // a single server entry
    }
    if (incoming.isEmpty()) {
        return tr("%1 has no \"mcpServers\" to import.").arg(QFileInfo(path).fileName());
    }
    QJsonObject servers = m_servers;
    for (auto it = incoming.begin(); it != incoming.end(); ++it) {
        QString name = it.key();
        name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")), "_");
        QString unique = name;
        for (int n = 2; servers.contains(unique); ++n) {
            unique = QStringLiteral("%1-%2").arg(name).arg(n);
        }
        servers.insert(unique, it.value());
        added << unique;
    }
    const QString result = write(servers);
    if (result.isEmpty()) {
        connectAll();
    }
    return result;
}

void LC_MCPManager::connectAll() {
    for (LC_MCPClient* c : std::as_const(m_clients)) {
        if (c->config().enabled && (c->state() == LC_MCPClient::State::Disconnected)) {
            c->connectToServer();
        }
    }
}

void LC_MCPManager::reconnect(const QString& name) {
    if (LC_MCPClient* c = client(name)) {
        if (c->config().enabled) {
            c->connectToServer();
        }
        else {
            c->disconnectFromServer();
        }
    }
}

QString LC_MCPManager::toolName(const QString& server, const QString& tool) {
    QString name = QStringLiteral("mcp__%1__%2").arg(server, tool);
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")), "_");
    return name.left(MAX_TOOL_NAME);
}

QList<LC_AITool> LC_MCPManager::aiTools() const {
    QList<LC_AITool> tools;
    for (LC_MCPClient* c : m_clients) {
        if (c->state() != LC_MCPClient::State::Connected) {
            continue;
        }
        const QString server = c->config().name;
        for (const LC_MCPClient::Tool& t : c->tools()) {
            LC_AITool tool;
            tool.name = toolName(server, t.name);
            tool.label = server + ": " + t.name;
            tool.description = QStringLiteral("[MCP server \"%1\"] %2").arg(server, t.description);
            tool.parameters = t.inputSchema.isEmpty() ? QJsonObject{{"type", "object"}, {"properties", QJsonObject()}} : t.inputSchema;
            if (!tool.parameters.contains("type")) {
                tool.parameters.insert("type", "object");
            }
            const QString mcpName = t.name;
            QPointer<LC_MCPClient> client(c);
            tool.executeAsync = [client, mcpName](const QJsonObject& args, std::function<void(const LC_AIToolResult&)> done) {
                if (!client) {
                    done(LC_AIToolResult::error(QStringLiteral("The MCP server was removed.")));
                    return;
                }
                client->callTool(mcpName, args, [done](const QJsonObject& result, const QString& error) {
                    done(error.isEmpty() ? convertResult(result) : LC_AIToolResult::error(error));
                });
            };
            tools << tool;
        }
    }
    return tools;
}
