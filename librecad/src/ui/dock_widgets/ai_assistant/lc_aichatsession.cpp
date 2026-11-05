/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aichatsession.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QTimer>
#include <QToolButton>

#include "lc_aichathistorypopup.h"
#include "lc_aichatstore.h"
#include "lc_aichattranscript.h"
#include "lc_aiicons.h"
#include "qc_applicationwindow.h"
#include "qc_mdiwindow.h"

namespace {
    constexpr int SAVE_DELAY_MS = 1000;
    // the key a drawing window was filed under, so a new file name (Save, Save As) is noticed
    const char* KEY_PROPERTY = "aiChatDrawingKey";

    /** Conversations only grow or lose their last, unanswered question: the length and last message tell them apart. */
    bool sameConversation(const QList<LC_AIChatMessage>& a, const QList<LC_AIChatMessage>& b) {
        if (a.size() != b.size()) {
            return false;
        }
        if (a.isEmpty()) {
            return true;
        }
        const LC_AIChatMessage& x = a.last();
        const LC_AIChatMessage& y = b.last();
        return x.role == y.role && x.text == y.text && x.toolCallId == y.toolCallId && x.toolCalls.size() == y.toolCalls.size();
    }
}

LC_AIChatSession::LC_AIChatSession(QWidget* panel, const LC_AITheme& theme)
    : QObject(panel), m_panel(panel), m_theme(theme) {
    m_button = new QToolButton(panel);
    m_button->setIcon(LC_AIIcons::lucideIcon("history", 14, panel->palette().color(QPalette::ButtonText)));
    m_button->setToolTip(tr("Chat history of this drawing"));
    m_button->setAutoRaise(true);
    connect(m_button, &QToolButton::clicked, this, &LC_AIChatSession::openHistory);

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(SAVE_DELAY_MS);
    connect(m_saveTimer, &QTimer::timeout, this, &LC_AIChatSession::flush);
    connect(qApp, &QCoreApplication::aboutToQuit, this, &LC_AIChatSession::flush);

    // the panel is built before the drawing area: connect to it once the main window is complete
    QTimer::singleShot(0, this, &LC_AIChatSession::attach);
}

LC_AIChatSession::~LC_AIChatSession() {
    flush();
}

void LC_AIChatSession::attach() {
    auto* app = qobject_cast<QC_ApplicationWindow*>(m_panel->window());
    if (app == nullptr || app->getMdiArea() == nullptr) {
        return;
    }
    m_mdi = app->getMdiArea();
    connect(m_mdi, &QMdiArea::subWindowActivated, this, &LC_AIChatSession::onWindowActivated);
    onWindowActivated(m_mdi->currentSubWindow());
}

QC_MDIWindow* LC_AIChatSession::rootOf(QMdiSubWindow* window) {
    auto* w = qobject_cast<QC_MDIWindow*>(window);
    // a block being edited or a print preview belongs to its drawing
    while (w != nullptr && w->getParentWindow() != nullptr) {
        w = w->getParentWindow();
    }
    return w;
}

QC_MDIWindow* LC_AIChatSession::activeDrawing() const {
    return m_mdi ? rootOf(m_mdi->currentSubWindow()) : nullptr;
}

QC_MDIWindow* LC_AIChatSession::windowOf(const QString& key) {
    if (!m_mdi || key.isEmpty()) {
        return nullptr;
    }
    for (QMdiSubWindow* sub : m_mdi->subWindowList()) {
        auto* w = qobject_cast<QC_MDIWindow*>(sub);
        if (w != nullptr && w->getParentWindow() == nullptr && keyOf(w) == key) {
            return w;
        }
    }
    return nullptr;
}

QString LC_AIChatSession::nameOf(QC_MDIWindow* window) const {
    if (window == nullptr) {
        return {};
    }
    const QString file = window->getFileName();
    if (!file.isEmpty()) {
        return QFileInfo(file).fileName();
    }
    QString title = window->windowTitle();
    title.remove(QStringLiteral("[*]"));
    title = title.trimmed();
    return title.isEmpty() ? tr("Unsaved drawing") : title;
}

