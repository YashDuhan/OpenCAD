/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aiapprovals.h"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include "lc_aiicons.h"

namespace {
    // Cypher's .approve colors
    const QColor GREEN("#4ade80");
    const QColor BLUE("#4c8dff");
    const QColor YELLOW("#fbbf24");
    const QColor RED("#f87171");
    constexpr int MAX_PREVIEW = 1500;

    QString rgba(const QColor& c, const double alpha) {
        return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha);
    }

    QString mono() {
        return QStringLiteral("\"JetBrains Mono Variable\", \"JetBrains Mono\", monospace");
    }

    /** What the call will do, the way Cypher shows a command or path instead of raw JSON. */
    QString preview(const LC_AIToolCall& call) {
        const QJsonObject& a = call.arguments;
        QString text;
        if (call.name == "run_command") {
            QStringList lines;
            for (const QJsonValue& v : a.value("commands").toArray()) {
                if (!v.toString().trimmed().isEmpty()) {
                    lines << v.toString().trimmed(); // as run_command types them: blank entries are skipped
                }
            }
            text = lines.isEmpty() ? a.value("commands").toString() : lines.join('\n');
        }
        else if (call.name == "add_text") {
            QStringList lines;
            QList<QJsonObject> labels;
            if (a.contains("text")) {
                labels << a;
            }
            for (const QJsonValue& v : a.value("labels").toArray()) {
                labels << v.toObject();
            }
            for (const QJsonObject& l : std::as_const(labels)) {
                lines << QStringLiteral("\"%1\" at (%2, %3)").arg(l.value("text").toString().replace('\n', " / "))
                             .arg(l.value("x").toDouble()).arg(l.value("y").toDouble());
            }
            text = lines.join('\n');
        }
        else if (call.name == "fix_drawing_issues") {
            // which of the checker's fixes, and where: "{ }" told the user nothing
            const auto list = [&a](const QString& key) {
                QStringList items;
                for (const QJsonValue& v : a.value(key).toArray()) {
                    items << (v.isDouble() ? QStringLiteral("#%1").arg(v.toInteger()) : v.toString().replace('_', ' '));
                }
                return items.join(", ");
            };
            QStringList lines;
            const QString rules = list("rules");
            lines << (rules.isEmpty() ? QObject::tr("Fixes: all (zero length, duplicate, overlap, endpoint gap, nearly closed, bylayer)")
                                      : QObject::tr("Fixes: %1").arg(rules));
            if (const QString ids = list("ids"); !ids.isEmpty()) {
                lines << QObject::tr("Only issues with: %1").arg(ids);
            }
            const QString scope = a.value("scope").toString();
            lines << (scope == "selection" ? QObject::tr("In: the selected entities")
                      : scope == "layer"   ? QObject::tr("In: layer \"%1\"").arg(a.value("layer").toString())
                                           : QObject::tr("In: the whole drawing"));
            text = lines.join('\n');
        }
        else if (a.value("path").isString()) {
            text = a.value("path").toString();
        }
        else {
            text = QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Indented)).trimmed();
        }
        return text.size() > MAX_PREVIEW ? text.left(MAX_PREVIEW) + "\n…" : text;
    }

    /**
     * A one-line label that never makes its row wider than the box: shortened with "…" when there is no room (the
     * whole text in its tooltip). A long reason ("Safety rule: deletes entities ...") pushed Deny out of view.
     */
    class ElidedLabel : public QLabel {
    public:
        /** `keep`: how many characters stay before it is shortened (the title keeps more than the reason beside it). */
        ElidedLabel(const QString& text, const int keep, QWidget* parent) : QLabel(parent), m_full(text), m_keep(keep) {
            QLabel::setText(text);
            setToolTip(text);
            setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        }
        QSize sizeHint() const override { return withTextWidth(fontMetrics().horizontalAdvance(m_full)); }
        QSize minimumSizeHint() const override {
            return withTextWidth(qMin(fontMetrics().horizontalAdvance(m_full),
                                      fontMetrics().horizontalAdvance(m_full.left(m_keep) + QString::fromUtf8("…"))));
        }

    protected:
        void resizeEvent(QResizeEvent* event) override {
            QLabel::resizeEvent(event);
            const QString shown = fontMetrics().elidedText(m_full, Qt::ElideRight, contentsRect().width());
            if (shown != text()) {
                QLabel::setText(shown); // the size hints use the full text, so this does not lay the row out again
            }
        }

    private:
        /** The label's size with a text this wide: its padding and frame (style sheet) stay as they are. */
        QSize withTextWidth(const int width) const {
            const QSize current = QLabel::sizeHint();
            return {current.width() - fontMetrics().horizontalAdvance(text()) + width, current.height()};
        }

        QString m_full;
        int m_keep;
    };

    class ClickFrame : public QFrame {
    public:
        ClickFrame(std::function<void()> onClick, QWidget* parent) : QFrame(parent), m_onClick(std::move(onClick)) {
            setAttribute(Qt::WA_Hover);
            setCursor(Qt::PointingHandCursor);
        }

    protected:
        void mouseReleaseEvent(QMouseEvent* event) override {
            if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
                m_onClick();
            }
        }

    private:
        std::function<void()> m_onClick;
    };

    QString segStyle(const LC_AITheme& t) {
        return QStringLiteral(R"(
QPushButton[seg="true"] { background: transparent; border: 1px solid %1; color: %2; border-radius: 0; min-height: 24px; max-height: 24px; padding: 0 10px; font-size: 12px; }
QPushButton[seg="true"][on="true"] { background: %3; color: %4; }
QPushButton[segPos="left"] { border-top-left-radius: 7px; border-bottom-left-radius: 7px; }
QPushButton[segPos="mid"] { border-left: none; }
QPushButton[segPos="right"] { border-top-right-radius: 7px; border-bottom-right-radius: 7px; border-left: none; }
)")
            .arg(t.borderDefault.name(QColor::HexArgb), t.textSecondary.name(), t.bgActive.name(QColor::HexArgb), t.textHeading.name());
    }
}

