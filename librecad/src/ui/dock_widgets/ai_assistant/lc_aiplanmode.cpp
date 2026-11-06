/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aiplanmode.h"

#include <QAbstractTextDocumentLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>

#include "lc_aicredentials.h"
#include "lc_aiicons.h"
#include "lc_aipermissions.h"

namespace {
    // Cypher's .approve colours, as the approval box uses them
    const QColor GREEN("#4ade80");
    const QColor BLUE("#4c8dff");
    const QColor RED("#f87171");
    const QColor DONE("#22c55e"); // the transcript's "done" green

    const QString PROPOSE_PLAN = QStringLiteral("propose_plan");
    const QString UPDATE_PLAN = QStringLiteral("update_plan");
    const QString SETTING_KEY = QStringLiteral("planFirst");
    constexpr int MAX_STEPS = 20;
    constexpr int TRACKER_MAX_HEIGHT = 150; // about six steps, then the list scrolls

    QString rgba(const QColor& c, const double alpha) {
        return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha);
    }

    /** A small button like the approval box's: tinted in its colour, stronger when it is the main answer. */
    QPushButton* actionButton(const QString& text, const QString& icon, const QColor& color, const bool primary, QWidget* parent) {
        auto* b = new QPushButton(text, parent);
        if (!icon.isEmpty()) {
            b->setIcon(LC_AIIcons::lucideIcon(icon, 11, color));
            b->setIconSize(QSize(11, 11));
        }
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QStringLiteral("QPushButton { color: %1; background: %2; border: 1px solid %3; border-radius: 6px; "
                                        "padding: 3px 9px; font-size: 12px; font-weight: %5; min-height: 20px; }"
                                        "QPushButton:hover { background: %4; }")
                             .arg(color.name(), rgba(color, primary ? 0.18 : 0.08), rgba(color, primary ? 0.45 : 0.28),
                                  rgba(color, primary ? 0.28 : 0.16), primary ? QStringLiteral("600") : QStringLiteral("400")));
        return b;
    }

    /** "1. Draw the outline" and "- Draw the outline" are both just "Draw the outline". */
    QString stepText(QString text) {
        static const QRegularExpression bullet(QStringLiteral(R"(^\s*(?:\d+\s*[.):]|[-*•])\s+)"));
        text.remove(bullet);
        return text.simplified();
    }

    /** The steps of a plan as the model sent them: strings, objects with a text, or one string with a step per line. */
    QStringList stepsFrom(const QJsonValue& value) {
        QStringList steps;
        const auto addStep = [&steps](const QString& text) {
            const QString step = stepText(text);
            if (!step.isEmpty() && steps.size() < MAX_STEPS) {
                steps << step;
            }
        };
        if (value.isString()) {
            for (const QString& line : value.toString().split('\n')) {
                addStep(line);
            }
            return steps;
        }
        for (const QJsonValue& v : value.toArray()) {
            if (v.isObject()) {
                const QJsonObject o = v.toObject();
                for (const char* key : {"text", "title", "step", "description", "name"}) {
                    if (o.value(key).isString()) {
                        addStep(o.value(key).toString());
                        break;
                    }
                }
            }
            else {
                addStep(v.toVariant().toString());
            }
        }
        return steps;
    }

    QString numbered(const QStringList& steps) {
        QStringList lines;
        for (int i = 0; i < steps.size(); ++i) {
            lines << QStringLiteral("%1. %2").arg(i + 1).arg(steps[i]);
        }
        return lines.join('\n');
    }

    bool finished(const LC_AIPlanStep& step) {
        return step.status == LC_AIPlanStep::Status::Done || step.status == LC_AIPlanStep::Status::Skipped;
    }

    /** The step being worked on: the last one started, else the first one still to do; -1 when all are finished. */
    int currentStep(const QList<LC_AIPlanStep>& steps) {
        for (int i = int(steps.size()) - 1; i >= 0; --i) {
            if (steps[i].status == LC_AIPlanStep::Status::InProgress) {
                return i;
            }
        }
        for (int i = 0; i < steps.size(); ++i) {
            if (!finished(steps[i])) {
                return i;
            }
        }
        return -1;
    }
}

// ------------------------------------------------------------------------------------------------ status icon

LC_AIStatusIcon::LC_AIStatusIcon(const LC_AITheme& theme, const int size, QWidget* parent)
    : QWidget(parent), m_theme(theme), m_size(size) {
    setFixedSize(size, size);
    m_timer = new QTimer(this);
    m_timer->setInterval(90);
    connect(m_timer, &QTimer::timeout, this, [this] {
        m_angle = (m_angle + 30) % 360;
        update();
    });
    setStatus(LC_AIPlanStep::Status::Pending);
}

