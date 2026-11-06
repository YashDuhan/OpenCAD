/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aivoiceinput.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScreen>
#include <QStyle>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <cmath>
#include <functional>

#include "lc_aicredentials.h"
#include "lc_aiicons.h"
#include "lc_aiprovider.h"
#include "lc_aivoice.h"
#include "lc_aivoicerecorder.h"

namespace {
    constexpr int HOLD_MS = 350;         // a press held longer than this is push-to-talk: letting go stops the recording
    constexpr qreal MIN_SECONDS = 0.3;   // anything shorter is a slip of the finger, not speech
    constexpr qreal MAX_SECONDS = 600;   // 10 minutes is ~19 MB of WAV, under the 25 MB upload limit of OpenAI and Groq
    constexpr int NOTE_MS = 2800;        // how long a short note ("No speech was recognized") stays
    constexpr int NO_AUDIO_MS = 1000;    // recording this long with under MIN_SECONDS of sound: the device sends nothing
    constexpr int MENU_WIDTH = 300;
#ifdef Q_OS_MACOS
    constexpr Qt::KeyboardModifier PTT_MODIFIER = Qt::MetaModifier; // the Control key: Cmd+Space is Spotlight
#else
    constexpr Qt::KeyboardModifier PTT_MODIFIER = Qt::ControlModifier;
#endif
    const QColor WAVE_COLOR("#10b981"); // Cypher's dictation meter
    constexpr qreal TAU = 6.283185307179586;

    QColor red(const LC_AITheme& t) {
        return t.dark ? QColor("#f87171") : QColor("#dc2626"); // Cypher's --accent-red
    }

    QColor alpha(QColor c, const qreal a) {
        c.setAlphaF(a);
        return c;
    }

    QString shortcutText() {
        return QKeySequence(QKeyCombination(PTT_MODIFIER, Qt::Key_Space)).toString(QKeySequence::NativeText);
    }

    /** Cypher's .voice-btn: the mic, a red stop square while recording, a blue spinner while transcribing. */
    class MicButton : public QToolButton {
    public:
        MicButton(const LC_AITheme& theme, QWidget* parent) : QToolButton(parent), m_theme(theme) {
            setFixedSize(32, 32);
            setCursor(Qt::PointingHandCursor);
            setAttribute(Qt::WA_Hover);
            setFocusPolicy(Qt::NoFocus); // the text cursor stays in the message box
            m_spin.setStartValue(0.0);
            m_spin.setEndValue(360.0);
            m_spin.setDuration(900);
            m_spin.setLoopCount(-1);
            QObject::connect(&m_spin, &QVariantAnimation::valueChanged, this, [this] { update(); });
        }

        void setLook(const LC_AIVoiceInput::State state, const bool ready) {
            m_state = state;
            m_ready = ready;
            if (state == LC_AIVoiceInput::State::Transcribing) {
                if (m_spin.state() != QAbstractAnimation::Running) {
                    m_spinner = LC_AIIcons::lucide("loader-circle", 16, m_theme.accentBlue, devicePixelRatioF());
                    m_spin.start();
                }
            }
            else {
                m_spin.stop();
            }
            update();
        }

    protected:
        void paintEvent(QPaintEvent*) override {
            QPainter p(this);
            p.setRenderHint(QPainter::Antialiasing);
            const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
            const bool hover = underMouse() && isEnabled();
            const qreal dpr = devicePixelRatioF();
            const QPointF centre = QRectF(rect()).center();
            if (m_state == LC_AIVoiceInput::State::Recording) { // .voice-btn.active-red
                p.setPen(QPen(alpha(red(m_theme), 0.4), 1));
                p.setBrush(alpha(red(m_theme), 0.15));
                p.drawRoundedRect(box, 8, 8);
                p.drawPixmap(centre - QPointF(6.5, 6.5), LC_AIIcons::lucide("square", 13, red(m_theme), dpr, true));
                return;
            }
            if (hover || isDown()) {
                p.setPen(Qt::NoPen);
                p.setBrush(m_theme.bgHover);
                p.drawRoundedRect(box, 8, 8);
            }
            if (m_state == LC_AIVoiceInput::State::Transcribing) {
                p.translate(centre);
                p.rotate(m_spin.currentValue().toReal());
                p.drawPixmap(QPointF(-8, -8), m_spinner);
                return;
            }
            const QPixmap icon = m_ready ? LC_AIIcons::lucide("mic", 16, hover ? m_theme.textPrimary : m_theme.textMuted, dpr)
                                         : LC_AIIcons::lucide("mic-off", 16, m_theme.textFaint, dpr);
            p.drawPixmap(centre - QPointF(8, 8), icon);
        }

