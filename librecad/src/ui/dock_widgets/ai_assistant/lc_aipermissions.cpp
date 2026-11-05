/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aipermissions.h"

#include <QJsonArray>
#include <QObject>
#include <QRegularExpression>

#include "lc_aicredentials.h"

namespace {
    const QString MODE_KEY = QStringLiteral("approvalMode");
    const QString PERMISSIONS_KEY = QStringLiteral("toolPermissions");

    // tools that only look at the drawing or move the view: allowed by default
    const QSet<QString> READ_ONLY = {"capture_view", "get_view", "zoom_to_fit", "zoom_to_area", "zoom", "pan",
                                     "get_drawing_info", "list_entities", "list_commands", "view_image_file",
                                     "open_drawing", "request_clarification", "list_toolbar_buttons",
                                     // follow-ups of a button the user already let the AI press; a modal dialog
                                     // would also block the approval box
                                     "handle_dialog", "select_entities", "look_at_app"};
    // tools that change the drawing: asked by default, approved by "Auto-accept edits"
    const QSet<QString> EDIT_TOOLS = {"run_command", "add_text", "press_toolbar_button", "click_drawing", "set_toolbar_value",
                                      "tool_options", "use_panel", "use_menu", "context_menu", "press_in_app"};

    /** Calls of edit tools that only read (describe a panel, list the menus): they need no approval. */
    bool onlyReads(const QString& toolName, const QJsonObject& arguments) {
        if (toolName == "use_panel") {
            return !arguments.contains("fields") && !arguments.contains("item") && !arguments.contains("keys")
                   && !arguments.contains("button");
        }
        if (toolName == "use_menu") {
            return arguments.value("path").toString().trimmed().isEmpty();
        }
        return false;
    }

    QString permissionText(const LC_AIPermissions::Permission p) {
        switch (p) {
            case LC_AIPermissions::Permission::Allow:
                return "allow";
            case LC_AIPermissions::Permission::Ask:
                return "ask";
            case LC_AIPermissions::Permission::Deny:
                return "deny";
        }
        return "ask";
    }

    /** Built-in safety rule: deleting entities asks even in Auto mode (Cypher's "ask" guardrails). */
    QString safetyRule(const QString& toolName, const QJsonObject& arguments) {
        if (toolName == "handle_dialog" || toolName == "use_panel") {
            // Application Preferences: wiping or replacing all settings, or the window layout
            static const QStringList wipes = {"layout", "all", "clear settings", "import settings from file", "export settings to file",
                                              "reset all"};
            const QString button = arguments.value("button").toString().trimmed().toLower().remove('&');
            if (wipes.contains(button)) {
                return QStringLiteral("Safety rule: \"%1\" clears or replaces settings").arg(arguments.value("button").toString());
            }
            return {};
        }
        if (toolName == "press_toolbar_button") {
            // buttons that delete entities, or close, write or print files, ask even in Auto mode
            static const QRegularExpression risky(QStringLiteral("^(ModifyDelete\\w*|ModifyDeleteQuick|EditCut|LayersRemove|"
                                                                 "BlocksRemove|FileClose\\w*|FileSave\\w*|FileExport\\w*|"
                                                                 "FilePrint|FilePrintPDF|BlocksSave)$"),
                                                  QRegularExpression::CaseInsensitiveOption);
            const QString id = arguments.value("id").toString().trimmed();
            if (risky.match(id).hasMatch()) {
                return QStringLiteral("Safety rule: \"%1\" deletes, closes or writes files").arg(id);
            }
            return {};
        }
        if (toolName != "run_command") {
            return {};
        }
        static const QRegularExpression deletes(QStringLiteral("^(moddelete|er|del|erase|delete|plinedel|pd)$"),
                                                QRegularExpression::CaseInsensitiveOption);
        QStringList lines;
        for (const QJsonValue& v : arguments.value("commands").toArray()) {
            lines << v.toString();
        }
        if (lines.isEmpty()) {
            lines << arguments.value("commands").toString();
        }
        for (const QString& line : std::as_const(lines)) {
            if (deletes.match(line.trimmed()).hasMatch()) {
                return QStringLiteral("Safety rule: deletes entities (\"%1\")").arg(line.trimmed());
            }
        }
        return {};
    }
}

LC_AIPermissions::Mode LC_AIPermissions::mode() {
    const QString m = LC_AICredentials::instance().setting(MODE_KEY).toString();
    if (m == "edits") {
        return Mode::Edits;
    }
    if (m == "auto") {
        return Mode::Auto;
    }
    if (m == "full") {
        return Mode::Full;
    }
    return Mode::Supervised;
}

void LC_AIPermissions::setMode(const Mode mode) {
    const char* names[] = {"supervised", "edits", "auto", "full"};
    LC_AICredentials::instance().setSetting(MODE_KEY, QString::fromLatin1(names[int(mode)]));
}

