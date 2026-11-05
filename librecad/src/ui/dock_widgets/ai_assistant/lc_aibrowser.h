/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIBROWSER_H
#define LC_AIBROWSER_H

#include <QList>

#include "lc_aitool.h"

class QWidget;

/**
 * Web lookups for the AI (the read-only part of Cypher's browser_* tools): search, open, read, list
 * links, click, scroll, go back, screenshot. Pages show in a small preview floating over the AI chat
 * (expandable to a full window), in a private session; never in the system browser. Needs Qt WebEngine (LC_AI_BROWSER);
 * without it there are no browser tools.
 */
namespace LC_AIBrowser {
    bool available();
    /** The widget the small browser preview floats over (the AI chat), like Cypher's floating preview. */
    void setHost(QWidget* host);
    /** Shows or hides the small preview (or raises the full window when it is open). */
    void showWindow(QWidget* parent);
    QList<LC_AITool> tools();
}

#endif