QString LC_AIChatSession::keyOf(QC_MDIWindow* window) {
    if (window == nullptr) {
        return {};
    }
    LC_AIChatStore& store = LC_AIChatStore::instance();
    const QString known = window->property(KEY_PROPERTY).toString();
    const QString file = window->getFileName();
    QString key;
    if (file.isEmpty()) {
        key = known.isEmpty() ? LC_AIChatStore::newUnsavedKey() : known;
    }
    else {
        key = LC_AIChatStore::pathKey(file);
        if (!known.isEmpty() && known != key) {
            // saved for the first time: its chats move to the file; saved as another file: they are copied,
            // so the old file keeps its history and the shown chat goes on as the new file's copy
            const QHash<QString, QString> ids = store.moveDrawing(known, key, nameOf(window), !LC_AIChatStore::isUnsavedKey(known));
            if (m_drawing == known) {
                m_chatId = ids.value(m_chatId, m_chatId);
                m_drawing = key;
                m_drawingName = nameOf(window);
            }
        }
    }
    window->setProperty(KEY_PROPERTY, key);
    return key;
}

void LC_AIChatSession::watch(QC_MDIWindow* window) {
    if (window == nullptr || m_watched.contains(window)) {
        return;
    }
    m_watched.removeAll(nullptr);
    m_watched << window;
    // Save and Save As retitle the window once the file is written: its key may change then
    QPointer<QC_MDIWindow> w(window);
    connect(window, &QWidget::windowTitleChanged, this, [this, w] {
        if (!w) {
            return;
        }
        if (w == m_window) {
            flush(); // what was said so far goes into the chat before it moves or is copied
        }
        keyOf(w);
        if (w == m_window) {
            m_drawingName = nameOf(w);
        }
    });
}

void LC_AIChatSession::onWindowActivated(QMdiSubWindow* sub) {
    QC_MDIWindow* window = rootOf(sub);
    if (window == nullptr || m_switching) {
        return; // no drawing (or OpenCAD lost the focus): the chat stays
    }
    watch(window);
    if (m_running) {
        m_pending = window != m_window ? window : nullptr; // the run stays with its chat
        return;
    }
    if (window != m_window) {
        follow(window);
    }
}

void LC_AIChatSession::follow(QC_MDIWindow* window) {
    flush();
    m_pending = nullptr;
    m_window = window;
    m_drawing = keyOf(window);
    m_drawingName = nameOf(window);
    show(LC_AIChatStore::instance().latest(m_drawing));
}

void LC_AIChatSession::show(const QString& chatId) {
    LC_AIChatStore& store = LC_AIChatStore::instance();
    m_saveTimer->stop();
    m_dirty = false;
    m_chatId = store.contains(chatId) ? chatId : QString();
    m_messages = m_chatId.isEmpty() ? QList<LC_AIChatMessage>() : store.load(m_chatId);
    emit showMessages(m_messages);
}

void LC_AIChatSession::save(const QList<LC_AIChatMessage>& messages) {
    if (!m_dirty && sameConversation(messages, m_messages)) {
        return; // e.g. a request that could not start: the saved chat is unchanged (and keeps its date)
    }
    m_messages = messages;
    m_dirty = true;
    if (!m_saveTimer->isActive()) { // not restarted on every message, so a long run is still saved as it goes
        m_saveTimer->start();
    }
}

void LC_AIChatSession::flush() {
    m_saveTimer->stop();
    if (!m_dirty) {
        return;
    }
    m_dirty = false;
    LC_AIChatStore& store = LC_AIChatStore::instance();
    if (m_messages.isEmpty()) {
        // it only held a question that got no answer (stopped or failed): nothing to keep
        if (!m_chatId.isEmpty()) {
            store.remove(m_chatId);
            m_chatId.clear();
        }
        return;
    }
    m_chatId = store.save(m_chatId, m_drawing, m_drawingName, m_messages);
}

void LC_AIChatSession::startNew() {
    flush();
    m_pending = nullptr;
    if (QC_MDIWindow* active = activeDrawing()) { // a new chat is about the drawing in front of you
        m_window = active;
        m_drawing = keyOf(active);
        m_drawingName = nameOf(active);
    }
    m_chatId.clear();
    m_messages.clear();
}