void LC_AIStatusIcon::setStatus(const LC_AIPlanStep::Status status) {
    const qreal dpr = devicePixelRatioF();
    switch (status) {
        case LC_AIPlanStep::Status::Pending:
            m_spinning = false;
            m_pixmap = m_showPending ? LC_AIIcons::lucide("circle", m_size, m_theme.textFaint, dpr) : QPixmap();
            break;
        case LC_AIPlanStep::Status::InProgress:
            setSpinning(BLUE);
            return;
        case LC_AIPlanStep::Status::Done:
            m_spinning = false;
            m_pixmap = LC_AIIcons::lucide("circle-check", m_size, DONE, dpr);
            break;
        case LC_AIPlanStep::Status::Skipped:
            m_spinning = false;
            m_pixmap = LC_AIIcons::lucide("circle-slash", m_size, m_theme.textFaint, dpr);
            break;
    }
    m_timer->stop();
    update();
}

void LC_AIStatusIcon::setIcon(const QString& name, const QColor& color) {
    m_spinning = false;
    m_timer->stop();
    m_pixmap = LC_AIIcons::lucide(name, m_size, color, devicePixelRatioF());
    update();
}

void LC_AIStatusIcon::setSpinning(const QColor& color) {
    m_spinning = true;
    m_pixmap = LC_AIIcons::lucide("loader", m_size, color, devicePixelRatioF());
    if (isVisible()) {
        m_timer->start();
    }
    update();
}

void LC_AIStatusIcon::paintEvent(QPaintEvent*) {
    if (m_pixmap.isNull()) {
        return;
    }
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::Antialiasing);
    if (m_spinning) {
        const QPointF centre(m_size / 2.0, m_size / 2.0);
        p.translate(centre);
        p.rotate(m_angle);
        p.translate(-centre);
    }
    p.drawPixmap(QPointF(0, 0), m_pixmap);
}

void LC_AIStatusIcon::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_spinning) {
        m_timer->start(); // only turns while it can be seen
    }
}

void LC_AIStatusIcon::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    m_timer->stop();
}

// ------------------------------------------------------------------------------------------------ plan card

