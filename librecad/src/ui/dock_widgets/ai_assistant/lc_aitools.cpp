/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aitool.h"

#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QLabel>
#include <QMap>
#include <QPixmap>
#include <QTextEdit>
#include <cmath>

#include "lc_graphicviewport.h"
#include "qc_applicationwindow.h"
#include "qg_commandwidget.h"
#include "rs_arc.h"
#include "rs_circle.h"
#include "rs_commands.h"
#include "rs_dimension.h"
#include "rs_document.h"
#include "rs_entity.h"
#include "rs_graphic.h"
#include "rs_graphicview.h"
#include "rs_layer.h"
#include "rs_layerlist.h"
#include "rs_line.h"
#include "rs_math.h"
#include "rs_mtext.h"
#include "rs_point.h"
#include "rs_text.h"
#include "rs_units.h"

namespace {
    constexpr int MAX_SCREENSHOT_EDGE = 1280; // keeps images small for every provider
    constexpr int DEFAULT_LIST_LIMIT = 100;
    constexpr int MAX_LIST_LIMIT = 500;
    constexpr int MAX_COMMAND_OUTPUT = 4000;

    // ------------------------------------------------------------------ schema helpers

    QJsonObject number(const QString& description) {
        return {{"type", "number"}, {"description", description}};
    }
    QJsonObject integer(const QString& description) {
        return {{"type", "integer"}, {"description", description}};
    }
    QJsonObject string(const QString& description) {
        return {{"type", "string"}, {"description", description}};
    }
    QJsonObject boolean(const QString& description) {
        return {{"type", "boolean"}, {"description", description}};
    }
    QJsonObject object(const QJsonObject& properties, const QStringList& required = {}) {
        QJsonObject schema{{"type", "object"}, {"properties", properties}};
        if (!required.isEmpty()) {
            schema.insert("required", QJsonArray::fromStringList(required));
        }
        return schema;
    }
    const QJsonObject CAPTURE_ARG = boolean("Also return a screenshot of the view after the change (default false).");

    // ------------------------------------------------------------------ access to the open drawing

    RS_GraphicView* g_viewForTests = nullptr;

    RS_GraphicView* currentView() {
        if (g_viewForTests != nullptr) {
            return g_viewForTests;
        }
        auto& app = QC_ApplicationWindow::getAppWindow();
        return app ? app->getCurrentGraphicView() : nullptr;
    }

    const QString NO_DRAWING = QStringLiteral("No drawing is open in OpenCAD. Ask the user to open or create one.");

    QString fmt(const double v) {
        if (!std::isfinite(v)) {
            return QStringLiteral("n/a");
        }
        QString s = QString::number(v, 'f', 4);
        if (s.contains('.')) {
            while (s.endsWith('0')) {
                s.chop(1);
            }
            if (s.endsWith('.')) {
                s.chop(1);
            }
        }
        return s == "-0" ? QStringLiteral("0") : s;
    }

    QString fmt(const RS_Vector& v) {
        return QStringLiteral("(%1, %2)").arg(fmt(v.x), fmt(v.y));
    }

    QString unitName(RS_Graphic* graphic) {
        return graphic != nullptr ? RS_Units::unitToString(graphic->getUnit()) : QStringLiteral("units");
    }

