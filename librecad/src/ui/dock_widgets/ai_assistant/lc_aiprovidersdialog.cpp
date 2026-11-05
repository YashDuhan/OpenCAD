/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aiprovidersdialog.h"

#include <QApplication>
#include <QScrollBar>
#include <QEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

#include "lc_aicredentials.h"
#include "lc_aigenerated.h"
#include "lc_aiicons.h"
#include "lc_aiprovider.h"

namespace {
    QString css(const QColor& c) {
        return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF(), 0, 'f', 3);
    }

    QColor rgba(const int r, const int g, const int b, const double a) {
        QColor c(r, g, b);
        c.setAlphaF(a);
        return c;
    }

    QColor mix(const QColor& a, const QColor& b, const double t) { // t of a, (1 - t) of b
        return QColor::fromRgbF(a.redF() * t + b.redF() * (1 - t), a.greenF() * t + b.greenF() * (1 - t),
                                a.blueF() * t + b.blueF() * (1 - t));
    }

    /** Opaque version of a translucent color over a background (for pixmaps drawn with a single color). */
    QColor over(const QColor& fg, const QColor& bg) {
        return mix(QColor(fg.red(), fg.green(), fg.blue()), bg, fg.alphaF());
    }

    /** A frame that runs a callback when clicked (the list rows of the Providers page). */
    class ClickableFrame : public QFrame {
    public:
        explicit ClickableFrame(std::function<void()> onClick, QWidget* parent = nullptr)
            : QFrame(parent), m_onClick(std::move(onClick)) {
            setAttribute(Qt::WA_Hover);
            setCursor(Qt::PointingHandCursor);
        }

    protected:
        void mouseReleaseEvent(QMouseEvent* event) override {
            if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
                m_onClick();
            }
            QFrame::mouseReleaseEvent(event);
        }

    private:
        std::function<void()> m_onClick;
    };

    /** Shows a child only while the pointer is over the row (".pv-model:hover .pv-use" in Cypher). */
    class HoverReveal : public QObject {
    public:
        HoverReveal(QWidget* row, QWidget* revealed) : QObject(row), m_revealed(revealed) {
            QSizePolicy policy = revealed->sizePolicy();
            policy.setRetainSizeWhenHidden(true);
            revealed->setSizePolicy(policy);
            revealed->hide();
            row->setAttribute(Qt::WA_Hover);
            row->installEventFilter(this);
        }

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override {
            if (event->type() == QEvent::Enter) {
                m_revealed->show();
            }
            else if (event->type() == QEvent::Leave && !m_revealed->hasFocus()) {
                m_revealed->hide();
            }
            return QObject::eventFilter(watched, event);
        }

    private:
        QWidget* m_revealed;
    };

    QFrame* iconTile(const QString& objectName, const QPixmap& pixmap, const int size) {
        auto* tile = new QFrame();
        tile->setObjectName(objectName);
        tile->setFixedSize(size, size);
        auto* layout = new QHBoxLayout(tile);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* label = new QLabel(tile);
        label->setPixmap(pixmap);
        label->setAlignment(Qt::AlignCenter);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(label);
        return tile;
    }
}

// ------------------------------------------------------------------------------------------------ theme

LC_AITheme LC_AITheme::current() {
    LC_AITheme t;
    t.dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    if (t.dark) {
        t.bgRoot = QColor("#000000");
        t.bgPage = QColor("#000000");
        t.bgChrome = QColor("#0a0a0b");
        t.bgCard = rgba(255, 255, 255, 0.025);
        t.bgSubtle = rgba(255, 255, 255, 0.035);
        t.bgHover = rgba(255, 255, 255, 0.055);
        t.bgActive = rgba(255, 255, 255, 0.075);
        t.bgElevated = QColor("#111112");
        t.borderSubtle = rgba(255, 255, 255, 0.06);
        t.borderDefault = rgba(255, 255, 255, 0.085);
        t.borderOverlay = rgba(255, 255, 255, 0.1);
        t.borderHover = rgba(255, 255, 255, 0.13);
        t.borderStrong = rgba(255, 255, 255, 0.16);
        t.textHeading = QColor("#f5f5f6");
        t.textPrimary = QColor("#e6e6e8");
        t.textSecondary = QColor("#bdbdc2");
        t.textTertiary = QColor("#9a9aa0");
        t.textMuted = QColor("#85858b");
        t.textFaint = QColor("#7f7f85");
        t.textPlaceholder = QColor("#85858b");
    }
    else {
        t.bgRoot = QColor("#fbfbfb");
        t.bgPage = QColor("#fbfbfb");
        t.bgChrome = QColor("#f3f3f4");
        t.bgCard = rgba(0, 0, 0, 0.02);
        t.bgSubtle = rgba(0, 0, 0, 0.03);
        t.bgHover = rgba(0, 0, 0, 0.045);
        t.bgActive = rgba(0, 0, 0, 0.065);
        t.bgElevated = QColor("#ffffff");
        t.borderSubtle = rgba(0, 0, 0, 0.07);
        t.borderDefault = rgba(0, 0, 0, 0.1);
        t.borderOverlay = rgba(0, 0, 0, 0.12);
        t.borderHover = rgba(0, 0, 0, 0.15);
        t.borderStrong = rgba(0, 0, 0, 0.18);
        t.textHeading = QColor("#111113");
        t.textPrimary = QColor("#1f1f23");
        t.textSecondary = QColor("#46464d");
        t.textTertiary = QColor("#5f5f66");
        t.textMuted = QColor("#6b6b72");
        t.textFaint = QColor("#6e6e75");
        t.textPlaceholder = QColor("#717178");
    }
    t.accentGreen = QColor("#4ade80");
    t.accentBlue = QColor("#4c8dff");
    t.accentBlueBorder = rgba(76, 141, 255, 0.4);
    t.accentBlueBg = rgba(76, 141, 255, 0.08);
    return t;
}

