/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aibrowser.h"

#ifndef LC_AI_BROWSER

bool LC_AIBrowser::available() {
    return false;
}

void LC_AIBrowser::setHost(QWidget*) {
}

void LC_AIBrowser::showWindow(QWidget*) {
}

QList<LC_AITool> LC_AIBrowser::tools() {
    return {};
}

#else

#include <QApplication>
#include <QBuffer>
#include <QCloseEvent>
#include <QFrame>
#include <QJsonObject>
#include <QMouseEvent>
#include <algorithm>
#include <cmath>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QToolButton>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWebEngineDownloadRequest>
#include <QWebEngineHistory>
#include <QWebEngineLoadingInfo>
#include <QWebEngineNewWindowRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>
#include <memory>
#include <utility>

#include "lc_aicredentials.h"
#include "lc_aiicons.h"
#include "lc_aiprovidersdialog.h"
#include "qc_applicationwindow.h"

namespace {
    constexpr int LOAD_TIMEOUT_MS = 25000;
    constexpr int JS_TIMEOUT_MS = 10000;    // a page script that never answers (the page is stuck or was replaced)
    constexpr int SETTLE_MS = 900;          // after a click: time for a navigation to start
    constexpr int NAVIGATE_TEXT = 4000;     // page text returned with browser_navigate
    constexpr int READ_TEXT = 20000;        // browser_read, like Cypher
    constexpr int MAX_SCREENSHOT_EDGE = 1280;

    using Done = std::function<void(const LC_AIToolResult&)>;

    bool webUrl(const QUrl& url) {
        return url.scheme() == "http" || url.scheme() == "https" || url.toString() == "about:blank";
    }

    /** Keeps the AI inside this window: http(s) only, and pages that open a new window open here instead. */
    class Page : public QWebEnginePage {
    public:
        Page(QWebEngineProfile* profile, QObject* parent) : QWebEnginePage(profile, parent) {
            // target=_blank links and window.open: the address opens in this window. Not by handing Chromium this page
            // for the new window (createWindow returning this): that swapped the page's contents under a running
            // browser_click, whose script then never answered. The popup itself is dropped (no openIn).
            connect(this, &QWebEnginePage::newWindowRequested, this, [this](QWebEngineNewWindowRequest& request) {
                const QUrl url = request.requestedUrl();
                if (webUrl(url) && url.toString() != QLatin1String("about:blank")) {
                    QTimer::singleShot(0, this, [this, url] { load(url); }); // after the click's script has answered
                }
            });
        }

    protected:
        bool acceptNavigationRequest(const QUrl& url, NavigationType, bool) override {
            return webUrl(url); // no file:, mailto:, javascript:, custom app links
        }
    };

    constexpr int PAGE_WIDTH = 1280; // pages are laid out like a laptop screen, then shown scaled (Cypher's FLOAT_VIEWPORT)
    constexpr int PAGE_HEIGHT = 800;
    constexpr double ASPECT = double(PAGE_WIDTH) / PAGE_HEIGHT;
    constexpr int GAP = 12;
    const QSize DEFAULT_FLOAT(400, 250); // Cypher's floating browser: 400 x 250, at least 240 x 150
    const QSize MIN_FLOAT(240, 150);
    const QString FLOAT_KEY = QStringLiteral("browserFloatWindow"); // position over the OpenCAD window

    QPointer<QWidget> g_host; // the AI panel the preview floats over

    class Browser;
    Browser* window();

    /** Cypher's FloatingPreview: just the page, small, over the chat. Grip to move, edges to resize. */
    class FloatPreview : public QFrame {
    public:
        /** host: the whole OpenCAD window it floats over; anchor: the AI chat it first opens beside. */
        FloatPreview(QWidget* host, QWidget* anchor, std::function<void()> onExpand, std::function<void()> onClose)
            : QFrame(host), m_host(host), m_anchor(anchor) {
            setObjectName("aiBrowserFloat");
            setAttribute(Qt::WA_Hover);
            const LC_AITheme t = LC_AITheme::current();
            setStyleSheet(QStringLiteral(
                "#aiBrowserFloat { background: %1; border: 1px solid %2; border-radius: 10px; }"
                "#fpGrip, #fpButton { background: %3; border: 1px solid %2; border-radius: 6px; }"
                "#fpButton:hover { background: %4; }")
                              .arg(t.bgElevated.name(), t.borderOverlay.name(QColor::HexArgb), t.bgElevated.name(),
                                   t.bgHover.name(QColor::HexArgb)));
            m_layout = new QVBoxLayout(this);
            m_layout->setContentsMargins(1, 1, 1, 1);
            const qreal dpr = devicePixelRatioF();
            m_grip = new QLabel(this);
            m_grip->setObjectName("fpGrip");
            m_grip->setPixmap(LC_AIIcons::lucide("grip-horizontal", 14, t.textMuted, dpr));
            m_grip->setAlignment(Qt::AlignCenter);
            m_grip->setFixedSize(40, 18);
            m_grip->setCursor(Qt::OpenHandCursor);
            m_grip->setToolTip(QObject::tr("Drag to move"));
            m_grip->installEventFilter(this);
            const auto button = [&](const QString& icon, const QString& tip, std::function<void()> onClick) {
                auto* b = new QToolButton(this);
                b->setObjectName("fpButton");
                b->setIcon(QIcon(LC_AIIcons::lucide(icon, 12, t.textSecondary, dpr)));
                b->setIconSize(QSize(12, 12));
                b->setFixedSize(22, 22);
                b->setToolTip(tip);
                b->setCursor(Qt::PointingHandCursor);
                connect(b, &QToolButton::clicked, this, [onClick] { onClick(); });
                return b;
            };
            m_expand = button("maximize-2", QObject::tr("Open the full browser window"), std::move(onExpand));
            m_close = button("x", QObject::tr("Hide the preview (the AI shows it again when it browses)"), std::move(onClose));
            // resize from every edge and corner: n, s, e, w, ne, nw, se, sw
            const QStringList directions{"n", "s", "e", "w", "ne", "nw", "se", "sw"};
            for (const QString& d : directions) {
                auto* h = new QWidget(this);
                h->setProperty("dir", d);
                h->setCursor(d == "n" || d == "s" ? Qt::SizeVerCursor : d == "e" || d == "w" ? Qt::SizeHorCursor
                             : d == "ne" || d == "sw" ? Qt::SizeBDiagCursor : Qt::SizeFDiagCursor);
                h->installEventFilter(this);
                m_handles << h;
            }
            setControlsVisible(false);
            if (m_host != nullptr) {
                m_host->installEventFilter(this);
            }
            restoreFrame();
        }

