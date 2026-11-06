/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aicontrol.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QStyle>
#include <QTimer>
#include <utility>

#include "lc_graphicviewport.h"
#include "rs_entity.h"
#include "rs_graphic.h"
#include "rs_graphicview.h"
#include "rs_layer.h"
#include "rs_layerlist.h"

namespace {
    int g_aiInput = 0;

    /** Tools that only look (no view restore before them: the user may keep looking around). */
    bool onlyLooks(const QString& tool) {
        static const QStringList looking = {"capture_view", "get_view", "get_drawing_info", "list_entities", "list_commands",
                                            "list_toolbar_buttons", "view_image_file", "request_clarification", "look_at_app"};
        return looking.contains(tool) || tool.startsWith("browser_") || tool.startsWith("mcp__");
    }

    /** Parts of the window the lock never touches: the AI's own panel, dialogs and browser, and the banner. */
    bool isAIWidget(const QWidget* w) {
        for (const QWidget* p = w; p != nullptr; p = p->parentWidget()) {
            const QString cls = p->metaObject()->className();
            if (cls.startsWith("LC_AI") || p->objectName() == "aiBrowserFloat" || p->objectName() == "aiControlBanner") {
                return true;
            }
        }
        return false;
    }

    bool inDrawingView(const QWidget* w) {
        for (const QWidget* p = w; p != nullptr; p = p->parentWidget()) {
            if (qobject_cast<const RS_GraphicView*>(p) != nullptr) {
                return true;
            }
        }
        return false;
    }
}

// ------------------------------------------------------------------------------------------------ the banner

/** The strip over the drawing that says who drives it, with Take over / Resume and Stop. */
class LC_AIControlBanner : public QFrame {
public:
    explicit LC_AIControlBanner(QWidget* parent) : QFrame(parent) {
        setObjectName("aiControlBanner");
        setAttribute(Qt::WA_StyledBackground);
        setStyleSheet(QStringLiteral(R"(
#aiControlBanner { background: rgba(24, 27, 35, 235); border: 1px solid rgba(255, 255, 255, 40); border-radius: 12px; }
#aiControlBanner[flash="true"] { border: 1px solid #f5b942; }
#aiControlBanner QLabel { color: #e8eaf0; font-size: 12.5px; background: transparent; }
#aiControlBanner QLabel#aiControlDot { color: #4c8dff; font-size: 14px; }
#aiControlBanner QLabel#aiControlDot[paused="true"] { color: #f5b942; }
#aiControlBanner QPushButton { color: #e8eaf0; background: rgba(255, 255, 255, 22); border: 1px solid rgba(255, 255, 255, 40);
                               border-radius: 8px; padding: 3px 11px; font-size: 12px; }
#aiControlBanner QPushButton:hover { background: rgba(255, 255, 255, 40); }
#aiControlBanner QPushButton#aiControlPrimary { background: #4c8dff; border-color: #4c8dff; color: white; }
)"));
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(12, 6, 8, 6);
        layout->setSpacing(10);
        m_dot = new QLabel(QString::fromUtf8("●"), this);
        m_dot->setObjectName("aiControlDot");
        m_text = new QLabel(this);
        m_primary = new QPushButton(this);
        m_primary->setObjectName("aiControlPrimary");
        m_primary->setCursor(Qt::PointingHandCursor);
        m_stop = new QPushButton(QObject::tr("Stop"), this);
        m_stop->setCursor(Qt::PointingHandCursor);
        layout->addWidget(m_dot);
        layout->addWidget(m_text);
        layout->addWidget(m_primary);
        layout->addWidget(m_stop);
        QObject::connect(m_primary, &QPushButton::clicked, this, [] {
            LC_AIControl& c = LC_AIControl::instance();
            c.state() == LC_AIControl::State::Paused ? c.resume() : c.takeOver();
        });
        QObject::connect(m_stop, &QPushButton::clicked, this, [] { emit LC_AIControl::instance().stopRequested(); });
        m_flashTimer = new QTimer(this);
        m_flashTimer->setSingleShot(true);
        m_flashTimer->setInterval(1400);
        QObject::connect(m_flashTimer, &QTimer::timeout, this, [this] { setFlash(false); });
    }

