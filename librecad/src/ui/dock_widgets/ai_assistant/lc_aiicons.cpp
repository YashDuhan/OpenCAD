/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aiicons.h"

#include <QFont>
#include <QPainter>
#include <QSvgRenderer>

#include "lc_aigenerated.h"

namespace {
    const char* findSvg(const QString& name) {
        const QByteArray key = name.toUtf8();
        for (int i = 0; i < LC_AIGenerated::ICON_COUNT; ++i) {
            if (key == LC_AIGenerated::ICONS[i].name) {
                return LC_AIGenerated::ICONS[i].svg;
            }
        }
        return nullptr;
    }

    QPixmap render(const char* svg, const int size, const QColor& color, const qreal dpr, const bool filled = false) {
        QByteArray data(svg);
        if (filled) {
            data.replace("fill=\"none\"", "fill=\"currentColor\""); // e.g. the favorite star
        }
        data.replace("currentColor", color.name(QColor::HexRgb).toUtf8());
        QSvgRenderer renderer(data);
        QPixmap pixmap(QSize(size, size) * dpr);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setOpacity(color.alphaF());
        renderer.render(&painter, QRectF(0, 0, size * dpr, size * dpr));
        painter.end();
        pixmap.setDevicePixelRatio(dpr);
        return pixmap;
    }

    QPixmap monogram(const QString& providerId, const int size, const QColor& color, const qreal dpr) {
        QPixmap pixmap(QSize(size, size) * dpr);
        pixmap.fill(Qt::transparent);
        pixmap.setDevicePixelRatio(dpr);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(color, 1));
        painter.drawRoundedRect(QRectF(0.5, 0.5, size - 1, size - 1), 4, 4);
        QFont font = painter.font();
        font.setPixelSize(qRound(size * 0.62));
        font.setWeight(QFont::DemiBold);
        painter.setFont(font);
        painter.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, providerId.left(1).toUpper());
        return pixmap;
    }
}

QPixmap LC_AIIcons::lucide(const QString& name, const int size, const QColor& color, const qreal dpr, const bool filled) {
    const char* svg = findSvg("lucide:" + name);
    return svg != nullptr ? render(svg, size, color, dpr, filled) : QPixmap();
}

QIcon LC_AIIcons::lucideIcon(const QString& name, const int size, const QColor& color, const bool filled) {
    QIcon icon;
    icon.addPixmap(lucide(name, size, color, 1.0, filled));
    icon.addPixmap(lucide(name, size, color, 2.0, filled));
    return icon;
}

QPixmap LC_AIIcons::provider(const QString& providerId, const int size, const QColor& color, const qreal dpr) {
    if (providerId == "custom") {
        return lucide("cpu", size, color, dpr); // Cypher shows local models with the Cpu icon
    }
    const char* svg = findSvg(providerId);
    return svg != nullptr ? render(svg, size, color, dpr) : monogram(providerId, size, color, dpr);
}
