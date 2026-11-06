/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIDEBUGGER_H
#define LC_AIDEBUGGER_H

#include <QFrame>
#include <QPointer>

#include "lc_aiinferencemanager.h"
#include "lc_aiprovidersdialog.h"

class QLabel;
class LC_AIChatTranscript;
class LC_AIStatusIcon;

/**
 * The Debugger's part of the conversation: a framed section headed "Debugger" (why it runs, and whether it is
 * still checking) that holds everything the debugger pass writes and runs, its own block of tool calls included.
 */
class LC_AIDebuggerSection : public QFrame {
    Q_OBJECT
public:
    LC_AIDebuggerSection(const QString& reason, const LC_AITheme& theme, QWidget* parent = nullptr);
    /** Where the transcript puts the pass's text and tool calls. */
    QWidget* body() const { return m_body; }
    void setDone();
    void setStopped();
    void setFailed();

private:
    void setStatus(const QString& text, const QColor& color);

    LC_AITheme m_theme;
    LC_AIStatusIcon* m_statusIcon {nullptr};
    QLabel* m_status {nullptr};
    QWidget* m_body {nullptr};
};

/**
 * The debugger agent: a second pass after a run, with a reviewer's instructions. Given the user's request and the
 * run's tool calls and results, it inspects the drawing with the read tools, lists concrete problems (wrong sizes,
 * missing parts, overlaps, failed steps), fixes them with the normal tools and ends with a short verdict.
 * It runs by itself when a tool call of the run failed, or after every run with "Check my work automatically";
 * otherwise "Check & fix" under the AI's final answer starts it.
 */
class LC_AIDebugger : public QObject {
    Q_OBJECT
public:
    enum class End { Finished, Stopped, Failed };

    LC_AIDebugger(const LC_AITheme& theme, QObject* parent = nullptr);

    /** "Check my work automatically": the debugger runs after every run that changed the drawing. Off by default. */
    static bool autoCheck();
    static void setAutoCheck(bool on);

    /**
     * What a run did to the drawing, from the request at `start` on: its calls that change the drawing and ran (not
     * reads, web lookups, questions, refused or stopped calls), and those of them that failed on their own.
     */
    struct Report {
        int toolCalls {0};
        int failures {0};
    };
    static Report inspect(const QList<LC_AIChatMessage>& history, int start);
    /** The reviewer's instructions for the system prompt, naming the read tools among `tools`. */
    static QString reviewerPrompt(const QList<LC_AITool>& tools);
    /** The message that starts the pass: the user's request and a digest of what the run did. */
    static QString reviewRequest(const QList<LC_AIChatMessage>& history, int start, const QString& reason);

    /** A new request: the Check & fix of the previous answer goes. */
    void clearOffer();
    /** The pass starts: its section opens in the transcript and collects what the AI writes and runs. */
    void beginPass(LC_AIChatTranscript* transcript, const QString& reason);
    /** A run ended: closes the Debugger's section, or checks the run automatically, or offers Check & fix. */
    void runEnded(LC_AIChatTranscript* transcript, const QList<LC_AIChatMessage>& history, int start, bool wasPass, End end);

signals:
    /** Run the debugger now; `reason` says why, for the section's header. */
    void checkRequested(const QString& reason);

private:
    void offer(LC_AIChatTranscript* transcript, bool again);

    LC_AITheme m_theme;
    QPointer<QWidget> m_offer;                // Check & fix under the last answer
    QPointer<LC_AIDebuggerSection> m_section; // the pass running now
};

#endif
