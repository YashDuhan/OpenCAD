/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_MCPCLIENT_H
#define LC_MCPCLIENT_H

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QTimer;

/**
 * One entry of the "mcpServers" config (the shape Cypher, Claude Desktop and UniCAD's docs use):
 * a local command started over stdio, or a Streamable HTTP URL.
 */
struct LC_MCPServerConfig {
    QString name;
    bool http {false};               // "type": "http" / has "url"
    QString command;                 // stdio
    QStringList args;
    QMap<QString, QString> env;
    QString cwd;
    QString url;                     // http
    QMap<QString, QString> headers;
    bool enabled {true};
    QString description;
    int timeoutSeconds {300};        // per request; long, since drawing renders can take a while

    static LC_MCPServerConfig fromJson(const QString& name, const QJsonObject& json);
    QJsonObject toJson() const;
    /** "uvx --from ... unicad-mcp" or the URL, for display. */
    QString target() const;
};

/**
 * MCP client for one server (port of Cypher's packages/mcp): JSON-RPC 2.0 over stdio (newline-delimited
 * messages to a child process) or Streamable HTTP (POST, JSON or SSE answers, Mcp-Session-Id).
 * Connecting runs initialize, notifications/initialized and tools/list.
 */
class LC_MCPClient : public QObject {
    Q_OBJECT
public:
    enum class State { Disconnected, Connecting, Connected, Failed };
    struct Tool {
        QString name;
        QString description;
        QJsonObject inputSchema;
    };
    using Callback = std::function<void(const QJsonObject& result, const QString& error)>;

    explicit LC_MCPClient(const LC_MCPServerConfig& config, QObject* parent = nullptr);
    ~LC_MCPClient() override;

    const LC_MCPServerConfig& config() const { return m_config; }
    State state() const { return m_state; }
    QString stateText() const;
    QString error() const { return m_error; }
    QString stderrTail() const { return m_stderr; }
    QString serverInfo() const { return m_serverInfo; }
    const QList<Tool>& tools() const { return m_tools; }

    void connectToServer();
    void disconnectFromServer();
    /** tools/call; the callback gets the MCP result object ({content, isError, structuredContent}) or an error. */
    void callTool(const QString& name, const QJsonObject& arguments, const Callback& callback);

signals:
    void changed();  // state, error or tools changed

private:
    struct Pending {
        Callback callback;
        QTimer* timer {nullptr};
    };

    void setState(State state, const QString& error = {});
    void request(const QString& method, const QJsonObject& params, const Callback& callback);
    void notify(const QString& method, const QJsonObject& params = {});
    void send(const QJsonObject& message);
    void handleMessage(const QJsonObject& message);
    void resolve(const QJsonValue& id, const QJsonObject& result, const QString& error);
    void failAll(const QString& error);
    void initialize();
    void listTools(const QString& cursor = {}, QList<Tool> collected = {});

    // stdio
    void startProcess();
    void onStdout();
    // http
    void post(const QJsonObject& message);

    LC_MCPServerConfig m_config;
    State m_state {State::Disconnected};
    QString m_error;
    QString m_stderr;
    QString m_serverInfo;
    QString m_protocolVersion;
    QList<Tool> m_tools;
    qint64 m_nextId {1};
    QHash<qint64, Pending> m_pending;
    int m_connection {0}; // bumped per connect, so callbacks of an old connection are ignored

    QProcess* m_process {nullptr};
    QByteArray m_stdoutBuffer;
    QNetworkAccessManager* m_network {nullptr};
    QString m_sessionId;
};

#endif
