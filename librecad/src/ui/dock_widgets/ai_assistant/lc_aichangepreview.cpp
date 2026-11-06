/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aichangepreview.h"

#include <QHBoxLayout>
#include <QJsonValue>
#include <QSignalBlocker>
#include <QHash>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

#include "lc_aicheckpoints.h"
#include "lc_aicontrol.h"
#include "lc_aicredentials.h"
#include "lc_aiicons.h"
#include "lc_aitool.h"
#include "lc_graphicviewport.h"
#include "lc_overlayentitiescontainer.h"
#include "lc_overlayentity.h"
#include "rs_document.h"
#include "rs_entity.h"
#include "rs_entitycontainer.h"
#include "rs_graphicview.h"
#include "rs_painter.h"
#include "rs_pen.h"

namespace {
    const QColor ADDED("#22c55e");   // the green of a finished tool call
    const QColor REMOVED("#ef4444"); // the red of a failed one
    const QString REVIEW_KEY = QStringLiteral("reviewAIChanges");
    constexpr int WATCH_MS = 600;    // how often an open review looks whether the drawing changed under it

    QString rgba(const QColor& c, const double alpha) {
        return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha);
    }

    void setPenDeep(RS_Entity* e, const RS_Pen& pen) {
        e->setPen(pen);
        e->clearSelectionFlag();
        if (e->isContainer()) {
            for (RS_Entity* child : *static_cast<RS_EntityContainer*>(e)) {
                if (child != nullptr) {
                    setPenDeep(child, pen);
                }
            }
        }
    }

    /**
     * The highlight in the drawing: copies of the changed entities drawn over it, each twice, as a soft wide halo and as
     * a thin line (dashed for what the run removed). Copies, so the drawing itself is never touched.
     */
    class ChangeOverlay : public LC_OverlayDrawable {
    public:
        std::vector<std::unique_ptr<RS_Entity>> added;
        std::vector<std::unique_ptr<RS_Entity>> removed;

        void draw(RS_Painter* painter) override {
            drawAll(painter, removed, REMOVED, RS2::DashLine);
            drawAll(painter, added, ADDED, RS2::SolidLine);
        }

    private:
        static void drawAll(RS_Painter* painter, std::vector<std::unique_ptr<RS_Entity>>& list, const QColor& color,
                            const RS2::LineType lineType) {
            for (int pass = 0; pass < 2; ++pass) {
                RS_Pen pen(RS_Color(color), RS2::Width00, pass == 0 ? RS2::SolidLine : lineType);
                pen.setScreenWidth(pass == 0 ? 7.0 : 2.0);
                pen.setAlpha(pass == 0 ? 0.25f : 1.0f);
                for (const auto& e : list) {
                    setPenDeep(e.get(), pen);
                    painter->setPen(pen);
                    e->draw(painter);
                }
            }
        }
    };

    QHash<unsigned long long, RS_Entity*> entitiesById(RS_Document* document) {
        QHash<unsigned long long, RS_Entity*> byId;
        for (RS_Entity* e : *document) {
            if (e != nullptr) {
                byId.insert(e->getId(), e);
            }
        }
        return byId;
    }
}

// ------------------------------------------------------------------------------------------------ the preview

LC_AIChangePreview& LC_AIChangePreview::instance() {
    static LC_AIChangePreview preview;
    return preview;
}

LC_AIChangePreview::LC_AIChangePreview() {
    m_watch = new QTimer(this);
    m_watch->setInterval(WATCH_MS);
    connect(m_watch, &QTimer::timeout, this, [this] { rebuild(); }); // undo, redo and the user's edits change what is left
    LC_AICheckpoints& checkpoints = LC_AICheckpoints::instance();
    connect(&checkpoints, &LC_AICheckpoints::runRecorded, this, [this](const int run) {
        // an answer that changed several drawings records one checkpoint each, one after the other (a new run closes
        // any review first): the review stays with the drawing in front
        if (isOpen() && m_view != nullptr && m_view == LC_AITools::frontView()) {
            return;
        }
        if (reviewAfterRuns() && !LC_AICheckpoints::instance().changesOf(run).isEmpty()) {
            show(run);
        }
    });
    // a new run builds on the drawing as it is: an open review ends and its changes stay
    connect(&checkpoints, &LC_AICheckpoints::runStarted, this, [this](int) { close(); });
    connect(&checkpoints, &LC_AICheckpoints::runReverted, this, [this](const int run, const bool reverted) {
        if (run == m_run && reverted) {
            const int was = m_run;
            clearOverlay();
            m_run = 0;
            m_watch->stop();
            emit finished(was, tr("Changes taken back."));
            emit changed();
        }
    });
}