    void showWorking() {
        setPaused(false);
        m_text->setText(QObject::tr("The AI is working on this drawing. You can look around (wheel, Shift/Ctrl+wheel, middle-button drag, scroll bars)."));
        m_primary->setText(QObject::tr("Take over"));
        m_primary->setToolTip(QObject::tr("Work on the drawing yourself; the AI waits and goes on when you stop"));
        adjustSize();
    }

    void showPaused(const int secondsLeft, const bool aiWaiting) {
        setPaused(true);
        m_text->setText(aiWaiting ? QObject::tr("You have control. The AI waits for you and goes on after %1 s without input.").arg(secondsLeft)
                                  : QObject::tr("You have control. The AI goes on after %1 s without input.").arg(secondsLeft));
        m_primary->setText(QObject::tr("Resume AI"));
        m_primary->setToolTip(QObject::tr("Give the drawing back to the AI now"));
        adjustSize();
    }

    /** The user tried to edit while the AI works: point at Take over. */
    void flash() {
        m_text->setText(QObject::tr("The AI is working on this drawing. Take over to edit it yourself."));
        adjustSize();
        setFlash(true);
        m_flashTimer->start();
    }

private:
    void setPaused(const bool paused) {
        m_dot->setProperty("paused", paused);
        m_dot->style()->unpolish(m_dot);
        m_dot->style()->polish(m_dot);
    }

    void setFlash(const bool on) {
        setProperty("flash", on);
        style()->unpolish(this);
        style()->polish(this);
        if (!on && LC_AIControl::instance().state() == LC_AIControl::State::Working) {
            showWorking();
            if (parentWidget() != nullptr) {
                QTimer::singleShot(0, this, [] {}); // size settles; the control places it again on the next resize
            }
        }
    }

    QLabel* m_dot {nullptr};
    QLabel* m_text {nullptr};
    QPushButton* m_primary {nullptr};
    QPushButton* m_stop {nullptr};
    QTimer* m_flashTimer {nullptr};
};

// ------------------------------------------------------------------------------------------------ control

LC_AIControl& LC_AIControl::instance() {
    static LC_AIControl* control = new LC_AIControl(); // lives as long as the application
    return *control;
}

LC_AIControl::AIInput::AIInput() {
    ++g_aiInput;
}

LC_AIControl::AIInput::~AIInput() {
    --g_aiInput;
}

bool LC_AIControl::aiInputActive() {
    return g_aiInput > 0;
}

void LC_AIControl::begin(QMainWindow* window) {
    if (window == nullptr) {
        return;
    }
    if (m_window != window) {
        if (m_window) {
            m_window->removeEventFilter(this);
        }
        m_window = window;
        m_window->installEventFilter(this); // keeps the banner in place
    }
    if (m_idle == nullptr) {
        m_idle = new QTimer(this);
        m_idle->setInterval(1000);
        connect(m_idle, &QTimer::timeout, this, &LC_AIControl::tick);
    }
    if (!m_banner) {
        m_banner = new LC_AIControlBanner(m_window);
    }
    m_views.clear();
    m_note.clear();
    m_waiting.clear();
    m_runView = LC_AITools::frontView(); // the drawing the question was asked about (set before the run is announced)
    qApp->installEventFilter(this);
    setState(State::Working);
}

void LC_AIControl::end() {
    if (m_state == State::Idle) {
        return;
    }
    for (const QPointer<LC_AIControlBanner>& b : std::as_const(m_dialogBanners)) {
        if (b) {
            b->deleteLater();
        }
    }
    m_dialogBanners.clear();
    qApp->removeEventFilter(this);
    if (m_idle != nullptr) {
        m_idle->stop();
    }
    m_waiting.clear(); // a stopped run: its held calls are dropped (the run ignores late answers anyway)
    m_note.clear();
    setState(State::Idle);
    m_runView.clear();
}

