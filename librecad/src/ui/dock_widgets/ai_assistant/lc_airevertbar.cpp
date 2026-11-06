/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_airevertbar.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include "lc_aichangepreview.h"
#include "lc_aichattranscript.h"
#include "lc_aicheckpoints.h"
#include "lc_aiicons.h"
#include "rs_graphicview.h"

namespace {
    const QColor BLUE("#4c8dff");   // --accent-blue, as the approval box's primary button
    const QColor GREEN("#22c55e");  // --accent-green-icon, as a finished tool call
    const QColor YELLOW("#fbbf24"); // the approval box's attention colour

    QString rgba(const QColor& c, const double alpha) {
        return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha);
    }

    QString changesText(const int count) {
        return count == 1 ? QObject::tr("1 change") : QObject::tr("%1 changes").arg(count);
    }

    /** The main window a widget belongs to, also when it sits in a floating dock. */
    QWidget* mainWindowOf(QWidget* widget) {
        for (QWidget* w = widget; w != nullptr; w = w->parentWidget()) {
            if (qobject_cast<QMainWindow*>(w) != nullptr) {
                return w;
            }
        }
        return widget != nullptr ? widget->window() : nullptr;
    }

    /** A small button of the card, coloured like the approval box's (primary: tinted fill). */
    QPushButton* cardButton(const QString& text, const QString& icon, const QColor& color, const bool primary, QWidget* parent) {
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
}

