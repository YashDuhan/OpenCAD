/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIPROPERTIES_H
#define LC_AIPROPERTIES_H

#include <QList>

#include "lc_aitool.h"

/**
 * OpenCAD's Properties panel (the "property_sheet" dock) for the AI, used the way a person uses it:
 *  - read: the rows the panel shows (the selection's properties, the drawing's settings when nothing is
 *    selected, or the running tool's options), with ids, types, values, choices and links;
 *  - set: change one row, as typing in it or choosing from its list would (an entity edit is one undo step);
 *  - click_link, choose_type, collapse / expand, settings, pick: the panel's links, its list of selected types,
 *    its sections, its options dialog and its "pick from the drawing" buttons.
 * One tool, use_properties; LC_AITools::all() lists it.
 */
namespace LC_AIProperties {
    QList<LC_AITool> tools();
}

#endif