RS_GraphicView* LC_AIControl::runView() const {
    return m_state != State::Idle ? m_runView.data() : nullptr;
}

void LC_AIControl::setRunView(RS_GraphicView* view) {
    if (view == nullptr || view == m_runView) {
        return;
    }
    m_runView = view;
    if (m_state != State::Idle) {
        emit runViewChanged(view);
    }
}

void LC_AIControl::setState(const State state) {
    m_state = state;
    if (m_banner) {
        if (state == State::Idle) {
            m_banner->hide();
        }
        else {
            if (state == State::Working) {
                m_banner->showWorking();
            }
            else {
                m_banner->showPaused(m_secondsLeft, !m_waiting.isEmpty());
            }
            placeBanner();
            m_banner->show();
            m_banner->raise();
        }
    }
    for (const QPointer<LC_AIControlBanner>& b : std::as_const(m_dialogBanners)) {
        if (b && state == State::Working) {
            b->showWorking();
        }
        else if (b && state == State::Paused) {
            b->showPaused(m_secondsLeft, !m_waiting.isEmpty());
        }
    }
    placeDialogBanners();
    emit stateChanged(state);
}

void LC_AIControl::placeBanner() {
    if (!m_banner || !m_window) {
        return;
    }
    m_banner->adjustSize();
    QWidget* central = m_window->centralWidget();
    const QRect area = central != nullptr ? QRect(central->mapTo(m_window, QPoint(0, 0)), central->size()) : m_window->rect();
    const int width = qMin(m_banner->sizeHint().width(), area.width() - 24);
    m_banner->setGeometry(area.left() + (area.width() - width) / 2, area.top() + 10, width, m_banner->sizeHint().height());
    m_banner->raise();
}

void LC_AIControl::addDialogBanner(QWidget* dialog) {
    for (const QPointer<LC_AIControlBanner>& b : std::as_const(m_dialogBanners)) {
        if (b && b->parentWidget() == dialog) {
            return;
        }
    }
    // a modal dialog blocks the main window and its banner: Take over and Stop go into the dialog too
    auto* banner = new LC_AIControlBanner(dialog);
    m_state == State::Paused ? banner->showPaused(m_secondsLeft, !m_waiting.isEmpty()) : banner->showWorking();
    m_dialogBanners << banner;
    placeDialogBanners();
    banner->show();
    banner->raise();
}

void LC_AIControl::placeDialogBanners() {
    for (const QPointer<LC_AIControlBanner>& b : std::as_const(m_dialogBanners)) {
        if (!b || b->parentWidget() == nullptr) {
            continue;
        }
        QWidget* dialog = b->parentWidget();
        b->adjustSize();
        const int width = qMin(b->sizeHint().width(), dialog->width() - 16);
        b->setGeometry(8, dialog->height() - b->sizeHint().height() - 8, width, b->sizeHint().height());
        b->raise();
    }
}

void LC_AIControl::takeOver() {
    if (m_state != State::Working) {
        return;
    }
    m_atPause = drawingNow();
    m_secondsLeft = RESUME_AFTER_IDLE_S;
    ++m_takeOvers;
    m_idle->start();
    setState(State::Paused);
}

void LC_AIControl::resume() {
    if (m_state != State::Paused) {
        return;
    }
    m_idle->stop();
    const QString changes = changesSince(m_atPause);
    if (!changes.isEmpty()) {
        m_note = changes;
    }
    setState(State::Working);
    const QList<std::function<void()>> waiting = std::exchange(m_waiting, {});
    for (const auto& proceed : waiting) {
        restoreView();
        proceed();
    }
}

void LC_AIControl::tick() {
    placeDialogBanners();
    if (m_state != State::Paused) {
        m_idle->stop();
        return;
    }
    if (--m_secondsLeft <= 0) {
        resume();
        return;
    }
    if (m_banner) {
        m_banner->showPaused(m_secondsLeft, !m_waiting.isEmpty());
        placeBanner();
    }
}