bool LC_AIChangePreview::reviewAfterRuns() {
    const QJsonValue v = LC_AICredentials::instance().setting(REVIEW_KEY);
    return v.isBool() ? v.toBool() : true;
}

void LC_AIChangePreview::setReviewAfterRuns(const bool on) {
    LC_AICredentials::instance().setSetting(REVIEW_KEY, on);
}

bool LC_AIChangePreview::show(const int run) {
    LC_AICheckpoints& checkpoints = LC_AICheckpoints::instance();
    RS_GraphicView* view = checkpoints.viewOf(run);
    if (view == nullptr || checkpoints.changesOf(run).isEmpty() || LC_AIControl::instance().state() != LC_AIControl::State::Idle) {
        return false;
    }
    if (m_run != 0 && m_view != view) {
        clearOverlay();
    }
    m_run = run;
    m_view = view;
    m_visible = true;
    m_shownKey.clear();
    rebuild();
    if (m_run == 0 || m_added + m_removed == 0) {
        clearOverlay();
        m_run = 0;
        emit changed();
        return false;
    }
    m_watch->start();
    emit changed();
    return true;
}

void LC_AIChangePreview::rebuild() {
    if (m_run == 0) {
        return;
    }
    RS_GraphicView* view = m_view;
    if (view == nullptr || view->getDocument() == nullptr || LC_AICheckpoints::instance().viewOf(m_run) == nullptr) {
        close(); // the drawing went away
        return;
    }
    const LC_AIRunChanges changes = LC_AICheckpoints::instance().changesOf(m_run);
    const QHash<unsigned long long, RS_Entity*> byId = entitiesById(view->getDocument());
    auto overlay = std::make_unique<ChangeOverlay>();
    QStringList key;
    // what the run drew and is still there; what it deleted and is still gone (an undo may have brought it back)
    for (const unsigned long long id : changes.added) {
        RS_Entity* e = byId.value(id);
        if (e != nullptr && !e->isDeleted()) {
            overlay->added.emplace_back(e->clone());
            key << "+" + QString::number(id);
        }
    }
    for (const unsigned long long id : changes.removed) {
        RS_Entity* e = byId.value(id);
        if (e != nullptr && e->isDeleted()) {
            overlay->removed.emplace_back(e->clone());
            key << "-" + QString::number(id);
        }
    }
    const QString shown = key.join(',') + (m_visible ? "|on" : "|off");
    if (shown == m_shownKey) {
        return;
    }
    m_shownKey = shown;
    m_added = int(overlay->added.size());
    m_removed = int(overlay->removed.size());
    LC_GraphicViewport* viewport = view->getViewPort();
    viewport->clearOverlayDrawablesContainer(RS2::OverlayGraphics::PermanentHighlights);
    if (m_visible && m_added + m_removed > 0) {
        viewport->getOverlaysDrawablesContainer(RS2::OverlayGraphics::PermanentHighlights)->add(overlay.release());
    }
    view->redraw(RS2::RedrawOverlay);
    emit changed();
}

void LC_AIChangePreview::clearOverlay() {
    if (m_view != nullptr && m_view->getViewPort() != nullptr) {
        m_view->getViewPort()->clearOverlayDrawablesContainer(RS2::OverlayGraphics::PermanentHighlights);
        m_view->redraw(RS2::RedrawOverlay);
    }
    m_shownKey.clear();
}

void LC_AIChangePreview::accept() {
    if (m_run == 0) {
        return;
    }
    const int run = m_run;
    clearOverlay();
    m_run = 0;
    m_watch->stop();
    emit finished(run, tr("Changes kept."));
    emit changed();
}

bool LC_AIChangePreview::reject(QString& message) {
    if (m_run == 0) {
        return false;
    }
    LC_AICheckpoints& checkpoints = LC_AICheckpoints::instance();
    const LC_AICheckpoints::Check check = checkpoints.check(m_run);
    bool done = false;
    switch (check.situation) {
        case LC_AICheckpoints::Situation::Unchanged:
            done = checkpoints.revert(m_run, message);
            break;
        case LC_AICheckpoints::Situation::ChangedSince:
            done = checkpoints.revertOwnChanges(m_run, message); // later changes stay
            break;
        default:
            message = check.text;
            break;
    }
    // a successful revert ends the review through runReverted
    return done;
}

void LC_AIChangePreview::close() {
    if (m_run == 0) {
        return;
    }
    const int run = m_run;
    clearOverlay();
    m_run = 0;
    m_watch->stop();
    emit finished(run, tr("The changes stay."));
    emit changed();
}

void LC_AIChangePreview::setHighlightVisible(const bool visible) {
    m_visible = visible;
    rebuild();
}

