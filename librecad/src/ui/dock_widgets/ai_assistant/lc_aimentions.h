/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIMENTIONS_H
#define LC_AIMENTIONS_H

#include <QList>
#include <QString>

#include "lc_aiattachments.h"

class QMenu;
class QG_GraphicView;
class RS_Entity;
struct LC_AITheme;

/**
 * An @-mention in a message to the AI: a part of the drawing it is about. When the message is sent, each one
 * becomes context for the model (the entities it stands for, read from the drawing at that moment).
 */
struct LC_AIMention {
    enum class Kind { Selection, View, Drawing, Layer, Block, Entity };
    Kind kind {Kind::Selection};
    QString argument;  // the layer or block name, or the entity id
    int start {0};     // where it is in the message
    int length {0};
    QString text() const { return text(kind, argument); }
    /** "@selection", "@layer Walls", "@entity #42". */
    static QString text(Kind kind, const QString& argument = {});
};

namespace LC_AIMentions {
    /**
     * The mentions in a message: @selection, @view, @drawing, @entity #id, and @layer / @block followed by the
     * name of a layer or block of the open drawing (names may contain spaces).
     */
    QList<LC_AIMention> find(const QString& text);

    /** One row of the @ popup. */
    struct Candidate {
        QString text;    // what is inserted, e.g. "@layer Walls"
        QString detail;  // e.g. "12 entities · current"
        QString icon;    // lucide icon name
        QString group;   // "Context", "Layers", "Blocks", "Entities"
    };
    /** What the @ popup offers for the text typed after "@": the context mentions, then layers, blocks and entities. */
    QList<Candidate> candidates(const QString& query);

    struct Expansion {
        QString context;                     // added after the user's text, for the model only
        QList<LC_AIAttachment> attachments;  // the @view screenshot
    };
    /** What the model gets for the mentions, read from the drawing now. */
    Expansion expand(const QList<LC_AIMention>& mentions);

    /** The message as HTML with its mentions highlighted, or empty when it has none (the bubble in the chat). */
    QString toHtml(const QString& text, const LC_AITheme& theme);

    /**
     * Adds "Ask AI about this" to the drawing's context menu when the window has the AI sidebar: on an entity it
     * selects it and starts a message with @entity #id; on empty space with @view.
     */
    void addAskAI(QMenu* menu, QG_GraphicView* view, RS_Entity* entity);
}

#endif