void LC_AIChatSession::setRunning(const bool running) {
    if (m_running == running) {
        return;
    }
    m_running = running;
    m_button->setEnabled(!running);
    if (running) {
        m_pending = nullptr;
        m_runWindows.clear();
        if (m_mdi) {
            for (QMdiSubWindow* sub : m_mdi->subWindowList()) {
                m_runWindows << sub;
            }
        }
        return;
    }
    flush();
    // the user moved to another drawing while the AI worked: show its chat now. A drawing the AI opened
    // itself is part of the run, so its answer stays in view.
    const QPointer<QC_MDIWindow> target = m_pending;
    m_pending = nullptr;
    if (target && m_runWindows.contains(QPointer<QMdiSubWindow>(target.data()))) {
        QTimer::singleShot(0, this, [this, target] {
            if (target && !m_running && target == activeDrawing() && target != m_window) {
                follow(target);
            }
        });
    }
    m_runWindows.clear();
}

void LC_AIChatSession::resume(const QString& chatId) {
    LC_AIChatStore& store = LC_AIChatStore::instance();
    if (m_running || !store.contains(chatId)) {
        return;
    }
    flush();
    const LC_AIChatInfo info = store.info(chatId);
    QC_MDIWindow* window = windowOf(info.drawing);
    auto* app = qobject_cast<QC_ApplicationWindow*>(m_panel->window());
    m_switching = true;
    if (window == nullptr && app != nullptr && !LC_AIChatStore::isUnsavedKey(info.drawing) && !info.drawing.isEmpty()
        && QFileInfo(info.drawing).isFile()) {
        app->openFile(info.drawing); // a chat of a drawing that is not open: open it
        window = windowOf(info.drawing);
    }
    if (window != nullptr && app != nullptr && activeDrawing() != window) {
        app->activateWindow(window);
        if (m_mdi && m_mdi->currentSubWindow() != window) { // the drawing area follows the focus, which may be elsewhere
            m_mdi->setActiveSubWindow(window);
        }
    }
    m_switching = false;
    m_pending = nullptr;
    // a chat whose drawing is gone (or was never saved) is shown as it is; it stays filed under its drawing
    m_window = window;
    m_drawing = info.drawing;
    m_drawingName = window != nullptr ? nameOf(window) : info.drawingName;
    if (window != nullptr) {
        watch(window);
    }
    show(chatId);
}

void LC_AIChatSession::openHistory() {
    flush(); // the list shows the chat as it is now
    // "This drawing" is the one in front of you (the shown chat may belong to another after a run)
    QC_MDIWindow* active = activeDrawing();
    const QString drawing = active != nullptr ? keyOf(active) : m_drawing;
    const QString name = active != nullptr ? nameOf(active) : m_drawingName;
    auto* popup = new LC_AIChatHistoryPopup(m_theme, drawing, name, m_chatId, m_panel);
    connect(popup, &LC_AIChatHistoryPopup::resumeRequested, this, &LC_AIChatSession::resume);
    connect(popup, &LC_AIChatHistoryPopup::deleted, this, [this](const QString& id) {
        if (id == m_chatId && !m_running) { // the shown chat is gone: show the drawing's next one
            show(LC_AIChatStore::instance().latest(m_drawing));
        }
    });
    popup->popup(m_button);
}

void LC_AIChatSession::replay(LC_AIChatTranscript* transcript, const QList<LC_AIChatMessage>& messages) {
    bool first = true;
    for (const LC_AIChatMessage& m : messages) {
        switch (m.role) {
            case LC_AIChatMessage::Role::User:
                if (!first) {
                    transcript->finishRun(); // each question started a run: close the one before, as it was closed then
                }
                transcript->addUser(m.text);
                break;
            case LC_AIChatMessage::Role::Assistant:
                transcript->addAssistant(m);
                break;
            case LC_AIChatMessage::Role::ToolResult:
                if (m.toolName == QStringLiteral("request_clarification") && !m.result.isError) {
                    // the question card shows the answer it got (questions are answered in the order they were asked)
                    const QString answer = QJsonDocument::fromJson(m.result.joinedText().toUtf8()).object().value("answer").toString();
                    for (LC_AIClarificationCard* card : transcript->findChildren<LC_AIClarificationCard*>()) {
                        if (!answer.isEmpty() && !card->isAnswered()) {
                            card->showAnswer(answer);
                            break;
                        }
                    }
                }
                transcript->toolResult(m);
                break;
        }
        first = false;
    }
    transcript->finishRun(); // calls that never got a result show as stopped; the last block of calls folds
}