LC_AIPlanCard::LC_AIPlanCard(const QString& title, const QStringList& steps, const LC_AITheme& theme, QWidget* parent)
    : QFrame(parent), m_theme(theme), m_steps(steps) {
    setObjectName("planCard");
    const LC_AITheme& t = theme;
    setStyleSheet(QStringLiteral(R"(
#planCard { background: %1; border: 1px solid %2; border-radius: 10px; }
#planKind { font-size: 12px; font-weight: 500; color: %3; }
#planTitle { font-size: 13.5px; font-weight: 600; color: %4; }
#planBadge { background: %5; color: %6; border-radius: 5px; font-size: 11.5px; font-weight: 600; }
#planStep { font-size: 13px; color: %7; }
#planStep[dim="true"] { color: %3; }
#planEditor { background: %8; border: 1px solid %2; border-radius: 8px; padding: 4px 6px; font-size: 13px; color: %7; }
#planEditor:focus { border-color: %9; }
#planHint { font-size: 11.5px; color: %3; }
#planStatus { font-size: 12px; }
)")
                      .arg(t.bgElevated.name(), t.borderDefault.name(QColor::HexArgb), t.textMuted.name(), t.textHeading.name(),
                           t.bgActive.name(QColor::HexArgb), t.textSecondary.name(), t.textPrimary.name(),
                           t.bgCard.name(QColor::HexArgb), t.accentBlueBorder.name(QColor::HexArgb)));
    const qreal dpr = devicePixelRatioF();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(7);

    // "Proposed plan" with its status on the right, then the title
    auto* head = new QHBoxLayout();
    head->setSpacing(7);
    auto* icon = new QLabel(this);
    icon->setPixmap(LC_AIIcons::lucide("list-checks", 14, BLUE, dpr));
    auto* kind = new QLabel(tr("Proposed plan"), this);
    kind->setObjectName("planKind");
    m_statusRow = new QWidget(this);
    auto* status = new QHBoxLayout(m_statusRow);
    status->setContentsMargins(0, 0, 0, 0);
    status->setSpacing(4);
    m_statusIcon = new QLabel(m_statusRow);
    m_statusText = new QLabel(m_statusRow);
    m_statusText->setObjectName("planStatus");
    status->addWidget(m_statusIcon);
    status->addWidget(m_statusText);
    m_statusRow->hide();
    head->addWidget(icon);
    head->addWidget(kind);
    head->addStretch(1);
    head->addWidget(m_statusRow);
    layout->addLayout(head);
    auto* titleLabel = new QLabel(title.trimmed().isEmpty() ? tr("Plan") : title.trimmed(), this);
    titleLabel->setObjectName("planTitle");
    titleLabel->setWordWrap(true);
    titleLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(titleLabel);

    m_stepList = new QWidget(this);
    auto* list = new QVBoxLayout(m_stepList);
    list->setContentsMargins(0, 2, 0, 2);
    list->setSpacing(5);
    layout->addWidget(m_stepList);
    showSteps(steps);

    // Edit: the steps as plain text, one per line
    m_editor = new QPlainTextEdit(this);
    m_editor->setObjectName("planEditor");
    QPalette pal = m_editor->palette();
    pal.setColor(QPalette::Text, t.textPrimary);
    pal.setColor(QPalette::PlaceholderText, t.textPlaceholder);
    m_editor->setPalette(pal);
    m_editor->setPlaceholderText(tr("One step per line"));
    m_editor->hide();
    layout->addWidget(m_editor);
    m_editHint = new QLabel(tr("One step per line, in order. The AI follows the plan as you leave it."), this);
    m_editHint->setObjectName("planHint");
    m_editHint->setWordWrap(true);
    m_editHint->hide();
    layout->addWidget(m_editHint);

    m_actions = new QWidget(this);
    auto* actions = new QHBoxLayout(m_actions);
    actions->setContentsMargins(0, 2, 0, 0);
    actions->setSpacing(5);
    QPushButton* approve = actionButton(tr("Approve"), "check", GREEN, true, m_actions);
    approve->setToolTip(tr("Let the AI carry out this plan"));
    QPushButton* edit = actionButton(tr("Edit"), "pencil", BLUE, false, m_actions);
    edit->setToolTip(tr("Change the steps before the AI starts"));
    QPushButton* reject = actionButton(tr("Reject"), "x", RED, false, m_actions);
    reject->setToolTip(tr("Turn the plan down; the AI changes nothing and asks what you want instead"));
    actions->addWidget(approve);
    actions->addWidget(edit);
    actions->addStretch(1);
    actions->addWidget(reject);
    layout->addWidget(m_actions);

    m_editActions = new QWidget(this);
    auto* editActions = new QHBoxLayout(m_editActions);
    editActions->setContentsMargins(0, 2, 0, 0);
    editActions->setSpacing(5);
    QPushButton* approveEdited = actionButton(tr("Approve edited plan"), "check", GREEN, true, m_editActions);
    QPushButton* cancel = actionButton(tr("Cancel"), QString(), t.textMuted, false, m_editActions);
    editActions->addWidget(approveEdited);
    editActions->addStretch(1);
    editActions->addWidget(cancel);
    m_editActions->hide();
    layout->addWidget(m_editActions);

    connect(approve, &QPushButton::clicked, this, [this] { answer(Answer::Approved, m_steps); });
    connect(reject, &QPushButton::clicked, this, [this] { answer(Answer::Rejected, m_steps); });
    connect(edit, &QPushButton::clicked, this, [this] { setEditing(true); });
    connect(cancel, &QPushButton::clicked, this, [this] { setEditing(false); });
    connect(approveEdited, &QPushButton::clicked, this, [this] {
        QStringList steps;
        for (const QString& line : m_editor->toPlainText().split('\n')) {
            if (const QString step = stepText(line); !step.isEmpty() && steps.size() < MAX_STEPS) {
                steps << step;
            }
        }
        if (steps.isEmpty()) {
            m_editHint->setText(tr("Write at least one step, or Reject the plan."));
            return;
        }
        answer(steps == m_steps ? Answer::Approved : Answer::Edited, steps);
    });
    connect(m_editor, &QPlainTextEdit::textChanged, this, &LC_AIPlanCard::fitEditor);
    m_editor->installEventFilter(this); // a new width wraps the steps differently
}

void LC_AIPlanCard::fitEditor() {
    // as tall as its lines as they wrap, at least three and at most about ten, then it scrolls
    QTextDocument* doc = m_editor->document();
    qreal lines = 0;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        lines += m_editor->document()->documentLayout()->blockBoundingRect(block).height();
    }
    const int lineHeight = m_editor->fontMetrics().lineSpacing();
    const int chrome = m_editor->height() - m_editor->viewport()->height() + qRound(2 * doc->documentMargin()) + 2;
    const int wanted = qBound(3 * lineHeight, qCeil(lines), 10 * lineHeight) + chrome;
    if (wanted != m_editor->height()) {
        m_editor->setFixedHeight(wanted);
    }
}

bool LC_AIPlanCard::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_editor && event->type() == QEvent::Resize) {
        QTimer::singleShot(0, this, &LC_AIPlanCard::fitEditor);
    }
    return QFrame::eventFilter(watched, event);
}

void LC_AIPlanCard::showSteps(const QStringList& steps) {
    auto* list = static_cast<QVBoxLayout*>(m_stepList->layout());
    while (QLayoutItem* item = list->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->deleteLater();
        }
        delete item->layout();
        delete item;
    }
    m_stepIcons.clear();
    m_stepLabels.clear();
    for (int i = 0; i < steps.size(); ++i) {
        auto* row = new QWidget(m_stepList);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(9);
        auto* badge = new QLabel(QString::number(i + 1), row);
        badge->setObjectName("planBadge");
        badge->setAlignment(Qt::AlignCenter);
        badge->setFixedSize(20, 20);
        auto* text = new QLabel(steps[i], row);
        text->setObjectName("planStep");
        text->setWordWrap(true);
        text->setTextFormat(Qt::PlainText);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto* state = new LC_AIStatusIcon(m_theme, 13, row);
        state->setShowPending(false); // the number says it is still to do
        state->setStatus(LC_AIPlanStep::Status::Pending);
        h->addWidget(badge, 0, Qt::AlignTop);
        h->addWidget(text, 1);
        h->addWidget(state, 0, Qt::AlignTop);
        list->addWidget(row);
        m_stepIcons << state;
        m_stepLabels << text;
    }
}

