/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AITOOLBAR_H
#define LC_AITOOLBAR_H

#include <QHash>
#include <QJsonArray>
#include <QList>
#include <functional>

#include "lc_aitool.h"

/**
 * Every toolbar button of OpenCAD, usable by the AI the way a person uses it:
 *  - list_toolbar_buttons: the buttons, where they are, what they do, their state, how to use one;
 *  - press_toolbar_button: press a button, then answer its prompts (type, click in the drawing, keys);
 *  - click_drawing: click a point of the drawing (pick an entity, give a point);
 *  - select_entities: select entities first, for buttons that work on a selection;
 *  - handle_dialog: read, fill in and close a dialog a button opened;
 *  - set_toolbar_value: choose a value in a toolbar list (pen colour, width, line type, view, UCS, ...);
 *  - test_toolbar_buttons: press buttons with a known recipe on a scratch drawing and check the result.
 *
 * Developer mode, in a running OpenCAD: OPENCAD_AI_TOOLBAR_TEST=<report.json> presses every button with
 * its recipe, writes the report and the checklist (OPENCAD_AI_TOOLBAR_CHECKLIST=<file>) and quits.
 * OPENCAD_AI_TOOLBAR_RECIPES=<recipes.json> uses recipes from a file instead of the built-in ones.
 */
/** The result of testing one toolbar button (or one AI tool, id "tool:<name>") with its recipe. */
struct LC_AIButtonTest {
    QString id;
    QString status; // passed, failed, skipped
    QString detail;
    QString when;   // ISO date and time of the run (saved results)
};

namespace LC_AIToolbar {
    QList<LC_AITool> tools();
    /** Every toolbar button and toolbar list: id, label, toolbars, kind and state. */
    QJsonArray inventory();

    /** The ids that have a test recipe (buttons, and "tool:<name>" for the AI's own tools). */
    QStringList recipeIds();
    /** Tests these ids (all with a recipe when empty) on scratch drawings, one at a time. */
    void runTests(const QStringList& ids, std::function<void(int done, int total, const LC_AIButtonTest&)> progress,
                  std::function<void(const QList<LC_AIButtonTest>&)> finished);
    bool testsRunning();
    /** Stops after the test that is running now. */
    void stopTests();
    /** The latest result of every id ever tested on this computer. */
    QHash<QString, LC_AIButtonTest> savedResults();
    /** Why the AI does not use this button (it quits OpenCAD, is the AI's own, is hidden), or empty. */
    QString notAgenticReason(const QString& id);
}

#endif