    private:
        LC_AITheme m_theme;
        LC_AIVoiceInput::State m_state {LC_AIVoiceInput::State::Idle};
        bool m_ready {true};
        QVariantAnimation m_spin;
        QPixmap m_spinner;
    };

    /**
     * Cypher's VoiceWaveform under the composer's actions: a pulsing red dot, the time recorded, and thin green
     * bars mirrored around the centre, the newest level in the middle. Between recordings it shows a short note.
     */
    class LevelStrip : public QWidget {
    public:
        LevelStrip(const LC_AITheme& theme, QWidget* parent) : QWidget(parent), m_theme(theme) {
            setFixedHeight(20);
            setAttribute(Qt::WA_TransparentForMouseEvents);
            hide();
        }

        void startLevels() {
            m_levels.clear();
            m_text.clear();
            m_clock.start();
            show();
            update();
        }

        void push(const qreal rms) {
            // decibels, so quiet speech still moves the bars: -50 dB and below is flat, -5 dB fills the strip
            const qreal db = 20.0 * std::log10(qMax(rms, 1e-6));
            m_levels.append(qBound(0.0, (db + 50.0) / 45.0, 1.0));
            if (m_levels.size() > 200) {
                m_levels.removeFirst();
            }
            update();
        }

        void showText(const QString& text) {
            m_text = text;
            show();
            update();
        }

        bool showsText() const { return !m_text.isEmpty(); }

    protected:
        void paintEvent(QPaintEvent*) override {
            QPainter p(this);
            p.setRenderHint(QPainter::Antialiasing);
            QFont f = font();
            f.setPixelSize(12);
            p.setFont(f);
            const int h = height();
            const QFontMetrics fm(f);
            if (!m_text.isEmpty()) {
                p.setPen(m_theme.textMuted);
                p.drawText(QRect(2, 0, width() - 4, h), Qt::AlignLeft | Qt::AlignVCenter,
                           fm.elidedText(m_text, Qt::ElideRight, width() - 4));
                return;
            }
            const qint64 ms = m_clock.isValid() ? m_clock.elapsed() : 0;
            const qreal pulse = 0.6 + 0.4 * std::cos(ms / 1200.0 * TAU); // composer-pulse, 1.2 s
            p.setPen(Qt::NoPen);
            p.setBrush(alpha(red(m_theme), pulse));
            p.drawEllipse(QPointF(5, h / 2.0), 3.5, 3.5);
            const QString time = QStringLiteral("%1:%2").arg(ms / 60000).arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'));
            p.setPen(m_theme.textMuted);
            p.drawText(QRect(13, 0, 40, h), Qt::AlignLeft | Qt::AlignVCenter, time);
            const int left = 13 + fm.horizontalAdvance(QStringLiteral("00:00")) + 8;
            const int right = width() - 2;
            constexpr int bar = 2;
            constexpr int gap = 1;
            const int count = qMax(0, (right - left) / (bar + gap));
            const int barRoom = h - 4;
            p.setBrush(WAVE_COLOR);
            for (int i = 0; i < count; ++i) {
                const int fromCentre = qRound(std::abs(i - count / 2.0)); // the same level on both sides
                const qsizetype index = m_levels.size() - 1 - fromCentre;
                const qreal value = index >= 0 ? m_levels.at(index) : 0.0;
                const qreal barHeight = qMax(1.0, value * barRoom);
                p.drawRect(QRectF(left + i * (bar + gap), (h - barHeight) / 2.0, bar, barHeight));
            }
        }

    private:
        LC_AITheme m_theme;
        QList<qreal> m_levels;
        QString m_text;
        QElapsedTimer m_clock;
    };

    /** A row of the voice menu that runs a callback when clicked. */
    class ClickRow : public QFrame {
    public:
        ClickRow(std::function<void()> onClick, QWidget* parent) : QFrame(parent), m_onClick(std::move(onClick)) {
            setAttribute(Qt::WA_Hover);
            setCursor(Qt::PointingHandCursor);
        }

    protected:
        void mouseReleaseEvent(QMouseEvent* event) override {
            if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
                m_onClick();
            }
            QFrame::mouseReleaseEvent(event);
        }

