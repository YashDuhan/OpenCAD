/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AILAYERS_H
#define LC_AILAYERS_H

#include <QList>

#include "lc_aitool.h"

/**
 * The AI's layer tool ("layers"): list, create, switch to, rename, set the look and switches of, move entities to and
 * remove layers by name, without the Layers panel or the command line (whose cnlayer creates a layer, and whose text is
 * lowercased). Names are matched exactly first, then ignoring case when that is unambiguous.
 */
namespace LC_AILayers {
    QList<LC_AITool> tools();
}

#endif
