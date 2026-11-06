/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aichathistorypopup.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

#include "lc_aichatstore.h"
#include "lc_aiicons.h"

namespace {
    constexpr int ROLE_ID = Qt::UserRole;          // the chat's id
    constexpr int ROLE_KIND = Qt::UserRole + 1;    // Kind
    constexpr int ROLE_DETAIL = Qt::UserRole + 2;  // a chat's date and size; a drawing's chat count
    enum Kind { Chat = 0, Header = 1, Note = 2 };
    constexpr int ROW_HEIGHT = 44;
    constexpr int HEADER_HEIGHT = 28;
    constexpr int NOTE_HEIGHT = 72;
    const QColor DANGER("#f87171"); // --accent-red

    QString css(const QColor& c) {
        return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF(), 0, 'f', 3);
    }

    QFont pxFont(const QFont& base, const double px, const QFont::Weight weight = QFont::Normal) {
        QFont f(base);
        f.setPixelSize(qRound(px));
        f.setWeight(weight);
        return f;
    }

    QRect binRect(const QRect& row) {
        return {row.right() - 6 - 24 + 1, row.center().y() - 12, 24, 24};
    }

    QRect pencilRect(const QRect& row) {
        return binRect(row).translated(-26, 0);
    }

    /** "Today 14:05", "Yesterday 09:12", "Mon 18:30", "3 Oct", "3 Oct 2025". */
    QString when(const QDateTime& utc) {
        const QDateTime t = utc.toLocalTime();
        const QDate today = QDate::currentDate();
        const QLocale locale;
        const QString time = locale.toString(t.time(), QLocale::ShortFormat);
        if (t.date() == today) {
            return QObject::tr("Today %1").arg(time);
        }
        if (t.date() == today.addDays(-1)) {
            return QObject::tr("Yesterday %1").arg(time);
        }
        if (t.date() > today.addDays(-7)) {
            return locale.dayName(t.date().dayOfWeek(), QLocale::ShortFormat) + " " + time;
        }
        return locale.toString(t.date(), t.date().year() == today.year() ? QStringLiteral("d MMM") : QStringLiteral("d MMM yyyy"));
    }
}

/** Paints a drawing's header row and the chat rows: icon, title over "date · N messages", check, pencil and bin. */
class LC_AIChatHistoryDelegate : public QStyledItemDelegate {
public:
    explicit LC_AIChatHistoryDelegate(LC_AIChatHistoryPopup* popup) : QStyledItemDelegate(popup), m_popup(popup) {}

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override {
        const int kind = index.data(ROLE_KIND).toInt();
        return {100, kind == Header ? HEADER_HEIGHT : kind == Note ? NOTE_HEIGHT : ROW_HEIGHT};
    }

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const LC_AITheme& t = m_popup->m_theme;
        const qreal dpr = p->device()->devicePixelRatioF();
        const QRect r = option.rect;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setRenderHint(QPainter::TextAntialiasing);
        const int kind = index.data(ROLE_KIND).toInt();

        if (kind == Note) { // nothing to list
            p->setFont(pxFont(option.font, 12.5));
            p->setPen(t.textFaint);
            p->drawText(r.adjusted(16, 0, -16, 0), Qt::AlignCenter | Qt::TextWordWrap, index.data(Qt::DisplayRole).toString());
            p->restore();
            return;
        }

