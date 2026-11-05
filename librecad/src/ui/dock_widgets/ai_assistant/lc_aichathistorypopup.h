/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AICHATHISTORYPOPUP_H
#define LC_AICHATHISTORYPOPUP_H

#include <QFrame>

#include "lc_aiprovidersdialog.h"

class QLineEdit;
class QListWidget;
class QPushButton;

/**
 * The chat history list, a popup under the header's history button (styled like the model picker): the chats of
 * the active drawing, or of all drawings grouped by drawing, newest first, each with its title (the first
 * question), date and message count. Typing searches titles, questions and drawing names. A click resumes a
 * chat; on the highlighted row the pencil renames it in place and the bin deletes it (a second click confirms).
 */
class LC_AIChatHistoryPopup : public QFrame {
    Q_OBJECT
public:
    /** drawing and drawingName: the active drawing ("This drawing"); current: the chat the panel shows. */
    LC_AIChatHistoryPopup(const LC_AITheme& theme, const QString& drawing, const QString& drawingName,
                          const QString& current, QWidget* parent = nullptr);
    /** Opens under the anchor, right edges aligned (above it when there is no room below). */
    void popup(QWidget* anchor);

signals:
    void resumeRequested(const QString& chatId);
    void deleted(const QString& chatId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setAllDrawings(bool all);
    void refresh();
    void resume(int row);
    void rename(int row);
    void remove(int row);
    void disarm();

    LC_AITheme m_theme;
    QString m_drawing;
    QString m_drawingName;
    QString m_current;
    bool m_all {false};
    QString m_armed;                 // the chat whose bin was clicked once: a second click deletes it
    QString m_editing;               // the chat being renamed: its row shows the name box only
    QLineEdit* m_search {nullptr};
    QListWidget* m_list {nullptr};
    QPushButton* m_thisTab {nullptr};
    QPushButton* m_allTab {nullptr};

    friend class LC_AIChatHistoryDelegate;
};

#endif