    private:
        std::function<void()> m_onClick;
    };

    /** Cypher's .status-dot: green for the engine in use, an empty ring for the others. */
    QPixmap statusDot(const bool on, const LC_AITheme& t, const qreal dpr) {
        QPixmap pixmap(QSize(8, 8) * dpr);
        pixmap.fill(Qt::transparent);
        pixmap.setDevicePixelRatio(dpr);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        if (on) {
            p.setPen(Qt::NoPen);
            p.setBrush(t.accentGreen);
        }
        else {
            p.setPen(QPen(t.textFaint, 1.3));
            p.setBrush(Qt::NoBrush);
        }
        p.drawEllipse(QRectF(1, 1, 6, 6));
        return pixmap;
    }

    /** Where a preset's key comes from, after its model under its name. */
    QString keyHint(const LC_AIVoice::Endpoint& e) {
        const QString provider = LC_AIVoice::keyProviderOf(e);
        if (provider.isEmpty()) {
            return QObject::tr("no key needed");
        }
        const LC_AIProvider* p = LC_AIProvider::find(provider);
        const QString name = p != nullptr ? p->name : provider;
        if (!LC_AICredentials::instance().apiKey(provider).isEmpty()) {
            return QObject::tr("your %1 key").arg(name);
        }
        return LC_AIVoice::startsWithVowel(name) ? QObject::tr("needs an %1 key").arg(name) : QObject::tr("needs a %1 key").arg(name);
    }

    /**
     * A preset's hint, "model · key", on one line when it fits the menu, else with the key on a line of its own: never
     * cut off at the menu's edge. Its height follows the text it shows at a width, so the menu can size itself before
     * it is shown.
     */
    class HintLabel : public QLabel {
    public:
        explicit HintLabel(QWidget* parent) : QLabel(parent) {
            setWordWrap(true); // a key text longer than a line still shows whole
        }

        void setParts(const QString& model, const QString& key) {
            m_model = model;
            m_key = key;
            setText(textFor(contentsRect().width()));
        }

        int heightForWidth(const int width) const override {
            const QMargins m = contentsMargins();
            const int inner = qMax(1, width - m.left() - m.right());
            return fontMetrics().boundingRect(QRect(0, 0, inner, 100000), Qt::TextWordWrap, textFor(inner)).height() + m.top() + m.bottom();
        }

    protected:
        void resizeEvent(QResizeEvent* event) override {
            QLabel::resizeEvent(event);
            const QString shown = textFor(contentsRect().width());
            if (shown != text()) {
                setText(shown);
            }
        }

    private:
        QString textFor(const int width) const {
            const QString line = m_model + QStringLiteral(" · ") + m_key;
            const QFontMetrics fm = fontMetrics();
            if (width <= 1 || fm.horizontalAdvance(line) <= width) {
                return line;
            }
            return fm.elidedText(m_model, Qt::ElideMiddle, width) + '\n' + m_key;
        }

        QString m_model;
        QString m_key;
    };

    /**
     * Cypher's VoiceDropdown, without the read-aloud part: the speech-to-text engines, the model and server
     * of the one in use, "Send after transcribing", and how to talk.
     */
    class VoiceMenu : public QFrame {
    public:
        VoiceMenu(const LC_AITheme& theme, QWidget* parent, std::function<void()> onChanged, std::function<void()> onProviders)
            : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint), m_theme(theme), m_onChanged(std::move(onChanged)) {
            setAttribute(Qt::WA_DeleteOnClose);
            setObjectName("voiceMenu");
            const LC_AITheme& t = m_theme;
            setStyleSheet(QStringLiteral(R"(
#voiceMenu { background: %1; border: 1px solid %2; border-radius: 10px; }
#voiceItem { border: none; border-radius: 8px; background: transparent; }
#voiceItem:hover { background: %3; }
#voiceItem[on="true"] { background: %4; }
#voiceName { font-size: 13px; color: %5; }
#voiceHint { font-size: 12px; color: %6; }
#voiceGroup { font-size: 11.5px; font-weight: 500; color: %6; padding: 6px 10px 2px 10px; }
#voiceSep { background: %7; border: none; }
#voiceField { font-size: 12px; color: %6; }
#voiceMenu QLineEdit { background: %8; border: 1px solid %9; border-radius: 7px; padding: 3px 8px; font-size: 12.5px; color: %5; }
#voiceMenu QLineEdit:focus { border-color: %10; }
)")
                              .arg(t.bgElevated.name(), t.borderOverlay.name(QColor::HexArgb), t.bgHover.name(QColor::HexArgb),
                                   t.bgActive.name(QColor::HexArgb), t.textPrimary.name(), t.textFaint.name(),
                                   t.borderSubtle.name(QColor::HexArgb), t.bgCard.name(QColor::HexArgb),
                                   t.borderDefault.name(QColor::HexArgb), t.accentBlueBorder.name(QColor::HexArgb)));
            auto* layout = new QVBoxLayout(this);
            layout->setContentsMargins(4, 4, 4, 4);
            layout->setSpacing(2);
            const qreal dpr = devicePixelRatioF();

            auto* group = new QLabel(QObject::tr("Speech to text"), this);
            group->setObjectName("voiceGroup");
            layout->addWidget(group);
            for (const LC_AIVoice::Preset& preset : LC_AIVoice::presets()) {
                Row row;
                row.endpoint = preset.endpoint;
                row.frame = new ClickRow([this, preset] {
                    // keep what is being typed in the fields for the engine it was typed for, then switch
                    m_model->clearFocus();
                    m_server->clearFocus();
                    commitFields();
                    LC_AIVoice::Settings s = LC_AIVoice::settings();
                    const QString language = s.endpoint.language;
                    s.endpoint = preset.endpoint;
                    s.endpoint.language = language;
                    save(s);
                }, this);
                row.frame->setObjectName("voiceItem");
                auto* h = new QHBoxLayout(row.frame);
                h->setContentsMargins(10, 7, 10, 7);
                h->setSpacing(10);
                row.dot = new QLabel(row.frame);
                row.dot->setFixedSize(8, 16); // centred on the name's line
                auto* text = new QVBoxLayout();
                text->setSpacing(1);
                auto* name = new QLabel(preset.name, row.frame);
                name->setObjectName("voiceName");
                row.hint = new HintLabel(row.frame);
                row.hint->setObjectName("voiceHint");
                text->addWidget(name);
                text->addWidget(row.hint);
                row.check = new QLabel(row.frame);
                row.check->setPixmap(LC_AIIcons::lucide("check", 13, t.textHeading, dpr));
                h->addWidget(row.dot, 0, Qt::AlignTop);
                h->addLayout(text, 1);
                h->addWidget(row.check, 0, Qt::AlignTop);
                layout->addWidget(row.frame);
                m_rows << row;
            }
            m_keyRow = item("key-round", QObject::tr("Add a key in Providers..."), [this, onProviders] {
                close();
                onProviders();
            });
            layout->addWidget(m_keyRow);

            // the engine in use, editable: another Whisper model, a proxy, or whisper.cpp on another port
            const auto field = [this, layout](const QString& label, QLineEdit*& edit, const QString& placeholder) {
                auto* row = new QWidget(this);
                auto* h = new QHBoxLayout(row);
                h->setContentsMargins(10, 2, 10, 2);
                h->setSpacing(8);
                auto* l = new QLabel(label, row);
                l->setObjectName("voiceField");
                l->setFixedWidth(42);
                edit = new QLineEdit(row);
                edit->setPlaceholderText(placeholder);
                h->addWidget(l);
                h->addWidget(edit, 1);
                layout->addWidget(row);
            };
            layout->addWidget(separator());
            field(QObject::tr("Model"), m_model, QStringLiteral("whisper-1"));
            field(QObject::tr("Server"), m_server, QStringLiteral("https://api.openai.com/v1"));
            connect(m_model, &QLineEdit::editingFinished, this, [this] { commitFields(); });
            connect(m_server, &QLineEdit::editingFinished, this, [this] { commitFields(); });

            layout->addWidget(separator());
            m_autoSendRow = item("send-horizontal", QObject::tr("Send after transcribing"), [this] {
                LC_AIVoice::Settings s = LC_AIVoice::settings();
                s.autoSend = !s.autoSend;
                save(s);
            });
            m_autoSendCheck = m_autoSendRow->findChild<QLabel*>("voiceCheck");
            layout->addWidget(m_autoSendRow);

            layout->addWidget(separator());
            const QString device = LC_AIVoiceRecorder::deviceName();
            auto* help = new QLabel(QObject::tr("Click the mic to start and stop. Or hold the mic, or %1 in the AI panel, "
                                                "while you talk. Esc drops the recording.").arg(shortcutText())
                                        + "\n" + (device.isEmpty() ? QObject::tr("No microphone found.") : QObject::tr("Microphone: %1").arg(device)),
                                    this);
            help->setObjectName("voiceHint");
            help->setWordWrap(true);
            help->setContentsMargins(10, 4, 10, 6);
            layout->addWidget(help);
            refresh();
        }

        /** Opens above the anchor (the composer sits at the bottom of the panel), right edges aligned. */
        void popup(QWidget* anchor) {
            setFixedWidth(MENU_WIDTH);
            fitHeight();
            QPoint pos = anchor->mapToGlobal(QPoint(anchor->width() - width(), -height() - 6));
            if (QScreen* screen = anchor->screen()) {
                const QRect area = screen->availableGeometry();
                pos.setX(qBound(area.left(), pos.x(), area.right() - width()));
                if (pos.y() < area.top()) {
                    pos.setY(anchor->mapToGlobal(QPoint(0, anchor->height() + 6)).y());
                }
            }
            move(pos);
            show();
        }

    protected:
        void hideEvent(QHideEvent* event) override {
            commitFields(); // closing with a click outside still keeps what was typed
            QFrame::hideEvent(event);
        }

    private:
        struct Row {
            LC_AIVoice::Endpoint endpoint;
            ClickRow* frame {nullptr};
            QLabel* dot {nullptr};
            HintLabel* hint {nullptr};
            QLabel* check {nullptr};
        };

        ClickRow* item(const QString& icon, const QString& text, std::function<void()> onClick) {
            auto* row = new ClickRow(std::move(onClick), this);
            row->setObjectName("voiceItem");
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(10, 7, 10, 7);
            h->setSpacing(10);
            auto* ic = new QLabel(row);
            ic->setPixmap(LC_AIIcons::lucide(icon, 14, m_theme.textMuted, devicePixelRatioF()));
            auto* name = new QLabel(text, row);
            name->setObjectName("voiceName");
            auto* check = new QLabel(row);
            check->setObjectName("voiceCheck");
            check->setFixedSize(13, 13);
            h->addWidget(ic);
            h->addWidget(name, 1);
            h->addWidget(check);
            return row;
        }

        QFrame* separator() {
            auto* sep = new QFrame(this);
            sep->setObjectName("voiceSep");
            sep->setFixedHeight(1);
            return sep;
        }

        void save(const LC_AIVoice::Settings& s) {
            LC_AIVoice::setSettings(s);
            refresh();
            m_onChanged();
        }

        void commitFields() {
            LC_AIVoice::Settings s = LC_AIVoice::settings();
            const QString model = m_model->text().trimmed();
            QString server = m_server->text().trimmed();
            while (server.endsWith('/')) {
                server.chop(1);
            }
            if (model == s.endpoint.model && server == s.endpoint.baseUrl) {
                return;
            }
            s.endpoint.model = model;
            s.endpoint.baseUrl = server;
            save(s);
        }

        void refresh() {
            const LC_AIVoice::Settings s = LC_AIVoice::settings();
            const qreal dpr = devicePixelRatioF();
            for (const Row& row : std::as_const(m_rows)) {
                const bool on = row.endpoint.sameServer(s.endpoint);
                row.dot->setPixmap(statusDot(on, m_theme, dpr));
                // the model in use for the chosen engine (it may have been edited), the preset's own for the others
                LC_AIVoice::Endpoint shown = row.endpoint;
                if (on) {
                    shown.model = s.endpoint.model;
                }
                row.hint->setParts(shown.model, keyHint(shown));
                row.check->setVisible(on);
                row.frame->setProperty("on", on);
                row.frame->style()->unpolish(row.frame);
                row.frame->style()->polish(row.frame);
            }
            const QString provider = LC_AIVoice::keyProviderOf(s.endpoint);
            m_keyRow->setVisible(!provider.isEmpty() && LC_AICredentials::instance().apiKey(provider).isEmpty());
            // from their start: a long server shows its scheme and host, not the tail of its path
            if (!m_model->hasFocus()) {
                m_model->setText(s.endpoint.model);
                m_model->setCursorPosition(0);
            }
            if (!m_server->hasFocus()) {
                m_server->setText(s.endpoint.baseUrl);
                m_server->setCursorPosition(0);
            }
            m_autoSendCheck->setPixmap(s.autoSend ? LC_AIIcons::lucide("check", 13, m_theme.textHeading, dpr) : QPixmap());
            if (isVisible()) {
                fitHeight();
            }
        }

        void fitHeight() {
            // exactly as tall as its rows at this width (the help text wraps), like the approval mode menu
            ensurePolished(); // the style sheet's font sizes, which the wrapped hints are measured in
            QLayout* l = layout();
            l->activate();
            setFixedHeight(l->hasHeightForWidth() ? l->totalHeightForWidth(MENU_WIDTH) : l->totalSizeHint().height());
        }

        LC_AITheme m_theme;
        std::function<void()> m_onChanged;
        QList<Row> m_rows;
        ClickRow* m_keyRow {nullptr};
        ClickRow* m_autoSendRow {nullptr};
        QLabel* m_autoSendCheck {nullptr};
        QLineEdit* m_model {nullptr};
        QLineEdit* m_server {nullptr};
    };
}