        if (kind == Header) { // the drawing: file icon, name, number of chats
            int x = r.left() + 8;
            p->drawPixmap(x, r.center().y() - 6 + 2, LC_AIIcons::lucide("file", 12, t.textFaint, dpr));
            x += 12 + 7;
            const QFont font = pxFont(option.font, 11.5, QFont::Medium);
            const QFontMetrics fm(font);
            const QString count = index.data(ROLE_DETAIL).toString();
            const int countWidth = fm.horizontalAdvance(count);
            p->setFont(font);
            p->setPen(t.textFaint);
            const QRect textRow(r.left(), r.top() + 4, r.width(), r.height() - 4); // a little closer to its chats
            p->drawText(QRect(r.right() - 8 - countWidth, textRow.top(), countWidth, textRow.height()),
                        Qt::AlignVCenter | Qt::AlignRight, count);
            const int width = qMax(20, r.right() - 8 - countWidth - 10 - x);
            p->setPen(t.textMuted);
            p->drawText(QRect(x, textRow.top(), width, textRow.height()), Qt::AlignVCenter | Qt::AlignLeft,
                        fm.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle, width));
            p->restore();
            return;
        }

        const QString id = index.data(ROLE_ID).toString();
        const bool highlight = m_popup->m_list->currentIndex() == index;
        const bool current = id == m_popup->m_current;
        const bool armed = id == m_popup->m_armed;
        if (highlight || armed) {
            QPainterPath bg;
            bg.addRoundedRect(QRectF(r), 7, 7);
            p->fillPath(bg, t.bgHover);
        }
        int x = r.left() + 10;
        p->drawPixmap(x, r.center().y() - 7, LC_AIIcons::lucide("message-square", 14, current ? t.textHeading : t.textMuted, dpr));
        x += 14 + 10;

        int right = r.right() - 8;
        if (highlight || armed) { // the row's actions: rename and delete (a second click confirms)
            const QRect bin = binRect(r);
            p->drawPixmap(bin.center().x() - 6, bin.center().y() - 6, LC_AIIcons::lucide("trash-2", 13, armed ? DANGER : t.textMuted, dpr));
            right = bin.left() - 2;
            if (armed) {
                const QFont font = pxFont(option.font, 12, QFont::Medium);
                const QString text = LC_AIChatHistoryPopup::tr("Delete?");
                const int width = QFontMetrics(font).horizontalAdvance(text);
                p->setFont(font);
                p->setPen(DANGER);
                p->drawText(QRect(right - width, r.top(), width, r.height()), Qt::AlignVCenter | Qt::AlignRight, text);
                right -= width + 10;
            }
            else {
                const QRect pencil = pencilRect(r);
                p->drawPixmap(pencil.center().x() - 6, pencil.center().y() - 6, LC_AIIcons::lucide("pencil", 13, t.textMuted, dpr));
                right = pencil.left() - 4;
            }
        }
        if (current) { // the chat the panel shows
            p->drawPixmap(right - 13 + 1, r.center().y() - 6, LC_AIIcons::lucide("check", 13, t.textHeading, dpr));
            right -= 13 + 10;
        }

        if (id == m_popup->m_editing) { // the name box covers the text
            p->restore();
            return;
        }
        const int textWidth = qMax(20, right - x);
        const QFont titleFont = pxFont(option.font, 13);
        const QFont smallFont = pxFont(option.font, 11.5);
        const int titleHeight = QFontMetrics(titleFont).height();
        const int smallHeight = QFontMetrics(smallFont).height();
        const int top = r.center().y() - (titleHeight + smallHeight) / 2;
        p->setFont(titleFont);
        p->setPen(current ? t.textHeading : highlight ? t.textPrimary : t.textSecondary);
        p->drawText(QRect(x, top, textWidth, titleHeight), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(titleFont).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, textWidth));
        p->setFont(smallFont);
        p->setPen(t.textFaint);
        p->drawText(QRect(x, top + titleHeight, textWidth, smallHeight), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(smallFont).elidedText(index.data(ROLE_DETAIL).toString(), Qt::ElideRight, textWidth));
        p->restore();
    }

    bool editorEvent(QEvent* event, QAbstractItemModel*, const QStyleOptionViewItem& option, const QModelIndex& index) override {
        if (event->type() != QEvent::MouseButtonRelease || index.data(ROLE_KIND).toInt() != Chat) {
            return false;
        }
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton) {
            return false;
        }
        const QPoint pos = mouse->position().toPoint();
        if (binRect(option.rect).contains(pos)) {
            m_popup->remove(index.row());
        }
        else if (pencilRect(option.rect).contains(pos) && m_popup->m_armed.isEmpty()) {
            m_popup->rename(index.row());
        }
        else {
            m_popup->resume(index.row());
        }
        return true;
    }

    // renaming happens in place, in a box over the title
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex&) const override {
        auto* edit = new QLineEdit(parent);
        edit->setObjectName("hpRename");
        edit->setPlaceholderText(LC_AIChatHistoryPopup::tr("Name of the chat (empty: its first question)"));
        return edit;
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        auto* edit = static_cast<QLineEdit*>(editor);
        edit->setText(index.data(Qt::DisplayRole).toString());
        edit->selectAll();
    }

    void setModelData(QWidget* editor, QAbstractItemModel*, const QModelIndex& index) const override {
        const QString id = index.data(ROLE_ID).toString();
        const QString name = static_cast<QLineEdit*>(editor)->text();
        if (name == index.data(Qt::DisplayRole).toString()) {
            return;
        }
        // the list is rebuilt after the editor has closed
        LC_AIChatHistoryPopup* popup = m_popup;
        QTimer::singleShot(0, popup, [popup, id, name] {
            LC_AIChatStore::instance().rename(id, name);
            popup->refresh();
        });
    }

    void destroyEditor(QWidget* editor, const QModelIndex& index) const override {
        m_popup->m_editing.clear();
        QStyledItemDelegate::destroyEditor(editor, index);
        // the list takes no focus, so the keys would go nowhere: back to the search box once the editor has closed
        LC_AIChatHistoryPopup* popup = m_popup;
        QTimer::singleShot(0, popup, [popup] { popup->m_search->setFocus(); });
    }

    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex&) const override {
        const QRect r = option.rect;
        editor->setGeometry(r.left() + 30, r.center().y() - 14, r.width() - 30 - 8, 28);
    }