        void setPage(QWidget* page) {
            m_layout->addWidget(page);
            raiseControls();
            applyZoom();
        }

        void fitToHost() {
            setGeometry(fit(geometry()));
            applyZoom();
            raise(); // stay above the AI sidebar, which raises itself when the window resizes
        }

    protected:
        bool event(QEvent* e) override {
            if (e->type() == QEvent::HoverEnter) {
                setControlsVisible(true);
            }
            else if (e->type() == QEvent::HoverLeave && m_drag.isEmpty()) {
                setControlsVisible(false);
            }
            return QFrame::event(e);
        }

        void resizeEvent(QResizeEvent* e) override {
            QFrame::resizeEvent(e);
            const int w = width();
            const int h = height();
            m_grip->move((w - m_grip->width()) / 2, 8);
            m_close->move(w - 8 - m_close->width(), 8);
            m_expand->move(m_close->x() - 4 - m_expand->width(), 8);
            constexpr int E = 6; // edge thickness
            for (QWidget* handle : std::as_const(m_handles)) {
                const QString d = handle->property("dir").toString();
                QRect r;
                if (d == "n") r = QRect(E, 0, w - 2 * E, E);
                else if (d == "s") r = QRect(E, h - E, w - 2 * E, E);
                else if (d == "e") r = QRect(w - E, E, E, h - 2 * E);
                else if (d == "w") r = QRect(0, E, E, h - 2 * E);
                else if (d == "ne") r = QRect(w - 2 * E, 0, 2 * E, 2 * E);
                else if (d == "nw") r = QRect(0, 0, 2 * E, 2 * E);
                else if (d == "se") r = QRect(w - 2 * E, h - 2 * E, 2 * E, 2 * E);
                else r = QRect(0, h - 2 * E, 2 * E, 2 * E);
                handle->setGeometry(r);
            }
            raiseControls();
            applyZoom();
        }

        bool eventFilter(QObject* watched, QEvent* e) override {
            if (watched == m_host) {
                if (e->type() == QEvent::Resize && isVisible()) {
                    fitToHost();
                }
                return false;
            }
            auto* w = qobject_cast<QWidget*>(watched);
            const QString dir = watched == m_grip ? QStringLiteral("move") : (w != nullptr ? w->property("dir").toString() : QString());
            if (dir.isEmpty()) {
                return false;
            }
            if (e->type() == QEvent::MouseButtonPress) {
                auto* me = static_cast<QMouseEvent*>(e);
                if (me->button() == Qt::LeftButton) {
                    m_drag = dir;
                    m_startPos = me->globalPosition().toPoint();
                    m_startFrame = geometry();
                    return true;
                }
            }
            else if (e->type() == QEvent::MouseMove && !m_drag.isEmpty()) {
                const QPoint d = static_cast<QMouseEvent*>(e)->globalPosition().toPoint() - m_startPos;
                setGeometry(m_drag == "move" ? fit(m_startFrame.translated(d)) : fit(resized(m_startFrame, m_drag, d)));
                return true;
            }
            else if (e->type() == QEvent::MouseButtonRelease && !m_drag.isEmpty()) {
                m_drag.clear();
                saveFrame();
                return true;
            }
            return false;
        }

    private:
        void setControlsVisible(const bool on) {
            m_grip->setVisible(on);
            m_expand->setVisible(on);
            m_close->setVisible(on);
        }

        void raiseControls() {
            for (QWidget* h : std::as_const(m_handles)) {
                h->raise();
            }
            m_grip->raise();
            m_expand->raise();
            m_close->raise();
        }

        void applyZoom() {
            // the page keeps its laptop layout and is shown scaled down, like Cypher's preview
            if (m_layout->count() > 0) {
                if (auto* view = qobject_cast<QWebEngineView*>(m_layout->itemAt(0)->widget())) {
                    view->setZoomFactor(qBound(0.25, (width() - 2) / double(PAGE_WIDTH), 1.0));
                }
            }
        }

        QSize area() const {
            return m_host != nullptr ? m_host->size() : QSize(1280, 800);
        }

