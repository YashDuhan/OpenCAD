/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aicomposerextras.h"

#include <QAction>
#include <QDialog>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTextBlock>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

#include "lc_aiassistantwidget.h"
#include "lc_aicredentials.h"
#include "lc_aiicons.h"
#include "lc_aiinferencemanager.h"
#include "lc_aiprovider.h"
#include "lc_aitool.h"
#include "rs_graphic.h"
#include "rs_graphicview.h"

namespace {
    constexpr int TILE_EDGE = 56;          // Cypher's .attachment
    constexpr int LARGE_TILE_EDGE = 96;    // .attachment.large: a single image
    constexpr int NOTICE_MS = 7000;
    constexpr int POPUP_ROW_HEIGHT = 30;
    constexpr int POPUP_HEADER_HEIGHT = 24;
    constexpr int POPUP_MAX_HEIGHT = 320;
    constexpr int ROLE_INDEX = Qt::UserRole;      // index into the candidates, -1 for a group header
    constexpr int ROLE_DETAIL = Qt::UserRole + 1;
    constexpr int ROLE_ICON = Qt::UserRole + 2;

    QString css(const QColor& c) {
        return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF(), 0, 'f', 3);
    }

    QFont pxFont(const QFont& base, const double px, const QFont::Weight weight = QFont::Normal) {
        QFont f(base);
        f.setPixelSize(qRound(px));
        f.setWeight(weight);
        return f;
    }

    /** Lays its widgets out in rows that wrap, on the left or (the files of a sent message) on the right. */
    class FlowLayout : public QLayout {
    public:
        FlowLayout(QWidget* parent, const int spacing, const bool alignRight) : QLayout(parent), m_gap(spacing), m_right(alignRight) {
            setContentsMargins(0, 0, 0, 0);
        }
        ~FlowLayout() override {
            while (QLayoutItem* item = FlowLayout::takeAt(0)) {
                delete item;
            }
        }
        void addItem(QLayoutItem* item) override { m_items.append(item); }
        int count() const override { return int(m_items.size()); }
        QLayoutItem* itemAt(const int i) const override { return m_items.value(i); }
        QLayoutItem* takeAt(const int i) override { return i >= 0 && i < m_items.size() ? m_items.takeAt(i) : nullptr; }
        Qt::Orientations expandingDirections() const override { return {}; }
        bool hasHeightForWidth() const override { return true; }
        int heightForWidth(const int width) const override { return arrange(QRect(0, 0, width, 0), false); }
        void setGeometry(const QRect& rect) override {
            QLayout::setGeometry(rect);
            arrange(rect, true);
        }
        QSize sizeHint() const override { // everything on one row
            int width = 0;
            int height = 0;
            for (QLayoutItem* item : m_items) {
                if (!item->isEmpty()) {
                    width += (width > 0 ? m_gap : 0) + item->sizeHint().width();
                    height = qMax(height, item->sizeHint().height());
                }
            }
            const QMargins m = contentsMargins();
            return {width + m.left() + m.right(), height + m.top() + m.bottom()};
        }
        QSize minimumSize() const override {
            QSize size;
            for (QLayoutItem* item : m_items) {
                size = size.expandedTo(item->minimumSize());
            }
            const QMargins m = contentsMargins();
            return size + QSize(m.left() + m.right(), m.top() + m.bottom());
        }

    private:
        int arrange(const QRect& outer, const bool apply) const {
            const QMargins m = contentsMargins();
            const QRect rect = outer.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
            int y = rect.y();
            int lineWidth = 0;
            int lineHeight = 0;
            QList<QLayoutItem*> line;
            const auto flush = [&] {
                int x = m_right ? rect.right() + 1 - lineWidth : rect.x();
                for (QLayoutItem* item : std::as_const(line)) {
                    const QSize s = item->sizeHint();
                    if (apply) {
                        item->setGeometry(QRect(QPoint(x, y + (lineHeight - s.height()) / 2), s));
                    }
                    x += s.width() + m_gap;
                }
                y += lineHeight + m_gap;
                line.clear();
                lineWidth = 0;
                lineHeight = 0;
            };
            for (QLayoutItem* item : m_items) {
                if (item->isEmpty()) {
                    continue;
                }
                const QSize s = item->sizeHint();
                if (!line.isEmpty() && lineWidth + m_gap + s.width() > rect.width()) {
                    flush();
                }
                lineWidth += (line.isEmpty() ? 0 : m_gap) + s.width();
                lineHeight = qMax(lineHeight, s.height());
                line << item;
            }
            if (!line.isEmpty()) {
                flush();
            }
            const int used = y > rect.y() ? y - rect.y() - m_gap : 0;
            return used + m.top() + m.bottom();
        }

        QList<QLayoutItem*> m_items;
        int m_gap;
        bool m_right;
    };

    /** Cypher's warning: the model picked in the catalog does not take images (empty when it does or is unknown). */
    QString blindModelNote() {
        LC_AICredentials& creds = LC_AICredentials::instance();
        const QString provider = creds.selectedProvider();
        const QString model = creds.model(provider);
        for (const LC_AIGenerated::ModelEntry* m : LC_AIProvider::catalog(provider)) {
            if (model == m->id) {
                return m->images ? QString()
                                 : QObject::tr("%1 does not read images or PDFs; pick another model with the model button.")
                                       .arg(QString::fromUtf8(m->name));
            }
        }
        return {};
    }

    void showFullImage(QWidget* from, const LC_AIAttachment& a) {
        QPixmap pixmap;
        pixmap.loadFromData(a.data);
        if (pixmap.isNull()) {
            pixmap = QPixmap::fromImage(a.preview);
        }
        auto* dialog = new QDialog(from->window());
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(a.name);
        auto* layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* scroll = new QScrollArea(dialog);
        auto* label = new QLabel();
        label->setPixmap(pixmap);
        scroll->setWidget(label);
        layout->addWidget(scroll);
        dialog->resize(qMin(pixmap.width() + 24, 1400), qMin(pixmap.height() + 24, 900));
        dialog->show();
    }

    /**
     * An attached file: a square tile in the composer (Cypher's .attachment, with its remove button), the picture
     * itself in a sent message, or a chip with the name and pages of a PDF.
     */
    class AttachmentView : public QWidget {
    public:
        enum class Mode { Tile, Fit, Chip };

        AttachmentView(const LC_AIAttachment& a, const Mode mode, const QSize& box, const LC_AITheme& theme, QWidget* parent)
            : QWidget(parent), m_a(a), m_mode(mode), m_theme(theme) {
            if (mode == Mode::Fit && !a.preview.isNull()) {
                m_size = a.preview.size().scaled(box, Qt::KeepAspectRatio).expandedTo(QSize(24, 24));
                if (a.preview.width() <= box.width() && a.preview.height() <= box.height()) {
                    m_size = a.preview.size(); // small pictures are not blown up
                }
            }
            else if (mode == Mode::Chip) {
                const QFontMetrics fm(pxFont(font(), 12.5, QFont::Medium));
                m_size = QSize(qMin(240, 40 + qMax(fm.horizontalAdvance(a.name), QFontMetrics(pxFont(font(), 11)).horizontalAdvance(detail())) + 12), 40);
            }
            else {
                m_size = box;
            }
            setFixedSize(m_size);
            QStringList tip{a.name};
            if (!a.isImage()) {
                tip << detail();
            }
            else {
                tip << QStringLiteral("%1 × %2 px, %3").arg(a.size.width()).arg(a.size.height()).arg(LC_AIAttachments::sizeText(a.data.size()));
            }
            if (!a.note.isEmpty()) {
                tip << a.note;
            }
            setToolTip(tip.join("\n"));
            if (a.isImage()) {
                setCursor(Qt::PointingHandCursor);
            }
        }

        /** Shows the remove button (white dot with an x, top right), which calls `remove`. */
        void setRemovable(std::function<void()> remove) {
            auto* button = new QToolButton(this);
            button->setObjectName("attachmentRemove");
            button->setIcon(LC_AIIcons::lucideIcon("x", 10, QColor("#111111")));
            button->setIconSize(QSize(10, 10));
            button->setToolTip(QObject::tr("Remove %1").arg(m_a.name));
            button->setCursor(Qt::PointingHandCursor);
            button->setStyleSheet(QStringLiteral("QToolButton { background: #ffffff; border: none; border-radius: 7px; padding: 0; }"
                                                 "QToolButton:hover { background: #e4e4e7; }"));
            button->setFixedSize(14, 14);
            button->move(width() - 14 - 4, 4);
            QObject::connect(button, &QToolButton::clicked, this, [remove = std::move(remove)] { remove(); });
        }

        QSize sizeHint() const override { return m_size; }

    protected:
        void paintEvent(QPaintEvent*) override {
            QPainter p(this);
            p.setRenderHint(QPainter::Antialiasing);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
            const qreal radius = m_mode == Mode::Tile ? 6 : 8;
            QPainterPath clip;
            clip.addRoundedRect(r, radius, radius);
            const qreal dpr = devicePixelRatioF();
            if (m_mode == Mode::Chip) { // Cypher's .element-chip: icon, name over "PDF · pages · size"
                p.fillPath(clip, m_theme.bgCard);
                p.setPen(QPen(m_theme.borderDefault, 1));
                p.drawPath(clip);
                p.drawPixmap(12, (height() - 16) / 2, LC_AIIcons::lucide("file-text", 16, m_theme.textMuted, dpr));
                const int x = 36;
                const int w = width() - x - 10;
                p.setFont(pxFont(font(), 12.5, QFont::Medium));
                p.setPen(m_theme.textSecondary);
                p.drawText(QRect(x, 4, w, 18), Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(m_a.name, Qt::ElideMiddle, w));
                p.setFont(pxFont(font(), 11));
                p.setPen(m_theme.textFaint);
                p.drawText(QRect(x, 20, w, 16), Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(detail(), Qt::ElideRight, w));
                return;
            }
            p.save();
            p.setClipPath(clip);
            p.fillRect(r, m_theme.bgActive);
            if (!m_a.preview.isNull()) {
                const QImage& image = m_a.preview;
                QRectF source(QPointF(0, 0), image.size());
                if (m_mode == Mode::Tile) { // object-fit: cover (a PDF page keeps its top, where the title is)
                    const qreal scale = qMax(r.width() / image.width(), r.height() / image.height());
                    const QSizeF shown(r.width() / scale, r.height() / scale);
                    source = QRectF(QPointF((image.width() - shown.width()) / 2, m_a.isImage() ? (image.height() - shown.height()) / 2 : 0), shown);
                }
                p.drawImage(r, image, source);
            }
            else { // a PDF without a picture of its first page: icon and name
                p.drawPixmap(QPointF((width() - 22) / 2.0, height() / 2.0 - 18), LC_AIIcons::lucide("file-text", 22, m_theme.textMuted, dpr));
                p.setFont(pxFont(font(), 9));
                p.setPen(m_theme.textMuted);
                p.drawText(QRect(4, height() / 2 + 6, width() - 8, 14), Qt::AlignCenter,
                           p.fontMetrics().elidedText(m_a.name, Qt::ElideMiddle, width() - 8));
            }
            // a label on pictures that are not plain images: "PDF", "@view"
            const QString badge = !m_a.isImage() && !m_a.preview.isNull() ? QStringLiteral("PDF") : m_a.fromMention ? m_a.name : QString();
            if (!badge.isEmpty()) {
                p.setFont(pxFont(font(), 9.5, QFont::DemiBold));
                const int bw = p.fontMetrics().horizontalAdvance(badge) + 8;
                const QRectF box(4, height() - 4 - 15, bw, 15);
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0, 0, 0, 150));
                p.drawRoundedRect(box, 4, 4);
                p.setPen(Qt::white);
                p.drawText(box, Qt::AlignCenter, badge);
            }
            p.restore();
            p.setPen(QPen(m_theme.borderDefault, 1));
            p.setBrush(Qt::NoBrush);
            p.drawPath(clip);
        }

        void mouseReleaseEvent(QMouseEvent* event) override {
            if (event->button() == Qt::LeftButton && m_a.isImage() && rect().contains(event->position().toPoint())) {
                showFullImage(this, m_a);
            }
        }

    private:
        QString detail() const {
            QStringList parts{QStringLiteral("PDF")};
            if (m_a.pages > 0) {
                parts << (m_a.pages == 1 ? QObject::tr("1 page") : QObject::tr("%1 pages").arg(m_a.pages));
            }
            parts << LC_AIAttachments::sizeText(m_a.data.size());
            return parts.join(QStringLiteral(" · "));
        }

        LC_AIAttachment m_a;
        Mode m_mode;
        LC_AITheme m_theme;
        QSize m_size;
    };

    /** Cypher's .popover rows: group names, and .menu-item rows with an icon, the mention and a faint detail. */
    class MentionDelegate : public QStyledItemDelegate {
    public:
        MentionDelegate(const LC_AITheme& theme, QObject* parent) : QStyledItemDelegate(parent), m_theme(theme) {}

        QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override {
            return {100, index.data(ROLE_INDEX).toInt() < 0 ? POPUP_HEADER_HEIGHT : POPUP_ROW_HEIGHT};
        }

        void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
            const LC_AITheme& t = m_theme;
            const QRect r = option.rect;
            p->save();
            p->setRenderHint(QPainter::Antialiasing);
            if (index.data(ROLE_INDEX).toInt() < 0) { // .popover-group
                p->setFont(pxFont(option.font, 11));
                p->setPen(t.textFaint);
                p->drawText(r.adjusted(8, 4, -8, 0), Qt::AlignLeft | Qt::AlignVCenter, index.data(Qt::DisplayRole).toString());
                p->restore();
                return;
            }
            const bool highlight = option.state & QStyle::State_Selected;
            if (highlight) {
                QPainterPath bg;
                bg.addRoundedRect(QRectF(r), 6, 6);
                p->fillPath(bg, t.bgHover);
            }
            const qreal dpr = p->device()->devicePixelRatioF();
            p->drawPixmap(r.left() + 8, r.center().y() - 6, LC_AIIcons::lucide(index.data(ROLE_ICON).toString(), 13,
                                                                            highlight ? t.textPrimary : t.textMuted, dpr));
            const int x = r.left() + 8 + 13 + 8;
            const QFont labelFont = pxFont(option.font, 13);
            const QFont detailFont = pxFont(option.font, 12);
            const QString label = index.data(Qt::DisplayRole).toString();
            const QString detail = index.data(ROLE_DETAIL).toString();
            // the mention keeps its full width; the detail gets the rest, and is left out when there is no room
            const int room = r.right() - 8 - x;
            // (+2: whole-pixel advances are a little short of what eliding measures)
            const int labelWidth = qMin(room, QFontMetrics(labelFont).horizontalAdvance(label) + 2);
            int detailWidth = qMin(QFontMetrics(detailFont).horizontalAdvance(detail) + 2, room - labelWidth - 12);
            if (detailWidth < 36) {
                detailWidth = 0;
            }
            p->setFont(labelFont);
            p->setPen(highlight ? t.textPrimary : t.textSecondary);
            p->drawText(QRect(x, r.top(), labelWidth, r.height()), Qt::AlignLeft | Qt::AlignVCenter,
                        QFontMetrics(labelFont).elidedText(label, Qt::ElideRight, labelWidth));
            if (detailWidth > 0) {
                p->setFont(detailFont);
                p->setPen(t.textFaint);
                p->drawText(QRect(r.right() - 8 - detailWidth, r.top(), detailWidth, r.height()), Qt::AlignRight | Qt::AlignVCenter,
                            QFontMetrics(detailFont).elidedText(detail, Qt::ElideRight, detailWidth));
            }
            p->restore();
        }

    private:
        LC_AITheme m_theme;
    };
}