void LC_AIChangePreview::zoomToChanges() {
    RS_GraphicView* view = m_view;
    if (m_run == 0 || view == nullptr || view->getDocument() == nullptr) {
        return;
    }
    const LC_AIRunChanges changes = LC_AICheckpoints::instance().changesOf(m_run);
    const QHash<unsigned long long, RS_Entity*> byId = entitiesById(view->getDocument());
    LC_GraphicViewport* vp = view->getViewPort();
    RS_Vector min(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
    RS_Vector max(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest());
    bool any = false;
    for (const unsigned long long id : changes.added + changes.removed) {
        RS_Entity* e = byId.value(id);
        if (e == nullptr) {
            continue;
        }
        e->calculateBorders();
        // the box in the view's own coordinates (a rotated UCS turns it)
        for (const RS_Vector& w : {e->getMin(), e->getMax(), RS_Vector(e->getMin().x, e->getMax().y), RS_Vector(e->getMax().x, e->getMin().y)}) {
            const RS_Vector u = vp->toUCS(w);
            min = RS_Vector(std::min(min.x, u.x), std::min(min.y, u.y));
            max = RS_Vector(std::max(max.x, u.x), std::max(max.y, u.y));
            any = true;
        }
    }
    if (!any) {
        return;
    }
    // some room around the changes, and a minimum size for a point or a short line
    const double pad = std::max({(max.x - min.x) * 0.15, (max.y - min.y) * 0.15, 5.0});
    vp->zoomWindow(min - RS_Vector(pad, pad), max + RS_Vector(pad, pad), true);
    view->redraw();
}

// ------------------------------------------------------------------------------------------------ the review bar

namespace {
    QToolButton* iconButton(const QString& icon, const QString& tip, const LC_AITheme& t, QWidget* parent) {
        auto* b = new QToolButton(parent);
        b->setObjectName("aiReviewIcon");
        b->setIcon(LC_AIIcons::lucideIcon(icon, 14, t.textSecondary));
        b->setIconSize(QSize(14, 14));
        b->setToolTip(tip);
        b->setAccessibleName(tip);
        b->setCursor(Qt::PointingHandCursor);
        b->setAutoRaise(true);
        return b;
    }
}

LC_AIReviewBar::LC_AIReviewBar(const LC_AITheme& theme, QWidget* parent)
    : QFrame(parent), m_theme(theme) {
    setObjectName("aiReviewBar");
    const LC_AITheme& t = theme;
    setStyleSheet(QStringLiteral(R"(
#aiReviewBar { background: %1; border: 1px solid %2; border-radius: 12px; }
#aiReviewBar QLabel { background: transparent; }
#aiReviewTitle { color: %3; font-size: 12.5px; font-weight: 600; }
#aiReviewLegend { color: %4; font-size: 12px; }
#aiReviewNote { color: %4; font-size: 11.5px; }
#aiReviewIcon { border: none; border-radius: 6px; padding: 3px; background: transparent; }
#aiReviewIcon:hover { background: %5; }
#aiReviewIcon:checked { background: %6; }
#aiReviewAccept { color: white; background: %7; border: none; border-radius: 7px; padding: 4px 14px; font-weight: 600; min-height: 22px; }
#aiReviewAccept:hover { background: %8; }
#aiReviewReject { color: %9; background: %10; border: 1px solid %11; border-radius: 7px; padding: 4px 12px; min-height: 22px; }
#aiReviewReject:hover { background: %12; }
#aiReviewAuto { border: none; background: transparent; color: %4; font-size: 11.5px; padding: 2px 4px; }
#aiReviewAuto:hover { color: %3; }
)")
                      .arg(t.bgElevated.name(QColor::HexArgb), rgba(t.accentBlue, 0.45), t.textHeading.name(), t.textMuted.name(),
                           t.bgHover.name(QColor::HexArgb), t.bgActive.name(QColor::HexArgb), ADDED.darker(115).name(), ADDED.darker(130).name(),
                           REMOVED.name(), rgba(REMOVED, 0.08), rgba(REMOVED, 0.35), rgba(REMOVED, 0.16)));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 9, 10, 10);
    layout->setSpacing(7);

    auto* top = new QHBoxLayout();
    top->setSpacing(6);
    auto* icon = new QLabel(this);
    icon->setPixmap(LC_AIIcons::lucide("eye", 15, t.accentBlue, devicePixelRatioF()));
    top->addWidget(icon);
    m_title = new QLabel(tr("Review the AI's changes"), this);
    m_title->setObjectName("aiReviewTitle");
    top->addWidget(m_title, 1);
    m_zoom = iconButton("scan", tr("Zoom to the changes"), t, this);
    m_eye = iconButton("eye-off", tr("Hide the highlight"), t, this);
    top->addWidget(m_zoom);
    top->addWidget(m_eye);
    layout->addLayout(top);

    m_legend = new QLabel(this);
    m_legend->setObjectName("aiReviewLegend");
    m_legend->setTextFormat(Qt::RichText);
    m_legend->setWordWrap(true);
    layout->addWidget(m_legend);

    m_note = new QLabel(this);
    m_note->setObjectName("aiReviewNote");
    m_note->setWordWrap(true);
    m_note->hide();
    layout->addWidget(m_note);

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(8);
    m_auto = new QToolButton(this);
    m_auto->setObjectName("aiReviewAuto");
    m_auto->setCheckable(true);
    m_auto->setCursor(Qt::PointingHandCursor);
    m_auto->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_auto->setIconSize(QSize(12, 12));
    m_auto->setToolTip(tr("Review after every run: open this review by itself whenever the AI changes the drawing"));
    buttons->addWidget(m_auto);
    buttons->addStretch(1);
    m_reject = new QPushButton(LC_AIIcons::lucideIcon("x", 12, REMOVED), tr("Reject"), this);
    m_reject->setObjectName("aiReviewReject");
    m_reject->setCursor(Qt::PointingHandCursor);
    m_reject->setToolTip(tr("Take the AI's changes back"));
    m_accept = new QPushButton(LC_AIIcons::lucideIcon("check", 12, Qt::white), tr("Accept"), this);
    m_accept->setObjectName("aiReviewAccept");
    m_accept->setCursor(Qt::PointingHandCursor);
    m_accept->setToolTip(tr("Keep the AI's changes"));
    buttons->addWidget(m_reject);
    buttons->addWidget(m_accept);
    layout->addLayout(buttons);

    LC_AIChangePreview& preview = LC_AIChangePreview::instance();
    connect(m_accept, &QPushButton::clicked, this, [] { LC_AIChangePreview::instance().accept(); });
    connect(m_reject, &QPushButton::clicked, this, [this] {
        QString message;
        if (!LC_AIChangePreview::instance().reject(message)) {
            m_note->setText(message.isEmpty() ? tr("The changes cannot be taken back now.") : message);
            m_note->show();
        }
    });
    connect(m_zoom, &QToolButton::clicked, this, [] { LC_AIChangePreview::instance().zoomToChanges(); });
    connect(m_eye, &QToolButton::clicked, this, [] {
        LC_AIChangePreview& p = LC_AIChangePreview::instance();
        p.setHighlightVisible(!p.highlightVisible());
    });
    connect(m_auto, &QToolButton::toggled, this, [this](const bool on) {
        LC_AIChangePreview::setReviewAfterRuns(on);
        refresh();
    });
    connect(&preview, &LC_AIChangePreview::changed, this, &LC_AIReviewBar::refresh);
    connect(&preview, &LC_AIChangePreview::finished, this, [this](int, const QString&) { m_note->hide(); });
    refresh();
}