// ------------------------------------------------------------------------------------------------ pending box

namespace {
    constexpr int PREVIEW_LINES = 4;      // lines of a call shown before "Show all"
    constexpr int EXPANDED_PREVIEW_MAX = 160;

    /** One sentence on what the call will do, above its preview. */
    QString whatItDoes(const LC_AIToolCall& call, const int lines) {
        if (call.name == "run_command") {
            return lines == 1 ? QObject::tr("Types 1 line into the command line")
                              : QObject::tr("Types %1 lines into the command line").arg(lines);
        }
        if (call.name == "add_text") {
            return lines == 1 ? QObject::tr("Adds 1 text label to the drawing")
                              : QObject::tr("Adds %1 text labels to the drawing").arg(lines);
        }
        if (call.name == "fix_drawing_issues") {
            return QObject::tr("Changes the drawing in one undo step, and may delete entities (duplicates, zero length)");
        }
        if (call.name.startsWith("mcp__")) {
            return QObject::tr("Calls the MCP server \"%1\"").arg(call.name.section("__", 1, 1));
        }
        return QObject::tr("Runs with these arguments");
    }
}

LC_AIApprovalBox::LC_AIApprovalBox(const LC_AITheme& theme, QWidget* parent)
    : QFrame(parent), m_theme(theme) {
    setObjectName("pendingBox");
    setStyleSheet(QStringLiteral(R"(
#pendingBox { background: %1; border: 1px solid %2; border-radius: 12px; }
#pendingHead { font-size: 12px; font-weight: 600; color: %3; }
#pendingItem { border: none; border-top: 1px solid %4; background: transparent; }
#pendingItem[first="true"] { border-top: none; }
#pendingTool { font-size: 13px; font-weight: 600; color: %5; }
#pendingReason { font-size: 11px; color: %3; background: %9; border-radius: 5px; padding: 1px 6px; }
#pendingWhat { font-size: 12px; color: %10; }
#pendingPre { font-family: %6; font-size: 11.5px; color: %7; background: %8; border-radius: 6px; padding: 6px 8px; }
#pendingMore { border: none; background: transparent; color: %11; font-size: 12px; padding: 0; text-align: left; }
#pendingMore:hover { text-decoration: underline; }
QScrollArea#pendingScroll, #pendingList { background: transparent; border: none; }
QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
QScrollBar::handle:vertical { background: %12; border-radius: 3px; min-height: 24px; }
QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page { background: none; height: 0; width: 0; }
)")
                      .arg(theme.bgElevated.name(), rgba(YELLOW, 0.3), YELLOW.name(), theme.borderSubtle.name(QColor::HexArgb),
                           theme.textPrimary.name(), mono(), theme.textSecondary.name(),
                           theme.dark ? QStringLiteral("rgba(0, 0, 0, 0.3)") : QStringLiteral("rgba(0, 0, 0, 0.05)"),
                           rgba(YELLOW, 0.12))
                      .arg(theme.textMuted.name(), BLUE.name(), theme.borderStrong.name(QColor::HexArgb)));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 8, 12, 10);
    layout->setSpacing(4);
    auto* head = new QHBoxLayout();
    head->setSpacing(6);
    auto* icon = new QLabel(this);
    icon->setPixmap(LC_AIIcons::lucide("shield-alert", 13, YELLOW, devicePixelRatioF()));
    m_head = new QLabel(this);
    m_head->setObjectName("pendingHead");
    head->addWidget(icon);
    head->addWidget(m_head, 1);
    layout->addLayout(head);
    // several pending calls scroll inside the box instead of pushing the chat away
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName("pendingScroll");
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* list = new QWidget();
    list->setObjectName("pendingList");
    m_items = new QVBoxLayout(list);
    m_items->setContentsMargins(0, 0, 0, 0);
    m_items->setSpacing(0);
    m_items->addStretch(1);
    m_scroll->setWidget(list);
    layout->addWidget(m_scroll);
    hide();
}

