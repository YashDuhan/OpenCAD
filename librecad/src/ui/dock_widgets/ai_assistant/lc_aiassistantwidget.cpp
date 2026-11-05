/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aiassistantwidget.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPainter>
#include <QStyle>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QUrl>
#include <QVariant>
#include <QToolButton>
#include <QVBoxLayout>

#include "lc_aiapprovals.h"
#include "lc_aibrowser.h"
#include "lc_aichattranscript.h"
#include "lc_aicredentials.h"
#include "lc_aiicons.h"
#include "lc_aimcpdialog.h"
#include "lc_aimodelpicker.h"
#include "lc_mcpmanager.h"
#include "lc_aiprovidersdialog.h"

LC_AIAssistantWidget::LC_AIAssistantWidget(QWidget* parent)
    : QWidget(parent), m_inference(new LC_AIInferenceManager(this)), m_theme(LC_AITheme::current()) {
    m_tools = LC_AITools::all();
    // ZeroDrive's requestClarification: the AI asks you one question and waits for the answer
    LC_AITool ask;
    ask.name = "request_clarification";
    ask.label = tr("Question");
    ask.description = "Pause and ask the user one necessary, specific question before continuing, optionally with 2-8 "
                      "choices to pick from (they can also type their own answer). Use only when the task cannot "
                      "safely proceed without the answer, e.g. which project, units or dimensions to use. Do not ask "
                      "what you can find out with the other tools.";
    ask.parameters = QJsonObject{
        {"type", "object"},
        {"properties", QJsonObject{{"question", QJsonObject{{"type", "string"}, {"description", "The question, one at a time"}}},
                                   {"choices", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}},
                                                           {"description", "2 to 8 short answers to pick from (optional)"}}}}},
        {"required", QJsonArray{"question"}}};
    ask.executeAsync = [this](const QJsonObject& args, std::function<void(const LC_AIToolResult&)> done) {
        if (args.value("question").toString().trimmed().isEmpty()) {
            done(LC_AIToolResult::error(tr("question must be a non-empty string.")));
            return;
        }
        m_transcript->activateClarification([done](const QString& answer) {
            done(LC_AIToolResult::text(QString::fromUtf8(QJsonDocument(QJsonObject{{"answer", answer}}).toJson(QJsonDocument::Compact))));
        });
    };
    m_tools << ask;
    m_tools << LC_AIBrowser::tools(); // web lookups, when built with Qt WebEngine
    m_runTools = m_tools;
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel(tr("<b>AI Assistant</b>"), this);
    auto* providers = new QToolButton(this);
    providers->setIcon(LC_AIIcons::lucideIcon("key-round", 14, palette().color(QPalette::ButtonText)));
    providers->setToolTip(tr("Providers: add API keys"));
    providers->setAutoRaise(true);
    auto* newChat = new QToolButton(this);
    newChat->setText(tr("New Chat"));
    newChat->setToolTip(tr("Clear the conversation"));
    newChat->setAutoRaise(true);
    header->addWidget(title);
    header->addStretch(1);
    auto* mcp = new QToolButton(this);
    mcp->setIcon(LC_AIIcons::lucideIcon("plug", 14, palette().color(QPalette::ButtonText)));
    mcp->setToolTip(tr("MCP servers: more tools for the AI"));
    mcp->setAutoRaise(true);
    connect(mcp, &QToolButton::clicked, this, &LC_AIAssistantWidget::openMcp);
    if (LC_AIBrowser::available()) {
        auto* browser = new QToolButton(this);
        browser->setIcon(LC_AIIcons::lucideIcon("globe", 14, palette().color(QPalette::ButtonText)));
        browser->setToolTip(tr("AI Browser: the window the AI uses for web lookups"));
        browser->setAutoRaise(true);
        connect(browser, &QToolButton::clicked, this, [this] { LC_AIBrowser::showWindow(this); });
        header->addWidget(browser);
    }
    header->addWidget(mcp);
    header->addWidget(providers);
    header->addWidget(newChat);
    layout->addLayout(header);

    m_transcript = new LC_AIChatTranscript(m_theme, this);
    m_transcript->setLabelLookup([this](const QString& name) { return toolLabel(name); });
    LC_AIBrowser::setHost(m_transcript); // the browser preview floats over the conversation, like Cypher's
    layout->addWidget(m_transcript, 1);

    // tool calls waiting for approval sit between the conversation and the composer, as in Cypher
    m_approvals = new LC_AIApprovalBox(m_theme, this);
    layout->addWidget(m_approvals);
    m_inference->setApprover([this](const LC_AIToolCall& call, std::function<void(bool, const QString&)> answer) {
        approve(call, answer);
    });

    // composer, as in Cypher: the input on top, the model select and the send button below it
    const LC_AITheme& t = m_theme;
    m_composer = new QFrame(this);
    m_composer->setObjectName("composer");
    m_composer->setStyleSheet(QStringLiteral(R"(
#composer { background: %1; border: 1px solid %2; border-radius: 14px; }
#composer QPlainTextEdit { background: transparent; border: none; color: %3; font-size: 13.5px; padding: 0; }
#modelSelect { border: none; border-radius: 8px; background: transparent; padding: 0 9px; min-height: 32px; max-height: 32px; text-align: left; }
#modelSelect:hover { background: %4; }
#modelSelect QLabel { background: transparent; color: %3; font-size: 12px; }
#sendBtn { border: none; border-radius: 8px; min-width: 32px; max-width: 32px; min-height: 32px; max-height: 32px; background: %5; }
#sendBtn[state="valid"] { background: %6; }
#sendBtn[state="stop"] { background: %7; }
)").arg(t.bgElevated.name(), t.borderSubtle.name(QColor::HexArgb), t.textPrimary.name(), LC_AITheme::current().bgHover.name(QColor::HexArgb),
         t.bgActive.name(QColor::HexArgb), t.textHeading.name(), t.dark ? QColor(255, 255, 255, 23).name(QColor::HexArgb)
                                                                   : QColor(0, 0, 0, 20).name(QColor::HexArgb)));
    auto* composerLayout = new QVBoxLayout(m_composer);
    composerLayout->setContentsMargins(10, 10, 10, 10);
    composerLayout->setSpacing(8);

    m_input = new QPlainTextEdit(m_composer);
    m_input->setPlaceholderText(tr("Ask anything (Enter to send, Shift+Enter for new line)"));
    QPalette inputPalette = m_input->palette();
    inputPalette.setColor(QPalette::PlaceholderText, t.textPlaceholder);
    inputPalette.setColor(QPalette::Text, t.textPrimary);
    m_input->setPalette(inputPalette);
    m_input->setFixedHeight(64);
    m_input->installEventFilter(this);
    connect(m_input, &QPlainTextEdit::textChanged, this, &LC_AIAssistantWidget::updateSendButton);
    composerLayout->addWidget(m_input);

    auto* actions = new QHBoxLayout();
    actions->setSpacing(8);
    m_approvalMode = new LC_AIApprovalModeButton(m_theme, m_composer);
    connect(m_approvalMode, &LC_AIApprovalModeButton::permissionsRequested, this, &LC_AIAssistantWidget::openToolPermissions,
            Qt::QueuedConnection);
    actions->addWidget(m_approvalMode);
    actions->addStretch(1);
    m_modelSelect = new QPushButton(m_composer);
    m_modelSelect->setObjectName("modelSelect");
    m_modelSelect->setMinimumWidth(96);
    m_modelSelect->setMaximumWidth(160);
    m_modelSelect->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_modelSelect->setCursor(Qt::PointingHandCursor);
    auto* selectLayout = new QHBoxLayout(m_modelSelect);
    selectLayout->setContentsMargins(9, 0, 9, 0);
    selectLayout->setSpacing(8);
    m_modelIcon = new QLabel(m_modelSelect);
    m_modelName = new QLabel(m_modelSelect);
    auto* chevron = new QLabel(m_modelSelect);
    QColor chevronColor = t.textPrimary;
    chevronColor.setAlphaF(0.4);
    chevron->setPixmap(LC_AIIcons::lucide("chevron-down", 12, chevronColor, devicePixelRatioF()));
    for (QLabel* l : {m_modelIcon, m_modelName, chevron}) {
        l->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    selectLayout->addWidget(m_modelIcon);
    selectLayout->addWidget(m_modelName, 1);
    selectLayout->addWidget(chevron);
    actions->addWidget(m_modelSelect);

    m_sendButton = new QToolButton(m_composer);
    m_sendButton->setObjectName("sendBtn");
    m_sendButton->setIconSize(QSize(16, 16));
    m_sendButton->setCursor(Qt::PointingHandCursor);
    actions->addWidget(m_sendButton);
    composerLayout->addLayout(actions);
    layout->addWidget(m_composer);

    connect(m_sendButton, &QToolButton::clicked, this, [this] { m_busy ? onStop() : onSend(); });
    connect(newChat, &QToolButton::clicked, this, &LC_AIAssistantWidget::onNewChat);
    connect(providers, &QToolButton::clicked, this, &LC_AIAssistantWidget::openSettings);
    connect(m_modelSelect, &QPushButton::clicked, this, &LC_AIAssistantWidget::openModelPicker);
    connect(m_inference, &LC_AIInferenceManager::delta, this, &LC_AIAssistantWidget::onDelta);
    connect(m_inference, &LC_AIInferenceManager::messageAdded, this, &LC_AIAssistantWidget::onMessageAdded);
    connect(m_inference, &LC_AIInferenceManager::toolStarted, this, &LC_AIAssistantWidget::onToolStarted);
    connect(m_inference, &LC_AIInferenceManager::retrying, this,
            [this](const int attempt, const int max, const int seconds, const QString& reason) {
                QString shortReason = reason.section('\n', 0, 0);
                if (shortReason.size() > 90) {
                    shortReason = shortReason.left(87) + "...";
                }
                m_transcript->showRetry(tr("%1 Retrying in %2 s (attempt %3 of %4)...").arg(shortReason).arg(seconds).arg(attempt).arg(max));
            });
    connect(m_inference, &LC_AIInferenceManager::finished, this, &LC_AIAssistantWidget::onFinished);
    connect(m_inference, &LC_AIInferenceManager::failed, this, &LC_AIAssistantWidget::onFailed);

    updateModelButton();
    updateSendButton();
}