void LC_AIPlanCard::setEditing(const bool editing) {
    if (m_answered) {
        return;
    }
    if (editing) {
        m_editor->setPlainText(m_steps.join('\n'));
        m_editHint->setText(tr("One step per line, in order. The AI follows the plan as you leave it."));
    }
    m_stepList->setVisible(!editing);
    m_actions->setVisible(!editing);
    m_editor->setVisible(editing);
    m_editHint->setVisible(editing);
    m_editActions->setVisible(editing);
    if (editing) {
        m_editor->setFocus();
        m_editor->moveCursor(QTextCursor::End);
    }
}

void LC_AIPlanCard::setStatus(const QString& icon, const QString& text, const QColor& color) {
    m_statusIcon->setPixmap(icon.isEmpty() ? QPixmap() : LC_AIIcons::lucide(icon, 12, color, devicePixelRatioF()));
    m_statusIcon->setVisible(!icon.isEmpty());
    m_statusText->setText(text);
    m_statusText->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
    m_statusRow->show();
}

void LC_AIPlanCard::answer(const Answer answer, const QStringList& steps) {
    if (m_answered) {
        return;
    }
    m_answered = true;
    m_answer = answer;
    if (answer == Answer::Edited) {
        m_steps = steps;
        showSteps(steps);
    }
    m_editor->hide();
    m_editHint->hide();
    m_editActions->hide();
    m_actions->hide();
    m_stepList->show();
    switch (answer) {
        case Answer::Approved:
            setStatus("check", tr("Approved"), DONE);
            break;
        case Answer::Edited:
            setStatus("pencil", tr("Edited and approved"), BLUE);
            break;
        case Answer::Rejected:
            setStatus("x", tr("Rejected"), RED);
            for (QLabel* label : std::as_const(m_stepLabels)) {
                label->setProperty("dim", true);
                label->style()->unpolish(label);
                label->style()->polish(label);
            }
            break;
    }
    if (m_deliver) {
        m_deliver(answer, m_steps);
    }
}

void LC_AIPlanCard::activate(Deliver deliver) {
    m_activated = true;
    m_deliver = std::move(deliver);
    if (m_answered && !m_stopped) {
        m_deliver(m_answer, m_steps); // answered before the tool ran
    }
}

void LC_AIPlanCard::setStopped() {
    if (m_answered) {
        return;
    }
    m_answered = true;
    m_stopped = true;
    m_editor->hide();
    m_editHint->hide();
    m_editActions->hide();
    m_actions->hide();
    m_stepList->show();
    setStatus(QString(), tr("Not answered (stopped)"), m_theme.textFaint);
}

void LC_AIPlanCard::setProgress(const QList<LC_AIPlanStep>& steps) {
    for (int i = 0; i < m_stepIcons.size() && i < steps.size(); ++i) {
        m_stepIcons[i]->setStatus(steps[i].status);
        m_stepIcons[i]->setToolTip(steps[i].note);
    }
}

// ------------------------------------------------------------------------------------------------ tracker

LC_AIPlanTracker::LC_AIPlanTracker(const LC_AITheme& theme, QWidget* parent)
    : QFrame(parent), m_theme(theme) {
    setObjectName("planTracker");
    const LC_AITheme& t = theme;
    setStyleSheet(QStringLiteral(R"(
#planTracker { background: %1; border: 1px solid %2; border-radius: 10px; }
#planTrackerHeader { border-radius: 9px; background: transparent; }
#planTrackerHeader:hover { background: %3; }
#planTrackerTitle { font-size: 12.5px; font-weight: 600; color: %4; }
#planTrackerCount { font-size: 11.5px; color: %5; }
#planTrackerStep { font-size: 12.5px; color: %6; }
#planTrackerStep[state="active"] { color: %4; font-weight: 500; }
#planTrackerStep[state="done"] { color: %5; }
#planTrackerStep[state="skipped"] { color: %7; }
#planTrackerNote { font-size: 11.5px; color: %5; }
QScrollArea#planTrackerScroll, #planTrackerList { background: transparent; border: none; }
QScrollBar:vertical { background: transparent; width: 6px; margin: 2px; }
QScrollBar::handle:vertical { background: %8; border-radius: 2px; min-height: 20px; }
QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page { background: none; height: 0; width: 0; }
)")
                      .arg(t.bgElevated.name(), t.accentBlueBorder.name(QColor::HexArgb), t.bgHover.name(QColor::HexArgb),
                           t.textHeading.name(), t.textMuted.name(), t.textPrimary.name(), t.textFaint.name(),
                           t.borderStrong.name(QColor::HexArgb)));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 4);
    layout->setSpacing(0);
    m_header = new QWidget(this);
    m_header->setObjectName("planTrackerHeader");
    m_header->setAttribute(Qt::WA_Hover);
    m_header->setCursor(Qt::PointingHandCursor);
    m_header->setToolTip(tr("Fold or unfold the plan"));
    auto* head = new QHBoxLayout(m_header);
    head->setContentsMargins(9, 6, 9, 5);
    head->setSpacing(7);
    auto* icon = new QLabel(m_header);
    icon->setPixmap(LC_AIIcons::lucide("list-checks", 13, BLUE, devicePixelRatioF()));
    m_title = new QLabel(m_header);
    m_title->setObjectName("planTrackerTitle");
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_count = new QLabel(m_header);
    m_count->setObjectName("planTrackerCount");
    m_chevron = new QLabel(m_header);
    head->addWidget(icon);
    head->addWidget(m_title, 1);
    head->addWidget(m_count);
    head->addWidget(m_chevron);
    layout->addWidget(m_header);

    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName("planTrackerScroll");
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* list = new QWidget();
    list->setObjectName("planTrackerList");
    m_rows = new QVBoxLayout(list);
    m_rows->setContentsMargins(9, 0, 9, 2);
    m_rows->setSpacing(3);
    m_scroll->setWidget(list);
    layout->addWidget(m_scroll);
    setFolded(false);
    hide();
}