QString LC_AITheme::styleSheet() const {
    QString s = QStringLiteral(R"(
QDialog { background: @bgPage; }
QWidget { font-family: "Onest Variable", "Onest", "Inter", system-ui, sans-serif; font-size: 13px; color: @textPrimary; }
QLabel { background: transparent; }
QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; border: none; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: @borderStrong; border-radius: 3px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: @textFaint; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: @borderStrong; border-radius: 3px; min-width: 30px; }
QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page { background: none; height: 0; width: 0; }

#pvTitle { font-size: 17px; font-weight: 500; color: @textHeading; }
#pvIntro { color: @textMuted; }
#pvRefresh { border: none; border-radius: 7px; padding: 4px 8px; font-size: 12.5px; color: @textMuted; background: transparent; }
#pvRefresh:hover { background: @bgHover; color: @textPrimary; }
#iconBtn { border: none; border-radius: 8px; background: transparent; min-width: 32px; min-height: 32px; max-width: 32px; max-height: 32px; }
#iconBtn:hover { background: @bgHover; }

#pvShell { background: @bgChrome; border: 1px solid @borderSubtle; border-radius: 14px; }
#pvList { background: transparent; border: none; border-right: 1px solid @borderSubtle; border-top-left-radius: 14px; border-bottom-left-radius: 14px; }
#pvSearch { background: @bgSubtle; border: 1px solid @borderSubtle; border-radius: 8px; }
#pvSearch QLineEdit { background: transparent; border: none; padding: 0; font-size: 13px; color: @textPrimary; }
#pvListGroup { padding: 10px 10px 4px 10px; font-size: 11.5px; font-weight: 500; color: @textFaint; }
#pvItem { border-radius: 9px; background: transparent; }
#pvItem:hover { background: @bgHover; }
#pvItem[on="true"] { background: @bgActive; }
#pvItemIcon { border: 1px solid @borderSubtle; border-radius: 8px; background: @bgSubtle; }
#pvItemName { font-size: 13.5px; font-weight: 500; color: @textPrimary; }
#pvItemName[off="true"] { color: @textSecondary; }
#pvItemSmall { font-size: 12px; color: @textFaint; }
#pvDot[tone="on"] { background: @accentGreen; border: none; border-radius: 3px; }
#pvDot[tone="off"] { background: transparent; border: 1.5px solid @textFaint; border-radius: 3px; }
#pvEmptyNote { padding: 16px 8px; font-size: 12.5px; color: @textFaint; }

#pvPanel { background: @bgPage; border: none; border-top-right-radius: 14px; border-bottom-right-radius: 14px; }
#pvDetailIcon { border: 1px solid @borderDefault; border-radius: 11px; background: @bgElevated; }
#pvH2 { font-size: 17px; font-weight: 500; color: @textHeading; }
#pvCode { font-family: "JetBrains Mono Variable", "JetBrains Mono", "JetBrainsMono Nerd Font", monospace; font-size: 12px; color: @textFaint; }
#pvStatus { border: 1px solid @borderSubtle; border-radius: 12px; }
#pvStatusText { font-size: 12px; color: @textSecondary; }
#pvSection { font-size: 13px; font-weight: 500; color: @textMuted; }
#pvCount { background: @bgActive; border-radius: 8px; padding: 0 7px; font-size: 11.5px; color: @textSecondary; }
#pvGroup { background: @bgElevated; border: 1px solid @borderSubtle; border-radius: 12px; }
#pvRow { background: transparent; border: none; }
#pvRow[first="false"] { border-top: 1px solid @borderSubtle; }
#pvRowTitle { font-size: 13.5px; color: @textPrimary; }
#pvRowHint { font-size: 12.5px; color: @textMuted; }
#pvEmpty { padding: 16px; font-size: 13px; color: @textMuted; }
#pvInUse { font-size: 12.5px; color: @textSecondary; }

QPushButton { min-height: 28px; max-height: 30px; padding: 0 12px; border-radius: 8px; font-size: 13px; }
QPushButton[kind="accent"] { background: @textHeading; color: @bgRoot; border: 1px solid transparent; font-weight: 500; }
QPushButton[kind="accent"]:hover { background: @accentHover; }
QPushButton[kind="ghost"] { background: transparent; color: @textSecondary; border: 1px solid @borderDefault; }
QPushButton[kind="ghost"]:hover { background: @bgHover; color: @textPrimary; }
QPushButton[size="small"] { min-height: 26px; max-height: 28px; font-size: 12.5px; }
QPushButton:disabled { color: @textFaint; }

QLineEdit#modalInput, QLineEdit#pvInput { background: @bgCard; border: 1px solid @borderDefault; border-radius: 8px; padding: 6px 10px; font-size: 13px; color: @textPrimary; }
QLineEdit#modalInput:focus, QLineEdit#pvInput:focus { border-color: @accentBlueBorder; }
#modalHead { font-size: 15px; font-weight: 600; color: @textHeading; }
#modalText { font-size: 13.5px; color: @textSecondary; }
#bannerInfo { border: 1px solid @accentBlueBorder; background: @accentBlueBg; border-radius: 10px; }
#bannerInfo QLabel { color: @accentBlue; font-size: 13px; }
#bannerError { border: 1px solid rgba(248, 113, 113, 0.25); background: rgba(248, 113, 113, 0.08); border-radius: 10px; }
#bannerError QLabel { color: #f87171; font-size: 13px; }
)");
    const QList<QPair<QString, QColor>> vars = {
        {"@bgRoot", bgRoot}, {"@bgPage", bgPage}, {"@bgChrome", bgChrome}, {"@bgCard", bgCard},
        {"@bgSubtle", bgSubtle}, {"@bgHover", bgHover}, {"@bgActive", bgActive}, {"@bgElevated", bgElevated},
        {"@borderSubtle", borderSubtle}, {"@borderDefault", borderDefault}, {"@borderOverlay", borderOverlay},
        {"@borderHover", borderHover}, {"@borderStrong", borderStrong}, {"@textHeading", textHeading},
        {"@textPrimary", textPrimary}, {"@textSecondary", textSecondary}, {"@textTertiary", textTertiary},
        {"@textMuted", textMuted}, {"@textFaint", textFaint}, {"@textPlaceholder", textPlaceholder},
        {"@accentGreen", accentGreen}, {"@accentBlueBorder", accentBlueBorder}, {"@accentBlueBg", accentBlueBg},
        {"@accentBlue", accentBlue}, {"@accentHover", mix(textHeading, bgRoot, 0.86)},
    };
    // longest names first, so "@accentBlueBorder" is not hit by "@accentBlue"
    QList<QPair<QString, QColor>> sorted = vars;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    for (const auto& [name, color] : sorted) {
        s.replace(name, css(color));
    }
    return s;
}

