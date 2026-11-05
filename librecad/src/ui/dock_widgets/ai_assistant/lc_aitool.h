/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AITOOL_H
#define LC_AITOOL_H

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

/**
 * Tools the AI assistant can call, shaped like Cypher's AgentTool (packages/agent/src/types.ts):
 * a name, a label for the UI, a description for the model, JSON-schema parameters, and execute(),
 * which returns text and image content for the model plus an error flag.
 */
struct LC_AIContent {
    enum class Type { Text, Image };
    Type type {Type::Text};
    QString text;          // Text
    QByteArray data;       // Image: raw bytes (sent base64-encoded)
    QString mimeType;      // Image: e.g. "image/png"

    static LC_AIContent makeText(const QString& text) { return {Type::Text, text, {}, {}}; }
    static LC_AIContent makeImage(const QByteArray& data, const QString& mimeType) {
        return {Type::Image, {}, data, mimeType};
    }
};

struct LC_AIToolResult {
    QList<LC_AIContent> content;
    bool isError {false};

    static LC_AIToolResult text(const QString& text) { return {{LC_AIContent::makeText(text)}, false}; }
    static LC_AIToolResult error(const QString& text) { return {{LC_AIContent::makeText(text)}, true}; }
    /** All text blocks joined (what text-only APIs receive). */
    QString joinedText() const {
        QStringList parts;
        for (const LC_AIContent& c : content) {
            if (c.type == LC_AIContent::Type::Text) {
                parts << c.text;
            }
        }
        return parts.join("\n");
    }
};

struct LC_AITool {
    QString name;          // what the model calls, e.g. "capture_view"
    QString label;         // shown in the chat, e.g. "Capture view"
    QString description;   // tells the model when and how to use it
    QJsonObject parameters; // JSON schema of the arguments (type "object")
    std::function<LC_AIToolResult(const QJsonObject& args)> execute;
    /** Tools that answer later (MCP servers): call done exactly once with the result. Used instead of execute. */
    std::function<void(const QJsonObject& args, std::function<void(const LC_AIToolResult&)> done)> executeAsync;
};

/** The CAD tools (view, drawing, command line) that act on OpenCAD's current drawing. */
class RS_Entity;
class RS_GraphicView;
namespace LC_AITools {
    QList<LC_AITool> all();
    /** Tests only: act on this view instead of the main window's current one (nullptr: back to normal). */
    void setViewForTests(RS_GraphicView* view);

    // shared with the other tool sets
    /** The view of the drawing the tools act on, or nullptr when no drawing is open. */
    RS_GraphicView* view();
    /** A PNG screenshot of the view, at most 1280 px on its longer edge. */
    LC_AIContent screenshot(RS_GraphicView* view);
    /** One line with the entity's id, type, layer and exact geometry, as list_entities prints it. */
    QString describeEntity(RS_Entity* entity);
    /** The visible area and the drawing extents. */
    QString describeView(RS_GraphicView* view);
    /** The type name list_entities uses ("line", "circle", "dim_linear", ...). */
    QString typeName(const RS_Entity* entity);
}

#endif
