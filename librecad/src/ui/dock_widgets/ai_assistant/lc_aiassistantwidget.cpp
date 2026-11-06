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

#include <QResizeEvent>
#include <QTimer>
#include <QFontMetrics>
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QtMath>
#include <QAction>
#include <QMainWindow>
#include <QTextCursor>

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
#include "lc_aitoolbar.h"
#include "lc_aicontrol.h"
#include "lc_aichattranscript.h"
#include "lc_aichatsession.h"
#include "lc_aicomposerextras.h"
#include "lc_aicredentials.h"
#include "lc_aidrawingchecker.h"
#include "lc_aiicons.h"
#include "lc_aimcpdialog.h"
#include "lc_aimodelpicker.h"
#include "lc_mcpmanager.h"
#include "lc_aiprovidersdialog.h"
#include "lc_aichangepreview.h"
#include "lc_airevertbar.h"
#include "lc_aivoiceinput.h"
#include "lc_aidebugger.h"
#include "lc_aiplanmode.h"

namespace {
    constexpr int MIN_INPUT_HEIGHT = 64; // about two lines
    constexpr int MAX_INPUT_LINES = 12;  // then it scrolls (and never more than a third of the panel)

    /** The model's name, shortened with "…" only when the row has no room for all of it. */
    class ElidingLabel : public QLabel {
    public:
        using QLabel::QLabel;
        QSize sizeHint() const override { return {fontMetrics().horizontalAdvance(text()) + 2, QLabel::sizeHint().height()}; }
        QSize minimumSizeHint() const override { return {fontMetrics().horizontalAdvance(QStringLiteral("Gem…")), QLabel::sizeHint().height()}; }

    protected:
        void paintEvent(QPaintEvent*) override {
            QPainter p(this);
            p.setPen(palette().color(foregroundRole()));
            p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(text(), Qt::ElideRight, width()));
        }
    };
}