// ------------------------------------------------------------------------------------------------ message box

LC_AIComposerInput::LC_AIComposerInput(QWidget* parent) : QPlainTextEdit(parent) {
    connect(this, &QPlainTextEdit::textChanged, this, &LC_AIComposerInput::refreshMentions);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &LC_AIComposerInput::stepOverMentions);
}

void LC_AIComposerInput::setMentionColors(const QColor& text, const QColor& background) {
    m_mentionText = text;
    m_mentionBackground = background;
    refreshMentions();
}

const QList<LC_AIMention>& LC_AIComposerInput::mentions() {
    if (document()->revision() != m_revision) {
        refreshMentions();
    }
    return m_mentions;
}

void LC_AIComposerInput::refreshMentions() {
    m_revision = document()->revision();
    m_mentions = LC_AIMentions::find(toPlainText());
    // the text of a mention in the accent colour; paintEvent draws the pill behind it
    QList<QTextEdit::ExtraSelection> selections;
    for (const LC_AIMention& m : std::as_const(m_mentions)) {
        QTextEdit::ExtraSelection s;
        s.cursor = QTextCursor(document());
        s.cursor.setPosition(m.start);
        s.cursor.setPosition(m.start + m.length, QTextCursor::KeepAnchor);
        s.format.setForeground(m_mentionText);
        selections << s;
    }
    setExtraSelections(selections);
    viewport()->update();
}

