/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */

/*
 * Developer mode: drives the AI chat like a person, from a script, to test it end to end in the real app.
 *
 *   OPENCAD_AI_E2E=<steps.json> OPENCAD_AI_E2E_OUT=<results.json> ./librecad
 *
 * Use it with a scripted local model (scripts/ai/fake_model.py or any OpenAI-compatible server) and throwaway settings
 * (XDG_CONFIG_HOME, XDG_DATA_HOME), offscreen. Steps run in order; each is {"do": ..., ...}:
 *   setup {base_url, model, mode: supervised|edits|auto|full}   the "custom" provider at base_url, the approval mode
 *   type {text}               puts text in the message box        keys {text | keys, target: input|focus}  types key by key
 *   send                      presses send                         wait_idle {timeout}  until the AI is done
 *   wait {ms}                 click {text, in: ai|main|dialog|popup|any, index}  a visible button by text, tooltip or name
 *   drop {files: [...]}       drops files on the message box       paste_image {file}  pastes an image into it
 *   shot {file, of: ai|main|dialog|popup}                          dump {file}  the chat's text (and any popup's)
 *   expect {regex, in: ai|popup|file:<path>, not: true}            a check: recorded as passed or failed
 *   tool {name, args, out}    calls one of the AI's tools directly (set up the drawing, save it, read it back)
 *   choose {text}             picks the entry with this text in an open list popup (the @-mention completions)
 *   invoke {class | name, method, args: [strings]}  calls a slot or Q_INVOKABLE of the first widget of that class or name
 *   setting {key, value}      sets one of the AI's stored settings (LC_AICredentials::setSetting)
 *   new_drawing | activate {index} | quit
 * Results: a JSON list {step, do, ok, detail}; the app quits after the last step (or a step times out).
 */

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMainWindow>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPointer>
#include <QRegularExpression>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>

#include "lc_aiassistantwidget.h"
#include "lc_aicontrol.h"
#include "lc_aicredentials.h"
#include "lc_aipermissions.h"
#include "lc_aitool.h"
#include "lc_aitoolbar.h"
#include "qc_applicationwindow.h"

namespace {
    QC_ApplicationWindow* appWin() {
        return QC_ApplicationWindow::getAppWindow().get();
    }

    QString norm(QString s) {
        s.remove('&');
        return s.simplified().toLower();
    }

