/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aichattranscript.h"

#include <QDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include "lc_aiicons.h"

namespace {
    constexpr int MAX_PRE_CHARS = 6000;
    constexpr int THUMBNAIL_HEIGHT = 120;
    const QColor STATUS_RUNNING("#4c8dff"); // --accent-blue
    const QColor STATUS_DONE("#22c55e");    // --accent-green-icon
    const QColor STATUS_ERROR("#f87171");   // --accent-red

    QString mono() {
        return QStringLiteral("\"JetBrains Mono Variable\", \"JetBrains Mono\", \"JetBrainsMono Nerd Font\", monospace");
    }

    constexpr int RENDER_MS = 70;          // streamed text is shown at most this often
    constexpr int FREEZE_CHARS = 700;      // a streamed block this long hands its finished paragraphs on
    constexpr int FOLLOW_SLACK = 24;       // px from the bottom that still counts as "at the bottom"

    QString superOrSub(const QString& text, const bool super) {
        static const QString from = QStringLiteral("0123456789+-=()n");
        static const QString sup = QString::fromUtf8("⁰¹²³⁴⁵⁶⁷⁸⁹⁺⁻⁼⁽⁾ⁿ");
        static const QString sub = QString::fromUtf8("₀₁₂₃₄₅₆₇₈₉₊₋₌₍₎ₙ");
        QString out;
        for (const QChar c : text) {
            const int i = from.indexOf(c);
            if (i < 0) {
                return {};
            }
            out += (super ? sup : sub).at(i);
        }
        return out;
    }

    /** LaTeX math, as models like to write it, turned into plain text with Unicode symbols. */
    QString latexToText(QString t) {
        static const QRegularExpression wrapped(QStringLiteral(R"(\\(?:text|mathrm|textrm|textbf|mathbf|mathit|textit|operatorname|mbox|mathsf|texttt)\s*\{([^{}]*)\})"));
        static const QRegularExpression frac(QStringLiteral(R"(\\[dt]?frac\s*\{([^{}]*)\}\s*\{([^{}]*)\})"));
        static const QRegularExpression sqrt(QStringLiteral(R"(\\sqrt\s*\{([^{}]*)\})"));
        static const QRegularExpression script(QStringLiteral(R"(([\^_])(?:\{([^{}]*)\}|([^\s\\{}])))")); // ^{..} _{..} ^x _x
        static const QRegularExpression command(QStringLiteral(R"(\\([A-Za-z]+))"));
        static const QList<QPair<QString, QString>> symbols = {
            {"varnothing", "⌀"}, {"emptyset", "⌀"}, {"diameter", "⌀"}, {"O", "⌀"}, {"times", "×"}, {"cdot", "·"},
            {"pm", "±"}, {"mp", "∓"}, {"leq", "≤"}, {"le", "≤"}, {"geq", "≥"}, {"ge", "≥"}, {"neq", "≠"}, {"ne", "≠"},
            {"approx", "≈"}, {"simeq", "≃"}, {"sim", "~"}, {"equiv", "≡"}, {"infty", "∞"}, {"degree", "°"}, {"circ", "°"},
            {"to", "→"}, {"rightarrow", "→"}, {"leftarrow", "←"}, {"Rightarrow", "⇒"}, {"leftrightarrow", "↔"},
            {"propto", "∝"}, {"sum", "Σ"}, {"prod", "Π"}, {"int", "∫"}, {"partial", "∂"}, {"nabla", "∇"}, {"angle", "∠"},
            {"parallel", "∥"}, {"perp", "⊥"}, {"div", "÷"}, {"ldots", "…"}, {"dots", "…"}, {"cdots", "…"},
            {"alpha", "α"}, {"beta", "β"}, {"gamma", "γ"}, {"Gamma", "Γ"}, {"delta", "δ"}, {"Delta", "Δ"},
            {"epsilon", "ε"}, {"varepsilon", "ε"}, {"eta", "η"}, {"theta", "θ"}, {"Theta", "Θ"}, {"lambda", "λ"},
            {"Lambda", "Λ"}, {"mu", "µ"}, {"pi", "π"}, {"Pi", "Π"}, {"rho", "ρ"}, {"sigma", "σ"}, {"Sigma", "Σ"},
            {"tau", "τ"}, {"phi", "φ"}, {"varphi", "φ"}, {"Phi", "Φ"}, {"psi", "ψ"}, {"omega", "ω"}, {"Omega", "Ω"},
            {"quad", " "}, {"qquad", " "}, {"left", ""}, {"right", ""}, {"displaystyle", ""}, {"limits", ""}};
        static const QRegularExpression group(QStringLiteral(R"(([\^_])\{([^{}]*)\})")); // ^{..} _{..}, done first
        for (int guard = 0; guard < 8; ++guard) { // innermost first, so nested ones unwrap too
            const QString before = t;
            t.replace(wrapped, QStringLiteral("\\1"));
            for (QRegularExpressionMatch m = frac.match(t); m.hasMatch(); m = frac.match(t)) { // a/b, (a + b)/c
                const auto part = [](const QString& x) { return x.trimmed().contains(' ') ? "(" + x.trimmed() + ")" : x.trimmed(); };
                t.replace(m.capturedStart(), m.capturedLength(), part(m.captured(1)) + "/" + part(m.captured(2)));
            }
            t.replace(sqrt, QString::fromUtf8("√(\\1)"));
            for (QRegularExpressionMatch m = group.match(t); m.hasMatch(); m = group.match(t)) {
                const bool super = m.captured(1) == "^";
                QString mapped = superOrSub(m.captured(2), super);
                if (mapped.isEmpty()) { // letters: ^(..) and _.. read fine as text
                    mapped = super ? QString("^(" + m.captured(2) + ")") : QString("\u0001" + m.captured(2));
                }
                t.replace(m.capturedStart(), m.capturedLength(), mapped);
            }
            if (t == before) {
                break;
            }
        }
        t.replace(QStringLiteral("\\%"), QStringLiteral("%")).replace(QStringLiteral("\\$"), QStringLiteral("$"));
        t.replace(QStringLiteral("\\&"), QStringLiteral("&")).replace(QStringLiteral("\\#"), QStringLiteral("#"));
        t.replace(QStringLiteral("\\_"), QStringLiteral("_")).replace(QStringLiteral("\\{"), QStringLiteral("("));
        t.replace(QStringLiteral("\\}"), QStringLiteral(")"));
        static const QRegularExpression spacing(QStringLiteral(R"(\\[,;:! ])"));
        t.replace(spacing, QStringLiteral(" "));
        QString out;
        int last = 0;
        for (auto it = command.globalMatch(t); it.hasNext();) { // \name -> its symbol, or just the name
            const QRegularExpressionMatch m = it.next();
            out += t.mid(last, m.capturedStart() - last);
            QString name = m.captured(1);
            QString symbol = name;
            for (const auto& [key, value] : symbols) {
                if (key == name) {
                    symbol = value;
                    break;
                }
            }
            out += symbol;
            last = m.capturedEnd();
        }
        out += t.mid(last);
        t = out;
        out.clear();
        last = 0;
        for (auto it = script.globalMatch(t); it.hasNext();) { // ^2 -> ², _{12} -> ₁₂, ^° -> °
            const QRegularExpressionMatch m = it.next();
            out += t.mid(last, m.capturedStart() - last);
            const bool super = m.captured(1) == "^";
            const QString body = m.captured(2).isNull() ? m.captured(3) : m.captured(2);
            QString mapped = body.contains(QChar(0xB0)) ? body : superOrSub(body, super);
            if (mapped.isEmpty()) {
                mapped = super ? QString(QStringLiteral("^(") + body + ")") : QString("\u0001" + body);
            }
            out += mapped;
            last = m.capturedEnd();
        }
        out += t.mid(last);
        out.remove(QChar('{')).remove(QChar('}')).replace(QChar('~'), QChar(' '));
        out.replace(QChar(1), QStringLiteral("\\_")); // a subscript that has no Unicode form: V\_rms
        static const QRegularExpression diameter(QString::fromUtf8(R"(⌀\s+(?=\d))"));
        out.replace(diameter, QString::fromUtf8("⌀"));
        static const QRegularExpression spaces(QStringLiteral(" {2,}"));
        out.replace(spaces, QStringLiteral(" "));
        return out.trimmed();
    }

