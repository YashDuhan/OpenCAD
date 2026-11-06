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
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QToolButton>

#include "lc_aichathistorypopup.h"
#include "lc_aichatstore.h"
#include "lc_aichattranscript.h"
#include "lc_aicheckpoints.h"
#include "lc_aidebugger.h"
#include "lc_aiicons.h"
#include "lc_aiplanmode.h"
#include "lc_airevertbar.h"
#include "qc_applicationwindow.h"
#include "qc_mdiwindow.h"

namespace {
    constexpr int SAVE_DELAY_MS = 1000;
    // the key a drawing window was filed under, so a new file name (Save, Save As) is noticed
    const char* KEY_PROPERTY = "aiChatDrawingKey";
    // the chat the panel showed for a drawing window ("" after New Chat), shown again when the drawing comes back to the front
    const char* CHAT_PROPERTY = "aiChatShown";

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
            if (const QString shown = window->property(CHAT_PROPERTY).toString(); ids.contains(shown)) {
                window->setProperty(CHAT_PROPERTY, ids.value(shown)); // the window goes on with the new file's copy
            }
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
    // the chat you had open for this drawing (a resumed older one, or the empty one of New Chat), else its latest
    LC_AIChatStore& store = LC_AIChatStore::instance();
    const QVariant shown = window->property(CHAT_PROPERTY);
    QString chatId = shown.toString();
    if (!shown.isValid() || (!chatId.isEmpty() && (!store.contains(chatId) || store.info(chatId).drawing != m_drawing))) {
        chatId = store.latest(m_drawing); // never shown yet, or deleted (or filed elsewhere) meanwhile
    }
    show(chatId);
}

void LC_AIChatSession::remember() {
    if (m_window) {
        m_window->setProperty(CHAT_PROPERTY, m_chatId); // also when empty: a New Chat stays new
    }
}

void LC_AIChatSession::show(const QString& chatId) {
    LC_AIChatStore& store = LC_AIChatStore::instance();
    m_saveTimer->stop();
    m_dirty = false;
    m_chatId = store.contains(chatId) ? chatId : QString();
    QString error;
    m_messages = m_chatId.isEmpty() ? QList<LC_AIChatMessage>() : store.load(m_chatId, &error);
    const QString unreadable = error.isEmpty() ? QString() : store.info(m_chatId).title;
    if (!error.isEmpty()) {
        m_chatId.clear(); // its file is left as it is: what is asked now goes into a new chat, not over it
        m_messages.clear();
    }
    remember();
    emit showMessages(m_messages);
    if (!error.isEmpty()) {
        if (auto* transcript = m_panel->findChild<LC_AIChatTranscript*>()) {
            transcript->addError(tr("The chat \"%1\" could not be read: %2. Its file is left as it is; what you ask now "
                                    "starts a new chat.")
                                     .arg(unreadable.isEmpty() ? tr("Untitled chat") : unreadable, error));
        }
    }
}

void LC_AIChatSession::save(const QList<LC_AIChatMessage>& messages) {
    if (!m_dirty && sameConversation(messages, m_messages)) {
        return; // e.g. a request that could not start: the saved chat is unchanged (and keeps its date)
    }
    // the AI run each answer and result belongs to, so a chat shown again gets its "Revert AI changes" bars back:
    // the messages saved before keep theirs, the ones new since the last save are of the run in progress
    // (the panel saves as each one arrives)
    QList<LC_AIChatMessage> stamped = messages;
    const int run = LC_AICheckpoints::instance().currentRun();
    for (int i = 0; i < stamped.size(); ++i) {
        LC_AIChatMessage& m = stamped[i];
        if (m.run == 0 && i < m_messages.size() && m_messages[i].role == m.role && m_messages[i].toolCallId == m.toolCallId) {
            m.run = m_messages[i].run;
        }
        else if (m.run == 0 && i >= m_messages.size() && m.role != LC_AIChatMessage::Role::User) {
            m.run = run;
        }
    }
    m_messages = stamped;
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
            remember();
        }
        return;
    }
    if (!m_running) {
        LC_AIChatStore::closeOpenCalls(m_messages); // a call stopped while it waited for approval is saved as stopped
    }
    m_chatId = store.save(m_chatId, m_drawing, m_drawingName, m_messages);
    remember();
}

void LC_AIChatSession::startNew() {
    flush();
    m_pending = nullptr;
    if (QC_MDIWindow* active = activeDrawing()) { // a new chat is about the drawing in front of you
        m_window = active;
        m_drawing = keyOf(active);
        m_drawingName = nameOf(active);
    }
    else if (!hasDrawings()) { // no drawing open: the chat is filed under none, not under the last one closed
        m_window = nullptr;
        m_drawing.clear();
        m_drawingName.clear();
    }
    m_chatId.clear();
    m_messages.clear();
    remember();
}