void LC_AIAssistantWidget::focusInput() {
    m_input->setFocus();
}

void LC_AIAssistantWidget::updateModelButton() {
    LC_AICredentials& creds = LC_AICredentials::instance();
    const QString id = creds.selectedProvider();
    const qreal dpr = devicePixelRatioF();
    if (!creds.isConnected(id)) { // nothing usable yet: grey dot + "Select", like Cypher without a model
        QPixmap dot(QSize(8, 8) * dpr);
        dot.fill(Qt::transparent);
        dot.setDevicePixelRatio(dpr);
        QPainter painter(&dot);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_theme.textFaint);
        painter.drawEllipse(QRectF(0, 0, 8, 8));
        painter.end();
        m_modelIcon->setPixmap(dot);
        m_modelName->setText(tr("Select"));
        m_modelName->setStyleSheet(QStringLiteral("color: %1;").arg(m_theme.textFaint.name()));
        m_modelSelect->setToolTip(tr("Choose a model"));
        return;
    }
    const QString model = creds.model(id);
    QString name = model;
    for (const LC_AIGenerated::ModelEntry* m : LC_AIProvider::catalog(id)) {
        if (model == m->id) {
            name = QString::fromUtf8(m->name);
            break;
        }
    }
    m_modelIcon->setPixmap(LC_AIIcons::provider(id, 13, m_theme.textPrimary, dpr));
    m_modelName->setStyleSheet(QString());
    m_modelName->setText(m_modelName->fontMetrics().elidedText(name, Qt::ElideRight, 105));
    m_modelSelect->setToolTip(id + "/" + model);
}