        /** Cypher's fit(): size within limits keeping the aspect, at most 45% of the window's width, then inside its edges. */
        QRect fit(const QRect& frame) const {
            const QSize a = area();
            const int maxW = std::max(MIN_FLOAT.width(), std::min(a.width() - GAP * 2, int(a.width() * 0.45)));
            const int maxH = std::max(MIN_FLOAT.height(), a.height() - GAP * 2);
            int w = std::clamp(frame.width(), std::max(MIN_FLOAT.width(), int(MIN_FLOAT.height() * ASPECT)),
                               std::max(MIN_FLOAT.width(), std::min(maxW, int(maxH * ASPECT))));
            const int h = int(std::round(w / ASPECT));
            const int x = std::clamp(frame.x(), GAP, std::max(GAP, a.width() - w - GAP));
            const int y = std::clamp(frame.y(), GAP, std::max(GAP, a.height() - h - GAP));
            return {x, y, w, h};
        }

        /** Cypher's resize(): corners and side edges drive the width, top/bottom the height; opposite edges stay put. */
        static QRect resized(const QRect& start, const QString& dir, const QPoint& d) {
            int w = start.width();
            int h = start.height();
            if (dir.contains('e')) w = start.width() + d.x();
            else if (dir.contains('w')) w = start.width() - d.x();
            if (dir == "n") h = start.height() - d.y();
            else if (dir == "s") h = start.height() + d.y();
            if (dir == "n" || dir == "s") w = int(h * ASPECT);
            h = int(std::round(w / ASPECT));
            const int x = dir.contains('w') ? start.right() + 1 - w : start.x();
            const int y = dir.contains('n') ? start.bottom() + 1 - h : start.y();
            return {x, y, w, h};
        }

        void restoreFrame() {
            const QJsonObject f = LC_AICredentials::instance().setting(FLOAT_KEY).toObject();
            QRect frame;
            if (f.contains("w")) {
                frame = QRect(f.value("x").toInt(), f.value("y").toInt(), f.value("w").toInt(), f.value("h").toInt());
            }
            else { // just left of the AI chat, over the drawing, so it never covers the conversation
                const QRect sized = fit(QRect(0, 0, DEFAULT_FLOAT.width(), DEFAULT_FLOAT.height()));
                int x = area().width() - sized.width() - GAP;
                int y = GAP + 60;
                if (m_anchor != nullptr && m_host != nullptr) {
                    const QPoint chat = m_anchor->mapTo(m_host, QPoint(0, 0));
                    x = chat.x() - sized.width() - GAP;
                    y = chat.y() + GAP;
                }
                frame = QRect(x, y, sized.width(), sized.height());
            }
            setGeometry(fit(frame));
        }

        void saveFrame() {
            const QRect g = geometry();
            LC_AICredentials::instance().setSetting(FLOAT_KEY, QJsonObject{{"x", g.x()}, {"y", g.y()}, {"w", g.width()}, {"h", g.height()}});
        }

        QPointer<QWidget> m_host;
        QPointer<QWidget> m_anchor;
        QVBoxLayout* m_layout {nullptr};
        QLabel* m_grip {nullptr};
        QToolButton* m_expand {nullptr};
        QToolButton* m_close {nullptr};
        QList<QWidget*> m_handles;
        QString m_drag;
        QPoint m_startPos;
        QRect m_startFrame;
    };

    /** The full window with back / forward / reload / address bar, for when the user wants a closer look. */
    class FullWindow : public QWidget {
    public:
        FullWindow(QWidget* parent, std::function<void()> onCollapse) : QWidget(parent, Qt::Window | Qt::Tool),
                                                                        m_onCollapse(std::move(onCollapse)) {
            setWindowTitle(QObject::tr("AI Browser"));
            const LC_AITheme t = LC_AITheme::current();
            setStyleSheet(QStringLiteral(
                "#browserBar { background: %1; border-bottom: 1px solid %2; }"
                "#browserBar QToolButton { border: none; border-radius: 6px; padding: 4px; background: transparent; }"
                "#browserBar QToolButton:hover { background: %3; }"
                "#browserUrl { background: %4; border: 1px solid %2; border-radius: 7px; padding: 4px 8px; color: %5; font-size: 12.5px; }")
                              .arg(t.bgElevated.name(), t.borderDefault.name(QColor::HexArgb), t.bgHover.name(QColor::HexArgb),
                                   t.bgCard.name(QColor::HexArgb), t.textPrimary.name()));
            m_layout = new QVBoxLayout(this);
            m_layout->setContentsMargins(0, 0, 0, 0);
            m_layout->setSpacing(0);
            auto* bar = new QWidget(this);
            bar->setObjectName("browserBar");
            m_bar = new QHBoxLayout(bar);
            m_bar->setContentsMargins(8, 6, 8, 6);
            m_bar->setSpacing(4);
            m_layout->addWidget(bar);
            resize(1000, 720);
            if (parent != nullptr && parent->screen() != nullptr) {
                const QRect area = parent->screen()->availableGeometry();
                move(area.center() - rect().center());
            }
        }