LC_AIVoiceInput::LC_AIVoiceInput(const LC_AITheme& theme, QPlainTextEdit* input, QWidget* panel, QWidget* parent)
    : QWidget(parent), m_theme(theme), m_input(input), m_panel(panel), m_composer(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_mic = new MicButton(m_theme, this);
    layout->addWidget(m_mic);
    // Cypher's .voice-select, reduced to its chevron next to the mic
    m_menuButton = new QToolButton(this);
    m_menuButton->setObjectName("voiceSelect");
    m_menuButton->setFixedSize(16, 32);
    m_menuButton->setCursor(Qt::PointingHandCursor);
    m_menuButton->setFocusPolicy(Qt::NoFocus);
    m_menuButton->setIcon(LC_AIIcons::lucideIcon("chevron-down", 12, alpha(m_theme.textPrimary, 0.4)));
    m_menuButton->setIconSize(QSize(12, 12));
    m_menuButton->setToolTip(tr("Voice settings"));
    m_menuButton->setStyleSheet(QStringLiteral("QToolButton#voiceSelect { border: none; border-radius: 6px; background: transparent; }"
                                               "QToolButton#voiceSelect:hover { background: %1; }")
                                    .arg(m_theme.bgHover.name(QColor::HexArgb)));
    layout->addWidget(m_menuButton);

    m_strip = new LevelStrip(m_theme, parent);
    m_recorder = new LC_AIVoiceRecorder(this);
    m_network = new QNetworkAccessManager(this);
    m_tick = new QTimer(this);
    m_tick->setInterval(100);

    if (!LC_AIVoiceRecorder::available()) {
        hide(); // built without Qt Multimedia: no way to record
        return;
    }
    // the composer's border turns red while recording, as Cypher's .composer.recording
    if (m_composer != nullptr) {
        m_composer->setStyleSheet(m_composer->styleSheet()
                                  + QStringLiteral("\n#composer[voice=\"recording\"] { border-color: %1; }").arg(alpha(red(m_theme), 0.6).name(QColor::HexArgb)));
    }
    connect(m_mic, &QToolButton::pressed, this, [this] { press(false); });
    connect(m_mic, &QToolButton::released, this, &LC_AIVoiceInput::release);
    connect(m_menuButton, &QToolButton::clicked, this, &LC_AIVoiceInput::openMenu);
    connect(m_recorder, &LC_AIVoiceRecorder::level, this, [this](const qreal rms) { static_cast<LevelStrip*>(m_strip)->push(rms); });
    connect(m_recorder, &LC_AIVoiceRecorder::failed, this, [this](const QString& error) {
        setState(State::Idle);
        fail(error);
    });
    connect(m_tick, &QTimer::timeout, this, [this] {
        m_strip->update(); // the clock and the pulse
        if (m_recorder->seconds() >= MAX_SECONDS) {
            stopRecording();
        }
    });
    // push-to-talk and Esc: the keys reach whichever widget of the panel has focus, so watch them application-wide
    qApp->installEventFilter(this);
    refresh();
}