    /** One line of Markdown prose (no code): $...$, $$...$$, \(...\) and \[...\] math made readable. */
    QString readableMathLine(const QString& line) {
        static const QRegularExpression display(QStringLiteral(R"(\$\$(.+?)\$\$|\\\[(.+?)\\\]|\\\((.+?)\\\))"));
        // inline $...$ as Pandoc reads it: no space inside the dollars, no digit right after the closing one
        static const QRegularExpression inlineMath(QStringLiteral(R"((?<![\\$\w])\$(?=[^\s$])([^$\n]*?[^\s\\$])\$(?![\d$]))"));
        static const QRegularExpression latexy(QStringLiteral(R"([\\^_{}])"));
        if (!line.contains('$') && !line.contains('\\')) {
            return line;
        }
        QString out;
        int last = 0;
        for (auto it = display.globalMatch(line); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            out += line.mid(last, m.capturedStart() - last);
            const QString body = !m.captured(1).isNull() ? m.captured(1) : !m.captured(2).isNull() ? m.captured(2) : m.captured(3);
            out += latexToText(body);
            last = m.capturedEnd();
        }
        out += line.mid(last);
        const QString pass = out;
        out.clear();
        last = 0;
        for (auto it = inlineMath.globalMatch(pass); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            const QString body = m.captured(1);
            const bool isMath = body.contains(latexy) || !body.contains(' '); // "$5 and $10" stays money
            out += pass.mid(last, m.capturedStart() - last);
            out += isMath ? latexToText(body) : m.captured(0);
            last = m.capturedEnd();
        }
        out += pass.mid(last);
        return out;
    }

    /**
     * The AI's Markdown as the chat shows it: math markup turned into readable text and headings scaled
     * down to the narrow panel (Qt draws # as a poster). Code blocks and `code` spans are left alone.
     */
    QString displayMarkdown(const QString& markdown) {
        static const QRegularExpression heading(QStringLiteral(R"(^(\s{0,3})(#{1,6})(\s))"));
        QStringList lines = markdown.split('\n');
        bool inFence = false;
        for (QString& line : lines) {
            const QString trimmed = line.trimmed();
            if (trimmed.startsWith(QStringLiteral("```")) || trimmed.startsWith(QStringLiteral("~~~"))) {
                inFence = !inFence;
                continue;
            }
            if (inFence) {
                continue;
            }
            if (const QRegularExpressionMatch m = heading.match(line); m.hasMatch()) {
                const int level = m.captured(2).size();
                line = m.captured(1) + QString(level <= 2 ? 3 : 4, '#') + line.mid(m.capturedEnd(2));
            }
            // math, outside `code` spans
            QString out;
            int pos = 0;
            while (pos < line.size()) {
                const int tick = line.indexOf('`', pos);
                if (tick < 0) {
                    out += readableMathLine(line.mid(pos));
                    break;
                }
                out += readableMathLine(line.mid(pos, tick - pos));
                int run = tick;
                while (run < line.size() && line.at(run) == '`') {
                    ++run;
                }
                const int close = line.indexOf(line.mid(tick, run - tick), run);
                const int end = close < 0 ? line.size() : close + (run - tick);
                out += line.mid(tick, end - tick);
                pos = end;
            }
            line = out;
        }
        return lines.join('\n');
    }

    /** Cypher's summarize(): the most telling argument, so a collapsed row stays readable. */
    QString summarize(const QString& name, const QJsonObject& args) {
        const auto pick = [&args](const QStringList& keys) -> QString {
            for (const QString& k : keys) {
                if (args.value(k).isString() && !args.value(k).toString().isEmpty()) {
                    return args.value(k).toString();
                }
            }
            return {};
        };
        if (name == "run_command") {
            QStringList lines;
            for (const QJsonValue& v : args.value("commands").toArray()) {
                lines << v.toString();
            }
            if (lines.isEmpty()) {
                lines << args.value("commands").toString(); // a single line given as a string
            }
            return lines.join("  ·  ");
        }
        if (name == "press_toolbar_button") {
            // the button, then what was typed or clicked for it
            QStringList parts{args.value("id").toString()};
            for (const QJsonValue& v : args.value("steps").toArray()) {
                const QJsonObject step = v.toObject();
                const QString type = step.value("type").toString();
                if (type == "type" || type == "key") {
                    parts << step.value("text").toString();
                }
                else if (type == "click" || type == "move") {
                    parts << QStringLiteral("%1 %2,%3").arg(type).arg(step.value("x").toDouble()).arg(step.value("y").toDouble());
                }
                else if (type == "drag") {
                    parts << QStringLiteral("drag %1,%2→%3,%4").arg(step.value("x").toDouble()).arg(step.value("y").toDouble())
                                 .arg(step.value("x2").toDouble()).arg(step.value("y2").toDouble());
                }
            }
            return parts.join("  ·  ");
        }
        if (name == "click_drawing") {
            return QStringLiteral("%1, %2").arg(args.value("x").toDouble()).arg(args.value("y").toDouble());
        }
        if (name == "handle_dialog" || name == "tool_options") {
            QStringList parts;
            for (const QJsonValue& v : args.value("fields").toArray()) {
                parts << QStringLiteral("%1 = %2").arg(v.toObject().value("field").toString(),
                                                      v.toObject().value("value").toVariant().toString());
            }
            if (!args.value("file_path").toString().isEmpty()) {
                parts << args.value("file_path").toString();
            }
            if (!args.value("button").toString().isEmpty()) {
                parts << args.value("button").toString();
            }
            return parts.join("  ·  ");
        }
        if (name == "select_entities") {
            if (args.value("all").toBool()) {
                return QStringLiteral("all");
            }
            if (args.value("ids").isArray()) {
                QStringList ids;
                for (const QJsonValue& v : args.value("ids").toArray()) {
                    ids << "#" + QString::number(v.toInteger());
                }
                return ids.join(" ");
            }
        }
        if (name == "zoom_to_area" || name == "list_entities") {
            if (args.contains("x1")) {
                return QStringLiteral("(%1, %2) – (%3, %4)").arg(args.value("x1").toDouble()).arg(args.value("y1").toDouble())
                    .arg(args.value("x2").toDouble()).arg(args.value("y2").toDouble());
            }
        }
        if (name == "zoom") {
            return QStringLiteral("×%1").arg(args.value("factor").toDouble());
        }
        if (name == "pan") {
            return QStringLiteral("dx %1, dy %2").arg(args.value("dx").toDouble()).arg(args.value("dy").toDouble());
        }
        QString first = pick({"description", "prompt", "query", "path", "file_path", "url", "command", "id", "pattern", "task",
                              "project_id", "name", "type", "layer", "prefix", "toolbar", "search", "value"});
        if (args.value("kinds").isArray()) {
            QStringList kinds;
            for (const QJsonValue& v : args.value("kinds").toArray()) {
                kinds << v.toString();
            }
            first = QString(first + "  " + kinds.join(", ")).trimmed();
        }
        if (first.isEmpty() && !args.isEmpty()) {
            first = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
        }
        return first;
    }

