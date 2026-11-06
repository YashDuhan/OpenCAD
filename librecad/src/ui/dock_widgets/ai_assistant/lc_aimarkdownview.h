/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIMARKDOWNVIEW_H
#define LC_AIMARKDOWNVIEW_H

#include <QList>
#include <QPointer>
#include <QScrollArea>
#include <QStringList>
#include <functional>

#include "lc_aiprovidersdialog.h"

class QGridLayout;
class QLabel;
class QVBoxLayout;

/**
 * A Markdown table of the AI's answer as a compact grid: small type, a light frame, a shaded header, cells that wrap at
 * a sensible width, and its own sideways scrolling when it is wider than the chat (a label cannot scroll sideways).
 * The mouse wheel keeps scrolling the chat; Shift+wheel or a sideways swipe scrolls the table.
 */
class LC_AIMarkdownTable : public QScrollArea {
    Q_OBJECT
public:
    using Render = std::function<QString(const QString&)>;
    LC_AIMarkdownTable(const LC_AITheme& theme, Render render, QWidget* parent = nullptr);
    /** The rows (the first one is the header) and each column's alignment. */
    void setRows(const QList<QStringList>& rows, const QList<Qt::Alignment>& alignments);

protected:
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void fitHeight();

    LC_AITheme m_theme;
    Render m_render;
    QWidget* m_grid {nullptr};
    QGridLayout* m_layout {nullptr};
    QList<QList<QLabel*>> m_cells;
};

/**
 * One block of the AI's answer: its Markdown shown as text labels with a compact table component for every Markdown
 * table in it. Setting new text (while it streams in) updates the parts in place, so nothing flickers.
 */
class LC_AIMarkdownView : public QWidget {
    Q_OBJECT
public:
    using Render = std::function<QString(const QString&)>;
    /** render turns Markdown into what a label shows (the transcript's math and tag clean-up). */
    LC_AIMarkdownView(const LC_AITheme& theme, Render render, QWidget* parent = nullptr);
    void setMarkdown(const QString& markdown);
    QString markdown() const { return m_markdown; }

private:
    struct Part {
        bool table {false};
        QString text;                       // a text part
        QList<QStringList> rows;            // a table: header first
        QList<Qt::Alignment> alignments;
    };
    static QList<Part> split(const QString& markdown);
    QLabel* textLabel();

    LC_AITheme m_theme;
    Render m_render;
    QVBoxLayout* m_layout {nullptr};
    QList<QPointer<QWidget>> m_widgets; // one per part, in order
    QList<bool> m_kinds;                // true: a table
    QString m_markdown;
};

#endif