        QToolButton* addButton(const QString& icon, const QString& tip) {
            auto* b = new QToolButton(this);
            b->setIcon(QIcon(LC_AIIcons::lucide(icon, 15, LC_AITheme::current().textSecondary, devicePixelRatioF())));
            b->setIconSize(QSize(15, 15));
            b->setToolTip(tip);
            b->setCursor(Qt::PointingHandCursor);
            m_bar->addWidget(b);
            return b;
        }
        void addBarWidget(QWidget* w, const int stretch) { m_bar->addWidget(w, stretch); }
        void setPage(QWidget* page) { m_layout->addWidget(page, 1); }

    protected:
        void closeEvent(QCloseEvent* e) override {
            QWidget::closeEvent(e);
            m_onCollapse(); // the page goes back to the small preview
        }

    private:
        QVBoxLayout* m_layout {nullptr};
        QHBoxLayout* m_bar {nullptr};
        std::function<void()> m_onCollapse;
    };

    /** Why a page did not load, in words for the AI: the HTTP status, the network error, a download, a stop. */
    QString loadFailure(const QWebEngineLoadingInfo& info) {
        if (info.isDownload()) {
            return QStringLiteral("the address is a file download, and the AI browser does not download files");
        }
        if (info.status() == QWebEngineLoadingInfo::LoadStoppedStatus) {
            return QStringLiteral("loading was stopped");
        }
        if (info.errorDomain() == QWebEngineLoadingInfo::HttpStatusCodeDomain) {
            return QStringLiteral("the server answered HTTP %1").arg(info.errorCode());
        }
        const QString error = info.errorString().trimmed();
        return error.isEmpty() ? QStringLiteral("the page could not be loaded (error %1)").arg(info.errorCode())
                               : QStringLiteral("the page could not be loaded (%1)").arg(error);
    }

    /** The page and its private session; shown in the small preview (default) or the full window. */
    class Browser : public QObject {
    public:
        /** ok, and when not ok why (an HTTP status, a network error, a download, the time-out). */
        using LoadDone = std::function<void(bool, const QString&)>;

        Browser() {
            // private session: nothing is kept between runs, and it is separate from the user's own browser
            m_profile = new QWebEngineProfile(this);
            connect(m_profile, &QWebEngineProfile::downloadRequested, this, [this](QWebEngineDownloadRequest* download) {
                download->cancel(); // lookups only: no downloads
                // a download is no page: the load waiting for it ends here, whether or not Chromium reports it too
                m_note = QStringLiteral("the address is a file download (%1), and the AI browser does not download files")
                             .arg(download->downloadFileName());
                finishLoad(false, m_note);
            });
            m_view = new QWebEngineView();
            m_view->setPage(new Page(m_profile, m_view));
            m_view->resize(PAGE_WIDTH, PAGE_HEIGHT);
            connect(m_view->page(), &QWebEnginePage::loadingChanged, this, [this](const QWebEngineLoadingInfo& info) {
                if (info.status() == QWebEngineLoadingInfo::LoadStartedStatus) {
                    m_loading = true;
                    return;
                }
                const bool ok = info.status() == QWebEngineLoadingInfo::LoadSucceededStatus;
                finishLoad(ok, ok ? QString() : loadFailure(info));
            });
        }

        QWebEngineView* view() const { return m_view; }

        /** False until a page was opened: reading, listing, clicking or scrolling then says so instead of answering empty. */
        bool hasPage() const {
            const QUrl url = m_view->url();
            return !url.isEmpty() && url.toString() != QLatin1String("about:blank");
        }

        /** Shows where the AI is browsing: the full window if the user opened it, else the small preview. */
        void reveal() {
            if (m_full != nullptr && m_full->isVisible()) {
                m_full->raise();
                return;
            }
            FloatPreview* f = preview();
            if (m_view->parentWidget() != f) {
                f->setPage(m_view);
            }
            f->fitToHost();
            f->show();
            f->raise();
        }

        void toggle() {
            if (m_full != nullptr && m_full->isVisible()) {
                m_full->raise();
            }
            else if (m_float != nullptr && m_float->isVisible()) {
                m_float->hide();
            }
            else {
                reveal();
            }
        }

        void waitForLoad(const LoadDone& done) {
            auto once = std::make_shared<bool>(false);
            const auto finish = [once, done](const bool ok, const QString& why) {
                if (!*once) {
                    *once = true;
                    done(ok, why);
                }
            };
            m_waiters.push_back(finish);
            QTimer::singleShot(LOAD_TIMEOUT_MS, this, [this, finish, once] {
                if (!*once) {
                    finish(false, QStringLiteral("it did not finish loading within %1 s (loading was stopped)").arg(LOAD_TIMEOUT_MS / 1000));
                    m_view->stop(); // the page should not change under the AI after the tool has answered
                }
            });
        }

        void load(const QUrl& url, const LoadDone& done) {
            reveal();
            waitForLoad(done);
            m_view->load(url);
        }

        /** After a click or a back step: done(problem) once a navigation it started has finished; problem is empty
         *  when it loaded, or nothing was loading. */
        void settle(const std::function<void(const QString&)>& done) {
            m_note.clear();
            QTimer::singleShot(SETTLE_MS, this, [this, done] {
                if (m_loading) {
                    waitForLoad([done](const bool ok, const QString& why) { done(ok ? QString() : why); });
                }
                else {
                    done(std::exchange(m_note, QString())); // e.g. the click started a download, which was refused
                }
            });
        }