void LC_AIAssistantWidget::openModelPicker() {
    auto* picker = new LC_AIModelPicker(m_theme, this);
    connect(picker, &LC_AIModelPicker::picked, this, [this] {
        updateModelButton();
        focusInput();
    });
    connect(picker, &LC_AIModelPicker::providersRequested, this, &LC_AIAssistantWidget::openSettings,
            Qt::QueuedConnection);
    picker->popup(m_modelSelect);
}

void LC_AIAssistantWidget::updateSendButton() {
    // Cypher's .send-btn: faint when empty, solid when there is text, Stop while a reply streams
    const bool valid = !m_input->toPlainText().trimmed().isEmpty();
    const QString state = m_busy ? "stop" : valid ? "valid" : "idle";
    if (m_sendButton->property("state").toString() != state) {
        m_sendButton->setProperty("state", state);
        m_sendButton->style()->unpolish(m_sendButton);
        m_sendButton->style()->polish(m_sendButton);
    }
    if (m_busy) {
        m_sendButton->setIcon(LC_AIIcons::lucideIcon("square", 16, m_theme.textHeading, true));
        m_sendButton->setToolTip(tr("Stop"));
    }
    else {
        m_sendButton->setIcon(LC_AIIcons::lucideIcon("send-horizontal", 16, valid ? m_theme.bgRoot : m_theme.textFaint));
        m_sendButton->setToolTip(tr("Send (Enter)"));
    }
}

void LC_AIAssistantWidget::openSettings() {
    LC_AIProvidersDialog dialog(window());
    dialog.exec();
    updateModelButton();
    focusInput();
}

void LC_AIAssistantWidget::openMcp() {
    LC_AIMcpDialog dialog(window());
    dialog.exec();
    focusInput();
}

void LC_AIAssistantWidget::openToolPermissions() {
    LC_AIToolPermissionsDialog dialog(m_tools + LC_MCPManager::instance().aiTools(), m_theme, window());
    dialog.exec();
}