void LC_AIReviewBar::refresh() {
    const LC_AIChangePreview& preview = LC_AIChangePreview::instance();
    setVisible(preview.isOpen());
    if (!preview.isOpen()) {
        return;
    }
    const LC_AITheme& t = m_theme;
    QStringList parts;
    if (preview.addedShown() > 0) {
        parts << QStringLiteral("<span style=\"color:%1\">&#9679;</span> %2").arg(ADDED.name())
                     .arg(preview.addedShown() == 1 ? tr("1 added") : tr("%1 added").arg(preview.addedShown()));
    }
    if (preview.removedShown() > 0) {
        parts << QStringLiteral("<span style=\"color:%1\">&#9679;</span> %2").arg(REMOVED.name())
                     .arg(preview.removedShown() == 1 ? tr("1 removed") : tr("%1 removed").arg(preview.removedShown()));
    }
    m_legend->setText(parts.join(QStringLiteral(" &nbsp;&nbsp; ")));
    m_legend->setToolTip(preview.highlightVisible() ? tr("Green: drawn by the AI. Red dashes: deleted or replaced by the AI.")
                                                    : tr("The highlight is hidden; the eye shows it again."));
    const bool visible = preview.highlightVisible();
    m_eye->setIcon(LC_AIIcons::lucideIcon(visible ? "eye-off" : "eye", 14, t.textSecondary));
    m_eye->setToolTip(visible ? tr("Hide the highlight") : tr("Show the highlight"));
    const bool on = LC_AIChangePreview::reviewAfterRuns();
    const QSignalBlocker block(m_auto);
    m_auto->setChecked(on);
    m_auto->setIcon(LC_AIIcons::lucideIcon(on ? "circle-check" : "circle", 12, on ? ADDED : t.textMuted));
    m_auto->setText(tr("Auto-review"));
}
