/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIREVERTBAR_H
#define LC_AIREVERTBAR_H

#include <QWidget>

#include "lc_aiprovidersdialog.h"

class LC_AIChatTranscript;
class QFrame;
class QLabel;
class QPushButton;

/**
 * "Revert AI changes" under an answer of the AI that changed the drawing, with the number of changes. It undoes
 * back to the checkpoint taken before the answer when nothing changed after it; otherwise it says what changed
 * and offers to revert only the answer's own changes, or to cancel. Once reverted, Restore brings them back.
 */
class LC_AIRevertBar : public QWidget {
    Q_OBJECT
public:
    LC_AIRevertBar(int run, const LC_AITheme& theme, QWidget* parent = nullptr);
    /** Puts a bar under every answer of this conversation that changes the drawing. */
    static void addAfterRuns(LC_AIChatTranscript* transcript, const LC_AITheme& theme);
    int run() const { return m_run; }

private:
    void revert();
    void revertOwnChanges();
    void restore();
    void showReady();
    void showReverted(const QString& note);
    void showCard(const QString& text, bool offerOwn);
    void showNote(const QString& text);
    void updateEnabled();
    void fitButton();

    int m_run {0};
    LC_AITheme m_theme;
    bool m_reverted {false};
    QPushButton* m_button {nullptr};  // icon, "Revert AI changes", and the number of changes in a fainter tone
    QLabel* m_icon {nullptr};
    QLabel* m_text {nullptr};
    QLabel* m_count {nullptr};
    QPushButton* m_restore {nullptr};
    QPushButton* m_review {nullptr};  // shows the run's changes in the drawing (lc_aichangepreview.cpp)
    QFrame* m_card {nullptr};         // why it cannot simply undo, and what it can do instead
    QLabel* m_cardText {nullptr};
    QPushButton* m_own {nullptr};
    QPushButton* m_cancel {nullptr};
    QLabel* m_note {nullptr};
};

#endif