LC_AIAssistantWidget::LC_AIAssistantWidget(QWidget* parent)
    : QWidget(parent), m_inference(new LC_AIInferenceManager(this)), m_theme(LC_AITheme::current()) {
    m_tools = LC_AITools::all();
    m_tools << LC_AIToolbar::tools(); // every toolbar button, its dialogs, clicks and selection
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
    newChat->setToolTip(tr("Start a new chat for this drawing (the current one stays in the history)"));
    newChat->setAutoRaise(true);
    header->addWidget(title);
    header->addStretch(1);
    // drawing checker: lists the drawing's problems here, with "Fix all"
    auto* checkDrawing = new QToolButton(this);
    checkDrawing->setIcon(LC_AIIcons::lucideIcon("shield-check", 14, palette().color(QPalette::ButtonText)));
    checkDrawing->setToolTip(tr("Check drawing: duplicates, gaps, open polylines, layers, pens..."));
    checkDrawing->setAutoRaise(true);
    connect(checkDrawing, &QToolButton::clicked, this, [this] { LC_AIDrawingChecker::showInChat(this); });
    header->addWidget(checkDrawing);
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
    // chat history per drawing: the chat is saved, follows the active drawing, and the history button lists it
    m_chats = new LC_AIChatSession(this, m_theme);
    connect(m_chats, &LC_AIChatSession::showMessages, this, &LC_AIAssistantWidget::showChat);
    header->addWidget(m_chats->historyButton());
    header->addWidget(newChat);
    layout->addLayout(header);

    m_transcript = new LC_AIChatTranscript(m_theme, this);
    m_transcript->setLabelLookup([this](const QString& name) { return toolLabel(name); });
    LC_AIBrowser::setHost(m_transcript); // the browser preview floats over the conversation, like Cypher's
    layout->addWidget(m_transcript, 1);
    // checkpoints: "Revert AI changes" under each answer that changed the drawing
    LC_AIRevertBar::addAfterRuns(m_transcript, m_theme);

    // tool calls waiting for approval sit between the conversation and the composer, as in Cypher
    m_approvals = new LC_AIApprovalBox(m_theme, this);
    layout->addWidget(m_approvals);
    layout->addWidget(new LC_AIReviewBar(m_theme, this)); // the change preview's Accept / Reject (lc_aichangepreview.cpp)
    m_inference->setApprover([this](const LC_AIToolCall& call, std::function<void(bool, const QString&)> answer) {
        approve(call, answer);
    });
    connect(&LC_AIControl::instance(), &LC_AIControl::stopRequested, this, [this] {
        if (m_busy) {
            onStop();
        }
    });

    // composer, as in Cypher: the input on top, the model select and the send button below it
    const LC_AITheme& t = m_theme;
    m_composer = new QFrame(this);
    m_composer->setObjectName("composer");
    m_composer->setStyleSheet(QStringLiteral(R"(
#composer { background: %1; border: 1px solid %2; border-radius: 14px; }
#composer QPlainTextEdit { background: transparent; border: none; color: %3; font-size: 13.5px; padding: 0; }
#composer QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
#composer QScrollBar::handle:vertical { background: %2; border-radius: 3px; min-height: 30px; }
#composer QScrollBar::add-line, #composer QScrollBar::sub-line, #composer QScrollBar::add-page, #composer QScrollBar::sub-page { background: none; height: 0; width: 0; }
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

    m_input = new LC_AIComposerInput(m_composer); // takes dropped and pasted files, shows @-mentions as pills
    m_input->setPlaceholderText(tr("Ask anything (Enter to send, Shift+Enter for new line)"));
    QPalette inputPalette = m_input->palette();
    inputPalette.setColor(QPalette::PlaceholderText, t.textPlaceholder);
    inputPalette.setColor(QPalette::Text, t.textPrimary);
    m_input->setPalette(inputPalette);
    m_input->setFixedHeight(MIN_INPUT_HEIGHT);
    m_input->installEventFilter(this);
    connect(m_input, &QPlainTextEdit::textChanged, this, &LC_AIAssistantWidget::updateSendButton);
    connect(m_input, &QPlainTextEdit::textChanged, this, &LC_AIAssistantWidget::fitInputHeight);
    // attachments above the text, paperclip and @ in the actions row, the @ popup over the conversation
    m_extras = new LC_AIComposerExtras(static_cast<LC_AIComposerInput*>(m_input), m_composer, m_theme, this);
    connect(m_extras, &LC_AIComposerExtras::changed, this, &LC_AIAssistantWidget::updateSendButton);
    composerLayout->addWidget(m_extras->strip());
    composerLayout->addWidget(m_input);

    auto* actions = new QHBoxLayout();
    actions->setSpacing(8);
    actions->addWidget(m_extras->buttons());
    m_approvalMode = new LC_AIApprovalModeButton(m_theme, m_composer);
    connect(m_approvalMode, &LC_AIApprovalModeButton::permissionsRequested, this, &LC_AIAssistantWidget::openToolPermissions,
            Qt::QueuedConnection);
    actions->addWidget(m_approvalMode);
    actions->addStretch(1);
    // voice input: the transcript lands in the message box, and is sent at once when the user turned that on
    m_voice = new LC_AIVoiceInput(m_theme, m_input, this, m_composer);
    connect(m_voice, &LC_AIVoiceInput::sendRequested, this, [this] {
        if (!m_busy) {
            onSend();
        }
    });
    connect(m_voice, &LC_AIVoiceInput::failed, this, [this](const QString& error) { m_transcript->addError(error); });
    connect(m_voice, &LC_AIVoiceInput::providersRequested, this, &LC_AIAssistantWidget::openSettings, Qt::QueuedConnection);
    actions->addWidget(m_voice);
    m_modelSelect = new QPushButton(m_composer);
    m_modelSelect->setObjectName("modelSelect");
    m_modelSelect->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed); // as wide as the model's name (fitModelButton)
    m_modelSelect->setCursor(Qt::PointingHandCursor);
    auto* selectLayout = new QHBoxLayout(m_modelSelect);
    selectLayout->setContentsMargins(9, 0, 9, 0);
    selectLayout->setSpacing(8);
    m_modelIcon = new QLabel(m_modelSelect);
    m_modelName = new ElidingLabel(m_modelSelect);
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
    composerLayout->addWidget(m_voice->levelStrip()); // the live level while recording, as in Cypher
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

    // plan mode: "Plan first" next to the approval mode, its plan cards in the conversation and its checklist
    // pinned above it; the debugger checks a run afterwards (lc_aiplanmode, lc_aidebugger)
    m_plan = new LC_AIPlanMode(m_theme, this);
    actions->insertWidget(actions->indexOf(m_approvalMode) + 1, m_plan->toggle());
    layout->insertWidget(layout->indexOf(m_transcript), m_plan->tracker());
    m_transcript->setCardFactory([this](const LC_AIToolCall& call) { return m_plan->cardFor(call); });
    m_debugger = new LC_AIDebugger(m_theme, this);
    connect(m_debugger, &LC_AIDebugger::checkRequested, this, &LC_AIAssistantWidget::startDebugger);

    updateModelButton();
    updateSendButton();
}

void LC_AIAssistantWidget::focusInput() {
    m_input->setFocus();
}

void LC_AIAssistantWidget::startTask(const QString& prompt, const bool send) {
    m_input->setPlainText(prompt);
    QTextCursor cursor = m_input->textCursor();
    cursor.movePosition(QTextCursor::End);
    m_input->setTextCursor(cursor);
    m_input->setFocus();
    if (send) {
        onSend();
    }
}

QList<LC_AITool> LC_AIAssistantWidget::availableTools() const {
    return m_tools + LC_MCPManager::instance().aiTools();
}

void LC_AIAssistantWidget::openWithTask(QWidget* window, const QString& prompt, const bool send) {
    QWidget* top = window != nullptr ? window->window() : nullptr;
    if (top == nullptr) {
        return;
    }
    if (auto* toggle = top->findChild<QAction*>("AIAssistantToggle"); toggle != nullptr && !toggle->isChecked()) {
        toggle->setChecked(true); // opens the sidebar
    }
    if (auto* chat = top->findChild<LC_AIAssistantWidget*>()) {
        chat->startTask(prompt, send);
    }
}

void LC_AIAssistantWidget::fitModelButton() {
    // a narrow panel shows the approval mode and Plan first as icons only (their names stay in the tooltips), leaves out
    // the @ button (typing @ does the same) and packs the row closer, so the model's name keeps its room
    constexpr int COMPACT_BELOW = 460;
    const bool compact = m_composer->width() < COMPACT_BELOW;
    QHBoxLayout* row = nullptr;
    for (QHBoxLayout* l : m_composer->findChildren<QHBoxLayout*>()) {
        if (l->indexOf(m_modelSelect) >= 0) {
            row = l;
        }
    }
    if (m_approvalMode->property("compact").toBool() != compact) {
        m_approvalMode->setProperty("compact", compact);
        m_approvalMode->refresh();
        if (m_plan != nullptr) {
            m_plan->toggle()->setText(compact ? QString() : tr("Plan"));
        }
        if (m_extras != nullptr) {
            if (QWidget* mention = m_extras->buttons()->findChild<QWidget*>("mentionBtn")) {
                mention->setVisible(!compact);
            }
        }
        if (row != nullptr) {
            row->setSpacing(compact ? 3 : 8);
        }
        m_modelIcon->setVisible(!compact); // the name matters more than the provider's logo
    }
    // a push button sizes itself by its own text, not by the labels laid out inside it: give it their width,
    // as far as the row has room next to the other buttons (the name is shortened then)
    const int wanted = m_modelSelect->layout()->sizeHint().width();
    const QMargins margins = m_composer->layout()->contentsMargins();
    int used = 0;
    int shown = 0;
    for (int i = 0; row != nullptr && i < row->count(); ++i) {
        QWidget* w = row->itemAt(i)->widget();
        if (w != nullptr && w != m_modelSelect && !w->isHidden()) {
            used += w->sizeHint().width();
            ++shown;
        }
    }
    const int room = m_composer->width() - margins.left() - margins.right() - used - (row != nullptr ? row->spacing() : 8) * shown;
    // at least 80 px while the row has them, else only the room left (down to its chevron in the narrowest panel, where
    // the voice control gives up its own chevron): an overflowing row drew the voice chevron over the mic and this
    // button over send
    const int least = qBound(40, room, 80);
    m_modelSelect->setFixedWidth(qBound(least, qMin(wanted, room > 0 ? room : wanted), 260));
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
        fitModelButton();
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
    m_modelName->setText(name); // shortened with "…" only when there is no room
    fitModelButton();
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
    const bool valid = !m_input->toPlainText().trimmed().isEmpty() || (m_extras != nullptr && m_extras->hasAttachments());
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
    m_voice->refresh(); // the mic shows at once whether the key it needs is there now
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
    if (m_plan->gate(call, answer)) { // plan mode: its own tools run at once, changes wait for an approved plan
        return;
    }
    const LC_AIPermissions::Decision decision = LC_AIPermissions::decide(call.name, call.arguments, m_sessionAllows);
    const QString name = call.name;
    // an allowed call waits while the user has taken over the drawing
    const auto run = [name, answer] { LC_AIControl::instance().beforeTool(name, [answer] { answer(true, QString()); }); };
    if (decision.permission == LC_AIPermissions::Permission::Allow) {
        run();
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
    m_approvals->add(call, toolLabel(call.name), decision, [this, answer, key, toolName, callId, run](const LC_AIApprovalBox::Choice choice) {
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
        run();
    });
}

void LC_AIAssistantWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    fitInputHeight(); // the limit is a share of the panel's height
    fitModelButton(); // the model's name gets the room the row has
}

void LC_AIAssistantWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // MCP servers start the first time the AI panel is opened, not with OpenCAD
    LC_MCPManager::instance().connectAll();
}

