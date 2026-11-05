/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aimodelpicker.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>

#include "lc_aicredentials.h"
#include "lc_aiicons.h"
#include "lc_aiprovider.h"

namespace {
    const QString FAVORITES_TAB = QStringLiteral("__favorites__");
    constexpr int ROLE_ENTRY = Qt::UserRole;        // index into m_entries, -1 for a group header
    constexpr int ROLE_NUMBER = Qt::UserRole + 1;   // 0-based position among model rows (for Ctrl+1..9)
    constexpr int ROW_HEIGHT = 42;
    constexpr int HEADER_HEIGHT = 26;
    const QColor STAR_ON("#fbbf24");                // --accent-yellow

    QString css(const QColor& c) {
        return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF(), 0, 'f', 3);
    }

    QColor over(const QColor& fg, const QColor& bg) {
        const double a = fg.alphaF();
        return QColor::fromRgbF(fg.redF() * a + bg.redF() * (1 - a), fg.greenF() * a + bg.greenF() * (1 - a),
                                fg.blueF() * a + bg.blueF() * (1 - a));
    }

    QFont pxFont(const QFont& base, const double px, const QFont::Weight weight = QFont::Normal) {
        QFont f(base);
        f.setPixelSize(qRound(px));
        f.setWeight(weight);
        return f;
    }
}

/** Paints Cypher's .mp-row: logo, name over "provider · ctx · reasoning · images", check, Ctrl+N, star. */
class LC_AIModelPickerDelegate : public QStyledItemDelegate {
public:
    explicit LC_AIModelPickerDelegate(LC_AIModelPicker* picker) : QStyledItemDelegate(picker), m_picker(picker) {}

    static QRect starRect(const QRect& row) {
        return {row.right() - 8 - 22 + 1, row.center().y() - 11, 22, 22};
    }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override {
        return {100, index.data(ROLE_ENTRY).toInt() < 0 ? HEADER_HEIGHT : ROW_HEIGHT};
    }

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const LC_AITheme& t = m_picker->m_theme;
        const qreal dpr = p->device()->devicePixelRatioF();
        const QRect r = option.rect;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setRenderHint(QPainter::TextAntialiasing);

        const int entryIndex = index.data(ROLE_ENTRY).toInt();
        if (entryIndex < 0) { // .mp-group: "Recent. Star a model to keep it here."
            const int x = r.left() + 8;
            p->drawPixmap(x, r.center().y() - 5, LC_AIIcons::lucide("clock", 11, t.textFaint, dpr));
            p->setFont(pxFont(option.font, 11.5));
            p->setPen(t.textFaint);
            p->drawText(QRect(x + 17, r.top(), r.width() - 25, r.height()), Qt::AlignVCenter | Qt::AlignLeft,
                        index.data(Qt::DisplayRole).toString());
            p->restore();
            return;
        }

        const LC_AIModelPicker::Entry& e = m_picker->m_entries.at(entryIndex);
        const bool highlight = m_picker->m_list->currentIndex() == index;
        const bool active = e.key() == m_picker->m_currentKey;
        const bool favorite = m_picker->m_favorites.contains(e.key());
        const int number = index.data(ROLE_NUMBER).toInt();

        if (highlight) {
            QPainterPath bg;
            bg.addRoundedRect(QRectF(r), 7, 7);
            p->fillPath(bg, t.bgHover);
        }

        int x = r.left() + 8;
        p->drawPixmap(x + 1, r.center().y() - 7, LC_AIIcons::provider(e.provider, 14, t.textMuted, dpr));
        x += 16 + 10;

        int right = r.right() - 8;
        if (favorite || highlight) { // .mp-star (visible on hover, always when on)
            const QRect star = starRect(r);
            p->drawPixmap(star.center().x() - 6, star.center().y() - 6,
                          LC_AIIcons::lucide("star", 12, favorite ? STAR_ON : t.textFaint, dpr, favorite));
        }
        right -= 22 + 10;
        if (number < 9) { // .mp-kbd
            const QFont kbdFont = pxFont(option.font, 10.5);
            const QString kbd = QStringLiteral("Ctrl+%1").arg(number + 1);
            const int w = QFontMetrics(kbdFont).horizontalAdvance(kbd) + 10;
            const QRectF box(right - w + 1, r.center().y() - 8.5, w, 17);
            p->setPen(QPen(over(t.borderDefault, t.bgElevated), 1));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
            p->setFont(kbdFont);
            p->setPen(t.textFaint);
            p->drawText(box, Qt::AlignCenter, kbd);
            right -= w + 10;
        }
        if (active) { // .mp-row-check
            p->drawPixmap(right - 13 + 1, r.center().y() - 6, LC_AIIcons::lucide("check", 13, t.textHeading, dpr));
            right -= 13 + 10;
        }