void LC_AIPlanTracker::setPlan(const QString& title, const QList<LC_AIPlanStep>& steps) {
    int done = 0;
    for (const LC_AIPlanStep& s : steps) {
        done += finished(s) ? 1 : 0;
    }
    m_titleText = title.trimmed().isEmpty() ? tr("Plan") : title.trimmed();
    m_title->setToolTip(m_titleText);
    m_count->setText(tr("%1 of %2").arg(done).arg(steps.size()));

    while (QLayoutItem* item = m_rows->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const int current = currentStep(steps);
    QWidget* currentRow = nullptr;
    for (int i = 0; i < steps.size(); ++i) {
        const LC_AIPlanStep& s = steps[i];
        auto* row = new QWidget(m_scroll->widget());
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 1, 0, 1);
        h->setSpacing(8);
        auto* state = new LC_AIStatusIcon(m_theme, 13, row);
        state->setStatus(s.status);
        auto* texts = new QVBoxLayout();
        texts->setSpacing(0);
        auto* text = new QLabel(s.text, row);
        text->setObjectName("planTrackerStep");
        text->setWordWrap(true);
        text->setTextFormat(Qt::PlainText);
        text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        const char* stateName = s.status == LC_AIPlanStep::Status::Done      ? "done"
                                : s.status == LC_AIPlanStep::Status::Skipped ? "skipped"
                                : i == current                               ? "active"
                                                                             : "todo";
        text->setProperty("state", stateName);
        if (s.status == LC_AIPlanStep::Status::Done || s.status == LC_AIPlanStep::Status::Skipped) {
            QFont f = text->font();
            f.setStrikeOut(true); // ticked off, like a to-do list
            text->setFont(f);
        }
        texts->addWidget(text);
        if (!s.note.trimmed().isEmpty()) {
            auto* note = new QLabel(s.note.trimmed(), row);
            note->setObjectName("planTrackerNote");
            note->setWordWrap(true);
            note->setTextFormat(Qt::PlainText);
            note->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            texts->addWidget(note);
        }
        h->addWidget(state, 0, Qt::AlignTop);
        h->addLayout(texts, 1);
        // folded, only the step being worked on stays
        row->setVisible(!m_folded || i == current);
        m_rows->addWidget(row);
        if (i == current) {
            currentRow = row;
        }
    }
    m_scroll->setVisible(current >= 0 || !m_folded);
    fit();
    // again once laid out at the panel's width, then the step in progress is scrolled into view
    QTimer::singleShot(0, this, [this, row = QPointer<QWidget>(currentRow)] {
        fit();
        if (row != nullptr) {
            m_scroll->ensureWidgetVisible(row, 0, 4);
        }
    });
}

void LC_AIPlanTracker::fit() {
    m_title->setText(m_title->fontMetrics().elidedText(m_titleText, Qt::ElideRight, qMax(40, m_title->width())));
    // as tall as the steps, up to about six of them, then they scroll
    const int width = m_scroll->viewport()->width() > 40 ? m_scroll->viewport()->width() : this->width() - 2;
    const int wanted = m_rows->hasHeightForWidth() ? m_rows->totalHeightForWidth(width) : m_rows->totalSizeHint().height();
    m_scroll->setFixedHeight(qBound(0, wanted, TRACKER_MAX_HEIGHT));
}

void LC_AIPlanTracker::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    fit(); // the steps wrap differently
}

