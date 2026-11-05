/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIICONS_H
#define LC_AIICONS_H

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

/** Draws the icons copied from Cypher (provider logos, Lucide UI icons) in any color. */
namespace LC_AIIcons {
    /** Lucide UI icon such as "key-round", "trash-2", "search", "check", "refresh-cw". */
    QPixmap lucide(const QString& name, int size, const QColor& color, qreal dpr, bool filled = false);
    QIcon lucideIcon(const QString& name, int size, const QColor& color, bool filled = false);
    /** Provider logo; providers without one get a monogram (first letter in a rounded box), like Cypher. */
    QPixmap provider(const QString& providerId, int size, const QColor& color, qreal dpr);
}

#endif