void LC_AIControl::userActivity() {
    m_secondsLeft = RESUME_AFTER_IDLE_S;
    if (m_banner) {
        m_banner->showPaused(m_secondsLeft, !m_waiting.isEmpty());
        placeBanner();
    }
}

void LC_AIControl::beforeTool(const QString& toolName, std::function<void()> proceed) {
    if (m_state == State::Paused) {
        m_waiting.append(std::move(proceed)); // runs when the AI gets the drawing back
        if (m_banner) {
            m_banner->showPaused(m_secondsLeft, true);
            placeBanner();
        }
        return;
    }
    if (m_state == State::Working && !onlyLooks(toolName)) {
        restoreView(); // the AI acts on what it last saw, wherever the user has looked meanwhile
    }
    proceed();
}

void LC_AIControl::afterTool(const QString& toolName, LC_AIToolResult& result) {
    if (m_state == State::Idle) {
        return;
    }
    if (!onlyLooks(toolName)) {
        rememberView();
    }
    else if (RS_GraphicView* view = LC_AITools::view(); view != nullptr && !m_views.contains(view)) {
        rememberView(); // the first look of a run fixes the AI's view
    }
    if (!m_note.isEmpty()) {
        result.content.prepend(LC_AIContent::makeText(m_note));
        m_note.clear();
    }
}

QList<LC_AITool> LC_AIControl::withControl(const QList<LC_AITool>& tools) {
    QList<LC_AITool> wrapped;
    for (LC_AITool tool : tools) {
        const QString name = tool.name;
        if (tool.executeAsync) {
            auto inner = tool.executeAsync;
            tool.executeAsync = [name, inner](const QJsonObject& args, std::function<void(const LC_AIToolResult&)> done) {
                const ToolStart start = LC_AIControl::instance().toolStarts(name);
                inner(args, [name, done, start](const LC_AIToolResult& result) {
                    LC_AIToolResult out = result;
                    LC_AIControl::instance().toolEnded(start);
                    LC_AIControl::instance().afterTool(name, out);
                    done(out);
                });
            };
        }
        else if (tool.execute) {
            auto inner = tool.execute;
            tool.execute = [name, inner](const QJsonObject& args) {
                const ToolStart start = LC_AIControl::instance().toolStarts(name);
                LC_AIToolResult result = inner(args);
                LC_AIControl::instance().toolEnded(start);
                LC_AIControl::instance().afterTool(name, result);
                return result;
            };
        }
        wrapped << tool;
    }
    return wrapped;
}

LC_AIControl::ToolStart LC_AIControl::toolStarts(const QString& toolName) {
    if (m_state != State::Idle) {
        if (!m_runView) {
            setRunView(LC_AITools::frontView()); // the run's drawing was closed: the tools go on with the one in front
        }
        else if (!onlyLooks(toolName)) {
            // the user looked at another drawing meanwhile: commands and buttons act on the drawing in front,
            // so the run's own comes back first (tools that only look read it where it is)
            LC_AITools::bringToFront(m_runView);
        }
    }
    return {LC_AITools::frontView(), m_takeOvers};
}

void LC_AIControl::toolEnded(const ToolStart& start) {
    // another drawing came to the front while the AI had control: the AI opened it or switched to it
    RS_GraphicView* front = LC_AITools::frontView();
    if (m_state == State::Working && m_takeOvers == start.takeOvers && front != nullptr && front != start.front.data()) {
        setRunView(front);
    }
}

// ------------------------------------------------------------------------------------------------ the AI's view

void LC_AIControl::rememberView() {
    RS_GraphicView* view = LC_AITools::view();
    LC_GraphicViewport* vp = view != nullptr ? view->getViewPort() : nullptr;
    if (vp == nullptr || vp->getFactor().x <= 0) {
        return;
    }
    if (!m_views.contains(view)) {
        connect(view, &QObject::destroyed, this, [this, view] { m_views.remove(view); });
    }
    m_views.insert(view, {vp->getOffsetX(), vp->getOffsetY(), vp->getFactor().x});
}