void LC_AIPlanTracker::setFolded(const bool folded) {
    m_folded = folded;
    m_chevron->setPixmap(LC_AIIcons::lucide(folded ? "chevron-right" : "chevron-down", 12, m_theme.textMuted, devicePixelRatioF()));
}

void LC_AIPlanTracker::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_header->geometry().contains(event->position().toPoint())) {
        setFolded(!m_folded);
        emit foldToggled(); // the plan mode lays the rows out again
    }
    QFrame::mouseReleaseEvent(event);
}

// ------------------------------------------------------------------------------------------------ plan mode

LC_AIPlanMode::LC_AIPlanMode(const LC_AITheme& theme, QWidget* parent)
    : QObject(parent), m_theme(theme) {
    m_toggle = new QPushButton(tr("Plan"), parent);
    m_toggle->setObjectName("planBtn");
    m_toggle->setAccessibleName(tr("Plan first"));
    m_toggle->setCheckable(true);
    m_toggle->setCursor(Qt::PointingHandCursor);
    m_toggle->setIconSize(QSize(14, 14));
    m_toggle->setStyleSheet(QStringLiteral("QPushButton#planBtn { border: none; border-radius: 6px; padding: 0 8px; min-height: 26px; "
                                           "max-height: 26px; font-size: 12px; font-weight: 500; color: %1; background: transparent; "
                                           "text-align: left; }"
                                           "QPushButton#planBtn:hover { background: %2; }"
                                           "QPushButton#planBtn:checked { color: %3; background: %4; }"
                                           "QPushButton#planBtn:checked:hover { background: %5; }")
                                .arg(theme.textMuted.name(), theme.bgHover.name(QColor::HexArgb), BLUE.name(), rgba(BLUE, 0.12),
                                     rgba(BLUE, 0.2)));
    m_enabled = LC_AICredentials::instance().setting(SETTING_KEY).toBool(false);
    m_toggle->setChecked(m_enabled);
    connect(m_toggle, &QPushButton::toggled, this, &LC_AIPlanMode::setEnabled);
    refreshToggle();

    m_tracker = new LC_AIPlanTracker(theme, parent);
    connect(m_tracker, &LC_AIPlanTracker::foldToggled, this, &LC_AIPlanMode::publish);
}

void LC_AIPlanMode::setEnabled(const bool enabled) {
    m_enabled = enabled;
    LC_AICredentials::instance().setSetting(SETTING_KEY, enabled);
    refreshToggle();
}

void LC_AIPlanMode::refreshToggle() {
    m_toggle->setIcon(LC_AIIcons::lucideIcon("list-checks", 14, m_enabled ? BLUE : m_theme.textMuted));
    m_toggle->setToolTip(m_enabled ? tr("Plan first is on: the AI proposes a plan for you to approve, edit or reject before "
                                        "it changes the drawing, and ticks the steps off as it works. Click to turn it off.")
                                   : tr("Plan first: have the AI propose a plan for you to approve before it changes the drawing"));
}

bool LC_AIPlanMode::isPlanTool(const QString& toolName) {
    return toolName == PROPOSE_PLAN || toolName == UPDATE_PLAN;
}

bool LC_AIPlanMode::changesDrawing(const QString& toolName) {
    if (isPlanTool(toolName) || toolName == "request_clarification") {
        return false;
    }
    if (toolName == "handle_dialog") {
        return true; // it fills in and confirms what a button opened
    }
    // what may run without asking only looks: the drawing, the view, the app or the web
    if (LC_AIPermissions::defaultPermission(toolName) == LC_AIPermissions::Permission::Allow) {
        return false;
    }
    // other tools (and MCP tools, mcp__server__tool) that only read, by their name
    static const QRegularExpression looks(QStringLiteral("^(get|list|look|check|capture|find|read|search|inspect|measure|describe|"
                                                         "show|view|query|fetch|count|validate|verify)(_|$)"));
    return !looks.match(toolName.section("__", -1)).hasMatch();
}