bool LC_AIChatSession::hasDrawings() const {
    if (m_mdi) {
        for (QMdiSubWindow* sub : m_mdi->subWindowList()) {
            if (rootOf(sub) != nullptr) {
                return true;
            }
        }
    }
    return false;
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
    QSet<QString> open; // the drawings open now: an unsaved one that is not is gone for good
    if (m_mdi) {
        for (QMdiSubWindow* sub : m_mdi->subWindowList()) {
            if (QC_MDIWindow* w = rootOf(sub)) {
                open.insert(keyOf(w));
            }
        }
    }
    auto* popup = new LC_AIChatHistoryPopup(m_theme, drawing, name, m_chatId, open, m_panel);
    // once the popup's click or key press has been handled: opening the chat's drawing runs the event loop, and the
    // popup (closed by the choice) is deleted in it, which must not happen while its own handler still runs
    connect(popup, &LC_AIChatHistoryPopup::resumeRequested, this, [this](const QString& id) {
        QTimer::singleShot(0, this, [this, id] { resume(id); });
    });
    connect(popup, &LC_AIChatHistoryPopup::deleted, this, [this](const QString& id) {
        if (id == m_chatId && !m_running) { // the shown chat is gone: show the drawing's next one
            show(LC_AIChatStore::instance().latest(m_drawing));
        }
    });
    popup->popup(m_button);
}

namespace {
    /** Cards of type T added to the transcript by the answer just replayed, matched to its calls named `tool` in order. */
    template <typename T>
    void matchCards(LC_AIChatTranscript* transcript, const QList<T*>& before, const LC_AIChatMessage& answer, const QString& tool,
                    QHash<QString, QPointer<T>>& cards) {
        QList<T*> added = transcript->findChildren<T*>();
        added.removeIf([&before](T* card) { return before.contains(card); });
        for (const LC_AIToolCall& call : answer.toolCalls) {
            // propose_plan without steps shows as a plain row (LC_AIPlanMode::cardFor)
            const QJsonValue steps = call.arguments.value("steps");
            const bool hasCard = call.name == tool
                                 && (tool != QStringLiteral("propose_plan") || !steps.toArray().isEmpty() || !steps.toString().trimmed().isEmpty());
            if (hasCard && !added.isEmpty()) {
                cards.insert(call.id, added.takeFirst());
            }
        }
    }

    /** The steps of an edited plan, as the result of propose_plan lists them ("1. ..."). */
    QStringList editedSteps(const QString& result) {
        static const QRegularExpression line(QStringLiteral("^\\d+\\. (.*)$"));
        QStringList steps;
        for (const QString& l : result.split('\n')) {
            if (const QRegularExpressionMatch match = line.match(l); match.hasMatch()) {
                steps << match.captured(1);
            }
        }
        return steps;
    }
}