void LC_AIAssistantWidget::fitInputHeight() {
    // the height the box's own layout gives its lines (wrapped at the box's width), plus its frame and margins
    QTextDocument* doc = m_input->document();
    QAbstractTextDocumentLayout* layout = doc->documentLayout();
    qreal textHeight = 0;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        textHeight += layout->blockBoundingRect(block).height();
    }
    const int chrome = m_input->height() - m_input->viewport()->height() + qRound(2 * doc->documentMargin()) + 2;
    const int lines = m_input->fontMetrics().lineSpacing();
    const int maxHeight = qMax(MIN_INPUT_HEIGHT, qMin(MAX_INPUT_LINES * lines + chrome, int(height() * 0.35)));
    const int target = qBound(MIN_INPUT_HEIGHT, qCeil(textHeight) + chrome, maxHeight);
    if (target != m_input->height()) {
        m_input->setFixedHeight(target);
    }
}

bool LC_AIAssistantWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_input && event->type() == QEvent::Resize
        && static_cast<QResizeEvent*>(event)->oldSize().width() != static_cast<QResizeEvent*>(event)->size().width()) {
        QTimer::singleShot(0, this, &LC_AIAssistantWidget::fitInputHeight); // the text wraps differently
    }
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
    if ((text.isEmpty() && !m_extras->hasAttachments()) || m_busy) {
        return;
    }
    LC_AIChatMessage question;
    question.role = LC_AIChatMessage::Role::User;
    question.text = text;
    m_extras->fill(question); // the attached files, and the drawing context the @-mentions stand for
    m_messages.append(question);
    m_runTools = LC_AIControl::withControl(m_tools + LC_MCPManager::instance().aiTools());
    m_inference->setTools(m_runTools);
    prepareRun();
    m_transcript->addUser(text, question.attachments);
    // when the request cannot start, onFailed has already taken the question back out of the history
    // and the text stays in the box, so it can be re-sent after fixing the settings
    if (m_inference->send(m_messages)) {
        m_chats->save(m_messages);
        m_input->clear();
        m_extras->clear();
        setBusy(true);
        m_transcript->setThinking(true);
        // the AI drives the drawing now; the user can look around, take over or stop
        LC_AIControl::instance().begin(qobject_cast<QMainWindow*>(window()));
    }
}