LC_AIRevertBar::LC_AIRevertBar(const int run, const LC_AITheme& theme, QWidget* parent)
    : QWidget(parent), m_run(run), m_theme(theme) {
    setObjectName("aiRevertBar");
    const LC_AITheme& t = theme;
    setStyleSheet(QStringLiteral(R"(
#aiRevertButton { border: 1px solid %1; border-radius: 7px; background: transparent; padding: 0; min-height: 24px; max-height: 24px; }
#aiRevertButton:hover { background: %2; border-color: %3; }
#aiRevertButton[reverted="true"] { border-color: transparent; }
#aiRevertButton QLabel { background: transparent; font-size: 12px; }
#aiRevertText { color: %4; }
#aiRevertCount { color: %5; }
#aiRevertRestore { border: none; border-radius: 7px; background: transparent; color: %6; font-size: 12px; padding: 0 8px;
                   min-height: 24px; max-height: 24px; }
#aiRevertRestore:hover { background: %2; color: %4; }
#aiRevertCard { background: %7; border: 1px solid %8; border-radius: 10px; }
#aiRevertCardText { color: %4; font-size: 12px; background: transparent; }
#aiRevertNote { color: %6; font-size: 11.5px; }
)")
                      .arg(t.borderDefault.name(QColor::HexArgb), t.bgHover.name(QColor::HexArgb), t.borderHover.name(QColor::HexArgb),
                           t.textSecondary.name(), t.textFaint.name(), t.textMuted.name(), t.bgElevated.name(), rgba(YELLOW, 0.3)));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 6);
    layout->setSpacing(6);

    // the button, like the model select of the composer: a push button holding its icon and two tones of text
    auto* row = new QHBoxLayout();
    row->setSpacing(4);
    m_button = new QPushButton(this);
    m_button->setObjectName("aiRevertButton");
    m_button->setCursor(Qt::PointingHandCursor);
    m_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto* inside = new QHBoxLayout(m_button);
    inside->setContentsMargins(8, 0, 9, 0);
    inside->setSpacing(6);
    m_icon = new QLabel(m_button);
    m_text = new QLabel(m_button);
    m_text->setObjectName("aiRevertText");
    m_count = new QLabel(changesText(LC_AICheckpoints::instance().changesOf(run).count()), m_button);
    m_count->setObjectName("aiRevertCount");
    for (QLabel* l : {m_icon, m_text, m_count}) {
        l->setAttribute(Qt::WA_TransparentForMouseEvents);
        inside->addWidget(l);
    }
    row->addWidget(m_button);
    m_restore = new QPushButton(tr("Restore"), this);
    m_restore->setObjectName("aiRevertRestore");
    m_restore->setIcon(LC_AIIcons::lucideIcon("redo-2", 12, t.textMuted));
    m_restore->setIconSize(QSize(12, 12));
    m_restore->setCursor(Qt::PointingHandCursor);
    m_restore->setToolTip(tr("Bring these changes back"));
    m_restore->hide();
    row->addWidget(m_restore);
    m_review = new QPushButton(tr("Review"), this);
    m_review->setObjectName("aiRevertRestore");
    m_review->setIcon(LC_AIIcons::lucideIcon("eye", 12, t.textMuted));
    m_review->setIconSize(QSize(12, 12));
    m_review->setCursor(Qt::PointingHandCursor);
    m_review->setToolTip(tr("Show these changes in the drawing, then keep or reject them"));
    row->addWidget(m_review);
    row->addStretch(1);
    layout->addLayout(row);

    m_card = new QFrame(this);
    m_card->setObjectName("aiRevertCard");
    auto* card = new QVBoxLayout(m_card);
    card->setContentsMargins(12, 10, 12, 10);
    card->setSpacing(8);
    auto* message = new QHBoxLayout();
    message->setSpacing(8);
    auto* alert = new QLabel(m_card);
    alert->setPixmap(LC_AIIcons::lucide("circle-alert", 14, YELLOW, devicePixelRatioF()));
    alert->setAlignment(Qt::AlignTop);
    alert->setStyleSheet("background: transparent; padding-top: 1px;");
    m_cardText = new QLabel(m_card);
    m_cardText->setObjectName("aiRevertCardText");
    m_cardText->setWordWrap(true);
    m_cardText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_cardText->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    message->addWidget(alert, 0, Qt::AlignTop);
    message->addWidget(m_cardText, 1);
    card->addLayout(message);
    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(6);
    buttons->addStretch(1);
    m_own = cardButton(tr("Revert only these changes"), "undo-2", BLUE, true, m_card);
    m_own->setObjectName("aiRevertOwn");
    m_cancel = cardButton(tr("Cancel"), QString(), t.textMuted, false, m_card);
    m_cancel->setObjectName("aiRevertCancel");
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_own);
    card->addLayout(buttons);
    m_card->hide();
    layout->addWidget(m_card);

    m_note = new QLabel(this);
    m_note->setObjectName("aiRevertNote");
    m_note->setWordWrap(true);
    m_note->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_note->setContentsMargins(2, 0, 2, 0);
    m_note->hide();
    layout->addWidget(m_note);

    connect(m_button, &QPushButton::clicked, this, &LC_AIRevertBar::revert);
    connect(m_restore, &QPushButton::clicked, this, &LC_AIRevertBar::restore);
    connect(m_review, &QPushButton::clicked, this, [this] {
        if (!LC_AIChangePreview::instance().show(m_run)) {
            showNote(tr("Nothing of these changes is left to show."));
        }
    });
    connect(m_own, &QPushButton::clicked, this, &LC_AIRevertBar::revertOwnChanges);
    connect(m_cancel, &QPushButton::clicked, m_card, &QWidget::hide);
    // while the AI works on the drawing, nothing is reverted under its feet
    connect(&LC_AICheckpoints::instance(), &LC_AICheckpoints::runStarted, this, &LC_AIRevertBar::updateEnabled);
    connect(&LC_AICheckpoints::instance(), &LC_AICheckpoints::runRecorded, this, &LC_AIRevertBar::updateEnabled);
    // reverted or restored elsewhere too (the change preview's Reject): the row shows how the run stands
    connect(&LC_AICheckpoints::instance(), &LC_AICheckpoints::runReverted, this, [this](const int run, const bool reverted) {
        if (run != m_run || reverted == m_reverted) {
            return;
        }
        if (reverted) {
            showReverted(tr("Taken back."));
        }
        else {
            showReady();
        }
    });
    // a bar made again for a chat shown from the history shows how the run stands now
    if (LC_AICheckpoints::instance().isRunReverted(run)) {
        showReverted(QString());
    }
    else {
        showReady();
    }
}

void LC_AIRevertBar::addAfterRuns(LC_AIChatTranscript* transcript, const LC_AITheme& theme) {
    connect(&LC_AICheckpoints::instance(), &LC_AICheckpoints::runRecorded, transcript, [transcript, theme](const int run) {
        addForRun(transcript, run, theme);
    });
}