    LC_AIAssistantWidget* chat() {
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (auto* c = w->findChild<LC_AIAssistantWidget*>()) {
                return c;
            }
        }
        return nullptr;
    }

    QPlainTextEdit* input() {
        LC_AIAssistantWidget* c = chat();
        QWidget* composer = c != nullptr ? c->findChild<QWidget*>("composer") : nullptr;
        return composer != nullptr ? composer->findChild<QPlainTextEdit*>() : nullptr;
    }

    QWidget* area(const QString& where) {
        if (where == "dialog") {
            return QApplication::activeModalWidget();
        }
        if (where == "popup") {
            return QApplication::activePopupWidget();
        }
        if (where == "main") {
            return appWin();
        }
        return chat();
    }

    /** The text a person sees in this widget, top to bottom: labels, text views, lists, buttons. */
    void collectText(QWidget* root, QStringList& out) {
        if (root == nullptr) {
            return;
        }
        for (QWidget* w : root->findChildren<QWidget*>()) {
            if (!w->isVisible()) {
                continue;
            }
            if (auto* l = qobject_cast<QLabel*>(w); l != nullptr && !l->text().trimmed().isEmpty()) {
                out << l->text();
            }
            else if (auto* b = qobject_cast<QTextBrowser*>(w)) {
                out << b->toPlainText();
            }
            else if (auto* e = qobject_cast<QPlainTextEdit*>(w)) {
                out << "[input] " + e->toPlainText();
            }
            else if (auto* v = qobject_cast<QAbstractItemView*>(w); v != nullptr && v->model() != nullptr) {
                for (int r = 0; r < v->model()->rowCount() && r < 200; ++r) {
                    out << "[row] " + v->model()->index(r, 0).data().toString();
                }
            }
            else if (auto* ab = qobject_cast<QAbstractButton*>(w); ab != nullptr && !ab->text().trimmed().isEmpty()) {
                out << "[button] " + ab->text();
            }
        }
    }

    QString textOf(const QString& where) {
        QStringList out;
        if (where == "any") {
            collectText(chat(), out);
            collectText(QApplication::activePopupWidget(), out);
            collectText(QApplication::activeModalWidget(), out);
        }
        else {
            collectText(area(where), out);
        }
        return out.join('\n');
    }

    QAbstractButton* findButton(const QString& wanted, const QString& where, int index) {
        QList<QWidget*> roots;
        if (where == "any") {
            roots << QApplication::activePopupWidget() << QApplication::activeModalWidget() << chat() << appWin();
        }
        else {
            roots << area(where);
        }
        const QString w = norm(wanted);
        for (QWidget* root : roots) {
            if (root == nullptr) {
                continue;
            }
            QList<QAbstractButton*> exact, partial;
            for (QAbstractButton* b : root->findChildren<QAbstractButton*>()) {
                if (!b->isVisible() || !b->isEnabled()) {
                    continue;
                }
                const QString text = norm(b->text()), tip = norm(b->toolTip()), name = norm(b->objectName());
                if (text == w || tip == w || name == w) {
                    exact << b;
                }
                else if (text.contains(w) || tip.contains(w)) {
                    partial << b;
                }
            }
            const QList<QAbstractButton*>& pick = exact.isEmpty() ? partial : exact;
            if (index < pick.size()) {
                return pick[index];
            }
        }
        return nullptr;
    }

    void sendKeys(QWidget* widget, const QJsonObject& step) {
        // a key may close or delete the widget it went to (Return in a popup): every event checks it is still there
        QPointer<QWidget> target = widget;
        if (target == nullptr) {
            return;
        }
        const LC_AIControl::AIInput aiInput;
        if (step.contains("keys")) {
            const QKeySequence seq = QKeySequence::fromString(step.value("keys").toString(), QKeySequence::PortableText);
            for (int i = 0; i < seq.count() && target; ++i) {
                QKeyEvent press(QEvent::KeyPress, seq[i].key(), seq[i].keyboardModifiers());
                QApplication::sendEvent(target, &press);
                if (target) {
                    QKeyEvent release(QEvent::KeyRelease, seq[i].key(), seq[i].keyboardModifiers());
                    QApplication::sendEvent(target, &release);
                }
            }
            return;
        }
        for (const QChar c : step.value("text").toString()) {
            if (!target) {
                return;
            }
            const int key = c == '\n' ? Qt::Key_Return : c == '@' ? Qt::Key_At : c.toUpper().unicode();
            const QString text = c == '\n' ? QStringLiteral("\r") : QString(c);
            QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
            QApplication::sendEvent(target, &press);
            if (target) {
                QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, text);
                QApplication::sendEvent(target, &release);
            }
            QApplication::processEvents();
        }
    }

    void dropFiles(QWidget* target, const QStringList& files) {
        if (target == nullptr) {
            return;
        }
        QList<QUrl> urls;
        for (const QString& f : files) {
            urls << QUrl::fromLocalFile(f);
        }
        const LC_AIControl::AIInput aiInput;
        const QPoint at = target->rect().center();
        auto mime = std::make_unique<QMimeData>();
        mime->setUrls(urls);
        QDragEnterEvent enter(at, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &enter);
        QDragMoveEvent move(at, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &move);
        QDropEvent drop(at, Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &drop);
    }

    bool busy() {
        LC_AIAssistantWidget* c = chat();
        auto* send = c != nullptr ? c->findChild<QToolButton*>("sendBtn") : nullptr;
        return (send != nullptr && send->property("state").toString() == "stop") || LC_AIControl::instance().state() != LC_AIControl::State::Idle;
    }

    struct Driver : std::enable_shared_from_this<Driver> {
        QJsonArray steps;
        int next {0};
        QJsonArray results;
        QString out;

        void record(const QJsonObject& step, const bool ok, const QString& detail) {
            results.append(QJsonObject{{"step", next}, {"do", step.value("do")}, {"ok", ok}, {"detail", detail.left(4000)}});
            fprintf(stderr, "E2E step %d %s: %s %s\n", next, qPrintable(step.value("do").toString()), ok ? "ok" : "FAILED",
                    qPrintable(detail.left(300)));
        }

        void finish() {
            QFile f(out);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(QJsonDocument(results).toJson());
                f.close();
            }
            fflush(stderr);
            std::_Exit(0);
        }

        void go(const int delay = 50) {
            auto self = shared_from_this();
            QTimer::singleShot(delay, qApp, [self] { self->run(); });
        }

        void run() {
            if (next >= steps.size()) {
                finish();
                return;
            }
            const QJsonObject step = steps[next].toObject();
            ++next;
            const QString what = step.value("do").toString();
            auto self = shared_from_this();
            if (what == "setup") {
                LC_AICredentials& c = LC_AICredentials::instance();
                c.setApiKey("custom", step.value("api_key").toString("test-key"));
                c.setBaseUrl("custom", step.value("base_url").toString());
                c.setModel("custom", step.value("model").toString("m"));
                c.setSelectedProvider("custom");
                const QString mode = step.value("mode").toString("auto");
                LC_AIPermissions::setMode(mode == "supervised" ? LC_AIPermissions::Mode::Supervised
                                          : mode == "edits"    ? LC_AIPermissions::Mode::Edits
                                          : mode == "full"     ? LC_AIPermissions::Mode::Full
                                                               : LC_AIPermissions::Mode::Auto);
                if (LC_AIAssistantWidget* w = chat()) {
                    w->window()->show();
                }
                record(step, chat() != nullptr, chat() != nullptr ? "chat ready" : "no AI chat panel");
                go(300);
            }
            else if (what == "type") {
                QPlainTextEdit* in = input();
                if (in != nullptr) {
                    in->setFocus();
                    in->setPlainText(step.value("text").toString());
                    in->moveCursor(QTextCursor::End);
                }
                record(step, in != nullptr, {});
                go();
            }
            else if (what == "keys") {
                QWidget* target = step.value("target").toString("input") == "focus" ? QApplication::focusWidget() : input();
                if (target == input() && target != nullptr) {
                    target->setFocus();
                }
                sendKeys(target, step);
                record(step, target != nullptr, {});
                go(step.value("then_ms").toInt(150));
            }
            else if (what == "send") {
                LC_AIAssistantWidget* c = chat();
                auto* send = c != nullptr ? c->findChild<QToolButton*>("sendBtn") : nullptr;
                if (send != nullptr) {
                    send->click();
                }
                record(step, send != nullptr, {});
                go(300);
            }
            else if (what == "wait") {
                record(step, true, {});
                go(step.value("ms").toInt(500));
            }
            else if (what == "wait_idle") {
                waitIdle(step, step.value("timeout").toInt(30000), 0);
            }
            else if (what == "click") {
                QAbstractButton* b = findButton(step.value("text").toString(), step.value("in").toString("any"), step.value("index").toInt(0));
                if (b != nullptr) {
                    const LC_AIControl::AIInput aiInput;
                    QPointer<QAbstractButton> button = b;
                    QTimer::singleShot(0, qApp, [button] {
                        if (button) {
                            button->click();
                        }
                    });
                }
                record(step, b != nullptr, b != nullptr ? QString("clicked \"" + (b->text().isEmpty() ? b->toolTip() : b->text()) + "\"") : QString("no such button"));
                go(step.value("then_ms").toInt(400));
            }
            else if (what == "drop") {
                QStringList files;
                for (const QJsonValue& f : step.value("files").toArray()) {
                    files << f.toString();
                }
                QPlainTextEdit* in = input();
                dropFiles(in != nullptr ? in->viewport() : nullptr, files);
                record(step, in != nullptr, files.join(", "));
                go(400);
            }
            else if (what == "paste_image") {
                QImage image(step.value("file").toString());
                QApplication::clipboard()->setImage(image);
                QPlainTextEdit* in = input();
                if (in != nullptr) {
                    in->setFocus();
                    sendKeys(in, QJsonObject{{"keys", "Ctrl+V"}});
                }
                record(step, !image.isNull() && in != nullptr, image.isNull() ? "cannot read the image" : QString());
                go(400);
            }
            else if (what == "shot") {
                QWidget* w = area(step.value("of").toString("ai"));
                if (w == chat() && w != nullptr && w->parentWidget() != nullptr) {
                    w = w->parentWidget(); // the dock, with its title
                }
                const bool ok = w != nullptr && w->grab().save(step.value("file").toString());
                record(step, ok, step.value("file").toString());
                go();
            }
            else if (what == "dump") {
                QFile f(step.value("file").toString());
                const bool ok = f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
                if (ok) {
                    f.write(textOf(step.value("in").toString("any")).toUtf8());
                }
                record(step, ok, {});
                go();
            }
            else if (what == "expect") {
                const QString in = step.value("in").toString("any");
                QString text;
                if (in.startsWith("file:")) {
                    QFile f(in.mid(5));
                    if (f.open(QIODevice::ReadOnly)) {
                        text = QString::fromUtf8(f.readAll());
                    }
                }
                else {
                    text = textOf(in);
                }
                const QRegularExpression re(step.value("regex").toString(), QRegularExpression::CaseInsensitiveOption);
                const bool found = re.match(text).hasMatch();
                const bool ok = step.value("not").toBool() ? !found : found;
                record(step, ok, (ok ? QStringLiteral("ok: ") : QStringLiteral("FAILED: ")) + step.value("label").toString(step.value("regex").toString())
                                     + (ok ? QString() : "\n--- text was:\n" + text.right(3000)));
                go();
            }
            else if (what == "tool") {
                const QString name = step.value("name").toString();
                const LC_AITool* tool = nullptr;
                static QList<LC_AITool> all;
                all = LC_AITools::all() + LC_AIToolbar::tools();
                for (const LC_AITool& t : all) {
                    if (t.name == name) {
                        tool = &t;
                    }
                }
                if (tool == nullptr) {
                    record(step, false, "no tool " + name);
                    go();
                    return;
                }
                auto finished = [self, step](const LC_AIToolResult& r) {
                    const QString text = r.joinedText();
                    if (const QString file = step.value("out").toString(); !file.isEmpty()) {
                        QFile f(file);
                        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                            f.write(text.toUtf8());
                        }
                    }
                    self->record(step, !r.isError || step.value("may_fail").toBool(), text);
                    self->go(200);
                };
                const QJsonObject args = step.value("args").toObject();
                if (tool->executeAsync) {
                    tool->executeAsync(args, finished);
                }
                else {
                    finished(tool->execute(args));
                }
            }
            else if (what == "choose") {
                const QString wanted = norm(step.value("text").toString());
                QAbstractItemView* found = nullptr;
                QModelIndex index;
                QList<QWidget*> roots;
                roots << QApplication::activePopupWidget();
                for (QWidget* w : QApplication::topLevelWidgets()) {
                    if (w->isVisible() && (w->windowFlags() & (Qt::Popup | Qt::ToolTip | Qt::Tool | Qt::FramelessWindowHint))) {
                        roots << w;
                    }
                }
                if (LC_AIAssistantWidget* c = chat()) {
                    roots << c;
                }
                for (QWidget* root : roots) {
                    if (root == nullptr || found != nullptr) {
                        continue;
                    }
                    QList<QAbstractItemView*> views = root->findChildren<QAbstractItemView*>();
                    if (auto* v = qobject_cast<QAbstractItemView*>(root)) {
                        views.prepend(v);
                    }
                    for (QAbstractItemView* v : views) {
                        if (!v->isVisible() || v->model() == nullptr) {
                            continue;
                        }
                        for (int r = 0; r < v->model()->rowCount() && found == nullptr; ++r) {
                            const QModelIndex i = v->model()->index(r, 0);
                            if (norm(i.data().toString()).contains(wanted)) {
                                found = v;
                                index = i;
                            }
                        }
                    }
                }
                if (found != nullptr) {
                    const LC_AIControl::AIInput aiInput;
                    found->setCurrentIndex(index);
                    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
                    QApplication::sendEvent(found, &press);
                    emit found->activated(index);
                    emit found->clicked(index);
                }
                record(step, found != nullptr, found != nullptr ? "chose " + index.data().toString() : QString("no list entry like that"));
                go(400);
            }
            else if (what == "invoke") {
                QWidget* target = nullptr;
                for (QWidget* w : QApplication::allWidgets()) {
                    if ((step.contains("class") && w->inherits(qPrintable(step.value("class").toString())))
                        || (step.contains("name") && w->objectName() == step.value("name").toString())) {
                        target = w;
                        break;
                    }
                }
                bool ok = false;
                if (target != nullptr) {
                    const QJsonArray a = step.value("args").toArray();
                    const QByteArray method = step.value("method").toString().toUtf8();
                    if (a.isEmpty()) {
                        ok = QMetaObject::invokeMethod(target, method.constData());
                    }
                    else if (a.size() == 1) {
                        ok = QMetaObject::invokeMethod(target, method.constData(), Q_ARG(QString, a[0].toString()));
                    }
                    else {
                        ok = QMetaObject::invokeMethod(target, method.constData(), Q_ARG(QString, a[0].toString()), Q_ARG(QString, a[1].toString()));
                    }
                }
                record(step, ok, target == nullptr ? QString("no such widget") : ok ? QString("called") : QString("no such slot"));
                go(step.value("then_ms").toInt(400));
            }
            else if (what == "setting") {
                LC_AICredentials::instance().setSetting(step.value("key").toString(), step.value("value"));
                record(step, true, {});
                go();
            }
            else if (what == "new_drawing") {
                if (QC_ApplicationWindow* app = appWin()) {
                    app->slotFileNewFromDefaultTemplate();
                }
                record(step, true, {});
                go(800);
            }
            else if (what == "activate") {
                QC_ApplicationWindow* app = appWin();
                const QList<QMdiSubWindow*> windows = app != nullptr ? app->getMdiArea()->subWindowList() : QList<QMdiSubWindow*>();
                const int i = step.value("index").toInt();
                if (i >= 0 && i < windows.size()) {
                    app->getMdiArea()->setActiveSubWindow(windows[i]);
                }
                record(step, i >= 0 && i < windows.size(), QStringLiteral("%1 drawing windows").arg(windows.size()));
                go(600);
            }
            else if (what == "quit") {
                finish();
            }
            else {
                record(step, false, "unknown step");
                go();
            }
        }

        void waitIdle(const QJsonObject& step, const int timeout, const int waited) {
            auto self = shared_from_this();
            if (!busy()) {
                // settled: still idle a moment later (the chat may start its next request at once)
                QTimer::singleShot(400, qApp, [self, step, timeout, waited] {
                    if (busy()) {
                        self->waitIdle(step, timeout, waited + 400);
                        return;
                    }
                    self->record(step, true, QStringLiteral("idle after %1 ms").arg(waited));
                    self->go();
                });
                return;
            }
            if (waited >= timeout) {
                record(step, false, "still busy after the timeout");
                go();
                return;
            }
            QTimer::singleShot(200, qApp, [self, step, timeout, waited] { self->waitIdle(step, timeout, waited + 200); });
        }
    };

}