/** Cypher's tool_call hook: allow, deny, or ask in the approval box and wait for the answer. */
void LC_AIAssistantWidget::approve(const LC_AIToolCall& call, const std::function<void(bool, const QString&)>& answer) {
    const LC_AIPermissions::Decision decision = LC_AIPermissions::decide(call.name, call.arguments, m_sessionAllows);
    if (decision.permission == LC_AIPermissions::Permission::Allow) {
        answer(true, QString());
        return;
    }
    if (decision.permission == LC_AIPermissions::Permission::Deny) {
        answer(false, decision.reason);
        return;
    }
    m_transcript->toolAwaitingApproval(call.id);
    const QString key = decision.key;
    const QString toolName = call.name;
    const QString callId = call.id;
    m_approvals->add(call, toolLabel(call.name), decision, [this, answer, key, toolName, callId](const LC_AIApprovalBox::Choice choice) {
        switch (choice) {
            case LC_AIApprovalBox::Choice::Session:
                m_sessionAllows.insert(key);
                break;
            case LC_AIApprovalBox::Choice::Always:
                LC_AIPermissions::setPermission(toolName, LC_AIPermissions::Permission::Allow);
                break;
            case LC_AIApprovalBox::Choice::Deny:
                answer(false, tr("The user denied this tool call. Stop and ask how to proceed."));
                return;
            case LC_AIApprovalBox::Choice::Once:
                break;
        }
        m_transcript->toolStarted({callId, toolName, {}, {}});
        answer(true, QString());
    });
}

void LC_AIAssistantWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // MCP servers start the first time the AI panel is opened, not with OpenCAD
    LC_MCPManager::instance().connectAll();
}

bool LC_AIAssistantWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        const auto* keyEvent = static_cast<QKeyEvent*>(event);
        if ((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter)
            && !(keyEvent->modifiers() & Qt::ShiftModifier)) {
            if (!m_busy) {
                onSend();
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void LC_AIAssistantWidget::onSend() {
    const QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty() || m_busy) {
        return;
    }
    LC_AIChatMessage question;
    question.role = LC_AIChatMessage::Role::User;
    question.text = text;
    m_messages.append(question);
    m_runTools = m_tools + LC_MCPManager::instance().aiTools();
    m_inference->setTools(m_runTools);
    m_transcript->addUser(text);
    // when the request cannot start, onFailed has already taken the question back out of the history
    // and the text stays in the box, so it can be re-sent after fixing the settings
    if (m_inference->send(m_messages)) {
        m_input->clear();
        setBusy(true);
        m_transcript->setThinking(true);
    }
}

void LC_AIAssistantWidget::onStop() {
    m_inference->cancel();
}

void LC_AIAssistantWidget::onNewChat() {
    m_inference->cancel();
    m_messages.clear();
    m_transcript->clear();
    m_approvals->clear();
    m_sessionAllows.clear(); // "Allow for this session" lasts until a new chat
    m_input->clear();
    focusInput();
}

void LC_AIAssistantWidget::onDelta(const QString& text) {
    m_transcript->appendDelta(text);
}

void LC_AIAssistantWidget::onMessageAdded(const LC_AIChatMessage& message) {
    m_messages.append(message);
    if (message.role == LC_AIChatMessage::Role::Assistant) {
        m_transcript->addAssistant(message);
    }
    else {
        m_transcript->toolResult(message);
        m_transcript->setThinking(true); // the model reads the results next (a following tool clears it)
    }
}

void LC_AIAssistantWidget::onToolStarted(const LC_AIToolCall& call) {
    m_transcript->toolStarted(call);
}

void LC_AIAssistantWidget::onFinished() {
    m_approvals->clear(); // a stopped run leaves nothing to approve
    if (!m_messages.isEmpty() && m_messages.last().role == LC_AIChatMessage::Role::User) {
        m_messages.removeLast(); // stopped before any reply: drop the unanswered question from the history
    }
    m_transcript->finishRun();
    setBusy(false);
}

QString LC_AIAssistantWidget::toolLabel(const QString& name) const {
    for (const LC_AITool& t : m_runTools) {
        if (t.name == name) {
            return t.label;
        }
    }
    return name;
}

void LC_AIAssistantWidget::onFailed(const QString& error) {
    // the question stays visible, but is not sent again as an unanswered turn
    if (!m_messages.isEmpty() && m_messages.last().role == LC_AIChatMessage::Role::User) {
        const QString question = m_messages.takeLast().text;
        if (m_input->toPlainText().trimmed().isEmpty()) {
            m_input->setPlainText(question);
        }
    }
    m_approvals->clear();
    m_transcript->finishRun();
    m_transcript->addError(error);
    setBusy(false);
    updateModelButton();
}

void LC_AIAssistantWidget::setBusy(const bool busy) {
    m_busy = busy;
    m_modelSelect->setEnabled(!busy);
    updateSendButton();
}
