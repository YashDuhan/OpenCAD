/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AICHATTRANSCRIPT_H
#define LC_AICHATTRANSCRIPT_H

#include <QElapsedTimer>
#include <QFrame>
#include <QHash>
#include <QScrollArea>
#include <functional>

class QTimer;

#include "lc_aiinferencemanager.h"
#include "lc_aiprovidersdialog.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

/**
 * One tool call, as Cypher shows it (apps/desktop/src/components/ToolCall.tsx): a one-line row with
 * chevron, wrench, tool name, a summary of its most telling argument and its status (Running, done
 * with the time it took, Error); clicking the row opens Arguments and Result. Images of the result
 * show as thumbnails that open full size.
 */
class LC_AIToolCallWidget : public QFrame {
    Q_OBJECT
public:
    LC_AIToolCallWidget(const LC_AIToolCall& call, const QString& label, const LC_AITheme& theme, QWidget* parent = nullptr);
    void setRunning();
    void setAwaitingApproval();
    void setResult(const LC_AIToolResult& result);
    void setStopped();
    bool isDone() const { return m_done; }

protected:
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void setStatus(const QString& icon, const QString& text, const QColor& color);
    void setOpen(bool open);
    void buildBody();

    LC_AIToolCall m_call;
    LC_AITheme m_theme;
    QWidget* m_row {nullptr};
    QLabel* m_chevron {nullptr};
    QLabel* m_statusIcon {nullptr};
    QLabel* m_statusText {nullptr};
    QWidget* m_images {nullptr};
    QWidget* m_body {nullptr};
    LC_AIToolResult m_result;
    QElapsedTimer m_timer;
    bool m_open {false};
    bool m_done {false};
};

/**
 * The AI asking you something (ZeroDrive's requestClarification card): the question, numbered choices
 * that answer with one click, and a box for any other answer. It locks once answered.
 */
class LC_AIClarificationCard : public QFrame {
    Q_OBJECT
public:
    LC_AIClarificationCard(const QString& question, const QStringList& choices, const LC_AITheme& theme, QWidget* parent = nullptr);
    /** Called when the tool runs: the answer goes to `deliver` (at once if it was already given). */
    void activate(std::function<void(const QString&)> deliver);
    bool isActivated() const { return m_activated; }
    bool isAnswered() const { return !m_answer.isEmpty(); }
    void setStopped();
    void focusInput();

private:
    void choose(const QString& answer);

    LC_AITheme m_theme;
    void collapse(const QString& summary, bool answered);
    void setBodyOpen(bool open);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QWidget* m_summary {nullptr};      // after answering: one row like a finished tool call
    QLabel* m_summaryChevron {nullptr};
    QLabel* m_summaryAnswer {nullptr};
    QLabel* m_summaryStatus {nullptr};
    QLabel* m_summaryStatusIcon {nullptr};
    QWidget* m_body {nullptr};         // question, choices and answer box
    bool m_bodyOpen {true};
    QList<QWidget*> m_choiceRows;
    QLineEdit* m_input {nullptr};
    QPushButton* m_continue {nullptr};
    QLabel* m_status {nullptr};
    QString m_answer;
    bool m_activated {false};
    std::function<void(const QString&)> m_deliver;
};

/** The conversation of the AI sidebar: your messages, the AI's text as it streams, and its tool calls. */
class LC_AIChatTranscript : public QScrollArea {
    Q_OBJECT
public:
    explicit LC_AIChatTranscript(const LC_AITheme& theme, QWidget* parent = nullptr);

    void setLabelLookup(std::function<QString(const QString&)> lookup) { m_labelFor = std::move(lookup); }
    void addUser(const QString& text);
    void appendDelta(const QString& text);
    void addAssistant(const LC_AIChatMessage& message);
    void toolStarted(const LC_AIToolCall& call);
    void toolAwaitingApproval(const QString& callId);
    /** request_clarification is running: hand the answer of its card to `deliver`. */
    void activateClarification(std::function<void(const QString&)> deliver);
    void toolResult(const LC_AIChatMessage& message);
    void addError(const QString& error);
    void setThinking(bool thinking);
    /** The thinking line, saying a failed request is being tried again. */
    void showRetry(const QString& text);
    void finishRun();
    void clear();

private:
    void add(QWidget* widget);
    QLabel* textBlock(const QString& markdown);
    void stickToBottom();
    /** Shows the streamed text: at most every RENDER_MS, and only its unfinished tail is laid out again. */
    void renderStream();
    void updateBubbleWidths();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:

    LC_AITheme m_theme;
    QWidget* m_content {nullptr};
    QVBoxLayout* m_layout {nullptr};
    QWidget* m_thinking {nullptr};      // Cypher's "Thinking" line: brain icon, dim text, moving dots
    QLabel* m_thinkingText {nullptr};
    QTimer* m_thinkingTimer {nullptr};
    int m_dots {0};
    QList<QLabel*> m_bubbles;          // your messages, at most 80% of the width
    QLabel* m_streaming {nullptr};     // the unfinished tail of the AI text being streamed
    QList<QLabel*> m_streamLabels;     // all the blocks of the streamed text, finished paragraphs first
    QString m_streamText;
    int m_streamFrozen {0};            // characters of m_streamText already in finished blocks
    QTimer* m_renderTimer {nullptr};
    bool m_follow {true};              // keep the newest output in view (off once you scroll up)
    QHash<QString, LC_AIToolCallWidget*> m_tools; // by tool call id, for the current run
    QList<LC_AIClarificationCard*> m_cards;       // questions of the current run, in order
    std::function<QString(const QString&)> m_labelFor;
};

#endif