LC_AIVoiceInput::~LC_AIVoiceInput() {
    ++m_generation;
    if (m_reply != nullptr) {
        m_reply->abort();
    }
}

QWidget* LC_AIVoiceInput::levelStrip() const {
    return m_strip;
}

bool LC_AIVoiceInput::panelHasFocus() const {
    const QWidget* focus = QApplication::focusWidget();
    return focus != nullptr && m_panel != nullptr && (focus == m_panel || m_panel->isAncestorOf(focus));
}

bool LC_AIVoiceInput::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
        case QEvent::ShortcutOverride:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
            break;
        case QEvent::ApplicationDeactivate:
            m_spaceDown = false; // the key-up goes to another application
            if (m_held && !m_mic->isDown()) {
                release();
            }
            return false;
        case QEvent::Enter:
            if (watched == m_mic) {
                refresh(); // a key may have been added meanwhile
            }
            return false;
        default:
            return false;
    }
    const auto* key = static_cast<QKeyEvent*>(event);
    const Qt::KeyboardModifiers modifiers = key->modifiers() & ~Qt::KeypadModifier;
    const bool pushToTalk = key->key() == Qt::Key_Space && modifiers == PTT_MODIFIER;
    const bool escape = key->key() == Qt::Key_Escape && modifiers == Qt::NoModifier && m_state != State::Idle;
    const bool space = key->key() == Qt::Key_Space;
    // an open popup gets the keys (Qt sends them to it while the focus stays in the message box): Esc is for closing
    // it, not for dropping the recording behind it, and push-to-talk closes the voice menu rather than stacking another
    QWidget* popup = QApplication::activePopupWidget();
    if (event->type() == QEvent::KeyRelease) {
        // letting go of Space or of Ctrl ends push-to-talk, whichever comes first
        const bool ours = space && m_spaceDown;
        if (ours && !key->isAutoRepeat()) {
            m_spaceDown = false;
        }
        const bool modifierUp = key->key() == Qt::Key_Control || key->key() == Qt::Key_Meta;
        if (m_held && !m_mic->isDown() && !key->isAutoRepeat() && (ours || modifierUp)) {
            release();
        }
        return ours;
    }
    if (event->type() == QEvent::KeyPress && space && m_spaceDown && key->isAutoRepeat()) {
        return true; // the held key repeating, also after Ctrl was let go first: no spaces in the message
    }
    if (popup != nullptr && (!pushToTalk || popup != m_menu)) {
        return false;
    }
    if (popup != nullptr) {
        if (event->type() == QEvent::KeyPress && !key->isAutoRepeat()) {
            m_spaceDown = true; // its repeats and key-up are ours: no spaces in the message once the menu is gone
            popup->close();
        }
        event->accept();
        return true;
    }
    if ((!pushToTalk && !escape) || !panelHasFocus()) {
        return false;
    }
    if (event->type() == QEvent::ShortcutOverride) {
        event->accept(); // the AI panel takes these keys before any shortcut of the main window
        return true;
    }
    if (escape) {
        cancel();
    }
    else if (!key->isAutoRepeat()) {
        m_spaceDown = true;
        press(true);
    }
    return true;
}