    QString truncated(const QString& text) {
        return text.size() > MAX_PRE_CHARS ? text.left(MAX_PRE_CHARS) + QStringLiteral("\n… (%1 more characters)").arg(text.size() - MAX_PRE_CHARS)
                                           : text;
    }

    /** A label that elides its single line on the right as it shrinks (.tool-summary). */
    class ElidedLabel : public QLabel {
    public:
        using QLabel::QLabel;
        void setFullText(const QString& text) {
            m_full = text;
            setToolTip(text.size() > 60 ? text : QString());
            update();
        }
        QSize minimumSizeHint() const override { return {20, QLabel::minimumSizeHint().height()}; }
        QSize sizeHint() const override { return {fontMetrics().horizontalAdvance(m_full) + 4, QLabel::sizeHint().height()}; }

    protected:
        void paintEvent(QPaintEvent*) override {
            QPainter p(this);
            p.setPen(palette().color(foregroundRole()));
            p.setFont(font());
            QString line = m_full;
            line.replace('\n', ' ');
            p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(line, Qt::ElideRight, width()));
        }

    private:
        QString m_full;
    };

    void showFullImage(QWidget* parent, const QPixmap& pixmap) {
        auto* dialog = new QDialog(parent->window());
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(QObject::tr("Tool result, full size"));
        auto* layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* scroll = new QScrollArea(dialog);
        auto* label = new QLabel();
        label->setPixmap(pixmap);
        scroll->setWidget(label);
        layout->addWidget(scroll);
        dialog->resize(qMin(pixmap.width() + 24, 1400), qMin(pixmap.height() + 24, 900));
        dialog->show();
    }

    constexpr int TINY_THUMB_HEIGHT = 18; // the small image on a tool row: there is a screenshot

    /** A very small thumbnail on a row's right side; a click shows or hides the screenshots. */
    class TinyThumb : public QLabel {
    public:
        TinyThumb(const QPixmap& full, std::function<void()> onClick, QWidget* parent)
            : QLabel(parent), m_onClick(std::move(onClick)) {
            const qreal dpr = devicePixelRatioF();
            QPixmap thumb = full.scaledToHeight(qRound(TINY_THUMB_HEIGHT * dpr), Qt::SmoothTransformation);
            if (thumb.width() > qRound(32 * dpr)) {
                thumb = thumb.copy((thumb.width() - qRound(32 * dpr)) / 2, 0, qRound(32 * dpr), thumb.height());
            }
            thumb.setDevicePixelRatio(dpr);
            setPixmap(thumb);
            setObjectName("toolThumb");
            setCursor(Qt::PointingHandCursor);
            setToolTip(QObject::tr("Show or hide the screenshot"));
        }

    protected:
        void mouseReleaseEvent(QMouseEvent* event) override {
            if (event->button() == Qt::LeftButton && m_onClick) {
                m_onClick();
            }
            event->accept(); // not a click on the row
        }
        void mousePressEvent(QMouseEvent* event) override { event->accept(); }

    private:
        std::function<void()> m_onClick;
    };

    /** A clickable thumbnail (.tool-image) that opens the full image. */
    class Thumbnail : public QLabel {
    public:
        Thumbnail(const QPixmap& full, QWidget* parent) : QLabel(parent), m_full(full) {
            QPixmap thumb = full.scaledToHeight(qMin(THUMBNAIL_HEIGHT, full.height()), Qt::SmoothTransformation);
            setPixmap(thumb);
            setCursor(Qt::PointingHandCursor);
            setToolTip(QObject::tr("Open full size"));
            setObjectName("toolImage");
        }

    protected:
        void mouseReleaseEvent(QMouseEvent* event) override {
            if (event->button() == Qt::LeftButton) {
                showFullImage(this, m_full);
            }
        }

    private:
        QPixmap m_full;
    };
}

// ------------------------------------------------------------------------------------------------ tool call

LC_AIToolCallWidget::LC_AIToolCallWidget(const LC_AIToolCall& call, const QString& label, const LC_AITheme& theme,
                                         QWidget* parent)
    : QFrame(parent), m_call(call), m_theme(theme) {
    setObjectName("tool");
    setProperty("open", false);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_row = new QWidget(this);
    m_row->setObjectName("toolRow");
    m_row->setAttribute(Qt::WA_Hover);
    m_row->setCursor(Qt::PointingHandCursor);
    auto* row = new QHBoxLayout(m_row);
    row->setContentsMargins(8, 4, 8, 4);
    row->setSpacing(7);
    const qreal dpr = devicePixelRatioF();
    m_chevron = new QLabel(m_row);
    m_chevron->setPixmap(LC_AIIcons::lucide("chevron-right", 12, theme.textFaint, dpr));
    auto* name = new QLabel(label, m_row);
    name->setObjectName("toolName");
    auto* summary = new ElidedLabel(m_row);
    summary->setObjectName("toolSummary");
    summary->setFullText(summarize(call.name, call.arguments));
    summary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_statusIcon = new QLabel(m_row);
    m_statusText = new QLabel(m_row);
    m_statusText->setObjectName("toolStatus");
    m_thumbs = new QWidget(m_row);
    auto* thumbs = new QHBoxLayout(m_thumbs);
    thumbs->setContentsMargins(0, 0, 0, 0);
    thumbs->setSpacing(3);
    m_thumbs->hide();
    row->addWidget(name);
    row->addWidget(summary, 1);
    row->addWidget(m_thumbs);
    row->addWidget(m_statusIcon);
    row->addWidget(m_statusText);
    row->addWidget(m_chevron);
    layout->addWidget(m_row);

    m_images = new QWidget(this);
    auto* images = new QHBoxLayout(m_images);
    images->setContentsMargins(10, 2, 8, 8);
    images->setSpacing(8);
    m_images->hide();
    layout->addWidget(m_images);

    m_body = new QWidget(this);
    m_body->hide();
    layout->addWidget(m_body);

    setStatus(QString(), tr("Waiting"), theme.textFaint);
}