QList<LC_AITool> LC_AIPlanMode::tools() {
    if (!m_enabled && !m_proposedInChat) {
        return {};
    }
    LC_AITool propose;
    propose.name = PROPOSE_PLAN;
    propose.label = tr("Plan");
    propose.description = "Plan mode: propose your plan and wait for the user to approve, edit or reject it. Call it once, "
                          "after looking at what you need and before any tool that changes the drawing. Give a short title "
                          "and 2-12 concrete steps in order, each one line with the sizes, positions and layers it uses. The "
                          "result says whether the plan was approved (follow it), edited (follow the user's steps exactly) or "
                          "rejected (change nothing; ask what they want instead).";
    propose.parameters = QJsonObject{
        {"type", "object"},
        {"properties", QJsonObject{{"title", QJsonObject{{"type", "string"}, {"description", "A short name for the job"}}},
                                   {"steps", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}},
                                                         {"description", "The steps in order, one short line each"}}}}},
        {"required", QJsonArray{"title", "steps"}}};
    propose.executeAsync = [this](const QJsonObject& args, std::function<void(const LC_AIToolResult&)> done) {
        m_proposedInChat = true;
        const QStringList steps = stepsFrom(args.value("steps"));
        if (steps.isEmpty()) {
            done(LC_AIToolResult::error(tr("steps must be a non-empty array of strings, one step each.")));
            return;
        }
        LC_AIPlanCard* card = nullptr;
        for (const QPointer<LC_AIPlanCard>& c : std::as_const(m_cards)) {
            if (c != nullptr && !c->isActivated()) {
                card = c;
                break;
            }
        }
        if (card == nullptr) {
            done(LC_AIToolResult::error(tr("The plan could not be shown to the user. Call propose_plan again.")));
            return;
        }
        const QString title = args.value("title").toString().trimmed();
        QPointer<LC_AIPlanMode> self(this);
        QPointer<LC_AIPlanCard> cardRef(card);
        card->activate([self, title, cardRef, done](const LC_AIPlanCard::Answer answer, const QStringList& chosen) {
            if (self != nullptr) {
                done(self->propose(title, answer, chosen, cardRef));
            }
        });
    };

    LC_AITool update;
    update.name = UPDATE_PLAN;
    update.label = tr("Plan progress");
    update.description = "Plan mode: report progress on the approved plan, shown to the user as a checklist. Set a step to "
                         "\"in_progress\" when you start it, \"done\" when it is finished and checked, or \"skipped\" (with a "
                         "note saying why) when it is not needed.";
    update.parameters = QJsonObject{
        {"type", "object"},
        {"properties", QJsonObject{{"step", QJsonObject{{"type", "integer"}, {"description", "The step's number, 1 for the first"}}},
                                   {"status", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"in_progress", "done", "skipped"}}}},
                                   {"note", QJsonObject{{"type", "string"}, {"description", "Optional: a few words for the user"}}}}},
        {"required", QJsonArray{"step", "status"}}};
    update.execute = [this](const QJsonObject& args) { return this->update(args); };
    return {propose, update};
}

QString LC_AIPlanMode::promptAddition() const {
    if (!m_enabled) {
        return {};
    }
    return QStringLiteral(
        "PLAN MODE is on: plan first, then act.\n"
        "- Before any tool that changes the drawing (run_command, add_text, press_toolbar_button, click_drawing, tool_options, "
        "set_toolbar_value, handle_dialog, MCP tools that create or change things), look at what you need with the read tools "
        "(capture_view, get_drawing_info, list_entities, ...), then call propose_plan once with a short title and 2-12 concrete "
        "steps in order, each with the sizes, positions and layers it uses.\n"
        "- propose_plan waits for the user. Approved: carry the plan out. Edited: follow the user's steps exactly. Rejected: "
        "change nothing and ask in one or two sentences what they want instead.\n"
        "- Until a plan is approved, every tool that changes the drawing is refused.\n"
        "- While you work, call update_plan {step, status}: \"in_progress\" when you start a step, \"done\" when it is "
        "finished and checked, \"skipped\" with a note when it turns out unnecessary. Keep to the plan; if it has to change "
        "substantially, call propose_plan again.\n"
        "- Each new request needs its own plan. A question you can answer without changing the drawing needs no plan.");
}

QWidget* LC_AIPlanMode::cardFor(const LC_AIToolCall& call) {
    if (call.name != PROPOSE_PLAN) {
        return nullptr;
    }
    const QStringList steps = stepsFrom(call.arguments.value("steps"));
    if (steps.isEmpty()) {
        return nullptr; // a plain tool row shows the call, and its error
    }
    auto* card = new LC_AIPlanCard(call.arguments.value("title").toString(), steps, m_theme);
    m_cards << card;
    return card;
}

bool LC_AIPlanMode::gate(const LC_AIToolCall& call, const std::function<void(bool, const QString&)>& answer) {
    if (isPlanTool(call.name)) {
        answer(true, QString()); // the plan card is where the user approves
        return true;
    }
    if (!m_enabled || m_reviewing || m_approved || !changesDrawing(call.name)) {
        return false;
    }
    // for the model, so not translated; "Plan mode:" also tells the debugger this was no mistake of the run
    if (m_rejected) {
        answer(false, QStringLiteral("Plan mode: the user rejected your plan, so nothing may change the drawing now. Ask the "
                                     "user what they want instead, or call propose_plan with a different plan."));
    }
    else {
        answer(false, QStringLiteral("Plan mode: \"%1\" changes the drawing and is refused until the user approves a plan. "
                                     "Call propose_plan {title, steps} first and wait for the answer. Read tools (capture_view, "
                                     "list_entities, get_drawing_info, ...) work without a plan.").arg(call.name));
    }
    return true;
}