private:
    LC_AIChatHistoryPopup* m_popup;
};

LC_AIChatHistoryPopup::LC_AIChatHistoryPopup(const LC_AITheme& theme, const QString& drawing, const QString& drawingName,
                                             const QString& current, const QSet<QString>& open, QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint), m_theme(theme), m_drawing(drawing), m_drawingName(drawingName),
      m_current(current), m_open(open) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setFixedSize(360, 420);

    const LC_AITheme& t = m_theme;
    setStyleSheet(QStringLiteral(R"(
#hpPopover { background: %1; border: 1px solid %2; border-radius: 10px; }
#hpTop { border: none; border-bottom: 1px solid %3; background: transparent; }
#hpTop QLineEdit { border: none; background: transparent; font-size: 13px; color: %4; padding: 0; }
#hpTabs { background: %5; border: none; border-radius: 7px; }
#hpTab { border: none; border-radius: 5px; background: transparent; padding: 0 8px; min-height: 22px; max-height: 22px; font-size: 11.5px; color: %6; }
#hpTab:hover { color: %4; }
#hpTab:checked { background: %7; color: %8; }
QListWidget { background: transparent; border: none; outline: none; padding: 4px 6px 6px 6px; }
#hpRename { background: %1; border: 1px solid %9; border-radius: 6px; padding: 0 7px; font-size: 13px; color: %4; }
QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
QScrollBar::handle:vertical { background: %10; border-radius: 3px; min-height: 30px; }
QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page { background: none; height: 0; }
)")
                      .arg(t.bgElevated.name(), css(t.borderOverlay), css(t.borderDefault), t.textPrimary.name(), css(t.bgSubtle),
                           t.textMuted.name(), css(t.bgActive), t.textHeading.name(), css(t.accentBlueBorder))
                      .arg(css(t.borderStrong)));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* popover = new QFrame(this);
    popover->setObjectName("hpPopover");
    outer->addWidget(popover);
    auto* layout = new QVBoxLayout(popover);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    // the search box, with "This drawing | All drawings" on its right
    auto* top = new QFrame(popover);
    top->setObjectName("hpTop");
    top->setFixedHeight(42);
    auto* topLayout = new QHBoxLayout(top);
    topLayout->setContentsMargins(12, 0, 8, 0);
    topLayout->setSpacing(8);
    auto* searchIcon = new QLabel(top);
    searchIcon->setPixmap(LC_AIIcons::lucide("search", 13, t.textMuted, devicePixelRatioF()));
    m_search = new QLineEdit(top);
    m_search->setPlaceholderText(tr("Search chats"));
    QPalette pal = m_search->palette();
    pal.setColor(QPalette::PlaceholderText, t.textPlaceholder);
    pal.setColor(QPalette::Text, t.textPrimary);
    m_search->setPalette(pal);
    m_search->installEventFilter(this);
    auto* tabs = new QFrame(top);
    tabs->setObjectName("hpTabs");
    auto* tabsLayout = new QHBoxLayout(tabs);
    tabsLayout->setContentsMargins(2, 2, 2, 2);
    tabsLayout->setSpacing(2);
    const auto tab = [this, tabs, tabsLayout](const QString& text, const QString& tip, const bool all) {
        auto* b = new QPushButton(text, tabs);
        b->setObjectName("hpTab");
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, [this, all] { setAllDrawings(all); });
        tabsLayout->addWidget(b);
        return b;
    };
    m_thisTab = tab(tr("This drawing"), tr("The chats of the drawing you are working on"), false);
    m_allTab = tab(tr("All drawings"), tr("Every saved chat, by drawing"), true);
    topLayout->addWidget(searchIcon);
    topLayout->addWidget(m_search, 1);
    topLayout->addWidget(tabs);
    layout->addWidget(top);

    m_list = new QListWidget(popover);
    m_list->setItemDelegate(new LC_AIChatHistoryDelegate(this));
    m_list->setMouseTracking(true);
    m_list->setFocusPolicy(Qt::NoFocus); // the search box keeps the keys
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers); // the pencil (or F2) renames
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(m_list, &QListWidget::itemEntered, this, [this](QListWidgetItem* item) {
        if (item->data(ROLE_KIND).toInt() == Chat) {
            if (item->data(ROLE_ID).toString() != m_armed) {
                disarm(); // a delete waits for its second click only on its own row
            }
            m_list->setCurrentItem(item);
            m_list->viewport()->update();
        }
    });
    layout->addWidget(m_list, 1);

    connect(m_search, &QLineEdit::textChanged, this, &LC_AIChatHistoryPopup::refresh);
    refresh();
}