void LC_AIChatSession::replay(LC_AIChatTranscript* transcript, const QList<LC_AIChatMessage>& messages) {
    const LC_AITheme theme = LC_AITheme::current();
    QHash<QString, QPointer<LC_AIClarificationCard>> questions; // by call id
    QHash<QString, QPointer<LC_AIPlanCard>> plans;
    QHash<QString, LC_AIToolCall> calls;
    QPointer<LC_AIPlanCard> plan; // the approved plan whose ticks update_plan set
    QList<LC_AIPlanStep> planSteps;
    LC_AIDebuggerSection* section = nullptr; // the debugger's pass being replayed
    bool answered = false;                   // ... and whether it ended with its verdict
    int run = 0;                             // the AI run of the answers so far, for its "Revert AI changes"
    QSet<int> runsShown;
    // how a run ended live: its calls closed, its revert bar under the answer, then the debugger's section closed
    const auto endRun = [&] {
        transcript->finishRun(); // calls that never got a result show as stopped; the last block of calls folds
        if (run != 0 && !runsShown.contains(run)) {
            runsShown.insert(run);
            LC_AIRevertBar::addForRun(transcript, run, theme);
        }
        run = 0;
        if (section != nullptr) {
            transcript->closeSection();
            answered ? section->setDone() : section->setStopped();
            section = nullptr;
        }
    };
    bool first = true;
    for (const LC_AIChatMessage& m : messages) {
        if (m.run != 0 && run != 0 && m.run != run) {
            endRun(); // a run of its own without a question of its own (not expected, but its bar stays its own)
        }
        if (m.run != 0) {
            run = m.run;
        }
        switch (m.role) {
            case LC_AIChatMessage::Role::User:
                if (!first) {
                    endRun(); // each question started a run: close the one before, as it was closed then
                }
                if (m.internal == QStringLiteral("debugger")) { // the debugger's request: its section, as live
                    section = new LC_AIDebuggerSection(m.internalReason, theme);
                    transcript->openSection(section, section->body());
                    answered = false;
                }
                else if (m.text.isEmpty() && m.attachments.isEmpty()) {
                    transcript->addUser(tr("(attached files the saved chat no longer has)"));
                }
                else {
                    transcript->addUser(m.text, m.attachments);
                }
                break;
            case LC_AIChatMessage::Role::Assistant: {
                bool asks = false;
                bool proposes = false;
                for (const LC_AIToolCall& call : m.toolCalls) {
                    calls.insert(call.id, call);
                    asks = asks || call.name == QStringLiteral("request_clarification");
                    proposes = proposes || call.name == QStringLiteral("propose_plan");
                }
                const QList<LC_AIClarificationCard*> questionsBefore =
                    asks ? transcript->findChildren<LC_AIClarificationCard*>() : QList<LC_AIClarificationCard*>();
                const QList<LC_AIPlanCard*> plansBefore = proposes ? transcript->findChildren<LC_AIPlanCard*>() : QList<LC_AIPlanCard*>();
                transcript->addAssistant(m);
                if (asks) {
                    matchCards(transcript, questionsBefore, m, QStringLiteral("request_clarification"), questions);
                }
                if (proposes) {
                    matchCards(transcript, plansBefore, m, QStringLiteral("propose_plan"), plans);
                }
                answered = m.toolCalls.isEmpty();
                break;
            }
            case LC_AIChatMessage::Role::ToolResult: {
                const QString text = m.result.joinedText();
                if (m.toolName == QStringLiteral("request_clarification") && !m.result.isError) {
                    // the question card shows the answer it got
                    const QString answer = QJsonDocument::fromJson(text.toUtf8()).object().value("answer").toString();
                    if (LC_AIClarificationCard* card = questions.value(m.toolCallId); card != nullptr && !answer.isEmpty()) {
                        card->showAnswer(answer);
                    }
                }
                else if (m.toolName == QStringLiteral("propose_plan")) {
                    // the plan card shows the answer it got and, below, the ticks of update_plan
                    if (LC_AIPlanCard* card = plans.value(m.toolCallId)) {
                        if (m.result.isError) {
                            card->setStopped();
                        }
                        else if (text.startsWith(QStringLiteral("The user rejected"))) {
                            card->showAnswer(LC_AIPlanCard::Answer::Rejected, {});
                        }
                        else {
                            const bool edited = text.startsWith(QStringLiteral("The user edited"));
                            const QStringList steps = editedSteps(text);
                            card->showAnswer(edited ? LC_AIPlanCard::Answer::Edited : LC_AIPlanCard::Answer::Approved, steps);
                            plan = card;
                            planSteps.clear();
                            for (const QString& step : steps) {
                                planSteps << LC_AIPlanStep{step, LC_AIPlanStep::Status::Pending, {}};
                            }
                        }
                    }
                }
                else if (m.toolName == QStringLiteral("update_plan") && !m.result.isError && plan != nullptr) {
                    static const QRegularExpression progress(QStringLiteral("^Step (\\d+) of \\d+ is (in progress|done|skipped|to do)\\."));
                    if (const QRegularExpressionMatch match = progress.match(text); match.hasMatch()) {
                        const int step = match.captured(1).toInt() - 1;
                        const QString status = match.captured(2);
                        while (planSteps.size() <= step) {
                            planSteps << LC_AIPlanStep{};
                        }
                        planSteps[step].status = status == QStringLiteral("in progress") ? LC_AIPlanStep::Status::InProgress
                                                 : status == QStringLiteral("done")      ? LC_AIPlanStep::Status::Done
                                                 : status == QStringLiteral("skipped")   ? LC_AIPlanStep::Status::Skipped
                                                                                         : LC_AIPlanStep::Status::Pending;
                        const QJsonObject args = calls.value(m.toolCallId).arguments;
                        if (args.contains("note")) {
                            planSteps[step].note = args.value("note").toString().trimmed();
                        }
                        plan->setProgress(planSteps);
                    }
                }
                if (!m.stopped) { // a call stopped before it ran keeps its row "Stopped" (finishRun)
                    transcript->toolResult(m);
                }
                break;
            }
        }
        first = false;
    }
    endRun();
    for (const QPointer<LC_AIPlanCard>& card : std::as_const(plans)) {
        if (card != nullptr && !card->isAnswered()) {
            card->setStopped(); // a plan nobody answered
        }
    }
}