void LC_AIControl::restoreView() {
    RS_GraphicView* view = LC_AITools::view();
    int ox = 0;
    int oy = 0;
    double factor = 0;
    if (view != nullptr && aiView(view, ox, oy, factor)) {
        view->getViewPort()->setOffsetAndFactor(ox, oy, factor);
        view->redraw(RS2::RedrawAll);
    }
}

bool LC_AIControl::aiView(RS_GraphicView* view, int& offsetX, int& offsetY, double& factor) const {
    if (m_state == State::Idle || view == nullptr || !m_views.contains(view) || view->getViewPort() == nullptr) {
        return false;
    }
    const View v = m_views.value(view);
    const LC_GraphicViewport* vp = view->getViewPort();
    if (vp->getOffsetX() == v.offsetX && vp->getOffsetY() == v.offsetY && qFuzzyCompare(vp->getFactor().x, v.factor)) {
        return false; // the user has not moved it
    }
    offsetX = v.offsetX;
    offsetY = v.offsetY;
    factor = v.factor;
    return true;
}

// ------------------------------------------------------------------------------------------------ what the user changed

LC_AIControl::Drawing LC_AIControl::drawingNow() {
    Drawing d;
    RS_GraphicView* view = LC_AITools::view();
    RS_Graphic* graphic = view != nullptr ? view->getGraphic() : nullptr;
    if (graphic == nullptr) {
        return d;
    }
    for (RS_Entity* e : *graphic) {
        if (e != nullptr && !e->isDeleted()) {
            d.entities.insert(e->getId(), LC_AITools::describeEntity(e));
            d.selected += e->isSelected() ? 1 : 0;
        }
    }
    for (RS_Layer* layer : *graphic->getLayerList()) {
        d.layers << layer->getName();
    }
    if (graphic->getLayerList()->getActive() != nullptr) {
        d.activeLayer = graphic->getLayerList()->getActive()->getName();
    }
    return d;
}

QString LC_AIControl::changesSince(const Drawing& before) const {
    const Drawing now = drawingNow();
    QStringList added;
    QStringList removed;
    for (auto it = now.entities.cbegin(); it != now.entities.cend(); ++it) {
        if (!before.entities.contains(it.key())) {
            added << it.value();
        }
    }
    for (auto it = before.entities.cbegin(); it != before.entities.cend(); ++it) {
        if (!now.entities.contains(it.key())) {
            removed << it.value();
        }
    }
    QStringList lines;
    if (!added.isEmpty()) {
        lines << QStringLiteral("Added %1 entities:").arg(added.size());
        for (int i = 0; i < added.size() && i < 10; ++i) {
            lines << "  " + added[i];
        }
        if (added.size() > 10) {
            lines << QStringLiteral("  ... and %1 more").arg(added.size() - 10);
        }
    }
    if (!removed.isEmpty()) {
        lines << QStringLiteral("Removed or changed %1 entities (an edit replaces the entities it changes):").arg(removed.size());
        for (int i = 0; i < removed.size() && i < 10; ++i) {
            lines << "  was " + removed[i];
        }
    }
    for (const QString& layer : now.layers) {
        if (!before.layers.contains(layer)) {
            lines << "New layer: " + layer;
        }
    }
    for (const QString& layer : before.layers) {
        if (!now.layers.contains(layer)) {
            lines << "Removed layer: " + layer;
        }
    }
    if (now.activeLayer != before.activeLayer) {
        lines << "Active layer is now " + now.activeLayer;
    }
    if (now.selected != before.selected) {
        lines << QStringLiteral("Selection: %1 entities").arg(now.selected);
    }
    if (lines.isEmpty()) {
        return {};
    }
    lines.prepend(QStringLiteral("NOTE: the user took over while you were working and changed the drawing. Take these "
                                 "changes into account (check with list_entities or capture_view if needed):"));
    return lines.join('\n');
}

// ------------------------------------------------------------------------------------------------ the lock