void LC_AIToolCallWidget::setStatus(const QString& icon, const QString& text, const QColor& color) {
    if (icon.isEmpty()) {
        m_statusIcon->clear();
    }
    else {
        m_statusIcon->setPixmap(LC_AIIcons::lucide(icon, 13, color, devicePixelRatioF()));
    }
    m_statusText->setText(text);
    m_statusText->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
    emit changed();
}

void LC_AIToolCallWidget::setRunning() {
    m_timer.start();
    m_active = true;
    setStatus("loader", tr("Running"), STATUS_RUNNING);
}

void LC_AIToolCallWidget::setAwaitingApproval() {
    m_active = true;
    setStatus("hourglass", tr("Awaiting approval"), QColor("#fbbf24"));
}

void LC_AIToolCallWidget::setResult(const LC_AIToolResult& result) {
    m_result = result;
    m_done = true;
    m_active = false;
    m_failed = result.isError;
    const qint64 ms = m_timer.isValid() ? m_timer.elapsed() : 0;
    const QString time = ms < 1000 ? QStringLiteral("%1ms").arg(ms) : QStringLiteral("%1s").arg(ms / 1000.0, 0, 'f', 1);
    static const QRegularExpression blocked(QStringLiteral("user denied this tool call|denied by the user's tool permissions"));
    if (result.isError && blocked.match(result.joinedText()).hasMatch()) {
        setStatus("shield-alert", tr("Blocked"), STATUS_ERROR); // Cypher marks denied calls as Blocked
    }
    else if (result.isError) {
        setStatus("circle-alert", tr("Error"), STATUS_ERROR);
    }
    else {
        setStatus("check", time, STATUS_DONE);
    }
    // screenshots: a tiny thumbnail on the row says there is one; a click on it shows them under the row
    auto* images = static_cast<QHBoxLayout*>(m_images->layout());
    auto* thumbs = static_cast<QHBoxLayout*>(m_thumbs->layout());
    for (const LC_AIContent& c : result.content) {
        if (c.type == LC_AIContent::Type::Image) {
            QPixmap pixmap;
            if (pixmap.loadFromData(c.data)) {
                m_pixmaps << pixmap;
                images->addWidget(new Thumbnail(pixmap, m_images));
                if (m_pixmaps.size() <= 2) {
                    thumbs->addWidget(new TinyThumb(pixmap, [this] { setPreviewOpen(m_images->isHidden()); }, m_thumbs));
                }
            }
        }
    }
    if (!m_pixmaps.isEmpty()) {
        images->addStretch(1);
        if (m_pixmaps.size() > 2) {
            auto* more = new QLabel(QStringLiteral("+%1").arg(m_pixmaps.size() - 2), m_thumbs);
            more->setObjectName("toolSummary");
            thumbs->addWidget(more);
        }
        m_thumbs->show();
    }
    if (m_open) {
        buildBody();
    }
    emit changed();
}

void LC_AIToolCallWidget::setPreviewOpen(const bool open) {
    if (!m_pixmaps.isEmpty()) {
        m_images->setVisible(open);
    }
}

void LC_AIToolCallWidget::setStopped() {
    m_active = false;
    if (!m_done) {
        m_done = true;
        setStatus(QString(), tr("Stopped"), m_theme.textFaint);
    }
}

void LC_AIToolCallWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_row->geometry().contains(event->position().toPoint())) {
        setOpen(!m_open);
    }
    QFrame::mouseReleaseEvent(event);
}

void LC_AIToolCallWidget::setOpen(const bool open) {
    m_open = open;
    m_chevron->setPixmap(LC_AIIcons::lucide(open ? "chevron-down" : "chevron-right", 12, m_theme.textFaint, devicePixelRatioF()));
    setProperty("open", open);
    style()->unpolish(this);
    style()->polish(this);
    if (open) {
        buildBody();
    }
    m_body->setVisible(open);
}

void LC_AIToolCallWidget::buildBody() {
    delete m_body->layout();
    qDeleteAll(m_body->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly));
    auto* layout = new QVBoxLayout(m_body);
    layout->setContentsMargins(10, 4, 10, 10);
    layout->setSpacing(8);
    const auto section = [this, layout](const QString& title, const QString& text, const bool error) {
        auto* label = new QLabel(title, m_body);
        label->setObjectName("toolSectionLabel");
        auto* pre = new QLabel(truncated(text), m_body);
        pre->setObjectName(error ? "toolPreError" : "toolPre");
        pre->setWordWrap(true);
        pre->setTextFormat(Qt::PlainText);
        pre->setTextInteractionFlags(Qt::TextSelectableByMouse);
        pre->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(label);
        layout->addWidget(pre);
    };
    if (!m_call.argumentsError.isEmpty()) {
        section(tr("Arguments error"), m_call.argumentsError, true);
    }
    section(tr("Arguments"), QString::fromUtf8(QJsonDocument(m_call.arguments).toJson(QJsonDocument::Indented)).trimmed(), false);
    if (m_done && !m_result.content.isEmpty()) {
        QString text = m_result.joinedText();
        // pretty-print JSON results (MCP servers such as UniCAD answer in JSON)
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
        if (!doc.isNull()) {
            text = QString::fromUtf8(doc.toJson(QJsonDocument::Indented)).trimmed();
        }
        if (!text.isEmpty()) {
            section(tr("Result"), text, m_result.isError);
        }
    }
}

// ------------------------------------------------------------------------------------------------ tool block

LC_AIToolGroup::LC_AIToolGroup(const LC_AITheme& theme, QWidget* parent)
    : QFrame(parent), m_theme(theme) {
    setObjectName("toolGroup");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->setSpacing(3);

    m_header = new QWidget(this);
    m_header->setObjectName("toolGroupHeader");
    m_header->setAttribute(Qt::WA_Hover);
    m_header->setCursor(Qt::PointingHandCursor);
    auto* row = new QHBoxLayout(m_header);
    row->setContentsMargins(6, 3, 6, 3);
    row->setSpacing(6);
    m_statusIcon = new QLabel(m_header);
    m_title = new QLabel(m_header);
    m_title->setObjectName("toolGroupTitle");
    m_chevron = new QLabel(m_header);
    m_thumbs = new QWidget(m_header);
    auto* thumbs = new QHBoxLayout(m_thumbs);
    thumbs->setContentsMargins(0, 0, 0, 0);
    thumbs->setSpacing(3);
    row->addWidget(m_statusIcon);
    row->addWidget(m_title);
    row->addWidget(m_chevron);
    row->addStretch(1);
    row->addWidget(m_thumbs);
    layout->addWidget(m_header);

    m_box = new QFrame(this);
    m_box->setObjectName("toolGroupBox");
    auto* box = new QVBoxLayout(m_box);
    box->setContentsMargins(4, 3, 4, 3);
    box->setSpacing(0);
    layout->addWidget(m_box);
    updateHeader();
}