        const int textWidth = qMax(20, right - x);
        const QFont nameFont = pxFont(option.font, 13);
        const QFont smallFont = pxFont(option.font, 11.5);
        QStringList detail{e.provider};
        if (e.contextWindow > 0) {
            detail << LC_AIProvider::compactNumber(e.contextWindow) + " ctx";
        }
        if (e.reasoning) {
            detail << "reasoning";
        }
        if (e.images) {
            detail << "images";
        }
        const int nameH = QFontMetrics(nameFont).height();
        const int smallH = QFontMetrics(smallFont).height();
        const int top = r.center().y() - (nameH + smallH) / 2;
        p->setFont(nameFont);
        p->setPen(active ? t.textHeading : highlight ? t.textPrimary : t.textSecondary);
        p->drawText(QRect(x, top, textWidth, nameH), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(nameFont).elidedText(e.name, Qt::ElideRight, textWidth));
        p->setFont(smallFont);
        p->setPen(t.textFaint);
        p->drawText(QRect(x, top + nameH, textWidth, smallH), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(smallFont).elidedText(detail.join(" · "), Qt::ElideRight, textWidth));
        p->restore();
    }

    bool editorEvent(QEvent* event, QAbstractItemModel*, const QStyleOptionViewItem& option,
                     const QModelIndex& index) override {
        if (event->type() != QEvent::MouseButtonRelease || index.data(ROLE_ENTRY).toInt() < 0) {
            return false;
        }
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton) {
            return false;
        }
        if (starRect(option.rect).contains(mouse->position().toPoint())) {
            m_picker->toggleFavorite(index.row());
        }
        else {
            m_picker->pick(index.row());
        }
        return true;
    }

private:
    LC_AIModelPicker* m_picker;
};