        /**
         * Runs a script in the page; its answer goes to then. When none comes within JS_TIMEOUT_MS (the page is stuck,
         * or was replaced and Chromium dropped the call), the tool fails through fail instead of running forever.
         */
        void js(const QString& script, const Done& fail, const std::function<void(const QVariant&)>& then) {
            auto once = std::make_shared<bool>(false);
            m_view->page()->runJavaScript(script, [once, then](const QVariant& v) {
                if (!*once) {
                    *once = true;
                    then(v);
                }
            });
            QTimer::singleShot(JS_TIMEOUT_MS, this, [once, fail] {
                if (!*once) {
                    *once = true;
                    fail(LC_AIToolResult::error(QStringLiteral("The page did not answer within %1 s (it may be busy or still "
                                                               "loading); try again, or open it anew with browser_navigate.")
                                                    .arg(JS_TIMEOUT_MS / 1000)));
                }
            });
        }

        /**
         * One step back (or forward) in the history. Not QWebEngineView::back(): Chromium skips history entries of pages
         * that moved on without a user's gesture, which every script click of browser_click is, so after a click
         * back() did nothing. Going to the entry itself does not skip. False when there is no such page.
         */
        bool step(const int offset) {
            QWebEngineHistory* history = m_view->history();
            const int index = history->currentItemIndex() + offset;
            if (index < 0 || index >= history->count()) {
                return false;
            }
            history->goToItem(history->itemAt(index));
            return true;
        }

    private:
        void finishLoad(const bool ok, const QString& why) {
            m_loading = false;
            const auto waiters = std::move(m_waiters);
            m_waiters.clear();
            for (const auto& w : waiters) {
                w(ok, why);
            }
        }

        FloatPreview* preview() {
            if (m_float == nullptr) {
                // it floats over the whole OpenCAD window: drag it over the drawing, the toolbars or the chat
                auto& app = QC_ApplicationWindow::getAppWindow();
                QWidget* host = app ? static_cast<QWidget*>(app.get()) : g_host.data();
                m_float = new FloatPreview(host, g_host, [this] { expand(); }, [this] { m_float->hide(); });
            }
            return m_float;
        }

        void expand() {
            if (m_full == nullptr) {
                auto& app = QC_ApplicationWindow::getAppWindow();
                m_full = new FullWindow(app ? app.get() : nullptr, [this] { collapse(); });
                QToolButton* back = m_full->addButton("arrow-left", QObject::tr("Back"));
                QToolButton* forward = m_full->addButton("arrow-right", QObject::tr("Forward"));
                QToolButton* reload = m_full->addButton("rotate-cw", QObject::tr("Reload"));
                auto* url = new QLineEdit(m_full);
                url->setObjectName("browserUrl");
                m_full->addBarWidget(url, 1);
                QToolButton* external = m_full->addButton("external-link", QObject::tr("Open this page in your own browser"));
                QToolButton* small = m_full->addButton("minimize-2", QObject::tr("Back to the small preview"));
                connect(back, &QToolButton::clicked, this, [this] { step(-1); }); // also past pages the AI clicked away from
                connect(forward, &QToolButton::clicked, this, [this] { step(1); });
                connect(reload, &QToolButton::clicked, m_view, &QWebEngineView::reload);
                connect(external, &QToolButton::clicked, this, [this] {
                    // only on the user's click: the AI never opens the system browser
                    if (webUrl(m_view->url())) {
                        QDesktopServices::openUrl(m_view->url());
                    }
                });
                connect(small, &QToolButton::clicked, m_full, &QWidget::close);
                connect(url, &QLineEdit::returnPressed, this, [this, url] {
                    const QUrl target = QUrl::fromUserInput(url->text().trimmed());
                    if (webUrl(target)) {
                        m_view->load(target);
                    }
                });
                url->setText(m_view->url().toString());
                connect(m_view, &QWebEngineView::urlChanged, url, [url](const QUrl& u) { url->setText(u.toString()); });
            }
            if (m_float != nullptr) {
                m_float->hide();
            }
            m_full->setPage(m_view);
            m_view->setZoomFactor(1.0);
            m_full->show();
            m_full->raise();
        }

        void collapse() {
            FloatPreview* f = preview();
            f->setPage(m_view);
            f->fitToHost();
            f->show();
            f->raise();
        }

        QWebEngineProfile* m_profile {nullptr};
        QWebEngineView* m_view {nullptr};
        QPointer<FloatPreview> m_float;
        QPointer<FullWindow> m_full;
        bool m_loading {false};
        QString m_note; // why the last load ended without a page, when nobody was waiting for it
        std::vector<LoadDone> m_waiters;
    };

    Browser* g_browser = nullptr;

    Browser* window() {
        if (g_browser == nullptr) {
            g_browser = new Browser();
        }
        return g_browser;
    }

    QString pageHeader(Browser* w) {
        return QStringLiteral("Title: %1\nURL: %2").arg(w->view()->title(), w->view()->url().toString());
    }

    LC_AIToolResult noPage() {
        return LC_AIToolResult::error("No page is open; open one with browser_navigate or browser_search first.");
    }

    /** browser_navigate's error: what was asked for, why it failed, and what the browser shows now. */
    QString couldNotOpen(Browser* w, const QUrl& url, const QString& why) {
        QString text = QStringLiteral("Could not open %1: %2.").arg(url.toString(), why);
        if (w->hasPage()) {
            text += QStringLiteral("\nThe browser now shows:\n") + pageHeader(w);
        }
        return text;
    }

