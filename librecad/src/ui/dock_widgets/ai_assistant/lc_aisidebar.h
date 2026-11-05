/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AISIDEBAR_H
#define LC_AISIDEBAR_H

#include <QWidget>

class QMainWindow;
class LC_AIAssistantWidget;

/**
 * AI sidebar pinned to the extreme right edge of the main window, outside of the dock areas
 * (like the Cursor chat panel). It is not a QDockWidget: it reserves a strip on the right via the
 * window's contents margins and positions itself there, so docks, saved dock layouts and the
 * dock fitting logic can never move, tab or hide it.
 */
class LC_AISidebar : public QWidget {
    Q_OBJECT
public:
    explicit LC_AISidebar(QMainWindow* window);
    bool isOpen() const { return m_open; }
    void setOpen(bool open);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updatePlacement();
    int clampedWidth(int width) const;

    QMainWindow* m_window {nullptr};
    QWidget* m_handle {nullptr};
    LC_AIAssistantWidget* m_chat {nullptr};
    int m_width {360};
    bool m_open {false};
    bool m_dragging {false};
};

#endif
