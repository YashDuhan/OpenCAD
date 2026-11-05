/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIASSISTANTWIDGET_H
#define LC_AIASSISTANTWIDGET_H

#include <QHash>
#include <QSet>
#include <functional>
#include <QWidget>

#include "lc_aiinferencemanager.h"
#include "lc_aiprovidersdialog.h"

class QFrame;
class QLabel;
class LC_AIChatTranscript;
class LC_AIApprovalBox;
class LC_AIApprovalModeButton;
class LC_AIChatSession;
class LC_AIVoiceInput;
class LC_AIComposerExtras;
class LC_AIDebugger;
class LC_AIPlanMode;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

/**
 * Chat panel of the AI sidebar (similar to the Cursor chat panel): sends the conversation
 * to the selected provider through LC_AIInferenceManager and streams the reply in.
 */
class LC_AIAssistantWidget : public QWidget {
    Q_OBJECT
public:
    explicit LC_AIAssistantWidget(QWidget* parent = nullptr);
    void focusInput();
    /** Puts a task in the message box (sent at once when send is true), ready for the user to finish. */
    void startTask(const QString& prompt, bool send = false);
    /** The tools the AI can use now: OpenCAD's own and those of connected MCP servers. */
    QList<LC_AITool> availableTools() const;
    /** Opens the AI sidebar of this window and starts a task in it. */
    static void openWithTask(QWidget* window, const QString& prompt, bool send = false);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void onSend();
    void onStop();
    void onNewChat();
    void openSettings();
    void openMcp();
    void openToolPermissions();
    void approve(const LC_AIToolCall& call, const std::function<void(bool, const QString&)>& answer);
    void openModelPicker();
    void updateSendButton();
    void fitModelButton();
    /** The message box grows with its text, up to almost half the panel, then scrolls. */
    void fitInputHeight();
    void onDelta(const QString& text);
    void onMessageAdded(const LC_AIChatMessage& message);
    void onToolStarted(const LC_AIToolCall& call);
    void onFinished();
    void onFailed(const QString& error);
    QString toolLabel(const QString& name) const;
    void setBusy(bool busy);
    void updateModelButton();

    QList<LC_AIChatMessage> m_messages;
    QList<LC_AITool> m_tools;      // built-in tools
    QList<LC_AITool> m_runTools;   // tools of the current run: built-in + MCP
    bool m_busy {false};
    LC_AIInferenceManager* m_inference {nullptr};
    LC_AITheme m_theme;
    LC_AIChatTranscript* m_transcript {nullptr};
    QFrame* m_composer {nullptr};       // Cypher's .composer: the input over the actions row
    QPlainTextEdit* m_input {nullptr};
    LC_AIComposerExtras* m_extras {nullptr}; // attached files and @-mentions of the message box
    QPushButton* m_modelSelect {nullptr}; // Cypher's .model-select: logo, model name, chevron
    QLabel* m_modelIcon {nullptr};
    QLabel* m_modelName {nullptr};
    QToolButton* m_sendButton {nullptr};
    LC_AIApprovalBox* m_approvals {nullptr};
    LC_AIApprovalModeButton* m_approvalMode {nullptr};
    QSet<QString> m_sessionAllows; // "Allow for this session" answers, until New Chat

    // chat history per drawing (lc_aichatsession.cpp): saves the chat, follows the active drawing
    LC_AIChatSession* m_chats {nullptr};
    /** Shows a saved chat (or an empty one) in place of the conversation. */
    void showChat(const QList<LC_AIChatMessage>& messages);
    LC_AIVoiceInput* m_voice {nullptr}; // the mic button and push-to-talk (hidden without Qt Multimedia)
    // plan mode and the debugger (lc_aiplanmode, lc_aidebugger)
    /** A request was added: plan mode's tools and prompt, and the Check & fix of the last answer goes. */
    void prepareRun();
    /** The debugger's pass over the last run: it checks the drawing against the request and fixes what is wrong. */
    void startDebugger(const QString& reason);
    /** A run ended: plan mode winds up, and the debugger checks the run or offers to. */
    void afterRun(bool stopped, bool failed = false);
    LC_AIPlanMode* m_plan {nullptr};
    LC_AIDebugger* m_debugger {nullptr};
    int m_runStart {-1};      // index in m_messages of the request the last run answered
    bool m_debugging {false}; // the run is the debugger's pass
};

#endif
