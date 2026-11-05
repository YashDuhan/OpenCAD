/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aidebugger.h"

#include <QHBoxLayout>
#include <QHash>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>

#include "lc_aichattranscript.h"
#include "lc_aicredentials.h"
#include "lc_aiicons.h"
#include "lc_aiplanmode.h"

namespace {
    const QColor BLUE("#4c8dff");
    const QColor DONE("#22c55e");
    const QColor RED("#f87171");
    const QString SETTING_KEY = QStringLiteral("checkWorkAutomatically");
    constexpr int MAX_DIGEST_CALLS = 40;  // the last ones; the full calls are in the conversation anyway
    constexpr int MAX_ARGS_CHARS = 160;
    constexpr int MAX_RESULT_CHARS = 220;

    // the read tools the reviewer inspects with, in the order it should reach for them (some may not exist)
    const QStringList REVIEW_TOOLS = {"check_drawing", "capture_view", "list_entities", "get_drawing_info", "look_at_app"};

    QString rgba(const QColor& c, const double alpha) {
        return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha);
    }

    QString oneLine(QString text, const int max) {
        text.replace('\n', QStringLiteral(" / "));
        text = text.simplified();
        return text.size() > max ? text.left(max - 1) + QString::fromUtf8("…") : text;
    }

    /** Refusals that are not mistakes of the run: the user said no, or plan mode held a change back. */
    bool refused(const QString& text) {
        static const QRegularExpression byUser(QStringLiteral("user denied this tool call|denied by the user's tool permissions"));
        return byUser.match(text).hasMatch() || text.startsWith(QStringLiteral("Plan mode:"));
    }

    /** Calls that are about the conversation, not the drawing. */
    bool bookkeeping(const QString& toolName) {
        return LC_AIPlanMode::isPlanTool(toolName) || toolName == "request_clarification";
    }
}

// ------------------------------------------------------------------------------------------------ section

LC_AIDebuggerSection::LC_AIDebuggerSection(const QString& reason, const LC_AITheme& theme, QWidget* parent)
    : QFrame(parent), m_theme(theme) {
    setObjectName("debugSection");
    // only the frame and the header: the text and tool calls inside keep the transcript's own look
    setStyleSheet(QStringLiteral(R"(
#debugSection { background: %1; border: 1px solid %2; border-radius: 10px; }
#debugTitle { font-size: 12.5px; font-weight: 600; color: %3; }
#debugReason { font-size: 11.5px; color: %4; }
#debugStatus { font-size: 12px; }
#debugRule { background: %2; border: none; }
)")
                      .arg(rgba(BLUE, theme.dark ? 0.035 : 0.03), rgba(BLUE, 0.3), theme.textHeading.name(), theme.textMuted.name()));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 4);
    layout->setSpacing(0);

    auto* header = new QWidget(this);
    auto* head = new QHBoxLayout(header);
    head->setContentsMargins(10, 6, 10, 6);
    head->setSpacing(7);
    auto* icon = new QLabel(header);
    icon->setPixmap(LC_AIIcons::lucide("bug", 13, BLUE, devicePixelRatioF()));
    auto* title = new QLabel(tr("Debugger"), header);
    title->setObjectName("debugTitle");
    auto* why = new QLabel(reason, header);
    why->setObjectName("debugReason");
    why->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    why->setToolTip(tr("The debugger reviews the run against your request, inspects the drawing, fixes what is wrong "
                       "and ends with a verdict."));
    m_statusIcon = new LC_AIStatusIcon(theme, 12, header);
    m_statusIcon->setSpinning(BLUE);
    m_status = new QLabel(header);
    m_status->setObjectName("debugStatus");
    head->addWidget(icon);
    head->addWidget(title);
    head->addWidget(why, 1);
    head->addWidget(m_statusIcon);
    head->addWidget(m_status);
    layout->addWidget(header);
    auto* rule = new QFrame(this);
    rule->setObjectName("debugRule");
    rule->setFixedHeight(1);
    layout->addWidget(rule);

    m_body = new QWidget(this);
    auto* body = new QVBoxLayout(m_body);
    body->setContentsMargins(6, 4, 6, 2);
    body->setSpacing(4);
    layout->addWidget(m_body);
    setStatus(tr("Checking"), BLUE);
}

void LC_AIDebuggerSection::setStatus(const QString& text, const QColor& color) {
    m_status->setText(text);
    m_status->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
}

void LC_AIDebuggerSection::setDone() {
    m_statusIcon->setIcon("check", DONE);
    setStatus(tr("Done"), DONE);
}

void LC_AIDebuggerSection::setStopped() {
    m_statusIcon->setIcon("square", m_theme.textFaint);
    setStatus(tr("Stopped"), m_theme.textFaint);
}

