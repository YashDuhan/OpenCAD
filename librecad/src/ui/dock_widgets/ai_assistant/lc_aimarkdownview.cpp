/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aimarkdownview.h"

#include <QGridLayout>
#include <QLabel>
#include <QRegularExpression>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {
    constexpr int CELL_MAX_WIDTH = 240; // a wider cell wraps; the table then scrolls sideways if it is still too wide
    constexpr int TABLE_GAP = 6;        // above and below a table

    /** A Markdown table's separator line: | --- | :---: | ---: | */
    bool isSeparator(const QString& line) {
        static const QRegularExpression re(QStringLiteral(R"(^\s*\|?\s*:?-{2,}:?\s*(\|\s*:?-{2,}:?\s*)*\|?\s*$)"));
        return line.contains('|') && re.match(line).hasMatch();
    }

    /** The cells of a table row; a | inside `code` or written \| is part of the cell. */
    QStringList cellsOf(const QString& line) {
        QStringList cells;
        QString cell;
        bool code = false;
        const QString t = line.trimmed();
        for (int i = 0; i < t.size(); ++i) {
            const QChar c = t[i];
            if (c == '\\' && i + 1 < t.size() && t[i + 1] == '|') {
                cell += '|';
                ++i;
                continue;
            }
            if (c == '`') {
                code = !code;
            }
            if (c == '|' && !code) {
                cells << cell.trimmed();
                cell.clear();
                continue;
            }
            cell += c;
        }
        cells << cell.trimmed();
        if (t.startsWith('|') && !cells.isEmpty()) {
            cells.removeFirst();
        }
        if (t.endsWith('|') && !t.endsWith(QStringLiteral("\\|")) && !cells.isEmpty()) {
            cells.removeLast();
        }
        return cells;
    }

    QList<Qt::Alignment> alignmentsOf(const QString& separator, const int columns) {
        QList<Qt::Alignment> out;
        const QStringList cells = cellsOf(separator);
        for (int i = 0; i < columns; ++i) {
            const QString c = i < cells.size() ? cells[i] : QString();
            const bool left = c.startsWith(':');
            const bool right = c.endsWith(':');
            out << (left && right ? Qt::AlignHCenter : right ? Qt::AlignRight : Qt::AlignLeft);
        }
        return out;
    }

    QString rgba(const QColor& c, const double alpha) {
        return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha);
    }
}

// ------------------------------------------------------------------------------------------------ the table

LC_AIMarkdownTable::LC_AIMarkdownTable(const LC_AITheme& theme, Render render, QWidget* parent)
    : QScrollArea(parent), m_theme(theme), m_render(std::move(render)) {
    setObjectName("aiMdTable");
    setWidgetResizable(false);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); // as wide as the chat, never wider: it scrolls instead
    setFrameShape(QFrame::NoFrame);
    const LC_AITheme& t = m_theme;
    setStyleSheet(QStringLiteral(R"(
#aiMdTable { background: transparent; border: 1px solid %1; border-radius: 8px; }
#aiMdTable > QWidget > QWidget#aiMdGrid { background: transparent; }
QLabel#aiMdHead { background: %2; color: %3; font-weight: 600; font-size: 12px; padding: 5px 9px; border-bottom: 1px solid %1; }
QLabel#aiMdCell { color: %4; font-size: 12px; padding: 5px 9px; border-top: 1px solid %5; }
#aiMdTable QScrollBar:horizontal { background: transparent; height: 8px; margin: 0 4px 2px 4px; }
#aiMdTable QScrollBar::handle:horizontal { background: %1; border-radius: 3px; min-width: 30px; }
#aiMdTable QScrollBar::add-line, #aiMdTable QScrollBar::sub-line, #aiMdTable QScrollBar::add-page, #aiMdTable QScrollBar::sub-page { background: none; width: 0; height: 0; }
)")
                      .arg(t.borderDefault.name(QColor::HexArgb), t.bgSubtle.name(QColor::HexArgb), t.textHeading.name(),
                           t.textPrimary.name(), rgba(t.textMuted, 0.18)));
    m_grid = new QWidget(this);
    m_grid->setObjectName("aiMdGrid");
    m_layout = new QGridLayout(m_grid);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    setWidget(m_grid);
}

void LC_AIMarkdownTable::setRows(const QList<QStringList>& rows, const QList<Qt::Alignment>& alignments) {
    const int columns = alignments.size();
    // rows streamed in keep their cells: only what changed is set again
    while (m_cells.size() > rows.size()) {
        for (QLabel* l : m_cells.takeLast()) {
            delete l;
        }
    }
    for (int r = 0; r < rows.size(); ++r) {
        if (r >= m_cells.size()) {
            m_cells.append(QList<QLabel*>());
        }
        QList<QLabel*>& line = m_cells[r];
        while (line.size() > columns) {
            delete line.takeLast();
        }
        for (int c = 0; c < columns; ++c) {
            if (c >= line.size()) {
                auto* label = new QLabel(m_grid);
                label->setObjectName(r == 0 ? "aiMdHead" : "aiMdCell");
                label->setTextFormat(Qt::MarkdownText);
                label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
                label->setOpenExternalLinks(true);
                m_layout->addWidget(label, r, c);
                line << label;
            }
            QLabel* label = line[c];
            const QString text = m_render(c < rows[r].size() ? rows[r][c] : QString());
            label->setAlignment(alignments[c] | Qt::AlignVCenter);
            if (label->text() == text && label->property("columnWidth").isValid()) {
                continue;
            }
            label->setText(text);
            label->ensurePolished(); // the padding of the style sheet counts in the width
            // as wide as its text up to a limit, then wrapping: a table of short values stays narrow
            label->setWordWrap(false);
            const int natural = label->sizeHint().width();
            label->setWordWrap(natural > CELL_MAX_WIDTH);
            label->setMinimumWidth(qMin(natural, CELL_MAX_WIDTH));
            label->setMaximumWidth(qMax(natural, CELL_MAX_WIDTH));
            label->setProperty("columnWidth", natural);
        }
    }
    m_grid->adjustSize();
    fitHeight();
}