QPushButton* LC_AIApprovalBox::approveButton(const QString& text, const QString& icon, const QColor& color, const QString& kind) {
    auto* b = new QPushButton(text);
    if (!icon.isEmpty()) {
        b->setIcon(LC_AIIcons::lucideIcon(icon, 11, color));
        b->setIconSize(QSize(11, 11));
    }
    b->setObjectName("approve_" + kind);
    b->setCursor(Qt::PointingHandCursor);
    const bool primary = kind == "once";
    b->setStyleSheet(QStringLiteral("QPushButton { color: %1; background: %2; border: 1px solid %3; border-radius: 6px; "
                                    "padding: 3px 9px; font-size: 12px; font-weight: %5; min-height: 20px; }"
                                    "QPushButton:hover { background: %4; }")
                         .arg(color.name(), rgba(color, primary ? 0.18 : 0.08), rgba(color, primary ? 0.45 : 0.28),
                              rgba(color, primary ? 0.28 : 0.16), primary ? QStringLiteral("600") : QStringLiteral("400")));
    return b;
}

void LC_AIApprovalBox::fitHeight() {
    // at most 45% of the AI panel, so the conversation stays visible
    const int limit = parentWidget() != nullptr ? qMax(160, int(parentWidget()->height() * 0.45)) : 360;
    QWidget* list = m_scroll->widget();
    list->adjustSize();
    const int content = list->sizeHint().height();
    m_scroll->setFixedHeight(qMin(content, limit - 40));
}