    const QString TEXT_JS = QStringLiteral("(document.body ? document.body.innerText : '')");

    /** Title, URL and the start of the page text (what browser_navigate, click and back return). */
    void describePage(Browser* w, const Done& done, const QString& what) {
        w->js(TEXT_JS, done, [w, done, what](const QVariant& v) {
            QString text = v.toString().trimmed();
            const int total = int(text.size());
            if (text.size() > NAVIGATE_TEXT) {
                text = text.left(NAVIGATE_TEXT) + QStringLiteral("\n… (%1 characters in all; browser_read returns more)").arg(total);
            }
            done(LC_AIToolResult::text(QStringLiteral("%1\n%2\n\n%3").arg(what, pageHeader(w), text)));
        });
    }

    QJsonObject object(const QJsonObject& properties, const QStringList& required = {}) {
        QJsonObject schema{{"type", "object"}, {"properties", properties}};
        if (!required.isEmpty()) {
            schema.insert("required", QJsonArray::fromStringList(required));
        }
        return schema;
    }

    const QString GUIDE = QStringLiteral(
        " Pages open in OpenCAD's own floating AI Browser window, which the user can watch; it is for lookups "
        "(standards, datasheets, documentation, product specs), not for logging in, buying or filling in forms.");

    LC_AITool tool(const QString& name, const QString& label, const QString& description, const QJsonObject& parameters,
                   std::function<void(const QJsonObject&, const Done&)> run) {
        LC_AITool t;
        t.name = name;
        t.label = label;
        t.description = description + GUIDE;
        t.parameters = parameters;
        t.executeAsync = [run](const QJsonObject& args, const Done& done) { run(args, done); };
        return t;
    }
}

bool LC_AIBrowser::available() {
    return true;
}

void LC_AIBrowser::setHost(QWidget* host) {
    g_host = host;
}

void LC_AIBrowser::showWindow(QWidget*) {
    window()->toggle();
}