void LC_AIVoiceInput::press(const bool fromKey) {
    m_startedByPress = false;
    m_menuOnRelease = false;
    switch (m_state) {
        case State::Recording: // started by a tap: this press ends it
            stopRecording();
            return;
        case State::Transcribing:
            if (!fromKey) {
                cancel(); // the spinner doubles as a cancel button
            }
            return;
        case State::Idle:
            break;
    }
    if (!LC_AIVoice::ready(LC_AIVoice::settings().endpoint)) {
        // the menu says what is missing; a click opens it on release, so the popup does not catch the button's release
        if (fromKey) {
            openMenu();
        }
        else {
            m_menuOnRelease = true;
        }
        return;
    }
    m_held = true;
    m_startedByPress = true;
    startRecording();
    // the hold counts from when the microphone is open: a sound server slow to open it (a busy machine) must not turn
    // a click into a hold whose release stops the recording at once
    m_pressed.start();
}

void LC_AIVoiceInput::release() {
    if (m_menuOnRelease) {
        m_menuOnRelease = false;
        openMenu();
        return;
    }
    if (!m_held) {
        return;
    }
    m_held = false;
    if (m_state == State::Recording && m_startedByPress && m_pressed.elapsed() >= HOLD_MS) {
        stopRecording();
    }
    // a tap keeps recording, as a click on the mic: the next tap or click stops it
}

