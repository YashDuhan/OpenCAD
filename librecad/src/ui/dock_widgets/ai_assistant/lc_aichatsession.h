/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AICHATSESSION_H
#define LC_AICHATSESSION_H

#include <QList>
#include <QObject>
#include <QPointer>

#include "lc_aiinferencemanager.h"
#include "lc_aiprovidersdialog.h"

class LC_AIChatTranscript;
class QC_MDIWindow;
class QMdiArea;
class QMdiSubWindow;
class QTimer;
class QToolButton;

/**
 * Files the AI panel's conversation under the drawing it is about. The chat is saved as it grows; moving to
 * another drawing shows the chat it showed last, else its latest (never in the middle of a run: the run stays with its chat,
 * and a drawing you moved to meanwhile shows once it ends); the header's history button lists the chats to
 * resume, rename or delete. A drawing that was never saved gets a temporary key, which becomes its file path
 * when it is saved.
 */
class LC_AIChatSession : public QObject {
    Q_OBJECT
public:
    LC_AIChatSession(QWidget* panel, const LC_AITheme& theme);
    ~LC_AIChatSession() override;

    /** The history button, for the panel's header. */
    QToolButton* historyButton() const { return m_button; }
    QString chatId() const { return m_chatId; }
    /** The key of the drawing the shown chat belongs to. */
    QString drawing() const { return m_drawing; }

    /** The conversation changed: it is written shortly after (once a second at most while the AI works). */
    void save(const QList<LC_AIChatMessage>& messages);
    /** Writes a pending save now. */
    void flush();
    /** New Chat: the old chat stays in the history; the next question starts a new chat for the active drawing. */
    void startNew();
    /** A run started or ended: the chat does not change while it goes. */
    void setRunning(bool running);
    /** Shows a saved chat, switching to its drawing (or opening its file) when it can. */
    void resume(const QString& chatId);

    /** Fills an empty transcript with a saved conversation: questions, answers, tool calls folded with their results. */
    static void replay(LC_AIChatTranscript* transcript, const QList<LC_AIChatMessage>& messages);

signals:
    /** The panel shows this conversation now: a saved chat, or an empty one. */
    void showMessages(const QList<LC_AIChatMessage>& messages);

private:
    void attach();
    void onWindowActivated(QMdiSubWindow* window);
    /** Shows the chat this drawing showed last, else its latest one (an empty one when it has none). */
    void follow(QC_MDIWindow* window);
    /** Shows a saved chat; one whose file cannot be read is not shown (a note says so) and a new chat starts. */
    void show(const QString& chatId);
    /** The drawing's window keeps which chat it shows, for when it comes back to the front. */
    void remember();
    void openHistory();
    void watch(QC_MDIWindow* window);
    QC_MDIWindow* activeDrawing() const;
    bool hasDrawings() const;
    QC_MDIWindow* windowOf(const QString& key);
    /** The drawing's key. When it was saved under a (new) file name meanwhile, its chats move along. */
    QString keyOf(QC_MDIWindow* window);
    QString nameOf(QC_MDIWindow* window) const;
    static QC_MDIWindow* rootOf(QMdiSubWindow* window);

    QWidget* m_panel;
    LC_AITheme m_theme;
    QToolButton* m_button {nullptr};
    QTimer* m_saveTimer {nullptr};
    QPointer<QMdiArea> m_mdi;
    QPointer<QC_MDIWindow> m_window;     // the drawing the shown chat belongs to
    QString m_drawing;                   // its key
    QString m_drawingName;
    QString m_chatId;                    // empty: a new chat, saved once it has a message
    QList<LC_AIChatMessage> m_messages;  // as last handed to save()
    bool m_dirty {false};
    bool m_running {false};
    bool m_switching {false};            // resume() activates a drawing itself
    QPointer<QC_MDIWindow> m_pending;    // the drawing the user moved to during a run
    QList<QPointer<QMdiSubWindow>> m_runWindows; // the drawings open when the run began
    QList<QPointer<QC_MDIWindow>> m_watched;
};

#endif