void LC_AIMarkdownTable::fitHeight() {
    m_grid->resize(m_grid->sizeHint());
    const bool scrolls = m_grid->width() > viewport()->width() + 1;
    const int bar = scrolls ? horizontalScrollBar()->sizeHint().height() : 0;
    setFixedHeight(m_grid->height() + bar + 2 * frameWidth() + 2);
    setMaximumWidth(m_grid->width() + 2 * frameWidth() + 2); // the frame hugs a narrow table
}

void LC_AIMarkdownTable::resizeEvent(QResizeEvent* event) {
    QScrollArea::resizeEvent(event);
    fitHeight(); // the scroll bar comes and goes with the chat's width
}

void LC_AIMarkdownTable::wheelEvent(QWheelEvent* event) {
    // a plain wheel scrolls the chat; Shift+wheel or a sideways swipe scrolls the table
    if (event->angleDelta().x() != 0 || (event->modifiers() & Qt::ShiftModifier)) {
        QScrollBar* bar = horizontalScrollBar();
        const int delta = event->angleDelta().x() != 0 ? event->angleDelta().x() : event->angleDelta().y();
        bar->setValue(bar->value() - delta / 2);
        event->accept();
        return;
    }
    event->ignore();
}

// ------------------------------------------------------------------------------------------------ the block

LC_AIMarkdownView::LC_AIMarkdownView(const LC_AITheme& theme, Render render, QWidget* parent)
    : QWidget(parent), m_theme(theme), m_render(std::move(render)) {
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
}

QList<LC_AIMarkdownView::Part> LC_AIMarkdownView::split(const QString& markdown) {
    QList<Part> parts;
    const QStringList lines = markdown.split('\n');
    QStringList text;
    bool fence = false;
    auto flushText = [&] {
        if (!text.isEmpty()) {
            Part p;
            p.text = text.join('\n');
            if (!p.text.trimmed().isEmpty()) {
                parts << p;
            }
            text.clear();
        }
    };
    for (int i = 0; i < lines.size(); ++i) {
        const QString& line = lines[i];
        const QString t = line.trimmed();
        if (t.startsWith(QStringLiteral("```")) || t.startsWith(QStringLiteral("~~~"))) {
            fence = !fence;
        }
        // a table: a header row followed by the separator row, then the rows that follow without a blank line
        if (!fence && line.contains('|') && i + 1 < lines.size() && isSeparator(lines[i + 1]) && !isSeparator(line)) {
            flushText();
            Part table;
            table.table = true;
            const QStringList header = cellsOf(line);
            table.rows << header;
            table.alignments = alignmentsOf(lines[i + 1], int(header.size()));
            int j = i + 2;
            for (; j < lines.size() && lines[j].contains('|') && !lines[j].trimmed().isEmpty(); ++j) {
                table.rows << cellsOf(lines[j]);
            }
            parts << table;
            i = j - 1;
            continue;
        }
        text << line;
    }
    flushText();
    return parts;
}

QLabel* LC_AIMarkdownView::textLabel() {
    auto* label = new QLabel(this);
    label->setTextFormat(Qt::MarkdownText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    label->setOpenExternalLinks(true);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setContentsMargins(2, 0, 2, 4);
    return label;
}

void LC_AIMarkdownView::setMarkdown(const QString& markdown) {
    m_markdown = markdown;
    const QList<Part> parts = split(markdown);
    // the parts that keep their kind keep their widget (no flicker while the answer streams in)
    int keep = 0;
    while (keep < parts.size() && keep < m_widgets.size() && m_widgets[keep] && m_kinds[keep] == parts[keep].table) {
        ++keep;
    }
    while (m_widgets.size() > keep) {
        if (QWidget* w = m_widgets.takeLast()) {
            w->deleteLater();
        }
        m_kinds.removeLast();
    }
    for (int i = 0; i < parts.size(); ++i) {
        const Part& p = parts[i];
        if (i >= m_widgets.size()) {
            QWidget* w = nullptr;
            if (p.table) {
                auto* holder = new QWidget(this); // the table with a little room above and below
                auto* box = new QVBoxLayout(holder);
                box->setContentsMargins(2, TABLE_GAP, 2, TABLE_GAP + 2);
                box->addWidget(new LC_AIMarkdownTable(m_theme, m_render, holder), 0, Qt::AlignLeft);
                holder->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                w = holder;
            }
            else {
                w = textLabel();
            }
            m_layout->addWidget(w);
            m_widgets << w;
            m_kinds << p.table;
        }
        if (p.table) {
            if (auto* table = m_widgets[i]->findChild<LC_AIMarkdownTable*>()) {
                table->setRows(p.rows, p.alignments);
            }
        }
        else if (auto* label = qobject_cast<QLabel*>(m_widgets[i].data())) {
            const QString shown = m_render(p.text);
            if (label->text() != shown) {
                label->setText(shown);
            }
        }
    }
}
