/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_MCPMANAGER_H
#define LC_MCPMANAGER_H

#include <QList>
#include <QObject>

#include "lc_aitool.h"
#include "lc_mcpclient.h"

/**
 * The MCP servers of OpenCAD, configured once for the whole application (not per drawing) in
 * mcp.json next to ai-auth.json, in the "mcpServers" shape Cypher and Claude Desktop use. Their tools
 * are offered to the AI as "mcp__<server>__<tool>" and act on whatever drawing is active.
 */
class LC_MCPManager : public QObject {
    Q_OBJECT
public:
    static LC_MCPManager& instance();

    QString configPath() const;
    QList<LC_MCPServerConfig> servers() const;
    LC_MCPClient* client(const QString& name) const;

    /** The raw file, for the JSON view. */
    QString configText() const;
    /** Replaces the whole config; returns an error message, or an empty string. */
    QString setConfigText(const QString& text);
    /** Adds or replaces one server; returns an error message, or an empty string. */
    QString saveServer(const LC_MCPServerConfig& config, const QString& previousName = {});
    void removeServer(const QString& name);
    void setEnabled(const QString& name, bool enabled);
    /** Merges the "mcpServers" of another config file (Claude Desktop, .mcp.json, Cypher); returns the names added or an error. */
    QString importFile(const QString& path, QStringList& added);

    void connectAll();          // enabled servers that are not connected yet
    void reconnect(const QString& name);
    /** Tools of the connected servers, ready for the AI. */
    QList<LC_AITool> aiTools() const;
    static QString toolName(const QString& server, const QString& tool);

signals:
    void changed(); // servers, states or tools changed

private:
    LC_MCPManager();
    void load();
    QString write(const QJsonObject& servers);
    void syncClients();

    QJsonObject m_servers; // "mcpServers" as in the file
    QList<LC_MCPClient*> m_clients;
};

#endif