// ------------------------------------------------------------------------------------------------ providers modal

LC_AIProvidersDialog::LC_AIProvidersDialog(QWidget* parent)
    : QDialog(parent), m_theme(LC_AITheme::current()) {
    setWindowTitle(tr("Providers"));
    setObjectName("pvDialog");
    setStyleSheet(m_theme.styleSheet());

    // a large modal: up to Cypher's 1180px page width, never bigger than the screen
    QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
    if (parent != nullptr && parent->screen() != nullptr) {
        screen = parent->screen()->availableGeometry();
    }
    resize(qMin(1180, int(screen.width() * 0.92)), qMin(780, int(screen.height() * 0.9)));
    setMinimumSize(760, 480);

    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(24, 18, 24, 22);
    page->setSpacing(14);

    // header: title, intro line, Refresh, close
    auto* header = new QHBoxLayout();
    header->setSpacing(12);
    auto* titleBox = new QVBoxLayout();
    titleBox->setSpacing(4);
    auto* title = new QLabel(tr("Providers"), this);
    title->setObjectName("pvTitle");
    auto* intro = new QLabel(tr("Add API keys for the AI assistant. Keys are saved on this computer only."), this);
    intro->setObjectName("pvIntro");
    titleBox->addWidget(title);
    titleBox->addWidget(intro);
    header->addLayout(titleBox, 1);

    auto* refresh = new QToolButton(this);
    refresh->setObjectName("pvRefresh");
    refresh->setText(tr("Refresh"));
    refresh->setToolTip(tr("Check credentials again"));
    refresh->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    refresh->setIcon(LC_AIIcons::lucideIcon("refresh-cw", 13, m_theme.textMuted));
    refresh->setIconSize(QSize(13, 13));
    refresh->setCursor(Qt::PointingHandCursor);
    connect(refresh, &QToolButton::clicked, this, &LC_AIProvidersDialog::reloadProviders);
    header->addWidget(refresh, 0, Qt::AlignVCenter);

    auto* close = new QToolButton(this);
    close->setObjectName("iconBtn");
    close->setIcon(LC_AIIcons::lucideIcon("x", 16, m_theme.textTertiary));
    close->setIconSize(QSize(16, 16));
    close->setToolTip(tr("Close"));
    close->setCursor(Qt::PointingHandCursor);
    connect(close, &QToolButton::clicked, this, &QDialog::accept);
    header->addWidget(close, 0, Qt::AlignVCenter);
    page->addLayout(header);

    // shell: provider list | detail panel
    auto* shell = new QFrame(this);
    shell->setObjectName("pvShell");
    auto* shellLayout = new QHBoxLayout(shell);
    shellLayout->setContentsMargins(1, 1, 1, 1);
    shellLayout->setSpacing(0);

    auto* list = new QFrame(shell);
    list->setObjectName("pvList");
    list->setFixedWidth(300);
    auto* listLayout = new QVBoxLayout(list);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(0);

    auto* search = new QFrame(list);
    search->setObjectName("pvSearch");
    search->setFixedHeight(30);
    auto* searchLayout = new QHBoxLayout(search);
    searchLayout->setContentsMargins(10, 0, 10, 0);
    searchLayout->setSpacing(8);
    auto* searchIcon = new QLabel(search);
    searchIcon->setPixmap(LC_AIIcons::lucide("search", 14, m_theme.textMuted, devicePixelRatioF()));
    m_search = new QLineEdit(search);
    m_search->setPlaceholderText(tr("Search %1 providers").arg(LC_AIProvider::all().size()));
    QPalette searchPalette = m_search->palette();
    searchPalette.setColor(QPalette::PlaceholderText, m_theme.textPlaceholder);
    m_search->setPalette(searchPalette);
    searchLayout->addWidget(searchIcon);
    searchLayout->addWidget(m_search, 1);
    auto* searchWrap = new QHBoxLayout();
    searchWrap->setContentsMargins(10, 10, 10, 10);
    searchWrap->addWidget(search);
    listLayout->addLayout(searchWrap);

    auto* listScroll = new QScrollArea(list);
    listScroll->setWidgetResizable(true);
    listScroll->setFrameShape(QFrame::NoFrame);
    listScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_listContent = new QWidget();
    m_listLayout = new QVBoxLayout(m_listContent);
    m_listLayout->setContentsMargins(6, 0, 6, 8);
    m_listLayout->setSpacing(0);
    listScroll->setWidget(m_listContent);
    listLayout->addWidget(listScroll, 1);
    shellLayout->addWidget(list);

    m_panel = new QScrollArea(shell);
    m_panel->setObjectName("pvPanel");
    m_panel->setWidgetResizable(true);
    m_panel->setFrameShape(QFrame::NoFrame);
    m_panel->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    shellLayout->addWidget(m_panel, 1);
    page->addWidget(shell, 1);

    connect(m_search, &QLineEdit::textChanged, this, &LC_AIProvidersDialog::rebuildList);

    m_selectedId = LC_AICredentials::instance().selectedProvider();
    reloadProviders();
    m_search->setFocus();
}