void LC_AIToolGroup::addTool(LC_AIToolCallWidget* tool) {
    auto* box = static_cast<QVBoxLayout*>(m_box->layout());
    if (!m_tools.isEmpty()) {
        auto* line = new QFrame(m_box);
        line->setObjectName("toolGroupLine");
        line->setFixedHeight(1);
        box->addWidget(line);
    }
    tool->setParent(m_box);
    box->addWidget(tool);
    m_tools << tool;
    connect(tool, &LC_AIToolCallWidget::changed, this, &LC_AIToolGroup::updateHeader);
    if (!m_userChose) {
        setExpanded(true); // open while the AI works
    }
    updateHeader();
}

void LC_AIToolGroup::close() {
    m_closed = true;
    updateHeader();
}

void LC_AIToolGroup::setExpanded(const bool expanded) {
    m_expanded = expanded;
    m_box->setVisible(expanded);
    m_chevron->setPixmap(LC_AIIcons::lucide(expanded ? "chevron-down" : "chevron-right", 13, m_theme.textMuted, devicePixelRatioF()));
    m_thumbs->setVisible(!expanded); // the rows show their own thumbnails
}

void LC_AIToolGroup::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_header->geometry().contains(event->position().toPoint())) {
        m_userChose = true;
        setExpanded(!m_expanded);
    }
    QFrame::mouseReleaseEvent(event);
}

void LC_AIToolGroup::updateHeader() {
    int active = 0;
    int failed = 0;
    QList<QPixmap> images;
    for (LC_AIToolCallWidget* t : std::as_const(m_tools)) {
        active += t->isActive() ? 1 : 0;
        failed += t->failed() ? 1 : 0;
        images << t->images();
    }
    const int n = m_tools.size();
    const qreal dpr = devicePixelRatioF();
    QString title;
    if (active > 0) {
        title = n == 1 ? tr("Running a command") : tr("Running %1 commands").arg(n);
        m_statusIcon->setPixmap(LC_AIIcons::lucide("loader", 13, STATUS_RUNNING, dpr));
        m_statusIcon->show();
    }
    else {
        title = n == 1 ? tr("Ran a command") : tr("Ran %1 commands").arg(n);
        if (failed > 0) {
            title += tr(", %1 failed").arg(failed);
            m_statusIcon->setPixmap(LC_AIIcons::lucide("circle-alert", 13, STATUS_ERROR, dpr));
            m_statusIcon->show();
        }
        else {
            m_statusIcon->hide();
        }
    }
    m_title->setText(title);
    // the last screenshots as tiny thumbnails on the right of the folded block
    auto* thumbs = static_cast<QHBoxLayout*>(m_thumbs->layout());
    while (QLayoutItem* item = thumbs->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const int shown = qMin(3, static_cast<int>(images.size()));
    for (int i = images.size() - shown; i < images.size(); ++i) {
        thumbs->addWidget(new TinyThumb(images[i], [this] { m_userChose = true; setExpanded(true); }, m_thumbs));
    }
    if (images.size() > shown) {
        auto* more = new QLabel(QStringLiteral("+%1").arg(images.size() - shown), m_thumbs);
        more->setObjectName("toolSummary");
        thumbs->addWidget(more);
    }
    // folded once the AI has moved on and nothing runs or waits for approval
    if (m_closed && active == 0 && !m_userChose && m_expanded) {
        setExpanded(false);
    }
    else if (active > 0 && !m_expanded && !m_userChose) {
        setExpanded(true);
    }
    else {
        m_thumbs->setVisible(!m_expanded && !images.isEmpty());
    }
}

// ------------------------------------------------------------------------------------------------ clarification

namespace {
    /** A choice row: number badge and text, clickable. */
    class ChoiceRow : public QFrame {
    public:
        ChoiceRow(const int number, const QString& text, std::function<void()> onClick, QWidget* parent)
            : QFrame(parent), m_onClick(std::move(onClick)) {
            setObjectName("choiceRow");
            setAttribute(Qt::WA_Hover);
            setCursor(Qt::PointingHandCursor);
            auto* layout = new QHBoxLayout(this);
            layout->setContentsMargins(10, 8, 10, 8);
            layout->setSpacing(10);
            auto* badge = new QLabel(QString::number(number), this);
            badge->setObjectName("choiceBadge");
            badge->setAlignment(Qt::AlignCenter);
            badge->setFixedSize(22, 22);
            auto* label = new QLabel(text, this);
            label->setObjectName("choiceText");
            label->setWordWrap(true);
            label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            layout->addWidget(badge, 0, Qt::AlignTop);
            layout->addWidget(label, 1);
        }

    protected:
        void mouseReleaseEvent(QMouseEvent* event) override {
            if (event->button() == Qt::LeftButton && isEnabled() && rect().contains(event->position().toPoint())) {
                m_onClick();
            }
        }

    private:
        std::function<void()> m_onClick;
    };
}

LC_AIClarificationCard::LC_AIClarificationCard(const QString& question, const QStringList& choices, const LC_AITheme& theme,
                                               QWidget* parent)
    : QFrame(parent), m_theme(theme) {
    setObjectName("clarCard");
    const LC_AITheme& t = theme;
    setStyleSheet(QStringLiteral(R"(
#clarCard { background: %1; border: 1px solid %2; border-radius: 10px; }
#clarCard[answered="true"] { background: transparent; border-color: transparent; }
#clarCard[answered="true"][open="true"] { border-color: %4; background: %7; }
#clarSummary { border-radius: 8px; background: transparent; }
#clarSummary:hover { background: %5; }
#clarSummaryName { font-size: 13px; font-weight: 500; color: %3; }
#clarSummaryAnswer { font-size: 12.5px; color: %3; }
#clarQuestion { font-size: 13.5px; color: %3; }
#clarChoices { border: 1px solid %2; border-radius: 8px; background: transparent; }
#choiceRow { border: none; border-top: 1px solid %4; border-radius: 0; }
#choiceRow[first="true"] { border-top: none; }
#choiceRow:hover { background: %5; }
#choiceRow[chosen="true"] { background: %6; }
#choiceBadge { background: #4b5563; color: #ffffff; border-radius: 5px; font-size: 12px; font-weight: 600; }
#choiceText { font-size: 13px; color: %3; }
#clarInput { background: %7; border: 1px solid %2; border-radius: 8px; padding: 6px 10px; font-size: 13px; color: %3; }
#clarInput:focus { border-color: %8; }
#clarContinue { background: %9; color: %10; border: none; border-radius: 8px; padding: 0 12px; min-height: 30px; font-size: 13px; font-weight: 500; }
#clarContinue:disabled { background: %5; color: %11; }
#clarStatus { font-size: 12px; color: %11; }
)")
                      .arg(t.bgElevated.name(), t.borderDefault.name(QColor::HexArgb), t.textPrimary.name(),
                           t.borderSubtle.name(QColor::HexArgb), t.bgHover.name(QColor::HexArgb), t.bgActive.name(QColor::HexArgb),
                           t.bgCard.name(QColor::HexArgb), t.accentBlueBorder.name(QColor::HexArgb), t.textHeading.name())
                      .arg(t.bgRoot.name(), t.textMuted.name()));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    // the row it collapses into once answered (Cypher's tool row: chevron, icon, name, answer, status)
    const qreal dpr = devicePixelRatioF();
    m_summary = new QWidget(this);
    m_summary->setObjectName("clarSummary");
    m_summary->setAttribute(Qt::WA_Hover);
    m_summary->setCursor(Qt::PointingHandCursor);
    m_summary->installEventFilter(this);
    auto* sum = new QHBoxLayout(m_summary);
    sum->setContentsMargins(8, 4, 8, 4);
    sum->setSpacing(7);
    m_summaryChevron = new QLabel(m_summary);
    m_summaryChevron->setPixmap(LC_AIIcons::lucide("chevron-right", 13, t.textSecondary, dpr));
    auto* icon = new QLabel(m_summary);
    icon->setPixmap(LC_AIIcons::lucide("circle-help", 13, t.textSecondary, dpr));
    auto* name = new QLabel(tr("Question"), m_summary);
    name->setObjectName("clarSummaryName");
    m_summaryAnswer = new QLabel(m_summary);
    m_summaryAnswer->setObjectName("clarSummaryAnswer");
    m_summaryAnswer->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_summaryStatusIcon = new QLabel(m_summary);
    m_summaryStatus = new QLabel(m_summary);
    m_summaryStatus->setStyleSheet(QStringLiteral("font-size: 12px;"));
    sum->addWidget(m_summaryChevron);
    sum->addWidget(icon);
    sum->addWidget(name);
    sum->addWidget(m_summaryAnswer, 1);
    sum->addWidget(m_summaryStatusIcon);
    sum->addWidget(m_summaryStatus);
    m_summary->hide();
    outer->addWidget(m_summary);
    m_body = new QWidget(this);
    outer->addWidget(m_body);

    auto* layout = new QVBoxLayout(m_body);
    layout->setContentsMargins(12, 10, 12, 12);
    layout->setSpacing(8);
    auto* q = new QLabel(question, this);
    q->setObjectName("clarQuestion");
    q->setWordWrap(true);
    q->setTextInteractionFlags(Qt::TextSelectableByMouse);
    q->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(q);
    if (!choices.isEmpty()) {
        auto* box = new QFrame(this);
        box->setObjectName("clarChoices");
        auto* list = new QVBoxLayout(box);
        list->setContentsMargins(0, 0, 0, 0);
        list->setSpacing(0);
        for (int i = 0; i < choices.size(); ++i) {
            const QString choice = choices[i];
            auto* row = new ChoiceRow(i + 1, choice, [this, choice] { choose(choice); }, box);
            row->setProperty("first", i == 0);
            list->addWidget(row);
            m_choiceRows << row;
        }
        layout->addWidget(box);
    }
    auto* answerRow = new QHBoxLayout();
    answerRow->setSpacing(8);
    m_input = new QLineEdit(this);
    m_input->setObjectName("clarInput");
    m_input->setPlaceholderText(tr("Type your answer"));
    QPalette pal = m_input->palette();
    pal.setColor(QPalette::PlaceholderText, t.textPlaceholder);
    pal.setColor(QPalette::Text, t.textPrimary);
    m_input->setPalette(pal);
    m_continue = new QPushButton(tr("Continue"), this);
    m_continue->setObjectName("clarContinue");
    m_continue->setCursor(Qt::PointingHandCursor);
    m_continue->setEnabled(false);
    answerRow->addWidget(m_input, 1);
    answerRow->addWidget(m_continue);
    layout->addLayout(answerRow);
    m_status = new QLabel(this);
    m_status->setObjectName("clarStatus");
    m_status->hide();
    layout->addWidget(m_status);

    connect(m_input, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_continue->setEnabled(!text.trimmed().isEmpty() && m_answer.isEmpty());
    });
    const auto submit = [this] {
        if (!m_input->text().trimmed().isEmpty()) {
            choose(m_input->text().trimmed());
        }
    };
    connect(m_continue, &QPushButton::clicked, this, submit);
    connect(m_input, &QLineEdit::returnPressed, this, submit);
}