LC_AIModelPicker::LC_AIModelPicker(const LC_AITheme& theme, QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint), m_theme(theme) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setFixedSize(460, 380);

    const LC_AITheme& t = m_theme;
    setStyleSheet(QStringLiteral(R"(
#mpPopover { background: %1; border: 1px solid %2; border-radius: 10px; }
#mpRail { background: %3; border: none; border-right: 1px solid %4; border-top-left-radius: 10px; border-bottom-left-radius: 10px; }
#mpRailItem { border: none; border-radius: 7px; background: transparent; min-width: 32px; max-width: 32px; min-height: 32px; max-height: 32px; }
#mpRailItem:hover { background: %5; }
#mpRailItem[on="true"] { background: %6; }
#mpRailSep { background: %7; border: none; }
#mpSearch { border: none; border-bottom: 1px solid %7; background: transparent; }
#mpSearch QLineEdit { border: none; background: transparent; font-size: 13px; color: %8; padding: 0; }
QListWidget { background: transparent; border: none; outline: none; padding: 6px; }
QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
QScrollBar::handle:vertical { background: %9; border-radius: 3px; min-height: 30px; }
QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page { background: none; height: 0; }
#mpEmpty { font-size: 12.5px; color: %10; }
QPushButton[kind="accent"] { background: %11; color: %12; border: none; border-radius: 8px; padding: 0 12px; min-height: 30px; font-size: 13px; font-weight: 500; }
)")
                      .arg(t.bgElevated.name(), css(t.borderOverlay), over(QColor(t.bgRoot.red(), t.bgRoot.green(), t.bgRoot.blue(), 102), t.bgElevated).name(),
                           css(t.borderSubtle), css(t.bgHover), css(t.bgActive), css(t.borderDefault), t.textPrimary.name(),
                           css(t.borderStrong))
                      .arg(t.textFaint.name(), t.textHeading.name(), t.bgRoot.name()));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* popover = new QFrame(this);
    popover->setObjectName("mpPopover");
    outer->addWidget(popover);
    auto* row = new QHBoxLayout(popover);
    row->setContentsMargins(1, 1, 1, 1);
    row->setSpacing(0);

    // .mp-rail
    auto* rail = new QFrame(popover);
    rail->setObjectName("mpRail");
    rail->setFixedWidth(44);
    auto* railScroll = new QScrollArea(rail);
    railScroll->setWidgetResizable(true);
    railScroll->setFrameShape(QFrame::NoFrame);
    railScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    railScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // scrollbar-width: none
    railScroll->setStyleSheet("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }");
    auto* railContent = new QWidget();
    m_railLayout = new QVBoxLayout(railContent);
    m_railLayout->setContentsMargins(6, 6, 6, 6);
    m_railLayout->setSpacing(2);
    railScroll->setWidget(railContent);
    auto* railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(0, 0, 0, 0);
    railLayout->addWidget(railScroll);
    row->addWidget(rail);

    // .mp-main
    auto* main = new QWidget(popover);
    auto* mainLayout = new QVBoxLayout(main);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    auto* search = new QFrame(main);
    search->setObjectName("mpSearch");
    search->setFixedHeight(40);
    auto* searchLayout = new QHBoxLayout(search);
    searchLayout->setContentsMargins(0, 0, 0, 0);
    searchLayout->setSpacing(8);
    auto* searchIcon = new QLabel(search);
    searchIcon->setPixmap(LC_AIIcons::lucide("search", 13, t.textMuted, devicePixelRatioF()));
    m_search = new QLineEdit(search);
    m_search->setPlaceholderText(tr("Search models"));
    QPalette pal = m_search->palette();
    pal.setColor(QPalette::PlaceholderText, t.textPlaceholder);
    m_search->setPalette(pal);
    m_search->installEventFilter(this);
    searchLayout->addWidget(searchIcon);
    searchLayout->addWidget(m_search, 1);
    auto* searchWrap = new QHBoxLayout();
    searchWrap->setContentsMargins(10, 0, 10, 0);
    searchWrap->addWidget(search);
    mainLayout->addLayout(searchWrap);

    m_list = new QListWidget(main);
    m_list->setItemDelegate(new LC_AIModelPickerDelegate(this));
    m_list->setMouseTracking(true);
    m_list->setFocusPolicy(Qt::NoFocus); // the search box keeps focus
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setUniformItemSizes(false);
    m_list->setContentsMargins(0, 0, 0, 0);
    connect(m_list, &QListWidget::itemEntered, this, [this](QListWidgetItem* item) {
        if (item->data(ROLE_ENTRY).toInt() >= 0) {
            m_list->setCurrentItem(item);
            m_list->viewport()->update();
        }
    });
    mainLayout->addWidget(m_list, 1);
    row->addWidget(main, 1);

    LC_AICredentials& creds = LC_AICredentials::instance();
    m_favorites = creds.favoriteModels();
    const QString selected = creds.selectedProvider();
    if (creds.isConnected(selected)) {
        m_currentKey = selected + "/" + creds.model(selected);
    }

    buildEntries();
    if (m_entries.isEmpty()) {
        // "No models with credentials. Add a provider on the Providers page."
        delete m_list;
        m_list = nullptr;
        search->hide();
        auto* empty = new QWidget(main);
        auto* emptyLayout = new QVBoxLayout(empty);
        emptyLayout->setContentsMargins(24, 24, 24, 24);
        emptyLayout->addStretch(1);
        auto* text = new QLabel(tr("No models with credentials. Add a provider on the Providers page."), empty);
        text->setObjectName("mpEmpty");
        text->setWordWrap(true);
        text->setAlignment(Qt::AlignCenter);
        auto* open = new QPushButton(LC_AIIcons::lucideIcon("key-round", 14, t.bgRoot), tr("Open Providers"), empty);
        open->setProperty("kind", "accent");
        open->setCursor(Qt::PointingHandCursor);
        connect(open, &QPushButton::clicked, this, [this] {
            emit providersRequested();
            close();
        });
        emptyLayout->addWidget(text);
        emptyLayout->addSpacing(12);
        emptyLayout->addWidget(open, 0, Qt::AlignHCenter);
        emptyLayout->addStretch(1);
        mainLayout->addWidget(empty, 1);
        return;
    }

    buildRail();
    // start on Favorites when there are any, else on the current model's provider
    bool hasFavorites = false;
    for (const Entry& e : std::as_const(m_entries)) {
        hasFavorites = hasFavorites || m_favorites.contains(e.key());
    }
    setTab(hasFavorites ? FAVORITES_TAB : m_currentKey.section('/', 0, 0).isEmpty() ? m_entries.first().provider
                                                                                    : m_currentKey.section('/', 0, 0));
    connect(m_search, &QLineEdit::textChanged, this, &LC_AIModelPicker::refreshList);
}