void LC_AIDebuggerSection::setFailed() {
    m_statusIcon->setIcon("circle-alert", RED);
    setStatus(tr("Failed"), RED);
}

// ------------------------------------------------------------------------------------------------ debugger

LC_AIDebugger::LC_AIDebugger(const LC_AITheme& theme, QObject* parent)
    : QObject(parent), m_theme(theme) {
}

bool LC_AIDebugger::autoCheck() {
    return LC_AICredentials::instance().setting(SETTING_KEY).toBool(false);
}

void LC_AIDebugger::setAutoCheck(const bool on) {
    LC_AICredentials::instance().setSetting(SETTING_KEY, on);
}

LC_AIDebugger::Report LC_AIDebugger::inspect(const QList<LC_AIChatMessage>& history, const int start) {
    Report report;
    for (int i = qMax(0, start); i < history.size(); ++i) {
        const LC_AIChatMessage& m = history[i];
        if (m.role != LC_AIChatMessage::Role::ToolResult || bookkeeping(m.toolName)) {
            continue;
        }
        ++report.toolCalls;
        if (m.result.isError && !refused(m.result.joinedText())) {
            ++report.failures;
        }
    }
    return report;
}

QString LC_AIDebugger::reviewerPrompt(const QList<LC_AITool>& tools) {
    QStringList readTools;
    for (const QString& name : REVIEW_TOOLS) {
        for (const LC_AITool& t : tools) {
            if (t.name == name) {
                readTools << name;
                break;
            }
        }
    }
    if (readTools.isEmpty()) {
        readTools << "capture_view" << "list_entities" << "get_drawing_info";
    }
    return QStringLiteral(
               "DEBUGGER PASS. You are now the reviewer of the run above, not its author: assume it has mistakes until you "
               "have checked.\n"
               "1. Read the user's request again, and the run's tool calls and results (the last message lists them, failed "
               "ones marked).\n"
               "2. Inspect the drawing as it is now with the read tools: %1. Measure rather than guess: compare exact "
               "coordinates, sizes, counts, layers and texts with what was asked.\n"
               "3. List the concrete problems you found, one short line each: wrong sizes or positions, missing parts, "
               "overlaps or duplicates, leftover construction geometry, wrong layers, steps that failed or were skipped. "
               "If there are none, say so.\n"
               "4. Fix each problem with the normal tools, then inspect again to confirm the fix.\n"
               "5. End with a short verdict: one line starting with \"Verdict:\" that says what was wrong, what you fixed "
               "and anything you could not fix.\n"
               "Do not redo work that is already right, do not undo changes the user made, and do not start work that was "
               "not asked for. Keep your text short.")
        .arg(readTools.join(", "));
}

QString LC_AIDebugger::reviewRequest(const QList<LC_AIChatMessage>& history, const int start, const QString& reason) {
    const QString request = start >= 0 && start < history.size() ? history[start].text.trimmed() : QString();
    QHash<QString, LC_AIToolCall> calls;
    QStringList lines;
    int total = 0;
    int failed = 0;
    for (int i = qMax(0, start + 1); i < history.size(); ++i) {
        const LC_AIChatMessage& m = history[i];
        if (m.role == LC_AIChatMessage::Role::Assistant) {
            for (const LC_AIToolCall& c : m.toolCalls) {
                calls.insert(c.id, c);
            }
            continue;
        }
        if (m.role != LC_AIChatMessage::Role::ToolResult || m.toolName == "update_plan") {
            continue; // the plan's ticks say nothing about the drawing
        }
        const LC_AIToolCall call = calls.value(m.toolCallId);
        const QString args = oneLine(QString::fromUtf8(QJsonDocument(call.arguments).toJson(QJsonDocument::Compact)), MAX_ARGS_CHARS);
        const QString text = m.result.joinedText();
        const bool error = m.result.isError && !refused(text);
        ++total;
        failed += error ? 1 : 0;
        lines << QStringLiteral("%1. %2 %3 -> %4: %5")
                     .arg(total)
                     .arg(m.toolName, args, error ? QStringLiteral("FAILED") : m.result.isError ? QStringLiteral("refused") : QStringLiteral("ok"),
                          oneLine(text.isEmpty() ? QStringLiteral("(no text)") : text, MAX_RESULT_CHARS));
    }
    QString digest;
    if (lines.size() > MAX_DIGEST_CALLS) {
        digest = QStringLiteral("(%1 earlier calls left out)\n").arg(lines.size() - MAX_DIGEST_CALLS);
        lines = lines.mid(lines.size() - MAX_DIGEST_CALLS);
    }
    digest += lines.isEmpty() ? QStringLiteral("(no tool calls)") : lines.join('\n');
    return QStringLiteral("[Debugger] Check and fix the work of the last run (%1).\n\n"
                          "The user's request:\n\"\"\"\n%2\n\"\"\"\n\n"
                          "What the run did: %3 tool calls, %4 failed.\n%5\n\n"
                          "Inspect the drawing now, list the problems you find, fix them, and end with \"Verdict: ...\".")
        .arg(reason, request.isEmpty() ? QStringLiteral("(not available)") : request, QString::number(total),
             QString::number(failed), digest); // in one go: a "%1" in the request stays as it is
}