void LC_AIApprovalBox::add(const LC_AIToolCall& call, const QString& label, const LC_AIPermissions::Decision& decision,
                           const Answer& answer) {
    auto* item = new QFrame(m_scroll->widget());
    item->setObjectName("pendingItem");
    item->setProperty("first", m_count == 0);
    auto* layout = new QVBoxLayout(item);
    layout->setContentsMargins(0, 6, 0, 8);
    layout->setSpacing(5);

    auto* title = new QHBoxLayout();
    title->setSpacing(8);
    auto* name = new ElidedLabel(tr("Approve %1?").arg(label), 24, item); // "Approve Run command?" stays whole
    name->setObjectName("pendingTool");
    title->addWidget(name);
    if (!decision.reason.isEmpty()) {
        auto* reason = new ElidedLabel(decision.reason, 6, item);
        reason->setObjectName("pendingReason");
        title->addWidget(reason, 0, Qt::AlignVCenter);
    }
    title->addStretch(1);
    layout->addLayout(title);

    const QString text = preview(call);
    const QStringList lines = text.split('\n');
    auto* what = new QLabel(whatItDoes(call, int(lines.size())), item);
    what->setObjectName("pendingWhat");
    what->setWordWrap(true); // e.g. a long MCP server name
    what->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(what);

    // a few lines, then "Show all": long command lists no longer fill the panel
    const bool longText = lines.size() > PREVIEW_LINES;
    auto* pre = new QLabel(longText ? lines.mid(0, PREVIEW_LINES).join('\n') : text, item);
    pre->setObjectName("pendingPre");
    pre->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    pre->setTextFormat(Qt::PlainText);
    pre->setWordWrap(true);
    pre->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pre->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* preScroll = new QScrollArea(item);
    preScroll->setObjectName("pendingScroll");
    preScroll->setWidgetResizable(true);
    preScroll->setFrameShape(QFrame::NoFrame);
    preScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    preScroll->setWidget(pre);
    preScroll->setFixedHeight(pre->sizeHint().height() + 4);
    layout->addWidget(preScroll);
    if (longText) {
        auto* more = new QPushButton(tr("Show all %1 lines").arg(lines.size()), item);
        more->setObjectName("pendingMore");
        more->setCursor(Qt::PointingHandCursor);
        connect(more, &QPushButton::clicked, this, [this, more, pre, preScroll, text, lines] {
            const bool expand = more->property("expanded").toBool() == false;
            more->setProperty("expanded", expand);
            pre->setText(expand ? text : lines.mid(0, PREVIEW_LINES).join('\n'));
            more->setText(expand ? tr("Show less") : tr("Show all %1 lines").arg(lines.size()));
            pre->adjustSize();
            preScroll->setFixedHeight(qMin(pre->sizeHint().height() + 4, EXPANDED_PREVIEW_MAX));
            fitHeight();
        });
        layout->addWidget(more, 0, Qt::AlignLeft);
    }

    // one compact row of answers
    auto* actions = new QHBoxLayout();
    actions->setSpacing(5);
    QPushButton* once = approveButton(tr("Allow"), "check", GREEN, "once");
    once->setToolTip(tr("Allow this call once"));
    QPushButton* session = approveButton(tr("Session"), QString(), BLUE, "session");
    session->setToolTip(tr("Allow this kind of action without asking until you start a new chat or restart OpenCAD"));
    QPushButton* always = decision.ruleAsked ? nullptr : approveButton(tr("Always"), QString(), YELLOW, "always");
    if (always != nullptr) {
        always->setToolTip(tr("Always allow this tool (change it later in Tool permissions)"));
    }
    QPushButton* deny = approveButton(tr("Deny"), "x", RED, "deny");
    deny->setToolTip(tr("Refuse this call; the AI is told to stop and ask you"));
    actions->addWidget(once);
    actions->addWidget(session);
    if (always != nullptr) {
        actions->addWidget(always);
    }
    actions->addStretch(1);
    actions->addWidget(deny);
    layout->addLayout(actions);

    m_items->insertWidget(m_items->count() - 1, item); // above the stretch
    ++m_count;
    updateHead();
    show();
    fitHeight();
    QTimer::singleShot(0, this, [this] { fitHeight(); }); // again once the new item is laid out

    const auto choose = [this, item, answer](const Choice choice) {
        item->hide();
        item->deleteLater();
        --m_count;
        updateHead();
        if (m_count <= 0) {
            hide();
        }
        else {
            QTimer::singleShot(0, this, [this] { fitHeight(); });
        }
        answer(choice);
    };
    connect(once, &QPushButton::clicked, this, [choose] { choose(Choice::Once); });
    connect(session, &QPushButton::clicked, this, [choose] { choose(Choice::Session); });
    if (always != nullptr) {
        connect(always, &QPushButton::clicked, this, [choose] { choose(Choice::Always); });
    }
    connect(deny, &QPushButton::clicked, this, [choose] { choose(Choice::Deny); });
}

