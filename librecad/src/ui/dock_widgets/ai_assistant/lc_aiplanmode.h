/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIPLANMODE_H
#define LC_AIPLANMODE_H

#include <QFrame>
#include <QList>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <functional>

#include "lc_aiinferencemanager.h"
#include "lc_aiprovidersdialog.h"

class QLabel;
class QPlainTextEdit;
class QScrollArea;
class QTimer;
class QVBoxLayout;

/** One step of a plan and how far the AI has got with it (update_plan). */
struct LC_AIPlanStep {
    enum class Status { Pending, InProgress, Done, Skipped };
    QString text;
    Status status {Status::Pending};
    QString note;
};

/**
 * A step's status as a small icon: an empty circle (to do), a turning loader (in progress), a green check
 * (done) or a slash (skipped). Any other lucide icon can be shown too, and the loader can turn on its own.
 */
class LC_AIStatusIcon : public QWidget {
    Q_OBJECT
public:
    explicit LC_AIStatusIcon(const LC_AITheme& theme, int size = 13, QWidget* parent = nullptr);
    void setStatus(LC_AIPlanStep::Status status);
    /** Pending steps show nothing (the plan card numbers its steps instead). */
    void setShowPending(bool show) { m_showPending = show; }
    void setIcon(const QString& name, const QColor& color);
    void setSpinning(const QColor& color);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    LC_AITheme m_theme;
    int m_size;
    QPixmap m_pixmap;
    QTimer* m_timer {nullptr};
    int m_angle {0};
    bool m_spinning {false};
    bool m_showPending {true};
};

/**
 * The plan the AI proposes in plan mode, as a card in the conversation: its title and numbered steps with
 * Approve, Edit (the steps as text, one per line) and Reject. Once answered it keeps the steps and shows how
 * far the AI got with each, so the conversation still reads as a checklist after the run.
 */
class LC_AIPlanCard : public QFrame {
    Q_OBJECT
public:
    enum class Answer { Approved, Edited, Rejected };
    using Deliver = std::function<void(Answer answer, const QStringList& steps)>;

    LC_AIPlanCard(const QString& title, const QStringList& steps, const LC_AITheme& theme, QWidget* parent = nullptr);
    /** propose_plan runs: the user's answer goes to `deliver` (at once when it was already given). */
    void activate(Deliver deliver);
    bool isActivated() const { return m_activated; }
    bool isAnswered() const { return m_answered; }
    /** The run stopped before the user answered: the card locks. */
    void setStopped();
    /** Shows the status of every step (the plan is being worked on). */
    void setProgress(const QList<LC_AIPlanStep>& steps);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void fitEditor();
    void answer(Answer answer, const QStringList& steps);
    void showSteps(const QStringList& steps);
    void setEditing(bool editing);
    void setStatus(const QString& icon, const QString& text, const QColor& color);

    LC_AITheme m_theme;
    QStringList m_steps;
    QWidget* m_stepList {nullptr};
    QList<LC_AIStatusIcon*> m_stepIcons; // right of each step: in progress, done or skipped
    QList<QLabel*> m_stepLabels;
    QPlainTextEdit* m_editor {nullptr};
    QLabel* m_editHint {nullptr};
    QWidget* m_actions {nullptr};     // Approve, Edit, Reject
    QWidget* m_editActions {nullptr}; // Approve edited plan, Cancel
    QWidget* m_statusRow {nullptr};
    QLabel* m_statusIcon {nullptr};
    QLabel* m_statusText {nullptr};
    bool m_activated {false};
    bool m_answered {false};
    bool m_stopped {false};
    Answer m_answer {Answer::Approved};
    Deliver m_deliver;
};

/**
 * The approved plan pinned above the conversation while the AI works through it: a compact checklist
 * with a spinner on the step in progress, like a to-do list. Clicking the header folds it to one line.
 */
class LC_AIPlanTracker : public QFrame {
    Q_OBJECT
public:
    explicit LC_AIPlanTracker(const LC_AITheme& theme, QWidget* parent = nullptr);
    void setPlan(const QString& title, const QList<LC_AIPlanStep>& steps);

signals:
    void foldToggled();

protected:
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void setFolded(bool folded);
    void fit();

    LC_AITheme m_theme;
    QWidget* m_header {nullptr};
    QLabel* m_title {nullptr};
    QString m_titleText;
    QLabel* m_count {nullptr};
    QLabel* m_chevron {nullptr};
    QScrollArea* m_scroll {nullptr};
    QVBoxLayout* m_rows {nullptr};
    bool m_folded {false};
};

/**
 * Plan mode ("Plan first" in the composer): the AI has to propose a plan (propose_plan) and get it approved
 * before any tool that changes the drawing runs; then it reports its progress with update_plan. Owns the
 * toggle, the plan tools, the approval gate, the plan cards and the pinned tracker.
 */
class LC_AIPlanMode : public QObject {
    Q_OBJECT
public:
    LC_AIPlanMode(const LC_AITheme& theme, QWidget* parent);

    QPushButton* toggle() const { return m_toggle; }
    LC_AIPlanTracker* tracker() const { return m_tracker; }
    bool isEnabled() const { return m_enabled; }

    /** propose_plan and update_plan, while plan mode is on or the conversation has a plan. */
    QList<LC_AITool> tools();
    /** Instructions for the system prompt; empty when plan mode is off. */
    QString promptAddition() const;
    /** The card that shows a propose_plan call in the conversation, or nullptr for any other call. */
    QWidget* cardFor(const LC_AIToolCall& call);
    /**
     * Before a tool call: the plan tools run at once (the plan card is their approval), and while no plan is
     * approved, tools that change the drawing are refused with a message for the model. Returns true when it
     * answered the call; false leaves it to the normal approval.
     */
    bool gate(const LC_AIToolCall& call, const std::function<void(bool, const QString&)>& answer);
    /** The debugger's pass checks and fixes the work outside the plan: no gate meanwhile. */
    void setReviewing(bool reviewing) { m_reviewing = reviewing; }

    /** A new request from the user: it needs a plan of its own. */
    void newRequest();
    /** The run ended: the tracker goes, a plan left unanswered locks. */
    void runEnded(bool stopped);
    /** New chat. */
    void reset();

    /** Tools that only look (at the drawing, the app or the web): they run without a plan. */
    static bool changesDrawing(const QString& toolName);
    static bool isPlanTool(const QString& toolName);

private:
    void setEnabled(bool enabled);
    void refreshToggle();
    void publish(); // the current plan's progress to its card and the tracker
    LC_AIToolResult propose(const QString& title, LC_AIPlanCard::Answer answer, const QStringList& steps, LC_AIPlanCard* card);
    LC_AIToolResult update(const QJsonObject& args);

    LC_AITheme m_theme;
    QPushButton* m_toggle {nullptr};
    LC_AIPlanTracker* m_tracker {nullptr};
    bool m_enabled {false};
    bool m_reviewing {false};
    bool m_approved {false};          // the current request has an approved plan
    bool m_rejected {false};          // ...or the user turned its plan down
    bool m_running {false};
    bool m_proposedInChat {false};    // the conversation has plan tool calls: keep the tools it refers to
    QString m_title;
    QList<LC_AIPlanStep> m_steps;
    QPointer<LC_AIPlanCard> m_planCard;          // the card of the current plan
    QList<QPointer<LC_AIPlanCard>> m_cards;      // proposed this run, waiting for their tool call
};

#endif