void LC_AIModelPicker::buildEntries() {
    LC_AICredentials& creds = LC_AICredentials::instance();
    for (const LC_AIProvider& provider : LC_AIProvider::all()) {
        if (!creds.isConnected(provider.id)) {
            continue;
        }
        const QString saved = creds.model(provider.id);
        bool savedListed = false;
        for (const LC_AIGenerated::ModelEntry* m : LC_AIProvider::catalog(provider.id)) {
            Entry e{provider.id, QString::fromUtf8(m->id), QString::fromUtf8(m->name), m->contextWindow, m->reasoning, m->images};
            savedListed = savedListed || e.id == saved;
            m_entries << e;
        }
        if (!saved.isEmpty() && !savedListed) { // a model typed by hand (or a local server's model)
            m_entries << Entry{provider.id, saved, saved, 0, false, false};
        }
    }
}

void LC_AIModelPicker::buildRail() {
    const auto railItem = [this](const QString& tab, const QString& label, const QPixmap& icon) {
        auto* b = new QToolButton();
        b->setObjectName("mpRailItem");
        b->setToolTip(label);
        b->setIcon(QIcon(icon));
        b->setIconSize(QSize(16, 16));
        b->setCursor(Qt::PointingHandCursor);
        b->setProperty("tab", tab);
        b->setFocusPolicy(Qt::NoFocus);
        connect(b, &QToolButton::clicked, this, [this, tab] {
            setTab(tab);
            m_search->clear();
            m_search->setFocus();
        });
        m_railButtons << b;
        m_railLayout->addWidget(b, 0, Qt::AlignHCenter);
    };
    const qreal dpr = devicePixelRatioF();
    railItem(FAVORITES_TAB, tr("Favorites"), LC_AIIcons::lucide("star", 16, m_theme.textMuted, dpr, true));
    auto* sep = new QFrame();
    sep->setObjectName("mpRailSep");
    sep->setFixedSize(20, 1);
    m_railLayout->addSpacing(3);
    m_railLayout->addWidget(sep, 0, Qt::AlignHCenter);
    m_railLayout->addSpacing(3);

    // providers sorted by id, like Cypher, each with its model count
    QMap<QString, int> counts;
    for (const Entry& e : std::as_const(m_entries)) {
        counts[e.provider] += 1;
    }
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        const LC_AIProvider* p = LC_AIProvider::find(it.key());
        const QString label = QStringLiteral("%1 · %2 %3").arg(p != nullptr ? p->name : it.key()).arg(it.value())
                                  .arg(it.value() == 1 ? tr("model") : tr("models"));
        railItem(it.key(), label, LC_AIIcons::provider(it.key(), 16, m_theme.textMuted, dpr));
    }
    m_railLayout->addStretch(1);
}

void LC_AIModelPicker::setTab(const QString& tab) {
    m_tab = tab;
    refreshList();
}

void LC_AIModelPicker::refreshList() {
    const QString needle = m_search->text().trimmed().toLower();
    const qreal dpr = devicePixelRatioF();
    for (QToolButton* b : std::as_const(m_railButtons)) { // .mp-rail-item.on
        const QString tab = b->property("tab").toString();
        const bool on = needle.isEmpty() && tab == m_tab;
        b->setProperty("on", on);
        const QColor color = on ? m_theme.textHeading : m_theme.textMuted;
        b->setIcon(QIcon(tab == FAVORITES_TAB ? LC_AIIcons::lucide("star", 16, color, dpr, true)
                                              : LC_AIIcons::provider(tab, 16, color, dpr)));
        b->style()->unpolish(b);
        b->style()->polish(b);
    }

    m_visible.clear();
    bool showRecent = false;
    if (!needle.isEmpty()) {
        for (int i = 0; i < m_entries.size(); ++i) {
            if ((m_entries[i].key() + " " + m_entries[i].name).toLower().contains(needle)) {
                m_visible << i;
            }
        }
    }
    else if (m_tab == FAVORITES_TAB) {
        const auto addKeys = [this](const QStringList& keys) {
            for (const QString& key : keys) {
                for (int i = 0; i < m_entries.size(); ++i) {
                    if (m_entries[i].key() == key) {
                        m_visible << i;
                        break;
                    }
                }
            }
        };
        addKeys(m_favorites);
        if (m_visible.isEmpty()) { // no favorites yet: show the recent models
            addKeys(LC_AICredentials::instance().recentModels());
            showRecent = !m_visible.isEmpty();
        }
    }
    else {
        for (int i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].provider == m_tab) {
                m_visible << i;
            }
        }
    }

    m_list->clear();
    if (showRecent) {
        auto* header = new QListWidgetItem(tr("Recent. Star a model to keep it here."), m_list);
        header->setData(ROLE_ENTRY, -1);
        header->setFlags(Qt::NoItemFlags);
    }
    for (int n = 0; n < m_visible.size(); ++n) {
        auto* item = new QListWidgetItem(m_list);
        item->setData(ROLE_ENTRY, m_visible[n]);
        item->setData(ROLE_NUMBER, n);
        item->setToolTip(m_entries[m_visible[n]].key());
    }
    if (m_visible.isEmpty()) {
        auto* empty = new QListWidgetItem(!needle.isEmpty()          ? tr("No models match")
                                          : m_tab == FAVORITES_TAB ? tr("Star models to pin them here")
                                                                   : tr("No models"), m_list);
        empty->setData(ROLE_ENTRY, -1);
        empty->setFlags(Qt::NoItemFlags);
        empty->setTextAlignment(Qt::AlignCenter);
    }
    else {
        // highlight the current model when it is listed, else the first row
        int row = showRecent ? 1 : 0;
        for (int i = 0; i < m_list->count(); ++i) {
            const int entry = m_list->item(i)->data(ROLE_ENTRY).toInt();
            if (needle.isEmpty() && entry >= 0 && m_entries[entry].key() == m_currentKey) { // search results start at the top
                row = i;
                break;
            }
        }
        m_list->setCurrentRow(row);
        m_list->scrollToItem(m_list->item(row));
    }
}