void LC_AIApprovalBox::updateHead() {
    m_head->setText(m_count <= 1 ? tr("Approval needed") : tr("%1 approvals needed").arg(m_count));
}

void LC_AIApprovalBox::clear() {
    while (m_items->count() > 1) { // keep the stretch
        QLayoutItem* item = m_items->takeAt(0);
        if (QWidget* w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    m_count = 0;
    hide();
}

// ------------------------------------------------------------------------------------------------ mode button

LC_AIApprovalModeButton::LC_AIApprovalModeButton(const LC_AITheme& theme, QWidget* parent)
    : QPushButton(parent), m_theme(theme) {
    setObjectName("approvalBtn");
    setCursor(Qt::PointingHandCursor);
    setIconSize(QSize(14, 14));
    connect(this, &QPushButton::clicked, this, &LC_AIApprovalModeButton::openMenu);
    refresh();
}

void LC_AIApprovalModeButton::refresh() {
    const LC_AIPermissions::Mode mode = LC_AIPermissions::mode();
    const bool full = mode == LC_AIPermissions::Mode::Full;
    const QColor color = full ? YELLOW : m_theme.textMuted; // .approval-btn.full is yellow
    setIcon(LC_AIIcons::lucideIcon(LC_AIPermissions::modeIcon(mode), 14, color));
    setText(property("compact").toBool() ? QString() : LC_AIPermissions::modeShortName(mode)); // a narrow panel: the icon only
    setToolTip(LC_AIPermissions::modeName(mode) + ": " + LC_AIPermissions::modeHint(mode));
    setStyleSheet(QStringLiteral("QPushButton#approvalBtn { border: none; border-radius: 6px; padding: 0 8px; min-height: 26px; "
                                 "max-height: 26px; font-size: 12px; font-weight: 500; color: %1; background: %2; text-align: left; }"
                                 "QPushButton#approvalBtn:hover { background: %3; }")
                      .arg(color.name(), full ? rgba(YELLOW, 0.1) : QStringLiteral("transparent"),
                           m_theme.bgHover.name(QColor::HexArgb)));
}

void LC_AIApprovalModeButton::openMenu() {
    constexpr int MENU_WIDTH = 330;
    auto* menu = new QFrame(this, Qt::Popup | Qt::FramelessWindowHint);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setObjectName("approvalMenu");
    const LC_AITheme& t = m_theme;
    menu->setStyleSheet(QStringLiteral(R"(
#approvalMenu { background: %1; border: 1px solid %2; border-radius: 10px; }
#approvalItem { border: none; border-radius: 8px; background: transparent; }
#approvalItem:hover { background: %3; }
#approvalItem[on="true"] { background: %4; }
#approvalName { font-size: 13px; color: %5; }
#approvalHint { font-size: 12px; color: %6; }
#approvalSep { background: %7; border: none; }
)")
                            .arg(t.bgElevated.name(), t.borderOverlay.name(QColor::HexArgb), t.bgHover.name(QColor::HexArgb),
                                 t.bgActive.name(QColor::HexArgb), t.textPrimary.name(), t.textFaint.name(),
                                 t.borderSubtle.name(QColor::HexArgb)));
    auto* layout = new QVBoxLayout(menu);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(2);
    const qreal dpr = devicePixelRatioF();
    const LC_AIPermissions::Mode current = LC_AIPermissions::mode();
    // .approval-item: icon, name over a wrapped hint, check on the current mode
    const auto item = [&](const QString& icon, const QString& name, const QString& hint, const bool on,
                          const std::function<void()>& onClick) {
        auto* row = new ClickFrame([menu, onClick] {
            menu->close();
            onClick();
        }, menu);
        row->setObjectName("approvalItem");
        row->setProperty("on", on);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(10, 8, 10, 8);
        h->setSpacing(10);
        auto* ic = new QLabel(row);
        ic->setPixmap(LC_AIIcons::lucide(icon, 14, on ? t.textHeading : t.textMuted, dpr));
        auto* text = new QVBoxLayout();
        text->setSpacing(1);
        auto* n = new QLabel(name, row);
        n->setObjectName("approvalName");
        text->addWidget(n);
        if (!hint.isEmpty()) {
            auto* hl = new QLabel(hint, row);
            hl->setObjectName("approvalHint");
            hl->setWordWrap(true);
            text->addWidget(hl);
        }
        h->addWidget(ic, 0, Qt::AlignTop);
        h->addLayout(text, 1);
        if (on) {
            auto* check = new QLabel(row);
            check->setPixmap(LC_AIIcons::lucide("check", 13, t.textHeading, dpr));
            h->addWidget(check, 0, Qt::AlignTop);
        }
        layout->addWidget(row);
    };
    for (const LC_AIPermissions::Mode mode : {LC_AIPermissions::Mode::Supervised, LC_AIPermissions::Mode::Edits,
                                              LC_AIPermissions::Mode::Auto, LC_AIPermissions::Mode::Full}) {
        item(LC_AIPermissions::modeIcon(mode), LC_AIPermissions::modeName(mode), LC_AIPermissions::modeHint(mode),
             mode == current, [this, mode] {
                 LC_AIPermissions::setMode(mode);
                 refresh();
             });
    }
    auto* sep = new QFrame(menu);
    sep->setObjectName("approvalSep");
    sep->setFixedHeight(1);
    layout->addSpacing(2);
    layout->addWidget(sep);
    layout->addSpacing(2);
    item("shield-check", tr("Tool permissions..."), QString(), false, [this] { emit permissionsRequested(); });

    // exactly as tall as its rows at this width: no stretched gaps between them
    menu->setFixedWidth(MENU_WIDTH);
    menu->setFixedHeight(layout->hasHeightForWidth() ? layout->totalHeightForWidth(MENU_WIDTH) : layout->totalSizeHint().height());
    // open above the button (it sits at the bottom of the panel), left edges aligned
    QPoint pos = mapToGlobal(QPoint(0, -menu->height() - 6));
    if (screen() != nullptr) {
        const QRect area = screen()->availableGeometry();
        pos.setX(qBound(area.left(), pos.x(), area.right() - menu->width()));
        if (pos.y() < area.top()) {
            pos.setY(mapToGlobal(QPoint(0, height() + 6)).y());
        }
    }
    menu->move(pos);
    menu->show();
}

// ------------------------------------------------------------------------------------------------ permissions dialog

LC_AIToolPermissionsDialog::LC_AIToolPermissionsDialog(const QList<LC_AITool>& tools, const LC_AITheme& theme, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Tool permissions"));
    setStyleSheet(theme.styleSheet() + segStyle(theme) +
                  QStringLiteral("#permRow { border: none; border-top: 1px solid %1; } #permName { font-size: 13px; color: %2; }"
                                 "#permDesc { font-size: 12px; color: %3; } #permDefault { font-size: 11.5px; color: %3; }")
                      .arg(theme.borderSubtle.name(QColor::HexArgb), theme.textPrimary.name(), theme.textFaint.name()));
    QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
    resize(qMin(720, int(screen.width() * 0.9)), qMin(680, int(screen.height() * 0.85)));

    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(24, 18, 24, 18);
    page->setSpacing(10);
    auto* title = new QLabel(tr("Tool permissions"), this);
    title->setObjectName("pvTitle");
    auto* intro = new QLabel(tr("Allow runs a tool without asking, Ask shows the approval box, Deny refuses it. The approval "
                                "mode can approve tools set to Ask; Deny holds in every mode."), this);
    intro->setObjectName("pvIntro");
    intro->setWordWrap(true);
    page->addWidget(title);
    page->addWidget(intro);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget();
    auto* list = new QVBoxLayout(content);
    list->setContentsMargins(0, 0, 8, 0);
    list->setSpacing(0);

    QString lastGroup;
    for (const LC_AITool& tool : tools) {
        const QString group = tool.name.startsWith("mcp__") ? tr("MCP: %1").arg(tool.name.section("__", 1, 1)) : tr("Built-in");
        if (group != lastGroup) {
            auto* g = new QLabel(group, content);
            g->setObjectName("pvSection");
            g->setContentsMargins(0, 14, 0, 6);
            list->addWidget(g);
            lastGroup = group;
        }
        auto* row = new QFrame(content);
        row->setObjectName("permRow");
        auto* r = new QHBoxLayout(row);
        r->setContentsMargins(0, 8, 0, 8);
        r->setSpacing(12);
        auto* text = new QVBoxLayout();
        text->setSpacing(1);
        auto* name = new QLabel(tool.label, row);
        name->setObjectName("permName");
        auto* desc = new QLabel(row);
        desc->setObjectName("permDesc");
        desc->setText(desc->fontMetrics().elidedText(QString(tool.description).replace('\n', ' '), Qt::ElideRight, 380));
        desc->setToolTip(tool.description);
        text->addWidget(name);
        text->addWidget(desc);
        r->addLayout(text, 1);
        auto* defaultHint = new QLabel(row);
        defaultHint->setObjectName("permDefault");
        r->addWidget(defaultHint);

        QList<QPushButton*> segs;
        const QStringList labels{tr("Allow"), tr("Ask"), tr("Deny")};
        const QList<LC_AIPermissions::Permission> values{LC_AIPermissions::Permission::Allow, LC_AIPermissions::Permission::Ask,
                                                         LC_AIPermissions::Permission::Deny};
        auto* seg = new QHBoxLayout();
        seg->setSpacing(0);
        for (int i = 0; i < 3; ++i) {
            auto* b = new QPushButton(labels[i], row);
            b->setProperty("seg", true);
            b->setProperty("segPos", i == 0 ? "left" : i == 2 ? "right" : "mid");
            b->setCursor(Qt::PointingHandCursor);
            seg->addWidget(b);
            segs << b;
        }
        r->addLayout(seg);
        const QString toolName = tool.name;
        const auto sync = [segs, values, toolName, defaultHint] {
            const LC_AIPermissions::Permission current = LC_AIPermissions::permission(toolName);
            for (int i = 0; i < segs.size(); ++i) {
                segs[i]->setProperty("on", values[i] == current);
                segs[i]->style()->unpolish(segs[i]);
                segs[i]->style()->polish(segs[i]);
            }
            defaultHint->setText(LC_AIPermissions::hasSavedPermission(toolName) ? QString() : tr("default"));
        };
        for (int i = 0; i < segs.size(); ++i) {
            const LC_AIPermissions::Permission value = values[i];
            connect(segs[i], &QPushButton::clicked, this, [toolName, value, sync] {
                if (value == LC_AIPermissions::defaultPermission(toolName)) {
                    LC_AIPermissions::resetPermission(toolName); // back to the default: nothing saved
                }
                else {
                    LC_AIPermissions::setPermission(toolName, value);
                }
                sync();
            });
        }
        sync();
        list->addWidget(row);
    }
    list->addStretch(1);
    scroll->setWidget(content);
    page->addWidget(scroll, 1);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto* close = new QPushButton(tr("Close"), this);
    close->setProperty("kind", "accent");
    close->setCursor(Qt::PointingHandCursor);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(close);
    page->addLayout(buttons);
}