bool LC_AIRevertBar::addForRun(LC_AIChatTranscript* transcript, const int run, const LC_AITheme& theme) {
    const LC_AIRunChanges changes = LC_AICheckpoints::instance().changesOf(run);
    RS_GraphicView* view = LC_AICheckpoints::instance().viewOf(run);
    // nothing changed, or the drawing belongs to another window and its chat
    if (run == 0 || changes.isEmpty() || view == nullptr || mainWindowOf(view) != mainWindowOf(transcript)) {
        return false;
    }
    transcript->addRunFooter(new LC_AIRevertBar(run, theme));
    return true;
}

void LC_AIRevertBar::fitButton() {
    // reverted, it reads as a status line rather than a button
    if (m_button->property("reverted").toBool() != m_reverted) {
        m_button->setProperty("reverted", m_reverted);
        m_button->style()->unpolish(m_button);
        m_button->style()->polish(m_button);
    }
    // a push button sizes itself by its own text, not by the labels laid out inside it
    m_button->setFixedSize(m_button->layout()->sizeHint().width(), 24);
}

void LC_AIRevertBar::updateEnabled() {
    const bool working = LC_AICheckpoints::instance().currentRun() != 0;
    m_button->setEnabled(!working && !m_reverted);
    m_restore->setEnabled(!working);
    m_review->setEnabled(!working);
    m_own->setEnabled(!working);
    m_button->setToolTip(working ? tr("Available when the AI has finished")
                                 : m_reverted ? QString()
                                              : tr("Undo what the AI changed in the drawing in this answer: %1")
                                                    .arg(LC_AICheckpoints::instance().changesOf(m_run).summary()));
}

void LC_AIRevertBar::showReady() {
    m_reverted = false;
    m_icon->setPixmap(LC_AIIcons::lucide("undo-2", 13, m_theme.textSecondary, devicePixelRatioF()));
    m_text->setText(tr("Revert AI changes"));
    m_restore->hide();
    m_review->show();
    m_note->hide();
    fitButton();
    updateEnabled();
}

void LC_AIRevertBar::showReverted(const QString& note) {
    m_reverted = true;
    m_card->hide();
    m_icon->setPixmap(LC_AIIcons::lucide("check", 13, GREEN, devicePixelRatioF()));
    m_text->setText(tr("Reverted"));
    m_restore->show();
    m_review->hide();
    fitButton();
    updateEnabled();
    showNote(note);
}

void LC_AIRevertBar::showCard(const QString& text, const bool offerOwn) {
    m_cardText->setText(text);
    m_own->setVisible(offerOwn);
    m_cancel->setText(offerOwn ? tr("Cancel") : tr("OK"));
    m_note->hide();
    m_card->show();
}

void LC_AIRevertBar::showNote(const QString& text) {
    m_note->setText(text);
    m_note->setVisible(!text.isEmpty());
}

void LC_AIRevertBar::revert() {
    LC_AICheckpoints& checkpoints = LC_AICheckpoints::instance();
    const LC_AICheckpoints::Check check = checkpoints.check(m_run);
    QString message;
    switch (check.situation) {
        case LC_AICheckpoints::Situation::Unchanged:
            if (checkpoints.revert(m_run, message)) {
                showReverted(message);
            }
            else {
                // the undo steps did not lead back to the checkpoint; its own changes may still be taken back
                const LC_AICheckpoints::Check again = checkpoints.check(m_run);
                showCard(again.situation == LC_AICheckpoints::Situation::ChangedSince ? again.text : message,
                         again.situation == LC_AICheckpoints::Situation::ChangedSince);
            }
            break;
        case LC_AICheckpoints::Situation::ChangedSince:
            showCard(check.text, true);
            break;
        default:
            showCard(check.text, false);
            break;
    }
}

void LC_AIRevertBar::revertOwnChanges() {
    QString message;
    if (LC_AICheckpoints::instance().revertOwnChanges(m_run, message)) {
        showReverted(message);
    }
    else {
        showCard(message, false);
    }
}

void LC_AIRevertBar::restore() {
    QString message;
    if (LC_AICheckpoints::instance().restore(m_run, message)) {
        showReady();
    }
    else {
        m_restore->hide();
        showNote(message);
    }
}