LC_AIToolResult LC_AIPlanMode::propose(const QString& title, const LC_AIPlanCard::Answer answer, const QStringList& steps,
                                       LC_AIPlanCard* card) {
    if (answer == LC_AIPlanCard::Answer::Rejected) {
        m_approved = false;
        m_rejected = true;
        publish();
        return LC_AIToolResult::text(QStringLiteral("The user rejected the plan. Do not change the drawing. In one or two "
                                                    "sentences, ask what they would like instead."));
    }
    m_title = title;
    m_steps.clear();
    for (const QString& s : steps) {
        m_steps << LC_AIPlanStep{s, LC_AIPlanStep::Status::Pending, {}};
    }
    m_approved = true;
    m_rejected = false;
    m_planCard = card;
    publish();
    const QString intro = answer == LC_AIPlanCard::Answer::Edited
                              ? QStringLiteral("The user edited the plan and approved it. Follow these steps exactly, in order:\n")
                              : QStringLiteral("The user approved the plan. Carry it out, in order:\n");
    return LC_AIToolResult::text(intro + numbered(steps)
                                 + QStringLiteral("\nCall update_plan with \"in_progress\" when you start a step and \"done\" "
                                                  "when it is finished and checked (\"skipped\" with a note if it is not needed)."));
}

LC_AIToolResult LC_AIPlanMode::update(const QJsonObject& args) {
    if (m_steps.isEmpty() || (!m_approved && !m_reviewing)) {
        return LC_AIToolResult::error(tr("There is no approved plan. Call propose_plan first and wait for the user's approval."));
    }
    const int step = args.value("step").toVariant().toInt();
    if (step < 1 || step > m_steps.size()) {
        return LC_AIToolResult::error(tr("step must be a number from 1 to %1.").arg(m_steps.size()));
    }
    const QString status = args.value("status").toString().trimmed().toLower().replace(' ', '_').replace('-', '_');
    LC_AIPlanStep& s = m_steps[step - 1];
    if (status == "in_progress" || status == "started" || status == "doing" || status == "active") {
        s.status = LC_AIPlanStep::Status::InProgress;
    }
    else if (status == "done" || status == "completed" || status == "complete" || status == "finished") {
        s.status = LC_AIPlanStep::Status::Done;
    }
    else if (status == "skipped" || status == "skip") {
        s.status = LC_AIPlanStep::Status::Skipped;
    }
    else if (status == "pending" || status == "todo") {
        s.status = LC_AIPlanStep::Status::Pending;
    }
    else {
        return LC_AIToolResult::error(tr("status must be \"in_progress\", \"done\" or \"skipped\"."));
    }
    if (args.contains("note")) {
        s.note = args.value("note").toString().trimmed();
    }
    publish();

    const QString what = s.status == LC_AIPlanStep::Status::InProgress ? QStringLiteral("in progress")
                         : s.status == LC_AIPlanStep::Status::Done      ? QStringLiteral("done")
                         : s.status == LC_AIPlanStep::Status::Skipped   ? QStringLiteral("skipped")
                                                                        : QStringLiteral("to do");
    QString text = QStringLiteral("Step %1 of %2 is %3.").arg(step).arg(m_steps.size()).arg(what);
    int next = -1;
    for (int i = 0; i < m_steps.size(); ++i) {
        if (!finished(m_steps[i]) && i != step - 1) {
            next = i;
            break;
        }
    }
    if (s.status != LC_AIPlanStep::Status::InProgress) {
        text += next >= 0 ? QStringLiteral(" Next: step %1, \"%2\".").arg(next + 1).arg(m_steps[next].text)
                          : QStringLiteral(" All steps are finished: check the result, then answer the user.");
    }
    return LC_AIToolResult::text(text);
}

void LC_AIPlanMode::publish() {
    if (m_planCard != nullptr) {
        m_planCard->setProgress(m_steps);
    }
    const bool show = m_running && m_approved && !m_steps.isEmpty();
    if (show) {
        m_tracker->setVisible(true); // laid out at the panel's width before the rows are measured
        m_tracker->setPlan(m_title, m_steps);
    }
    else {
        m_tracker->hide();
    }
}

void LC_AIPlanMode::newRequest() {
    m_approved = false;
    m_rejected = false;
    m_running = true;
    m_planCard = nullptr; // an old plan's card keeps the ticks it had
    m_cards.clear();
    publish();
}

void LC_AIPlanMode::runEnded(const bool) {
    m_running = false;
    for (const QPointer<LC_AIPlanCard>& card : std::as_const(m_cards)) {
        if (card != nullptr && !card->isAnswered()) {
            card->setStopped(); // a plan nobody answered
        }
    }
    m_cards.clear();
    publish();
}

void LC_AIPlanMode::reset(const QList<LC_AIChatMessage>& chat) {
    m_approved = false;
    m_rejected = false;
    m_running = false;
    m_reviewing = false;
    m_proposedInChat = false;
    m_title.clear();
    m_steps.clear();
    m_planCard = nullptr;
    m_cards.clear();
    m_tracker->hide();
    for (const LC_AIChatMessage& m : chat) { // a model that repeats a call of its history must find the tool
        for (const LC_AIToolCall& call : m.toolCalls) {
            m_proposedInChat = m_proposedInChat || isPlanTool(call.name);
        }
    }
}