void LC_AIVoiceInput::startRecording() {
    setState(State::Recording);
    m_recorder->start(); // failed() sets the state back
    m_recording.start(); // also from when the device is open: a slow sound server is not a silent microphone
}

void LC_AIVoiceInput::stopRecording() {
    if (m_state != State::Recording) {
        return;
    }
    m_held = false;
    transcribe(m_recorder->stop());
}

void LC_AIVoiceInput::transcribe(const QByteArray& wav) {
    const qreal seconds = qMax(0, int(wav.size()) - 44) / (2.0 * LC_AIVoice::SAMPLE_RATE);
    if (seconds < MIN_SECONDS) {
        setState(State::Idle);
        // the clock ran but the device delivered (next to) nothing: it is muted, suspended or not connected, and
        // holding the key longer would not help
        if (m_recording.isValid() && m_recording.elapsed() >= NO_AUDIO_MS) {
            showNote(tr("The microphone sent no sound. Is it muted?"));
        }
        else {
            showNote(tr("Too short. Hold the mic or %1 while you talk.").arg(shortcutText()));
        }
        return;
    }
    setState(State::Transcribing);
    const int run = ++m_generation;
    const LC_AIVoice::Settings settings = LC_AIVoice::settings();
    m_reply = LC_AIVoice::transcribe(m_network, settings.endpoint, wav, this, [this, run](const QString& text, const QString& error) {
        if (run != m_generation) {
            return; // cancelled
        }
        m_reply = nullptr;
        setState(State::Idle);
        if (!error.isEmpty()) {
            fail(error);
            return;
        }
        m_lastError.clear(); // voice input works: a later error is news again
        if (text.isEmpty()) {
            showNote(tr("No speech was recognized."));
            return;
        }
        insertTranscript(text);
        if (LC_AIVoice::settings().autoSend) {
            emit sendRequested();
        }
    });
}

