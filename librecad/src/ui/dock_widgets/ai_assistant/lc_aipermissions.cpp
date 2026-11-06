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
#include "lc_aitool.h"
#include "rs_entity.h"
#include "rs_graphic.h"
#include "rs_graphicview.h"
#include "rs_layer.h"

namespace {
    const QString MODE_KEY = QStringLiteral("approvalMode");
    const QString PERMISSIONS_KEY = QStringLiteral("toolPermissions");

    // tools that only look at the drawing or move the view: allowed by default
    const QSet<QString> READ_ONLY = {"capture_view", "get_view", "zoom_to_fit", "zoom_to_area", "zoom", "pan",
                                     "get_drawing_info", "list_entities", "list_commands", "view_image_file",
                                     "open_drawing", "request_clarification", "list_toolbar_buttons",
                                     // follow-ups of a button the user already let the AI press; a modal dialog
                                     // would also block the approval box
                                     "handle_dialog", "select_entities", "look_at_app",
                                     "check_drawing"}; // the drawing checker only reads
    // tools that change the drawing: asked by default, approved by "Auto-accept edits"
    const QSet<QString> EDIT_TOOLS = {"run_command", "add_text", "press_toolbar_button", "click_drawing", "set_toolbar_value",
                                      "tool_options", "use_panel", "use_menu", "context_menu", "press_in_app", "use_properties",
                                      "fix_drawing_issues", // the drawing checker's safe fixes
                                      "layers"};

    /** Calls of edit tools that only read (describe a panel, list the menus): they need no approval. */
    bool onlyReads(const QString& toolName, const QJsonObject& arguments) {
        if (toolName == "use_panel") {
            return !arguments.contains("fields") && !arguments.contains("item") && !arguments.contains("keys")
                   && !arguments.contains("button");
        }
        if (toolName == "use_menu") {
            return arguments.value("path").toString().trimmed().isEmpty();
        }
        if (toolName == "layers") {
            const QString action = arguments.value("action").toString("list").trimmed().toLower();
            return action.isEmpty() || action == "list";
        }
        return false;
    }

    /** The live entities of the drawing the AI works on, and how many of them are selected. */
    void drawingCounts(int& all, int& selected) {
        all = 0;
        selected = 0;
        RS_GraphicView* view = LC_AITools::view();
        RS_Graphic* graphic = view != nullptr ? view->getGraphic() : nullptr;
        if (graphic == nullptr) {
            return;
        }
        for (RS_Entity* e : *graphic) {
            if (e != nullptr && !e->isDeleted()) {
                ++all;
                selected += e->isSelected() ? 1 : 0;
            }
        }
    }

    /**
     * A call that would delete most of the drawing (at least 20 entities and at least half of it) asks in every mode,
     * Full access too: a run that "starts over" should not wipe the user's work without a word.
     */
    QString wipeRule(const QString& toolName, const QJsonObject& arguments) {
        constexpr int AT_LEAST = 20;
        bool deletes = false;
        if (toolName == "press_toolbar_button") {
            static const QRegularExpression ids(QStringLiteral("^(ModifyDelete\\w*|EditCut\\w*)$"), QRegularExpression::CaseInsensitiveOption);
            deletes = ids.match(arguments.value("id").toString().trimmed()).hasMatch();
        }
        else if (toolName == "run_command") {
            static const QRegularExpression words(QStringLiteral("^(moddelete|er|del|erase|delete|cut)$"), QRegularExpression::CaseInsensitiveOption);
            for (const QJsonValue& v : arguments.value("commands").toArray()) {
                deletes = deletes || words.match(v.toString().trimmed()).hasMatch();
            }
        }
        if (deletes) {
            int all = 0;
            int selected = 0;
            drawingCounts(all, selected);
            if (selected >= AT_LEAST && selected * 2 >= all) {
                return QStringLiteral("Safety rule: deletes most of the drawing (%1 of %2 entities)").arg(selected).arg(all);
            }
            return {};
        }
        if (toolName == "layers" && arguments.value("action").toString().trimmed().toLower() == "remove"
            && arguments.value("entities").toString().trimmed().compare("delete", Qt::CaseInsensitive) == 0) {
            RS_GraphicView* view = LC_AITools::view();
            RS_Graphic* graphic = view != nullptr ? view->getGraphic() : nullptr;
            if (graphic == nullptr) {
                return {};
            }
            QStringList names;
            for (const QJsonValue& v : arguments.value("names").toArray()) {
                names << v.toString().trimmed().toLower();
            }
            names << arguments.value("name").toString().trimmed().toLower();
            int all = 0;
            int onThem = 0;
            for (RS_Entity* e : *graphic) {
                if (e != nullptr && !e->isDeleted()) {
                    ++all;
                    onThem += e->getLayer() != nullptr && names.contains(e->getLayer()->getName().toLower()) ? 1 : 0;
                }
            }
            if (onThem >= AT_LEAST && onThem * 2 >= all) {
                return QStringLiteral("Safety rule: deletes most of the drawing with these layers (%1 of %2 entities)")
                    .arg(onThem).arg(all);
            }
        }
        return {};
    }

    /** Calls of an edit tool that only look: reading the Properties panel, folding it, choosing which type it shows. */
    bool onlyLooks(const QString& toolName, const QJsonObject& arguments) {
        if (toolName != "use_properties") {
            return false;
        }
        const QString action = arguments.value("action").toString("read").trimmed().toLower();
        return action.isEmpty() || action == "read" || action == "collapse" || action == "expand" || action == "choose_type";
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
        if (toolName == "layers") {
            if (arguments.value("action").toString().trimmed().toLower() == "remove"
                && arguments.value("entities").toString().trimmed().compare("delete", Qt::CaseInsensitive) == 0) {
                return QStringLiteral("Safety rule: removes layers and deletes their entities (a removed layer cannot come back)");
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
    QString rule = m == Mode::Full ? QString() : safetyRule(toolName, arguments); // Full access asks for nothing...
    if (const QString wipe = wipeRule(toolName, arguments); !wipe.isEmpty()) {
        rule = wipe; // ...except before deleting most of the drawing
    }
    if (p == Permission::Ask && !hasSavedPermission(toolName) && onlyLooks(toolName, arguments)) {
        p = Permission::Allow; // a look at the panel is not an edit (unless the user asked to be asked)
    }
    // the approval mode can only loosen "ask"
    if (p == Permission::Ask && (m == Mode::Auto || m == Mode::Full || (m == Mode::Edits && isEditTool(toolName)))) {
        p = Permission::Allow;
    }
    if (!rule.isEmpty()) {
        d.permission = Permission::Ask;
        d.reason = rule;
        d.ruleAsked = true;
        d.key = rule.contains("most of the drawing") ? QStringLiteral("rule:wipe")
                : toolName == "press_toolbar_button" ? "rule:button:" + arguments.value("id").toString()
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
