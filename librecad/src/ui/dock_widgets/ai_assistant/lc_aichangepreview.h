/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AICHANGEPREVIEW_H
#define LC_AICHANGEPREVIEW_H

#include <QFrame>
#include <QPointer>

#include "lc_aiprovidersdialog.h"

class QLabel;
class QPushButton;
class QTimer;
class QToolButton;
class RS_GraphicView;

/**
 * The change preview: what an AI run changed, shown in the drawing before the user keeps it. Entities the run drew glow
 * green, entities it deleted (or replaced by editing them) show as red dashed ghosts. Accept keeps the changes, Reject
 * takes them back (through the run's checkpoint, see lc_aicheckpoints). With "Review the AI's changes" on (the default)
 * every run that changed the drawing opens the preview by itself; the Review button under an answer opens it later.
 * The highlight is an overlay of the drawing's view: it never changes the drawing.
 */
class LC_AIChangePreview : public QObject {
    Q_OBJECT
public:
    static LC_AIChangePreview& instance();

    /** Highlights the run's changes in its drawing; false when there is nothing left to show (or the drawing is closed). */
    bool show(int run);
    /** Keeps the changes: the highlight goes. */
    void accept();
    /** Takes the changes back: the whole run while nothing changed after it, else only the run's own changes. */
    bool reject(QString& message);
    /** Zooms the drawing to the changed entities. */
    void zoomToChanges();
    /** Shows or hides the highlight while the review stays open. */
    void setHighlightVisible(bool visible);
    bool highlightVisible() const { return m_visible; }
    /** Ends the review without a decision (a new run starts, the drawing closed): the changes stay. */
    void close();

    int run() const { return m_run; }
    bool isOpen() const { return m_run > 0; }
    int addedShown() const { return m_added; }
    int removedShown() const { return m_removed; }

    static bool reviewAfterRuns();
    static void setReviewAfterRuns(bool on);

signals:
    /** A review opened (run > 0) or ended (run == 0); also when what it shows changed. */
    void changed();
    /** The review ended: accepted, rejected or closed, with a note for the chat. */
    void finished(int run, const QString& note);

private:
    LC_AIChangePreview();
    void rebuild();
    void clearOverlay();

    int m_run {0};
    QPointer<RS_GraphicView> m_view;
    bool m_visible {true};
    int m_added {0};
    int m_removed {0};
    QString m_shownKey; // which entities the overlay shows, to rebuild it only when that changes
    QTimer* m_watch {nullptr};
};

/** The review bar above the message box while a change preview is open: what changed, Accept, Reject. */
class LC_AIReviewBar : public QFrame {
    Q_OBJECT
public:
    explicit LC_AIReviewBar(const LC_AITheme& theme, QWidget* parent = nullptr);

private:
    void refresh();

    LC_AITheme m_theme;
    QLabel* m_title {nullptr};
    QLabel* m_legend {nullptr};
    QPushButton* m_accept {nullptr};
    QPushButton* m_reject {nullptr};
    QToolButton* m_zoom {nullptr};
    QToolButton* m_eye {nullptr};
    QToolButton* m_auto {nullptr};
    QLabel* m_note {nullptr};
};

#endif