void LC_AIAssistantWidget::onStop() {
    m_inference->cancel();
}

void LC_AIAssistantWidget::onNewChat() {
    m_inference->cancel();
    m_chats->startNew(); // the chat stays in the history; the next question starts a new one
    m_messages.clear();
    m_transcript->clear();
    m_approvals->clear();
    m_sessionAllows.clear(); // "Allow for this session" lasts until a new chat
    m_plan->reset();
    m_runStart = -1;
    m_input->clear();
    m_extras->clear();
    focusInput();
}

void LC_AIAssistantWidget::onDelta(const QString& text) {
    m_transcript->appendDelta(text);
}

void LC_AIAssistantWidget::onMessageAdded(const LC_AIChatMessage& message) {
    m_messages.append(message);
    m_chats->save(m_messages);
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
    m_chats->save(m_messages);
    m_transcript->finishRun();
    setBusy(false);
    LC_AIControl::instance().end();
    afterRun(m_inference->wasCancelled());
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
    if (m_debugging && !m_messages.isEmpty() && m_messages.last().role == LC_AIChatMessage::Role::User) {
        m_messages.removeLast(); // the debugger's request is not the user's text: it does not go back in the box
    }
    // the question stays visible, but is not sent again as an unanswered turn
    if (!m_messages.isEmpty() && m_messages.last().role == LC_AIChatMessage::Role::User) {
        const LC_AIChatMessage failed = m_messages.takeLast();
        const QString question = failed.text;
        if (m_input->toPlainText().trimmed().isEmpty() && !m_extras->hasAttachments()) {
            m_input->setPlainText(question);
            m_extras->restore(failed.attachments); // the files go back with the text
        }
    }
    m_chats->save(m_messages);
    m_approvals->clear();
    m_transcript->finishRun();
    m_transcript->addError(error);
    setBusy(false);
    LC_AIControl::instance().end();
    updateModelButton();
    afterRun(true, true);
}