void LC_AIChatHistoryPopup::setAllDrawings(const bool all) {
    m_all = all;
    disarm();
    refresh();
    m_search->setFocus();
}

void LC_AIChatHistoryPopup::disarm() {
    if (!m_armed.isEmpty()) {
        m_armed.clear();
        m_list->viewport()->update();
    }
}

void LC_AIChatHistoryPopup::refresh() {
    m_thisTab->setChecked(!m_all);
    m_allTab->setChecked(m_all);
    const QString needle = m_search->text().trimmed().toLower();
    const LC_AIChatStore& store = LC_AIChatStore::instance();
    const QList<LC_AIChatInfo> chats = m_all ? store.allChats() : store.chats(m_drawing);

    // the matching chats grouped by drawing, the drawing with the newest chat first
    QStringList order;
    QHash<QString, QList<LC_AIChatInfo>> groups;
    for (const LC_AIChatInfo& c : chats) {
        if (!needle.isEmpty() && !QString(c.title + "\n" + c.drawingName + "\n" + c.questions).toLower().contains(needle)) {
            continue;
        }
        if (!groups.contains(c.drawing)) {
            order << c.drawing;
        }
        groups[c.drawing] << c;
    }
    if (!m_all && order.isEmpty() && needle.isEmpty()) {
        order << m_drawing; // the drawing's header, over the note that it has no chats yet
    }

    m_list->clear();
    QListWidgetItem* currentItem = nullptr;
    QListWidgetItem* firstChat = nullptr;
    for (const QString& drawing : std::as_const(order)) {
        const QList<LC_AIChatInfo> list = groups.value(drawing);
        QString name = drawing == m_drawing ? m_drawingName : list.isEmpty() ? QString() : list.first().drawingName;
        if (name.isEmpty()) {
            name = drawing.isEmpty() ? tr("No drawing open") : tr("Unsaved drawing");
        }
        // an unsaved drawing that is not open was closed without saving; its name ("unnamed document 1") may be
        // the very name of the drawing open now
        const bool gone = LC_AIChatStore::isUnsavedKey(drawing) && drawing != m_drawing && !m_open.contains(drawing);
        if (gone) {
            name = tr("%1 (closed, never saved)").arg(name);
        }
        auto* header = new QListWidgetItem(m_list);
        header->setData(ROLE_KIND, Header);
        header->setData(Qt::DisplayRole, name);
        header->setData(ROLE_DETAIL, list.isEmpty() ? QString() : list.size() == 1 ? tr("1 chat") : tr("%1 chats").arg(list.size()));
        header->setToolTip(drawing.isEmpty()                         ? tr("Chats made while no drawing was open")
                           : gone                                  ? tr("A drawing that was closed without being saved: only its chats are left")
                           : LC_AIChatStore::isUnsavedKey(drawing) ? tr("A drawing that has not been saved")
                                                                     : drawing);
        header->setFlags(Qt::NoItemFlags);
        for (const LC_AIChatInfo& c : list) {
            auto* item = new QListWidgetItem(m_list);
            item->setData(ROLE_KIND, Chat);
            item->setData(ROLE_ID, c.id);
            item->setData(Qt::DisplayRole, c.title.isEmpty() ? tr("Untitled chat") : c.title);
            item->setData(ROLE_DETAIL, QString(when(c.updated) + QStringLiteral("  ·  ")
                                               + (c.count == 1 ? tr("1 message") : tr("%1 messages").arg(c.count))));
            item->setToolTip(c.title);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsEditable);
            firstChat = firstChat != nullptr ? firstChat : item;
            if (c.id == m_current) {
                currentItem = item;
            }
        }
    }
    if (firstChat == nullptr) {
        auto* note = new QListWidgetItem(!needle.isEmpty() ? tr("No chats match")
                                         : m_all          ? tr("No saved chats yet")
                                                          : tr("No chats for this drawing yet.\nWhat you ask the AI here is kept here."),
                                         m_list);
        note->setData(ROLE_KIND, Note);
        note->setFlags(Qt::NoItemFlags);
        return;
    }
    // the shown chat is highlighted when it is listed (search results start at the top)
    QListWidgetItem* highlight = needle.isEmpty() && currentItem != nullptr ? currentItem : firstChat;
    m_list->setCurrentItem(highlight);
    m_list->scrollToItem(highlight);
}

