/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aisidebar.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QMainWindow>
#include <QMouseEvent>

#include "lc_aiassistantwidget.h"

namespace {
    constexpr int MIN_SIDEBAR_WIDTH = 260;
    constexpr int MIN_REST_OF_WINDOW = 400;
    constexpr int HANDLE_WIDTH = 5;
}

LC_AISidebar::LC_AISidebar(QMainWindow* window)
    : QWidget(window), m_window(window) {
    setObjectName("ai_assistant_sidebar");
    setAutoFillBackground(true);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // left edge: drag to resize
    m_handle = new QFrame(this);
    static_cast<QFrame*>(m_handle)->setFrameShape(QFrame::VLine);
    static_cast<QFrame*>(m_handle)->setFrameShadow(QFrame::Sunken);
    m_handle->setFixedWidth(HANDLE_WIDTH);
    m_handle->setCursor(Qt::SizeHorCursor);
    m_handle->setToolTip(tr("Drag to resize the AI sidebar"));
    m_handle->installEventFilter(this);
    layout->addWidget(m_handle);

    m_chat = new LC_AIAssistantWidget(this);
    layout->addWidget(m_chat, 1);

    m_window->installEventFilter(this);
    hide();
}

int LC_AISidebar::clampedWidth(const int width) const {
    const int maxWidth = qMax(MIN_SIDEBAR_WIDTH, m_window->width() - MIN_REST_OF_WINDOW);
    return qBound(MIN_SIDEBAR_WIDTH, width, maxWidth);
}

void LC_AISidebar::setOpen(const bool open) {
    if (m_open == open) {
        return;
    }
    m_open = open;
    setVisible(open);
    updatePlacement();
    if (open) {
        m_chat->focusInput();
    }
}

void LC_AISidebar::updatePlacement() {
    QMargins margins = m_window->contentsMargins();
    const int width = m_open ? clampedWidth(m_width) : 0;
    if (margins.right() != width) {
        // reserve the strip; the main window layout (menu, toolbars, docks, canvas) shrinks to the left of it
        margins.setRight(width);
        m_window->setContentsMargins(margins);
    }
    if (m_open) {
        setGeometry(m_window->width() - width, 0, width, m_window->height());
        raise();
    }
}

bool LC_AISidebar::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_window) {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show) {
            updatePlacement();
        }
    }
    else if (watched == m_handle) {
        switch (event->type()) {
            case QEvent::MouseButtonPress: {
                const auto* mouse = static_cast<QMouseEvent*>(event);
                if (mouse->button() == Qt::LeftButton) {
                    m_dragging = true;
                    return true;
                }
                break;
            }
            case QEvent::MouseMove: {
                if (m_dragging) {
                    const auto* mouse = static_cast<QMouseEvent*>(event);
                    const int x = m_window->mapFromGlobal(mouse->globalPosition().toPoint()).x();
                    m_width = clampedWidth(m_window->width() - x);
                    updatePlacement();
                    return true;
                }
                break;
            }
            case QEvent::MouseButtonRelease:
                m_dragging = false;
                break;
            default:
                break;
        }
    }
    return QWidget::eventFilter(watched, event);
}