QList<LC_AITool> LC_AIBrowser::tools() {
    QList<LC_AITool> list;

    list << tool("browser_search", "Web search",
                 "Search the web (DuckDuckGo) and return the top results with their titles, URLs and snippets. "
                 "Open a result with browser_navigate.",
                 object({{"query", QJsonObject{{"type", "string"}, {"description", "What to search for"}}}}, {"query"}),
                 [](const QJsonObject& args, const Done& done) {
                     const QString query = args.value("query").toString().trimmed();
                     if (query.isEmpty()) {
                         done(LC_AIToolResult::error("query must not be empty."));
                         return;
                     }
                     QUrl url("https://html.duckduckgo.com/html/");
                     QUrlQuery q;
                     q.addQueryItem("q", query);
                     url.setQuery(q);
                     Browser* w = window();
                     w->load(url, [w, done, query](const bool ok, const QString& why) {
                         if (!ok) {
                             done(LC_AIToolResult::error(QStringLiteral("The search page did not load: %1.").arg(why)));
                             return;
                         }
                         const QString js = QStringLiteral(
                             "JSON.stringify(Array.from(document.querySelectorAll('.result')).slice(0, 10).map(r => {"
                             " const a = r.querySelector('.result__a'); const s = r.querySelector('.result__snippet');"
                             " return {title: a ? a.innerText.trim() : '', url: a ? a.href : '', snippet: s ? s.innerText.trim() : ''}; }))");
                         w->js(js, done, [w, done, query](const QVariant& v) {
                             const QJsonArray results = QJsonDocument::fromJson(v.toString().toUtf8()).array();
                             if (results.isEmpty()) {
                                 describePage(w, done, QStringLiteral("Searched for \"%1\" (no result list found; page text below).").arg(query));
                                 return;
                             }
                             QStringList lines;
                             int n = 0;
                             for (const QJsonValue& r : results) {
                                 const QJsonObject o = r.toObject();
                                 QString link = o.value("url").toString();
                                 // DuckDuckGo wraps results in a redirect: keep the real address
                                 const QUrlQuery redirect(QUrl(link).query());
                                 if (redirect.hasQueryItem("uddg")) {
                                     link = QUrl::fromPercentEncoding(redirect.queryItemValue("uddg", QUrl::FullyEncoded).toUtf8());
                                 }
                                 if (o.value("title").toString().isEmpty() || link.isEmpty()) {
                                     continue;
                                 }
                                 lines << QStringLiteral("%1. %2\n   %3\n   %4").arg(++n).arg(o.value("title").toString(), link,
                                                                                     o.value("snippet").toString().left(300));
                             }
                             done(LC_AIToolResult::text(QStringLiteral("Results for \"%1\":\n\n%2").arg(query, lines.join("\n\n"))));
                         });
                     });
                 });

    list << tool("browser_navigate", "Open page",
                 "Open a URL (http or https) and return its title and the start of its text.",
                 object({{"url", QJsonObject{{"type", "string"}, {"description", "The address, e.g. https://example.com/page"}}}}, {"url"}),
                 [](const QJsonObject& args, const Done& done) {
                     const QUrl url = QUrl::fromUserInput(args.value("url").toString().trimmed());
                     if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https")) {
                         done(LC_AIToolResult::error("Only http and https addresses can be opened."));
                         return;
                     }
                     Browser* w = window();
                     w->load(url, [w, done, url](const bool ok, const QString& why) {
                         if (!ok) {
                             done(LC_AIToolResult::error(couldNotOpen(w, url, why)));
                             return;
                         }
                         describePage(w, done, QStringLiteral("Opened the page."));
                     });
                 });

    list << tool("browser_read", "Read page",
                 QStringLiteral("Return the visible text of the current page (up to %1 characters; use offset to read further).").arg(READ_TEXT),
                 object({{"offset", QJsonObject{{"type", "integer"}, {"description", "Start at this character (default 0)"}}}}),
                 [](const QJsonObject& args, const Done& done) {
                     Browser* w = window();
                     if (!w->hasPage()) {
                         done(noPage());
                         return;
                     }
                     const int offset = qMax(0, args.value("offset").toInt());
                     w->js(TEXT_JS, done, [w, done, offset](const QVariant& v) {
                         const QString text = v.toString();
                         if (offset > 0 && offset >= text.size()) {
                             done(LC_AIToolResult::text(pageHeader(w) + QStringLiteral("\n\nNothing at offset %1: the page has only %2 characters.")
                                                                           .arg(offset).arg(text.size())));
                             return;
                         }
                         QString part = text.mid(offset, READ_TEXT);
                         if (offset + READ_TEXT < text.size()) {
                             part += QStringLiteral("\n… (%1 characters in all; next offset %2)").arg(text.size()).arg(offset + READ_TEXT);
                         }
                         done(LC_AIToolResult::text(pageHeader(w) + "\n\n" + part));
                     });
                 });

    list << tool("browser_snapshot", "Page links",
                 "List the page's headings and its visible links and buttons with numbers (refs) for browser_click. "
                 "Refs change after the page changes, so take a new snapshot before clicking.",
                 object({}),
                 [](const QJsonObject&, const Done& done) {
                     Browser* w = window();
                     if (!w->hasPage()) {
                         done(noPage());
                         return;
                     }
                     const QString js = QStringLiteral(R"((() => {
  document.querySelectorAll('[data-ai-ref]').forEach(e => e.removeAttribute('data-ai-ref'));
  const out = []; let n = 0;
  for (const e of document.querySelectorAll('a[href], button, [role=button], [role=link], summary, input[type=submit], input[type=button]')) {
    const r = e.getBoundingClientRect(); const s = getComputedStyle(e);
    if (r.width < 1 || r.height < 1 || s.visibility === 'hidden' || s.display === 'none') continue;
    const text = (e.innerText || e.value || e.getAttribute('aria-label') || e.title || '').trim().replace(/\s+/g, ' ').slice(0, 120);
    if (!text) continue;
    e.setAttribute('data-ai-ref', String(++n));
    out.push({n, kind: e.tagName === 'A' ? 'link' : 'button', text, href: e.tagName === 'A' ? e.href : ''});
    if (n >= 200) break;
  }
  const heads = Array.from(document.querySelectorAll('h1, h2, h3')).slice(0, 40)
    .map(h => h.tagName.toLowerCase() + ': ' + h.innerText.trim().replace(/\s+/g, ' ').slice(0, 120));
  return JSON.stringify({heads, els: out});
})())");
                     w->js(js, done, [w, done](const QVariant& v) {
                         const QJsonObject o = QJsonDocument::fromJson(v.toString().toUtf8()).object();
                         QStringList lines{pageHeader(w)};
                         const QJsonArray heads = o.value("heads").toArray();
                         if (!heads.isEmpty()) {
                             lines << "" << "Headings:";
                             for (const QJsonValue& h : heads) {
                                 lines << "  " + h.toString();
                             }
                         }
                         lines << "" << "Links and buttons:";
                         if (o.value("els").toArray().isEmpty()) {
                             lines << "  (none visible)";
                         }
                         for (const QJsonValue& e : o.value("els").toArray()) {
                             const QJsonObject el = e.toObject();
                             lines << QStringLiteral("  [%1] %2: %3%4").arg(el.value("n").toInt()).arg(el.value("kind").toString(),
                                 el.value("text").toString(), el.value("href").toString().isEmpty() ? QString() : "  → " + el.value("href").toString());
                         }
                         done(LC_AIToolResult::text(lines.join('\n')));
                     });
                 });

    list << tool("browser_click", "Click",
                 "Click a link or button by its number from browser_snapshot, then return the resulting page. Links that "
                 "open a new window open in this same window.",
                 object({{"ref", QJsonObject{{"type", "integer"}, {"description", "The number from browser_snapshot"}}}}, {"ref"}),
                 [](const QJsonObject& args, const Done& done) {
                     Browser* w = window();
                     if (!w->hasPage()) {
                         done(noPage());
                         return;
                     }
                     const int ref = args.value("ref").toInt();
                     // answers 'ok:' and the address of the link it clicked, if any
                     const QString js = QStringLiteral("(() => { const e = document.querySelector('[data-ai-ref=\"%1\"]');"
                                                       " if (!e) return 'missing'; e.scrollIntoView({block: 'center'}); e.click();"
                                                       " const a = e.closest('a[href]'); return 'ok:' + (a ? a.href : ''); })()")
                                            .arg(ref);
                     const QUrl before = w->view()->url();
                     w->reveal();
                     w->js(js, done, [w, done, ref, before](const QVariant& v) {
                         const QString answer = v.toString();
                         if (!answer.startsWith(QLatin1String("ok:"))) {
                             done(LC_AIToolResult::error(QStringLiteral("No element [%1]; take a new browser_snapshot.").arg(ref)));
                             return;
                         }
                         const QUrl link(answer.mid(3));
                         w->settle([w, done, ref, before, link](QString problem) {
                             // a mailto:, file: or app link is refused without a sound: say so, or the AI thinks it worked
                             if (problem.isEmpty() && link.isValid() && !link.scheme().isEmpty() && !webUrl(link)
                                 && link.scheme() != QLatin1String("javascript") && w->view()->url() == before) {
                                 problem = QStringLiteral("the link goes to a %1: address, which the AI browser does not open")
                                               .arg(link.scheme());
                             }
                             describePage(w, done, problem.isEmpty() ? QStringLiteral("Clicked [%1].").arg(ref)
                                                                     : QStringLiteral("Clicked [%1], but the page did not load: %2.")
                                                                           .arg(ref).arg(problem));
                         });
                     });
                 });

    list << tool("browser_scroll", "Scroll", "Scroll the page vertically by dy pixels (negative scrolls up).",
                 object({{"dy", QJsonObject{{"type", "number"}, {"description", "Pixels to scroll"}}}}, {"dy"}),
                 [](const QJsonObject& args, const Done& done) {
                     Browser* w = window();
                     if (!w->hasPage()) {
                         done(noPage());
                         return;
                     }
                     const QJsonValue value = args.value("dy");
                     bool number = value.isDouble();
                     double dy = value.toDouble();
                     if (value.isString()) { // some models send numbers as text
                         dy = value.toString().trimmed().toDouble(&number);
                     }
                     if (!number || !std::isfinite(dy)) {
                         done(LC_AIToolResult::error("dy must be a number of pixels, e.g. 600 to scroll down or -600 to scroll up."));
                         return;
                     }
                     // instant: on a page with smooth scrolling the position would still be moving when it is read
                     const QString js = QStringLiteral("(() => { const before = window.scrollY;"
                                                       " window.scrollBy({top: %1, behavior: 'instant'});"
                                                       " const end = Math.max(0, document.documentElement.scrollHeight - window.innerHeight);"
                                                       " return JSON.stringify({before, y: window.scrollY, end}); })()")
                                            .arg(dy);
                     w->js(js, done, [done, dy](const QVariant& v) {
                         const QJsonObject o = QJsonDocument::fromJson(v.toString().toUtf8()).object();
                         const int y = qRound(o.value("y").toDouble());
                         const int end = qRound(o.value("end").toDouble());
                         if (qRound(o.value("before").toDouble()) == y && dy != 0) {
                             const QString why = dy < 0     ? QStringLiteral("already at the top")
                                                 : end == 0 ? QStringLiteral("the page fits in the window, or it scrolls inside a panel")
                                                 : y >= end ? QStringLiteral("already at the bottom")
                                                            : QStringLiteral("the page scrolls inside a panel");
                             done(LC_AIToolResult::text(QStringLiteral("Did not scroll: %1 (%2 px from the top; the page scrolls to %3 px).")
                                                            .arg(why).arg(y).arg(end)));
                             return;
                         }
                         done(LC_AIToolResult::text(QStringLiteral("Scrolled; now %1 px from the top (the page scrolls to %2 px).").arg(y).arg(end)));
                     });
                 });

    list << tool("browser_back", "Back", "Go back to the previous page.", object({}),
                 [](const QJsonObject&, const Done& done) {
                     Browser* w = window();
                     if (!w->hasPage()) {
                         done(noPage());
                         return;
                     }
                     const int from = w->view()->history()->currentItemIndex();
                     if (!w->step(-1)) {
                         done(LC_AIToolResult::error("There is no previous page."));
                         return;
                     }
                     // a back step is often restored from Chromium's page cache without a load: settle like a click
                     w->settle([w, done, from](const QString&) {
                         if (w->view()->history()->currentItemIndex() >= from) {
                             done(LC_AIToolResult::error(QStringLiteral("Could not go back; the browser still shows:\n") + pageHeader(w)));
                             return;
                         }
                         describePage(w, done, QStringLiteral("Went back."));
                     });
                 });

    list << tool("browser_screenshot", "Page screenshot",
                 "Take a screenshot of the visible part of the current page, e.g. to look at a diagram or table.",
                 object({}),
                 [](const QJsonObject&, const Done& done) {
                     Browser* w = window();
                     if (!w->hasPage()) {
                         done(noPage());
                         return;
                     }
                     w->reveal();
                     QTimer::singleShot(150, w, [w, done] {
                         QPixmap pixmap = w->view()->grab();
                         if (pixmap.width() > MAX_SCREENSHOT_EDGE || pixmap.height() > MAX_SCREENSHOT_EDGE) {
                             pixmap = pixmap.scaled(MAX_SCREENSHOT_EDGE, MAX_SCREENSHOT_EDGE, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                         }
                         QByteArray png;
                         QBuffer buffer(&png);
                         buffer.open(QIODevice::WriteOnly);
                         pixmap.save(&buffer, "PNG");
                         LC_AIToolResult r = LC_AIToolResult::text(pageHeader(w));
                         r.content.prepend(LC_AIContent::makeImage(png, "image/png"));
                         done(r);
                     });
                 });
    return list;
}

#endif