const LC_AIMention* LC_AIComposerInput::mentionAround(const int position, const bool includeEnds) const {
    for (const LC_AIMention& m : m_mentions) {
        const int end = m.start + m.length;
        if (includeEnds ? (position >= m.start && position <= end) : (position > m.start && position < end)) {
            return &m;
        }
    }
    return nullptr;
}

void LC_AIComposerInput::stepOverMentions() {
    mentions(); // up to date with the text
    QTextCursor cursor = textCursor();
    const int position = cursor.position();
    if (!cursor.hasSelection()) {
        if (const LC_AIMention* m = mentionAround(position, false)) {
            const int start = m->start;
            const int end = m->start + m->length;
            const int target = m_lastPosition <= start ? end
                               : m_lastPosition >= end ? start
                               : (position - start < end - position ? start : end);
            m_lastPosition = target;
            cursor.setPosition(target);
            setTextCursor(cursor);
            return;
        }
    }
    m_lastPosition = position;
}

void LC_AIComposerInput::keyPressEvent(QKeyEvent* event) {
    // Backspace or Delete next to a mention removes all of it, like a chip
    const QTextCursor cursor = textCursor();
    const bool back = event->key() == Qt::Key_Backspace;
    const bool forward = event->key() == Qt::Key_Delete;
    if ((back || forward) && !cursor.hasSelection() && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
        const int position = cursor.position();
        for (const LC_AIMention& m : mentions()) {
            const int end = m.start + m.length;
            if ((back && position > m.start && position <= end) || (forward && position >= m.start && position < end)) {
                QTextCursor remove(document());
                remove.setPosition(m.start);
                remove.setPosition(end, QTextCursor::KeepAnchor);
                remove.removeSelectedText();
                setTextCursor(remove);
                event->accept();
                return;
            }
        }
    }
    // a letter typed right before a pill would join its "@" to a word and turn it into plain text: a space keeps them apart
    if (!(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && gluesToMention(event->text())) {
        QTextCursor typed = textCursor();
        typed.insertText(event->text() + QLatin1Char(' '));
        typed.movePosition(QTextCursor::PreviousCharacter);
        setTextCursor(typed);
        event->accept();
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

bool LC_AIComposerInput::gluesToMention(const QString& typed) {
    if (typed.isEmpty() || !typed.back().isPrint() || textCursor().hasSelection()) {
        return false;
    }
    const QChar last = typed.back();
    if (!last.isLetterOrNumber() && last != '_' && last != '@' && last != '.') { // what LC_AIMentions::find() reads as joined
        return false;
    }
    const int position = textCursor().position();
    for (const LC_AIMention& m : mentions()) {
        if (m.start == position) {
            return true;
        }
    }
    return false;
}

void LC_AIComposerInput::paintEvent(QPaintEvent* event) {
    if (!m_mentions.isEmpty() && m_mentionBackground.isValid()) {
        // the pills go under the text, which the base class draws next
        QPainter p(viewport());
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(m_mentionBackground);
        const auto rectAt = [this](const int position) {
            QTextCursor at(document());
            at.setPosition(position);
            return cursorRect(at);
        };
        // 2 px around the text, less between two pills one space apart: padded fully they touch and read as one
        const auto padBetween = [&rectAt](const LC_AIMention& a, const LC_AIMention& b) -> qreal {
            const QRect end = rectAt(a.start + a.length);
            const QRect start = rectAt(b.start);
            return end.top() != start.top() ? 2.0 : qBound(0.0, (start.left() - end.left() - 3) / 2.0, 2.0);
        };
        for (int i = 0; i < m_mentions.size(); ++i) {
            const LC_AIMention& m = m_mentions.at(i);
            qreal left = i > 0 ? padBetween(m_mentions.at(i - 1), m) : 2.0;
            const qreal right = i + 1 < m_mentions.size() ? padBetween(m, m_mentions.at(i + 1)) : 2.0;
            QRect from = rectAt(m.start);
            const QRect to = rectAt(m.start + m.length);
            // one rectangle per line the mention spans (a long layer name may wrap)
            for (int line = 0; from.top() < to.top() && from.height() > 0 && line < 32; ++line) {
                p.drawRoundedRect(QRectF(from.left() - left, from.top() + 1, viewport()->width() - from.left() + left - 4, from.height() - 2), 4, 4);
                from = QRect(int(document()->documentMargin()), from.bottom() + 1, 0, from.height());
                left = 2.0;
            }
            p.drawRoundedRect(QRectF(from.left() - left, from.top() + 1, to.left() - from.left() + left + right, from.height() - 2), 4, 4);
        }
    }
    QPlainTextEdit::paintEvent(event);
}

bool LC_AIComposerInput::canInsertFromMimeData(const QMimeData* source) const {
    return source->hasImage() || !LC_AIAttachments::attachableFiles(source).isEmpty() || QPlainTextEdit::canInsertFromMimeData(source);
}

void LC_AIComposerInput::insertFromMimeData(const QMimeData* source) {
    // pasted files and pictures become attachments; text stays text (a word processor also offers a picture of it)
    if (const QStringList files = LC_AIAttachments::attachableFiles(source); !files.isEmpty()) {
        emit filesOffered(files);
        return;
    }
    if (source->hasImage() && !source->hasText()) {
        const QImage image = qvariant_cast<QImage>(source->imageData());
        if (!image.isNull()) {
            emit imageOffered(image);
            return;
        }
    }
    if (source->hasText() && gluesToMention(source->text())) { // as when typing: the pill after it stays a pill
        QTextCursor pasted = textCursor();
        pasted.insertText(source->text() + QLatin1Char(' '));
        pasted.movePosition(QTextCursor::PreviousCharacter);
        setTextCursor(pasted);
        return;
    }
    QPlainTextEdit::insertFromMimeData(source);
}

// ------------------------------------------------------------------------------------------------ extras

LC_AIComposerExtras::LC_AIComposerExtras(LC_AIComposerInput* input, QWidget* composer, const LC_AITheme& theme, QWidget* panel)
    : QObject(panel), m_input(input), m_composer(composer), m_panel(panel), m_theme(theme) {
    const LC_AITheme& t = m_theme;
    QColor pill = t.accentBlue;
    pill.setAlphaF(t.dark ? 0.2 : 0.14);
    m_input->setMentionColors(t.accentBlue, pill);
    m_input->setPlaceholderText(tr("Ask anything, @ to mention the drawing (Enter to send, Shift+Enter for new line)"));

    // Cypher's .attach-btn, and the drop target look of the composer
    composer->setStyleSheet(composer->styleSheet() + QStringLiteral(R"(
#composer[dropping="true"] { border: 1px dashed %1; background: %2; }
#attachBtn, #mentionBtn { border: none; border-radius: 8px; background: transparent; min-width: 32px; max-width: 32px; min-height: 32px; max-height: 32px; }
#attachBtn:hover, #mentionBtn:hover { background: %3; }
#attachNotice { color: #f87171; font-size: 12px; }
)").arg(t.accentBlue.name(), css(t.accentBlueBg), css(t.bgHover)));

    // the attachment strip, above the text
    m_strip = new QWidget(composer);
    auto* stripLayout = new QVBoxLayout(m_strip);
    stripLayout->setContentsMargins(0, 0, 0, 0);
    stripLayout->setSpacing(4);
    m_tiles = new QWidget(m_strip);
    new FlowLayout(m_tiles, 8, false);
    stripLayout->addWidget(m_tiles);
    m_notice = new QLabel(m_strip);
    m_notice->setObjectName("attachNotice");
    m_notice->setWordWrap(true);
    m_notice->hide();
    stripLayout->addWidget(m_notice);
    m_strip->hide();
    m_noticeTimer = new QTimer(this);
    m_noticeTimer->setSingleShot(true);
    connect(m_noticeTimer, &QTimer::timeout, this, [this] {
        m_notice->hide();
        m_strip->setVisible(!m_attachments.isEmpty());
    });

    // paperclip and @, at the start of the actions row
    m_buttons = new QWidget(composer);
    auto* buttonsLayout = new QHBoxLayout(m_buttons);
    buttonsLayout->setContentsMargins(0, 0, 0, 0);
    buttonsLayout->setSpacing(2);
    m_attachButton = new QToolButton(m_buttons);
    m_attachButton->setObjectName("attachBtn");
    m_attachButton->setIcon(LC_AIIcons::lucideIcon("paperclip", 16, t.textMuted));
    m_attachButton->setIconSize(QSize(16, 16));
    m_attachButton->setToolTip(tr("Attach images or PDFs (or drop or paste them on the message box)"));
    m_attachButton->setCursor(Qt::PointingHandCursor);
    m_mentionButton = new QToolButton(m_buttons);
    m_mentionButton->setObjectName("mentionBtn");
    m_mentionButton->setIcon(LC_AIIcons::lucideIcon("at-sign", 16, t.textMuted));
    m_mentionButton->setIconSize(QSize(16, 16));
    m_mentionButton->setToolTip(tr("Mention the selection, the view, the drawing, a layer, a block or an entity (@)"));
    m_mentionButton->setCursor(Qt::PointingHandCursor);
    buttonsLayout->addWidget(m_attachButton);
    buttonsLayout->addWidget(m_mentionButton);
    connect(m_attachButton, &QToolButton::clicked, this, &LC_AIComposerExtras::openFileDialog);
    connect(m_mentionButton, &QToolButton::clicked, this, &LC_AIComposerExtras::startMention);

    // the @ popup floats over the conversation, above the composer (Cypher's .popover.up); the text keeps the focus
    m_popup = new QFrame(panel);
    m_popup->setObjectName("aiMentionPopover");
    m_popup->setFocusPolicy(Qt::NoFocus);
    m_popup->setStyleSheet(QStringLiteral(R"(
#aiMentionPopover { background: %1; border: 1px solid %2; border-radius: 10px; }
#aiMentionPopover QListWidget { background: transparent; border: none; outline: none; }
#aiMentionPopover QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
#aiMentionPopover QScrollBar::handle:vertical { background: %3; border-radius: 3px; min-height: 30px; }
#aiMentionPopover QScrollBar::add-line, #aiMentionPopover QScrollBar::sub-line, #aiMentionPopover QScrollBar::add-page, #aiMentionPopover QScrollBar::sub-page { background: none; height: 0; }
)").arg(t.bgElevated.name(), css(t.borderOverlay), css(t.borderStrong)));
    auto* shadow = new QGraphicsDropShadowEffect(m_popup);
    shadow->setBlurRadius(24);
    shadow->setOffset(0, 6);
    shadow->setColor(QColor(0, 0, 0, t.dark ? 110 : 45));
    m_popup->setGraphicsEffect(shadow);
    auto* popupLayout = new QVBoxLayout(m_popup);
    popupLayout->setContentsMargins(4, 4, 4, 4);
    m_list = new QListWidget(m_popup);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setMouseTracking(true);
    m_list->viewport()->setMouseTracking(true);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setItemDelegate(new MentionDelegate(m_theme, m_list));
    popupLayout->addWidget(m_list);
    m_popup->hide();
    connect(m_list, &QListWidget::itemEntered, this, [this](QListWidgetItem* item) {
        if (item->data(ROLE_INDEX).toInt() >= 0) {
            m_list->setCurrentItem(item);
        }
    });
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) { acceptCandidate(m_list->row(item)); });

    connect(input, &LC_AIComposerInput::filesOffered, this, &LC_AIComposerExtras::addFiles);
    connect(input, &LC_AIComposerInput::imageOffered, this, [this](const QImage& image) { addImage(image); });
    connect(input, &QPlainTextEdit::textChanged, this, &LC_AIComposerExtras::updatePopup);
    connect(input, &QPlainTextEdit::cursorPositionChanged, this, &LC_AIComposerExtras::updatePopup);
    // installed after the panel's own filter, so it runs first: Enter picks a mention while the popup is open
    m_viewport = m_input->viewport();
    m_input->installEventFilter(this);
    m_viewport->installEventFilter(this);
    composer->setAcceptDrops(true);
    composer->installEventFilter(this);
    panel->installEventFilter(this); // the popup follows the composer when the panel is resized
}

void LC_AIComposerExtras::addFiles(const QStringList& paths) {
    QStringList problems;
    for (const QString& path : paths) {
        if (m_attachments.size() >= LC_AIAttachments::MAX_ATTACHMENTS) {
            problems << tr("A message can have at most %1 files.").arg(LC_AIAttachments::MAX_ATTACHMENTS);
            break;
        }
        if (!LC_AIAttachments::isAttachable(path)) {
            problems << tr("%1: only images (PNG, JPEG, WebP, GIF, BMP) and PDFs can be attached.").arg(QFileInfo(path).fileName());
            continue;
        }
        LC_AIAttachment attachment;
        QString error;
        if (!LC_AIAttachments::fromFile(path, attachment, error)) {
            problems << error;
            continue;
        }
        // the same file twice would only be sent twice (a drop repeated by mistake)
        const auto same = std::find_if(m_attachments.cbegin(), m_attachments.cend(), [&attachment](const LC_AIAttachment& a) {
            return !a.fromMention && a.kind == attachment.kind && a.data == attachment.data;
        });
        if (same != m_attachments.cend()) {
            problems << (same->name == attachment.name ? tr("%1 is already attached.").arg(attachment.name)
                                                       : tr("%1 is already attached (as %2).").arg(attachment.name, same->name));
            continue;
        }
        m_attachments << attachment;
    }
    rebuildStrip();
    if (const QString note = blindModelNote(); !note.isEmpty() && !m_attachments.isEmpty()) {
        problems << note;
    }
    if (!problems.isEmpty()) {
        problems.removeDuplicates(); // one line for a file dropped five times
        showNotice(problems.join('\n'));
    }
    emit changed();
}

void LC_AIComposerExtras::addImage(const QImage& image, const QString& name) {
    if (m_attachments.size() >= LC_AIAttachments::MAX_ATTACHMENTS) {
        showNotice(tr("A message can have at most %1 files.").arg(LC_AIAttachments::MAX_ATTACHMENTS));
        return;
    }
    static int pasted = 0;
    LC_AIAttachment attachment;
    QString error;
    if (!LC_AIAttachments::fromImage(image, name.isEmpty() ? tr("pasted-%1.png").arg(++pasted) : name, attachment, error)) {
        showNotice(error);
        return;
    }
    m_attachments << attachment;
    rebuildStrip();
    if (const QString note = blindModelNote(); !note.isEmpty()) {
        showNotice(note);
    }
    emit changed();
}

void LC_AIComposerExtras::fill(LC_AIChatMessage& message) const {
    message.attachments = m_attachments;
    const QList<LC_AIMention> mentions = LC_AIMentions::find(message.text);
    if (!mentions.isEmpty()) {
        const LC_AIMentions::Expansion expansion = LC_AIMentions::expand(mentions);
        message.context = expansion.context;
        message.attachments += expansion.attachments;
    }
}

void LC_AIComposerExtras::clear() {
    m_attachments.clear();
    m_noticeTimer->stop();
    m_notice->hide();
    rebuildStrip();
    hidePopup();
    m_dismissedAt = -1;
    emit changed();
}

void LC_AIComposerExtras::restore(const QList<LC_AIAttachment>& attachments) {
    m_attachments.clear();
    for (const LC_AIAttachment& a : attachments) {
        if (!a.fromMention) { // the @view screenshot is taken again on the next send
            m_attachments << a;
        }
    }
    rebuildStrip();
    emit changed();
}

void LC_AIComposerExtras::insertMention(const QString& mention) {
    const QString text = m_input->toPlainText();
    QTextCursor cursor(m_input->document());
    cursor.movePosition(QTextCursor::End);
    // already there as a mention of its own (a text search would take "@entity #131" for "@entity #13")
    bool present = false;
    for (const LC_AIMention& m : m_input->mentions()) {
        present = present || m.text().compare(mention, Qt::CaseInsensitive) == 0;
    }
    if (!present) {
        const bool separate = !text.isEmpty() && !text.back().isSpace();
        cursor.insertText((separate ? QStringLiteral(" ") : QString()) + mention + " ");
    }
    m_input->setTextCursor(cursor);
    hidePopup();
    m_input->window()->activateWindow();
    m_input->setFocus(Qt::OtherFocusReason);
}

bool LC_AIComposerExtras::isPopupVisible() const {
    return m_popup != nullptr && m_popup->isVisible();
}

void LC_AIComposerExtras::openWithMention(QWidget* window, const QString& mention) {
    QWidget* top = window != nullptr ? window->window() : nullptr;
    if (top == nullptr) {
        return;
    }
    if (auto* toggle = top->findChild<QAction*>("AIAssistantToggle"); toggle != nullptr && !toggle->isChecked()) {
        toggle->setChecked(true); // opens the sidebar
    }
    if (auto* extras = top->findChild<LC_AIComposerExtras*>()) {
        extras->insertMention(mention);
    }
}

QWidget* LC_AIComposerExtras::messageRow(const QList<LC_AIAttachment>& attachments, const LC_AITheme& theme, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* flow = new FlowLayout(row, 6, true);
    flow->setContentsMargins(0, 10, 0, 0);
    int images = 0;
    for (const LC_AIAttachment& a : attachments) {
        images += a.isImage() ? 1 : 0;
    }
    for (const LC_AIAttachment& a : attachments) {
        if (!a.isImage()) {
            flow->addWidget(new AttachmentView(a, AttachmentView::Mode::Chip, {}, theme, row));
        }
        else if (images == 1) { // Cypher's .msg-user img: up to 240 px wide
            flow->addWidget(new AttachmentView(a, AttachmentView::Mode::Fit, QSize(240, 180), theme, row));
        }
        else {
            flow->addWidget(new AttachmentView(a, AttachmentView::Mode::Tile, QSize(72, 72), theme, row));
        }
    }
    return row;
}

void LC_AIComposerExtras::rebuildStrip() {
    QLayout* flow = m_tiles->layout();
    while (QLayoutItem* item = flow->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide();
            w->deleteLater(); // the remove button that asked for this may still be running
        }
        delete item;
    }
    const bool large = m_attachments.size() == 1 && m_attachments.first().isImage();
    const QSize edge = large ? QSize(LARGE_TILE_EDGE, LARGE_TILE_EDGE) : QSize(TILE_EDGE, TILE_EDGE);
    for (const LC_AIAttachment& a : std::as_const(m_attachments)) {
        auto* tile = new AttachmentView(a, AttachmentView::Mode::Tile, edge, m_theme, m_tiles);
        const QString id = a.id;
        tile->setRemovable([this, id] {
            for (int i = 0; i < m_attachments.size(); ++i) {
                if (m_attachments.at(i).id == id) {
                    m_attachments.removeAt(i);
                    break;
                }
            }
            rebuildStrip();
            emit changed();
            m_input->setFocus();
        });
        flow->addWidget(tile);
    }
    m_tiles->setVisible(!m_attachments.isEmpty());
    m_strip->setVisible(!m_attachments.isEmpty() || !m_notice->isHidden());
}

void LC_AIComposerExtras::showNotice(const QString& text) {
    m_notice->setText(text);
    m_notice->show();
    m_strip->show();
    m_noticeTimer->start(NOTICE_MS);
}

void LC_AIComposerExtras::openFileDialog() {
    static QString lastFolder;
    QString folder = lastFolder;
    if (folder.isEmpty()) { // the first time: the open drawing's folder (its references are often next to it), or home
        RS_GraphicView* view = LC_AITools::view();
        const RS_Graphic* graphic = view != nullptr ? view->getGraphic() : nullptr;
        const QString drawing = graphic != nullptr ? graphic->getFilename() : QString();
        folder = !drawing.isEmpty() && QFileInfo(drawing).absoluteDir().exists() ? QFileInfo(drawing).absolutePath() : QDir::homePath();
    }
    const QStringList files = QFileDialog::getOpenFileNames(m_panel->window(), tr("Attach images or PDFs"), folder,
                                                            LC_AIAttachments::fileDialogFilter());
    if (!files.isEmpty()) {
        lastFolder = QFileInfo(files.first()).absolutePath();
        addFiles(files);
    }
    m_input->setFocus();
}

void LC_AIComposerExtras::startMention() {
    m_input->setFocus();
    QTextCursor cursor = m_input->textCursor();
    const QString text = m_input->toPlainText();
    const int position = cursor.position();
    const bool separate = position > 0 && !text.at(position - 1).isSpace();
    m_dismissedAt = -1;
    cursor.insertText(separate ? QStringLiteral(" @") : QStringLiteral("@"));
    m_input->setTextCursor(cursor);
    updatePopup();
}

void LC_AIComposerExtras::updatePopup() {
    // Escape closed the popup for one "@": once that "@" is deleted, a new one typed in its place opens it again
    if (m_dismissedAt >= 0 && m_input->document()->characterAt(m_dismissedAt) != QLatin1Char('@')) {
        m_dismissedAt = -1;
    }
    const QTextCursor cursor = m_input->textCursor();
    if (cursor.hasSelection()) {
        hidePopup();
        return;
    }
    // the "@word" being typed at the cursor (Cypher's file completion), on its own or after a space
    const QString before = cursor.block().text().left(cursor.positionInBlock());
    const int at = int(before.lastIndexOf('@'));
    if (at < 0 || (at > 0 && !before.at(at - 1).isSpace() && before.at(at - 1) != '(')) {
        hidePopup();
        return;
    }
    const int atPosition = cursor.block().position() + at;
    const QString query = before.mid(at + 1);
    // a few words ("lay wal", "layer Doors and windows"); prose after them finds no row and closes the popup
    if (query.contains('\t') || query.count(' ') > 4) {
        hidePopup();
        return;
    }
    // a finished mention the user typed on from: nothing to complete
    for (const LC_AIMention& m : m_input->mentions()) {
        if (m.start == atPosition && cursor.position() > m.start + m.length) {
            hidePopup();
            return;
        }
    }
    if (atPosition == m_dismissedAt) {
        hidePopup();
        return;
    }
    m_dismissedAt = -1;
    m_candidates = LC_AIMentions::candidates(query);
    if (m_candidates.isEmpty()
        || (m_candidates.size() == 1 && m_candidates.first().text.compare("@" + query.trimmed(), Qt::CaseInsensitive) == 0)) {
        hidePopup();
        return;
    }
    m_atPosition = atPosition;
    m_list->clear();
    QString group;
    int firstRow = -1;
    for (int i = 0; i < m_candidates.size(); ++i) {
        const LC_AIMentions::Candidate& c = m_candidates.at(i);
        if (c.group != group) {
            group = c.group;
            auto* header = new QListWidgetItem(group, m_list);
            header->setData(ROLE_INDEX, -1);
            header->setFlags(Qt::NoItemFlags);
        }
        auto* item = new QListWidgetItem(c.text, m_list);
        item->setData(ROLE_INDEX, i);
        item->setData(ROLE_DETAIL, c.detail);
        item->setData(ROLE_ICON, c.icon);
        if (firstRow < 0) {
            firstRow = m_list->row(item);
        }
    }
    m_list->setCurrentRow(firstRow);
    placePopup();
    m_popup->show();
    m_popup->raise();
}

void LC_AIComposerExtras::hidePopup() {
    if (m_popup != nullptr && m_popup->isVisible()) {
        m_popup->hide();
    }
}

void LC_AIComposerExtras::placePopup() {
    int content = 8 + 2;
    for (int i = 0; i < m_list->count(); ++i) {
        content += m_list->item(i)->data(ROLE_INDEX).toInt() < 0 ? POPUP_HEADER_HEIGHT : POPUP_ROW_HEIGHT;
    }
    const QPoint composerTop = m_composer->mapTo(m_panel, QPoint(0, 0));
    const int width = qBound(qMin(220, m_composer->width()), m_composer->width(), 420);
    const int height = qMax(POPUP_ROW_HEIGHT * 2, std::min({content, POPUP_MAX_HEIGHT, composerTop.y() - 12}));
    m_popup->setGeometry(composerTop.x(), composerTop.y() - height - 6, width, height);
    if (QListWidgetItem* current = m_list->currentItem()) {
        m_list->scrollToItem(current);
    }
}

void LC_AIComposerExtras::moveHighlight(const int step) {
    const int count = m_list->count();
    int row = m_list->currentRow();
    for (int tries = 0; tries < count; ++tries) {
        row = (row + step + count) % count;
        if (m_list->item(row)->data(ROLE_INDEX).toInt() >= 0) {
            m_list->setCurrentRow(row);
            m_list->scrollToItem(m_list->item(row));
            return;
        }
    }
}

void LC_AIComposerExtras::acceptCandidate(const int row) {
    QListWidgetItem* item = m_list->item(row);
    const int index = item != nullptr ? item->data(ROLE_INDEX).toInt() : -1;
    if (index < 0 || index >= m_candidates.size() || m_atPosition < 0) {
        return;
    }
    const QString mention = m_candidates.at(index).text;
    QTextCursor cursor = m_input->textCursor();
    const int end = cursor.position();
    const QString text = m_input->toPlainText();
    const bool spaceFollows = end < text.size() && text.at(end).isSpace();
    cursor.setPosition(m_atPosition);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    cursor.insertText(spaceFollows ? mention : mention + " ");
    if (spaceFollows) {
        cursor.movePosition(QTextCursor::NextCharacter);
    }
    m_input->setTextCursor(cursor);
    hidePopup();
    m_input->setFocus();
}

void LC_AIComposerExtras::setDropping(const bool dropping) {
    if (m_composer->property("dropping").toBool() != dropping) {
        m_composer->setProperty("dropping", dropping);
        m_composer->style()->unpolish(m_composer);
        m_composer->style()->polish(m_composer);
    }
}

bool LC_AIComposerExtras::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_input && event->type() == QEvent::KeyPress && isPopupVisible()) {
        const auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
            case Qt::Key_Down:
                moveHighlight(1);
                return true;
            case Qt::Key_Up:
                moveHighlight(-1);
                return true;
            case Qt::Key_Return:
            case Qt::Key_Enter:
            case Qt::Key_Tab:
                if (!(key->modifiers() & Qt::ShiftModifier) && m_list->currentRow() >= 0) {
                    acceptCandidate(m_list->currentRow());
                    return true;
                }
                break;
            case Qt::Key_Escape:
                m_dismissedAt = m_atPosition;
                hidePopup();
                return true;
            default:
                break;
        }
    }
    if (watched == m_input && event->type() == QEvent::FocusOut) {
        QTimer::singleShot(150, this, [this] {
            if (m_input && !m_input->hasFocus()) {
                hidePopup();
            }
        });
    }
    if (watched == m_panel && event->type() == QEvent::Resize && isPopupVisible()) {
        QTimer::singleShot(0, this, &LC_AIComposerExtras::placePopup);
    }
    // files dragged onto the message box or the composer around it
    if (watched == m_viewport || watched == m_composer) {
        switch (event->type()) {
            case QEvent::DragEnter:
            case QEvent::DragMove: {
                auto* drag = static_cast<QDragMoveEvent*>(event);
                const QMimeData* mime = drag->mimeData();
                bool files = false;
                for (const QUrl& url : mime->urls()) {
                    files = files || url.isLocalFile();
                }
                if (files || (mime->hasImage() && !mime->hasText())) {
                    drag->acceptProposedAction();
                    setDropping(true);
                    return true;
                }
                break;
            }
            case QEvent::DragLeave:
                setDropping(false);
                break;
            case QEvent::Drop: {
                auto* drop = static_cast<QDropEvent*>(event);
                const QMimeData* mime = drop->mimeData();
                setDropping(false);
                QStringList files;
                for (const QUrl& url : mime->urls()) {
                    if (url.isLocalFile()) {
                        files << url.toLocalFile();
                    }
                }
                if (!files.isEmpty()) {
                    addFiles(files); // says which ones cannot be attached
                    drop->acceptProposedAction();
                    m_input->setFocus();
                    return true;
                }
                if (mime->hasImage() && !mime->hasText()) {
                    addImage(qvariant_cast<QImage>(mime->imageData()));
                    drop->acceptProposedAction();
                    return true;
                }
                break;
            }
            default:
                break;
        }
    }
    return QObject::eventFilter(watched, event);
}
