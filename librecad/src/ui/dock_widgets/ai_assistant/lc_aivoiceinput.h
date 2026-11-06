/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIVOICEINPUT_H
#define LC_AIVOICEINPUT_H

#include <QElapsedTimer>
#include <QPointer>
#include <QWidget>

#include "lc_aiprovidersdialog.h"

class LC_AIVoiceRecorder;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QTimer;
class QToolButton;

/**
 * Voice input of the AI composer, like Cypher's dictation (apps/desktop/src/components/Composer.tsx): a mic
 * button with the voice menu next to it, a live level strip in the composer while recording, and the
 * transcript appended to the message box (and sent at once when the user wants that). Click the mic to
 * start and again to stop, or hold it (or Ctrl+Space while the AI panel has focus) and let go to stop;
 * Esc drops a recording. Built without Qt Multimedia, the control stays hidden.
 */
class LC_AIVoiceInput : public QWidget {
    Q_OBJECT
public:
    enum class State { Idle, Recording, Transcribing };

    /** panel: the AI panel, whose focus enables push-to-talk; parent: the composer frame. */
    LC_AIVoiceInput(const LC_AITheme& theme, QPlainTextEdit* input, QWidget* panel, QWidget* parent);
    ~LC_AIVoiceInput() override;

    /** The live level strip, for the composer's layout under its actions row. Hidden while there is nothing to show. */
    QWidget* levelStrip() const;
    State state() const { return m_state; }
    /** The voice menu: speech endpoint and model, send after transcribing. One at a time. */
    void openMenu();
    /** Drops a recording, or a transcription on its way. */
    void cancel();
    /** The mic's look and tooltip: ready, or what is missing. Call it when keys may have changed (Providers). */
    void refresh();

signals:
    /** A transcript is in the message box and the user asked to send it at once. */
    void sendRequested();
    void failed(const QString& error);
    /** The voice menu's "Add a key" row: open the Providers modal. */
    void providersRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    /** Mic button or push-to-talk key went down: start, or stop a recording a tap started. */
    void press(bool fromKey);
    /** ...and up: a long press stops the recording it started (push-to-talk), a tap keeps it going. */
    void release();
    void startRecording();
    void stopRecording();
    /** Sends the recording to the speech endpoint; the transcript goes into the message box. */
    void transcribe(const QByteArray& wav);
    void setState(State state);
    void insertTranscript(const QString& text);
    /** A short note in the level strip, gone after a few seconds. */
    void showNote(const QString& text);
    /** Reports an error in the conversation; the same one again only as a note, so retries do not pile up bubbles. */
    void fail(const QString& error);
    bool panelHasFocus() const;

    LC_AITheme m_theme;
    QPlainTextEdit* m_input {nullptr};
    QWidget* m_panel {nullptr};
    QWidget* m_composer {nullptr};
    QToolButton* m_mic {nullptr};
    QToolButton* m_menuButton {nullptr};
    QWidget* m_strip {nullptr};
    LC_AIVoiceRecorder* m_recorder {nullptr};
    QNetworkAccessManager* m_network {nullptr};
    QPointer<QNetworkReply> m_reply;
    QPointer<QWidget> m_menu;      // the voice menu while it is open
    QTimer* m_tick {nullptr};
    State m_state {State::Idle};
    int m_generation {0};          // a transcription that comes back after cancel() is dropped
    int m_note {0};                // the note shown last: an older one's timer leaves it alone
    bool m_held {false};           // the mic button or the push-to-talk key is down
    bool m_startedByPress {false}; // ... and that press started the recording
    bool m_spaceDown {false};      // the push-to-talk Space is down: its repeats and key-up are ours
    bool m_menuOnRelease {false};  // a click on the mic while nothing is set up: the voice menu opens on release
    QElapsedTimer m_pressed;
    QElapsedTimer m_recording;     // since the recording started: tells a silent device from a short tap
    QString m_lastError;           // the error last reported in the conversation, until a recording works
};

#endif