void LC_AIChatHistoryPopup::resume(const int row) {
    QListWidgetItem* item = m_list->item(row);
    if (item == nullptr || item->data(ROLE_KIND).toInt() != Chat) {
        return;
    }
    const QString id = item->data(ROLE_ID).toString();
    close();
    if (id != m_current) {
        emit resumeRequested(id);
    }
}

void LC_AIChatHistoryPopup::rename(const int row) {
    QListWidgetItem* item = m_list->item(row);
    if (item == nullptr || item->data(ROLE_KIND).toInt() != Chat) {
        return;
    }
    disarm();
    // once the click that asked for it has been handled
    const QString id = item->data(ROLE_ID).toString();
    QTimer::singleShot(0, this, [this, row, id] {
        QListWidgetItem* it = m_list->item(row);
        if (it != nullptr && it->data(ROLE_ID).toString() == id) {
            m_editing = id;
            m_list->setCurrentItem(it);
            m_list->editItem(it);
        }
    });
}

void LC_AIChatHistoryPopup::remove(const int row) {
    QListWidgetItem* item = m_list->item(row);
    if (item == nullptr || item->data(ROLE_KIND).toInt() != Chat) {
        return;
    }
    const QString id = item->data(ROLE_ID).toString();
    if (m_armed != id) { // the first click asks, the second deletes
        m_armed = id;
        m_list->viewport()->update();
        return;
    }
    m_armed.clear();
    LC_AIChatStore::instance().remove(id);
    emit deleted(id);
    refresh();
    m_search->setFocus();
}

bool LC_AIChatHistoryPopup::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_search && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) {
            const int count = m_list->count();
            int row = m_list->currentRow();
            for (int step = 0; step < count; ++step) { // wrap around, skipping the drawings' headers
                row = (row + (key->key() == Qt::Key_Down ? 1 : count - 1)) % count;
                if (m_list->item(row)->data(ROLE_KIND).toInt() == Chat) {
                    disarm();
                    m_list->setCurrentRow(row);
                    m_list->scrollToItem(m_list->item(row));
                    m_list->viewport()->update();
                    break;
                }
            }
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            resume(m_list->currentRow());
            return true;
        }
        if (key->key() == Qt::Key_F2) {
            rename(m_list->currentRow());
            return true;
        }
        // Delete as the bin: a first press asks, a second deletes; while there is text after the cursor, it edits the search
        if (key->key() == Qt::Key_Delete && key->modifiers() == Qt::NoModifier && !m_search->hasSelectedText()
            && m_search->cursorPosition() == m_search->text().size()) {
            remove(m_list->currentRow());
            return true;
        }
        if (key->key() == Qt::Key_Escape && !m_armed.isEmpty()) {
            disarm(); // the question goes, the list stays
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void LC_AIChatHistoryPopup::popup(QWidget* anchor) {
    const QPoint bottomRight = anchor->mapToGlobal(QPoint(anchor->width(), anchor->height()));
    const QPoint topRight = anchor->mapToGlobal(QPoint(anchor->width(), 0));
    const QRect screen = anchor->screen() != nullptr ? anchor->screen()->availableGeometry() : QRect();
    QPoint pos(bottomRight.x() - width(), bottomRight.y() + 4);
    if (!screen.isNull()) {
        if (pos.y() + height() > screen.bottom()) {
            pos.setY(topRight.y() - height() - 4); // no room below: open above
        }
        pos.setX(qBound(screen.left(), pos.x(), screen.right() - width()));
    }
    move(pos);
    show();
    m_search->setFocus();
    // the list was filled before it had its size: from the top now, so the drawing's header stays above its chats
    if (QListWidgetItem* highlight = m_list->currentItem()) {
        m_list->scrollToTop();
        m_list->scrollToItem(highlight);
    }
}