void LC_AIClarificationCard::choose(const QString& answer) {
    if (!m_answer.isEmpty()) {
        return;
    }
    m_answer = answer;
    for (QWidget* row : std::as_const(m_choiceRows)) {
        const bool chosen = row->findChild<QLabel*>("choiceText")->text() == answer;
        row->setProperty("chosen", chosen);
        row->style()->unpolish(row);
        row->style()->polish(row);
        row->setEnabled(false);
        row->setCursor(Qt::ArrowCursor);
    }
    // reopened later it shows the question and choices only, the chosen one highlighted
    m_input->hide();
    m_continue->hide();
    m_status->hide();
    collapse(answer, true);
    if (m_deliver) {
        m_deliver(m_answer);
    }
}

void LC_AIClarificationCard::collapse(const QString& summary, const bool answered) {
    const QColor color = answered ? QColor("#22c55e") : m_theme.textFaint;
    m_summaryAnswer->setText(summary);
    m_summaryStatusIcon->setPixmap(answered ? LC_AIIcons::lucide("check", 13, color, devicePixelRatioF()) : QPixmap());
    m_summaryStatus->setText(answered ? tr("Answered") : tr("Not answered"));
    m_summaryStatus->setStyleSheet(QStringLiteral("font-size: 12px; color: %1;").arg(color.name()));
    m_summary->show();
    setProperty("answered", true);
    setBodyOpen(false);
}

void LC_AIClarificationCard::setBodyOpen(const bool open) {
    m_bodyOpen = open;
    m_body->setVisible(open);
    m_summaryChevron->setPixmap(LC_AIIcons::lucide(open ? "chevron-down" : "chevron-right", 13, m_theme.textSecondary, devicePixelRatioF()));
    setProperty("open", open);
    style()->unpolish(this);
    style()->polish(this);
}

bool LC_AIClarificationCard::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_summary && event->type() == QEvent::MouseButtonRelease
        && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        setBodyOpen(!m_bodyOpen); // look at the question and choices again
        return true;
    }
    return QFrame::eventFilter(watched, event);
}

