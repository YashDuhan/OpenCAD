/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIDRAWINGCHECKER_H
#define LC_AIDRAWINGCHECKER_H

#include <QList>
#include <QString>
#include <QStringList>

#include "lc_aitool.h"
#include "rs_vector.h"

class QAction;
class QWidget;
class RS_Graphic;

/**
 * The drawing checker: finds what a careful reviewer would flag in a drawing (zero-length and duplicate
 * geometry, overlaps, endpoints that nearly meet, polylines left open, tiny slivers, mixed text heights,
 * dimension style drift, layer and pen hygiene, odd block inserts, strays far from the drawing) and applies
 * the safe fixes as one undo step. The AI uses it through check_drawing and fix_drawing_issues; people through
 * "Check Drawing" (Tools menu, Agent Progress menu, chat header), which shows the issues in the AI chat with a
 * "Fix all" that hands them to the AI.
 */
namespace LC_AIDrawingChecker {
    struct Issue {
        QString rule;                  // "duplicate", "endpoint_gap", ... (see rules())
        QString severity;              // "error", "warning" or "note"
        QList<unsigned long long> ids; // the entities involved; the first is the one a fix changes
        RS_Vector position {false};    // where to look
        QString message;               // what is wrong, with ids and coordinates
        QString fix;                   // what to do about it
        bool fixable {false};          // fix_drawing_issues does it by itself
        RS_Vector moveFrom {false};    // a gap (endpoint_gap, nearly_closed): the loose end that moves...
        RS_Vector moveTo {false};      // ...onto this point
    };

    struct Options {
        QString scope {QStringLiteral("all")}; // all, selection or layer
        QString layer;                         // for scope layer
        QStringList rules;                     // empty: every rule
        double tolerance {0};                  // gaps below this should meet; 0: from the drawing's size
    };

    struct Report {
        QList<Issue> issues;
        QString error;     // set when nothing could be checked
        QString scopeText; // "the whole drawing", "the selection", "layer Walls"
        QStringList rules; // the rules that ran
        int checked {0};   // entities checked
        double tolerance {0};
        double precision {0}; // points closer than this are the same point
        double size {0};      // diagonal of the main extents
        QString unit;
        const RS_Graphic* graphic {nullptr}; // only to tell whether the open drawing is still the checked one
    };

    /** Every rule id with a short title, in report order. */
    QList<QPair<QString, QString>> rules();
    /** Checks the drawing. */
    Report check(RS_Graphic* graphic, const Options& options);
    /** The report as the AI reads it (at most `limit` issue lines). */
    QString format(const Report& report, int limit = 60);
    /** check_drawing and fix_drawing_issues. */
    QList<LC_AITool> tools();
    /** Checks the drawing of this window and shows the issues in its AI chat, with "Fix all". */
    void showInChat(QWidget* window);
    /** "Check Drawing" for the menus of this window (one action per window). */
    QAction* action(QWidget* window);
}

#endif