void LC_AIAssistantWidget::setBusy(const bool busy) {
    m_busy = busy;
    m_modelSelect->setEnabled(!busy);
    m_chats->setRunning(busy);
    updateSendButton();
}

void LC_AIAssistantWidget::showChat(const QList<LC_AIChatMessage>& messages) {
    m_messages = messages;
    m_approvals->clear();
    m_sessionAllows.clear(); // "Allow for this session" answers belong to the chat they were given in
    m_transcript->clear();
    LC_AIChatSession::replay(m_transcript, messages);
    m_plan->reset(); // a plan belongs to the chat it was made in
    m_runStart = -1; // a run of another chat cannot be checked from here
}

// ------------------------------------------------------------------------------------------------ plan mode and debugger

void LC_AIAssistantWidget::prepareRun() {
    m_runStart = int(m_messages.size()) - 1; // the request just added
    m_plan->newRequest();
    m_runTools += m_plan->tools();
    m_inference->setTools(m_runTools);
    m_inference->setPromptAddition(m_plan->promptAddition());
    m_debugger->clearOffer();
}

void LC_AIAssistantWidget::startDebugger(const QString& reason) {
    if (m_busy || m_runStart < 0 || m_runStart >= m_messages.size()) {
        return;
    }
    // the pass goes on in the same conversation, so it sees the request and every call and screenshot of the run
    LC_AIChatMessage review;
    review.role = LC_AIChatMessage::Role::User;
    review.text = LC_AIDebugger::reviewRequest(m_messages, m_runStart, reason);
    m_messages.append(review);
    m_runTools = LC_AIControl::withControl(m_tools + LC_MCPManager::instance().aiTools());
    m_runTools += m_plan->tools();
    m_inference->setTools(m_runTools);
    m_inference->setPromptAddition(LC_AIDebugger::reviewerPrompt(m_runTools));
    m_plan->setReviewing(true);
    m_debugging = true;
    m_debugger->beginPass(m_transcript, reason);
    if (m_inference->send(m_messages)) {
        setBusy(true);
        m_transcript->setThinking(true);
        LC_AIControl::instance().begin(qobject_cast<QMainWindow*>(window()));
    }
}

void LC_AIAssistantWidget::afterRun(const bool stopped, const bool failed) {
    const bool pass = m_debugging;
    m_debugging = false;
    m_plan->setReviewing(false);
    m_plan->runEnded(stopped);
    m_inference->setPromptAddition(QString());
    const LC_AIDebugger::End end = failed ? LC_AIDebugger::End::Failed
                                   : stopped ? LC_AIDebugger::End::Stopped
                                             : LC_AIDebugger::End::Finished;
    m_debugger->runEnded(m_transcript, m_messages, m_runStart, pass, end);
}