void LC_AIClarificationCard::activate(std::function<void(const QString&)> deliver) {
    m_activated = true;
    m_deliver = std::move(deliver);
    if (!m_answer.isEmpty()) {
        m_deliver(m_answer); // answered before the tool ran
    }
}

void LC_AIClarificationCard::setStopped() {
    if (m_answer.isEmpty()) {
        m_answer = QStringLiteral(" "); // locks the card
        for (QWidget* row : std::as_const(m_choiceRows)) {
            row->setEnabled(false);
        }
        m_input->setEnabled(false);
        m_continue->setEnabled(false);
        m_status->setText(tr("Not answered (stopped)"));
        m_status->show();
        collapse(QString(), false);
    }
}

void LC_AIClarificationCard::focusInput() {
    m_input->setFocus();
}

// ------------------------------------------------------------------------------------------------ transcript

LC_AIChatTranscript::LC_AIChatTranscript(const LC_AITheme& theme, QWidget* parent)
    : QScrollArea(parent), m_theme(theme) {
    setObjectName("aiTranscript");
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    const LC_AITheme& t = theme;
    const QColor preBg = t.dark ? QColor(0, 0, 0, 77) : QColor(0, 0, 0, 20); // --overlay-dim
    setStyleSheet(QStringLiteral(R"(
#aiTranscript, #aiTranscriptContent { background: transparent; }
#userBubble { border: 1px solid %8; border-radius: 12px; background: %9; padding: 8px 14px; }
#aiThinkingText { font-size: 12.5px; color: %5; }
#toolGroup { background: transparent; }
#toolGroupHeader { border-radius: 7px; background: transparent; }
#toolGroupHeader:hover { background: %3; }
#toolGroupTitle { font-size: 13px; color: %5; }
#toolGroupBox { border: 1px solid %1; border-radius: 10px; background: %2; }
#toolGroupLine { background: %1; border: none; margin: 0 6px; }
#tool { border: none; border-radius: 8px; background: transparent; }
#toolRow { border-radius: 7px; background: transparent; }
#toolRow:hover { background: %3; }
#toolName { font-size: 12.5px; font-weight: 500; }
#toolSummary { font-family: %4; font-size: 11.5px; color: %5; }
#toolThumb { border: 1px solid %7; border-radius: 3px; }
#toolStatus { font-size: 12px; }
#toolSectionLabel { font-size: 11.5px; font-weight: 500; color: %5; }
#toolPre, #toolPreError { font-family: %4; font-size: 12px; background: %6; border-radius: 6px; padding: 8px 10px; }
#toolPreError { color: #f87171; }
#toolImage { border: 1px solid %7; border-radius: 8px; }
#aiError { color: #f87171; }
QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
QScrollBar::handle:vertical { background: %10; border-radius: 3px; min-height: 30px; }
QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page { background: none; height: 0; width: 0; }
)")
                      .arg(t.borderSubtle.name(QColor::HexArgb), t.bgCard.name(QColor::HexArgb), t.bgHover.name(QColor::HexArgb),
                           mono(), t.textMuted.name(), preBg.name(QColor::HexArgb), t.borderDefault.name(QColor::HexArgb),
                           t.borderOverlay.name(QColor::HexArgb))
                      .arg(t.dark ? QColor(255, 255, 255, 18).name(QColor::HexArgb) : QColor(0, 0, 0, 13).name(QColor::HexArgb),
                           t.borderStrong.name(QColor::HexArgb)));
    m_content = new QWidget();
    m_content->setObjectName("aiTranscriptContent");
    m_layout = new QVBoxLayout(m_content);
    m_layout->setContentsMargins(8, 8, 8, 8);
    m_layout->setSpacing(4);
    m_layout->addStretch(1);
    setWidget(m_content);
    setFrameShape(QFrame::NoFrame);
    viewport()->setAutoFillBackground(false); // the panel's own background shows through
    m_content->setAutoFillBackground(false);

    m_thinking = new QWidget(m_content);
    auto* thinking = new QHBoxLayout(m_thinking);
    thinking->setContentsMargins(4, 6, 4, 4);
    thinking->setSpacing(6);
    auto* brain = new QLabel(m_thinking);
    brain->setPixmap(LC_AIIcons::lucide("brain", 13, t.textMuted, devicePixelRatioF()));
    m_thinkingText = new QLabel(tr("Thinking"), m_thinking);
    m_thinkingText->setObjectName("aiThinkingText");
    thinking->addWidget(brain);
    thinking->addWidget(m_thinkingText);
    thinking->addStretch(1);
    m_thinking->hide();
    m_layout->insertWidget(m_layout->count() - 1, m_thinking);
    m_thinkingTimer = new QTimer(this);
    m_thinkingTimer->setInterval(400);
    connect(m_thinkingTimer, &QTimer::timeout, this, [this] {
        m_dots = (m_dots + 1) % 4;
        m_thinkingText->setText(tr("Thinking") + QString(m_dots, '.'));
    });

    m_renderTimer = new QTimer(this);
    m_renderTimer->setSingleShot(true);
    m_renderTimer->setInterval(RENDER_MS);
    connect(m_renderTimer, &QTimer::timeout, this, &LC_AIChatTranscript::renderStream);

    // Follow new output while you are at the bottom; scroll up and it stays where you put it.
    QScrollBar* bar = verticalScrollBar();
    connect(bar, &QScrollBar::rangeChanged, this, [this](int, const int max) {
        if (m_follow) {
            verticalScrollBar()->setValue(max);
        }
    });
    connect(bar, &QScrollBar::valueChanged, this, [this](const int value) {
        m_follow = value >= verticalScrollBar()->maximum() - FOLLOW_SLACK;
    });
}

void LC_AIChatTranscript::add(QWidget* widget) {
    // everything goes above the "Thinking..." line and the bottom stretch
    m_layout->insertWidget(m_layout->indexOf(m_thinking), widget);
    stickToBottom();
}

void LC_AIChatTranscript::addCard(QWidget* card) {
    closeGroup(); // the card ends the block of tool calls before it
    m_follow = true; // the user asked for it: show it
    add(card);
}

void LC_AIChatTranscript::stickToBottom() {
    // the growth itself is followed in rangeChanged; this only catches up when following
    if (m_follow) {
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    }
}

QLabel* LC_AIChatTranscript::textBlock(const QString& markdown) {
    auto* label = new QLabel(m_content);
    label->setTextFormat(Qt::MarkdownText);
    label->setText(displayMarkdown(markdown));
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    label->setOpenExternalLinks(true);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setContentsMargins(2, 0, 2, 4);
    return label;
}

void LC_AIChatTranscript::addUser(const QString& text) {
    // Cypher's .msg-user: a bubble on the right, at most 80% wide
    auto* row = new QWidget(m_content);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 10, 0, 4);
    layout->addStretch(1);
    auto* bubble = new QLabel(text, row);
    bubble->setObjectName("userBubble");
    bubble->setTextFormat(Qt::PlainText);
    bubble->setWordWrap(true);
    bubble->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(bubble);
    m_bubbles << bubble;
    updateBubbleWidths();
    m_follow = true; // what you just sent, and the answer to it, come into view
    add(row);
}