QString LC_AIPermissions::modeName(const Mode mode) {
    switch (mode) {
        case Mode::Supervised:
            return QObject::tr("Supervised");
        case Mode::Edits:
            return QObject::tr("Auto-accept edits");
        case Mode::Auto:
            return QObject::tr("Auto");
        case Mode::Full:
            return QObject::tr("Full access");
    }
    return {};
}

QString LC_AIPermissions::modeShortName(const Mode mode) {
    switch (mode) {
        case Mode::Supervised:
            return QObject::tr("Supervised");
        case Mode::Edits:
            return QObject::tr("Edits");
        case Mode::Auto:
            return QObject::tr("Auto");
        case Mode::Full:
            return QObject::tr("Full access");
    }
    return {};
}

QString LC_AIPermissions::modeHint(const Mode mode) {
    switch (mode) {
        case Mode::Supervised:
            return QObject::tr("Ask before changing the drawing and before MCP tools, as set per tool.");
        case Mode::Edits:
            return QObject::tr("Approve drawing edits (commands, text); ask before other actions.");
        case Mode::Auto:
            return QObject::tr("Approve routine actions; ask when a safety rule flags one (e.g. deleting).");
        case Mode::Full:
            return QObject::tr("Run everything without asking. Tools set to Deny stay denied.");
    }
    return {};
}

QString LC_AIPermissions::modeIcon(const Mode mode) {
    switch (mode) {
        case Mode::Supervised:
            return "lock";
        case Mode::Edits:
            return "pencil";
        case Mode::Auto:
            return "sparkles";
        case Mode::Full:
            return "lock-open";
    }
    return "lock";
}

bool LC_AIPermissions::isEditTool(const QString& toolName) {
    return EDIT_TOOLS.contains(toolName);
}

LC_AIPermissions::Permission LC_AIPermissions::defaultPermission(const QString& toolName) {
    // browser tools only look things up in OpenCAD's own window (Cypher keeps them out of tool approval too)
    return READ_ONLY.contains(toolName) || toolName.startsWith("browser_") ? Permission::Allow : Permission::Ask; // edits and MCP tools ask
}

LC_AIPermissions::Permission LC_AIPermissions::permission(const QString& toolName) {
    const QString saved = LC_AICredentials::instance().setting(PERMISSIONS_KEY).toObject().value(toolName).toString();
    if (saved == "allow") {
        return Permission::Allow;
    }
    if (saved == "ask") {
        return Permission::Ask;
    }
    if (saved == "deny") {
        return Permission::Deny;
    }
    return defaultPermission(toolName);
}

bool LC_AIPermissions::hasSavedPermission(const QString& toolName) {
    return LC_AICredentials::instance().setting(PERMISSIONS_KEY).toObject().contains(toolName);
}

void LC_AIPermissions::setPermission(const QString& toolName, const Permission permission) {
    QJsonObject saved = LC_AICredentials::instance().setting(PERMISSIONS_KEY).toObject();
    saved.insert(toolName, permissionText(permission));
    LC_AICredentials::instance().setSetting(PERMISSIONS_KEY, saved);
}

void LC_AIPermissions::resetPermission(const QString& toolName) {
    QJsonObject saved = LC_AICredentials::instance().setting(PERMISSIONS_KEY).toObject();
    saved.remove(toolName);
    LC_AICredentials::instance().setSetting(PERMISSIONS_KEY, saved);
}

LC_AIPermissions::Decision LC_AIPermissions::decide(const QString& toolName, const QJsonObject& arguments,
                                                    const QSet<QString>& sessionAllows) {
    Decision d;
    const Mode m = mode();
    Permission p = permission(toolName);
    if (p != Permission::Deny && onlyReads(toolName, arguments)) {
        p = Permission::Allow;
    }
    if (p == Permission::Deny) {
        d.permission = Permission::Deny;
        d.reason = QObject::tr("Tool \"%1\" is denied by the user's tool permissions.").arg(toolName);
        return d;
    }
    const QString rule = m == Mode::Full ? QString() : safetyRule(toolName, arguments); // Full access asks for nothing
    // the approval mode can only loosen "ask"
    if (p == Permission::Ask && (m == Mode::Auto || m == Mode::Full || (m == Mode::Edits && isEditTool(toolName)))) {
        p = Permission::Allow;
    }
    if (!rule.isEmpty()) {
        d.permission = Permission::Ask;
        d.reason = rule;
        d.ruleAsked = true;
        d.key = toolName == "press_toolbar_button" ? "rule:button:" + arguments.value("id").toString()
                : toolName == "run_command" ? QStringLiteral("rule:delete") : "rule:" + toolName + ":" + rule;
    }
    else {
        d.permission = p;
        d.key = "tool:" + toolName;
    }
    if (d.permission == Permission::Ask && sessionAllows.contains(d.key)) {
        d.permission = Permission::Allow; // "Allow for this session"
    }
    return d;
}