    /** Visible area in world (drawing) coordinates: bounding box of the view corners. */
    bool visibleArea(RS_GraphicView* view, RS_Vector& min, RS_Vector& max) {
        LC_GraphicViewport* vp = view->getViewPort();
        if (vp == nullptr) {
            return false;
        }
        const RS_Vector lb = vp->getUCSViewLeftBottom();
        const RS_Vector rt = vp->getUCSViewRightTop();
        min = RS_Vector(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
        max = RS_Vector(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest());
        for (const RS_Vector& c : {RS_Vector(lb.x, lb.y), RS_Vector(rt.x, lb.y), RS_Vector(rt.x, rt.y), RS_Vector(lb.x, rt.y)}) {
            const RS_Vector w = vp->toWorld(c);
            min = RS_Vector(std::min(min.x, w.x), std::min(min.y, w.y));
            max = RS_Vector(std::max(max.x, w.x), std::max(max.y, w.y));
        }
        return true;
    }

    QString describeView(RS_GraphicView* view) {
        RS_Graphic* graphic = view->getGraphic();
        const QString unit = unitName(graphic);
        QStringList lines;
        RS_Vector min;
        RS_Vector max;
        if (visibleArea(view, min, max)) {
            lines << QStringLiteral("Visible area: x %1 .. %2, y %3 .. %4 %5 (centre %6).")
                         .arg(fmt(min.x), fmt(max.x), fmt(min.y), fmt(max.y), unit, fmt((min + max) * 0.5));
        }
        LC_GraphicViewport* vp = view->getViewPort();
        if (vp != nullptr && vp->getFactor().x > 0) {
            lines << QStringLiteral("View size: %1 x %2 px; 1 px = %3 %4.")
                         .arg(vp->getWidth()).arg(vp->getHeight()).arg(fmt(1.0 / vp->getFactor().x), unit);
        }
        if (graphic != nullptr) {
            graphic->calculateBorders();
            if (graphic->getMin().valid && graphic->getMax().valid && graphic->getMax().x >= graphic->getMin().x) {
                lines << QStringLiteral("Drawing extents: %1 .. %2.").arg(fmt(graphic->getMin()), fmt(graphic->getMax()));
            }
            else {
                lines << QStringLiteral("The drawing is empty.");
            }
        }
        return lines.join('\n');
    }

    LC_AIContent screenshot(RS_GraphicView* view) {
        view->redraw(RS2::RedrawAll, true);
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        QPixmap pixmap = view->grab();
        if (pixmap.width() > MAX_SCREENSHOT_EDGE || pixmap.height() > MAX_SCREENSHOT_EDGE) {
            pixmap = pixmap.scaled(MAX_SCREENSHOT_EDGE, MAX_SCREENSHOT_EDGE, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        pixmap.save(&buffer, "PNG");
        return LC_AIContent::makeImage(png, QStringLiteral("image/png"));
    }

    /** Result of a view tool: what is visible now, plus a screenshot when asked for. */
    LC_AIToolResult viewResult(RS_GraphicView* view, const QString& what, const bool capture) {
        LC_AIToolResult result = LC_AIToolResult::text(what + "\n" + describeView(view));
        if (capture) {
            result.content.prepend(screenshot(view));
        }
        return result;
    }

    double arg(const QJsonObject& args, const char* name, bool& ok) {
        const QJsonValue v = args.value(QLatin1String(name));
        ok = ok && v.isDouble() && std::isfinite(v.toDouble());
        return v.toDouble();
    }

    // ------------------------------------------------------------------ entities

    QString typeName(const RS2::EntityType type) {
        static const QMap<RS2::EntityType, QString> names = {
            {RS2::EntityLine, "line"}, {RS2::EntityCircle, "circle"}, {RS2::EntityArc, "arc"},
            {RS2::EntityEllipse, "ellipse"}, {RS2::EntityPoint, "point"}, {RS2::EntityPolyline, "polyline"},
            {RS2::EntitySpline, "spline"}, {RS2::EntitySplinePoints, "spline"}, {RS2::EntityText, "text"},
            {RS2::EntityMText, "mtext"}, {RS2::EntityHatch, "hatch"}, {RS2::EntityInsert, "insert"},
            {RS2::EntitySolid, "solid"}, {RS2::EntityImage, "image"}, {RS2::EntityConstructionLine, "xline"},
            {RS2::EntityDimAligned, "dim_aligned"}, {RS2::EntityDimLinear, "dim_linear"},
            {RS2::EntityDimRadial, "dim_radial"}, {RS2::EntityDimDiametric, "dim_diametric"},
            {RS2::EntityDimAngular, "dim_angular"}, {RS2::EntityDimArc, "dim_arc"},
            {RS2::EntityDimOrdinate, "dim_ordinate"}, {RS2::EntityDimLeader, "leader"},
            {RS2::EntityMLeader, "mleader"}, {RS2::EntityTolerance, "tolerance"}, {RS2::EntityWipeout, "wipeout"},
            {RS2::EntityParabola, "parabola"}, {RS2::EntityHyperbola, "hyperbola"},
        };
        return names.value(type, QStringLiteral("entity"));
    }

    double degrees(const double radians) {
        return RS_Math::rad2deg(RS_Math::correctAngle(radians));
    }

    QString describeEntity(RS_Entity* e) {
        const QString layer = e->getLayer() != nullptr ? e->getLayer()->getName() : QStringLiteral("0");
        QString line = QStringLiteral("#%1 %2 layer=%3").arg(e->getId()).arg(typeName(e->rtti()), layer);
        switch (e->rtti()) {
            case RS2::EntityLine: {
                auto* l = static_cast<RS_Line*>(e);
                const RS_Vector a = l->getStartpoint();
                const RS_Vector b = l->getEndpoint();
                line += QStringLiteral(" %1 -> %2 length=%3 angle=%4°")
                            .arg(fmt(a), fmt(b), fmt(a.distanceTo(b)), fmt(degrees(a.angleTo(b))));
                break;
            }
            case RS2::EntityCircle: {
                auto* c = static_cast<RS_Circle*>(e);
                line += QStringLiteral(" centre=%1 radius=%2").arg(fmt(c->getCenter()), fmt(c->getRadius()));
                break;
            }
            case RS2::EntityArc: {
                auto* a = static_cast<RS_Arc*>(e);
                line += QStringLiteral(" centre=%1 radius=%2 start=%3° end=%4°")
                            .arg(fmt(a->getCenter()), fmt(a->getRadius()), fmt(degrees(a->getAngle1())),
                                 fmt(degrees(a->getAngle2())));
                break;
            }
            case RS2::EntityPoint:
                line += QStringLiteral(" at=%1").arg(fmt(static_cast<RS_Point*>(e)->getPos()));
                break;
            case RS2::EntityText: {
                auto* t = static_cast<RS_Text*>(e);
                line += QStringLiteral(" at=%1 height=%2 text=\"%3\"")
                            .arg(fmt(t->getInsertionPoint()), fmt(t->getHeight()), t->getText().left(80).replace('\n', " / "));
                break;
            }
            case RS2::EntityMText: {
                auto* t = static_cast<RS_MText*>(e);
                line += QStringLiteral(" at=%1 height=%2 text=\"%3\"")
                            .arg(fmt(t->getInsertionPoint()), fmt(t->getHeight()), t->getText().left(80).replace('\n', " / "));
                break;
            }
            default:
                if (auto* d = dynamic_cast<RS_Dimension*>(e)) {
                    line += QStringLiteral(" value=%1").arg(fmt(d->getMeasurement()));
                }
                if (e->getMin().valid && e->getMax().valid) {
                    line += QStringLiteral(" bounds=%1..%2").arg(fmt(e->getMin()), fmt(e->getMax()));
                }
                if (e->rtti() == RS2::EntityPolyline || e->rtti() == RS2::EntityEllipse || e->rtti() == RS2::EntitySpline) {
                    line += QStringLiteral(" length=%1").arg(fmt(e->getLength()));
                }
                break;
        }
        if (e->isSelected()) {
            line += QStringLiteral(" [selected]");
        }
        return line;
    }

    // ------------------------------------------------------------------ tools

    LC_AITool captureView() {
        LC_AITool t;
        t.name = "capture_view";
        t.label = "Capture view";
        t.description = "Take a screenshot of the drawing view exactly as the user sees it, with a description of "
                        "the visible area in drawing coordinates. Use it to look at the design, and to check the "
                        "result after any change.";
        t.parameters = object({});
        t.execute = [](const QJsonObject&) {
            RS_GraphicView* view = currentView();
            if (view == nullptr) {
                return LC_AIToolResult::error(NO_DRAWING);
            }
            return viewResult(view, QStringLiteral("Screenshot of the drawing view."), true);
        };
        return t;
    }

    LC_AITool getView() {
        LC_AITool t;
        t.name = "get_view";
        t.label = "Get view";
        t.description = "Describe the current view without a screenshot: the visible area in drawing coordinates, "
                        "the scale (drawing units per pixel) and the drawing extents.";
        t.parameters = object({});
        t.execute = [](const QJsonObject&) {
            RS_GraphicView* view = currentView();
            return view == nullptr ? LC_AIToolResult::error(NO_DRAWING) : LC_AIToolResult::text(describeView(view));
        };
        return t;
    }

    LC_AITool zoomToFit() {
        LC_AITool t;
        t.name = "zoom_to_fit";
        t.label = "Zoom to fit";
        t.description = "Zoom so the whole drawing fits in the view (zoom extents).";
        t.parameters = object({{"capture", CAPTURE_ARG}});
        t.execute = [](const QJsonObject& args) {
            RS_GraphicView* view = currentView();
            if (view == nullptr || view->getViewPort() == nullptr) {
                return LC_AIToolResult::error(NO_DRAWING);
            }
            view->getViewPort()->zoomAuto(false, true);
            return viewResult(view, QStringLiteral("Zoomed to fit the drawing."), args.value("capture").toBool());
        };
        return t;
    }

    LC_AITool zoomToArea() {
        LC_AITool t;
        t.name = "zoom_to_area";
        t.label = "Zoom to area";
        t.description = "Zoom the view to a rectangle given by two opposite corners in drawing coordinates. "
                        "The aspect ratio is kept, so slightly more than the rectangle may be visible.";
        t.parameters = object({{"x1", number("X of the first corner")}, {"y1", number("Y of the first corner")},
                               {"x2", number("X of the opposite corner")}, {"y2", number("Y of the opposite corner")},
                               {"capture", CAPTURE_ARG}},
                              {"x1", "y1", "x2", "y2"});
        t.execute = [](const QJsonObject& args) {
            RS_GraphicView* view = currentView();
            if (view == nullptr || view->getViewPort() == nullptr) {
                return LC_AIToolResult::error(NO_DRAWING);
            }
            bool ok = true;
            const RS_Vector a(arg(args, "x1", ok), arg(args, "y1", ok));
            const RS_Vector b(arg(args, "x2", ok), arg(args, "y2", ok));
            if (!ok) {
                return LC_AIToolResult::error(QStringLiteral("x1, y1, x2 and y2 must all be numbers."));
            }
            if (std::abs(a.x - b.x) < RS_TOLERANCE && std::abs(a.y - b.y) < RS_TOLERANCE) {
                return LC_AIToolResult::error(QStringLiteral("The two corners are the same point; give a rectangle."));
            }
            LC_GraphicViewport* vp = view->getViewPort();
            RS_Vector min(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
            RS_Vector max(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest());
            for (const RS_Vector& w : {a, b, RS_Vector(a.x, b.y), RS_Vector(b.x, a.y)}) {
                const RS_Vector u = vp->toUCS(w);
                min = RS_Vector(std::min(min.x, u.x), std::min(min.y, u.y));
                max = RS_Vector(std::max(max.x, u.x), std::max(max.y, u.y));
            }
            vp->zoomWindow(min, max, true);
            return viewResult(view, QStringLiteral("Zoomed to the area %1 .. %2.").arg(fmt(a), fmt(b)),
                              args.value("capture").toBool());
        };
        return t;
    }

    LC_AITool zoom() {
        LC_AITool t;
        t.name = "zoom";
        t.label = "Zoom";
        t.description = "Zoom in or out by a factor: above 1 zooms in (2 = twice as large), below 1 zooms out "
                        "(0.5 = half as large). Optionally keep a given drawing point fixed instead of the view centre.";
        t.parameters = object({{"factor", number("Zoom factor, e.g. 2 to zoom in, 0.5 to zoom out")},
                               {"center_x", number("Optional X of the point to zoom around")},
                               {"center_y", number("Optional Y of the point to zoom around")},
                               {"capture", CAPTURE_ARG}},
                              {"factor"});
        t.execute = [](const QJsonObject& args) {
            RS_GraphicView* view = currentView();
            if (view == nullptr || view->getViewPort() == nullptr) {
                return LC_AIToolResult::error(NO_DRAWING);
            }
            bool ok = true;
            const double factor = arg(args, "factor", ok);
            if (!ok || factor <= 0 || factor > 1000) {
                return LC_AIToolResult::error(QStringLiteral("factor must be a number between 0 and 1000 (not 0)."));
            }
            LC_GraphicViewport* vp = view->getViewPort();
            RS_Vector center(false);
            if (args.contains("center_x") && args.contains("center_y")) {
                bool centerOk = true;
                const RS_Vector w(arg(args, "center_x", centerOk), arg(args, "center_y", centerOk));
                if (!centerOk) {
                    return LC_AIToolResult::error(QStringLiteral("center_x and center_y must be numbers."));
                }
                center = vp->toUCS(w);
            }
            if (factor >= 1.0) {
                vp->zoomIn(factor, center);
            }
            else {
                vp->zoomOut(1.0 / factor, center);
            }
            return viewResult(view, QStringLiteral("Zoomed by %1.").arg(fmt(factor)), args.value("capture").toBool());
        };
        return t;
    }

    LC_AITool pan() {
        LC_AITool t;
        t.name = "pan";
        t.label = "Pan";
        t.description = "Move the view by a distance in drawing units without changing the zoom: positive dx shows "
                        "what is further right, positive dy shows what is further up.";
        t.parameters = object({{"dx", number("Distance to move right (negative: left), in drawing units")},
                               {"dy", number("Distance to move up (negative: down), in drawing units")},
                               {"capture", CAPTURE_ARG}},
                              {"dx", "dy"});
        t.execute = [](const QJsonObject& args) {
            RS_GraphicView* view = currentView();
            if (view == nullptr || view->getViewPort() == nullptr) {
                return LC_AIToolResult::error(NO_DRAWING);
            }
            bool ok = true;
            const RS_Vector delta(arg(args, "dx", ok), arg(args, "dy", ok));
            if (!ok) {
                return LC_AIToolResult::error(QStringLiteral("dx and dy must be numbers."));
            }
            LC_GraphicViewport* vp = view->getViewPort();
            const RS_Vector ucsDelta = vp->toUCSDelta(delta);
            // same window shifted by the delta: the scale stays as it is
            vp->zoomWindow(vp->getUCSViewLeftBottom() + ucsDelta, vp->getUCSViewRightTop() + ucsDelta, true);
            return viewResult(view, QStringLiteral("Panned by %1.").arg(fmt(delta)), args.value("capture").toBool());
        };
        return t;
    }

    LC_AITool drawingInfo() {
        LC_AITool t;
        t.name = "get_drawing_info";
        t.label = "Drawing info";
        t.description = "Summarise the open drawing: file, units, extents, entity counts by type, layers (with "
                        "their entity counts and visibility), blocks and the current selection.";
        t.parameters = object({});
        t.execute = [](const QJsonObject&) {
            RS_GraphicView* view = currentView();
            RS_Graphic* graphic = view != nullptr ? view->getGraphic() : nullptr;
            if (graphic == nullptr) {
                return LC_AIToolResult::error(NO_DRAWING);
            }
            QStringList out;
            const QString file = graphic->getFilename();
            out << QStringLiteral("File: %1").arg(file.isEmpty() ? QStringLiteral("(not saved yet)") : QFileInfo(file).fileName());
            out << QStringLiteral("Units: %1").arg(unitName(graphic));
            graphic->calculateBorders();
            if (graphic->getMin().valid && graphic->getMax().valid && graphic->getMax().x >= graphic->getMin().x) {
                const RS_Vector size = graphic->getMax() - graphic->getMin();
                out << QStringLiteral("Extents: %1 .. %2 (size %3 x %4)")
                           .arg(fmt(graphic->getMin()), fmt(graphic->getMax()), fmt(size.x), fmt(size.y));
            }
            QMap<QString, int> byType;
            QMap<QString, int> byLayer;
            int total = 0;
            int selected = 0;
            for (RS_Entity* e : *graphic) {
                if (e == nullptr || e->isDeleted()) {
                    continue;
                }
                ++total;
                byType[typeName(e->rtti())] += 1;
                byLayer[e->getLayer() != nullptr ? e->getLayer()->getName() : QStringLiteral("0")] += 1;
                selected += e->isSelected() ? 1 : 0;
            }
            QStringList types;
            for (auto it = byType.cbegin(); it != byType.cend(); ++it) {
                types << QStringLiteral("%1 %2").arg(it.value()).arg(it.key());
            }
            out << QStringLiteral("Entities: %1%2").arg(total).arg(types.isEmpty() ? QString() : " (" + types.join(", ") + ")");
            out << QStringLiteral("Selected: %1").arg(selected);
            out << QStringLiteral("Layers:");
            for (RS_Layer* layer : *graphic->getLayerList()) {
                QStringList flags;
                if (layer->isFrozen()) {
                    flags << "hidden";
                }
                if (layer->isLocked()) {
                    flags << "locked";
                }
                if (layer == graphic->getLayerList()->getActive()) {
                    flags << "current";
                }
                out << QStringLiteral("- %1: %2 entities%3").arg(layer->getName()).arg(byLayer.value(layer->getName()))
                           .arg(flags.isEmpty() ? QString() : " [" + flags.join(", ") + "]");
            }
            out << QStringLiteral("Blocks: %1").arg(graphic->countBlocks());
            return LC_AIToolResult::text(out.join('\n'));
        };
        return t;
    }

    LC_AITool listEntities() {
        LC_AITool t;
        t.name = "list_entities";
        t.label = "List entities";
        t.description = "List entities of the drawing with their exact geometry (end points, centres, radii, "
                        "lengths, text). Filter by type, layer, an area, or the selection.";
        t.parameters = object({
            {"type", string("Only this type: line, circle, arc, polyline, text, mtext, point, ellipse, spline, "
                            "hatch, insert, dim_linear, dim_aligned, ...")},
            {"layer", string("Only entities on this layer")},
            {"x1", number("Area filter: X of one corner")}, {"y1", number("Area filter: Y of one corner")},
            {"x2", number("Area filter: X of the opposite corner")}, {"y2", number("Area filter: Y of the opposite corner")},
            {"selected_only", boolean("Only selected entities")},
            {"limit", integer(QStringLiteral("Maximum number of entities (default %1, at most %2)").arg(DEFAULT_LIST_LIMIT).arg(MAX_LIST_LIMIT))},
            {"offset", integer("Skip this many matching entities (for paging)")}});
        t.execute = [](const QJsonObject& args) {
            RS_GraphicView* view = currentView();
            RS_Graphic* graphic = view != nullptr ? view->getGraphic() : nullptr;
            if (graphic == nullptr) {
                return LC_AIToolResult::error(NO_DRAWING);
            }
            const QString type = args.value("type").toString().trimmed().toLower();
            const QString layer = args.value("layer").toString();
            const bool selectedOnly = args.value("selected_only").toBool();
            const int limit = qBound(1, args.contains("limit") ? args.value("limit").toInt() : DEFAULT_LIST_LIMIT, MAX_LIST_LIMIT);
            const int offset = qMax(0, args.value("offset").toInt());
            bool hasArea = args.contains("x1") || args.contains("y1") || args.contains("x2") || args.contains("y2");
            bool ok = true;
            RS_Vector areaMin;
            RS_Vector areaMax;
            if (hasArea) {
                const double x1 = arg(args, "x1", ok);
                const double y1 = arg(args, "y1", ok);
                const double x2 = arg(args, "x2", ok);
                const double y2 = arg(args, "y2", ok);
                if (!ok) {
                    return LC_AIToolResult::error(QStringLiteral("An area filter needs all of x1, y1, x2 and y2 as numbers."));
                }
                areaMin = RS_Vector(std::min(x1, x2), std::min(y1, y2));
                areaMax = RS_Vector(std::max(x1, x2), std::max(y1, y2));
            }
            QStringList lines;
            int matched = 0;
            for (RS_Entity* e : *graphic) {
                if (e == nullptr || e->isDeleted()) {
                    continue;
                }
                if (!type.isEmpty() && typeName(e->rtti()) != type) {
                    continue;
                }
                if (!layer.isEmpty() && (e->getLayer() == nullptr || e->getLayer()->getName() != layer)) {
                    continue;
                }
                if (selectedOnly && !e->isSelected()) {
                    continue;
                }
                if (hasArea && (e->getMax().x < areaMin.x || e->getMin().x > areaMax.x || e->getMax().y < areaMin.y
                                || e->getMin().y > areaMax.y)) {
                    continue;
                }
                if (matched >= offset && lines.size() < limit) {
                    lines << describeEntity(e);
                }
                ++matched;
            }
            if (matched == 0) {
                return LC_AIToolResult::text(QStringLiteral("No entities match."));
            }
            QString header = QStringLiteral("%1 matching entities (units: %2).").arg(matched).arg(unitName(graphic));
            if (matched > offset + lines.size()) {
                header += QStringLiteral(" Showing %1-%2; use offset %3 for more.")
                              .arg(offset + 1).arg(offset + lines.size()).arg(offset + lines.size());
            }
            return LC_AIToolResult::text(header + "\n" + lines.join('\n'));
        };
        return t;
    }

    LC_AITool listCommands() {
        LC_AITool t;
        t.name = "list_commands";
        t.label = "List commands";
        t.description = "List the commands OpenCAD's command line accepts (optionally only those starting with a "
                        "prefix), and how to enter coordinates.";
        t.parameters = object({{"prefix", string("Only commands starting with this text")}});
        t.execute = [](const QJsonObject& args) {
            const QStringList commands = RS_COMMANDS->complete(args.value("prefix").toString());
            return LC_AIToolResult::text(
                QStringLiteral("Commands (%1): %2\n\n"
                               "Coordinates: \"x,y\" absolute, \"@dx,dy\" relative to the last point, "
                               "\"@distance<angle\" polar relative (angle in degrees, counter-clockwise from +X). "
                               "Plain numbers answer prompts such as a radius. \"ESC\" ends or cancels the current command.")
                    .arg(commands.size()).arg(commands.join(", ")));
        };
        return t;
    }

    LC_AITool runCommand() {
        LC_AITool t;
        t.name = "run_command";
        t.label = "Run command";
        t.description =
            "Type lines into OpenCAD's command line, in order, exactly as the user would at the \"Enter Command\" "
            "prompt, and return what the command line printed plus its current prompt. Use it to draw, edit or query "
            "the drawing. Example for a 100 x 50 rectangle from the origin: [\"line\", \"0,0\", \"100,0\", "
            "\"@0,50\", \"@-100,0\", \"close\"]; a circle: [\"circle\", \"0,0\", \"25\"]. Coordinates: \"x,y\", "
            "\"@dx,dy\" relative, \"@distance<angle\" polar. Send \"ESC\" to end or cancel the current command, "
            "\"undo\" to revert the last change. Do not use the text or mtext commands (they open a dialog and wait for "
            "the user); place labels with add_text. Check the result with capture_view or list_entities.";
        t.parameters = object({{"commands", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}},
                                                        {"description", "Lines to type, one per entry, e.g. [\"circle\", \"0,0\", \"25\"]"}}},
                               {"capture", CAPTURE_ARG}},
                              {"commands"});
        t.execute = [](const QJsonObject& args) {
            auto& app = QC_ApplicationWindow::getAppWindow();
            RS_GraphicView* view = currentView();
            auto* widget = app ? app->findChild<QG_CommandWidget*>() : nullptr;
            if (view == nullptr || widget == nullptr) {
                return LC_AIToolResult::error(NO_DRAWING);
            }
            QStringList commands;
            for (const QJsonValue& v : args.value("commands").toArray()) {
                if (!v.toString().trimmed().isEmpty()) {
                    commands << v.toString().trimmed();
                }
            }
            if (commands.isEmpty() && !args.value("commands").toString().trimmed().isEmpty()) {
                commands << args.value("commands").toString().trimmed(); // a single string instead of a list
            }
            if (commands.isEmpty()) {
                return LC_AIToolResult::error(QStringLiteral("commands must be a non-empty list of strings."));
            }
            const QString before = widget->teHistory->toPlainText();
            for (const QString& command : std::as_const(commands)) {
                if (command.compare("ESC", Qt::CaseInsensitive) == 0 || command.compare("escape", Qt::CaseInsensitive) == 0) {
                    widget->escape();
                }
                else {
                    widget->handleCommand(command);
                }
                QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            }
            const QString after = widget->teHistory->toPlainText();
            QString output = after.startsWith(before) ? after.mid(before.size()).trimmed() : after.right(MAX_COMMAND_OUTPUT).trimmed();
            if (output.size() > MAX_COMMAND_OUTPUT) {
                output = "..." + output.right(MAX_COMMAND_OUTPUT);
            }
            QString text = QStringLiteral("Typed %1 line(s).\nCommand line output:\n%2\nCurrent prompt: %3")
                               .arg(commands.size())
                               .arg(output.isEmpty() ? QStringLiteral("(nothing)") : output,
                                    widget->lCommand->text());
            LC_AIToolResult result = LC_AIToolResult::text(text);
            if (args.value("capture").toBool()) {
                result.content.prepend(screenshot(view));
            }
            return result;
        };
        return t;
    }
    LC_AITool openDrawing() {
        LC_AITool t;
        t.name = "open_drawing";
        t.label = "Open drawing";
        t.description = "Open a drawing file (DXF or DWG) from this computer in OpenCAD, in a new tab that becomes the "
                        "active drawing; the other tools then act on it. Use it to bring in drawings produced elsewhere, "
                        "e.g. the DXF paths an MCP server such as UniCAD returns from drawings_render.";
        t.parameters = object({{"path", string("Absolute path of the .dxf or .dwg file")},
                               {"capture", CAPTURE_ARG}},
                              {"path"});
        t.execute = [](const QJsonObject& args) {
            auto& app = QC_ApplicationWindow::getAppWindow();
            if (!app) {
                return LC_AIToolResult::error(QStringLiteral("OpenCAD's main window is not available."));
            }
            const QString path = QDir::fromNativeSeparators(args.value("path").toString().trimmed());
            const QFileInfo info(path);
            if (path.isEmpty() || !info.isAbsolute()) {
                return LC_AIToolResult::error(QStringLiteral("Give the absolute path of the file."));
            }
            if (!info.isFile()) {
                return LC_AIToolResult::error(QStringLiteral("No file at %1 on this computer. (A file on a remote MCP "
                                                             "server's disk cannot be opened here.)").arg(path));
            }
            const QString suffix = info.suffix().toLower();
            if (suffix != "dxf" && suffix != "dwg") {
                return LC_AIToolResult::error(QStringLiteral("Only .dxf and .dwg files can be opened (got .%1).").arg(suffix));
            }
            app->openFile(info.absoluteFilePath());
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            RS_GraphicView* view = currentView();
            if (view == nullptr || view->getGraphic() == nullptr
                || QFileInfo(view->getGraphic()->getFilename()).absoluteFilePath() != info.absoluteFilePath()) {
                return LC_AIToolResult::error(QStringLiteral("OpenCAD could not open %1.").arg(info.fileName()));
            }
            if (view->getViewPort() != nullptr) {
                view->getViewPort()->zoomAuto(false, true);
            }
            return viewResult(view, QStringLiteral("Opened %1; it is now the active drawing.").arg(info.fileName()),
                              args.value("capture").toBool());
        };
        return t;
    }

    LC_AITool viewImageFile() {
        LC_AITool t;
        t.name = "view_image_file";
        t.label = "View image";
        t.description = "Look at a PNG or JPEG image file on this computer, e.g. a sheet preview an MCP server such as "
                        "UniCAD rendered (drawings_preview returns its path).";
        t.parameters = object({{"path", string("Absolute path of the .png or .jpg file")}}, {"path"});
        t.execute = [](const QJsonObject& args) {
            const QString path = QDir::fromNativeSeparators(args.value("path").toString().trimmed());
            QImage image(path);
            if (image.isNull()) {
                return LC_AIToolResult::error(QStringLiteral("Could not read an image at %1.").arg(path));
            }
            const QSize original = image.size();
            if (image.width() > MAX_SCREENSHOT_EDGE || image.height() > MAX_SCREENSHOT_EDGE) {
                image = image.scaled(MAX_SCREENSHOT_EDGE, MAX_SCREENSHOT_EDGE, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            LC_AIToolResult result = LC_AIToolResult::text(
                QStringLiteral("%1 (%2 x %3 px%4)").arg(QFileInfo(path).fileName()).arg(original.width()).arg(original.height())
                    .arg(original != image.size() ? QStringLiteral(", scaled to %1 x %2").arg(image.width()).arg(image.height()) : QString()));
            result.content.prepend(LC_AIContent::makeImage(png, QStringLiteral("image/png")));
            return result;
        };
        return t;
    }
    LC_AITool addText() {
        LC_AITool t;
        t.name = "add_text";
        t.label = "Add text";
        t.description =
            "Place text labels in the drawing (names, notes, numbers, titles) without any dialog. Give one label "
            "(text, x, y, ...) or several in \"labels\"; all of them are added as one undo step. Text with line "
            "breaks becomes multi-line text. The point x, y is where the text is anchored according to align/valign "
            "(e.g. align center + valign middle centres the text on the point). Check the result with capture_view.";
        const QJsonObject labelProperties{
            {"text", string("The text. Use \\n for line breaks (multi-line text).")},
            {"x", number("X of the anchor point, in drawing units")},
            {"y", number("Y of the anchor point, in drawing units")},
            {"height", number("Text height in drawing units. Default: 1/40 of the visible area's height.")},
            {"angle", number("Rotation in degrees, counter-clockwise (default 0)")},
            {"align", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"left", "center", "right"}},
                                  {"description", "Horizontal anchor (default left)"}}},
            {"valign", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"baseline", "bottom", "middle", "top"}},
                                   {"description", "Vertical anchor (default baseline; multi-line text uses top for baseline)"}}},
            {"layer", string("Existing layer to put the text on (default: the current layer)")}};
        QJsonObject properties = labelProperties;
        properties.insert("labels", QJsonObject{{"type", "array"},
                                                {"description", "Several labels to add at once, each with the fields above"},
                                                {"items", object(labelProperties, {"text", "x", "y"})}});
        properties.insert("capture", CAPTURE_ARG);
        t.parameters = object(properties);
        t.execute = [](const QJsonObject& args) {
            RS_GraphicView* view = currentView();
            RS_Graphic* graphic = view != nullptr ? view->getGraphic() : nullptr;
            RS_Document* document = view != nullptr ? view->getDocument() : nullptr;
            if (graphic == nullptr || document == nullptr || view->getViewPort() == nullptr) {
                return LC_AIToolResult::error(NO_DRAWING);
            }
            QList<QJsonObject> specs;
            for (const QJsonValue& v : args.value("labels").toArray()) {
                specs << v.toObject();
            }
            if (args.contains("text")) {
                specs.prepend(args);
            }
            if (specs.isEmpty()) {
                return LC_AIToolResult::error(QStringLiteral("Give text, x and y, or a list of labels."));
            }
            RS_Vector visMin;
            RS_Vector visMax;
            double defaultHeight = 2.5;
            if (visibleArea(view, visMin, visMax) && visMax.y > visMin.y) {
                // a readable size at the current zoom, rounded to a tidy number
                const double raw = (visMax.y - visMin.y) / 40.0;
                const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
                const double step = raw / magnitude < 2 ? 1 : raw / magnitude < 5 ? 2 : 5;
                defaultHeight = step * magnitude;
            }
            RS_Layer* activeLayer = graphic->getLayerList()->getActive();

            QList<RS_Entity*> entities;
            QStringList placed;
            const auto discard = [&entities] {
                qDeleteAll(entities);
                entities.clear();
            };
            for (int i = 0; i < specs.size(); ++i) {
                const QJsonObject& spec = specs[i];
                const QString which = specs.size() > 1 ? QStringLiteral("Label %1: ").arg(i + 1) : QString();
                const QString text = spec.value("text").toString();
                bool ok = true;
                const RS_Vector pos(arg(spec, "x", ok), arg(spec, "y", ok));
                if (text.trimmed().isEmpty() || !ok) {
                    discard();
                    return LC_AIToolResult::error(which + QStringLiteral("needs a non-empty text and numeric x and y."));
                }
                const double height = spec.value("height").isDouble() ? spec.value("height").toDouble() : defaultHeight;
                if (!(height > 0) || !std::isfinite(height)) {
                    discard();
                    return LC_AIToolResult::error(which + QStringLiteral("height must be a positive number."));
                }
                const double angle = RS_Math::deg2rad(spec.value("angle").toDouble(0.0));
                const QString align = spec.value("align").toString("left").toLower();
                const QString valign = spec.value("valign").toString("baseline").toLower();
                RS_Layer* layer = activeLayer;
                const QString layerName = spec.value("layer").toString();
                if (!layerName.isEmpty()) {
                    layer = graphic->getLayerList()->find(layerName);
                    if (layer == nullptr) {
                        QStringList names;
                        for (RS_Layer* l : *graphic->getLayerList()) {
                            names << l->getName();
                        }
                        discard();
                        return LC_AIToolResult::error(which + QStringLiteral("there is no layer \"%1\". Layers: %2.")
                                                                  .arg(layerName, names.join(", ")));
                    }
                }
                const bool multiline = text.contains('\n');
                RS_Entity* entity = nullptr;
                if (multiline) {
                    const RS_MTextData::HAlign ha = align == "center" ? RS_MTextData::HACenter
                                                    : align == "right" ? RS_MTextData::HARight : RS_MTextData::HALeft;
                    const RS_MTextData::VAlign va = valign == "middle" ? RS_MTextData::VAMiddle
                                                    : valign == "bottom" ? RS_MTextData::VABottom : RS_MTextData::VATop;
                    int longest = 1;
                    for (const QString& line : text.split('\n')) {
                        longest = std::max(longest, int(line.size()));
                    }
                    // reference width wide enough that the given lines are not wrapped again
                    const RS_MTextData data(pos, height, longest * height * 1.2, va, ha, RS_MTextData::LeftToRight,
                                            RS_MTextData::AtLeast, 1.0, text, "standard", angle);
                    entity = new RS_MText(document, data);
                }
                else {
                    const RS_TextData::HAlign ha = align == "center" ? RS_TextData::HACenter
                                                   : align == "right" ? RS_TextData::HARight : RS_TextData::HALeft;
                    const RS_TextData::VAlign va = valign == "middle" ? RS_TextData::VAMiddle
                                                   : valign == "bottom" ? RS_TextData::VABottom
                                                   : valign == "top" ? RS_TextData::VATop : RS_TextData::VABaseline;
                    const RS_TextData data(pos, pos, height, 1.0, va, ha, RS_TextData::None, text, "standard", angle);
                    entity = new RS_Text(document, data);
                }
                entity->setLayer(layer);
                entity->update();
                entities << entity;
                placed << QStringLiteral("\"%1\" at %2, height %3, %4%5 on layer %6")
                              .arg(QString(text).replace('\n', " / ").left(60), fmt(pos), fmt(height),
                                   multiline ? QStringLiteral("multi-line, ") : QString(),
                                   align + "/" + (multiline && valign == "baseline" ? QStringLiteral("top") : valign),
                                   layer != nullptr ? layer->getName() : QStringLiteral("0"));
            }
            const bool added = document->undoableModify(view->getViewPort(), [&entities](LC_DocumentModificationBatch& ctx) -> bool {
                ctx.setActiveLayerAndPen(false, true); // each label already has its layer
                for (RS_Entity* e : std::as_const(entities)) {
                    ctx.add(e);
                }
                return true;
            });
            if (!added) {
                return LC_AIToolResult::error(QStringLiteral("OpenCAD did not accept the text (the drawing may be read-only)."));
            }
            view->redraw();
            QString text = QStringLiteral("Added %1 text label(s) as one undo step:\n- %2")
                               .arg(placed.size()).arg(placed.join("\n- "));
            LC_AIToolResult result = LC_AIToolResult::text(text);
            if (args.value("capture").toBool()) {
                result.content.prepend(screenshot(view));
            }
            return result;
        };
        return t;
    }
}

void LC_AITools::setViewForTests(RS_GraphicView* view) {
    g_viewForTests = view;
}

QList<LC_AITool> LC_AITools::all() {
    return {captureView(), getView(), zoomToFit(), zoomToArea(), zoom(), pan(),
            drawingInfo(), listEntities(), listCommands(), runCommand(), addText(), openDrawing(), viewImageFile()};
}