void LC_AIProvidersDialog::reloadProviders() {
    LC_AICredentials::instance().reload();
    rebuildList();
    showDetail(m_selectedId);
}

bool LC_AIProvidersDialog::isConfigured(const LC_AIProvider& provider) const {
    return LC_AICredentials::instance().isConnected(provider.id);
}

LC_AIProvidersDialog::Status LC_AIProvidersDialog::statusOf(const LC_AIProvider& provider) const {
    if (!isConfigured(provider)) {
        return {Tone::Off, tr("Not connected")};
    }
    switch (LC_AICredentials::instance().keySource(provider.id)) {
        case LC_AICredentials::KeySource::Saved:
            return {Tone::On, tr("API key · saved")};
        case LC_AICredentials::KeySource::Environment:
            return {Tone::On, tr("API key · %1").arg(provider.envVar)};
        case LC_AICredentials::KeySource::None:
            break;
    }
    return {Tone::On, tr("Endpoint · %1").arg(QUrl(LC_AICredentials::instance().baseUrl(provider.id)).authority())};
}

QString LC_AIProvidersDialog::credentialTitle(const LC_AIProvider& provider) const {
    if (!isConfigured(provider)) {
        return tr("None");
    }
    switch (LC_AICredentials::instance().keySource(provider.id)) {
        case LC_AICredentials::KeySource::Saved:
            return tr("Saved");
        case LC_AICredentials::KeySource::Environment:
            return tr("Environment");
        case LC_AICredentials::KeySource::None:
            break;
    }
    return tr("No key needed");
}

QString LC_AIProvidersDialog::credentialDetail(const LC_AIProvider& provider) const {
    const QString path = LC_AICredentials::instance().filePath().toHtmlEscaped();
    if (!isConfigured(provider)) {
        return tr("Nothing saved. Add an API key to use this provider's models.");
    }
    switch (LC_AICredentials::instance().keySource(provider.id)) {
        case LC_AICredentials::KeySource::Saved:
            return tr("Your API key is saved in <code>%1</code>.").arg(path);
        case LC_AICredentials::KeySource::Environment:
            return tr("Read from the <code>%1</code> environment variable when OpenCAD starts. Change it in your shell "
                      "profile, or add a key here to override it.").arg(provider.envVar);
        case LC_AICredentials::KeySource::None:
            break;
    }
    return tr("Requests go to the endpoint without a key. Add one if your server requires it.");
}

QLabel* LC_AIProvidersDialog::dot(const Tone tone) {
    auto* d = new QLabel();
    d->setObjectName("pvDot");
    d->setProperty("tone", tone == Tone::On ? "on" : "off");
    d->setFixedSize(7, 7);
    return d;
}