bool LC_AIControl::shouldBlock(QObject* watched, QEvent* event) const {
    switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        case QEvent::Wheel:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::Shortcut:
        case QEvent::ContextMenu:
        case QEvent::Drop:
        case QEvent::DragEnter:
            break;
        default:
            return false;
    }
    if (event->type() == QEvent::Shortcut) {
        // menu and toolbar shortcuts act on the drawing; the AI's own (Ctrl+L) stays
        if (auto* action = qobject_cast<QAction*>(watched)) {
            return action->objectName() != "AIAssistantToggle";
        }
        if (auto* shortcut = qobject_cast<QShortcut*>(watched)) {
            return !isAIWidget(qobject_cast<QWidget*>(shortcut->parent()));
        }
        return false;
    }
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        // Shift, Ctrl and Alt alone change nothing; they go with the wheel (Shift+wheel scrolls, Ctrl+wheel zooms)
        switch (static_cast<QKeyEvent*>(event)->key()) {
            case Qt::Key_Shift:
            case Qt::Key_Control:
            case Qt::Key_Alt:
            case Qt::Key_Meta:
            case Qt::Key_AltGr:
                return false;
            default:
                break;
        }
    }
    auto* w = qobject_cast<QWidget*>(watched);
    if (w == nullptr || isAIWidget(w)) {
        return false;
    }
    if (qobject_cast<QScrollBar*>(w) != nullptr || qobject_cast<QScrollBar*>(w->parentWidget()) != nullptr) {
        return false; // the drawing's scroll bars only move the view
    }
    const QWidget* top = w->window();
    const bool ours = top == m_window || qobject_cast<const QMenu*>(top) != nullptr
                      || (qobject_cast<const QDialog*>(top) != nullptr && !isAIWidget(top));
    if (!ours) {
        return false;
    }
    if (inDrawingView(w)) {
        // looking around stays free: wheel zoom and middle-button panning
        if (event->type() == QEvent::Wheel) {
            return false;
        }
        if (auto* mouse = dynamic_cast<QMouseEvent*>(event)) {
            const bool middle = mouse->button() == Qt::MiddleButton || (mouse->buttons() & Qt::MiddleButton);
            return !middle;
        }
        return true;
    }
    if (event->type() == QEvent::MouseMove) {
        return false; // hovering toolbars and panels (tooltips) is harmless
    }
    return true;
}

bool LC_AIControl::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_window && (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest)) {
        QTimer::singleShot(0, this, [this] { placeBanner(); });
        return false;
    }
    if (event->type() == QEvent::Show && m_state != State::Idle) {
        auto* dialog = qobject_cast<QDialog*>(watched);
        if (dialog != nullptr && dialog->isWindow() && dialog->isModal() && !isAIWidget(dialog)) {
            QPointer<QDialog> d = dialog;
            QTimer::singleShot(0, this, [this, d] {
                if (d && m_state != State::Idle) {
                    addDialogBanner(d);
                }
            });
        }
    }
    if (event->type() == QEvent::Resize && qobject_cast<QDialog*>(watched) != nullptr) {
        QTimer::singleShot(0, this, [this] { placeDialogBanners(); });
    }
    if (m_state == State::Paused) {
        switch (event->type()) {
            case QEvent::MouseButtonPress:
            case QEvent::KeyPress:
            case QEvent::Wheel:
                if (!aiInputActive()) {
                    auto* w = qobject_cast<QWidget*>(watched);
                    if (w != nullptr && !isAIWidget(w)) {
                        userActivity(); // still working: the AI keeps waiting
                    }
                }
                break;
            default:
                break;
        }
        return false;
    }
    if (m_state != State::Working || aiInputActive()) {
        return false;
    }
    if (shouldBlock(watched, event)) {
        if ((event->type() == QEvent::MouseButtonPress || event->type() == QEvent::KeyPress || event->type() == QEvent::Shortcut)
            && m_banner) {
            m_banner->flash();
            placeBanner();
            for (const QPointer<LC_AIControlBanner>& b : std::as_const(m_dialogBanners)) {
                if (b) {
                    b->flash();
                }
            }
            placeDialogBanners();
        }
        return true;
    }
    return false;
}