void LC_AIDebugger::clearOffer() {
    if (m_offer != nullptr) {
        m_offer->hide();
        m_offer->deleteLater();
    }
    m_offer = nullptr;
}

void LC_AIDebugger::beginPass(LC_AIChatTranscript* transcript, const QString& reason) {
    clearOffer();
    auto* section = new LC_AIDebuggerSection(reason, m_theme);
    transcript->openSection(section, section->body());
    m_section = section;
}

void LC_AIDebugger::runEnded(LC_AIChatTranscript* transcript, const QList<LC_AIChatMessage>& history, const int start,
                             const bool wasPass, const End end) {
    if (wasPass) {
        transcript->closeSection();
        if (m_section != nullptr) {
            switch (end) {
                case End::Finished:
                    m_section->setDone();
                    break;
                case End::Stopped:
                    m_section->setStopped();
                    break;
                case End::Failed:
                    m_section->setFailed();
                    break;
            }
        }
        m_section = nullptr;
        if (end == End::Finished) {
            offer(transcript, true); // never on its own again: no loops of checks
        }
        return;
    }
    if (end != End::Finished) {
        return; // stopped or failed: the user decides what happens next
    }
    const Report report = inspect(history, start);
    if (report.toolCalls == 0) {
        return; // an answer without tools: nothing in the drawing to check
    }
    if (report.failures > 0 || autoCheck()) {
        const QString reason = report.failures == 1 ? tr("a tool call failed")
                               : report.failures > 1 ? tr("%1 tool calls failed").arg(report.failures)
                                                     : tr("checking automatically");
        // once the finished run has been wound up
        QTimer::singleShot(0, this, [this, reason] { emit checkRequested(reason); });
        return;
    }
    offer(transcript, false);
}

void LC_AIDebugger::offer(LC_AIChatTranscript* transcript, const bool again) {
    clearOffer();
    const LC_AITheme& t = m_theme;
    auto* row = new QWidget();
    row->setObjectName("debugOffer");
    row->setStyleSheet(QStringLiteral(R"(
QPushButton#debugCheck { border: 1px solid %1; border-radius: 7px; padding: 0 9px; min-height: 24px; max-height: 24px;
                         font-size: 12px; color: %2; background: transparent; }
QPushButton#debugCheck:hover { background: %3; color: %4; }
QPushButton#debugAuto { border: none; border-radius: 7px; padding: 0 7px; min-height: 24px; max-height: 24px; font-size: 12px;
                        color: %5; background: transparent; }
QPushButton#debugAuto:hover { background: %3; }
QPushButton#debugAuto:checked { color: %6; }
)")
                           .arg(t.borderDefault.name(QColor::HexArgb), t.textSecondary.name(), t.bgHover.name(QColor::HexArgb),
                                t.textHeading.name(), t.textMuted.name(), BLUE.name()));
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(2, 0, 0, 6);
    layout->setSpacing(6);
    auto* check = new QPushButton(again ? tr("Check again") : tr("Check && fix"), row); // && shows one &
    check->setObjectName("debugCheck");
    check->setIcon(LC_AIIcons::lucideIcon("bug", 13, t.textSecondary));
    check->setIconSize(QSize(13, 13));
    check->setCursor(Qt::PointingHandCursor);
    check->setToolTip(tr("Have the debugger review this work: it inspects the drawing, lists what is wrong and fixes it"));
    auto* automatic = new QPushButton(tr("Check my work automatically"), row);
    automatic->setObjectName("debugAuto");
    automatic->setCheckable(true);
    automatic->setChecked(autoCheck());
    automatic->setIconSize(QSize(13, 13));
    automatic->setCursor(Qt::PointingHandCursor);
    automatic->setToolTip(tr("Run the debugger after every run that used tools (it always runs when a tool call failed)"));
    const auto refresh = [automatic, t] {
        automatic->setIcon(LC_AIIcons::lucideIcon(automatic->isChecked() ? "circle-check" : "circle", 13,
                                                  automatic->isChecked() ? BLUE : t.textMuted));
    };
    refresh();
    connect(automatic, &QPushButton::toggled, row, [refresh](const bool on) {
        setAutoCheck(on);
        refresh();
    });
    connect(check, &QPushButton::clicked, this, [this] {
        clearOffer();
        emit checkRequested(tr("you asked"));
    });
    layout->addWidget(check);
    layout->addWidget(automatic);
    layout->addStretch(1);
    transcript->addWidget(row);
    m_offer = row;
}