void LC_AIChatTranscript::updateBubbleWidths() {
    const int max = qMax(120, int(viewport()->width() * 0.8));
    for (QLabel* bubble : std::as_const(m_bubbles)) {
        bubble->setMaximumWidth(max);
    }
}

void LC_AIChatTranscript::resizeEvent(QResizeEvent* event) {
    QScrollArea::resizeEvent(event);
    updateBubbleWidths();
}

void LC_AIChatTranscript::closeGroup() {
    if (m_group != nullptr) {
        m_group->close();
        m_group = nullptr;
    }
}

void LC_AIChatTranscript::appendDelta(const QString& text) {
    if (m_streaming == nullptr) {
        closeGroup(); // the AI is writing: the commands before it fold away
        m_streamText.clear();
        m_streamFrozen = 0;
        m_streamLabels.clear();
        m_streaming = textBlock(QString());
        m_streamLabels << m_streaming;
        add(m_streaming);
        setThinking(false);
    }
    m_streamText += text;
    // Re-rendering Markdown on every token re-lays out the whole transcript and makes it flicker and
    // fight your scrolling; coalesce the tokens instead.
    if (!m_renderTimer->isActive()) {
        m_renderTimer->start();
    }
}

void LC_AIChatTranscript::renderStream() {
    if (m_streaming == nullptr) {
        return;
    }
    QString tail = m_streamText.mid(m_streamFrozen);
    if (tail.size() > FREEZE_CHARS) {
        // hand the finished paragraphs to a block of their own, so only the last one keeps changing
        const int split = tail.lastIndexOf(QStringLiteral("\n\n"));
        const QString done = split > 0 ? tail.left(split) : QString();
        const bool balanced = done.count(QStringLiteral("```")) % 2 == 0 && done.count(QStringLiteral("~~~")) % 2 == 0;
        if (split > 0 && balanced) {
            m_streaming->setText(displayMarkdown(done));
            m_streamFrozen += split + 2;
            tail = m_streamText.mid(m_streamFrozen);
            m_streaming = textBlock(QString());
            m_streamLabels << m_streaming;
            add(m_streaming);
        }
    }
    m_streaming->setText(displayMarkdown(tail));
}

void LC_AIChatTranscript::addAssistant(const LC_AIChatMessage& message) {
    if (m_streaming != nullptr) {
        m_renderTimer->stop();
        // the final text replaces the streamed one
        if (message.text.startsWith(m_streamText.left(m_streamFrozen))) {
            m_streaming->setText(displayMarkdown(message.text.mid(m_streamFrozen)));
        }
        else {
            while (m_streamLabels.size() > 1) {
                m_streamLabels.takeLast()->deleteLater();
            }
            m_streamLabels.first()->setText(displayMarkdown(message.text));
        }
        m_streaming = nullptr;
        m_streamLabels.clear();
    }
    else if (!message.text.isEmpty()) {
        closeGroup();
        add(textBlock(message.text));
    }
    for (const LC_AIToolCall& call : message.toolCalls) {
        if (call.name == "request_clarification") { // a question card instead of a tool row
            QStringList choices;
            for (const QJsonValue& v : call.arguments.value("choices").toArray()) {
                if (!v.toString().trimmed().isEmpty()) {
                    choices << v.toString().trimmed();
                }
            }
            auto* card = new LC_AIClarificationCard(call.arguments.value("question").toString(), choices.mid(0, 8), m_theme, m_content);
            m_cards << card;
            closeGroup();
            add(card);
            continue;
        }
        auto* widget = new LC_AIToolCallWidget(call, m_labelFor ? m_labelFor(call.name) : call.name, m_theme, m_content);
        m_tools.insert(call.id, widget);
        if (m_group == nullptr) {
            m_group = new LC_AIToolGroup(m_theme, m_content);
            add(m_group);
        }
        m_group->addTool(widget);
        stickToBottom();
    }
}

void LC_AIChatTranscript::toolStarted(const LC_AIToolCall& call) {
    if (LC_AIToolCallWidget* w = m_tools.value(call.id)) {
        w->setRunning();
    }
    setThinking(false);
}

void LC_AIChatTranscript::toolAwaitingApproval(const QString& callId) {
    if (LC_AIToolCallWidget* w = m_tools.value(callId)) {
        w->setAwaitingApproval();
    }
    setThinking(false);
    stickToBottom();
}

void LC_AIChatTranscript::activateClarification(std::function<void(const QString&)> deliver) {
    for (LC_AIClarificationCard* card : std::as_const(m_cards)) {
        if (!card->isActivated()) {
            card->activate(std::move(deliver));
            card->focusInput();
            setThinking(false);
            stickToBottom();
            return;
        }
    }
}

void LC_AIChatTranscript::toolResult(const LC_AIChatMessage& message) {
    if (LC_AIToolCallWidget* w = m_tools.value(message.toolCallId)) {
        w->setResult(message.result);
    }
    stickToBottom();
}

void LC_AIChatTranscript::addError(const QString& error) {
    auto* label = new QLabel(error, m_content);
    label->setObjectName("aiError");
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setContentsMargins(2, 4, 2, 4);
    add(label);
}

void LC_AIChatTranscript::setThinking(const bool thinking) {
    m_thinking->setVisible(thinking);
    if (thinking) {
        if (!m_thinkingTimer->isActive()) {
            m_dots = 0;
            m_thinkingText->setText(tr("Thinking"));
            m_thinkingTimer->start();
        }
        stickToBottom();
    }
    else {
        m_thinkingTimer->stop();
    }
}

void LC_AIChatTranscript::showRetry(const QString& text) {
    m_thinkingTimer->stop();
    m_thinkingText->setText(text);
    m_thinking->show();
    stickToBottom();
}

void LC_AIChatTranscript::finishRun() {
    for (LC_AIToolCallWidget* w : std::as_const(m_tools)) {
        w->setStopped(); // calls that never ran (the run was stopped)
    }
    m_tools.clear();
    closeGroup();
    for (LC_AIClarificationCard* card : std::as_const(m_cards)) {
        card->setStopped(); // questions left unanswered
    }
    m_cards.clear();
    if (m_streaming != nullptr && m_renderTimer->isActive()) {
        m_renderTimer->stop();
        renderStream(); // what had arrived before the stop
    }
    m_streaming = nullptr;
    m_streamLabels.clear();
    setThinking(false);
}

void LC_AIChatTranscript::clear() {
    finishRun();
    m_bubbles.clear();
    while (m_layout->count() > 0) {
        QLayoutItem* item = m_layout->takeAt(0);
        if (QWidget* w = item->widget(); w != nullptr && w != m_thinking) {
            w->deleteLater();
        }
        delete item;
    }
    m_layout->addWidget(m_thinking);
    m_layout->addStretch(1);
}