void LC_AIVoiceInput::showNote(const QString& text) {
    static_cast<LevelStrip*>(m_strip)->showText(text);
    const int run = m_generation;
    const int note = ++m_note;
    QTimer::singleShot(qMax(NOTE_MS, int(text.size()) * 60), this, [this, run, note] { // time to read a longer one
        if (m_state == State::Idle && run == m_generation && note == m_note) { // not a newer note's time
            m_strip->hide();
        }
    });
}

void LC_AIVoiceInput::fail(const QString& error) {
    if (error == m_lastError) {
        showNote(error); // clicked again with no microphone: one bubble says it, the strip answers the retries
        return;
    }
    m_lastError = error;
    emit failed(error);
}

void LC_AIVoiceInput::cancel() {
    m_held = false;
    ++m_generation;
    m_recorder->cancel();
    if (m_reply != nullptr) {
        QNetworkReply* reply = m_reply;
        m_reply = nullptr;
        reply->abort();
    }
    setState(State::Idle);
}

void LC_AIVoiceInput::insertTranscript(const QString& text) {
    // appended after what is typed, with one space between (Cypher's editorText with append)
    const QString current = m_input->toPlainText();
    qsizetype trailing = 0;
    while (trailing < current.size() && current.at(current.size() - 1 - trailing).isSpace()) {
        ++trailing;
    }
    QTextCursor cursor(m_input->document());
    cursor.beginEditBlock();
    cursor.movePosition(QTextCursor::End);
    cursor.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, int(trailing));
    cursor.insertText(current.trimmed().isEmpty() ? text : " " + text);
    cursor.endEditBlock();
    cursor.movePosition(QTextCursor::End);
    m_input->setTextCursor(cursor);
    m_input->setFocus();
}

void LC_AIVoiceInput::setState(const State state) {
    m_state = state;
    auto* strip = static_cast<LevelStrip*>(m_strip);
    switch (state) {
        case State::Recording:
            strip->startLevels();
            m_tick->start();
            break;
        case State::Transcribing:
            m_tick->stop();
            strip->showText(tr("Transcribing..."));
            break;
        case State::Idle:
            m_tick->stop();
            strip->hide();
            break;
    }
    if (m_composer != nullptr) {
        const QString look = state == State::Recording ? QStringLiteral("recording") : QString();
        if (m_composer->property("voice").toString() != look) {
            m_composer->setProperty("voice", look);
            m_composer->style()->unpolish(m_composer);
            m_composer->style()->polish(m_composer);
        }
    }
    refresh();
}

void LC_AIVoiceInput::refresh() {
    QString why;
    const bool ready = LC_AIVoice::ready(LC_AIVoice::settings().endpoint, &why);
    static_cast<MicButton*>(m_mic)->setLook(m_state, ready);
    switch (m_state) {
        case State::Recording:
            m_mic->setToolTip(tr("Stop and transcribe (click, or %1)").arg(shortcutText()));
            break;
        case State::Transcribing:
            m_mic->setToolTip(tr("Transcribing... click to cancel"));
            break;
        case State::Idle:
            m_mic->setToolTip(ready ? tr("Dictate: click to start and stop, or hold the mic or %1 while you talk").arg(shortcutText())
                                    : tr("Voice input: %1").arg(why));
            break;
    }
}

void LC_AIVoiceInput::openMenu() {
    if (m_menu != nullptr && m_menu->isVisible()) {
        return; // already open: a second one would stack on it
    }
    auto* menu = new VoiceMenu(m_theme, this, [this] { refresh(); }, [this] { emit providersRequested(); });
    m_menu = menu;
    menu->popup(this);
}

void LC_AIVoiceInput::hideEvent(QHideEvent* event) {
    // the AI panel was closed: never keep the microphone open behind the user's back
    if (!event->spontaneous() && m_state == State::Recording) {
        cancel();
    }
    QWidget::hideEvent(event);
}

void LC_AIVoiceInput::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refresh();
}
