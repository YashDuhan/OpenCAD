/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AICOMPOSEREXTRAS_H
#define LC_AICOMPOSEREXTRAS_H

#include <QList>
#include <QObject>
#include <QPlainTextEdit>
#include <QPointer>

#include "lc_aiattachments.h"
#include "lc_aimentions.h"
#include "lc_aiprovidersdialog.h"

class QLabel;
class QListWidget;
class QTimer;
class QToolButton;
struct LC_AIChatMessage;

/**
 * The AI chat's message box: files dropped or pasted on it become attachments, and the @-mentions in its text
 * show as pills that are deleted and stepped over as one piece.
 */
class LC_AIComposerInput : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit LC_AIComposerInput(QWidget* parent = nullptr);
    void setMentionColors(const QColor& text, const QColor& background);
    /** The mentions in the text now, recognised against the open drawing. */
    const QList<LC_AIMention>& mentions();
    /** Recognises the mentions again (the drawing's layers or blocks changed). */
    void refreshMentions();

signals:
    void filesOffered(const QStringList& paths);
    void imageOffered(const QImage& image);

protected:
    bool canInsertFromMimeData(const QMimeData* source) const override;
    void insertFromMimeData(const QMimeData* source) override;
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    /** The cursor never stops inside a mention: it goes to the end it was heading for. */
    void stepOverMentions();
    const LC_AIMention* mentionAround(int position, bool includeEnds) const;

    QList<LC_AIMention> m_mentions;
    int m_revision {-1};      // the document revision m_mentions were found in
    QColor m_mentionText;
    QColor m_mentionBackground;
    int m_lastPosition {0};
};

/**
 * What the composer adds to plain text (Cypher's composer): the attachment strip above the message box with
 * removable thumbnails, the paperclip and @ buttons, and the @ completion popup with the selection, the view,
 * the drawing and its layers, blocks and entities. On send it fills the message with the files and with the
 * drawing context its mentions stand for.
 */
class LC_AIComposerExtras : public QObject {
    Q_OBJECT
public:
    /** `panel` hosts the popup (it opens over the conversation, above `composer`). */
    LC_AIComposerExtras(LC_AIComposerInput* input, QWidget* composer, const LC_AITheme& theme, QWidget* panel);

    QWidget* strip() const { return m_strip; }
    /** The paperclip and @ buttons, for the composer's actions row. */
    QWidget* buttons() const { return m_buttons; }
    bool hasAttachments() const { return !m_attachments.isEmpty(); }
    const QList<LC_AIAttachment>& attachments() const { return m_attachments; }

    /** Attaches image and PDF files; the strip says why one could not be attached. */
    void addFiles(const QStringList& paths);
    void addImage(const QImage& image, const QString& name = {});
    /** The message about to be sent: its files, and the drawing context of its @-mentions. */
    void fill(LC_AIChatMessage& message) const;
    /** The message went out: no files, no popup. */
    void clear();
    /** A send failed after the box was emptied: the files go back in. */
    void restore(const QList<LC_AIAttachment>& attachments);
    /** Adds a mention at the end of the message box and puts the cursor after it. */
    void insertMention(const QString& mention);
    bool isPopupVisible() const;
    QListWidget* popupList() const { return m_list; }

    /** Opens the AI sidebar of this window with a mention in the message box, ready for the question. */
    static void openWithMention(QWidget* window, const QString& mention);
    /** The files of a sent message, as the conversation shows them above its bubble. */
    static QWidget* messageRow(const QList<LC_AIAttachment>& attachments, const LC_AITheme& theme, QWidget* parent);

signals:
    /** Attachments were added or removed (the send button follows). */
    void changed();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuildStrip();
    void showNotice(const QString& text);
    void openFileDialog();
    void startMention();
    void updatePopup();
    void hidePopup();
    void placePopup();
    void moveHighlight(int step);
    void acceptCandidate(int row);
    void setDropping(bool dropping);

    QPointer<LC_AIComposerInput> m_input;
    QWidget* m_viewport {nullptr};  // the message box's viewport, where files are dropped
    QWidget* m_composer {nullptr};
    QWidget* m_panel {nullptr};
    LC_AITheme m_theme;
    QList<LC_AIAttachment> m_attachments;
    QWidget* m_strip {nullptr};
    QWidget* m_tiles {nullptr};
    QLabel* m_notice {nullptr};
    QTimer* m_noticeTimer {nullptr};
    QWidget* m_buttons {nullptr};
    QToolButton* m_attachButton {nullptr};
    QToolButton* m_mentionButton {nullptr};

    // the @ popup
    QWidget* m_popup {nullptr};
    QListWidget* m_list {nullptr};
    QList<LC_AIMentions::Candidate> m_candidates;
    int m_atPosition {-1};      // where the "@" being completed is
    int m_dismissedAt {-1};     // Escape closed the popup for the "@" at this position
};

#endif