void LC_AIModelPicker::pick(const int row) {
    QListWidgetItem* item = m_list->item(row);
    if (item == nullptr || item->data(ROLE_ENTRY).toInt() < 0) {
        return;
    }
    const Entry e = m_entries.at(item->data(ROLE_ENTRY).toInt());
    LC_AICredentials& creds = LC_AICredentials::instance();
    creds.setModel(e.provider, e.id);
    creds.setSelectedProvider(e.provider);
    creds.addRecentModel(e.key());
    emit picked(e.provider, e.id);
    close();
}

void LC_AIModelPicker::toggleFavorite(const int row) {
    QListWidgetItem* item = m_list->item(row);
    if (item == nullptr || item->data(ROLE_ENTRY).toInt() < 0) {
        return;
    }
    const QString key = m_entries.at(item->data(ROLE_ENTRY).toInt()).key();
    if (m_favorites.contains(key)) {
        m_favorites.removeAll(key);
    }
    else {
        m_favorites << key;
    }
    LC_AICredentials::instance().setFavoriteModels(m_favorites);
    m_list->viewport()->update();
    m_search->setFocus();
}

bool LC_AIModelPicker::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_search && event->type() == QEvent::KeyPress && m_list != nullptr) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if ((key->modifiers() & Qt::ControlModifier) && key->key() >= Qt::Key_1 && key->key() <= Qt::Key_9) {
            const int n = key->key() - Qt::Key_1;
            for (int i = 0; i < m_list->count(); ++i) {
                if (m_list->item(i)->data(ROLE_ENTRY).toInt() >= 0 && m_list->item(i)->data(ROLE_NUMBER).toInt() == n) {
                    pick(i);
                    break;
                }
            }
            return true;
        }
        if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) {
            const int count = m_list->count();
            int row = m_list->currentRow();
            for (int step = 0; step < count; ++step) { // wrap around, skipping headers
                row = (row + (key->key() == Qt::Key_Down ? 1 : count - 1)) % count;
                if (m_list->item(row)->data(ROLE_ENTRY).toInt() >= 0) {
                    m_list->setCurrentRow(row);
                    m_list->scrollToItem(m_list->item(row));
                    m_list->viewport()->update();
                    break;
                }
            }
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            pick(m_list->currentRow());
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void LC_AIModelPicker::popup(QWidget* anchor) {
    const QPoint topRight = anchor->mapToGlobal(QPoint(anchor->width(), 0));
    const QPoint bottomRight = anchor->mapToGlobal(QPoint(anchor->width(), anchor->height()));
    QRect screen = anchor->screen() != nullptr ? anchor->screen()->availableGeometry() : QRect();
    QPoint pos(topRight.x() - width(), topRight.y() - height() - 6);
    if (!screen.isNull()) {
        if (pos.y() < screen.top()) {
            pos.setY(bottomRight.y() + 6); // no room above: open below
        }
        pos.setX(qBound(screen.left(), pos.x(), screen.right() - width()));
    }
    move(pos);
    show();
    m_search->setFocus();
}