/** Called at startup from lc_aitoolbar.cpp (a file of its own in a static library would not be linked in). */
void LC_AIE2E::start() {
    {
        const QString path = qEnvironmentVariable("OPENCAD_AI_E2E");
        if (path.isEmpty()) {
            return;
        }
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            fprintf(stderr, "E2E: cannot read %s\n", qPrintable(path));
            std::_Exit(3);
        }
        auto driver = std::make_shared<Driver>();
        driver->steps = QJsonDocument::fromJson(f.readAll()).array();
        driver->out = qEnvironmentVariable("OPENCAD_AI_E2E_OUT", QDir::temp().filePath("opencad-ai-e2e.json"));
        // wait for the window, its first drawing and the chat panel
        auto* poll = new QTimer(qApp);
        poll->setInterval(300);
        QObject::connect(poll, &QTimer::timeout, qApp, [poll, driver] {
            static int tries = 0;
            QC_ApplicationWindow* app = appWin();
            const bool ready = app != nullptr && chat() != nullptr && app->getMdiArea() != nullptr && !app->getMdiArea()->subWindowList().isEmpty();
            if (!ready && ++tries < 100) {
                return;
            }
            poll->stop();
            poll->deleteLater();
            // a long run never hangs a test session
            QTimer::singleShot(qEnvironmentVariableIntValue("OPENCAD_AI_E2E_TIMEOUT_S") > 0 ? qEnvironmentVariableIntValue("OPENCAD_AI_E2E_TIMEOUT_S") * 1000 : 600000,
                               qApp, [driver] {
                                   driver->results.append(QJsonObject{{"step", driver->next}, {"do", "timeout"}, {"ok", false}, {"detail", "the run timed out"}});
                                   driver->finish();
                               });
            driver->go(500);
        });
        poll->start();
    }
}