void LC_AIProvidersDialog::rebuildList() {
    while (QLayoutItem* item = m_listLayout->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            // rows can be rebuilt from their own click handler: delete them once it has returned
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    const QString needle = m_search->text().trimmed().toLower();
    QList<const LC_AIProvider*> connected;
    QList<const LC_AIProvider*> available;
    for (const LC_AIProvider& p : LC_AIProvider::all()) {
        if (!needle.isEmpty() && !(p.name + " " + p.id).toLower().contains(needle)) {
            continue;
        }
        (isConfigured(p) ? connected : available).append(&p);
    }
    const auto group = [this](const QString& text) {
        auto* label = new QLabel(text, m_listContent);
        label->setObjectName("pvListGroup");
        m_listLayout->addWidget(label);
    };
    if (!connected.isEmpty()) {
        group(tr("Connected"));
    }
    for (const LC_AIProvider* p : connected) {
        m_listLayout->addWidget(listRow(*p));
    }
    if (!available.isEmpty()) {
        group(tr("Available"));
    }
    for (const LC_AIProvider* p : available) {
        m_listLayout->addWidget(listRow(*p));
    }
    if (connected.isEmpty() && available.isEmpty()) {
        auto* empty = new QLabel(tr("No providers match"), m_listContent);
        empty->setObjectName("pvEmptyNote");
        empty->setAlignment(Qt::AlignCenter);
        m_listLayout->addWidget(empty);
    }
    m_listLayout->addStretch(1);
}

QWidget* LC_AIProvidersDialog::listRow(const LC_AIProvider& provider) {
    const QString id = provider.id;
    const bool on = id == m_selectedId;
    const bool configured = isConfigured(provider);
    const Status status = statusOf(provider);

    auto* row = new ClickableFrame([this, id] { showDetail(id); }, m_listContent);
    row->setObjectName("pvItem");
    row->setProperty("on", on);
    row->setToolTip(provider.name);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(10, 9, 10, 9);
    layout->setSpacing(11);

    const QColor iconColor = on ? m_theme.textHeading : m_theme.textSecondary;
    QFrame* tile = iconTile("pvItemIcon", LC_AIIcons::provider(id, 16, iconColor, devicePixelRatioF()), 30);
    tile->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(tile);

    auto* text = new QVBoxLayout();
    text->setSpacing(0);
    auto* name = new QLabel(provider.name, row);
    name->setObjectName("pvItemName");
    name->setProperty("off", !configured);
    auto* small = new QLabel(row);
    small->setObjectName("pvItemSmall");
    small->setText(small->fontMetrics().elidedText(status.text, Qt::ElideRight, 190));
    for (QLabel* l : {name, small}) {
        l->setAttribute(Qt::WA_TransparentForMouseEvents);
        text->addWidget(l);
    }
    layout->addLayout(text, 1);
    QLabel* d = dot(status.tone);
    d->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(d, 0, Qt::AlignVCenter);
    return row;
}

void LC_AIProvidersDialog::showDetail(const QString& providerId) {
    const LC_AIProvider* provider = LC_AIProvider::find(providerId);
    if (provider == nullptr) {
        provider = &LC_AIProvider::all().first();
    }
    if (m_selectedId != provider->id) {
        m_selectedId = provider->id;
        rebuildList(); // the highlight and icon colors follow the selection
    }
    QWidget* old = m_panel->takeWidget();
    if (old != nullptr) {
        old->deleteLater();
    }
    m_panel->setWidget(detail(*provider));
    m_panel->verticalScrollBar()->setValue(0);
}

QPushButton* LC_AIProvidersDialog::button(const QString& text, const QString& icon, const bool accent) {
    auto* b = new QPushButton(text);
    b->setProperty("kind", accent ? "accent" : "ghost");
    b->setCursor(Qt::PointingHandCursor);
    if (!icon.isEmpty()) {
        b->setIcon(LC_AIIcons::lucideIcon(icon, 14, accent ? m_theme.bgRoot : m_theme.textSecondary));
        b->setIconSize(QSize(14, 14));
    }
    return b;
}

QLabel* LC_AIProvidersDialog::sectionLabel(const QString& text, const int count) {
    auto* label = new QLabel();
    label->setObjectName("pvSection");
    label->setTextFormat(Qt::RichText);
    QString html = text.toHtmlEscaped();
    if (count >= 0) {
        // the count pill of Cypher's .pv-count
        html += QStringLiteral("&nbsp;&nbsp;<span style=\"background:%1; color:%2; font-size:11.5px;\">&nbsp;%3&nbsp;</span>")
                    .arg(over(m_theme.bgActive, m_theme.bgPage).name(), m_theme.textSecondary.name())
                    .arg(count);
    }
    label->setText(html);
    label->setContentsMargins(0, 24, 0, 8);
    return label;
}

QWidget* LC_AIProvidersDialog::settingRow(const QString& title, const QString& hint, QWidget* control, const bool first) {
    auto* row = new QFrame();
    row->setObjectName("pvRow");
    row->setProperty("first", first);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(16);
    auto* text = new QVBoxLayout();
    text->setSpacing(2);
    auto* titleLabel = new QLabel(title, row);
    titleLabel->setObjectName("pvRowTitle");
    text->addWidget(titleLabel);
    if (!hint.isEmpty()) {
        auto* hintLabel = new QLabel(hint, row);
        hintLabel->setObjectName("pvRowHint");
        hintLabel->setWordWrap(true);
        hintLabel->setTextFormat(Qt::RichText);
        hintLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        text->addWidget(hintLabel);
    }
    layout->addLayout(text, 1);
    if (control != nullptr) {
        layout->addWidget(control, 0, Qt::AlignVCenter);
    }
    return row;
}

QWidget* LC_AIProvidersDialog::detail(const LC_AIProvider& provider) {
    LC_AICredentials& creds = LC_AICredentials::instance();
    const bool configured = isConfigured(provider);
    const bool keyStored = creds.keySource(provider.id) == LC_AICredentials::KeySource::Saved;

    auto* outer = new QWidget();
    auto* outerLayout = new QHBoxLayout(outer);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    auto* content = new QWidget(outer);
    content->setMaximumWidth(760);
    outerLayout->addWidget(content, 1);
    outerLayout->addStretch(0);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(26, 22, 26, 30);
    layout->setSpacing(0);

    // head: icon, name + id, status pill
    auto* head = new QHBoxLayout();
    head->setSpacing(12);
    head->addWidget(iconTile("pvDetailIcon", LC_AIIcons::provider(provider.id, 20, m_theme.textHeading, devicePixelRatioF()), 40));
    auto* titles = new QVBoxLayout();
    titles->setSpacing(0);
    auto* h2 = new QLabel(provider.name, content);
    h2->setObjectName("pvH2");
    auto* code = new QLabel(provider.id, content);
    code->setObjectName("pvCode");
    code->setTextInteractionFlags(Qt::TextSelectableByMouse);
    titles->addWidget(h2);
    titles->addWidget(code);
    head->addLayout(titles, 1);
    auto* pill = new QFrame(content);
    pill->setObjectName("pvStatus");
    auto* pillLayout = new QHBoxLayout(pill);
    pillLayout->setContentsMargins(10, 4, 10, 4);
    pillLayout->setSpacing(7);
    pillLayout->addWidget(dot(configured ? Tone::On : Tone::Off), 0, Qt::AlignVCenter);
    auto* pillText = new QLabel(configured ? tr("Connected") : tr("Not connected"), pill);
    pillText->setObjectName("pvStatusText");
    pillLayout->addWidget(pillText);
    head->addWidget(pill, 0, Qt::AlignVCenter);
    layout->addLayout(head);

    // Connection
    layout->addWidget(sectionLabel(tr("Connection")));
    auto* connection = new QFrame(content);
    connection->setObjectName("pvGroup");
    auto* connectionLayout = new QVBoxLayout(connection);
    connectionLayout->setContentsMargins(0, 0, 0, 0);
    connectionLayout->setSpacing(0);

    QString keyHint;
    if (keyStored) {
        keyHint = tr("A key is saved. Replacing it takes effect for new requests.");
    }
    else if (creds.keySource(provider.id) == LC_AICredentials::KeySource::Environment) {
        keyHint = tr("Using <code>%1</code>. A saved key takes precedence.").arg(provider.envVar);
    }
    else if (provider.keyOptional) {
        keyHint = tr("Optional: local servers such as Ollama or LM Studio need no key.");
    }
    else {
        keyHint = tr("Paste a key from the provider's dashboard.");
    }
    auto* keyControls = new QWidget(connection);
    auto* keyLayout = new QHBoxLayout(keyControls);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->setSpacing(6);
    QPushButton* addKey = button(keyStored ? tr("Replace key") : tr("Add key"), "key-round", !configured);
    connect(addKey, &QPushButton::clicked, this, [this, &provider] { startKeyEntry(provider); });
    keyLayout->addWidget(addKey);
    if (keyStored) {
        auto* trash = new QToolButton(keyControls);
        trash->setObjectName("iconBtn");
        trash->setIcon(LC_AIIcons::lucideIcon("trash-2", 14, m_theme.textTertiary));
        trash->setIconSize(QSize(14, 14));
        trash->setToolTip(tr("Remove the saved key"));
        trash->setCursor(Qt::PointingHandCursor);
        connect(trash, &QToolButton::clicked, this, [this, &provider] { removeKey(provider); });
        keyLayout->addWidget(trash);
    }
    connectionLayout->addWidget(settingRow(tr("API key"), keyHint, keyControls, true));

    if (provider.customBaseUrl) {
        // endpoint of an OpenAI-compatible server (not on Cypher's page: its local models have their own page)
        auto* endpoint = new QWidget(connection);
        auto* endpointLayout = new QHBoxLayout(endpoint);
        endpointLayout->setContentsMargins(0, 0, 0, 0);
        endpointLayout->setSpacing(6);
        auto* url = new QLineEdit(creds.baseUrl(provider.id), endpoint);
        url->setObjectName("pvInput");
        url->setMinimumWidth(260);
        url->setPlaceholderText(provider.baseUrl);
        QPushButton* save = button(tr("Save"), QString(), false);
        connect(save, &QPushButton::clicked, this, [this, url, id = provider.id] {
            LC_AICredentials::instance().setBaseUrl(id, url->text().trimmed().isEmpty() ? url->placeholderText() : url->text());
            rebuildList();
            showDetail(id);
        });
        endpointLayout->addWidget(url);
        endpointLayout->addWidget(save);
        connectionLayout->addWidget(settingRow(tr("Endpoint"),
                                               tr("Base URL of an OpenAI-compatible server, such as Ollama "
                                                  "(<code>http://localhost:11434/v1</code>) or LM Studio "
                                                  "(<code>http://localhost:1234/v1</code>)."),
                                               endpoint, false));
    }
    layout->addWidget(connection);

    // Credential
    layout->addWidget(sectionLabel(tr("Credential")));
    auto* credential = new QFrame(content);
    credential->setObjectName("pvGroup");
    auto* credentialLayout = new QVBoxLayout(credential);
    credentialLayout->setContentsMargins(0, 0, 0, 0);
    credentialLayout->addWidget(settingRow(credentialTitle(provider), credentialDetail(provider), nullptr, true));
    layout->addWidget(credential);

    // Models
    const int count = int(LC_AIProvider::catalog(provider.id).size());
    layout->addWidget(sectionLabel(tr("Models"), count > 0 ? count : -1));
    layout->addWidget(modelsGroup(provider));
    layout->addStretch(1);
    return outer;
}

QWidget* LC_AIProvidersDialog::modelsGroup(const LC_AIProvider& provider) {
    LC_AICredentials& creds = LC_AICredentials::instance();
    auto* group = new QFrame();
    group->setObjectName("pvGroup");
    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    if (!isConfigured(provider)) {
        auto* empty = new QLabel(tr("Connect %1 to see its models.").arg(provider.name), group);
        empty->setObjectName("pvEmpty");
        layout->addWidget(empty);
        return group;
    }

    const bool selectedProvider = creds.selectedProvider() == provider.id;
    const QString currentModel = creds.model(provider.id);
    bool first = true;
    bool currentListed = false;

    const auto inUse = [this, group] {
        auto* box = new QWidget(group);
        auto* l = new QHBoxLayout(box);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(5);
        auto* check = new QLabel(box);
        check->setPixmap(LC_AIIcons::lucide("check", 13, m_theme.textSecondary, devicePixelRatioF()));
        auto* text = new QLabel(tr("In use"), box);
        text->setObjectName("pvInUse");
        l->addWidget(check);
        l->addWidget(text);
        return box;
    };

    for (const LC_AIGenerated::ModelEntry* entry : LC_AIProvider::catalog(provider.id)) {
        const LC_AIGenerated::ModelEntry& m = *entry;
        const QString id = QString::fromUtf8(m.id);
        const bool active = selectedProvider && id == currentModel;
        currentListed = currentListed || id == currentModel;
        QString hint = QStringLiteral("<code>%1</code> · %2 context").arg(id.toHtmlEscaped(), LC_AIProvider::compactNumber(m.contextWindow));
        if (m.reasoning) {
            hint += tr(" · reasoning");
        }
        if (m.images) {
            hint += tr(" · images");
        }
        QWidget* control = nullptr;
        if (active) {
            control = inUse();
        }
        else {
            QPushButton* use = button(tr("Use in chat"), QString(), false);
            use->setProperty("size", "small");
            connect(use, &QPushButton::clicked, this, [this, pid = provider.id, id] { useModel(pid, id); });
            control = use;
        }
        QWidget* row = settingRow(QString::fromUtf8(m.name), hint, control, first);
        row->layout()->setContentsMargins(16, 10, 16, 10);
        if (!active) {
            new HoverReveal(row, control);
        }
        layout->addWidget(row);
        first = false;
    }

    // any model id the catalog does not list (new releases, local models)
    auto* other = new QWidget(group);
    auto* otherLayout = new QHBoxLayout(other);
    otherLayout->setContentsMargins(0, 0, 0, 0);
    otherLayout->setSpacing(6);
    auto* modelId = new QLineEdit(other);
    modelId->setObjectName("pvInput");
    modelId->setMinimumWidth(240);
    modelId->setPlaceholderText(tr("Model id"));
    if (!currentListed) {
        modelId->setText(currentModel);
    }
    otherLayout->addWidget(modelId);
    if (selectedProvider && !currentListed && !currentModel.isEmpty()) {
        otherLayout->addWidget(inUse());
    }
    QPushButton* useOther = button(tr("Use in chat"), QString(), first);
    useOther->setProperty("size", "small");
    const auto applyOther = [this, modelId, pid = provider.id] {
        if (!modelId->text().trimmed().isEmpty()) {
            useModel(pid, modelId->text().trimmed());
        }
    };
    connect(useOther, &QPushButton::clicked, this, applyOther);
    connect(modelId, &QLineEdit::returnPressed, this, applyOther);
    otherLayout->addWidget(useOther);
    layout->addWidget(settingRow(first ? tr("Model") : tr("Other model"),
                                 first ? tr("Type the id of a model this endpoint serves.")
                                       : tr("Any model id %1 offers that is not listed above.").arg(provider.name),
                                 other, first));
    return group;
}

void LC_AIProvidersDialog::startKeyEntry(const LC_AIProvider& provider) {
    LC_AIKeyDialog dialog(provider, m_theme, this);
    dialog.exec();
    const QString id = provider.id;
    rebuildList();
    showDetail(id);
}

void LC_AIProvidersDialog::removeKey(const LC_AIProvider& provider) {
    if (QMessageBox::question(this, tr("Remove API key"), tr("Remove the saved API key for %1?").arg(provider.name))
        != QMessageBox::Yes) {
        return;
    }
    const QString id = provider.id;
    LC_AICredentials::instance().removeApiKey(id);
    rebuildList();
    showDetail(id);
}

void LC_AIProvidersDialog::useModel(const QString& providerId, const QString& modelId) {
    LC_AICredentials& creds = LC_AICredentials::instance();
    creds.setModel(providerId, modelId);
    creds.setSelectedProvider(providerId);
    creds.addRecentModel(providerId + "/" + modelId);
    const int scroll = m_panel->verticalScrollBar()->value();
    showDetail(providerId);
    m_panel->verticalScrollBar()->setValue(scroll);
}

// ------------------------------------------------------------------------------------------------ API key modal

LC_AIKeyDialog::LC_AIKeyDialog(const LC_AIProvider& provider, const LC_AITheme& theme, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("%1 API key").arg(provider.name));
    setStyleSheet(theme.styleSheet() + QStringLiteral("QDialog { background: %1; }").arg(css(theme.bgElevated)));
    setMinimumWidth(560);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* head = new QLabel(tr("%1 API key").arg(provider.name), this);
    head->setObjectName("modalHead");
    head->setContentsMargins(16, 14, 16, 6);
    layout->addWidget(head);

    auto* body = new QWidget(this);
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(16, 4, 16, 12);
    bodyLayout->setSpacing(12);

    auto* banner = new QFrame(body);
    banner->setObjectName("bannerInfo");
    auto* bannerLayout = new QHBoxLayout(banner);
    bannerLayout->setContentsMargins(14, 10, 14, 10);
    bannerLayout->setSpacing(10);
    auto* bannerIcon = new QLabel(banner);
    bannerIcon->setPixmap(LC_AIIcons::lucide("check", 15, theme.accentBlue, devicePixelRatioF()));
    auto* bannerText = new QLabel(tr("Saved. Models from %1 are now available.").arg(provider.name), banner);
    bannerLayout->addWidget(bannerIcon);
    bannerLayout->addWidget(bannerText, 1);
    banner->hide();
    bodyLayout->addWidget(banner);

    auto* step = new QWidget(body);
    auto* stepLayout = new QVBoxLayout(step);
    stepLayout->setContentsMargins(0, 0, 0, 0);
    stepLayout->setSpacing(8);
    auto* message = new QLabel(provider.envVar.isEmpty()
                                   ? tr("Paste your %1 API key.").arg(provider.name)
                                   : tr("Paste your %1 API key. It is used instead of <code>%2</code>.")
                                         .arg(provider.name, provider.envVar), step);
    message->setObjectName("modalText");
    message->setWordWrap(true);
    stepLayout->addWidget(message);
    auto* form = new QHBoxLayout();
    form->setSpacing(8);
    auto* input = new QLineEdit(step);
    input->setObjectName("modalInput");
    input->setEchoMode(QLineEdit::Password);
    input->setPlaceholderText(tr("API key"));
    auto* proceed = new QPushButton(tr("Continue"), step);
    proceed->setProperty("kind", "accent");
    proceed->setCursor(Qt::PointingHandCursor);
    proceed->setDefault(true);
    form->addWidget(input, 1);
    form->addWidget(proceed);
    stepLayout->addLayout(form);
    bodyLayout->addWidget(step);
    layout->addWidget(body);

    auto* foot = new QHBoxLayout();
    foot->setContentsMargins(16, 4, 16, 14);
    foot->addStretch(1);
    auto* cancel = new QPushButton(tr("Cancel"), this);
    cancel->setProperty("kind", "ghost");
    cancel->setCursor(Qt::PointingHandCursor);
    cancel->setAutoDefault(false);
    foot->addWidget(cancel);
    layout->addLayout(foot);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    const auto submit = [this, input, step, banner, cancel, id = provider.id] {
        const QString key = input->text().trimmed();
        if (key.isEmpty()) {
            input->setFocus();
            return;
        }
        LC_AICredentials::instance().setApiKey(id, key);
        input->clear();
        step->hide();
        banner->show();
        cancel->setText(tr("Close"));
        cancel->setDefault(true);
        cancel->setFocus();
        adjustSize();
    };
    connect(proceed, &QPushButton::clicked, this, submit);
    connect(input, &QLineEdit::returnPressed, this, submit);
    input->setFocus();
}
