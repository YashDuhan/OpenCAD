/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AICONTROL_H
#define LC_AICONTROL_H

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <functional>

#include "lc_aitool.h"

class QMainWindow;
class QTimer;
class RS_GraphicView;
class LC_AIControlBanner;

/**
 * Who drives the drawing while the AI works, so the user and the AI do not race for it:
 *  - Working: the AI drives. The user can look around (wheel zoom, middle-button pan) but clicks, keys and
 *    toolbar presses on the drawing are held back. A banner says so and offers Take over and Stop.
 *  - Paused: the user took over and has full control; the AI waits before its next tool call. It goes on by
 *    itself after a few seconds without input (or on Resume), and is told what the user changed meanwhile.
 * The AI keeps its own view of the drawing: screenshots show what it last looked at, even when the user has
 * moved the visible view, and the view comes back to it before the AI acts.
 */
class LC_AIControl : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Working, Paused };
    static constexpr int RESUME_AFTER_IDLE_S = 8;

    static LC_AIControl& instance();

    /** A run starts in this window: the AI drives, the user looks on. */
    void begin(QMainWindow* window);
    /** The run ended (finished, failed or stopped): everything is the user's again. */
    void end();
    State state() const { return m_state; }

    /** The user takes the drawing: the AI waits before its next tool call. */
    void takeOver();
    /** The AI goes on (Resume, or no input from the user for a while). */
    void resume();

    /** Before a tool call runs: waits while the user has control, and brings the AI's view back for tools that act. */
    void beforeTool(const QString& toolName, std::function<void()> proceed);
    /** After a tool call ran: remembers the AI's view; tells the AI what the user changed while it waited. */
    void afterTool(const QString& toolName, LC_AIToolResult& result);
    /** Wraps tools so afterTool runs on their results. */
    static QList<LC_AITool> withControl(const QList<LC_AITool>& tools);

    /** The AI's own view of this drawing when the visible one differs (the user moved it): offset and zoom. */
    bool aiView(RS_GraphicView* view, int& offsetX, int& offsetY, double& factor) const;

    /** Mouse and key events the AI's tools send pass the lock while one of these lives. */
    struct AIInput {
        AIInput();
        ~AIInput();
    };
    static bool aiInputActive();

signals:
    void stopRequested();
    void stateChanged(LC_AIControl::State state);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct View {
        int offsetX {0};
        int offsetY {0};
        double factor {0};
    };
    struct Drawing {
        QHash<unsigned long long, QString> entities; // id -> one-line description
        QStringList layers;
        QString activeLayer;
        int selected {0};
    };

    LC_AIControl() = default;
    bool shouldBlock(QObject* watched, QEvent* event) const;
    void userActivity();
    void tick();
    void placeBanner();
    void setState(State state);
    void rememberView();
    void restoreView();
    static Drawing drawingNow();
    QString changesSince(const Drawing& before) const;

    State m_state {State::Idle};
    QPointer<QMainWindow> m_window;
    QPointer<LC_AIControlBanner> m_banner;
    QTimer* m_idle {nullptr};
    int m_secondsLeft {RESUME_AFTER_IDLE_S};
    QList<std::function<void()>> m_waiting; // tool calls held while the user has control
    QHash<RS_GraphicView*, View> m_views;   // the AI's own view per drawing
    Drawing m_atPause;
    QString m_note;                         // what the user changed, for the next tool result
};

#endif
