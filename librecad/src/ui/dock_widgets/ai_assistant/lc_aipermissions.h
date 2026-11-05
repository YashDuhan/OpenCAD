/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIPERMISSIONS_H
#define LC_AIPERMISSIONS_H

#include <QJsonObject>
#include <QSet>
#include <QString>

/**
 * When the AI may run a tool without asking, ported from Cypher (apps/desktop/extension/gui-bridge.ts):
 * every tool has a permission (allow, ask, deny), the approval mode can loosen "ask", safety rules ask
 * even for allowed tools, and Deny holds in every mode. Kept in ai-auth.json next to the keys.
 */
namespace LC_AIPermissions {
    enum class Mode { Supervised, Edits, Auto, Full };
    enum class Permission { Allow, Ask, Deny };

    struct Decision {
        Permission permission {Permission::Allow};
        QString reason;   // why it asks (a safety rule), shown in the approval box
        QString key;      // what "Allow for this session" remembers
        bool ruleAsked {false}; // asked by a safety rule: "Allow always" is not offered
    };

    Mode mode();
    void setMode(Mode mode);
    QString modeName(Mode mode);
    QString modeShortName(Mode mode); // for the composer button
    QString modeHint(Mode mode);
    QString modeIcon(Mode mode);

    /** The saved permission, else the default for that kind of tool. */
    Permission permission(const QString& toolName);
    Permission defaultPermission(const QString& toolName);
    void setPermission(const QString& toolName, Permission permission);
    void resetPermission(const QString& toolName);
    bool hasSavedPermission(const QString& toolName);
    /** Tools that change the drawing: what "Auto-accept edits" approves. */
    bool isEditTool(const QString& toolName);

    /** What to do with one call, given the mode, the tool's permission and the safety rules. */
    Decision decide(const QString& toolName, const QJsonObject& arguments, const QSet<QString>& sessionAllows);
}

#endif
