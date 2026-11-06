/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aidrawingchecker.h"

#include <QAction>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonArray>
#include <QLabel>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>

#include "lc_aiassistantwidget.h"
#include "lc_aichattranscript.h"
#include "lc_aiicons.h"
#include "lc_aiprovidersdialog.h"
#include "lc_graphicviewport.h"
#include "lc_undosection.h"
#include "rs_arc.h"
#include "rs_block.h"
#include "rs_blocklist.h"
#include "rs_circle.h"
#include "rs_dimension.h"
#include "rs_document.h"
#include "rs_ellipse.h"
#include "rs_entity.h"
#include "rs_graphic.h"
#include "rs_graphicview.h"
#include "rs_insert.h"
#include "rs_layer.h"
#include "rs_layerlist.h"
#include "rs_line.h"
#include "rs_math.h"
#include "rs_mtext.h"
#include "rs_pen.h"
#include "rs_point.h"
#include "rs_polyline.h"
#include "rs_selection.h"
#include "rs_text.h"
#include "rs_units.h"

namespace {
    using Id = unsigned long long;
    using Issue = LC_AIDrawingChecker::Issue;
    using Report = LC_AIDrawingChecker::Report;
    using Options = LC_AIDrawingChecker::Options;

    struct Rule {
        const char* id;
        const char* title;
        const char* severity;
        bool fixable; // fix_drawing_issues handles it
    };
    // report order: what breaks the geometry first, housekeeping last (titles show in the chat card)
    const Rule RULES[] = {
        {"zero_length", QT_TRANSLATE_NOOP("QObject", "Zero-length geometry"), "error", true},
        {"duplicate", QT_TRANSLATE_NOOP("QObject", "Exact duplicates"), "error", true},
        {"overlap", QT_TRANSLATE_NOOP("QObject", "Overlapping lines and arcs"), "warning", true},
        {"endpoint_gap", QT_TRANSLATE_NOOP("QObject", "Endpoints that nearly meet"), "warning", true},
        {"nearly_closed", QT_TRANSLATE_NOOP("QObject", "Polylines left open"), "warning", true},
        {"tiny_segment", QT_TRANSLATE_NOOP("QObject", "Tiny segments"), "warning", false},
        {"outlier", QT_TRANSLATE_NOOP("QObject", "Far from the drawing"), "warning", false},
        {"block_insert", QT_TRANSLATE_NOOP("QObject", "Block inserts"), "warning", false},
        {"text_height", QT_TRANSLATE_NOOP("QObject", "Text heights"), "note", false},
        {"dim_style", QT_TRANSLATE_NOOP("QObject", "Dimension styles"), "note", false},
        {"layer0", QT_TRANSLATE_NOOP("QObject", "Entities on layer 0"), "note", false},
        {"empty_layer", QT_TRANSLATE_NOOP("QObject", "Empty layers"), "note", false},
        {"layer_name", QT_TRANSLATE_NOOP("QObject", "Layer names"), "note", false},
        {"bylayer", QT_TRANSLATE_NOOP("QObject", "Pens not ByLayer"), "note", true},
    };

    const Rule* ruleInfo(const QString& id) {
        for (const Rule& r : RULES) {
            if (id == QLatin1String(r.id)) {
                return &r;
            }
        }
        return nullptr;
    }

    const QString NO_DRAWING = QStringLiteral("No drawing is open in OpenCAD. Ask the user to open or create one.");
    constexpr double PRECISION = 1e-9;    // of the drawing's size: points closer than this are the same point
    constexpr double GAP = 5e-4;          // of the drawing's size: endpoints closer than this (not touching) should meet
    constexpr double OUTLIER_FACTOR = 10; // an entity farther from the others than 10 x their spread is a stray
    constexpr double TEXT_CLUSTER = 0.005; // text heights within 0.5 % count as the same height
    constexpr double TEXT_NEIGHBOUR = 0.12; // a rare height within 12 % of a common one was probably meant to be it
    constexpr int MAX_IDS = 12;           // ids written out in one message
    constexpr int MAX_FIX_LINES = 40;

    // ------------------------------------------------------------------ small helpers

    QString fmt(const double v) {
        if (!std::isfinite(v)) {
            return QStringLiteral("n/a");
        }
        QString s = QString::number(v, 'f', std::abs(v) < 0.01 && v != 0 ? 8 : 4);
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

    bool live(const RS_Entity* e) {
        return e != nullptr && !e->isDeleted();
    }

    QString ref(const RS_Entity* e) {
        return QStringLiteral("#%1 %2").arg(e->getId()).arg(LC_AITools::typeName(e));
    }

    QString idList(const QList<Id>& ids, const int max = MAX_IDS) {
        QStringList out;
        for (int i = 0; i < ids.size() && i < max; ++i) {
            out << QStringLiteral("#%1").arg(ids[i]);
        }
        if (ids.size() > max) {
            out << QStringLiteral("and %1 more").arg(ids.size() - max);
        }
        return out.join(", ");
    }

    QString layerOf(const RS_Entity* e) {
        const RS_Layer* layer = e->getLayer(true);
        return layer != nullptr ? layer->getName() : QStringLiteral("0");
    }

    /** Entities on a locked layer are left alone (OpenCAD would not delete them either). */
    bool editable(const RS_Entity* e) {
        const RS_Layer* layer = e->getLayer(true);
        return layer == nullptr || !layer->isLocked();
    }

    /** The fix, saying to unlock the layer first when the entity cannot be changed. */
    QString unlockFirst(const QString& fix, const RS_Entity* e) {
        return editable(e) ? fix : QStringLiteral("%1 (unlock layer %2 first)").arg(fix, layerOf(e));
    }

    bool bounds(const RS_Entity* e, RS_Vector& min, RS_Vector& max) {
        min = e->getMin();
        max = e->getMax();
        return min.valid && max.valid && std::isfinite(min.x) && std::isfinite(min.y) && std::isfinite(max.x)
               && std::isfinite(max.y) && max.x >= min.x && max.y >= min.y && max.x - min.x < 1e300;
    }

    RS_Vector centreOf(const RS_Entity* e) {
        RS_Vector min;
        RS_Vector max;
        return bounds(e, min, max) ? (min + max) * 0.5 : RS_Vector(false);
    }

    bool same(const RS_Vector& a, const RS_Vector& b, const double eps) {
        return a.valid && b.valid && a.distanceTo(b) <= eps;
    }

    QString colourName(const QColor& c) {
        static const QList<QPair<QColor, QString>> names = {
            {Qt::red, "red"}, {Qt::yellow, "yellow"}, {Qt::green, "green"}, {Qt::cyan, "cyan"}, {Qt::blue, "blue"},
            {Qt::magenta, "magenta"}, {Qt::white, "white"}, {Qt::black, "black"}, {QColor(128, 128, 128), "grey"},
            {QColor(192, 192, 192), "light grey"}};
        for (const auto& n : names) {
            if (n.first.rgb() == c.rgb()) {
                return n.second;
            }
        }
        return c.name();
    }

    /** What of the entity's own pen is set on it instead of coming from its layer. */
    QStringList explicitPen(const RS_Entity* e) {
        const RS_Pen pen = e->getPen(false);
        QStringList what;
        if (!pen.getColor().isByLayer() && !pen.getColor().isByBlock()) {
            what << QStringLiteral("colour %1").arg(colourName(pen.getColor().toQColor()));
        }
        if (pen.getLineType() != RS2::LineByLayer && pen.getLineType() != RS2::LineByBlock) {
            what << QStringLiteral("line type %1").arg(pen.getLineTypeName());
        }
        if (pen.getWidth() != RS2::WidthByLayer && pen.getWidth() != RS2::WidthByBlock) {
            what << (pen.getWidth() == RS2::WidthDefault ? QStringLiteral("width Default")
                                                          : QStringLiteral("width %1 mm").arg(fmt(int(pen.getWidth()) / 100.0)));
        }
        return what;
    }

    /** Same layer and the same own colour, line type and width: one of the two can go without anything changing. */
    bool sameAttributes(const RS_Entity* a, const RS_Entity* b) {
        if (layerOf(a) != layerOf(b)) {
            return false;
        }
        const RS_Pen pa = a->getPen(false);
        const RS_Pen pb = b->getPen(false);
        const RS_Color ca = pa.getColor();
        const RS_Color cb = pb.getColor();
        return ca.isByLayer() == cb.isByLayer() && ca.isByBlock() == cb.isByBlock()
               && (ca.isByLayer() || ca.isByBlock() || ca.toQColor() == cb.toQColor()) && pa.getLineType() == pb.getLineType()
               && pa.getLineTypeName() == pb.getLineTypeName() && pa.getWidth() == pb.getWidth();
    }

    QString attributeDifference(const RS_Entity* a, const RS_Entity* b) {
        if (layerOf(a) != layerOf(b)) {
            return QStringLiteral("#%1 is on layer %2, #%3 on layer %4").arg(a->getId()).arg(layerOf(a)).arg(b->getId()).arg(layerOf(b));
        }
        return QStringLiteral("they differ in colour, line type or width");
    }

    /** A segment of a polyline as its start vertex and bulge (0 for a line). */
    struct Vertex {
        RS_Vector point;
        double bulge {0};
    };

    /** The vertices of a polyline made of lines and arcs: one per segment start, then the last end. */
    bool vertices(RS_Polyline* polyline, QList<Vertex>& out) {
        out.clear();
        RS_Entity* last = nullptr;
        for (RS_Entity* segment : *polyline) {
            if (segment == nullptr) {
                continue;
            }
            if (segment->rtti() == RS2::EntityLine) {
                out << Vertex{segment->getStartpoint(), 0.0};
            }
            else if (segment->rtti() == RS2::EntityArc) {
                out << Vertex{segment->getStartpoint(), static_cast<RS_Arc*>(segment)->getBulge()};
            }
            else {
                return false; // ellipse segments (from fonts): not rebuilt
            }
            last = segment;
        }
        if (last == nullptr) {
            return false;
        }
        out << Vertex{last->getEndpoint(), 0.0};
        return true;
    }

    /** A new polyline with the old one's layer and pen through these vertices (closed: back to the first). */
    RS_Polyline* rebuiltPolyline(RS_Polyline* source, const QList<Vertex>& points, const bool closed) {
        auto* polyline = new RS_Polyline(source->getParent(), RS_PolylineData(RS_Vector(false), RS_Vector(false), false));
        polyline->setLayer(source->getLayer(false));
        polyline->setPen(source->getPen(false));
        for (const Vertex& v : points) {
            polyline->addVertex(v.point, v.bulge);
        }
        if (closed) {
            polyline->setClosed(true); // the closing segment takes the bulge given with the last vertex
            polyline->endPolyline();
        }
        polyline->calculateBorders();
        return polyline;
    }

    int segmentCount(RS_Polyline* polyline, bool* hasArc = nullptr) {
        int n = 0;
        for (RS_Entity* s : *polyline) {
            if (s != nullptr) {
                ++n;
                if (hasArc != nullptr && s->rtti() == RS2::EntityArc) {
                    *hasArc = true;
                }
            }
        }
        return n;
    }

    /** A counter-clockwise angular interval of an arc: start angle and sweep. */
    void arcInterval(const RS_Arc* a, double& start, double& sweep) {
        start = a->isReversed() ? a->getAngle2() : a->getAngle1();
        sweep = a->getAngleLength();
    }

    /** How far two counter-clockwise intervals overlap (radians). */
    double arcOverlap(const double a, const double la, const double b, const double lb) {
        const double d = RS_Math::correctAngle(b - a);
        const double e = RS_Math::correctAngle(a - b);
        const double o1 = d < la ? std::min(la - d, lb) : 0.0;
        const double o2 = e < lb ? std::min(lb - e, la) : 0.0;
        return std::max(o1, o2);
    }

    /** The interval covering both (they overlap or touch); false when they do not meet. */
    bool arcUnion(const double a, const double la, const double b, const double lb, const double tiny, double& start, double& sweep) {
        const double d = RS_Math::correctAngle(b - a);
        const double e = RS_Math::correctAngle(a - b);
        if (d <= la + tiny) {
            start = a;
            sweep = std::max(la, d + lb);
        }
        else if (e <= lb + tiny) {
            start = b;
            sweep = std::max(lb, e + la);
        }
        else {
            return false;
        }
        sweep = std::min(sweep, 2 * M_PI);
        return true;
    }

    /** Two significant digits, or quarters (2.5, 3.5, 12.5, 0.18): the heights people pick on purpose. */
    bool roundHeight(const double h) {
        if (!(h > 0)) {
            return false;
        }
        const double m = h / std::pow(10.0, std::floor(std::log10(h)));
        const auto near = [m](const double steps) { return std::abs(m * steps - std::round(m * steps)) < 1e-6 * steps * m; };
        return near(10) || near(4);
    }

    // ------------------------------------------------------------------ the check

    struct Context {
        RS_Graphic* graphic {nullptr};
        QList<RS_Entity*> all;    // every entity of the drawing (top level, not deleted)
        QList<RS_Entity*> scoped; // the ones checked
        QSet<QString> rules;
        bool wholeDrawing {true};
        QString layerScope;       // scope "layer": its name
        double eps {PRECISION};   // points closer than this are the same point
        double gap {0};           // endpoints closer than this (and not the same point) should meet
        double size {1};          // diagonal of the main extents
        RS_Vector mainMin {false};
        RS_Vector mainMax {false};
        QSet<Id> outliers;
        QSet<Id> skip;            // reported by a stronger rule (zero length, duplicate): left out of the pair rules
        QList<Issue> issues;
        // where entities are, for "does this point lie on some entity" (built on first use)
        QHash<QPair<qint64, qint64>, QList<RS_Entity*>> grid;
        QList<RS_Entity*> gridBig;
        double gridCell {0};

        bool wants(const char* rule) const { return rules.contains(QLatin1String(rule)); }

        void add(Issue issue) {
            if (issue.severity.isEmpty()) {
                const Rule* r = ruleInfo(issue.rule);
                issue.severity = r != nullptr ? QString::fromLatin1(r->severity) : QStringLiteral("note");
            }
            issues << issue;
        }
    };

    QStringList expandRule(QString name) {
        name = name.trimmed().toLower().replace('-', '_').replace(' ', '_');
        if (ruleInfo(name) != nullptr) {
            return {name};
        }
        static const QHash<QString, QStringList> aliases = {
            {"geometry", {"zero_length", "tiny_segment", "duplicate", "overlap", "endpoint_gap", "nearly_closed"}},
            {"layers", {"layer0", "empty_layer", "layer_name"}},
            {"fixable", {"zero_length", "duplicate", "overlap", "endpoint_gap", "nearly_closed", "bylayer"}},
            {"text", {"text_height"}}, {"texts", {"text_height"}}, {"text_heights", {"text_height"}},
            {"dimensions", {"dim_style"}}, {"dims", {"dim_style"}}, {"dim_styles", {"dim_style"}},
            {"pens", {"bylayer"}}, {"attributes", {"bylayer"}}, {"by_layer", {"bylayer"}},
            {"blocks", {"block_insert"}}, {"inserts", {"block_insert"}}, {"block_inserts", {"block_insert"}},
            {"duplicates", {"duplicate"}}, {"overlaps", {"overlap"}}, {"gaps", {"endpoint_gap"}}, {"gap", {"endpoint_gap"}},
            {"near_miss", {"endpoint_gap"}}, {"zero", {"zero_length"}}, {"tiny", {"tiny_segment"}},
            {"tiny_segments", {"tiny_segment"}}, {"open_polylines", {"nearly_closed"}}, {"layer_0", {"layer0"}},
            {"empty_layers", {"empty_layer"}}, {"layer_names", {"layer_name"}}, {"outliers", {"outlier"}},
        };
        return aliases.value(name);
    }

    QString allRuleIds() {
        QStringList ids;
        for (const Rule& r : RULES) {
            ids << QString::fromLatin1(r.id);
        }
        return ids.join(", ");
    }

    /** The rules asked for (all when none); an error names the unknown ones. */
    QString parseRules(const QStringList& wanted, QSet<QString>& out) {
        out.clear();
        QStringList unknown;
        for (const QString& w : wanted) {
            const QStringList ids = expandRule(w);
            if (ids.isEmpty()) {
                unknown << w;
            }
            for (const QString& id : ids) {
                out.insert(id);
            }
        }
        if (!unknown.isEmpty()) {
            return QStringLiteral("Unknown rule(s): %1. Rules: %2 (or the groups geometry, layers, fixable).")
                .arg(unknown.join(", "), allRuleIds());
        }
        if (wanted.isEmpty()) {
            for (const Rule& r : RULES) {
                out.insert(QString::fromLatin1(r.id));
            }
        }
        return {};
    }

    /**
     * The size the tolerances scale with, from where most entities are: a stray far away (a point at
     * 1e6, 1e6) must neither set the scale nor hide among the rest.
     */
    void computeExtents(Context& c) {
        QList<RS_Entity*> boxed;
        QList<double> xs;
        QList<double> ys;
        QList<double> sizes;
        for (RS_Entity* e : std::as_const(c.all)) {
            RS_Vector min;
            RS_Vector max;
            if (!bounds(e, min, max)) {
                continue;
            }
            boxed << e;
            xs << (min.x + max.x) / 2;
            ys << (min.y + max.y) / 2;
            sizes << std::max(max.x - min.x, max.y - min.y);
        }
        c.outliers.clear();
        if (boxed.size() >= 5) {
            std::sort(xs.begin(), xs.end());
            std::sort(ys.begin(), ys.end());
            std::sort(sizes.begin(), sizes.end());
            const int n = int(boxed.size());
            const int lo = int(std::floor(0.1 * (n - 1)));
            const int hi = int(std::ceil(0.9 * (n - 1)));
            const RS_Vector coreMin(xs[lo], ys[lo]);
            const RS_Vector coreMax(xs[hi], ys[hi]);
            const double spread = std::max({coreMax.x - coreMin.x, coreMax.y - coreMin.y, sizes[n / 2], 1e-9});
            const double reach = OUTLIER_FACTOR * spread;
            for (RS_Entity* e : std::as_const(boxed)) {
                RS_Vector min;
                RS_Vector max;
                bounds(e, min, max);
                const double dx = std::max({0.0, coreMin.x - max.x, min.x - coreMax.x});
                const double dy = std::max({0.0, coreMin.y - max.y, min.y - coreMax.y});
                if (std::hypot(dx, dy) > reach) {
                    c.outliers.insert(e->getId());
                }
            }
        }
        c.mainMin = RS_Vector(false);
        c.mainMax = RS_Vector(false);
        for (RS_Entity* e : std::as_const(boxed)) {
            if (c.outliers.contains(e->getId())) {
                continue;
            }
            RS_Vector min;
            RS_Vector max;
            bounds(e, min, max);
            c.mainMin = c.mainMin.valid ? RS_Vector(std::min(c.mainMin.x, min.x), std::min(c.mainMin.y, min.y)) : min;
            c.mainMax = c.mainMax.valid ? RS_Vector(std::max(c.mainMax.x, max.x), std::max(c.mainMax.y, max.y)) : max;
        }
        c.size = c.mainMin.valid ? c.mainMin.distanceTo(c.mainMax) : 0;
        if (!(c.size > 0)) {
            c.size = 1;
        }
        // far from the origin, coordinates carry fewer decimals: the precision follows them too
        const double magnitude = c.mainMin.valid ? std::max({std::abs(c.mainMin.x), std::abs(c.mainMin.y), std::abs(c.mainMax.x),
                                                             std::abs(c.mainMax.y)})
                                                 : 0.0;
        c.eps = PRECISION * std::max({c.size, magnitude, 1.0});
    }

    /** The entities of the drawing and those the scope picks. */
    void collect(Context& c, const QSet<Id>& only) {
        c.all.clear();
        c.scoped.clear();
        for (RS_Entity* e : *c.graphic) {
            if (live(e)) {
                c.all << e;
            }
        }
        for (RS_Entity* e : std::as_const(c.all)) {
            if (only.isEmpty() || only.contains(e->getId())) {
                c.scoped << e;
            }
        }
    }

    /** Sets up the context from the options: scope, rules, tolerances. Returns an error, or nothing. */
    QString setUp(Context& c, RS_Graphic* graphic, const Options& o, QString& scopeText) {
        c.graphic = graphic;
        collect(c, {});
        const QString scope = o.scope.trimmed().toLower();
        if (scope.isEmpty() || scope == "all" || scope == "drawing") {
            scopeText = QStringLiteral("the whole drawing");
        }
        else if (scope == "selection" || scope == "selected") {
            c.wholeDrawing = false;
            QList<RS_Entity*> chosen;
            for (RS_Entity* e : std::as_const(c.all)) {
                if (e->isSelected()) {
                    chosen << e;
                }
            }
            if (chosen.isEmpty()) {
                return QStringLiteral("Nothing is selected. Select entities first, or check scope \"all\".");
            }
            c.scoped = chosen;
            scopeText = QStringLiteral("the selection");
        }
        else if (scope == "layer") {
            c.wholeDrawing = false;
            RS_Layer* layer = o.layer.isEmpty() ? nullptr : graphic->getLayerList()->find(o.layer);
            if (layer == nullptr) {
                QStringList names;
                for (RS_Layer* l : *graphic->getLayerList()) {
                    names << l->getName();
                }
                return o.layer.isEmpty() ? QStringLiteral("scope \"layer\" needs the layer's name in \"layer\".")
                                         : QStringLiteral("There is no layer \"%1\". Layers: %2.").arg(o.layer, names.join(", "));
            }
            c.layerScope = layer->getName();
            QList<RS_Entity*> chosen;
            for (RS_Entity* e : std::as_const(c.all)) {
                if (layerOf(e) == c.layerScope) {
                    chosen << e;
                }
            }
            c.scoped = chosen;
            scopeText = QStringLiteral("layer %1").arg(c.layerScope);
        }
        else {
            return QStringLiteral("scope must be all, selection or layer.");
        }
        if (const QString error = parseRules(o.rules, c.rules); !error.isEmpty()) {
            return error;
        }
        computeExtents(c);
        if (o.tolerance > 0 && std::isfinite(o.tolerance)) {
            c.gap = o.tolerance;
        }
        else {
            // two significant digits: "gaps under 0.35", not 0.3542
            const double raw = c.size * GAP;
            const double step = std::pow(10.0, std::floor(std::log10(raw)) - 1);
            c.gap = std::round(raw / step) * step;
        }
        c.gap = std::max(c.gap, c.eps * 10);
        return {};
    }

    void issueAt(Context& c, const char* rule, const RS_Entity* e, const RS_Vector& at, const QString& message, const QString& fix,
                 const bool fixable) {
        Issue issue;
        issue.rule = QString::fromLatin1(rule);
        issue.ids = {e->getId()};
        issue.position = at;
        issue.message = message;
        issue.fix = fixable ? unlockFirst(fix, e) : fix;
        issue.fixable = fixable && editable(e);
        c.add(issue);
    }

    // --- zero length, tiny segments

    void checkLengths(Context& c) {
        const bool zero = c.wants("zero_length");
        const bool tiny = c.wants("tiny_segment");
        const QString tinyFix = QStringLiteral("delete it and join its neighbours, or extend them to meet");
        for (RS_Entity* e : std::as_const(c.scoped)) {
            const Id id = e->getId();
            switch (e->rtti()) {
                case RS2::EntityLine: {
                    const double length = e->getStartpoint().distanceTo(e->getEndpoint());
                    if (length <= c.eps) {
                        c.skip.insert(id);
                        if (zero) {
                            issueAt(c, "zero_length", e, e->getStartpoint(),
                                    QStringLiteral("%1 at %2 has zero length").arg(ref(e), fmt(e->getStartpoint())),
                                    QStringLiteral("delete it"), true);
                        }
                    }
                    else if (length < c.gap && tiny) {
                        issueAt(c, "tiny_segment", e, (e->getStartpoint() + e->getEndpoint()) * 0.5,
                                QStringLiteral("%1 %2 -> %3 is only %4 long (under the %5 tolerance): a sliver left from trimming?")
                                    .arg(ref(e), fmt(e->getStartpoint()), fmt(e->getEndpoint()), fmt(length), fmt(c.gap)),
                                tinyFix, false);
                    }
                    break;
                }
                case RS2::EntityArc: {
                    auto* a = static_cast<RS_Arc*>(e);
                    const double length = a->getRadius() * a->getAngleLength();
                    if (a->getRadius() <= c.eps || length <= c.eps) {
                        c.skip.insert(id);
                        if (zero) {
                            issueAt(c, "zero_length", e, a->getCenter(),
                                    QStringLiteral("%1 at %2 has %3").arg(ref(e), fmt(a->getCenter()),
                                                                          a->getRadius() <= c.eps ? QStringLiteral("zero radius")
                                                                                                  : QStringLiteral("no sweep (zero length)")),
                                    QStringLiteral("delete it"), true);
                        }
                    }
                    else if (length < c.gap && tiny) {
                        issueAt(c, "tiny_segment", e, a->getMiddlePoint(),
                                QStringLiteral("%1 at %2 is only %3 long (under the %4 tolerance)")
                                    .arg(ref(e), fmt(a->getMiddlePoint()), fmt(length), fmt(c.gap)),
                                tinyFix, false);
                    }
                    break;
                }
                case RS2::EntityCircle: {
                    auto* circle = static_cast<RS_Circle*>(e);
                    if (circle->getRadius() <= c.eps) {
                        c.skip.insert(id);
                        if (zero) {
                            issueAt(c, "zero_length", e, circle->getCenter(),
                                    QStringLiteral("%1 at %2 has zero radius").arg(ref(e), fmt(circle->getCenter())),
                                    QStringLiteral("delete it"), true);
                        }
                    }
                    break;
                }
                case RS2::EntityEllipse: {
                    auto* ellipse = static_cast<RS_Ellipse*>(e);
                    if (ellipse->getMajorRadius() <= c.eps || ellipse->getMinorRadius() <= c.eps) {
                        c.skip.insert(id);
                        if (zero) {
                            issueAt(c, "zero_length", e, ellipse->getCenter(),
                                    QStringLiteral("%1 at %2 is flat (a zero radius)").arg(ref(e), fmt(ellipse->getCenter())),
                                    QStringLiteral("delete it"), true);
                        }
                    }
                    break;
                }
                case RS2::EntityPolyline: {
                    auto* polyline = static_cast<RS_Polyline*>(e);
                    double total = 0;
                    int zeroSegments = 0;
                    int tinySegments = 0;
                    RS_Vector zeroAt(false);
                    RS_Vector tinyAt(false);
                    double shortest = 0;
                    for (RS_Entity* s : *polyline) {
                        if (s == nullptr) {
                            continue;
                        }
                        const double length = s->getLength();
                        total += length;
                        if (length <= c.eps) {
                            ++zeroSegments;
                            zeroAt = zeroAt.valid ? zeroAt : s->getStartpoint();
                        }
                        else if (length < c.gap) {
                            ++tinySegments;
                            if (!tinyAt.valid || length < shortest) {
                                tinyAt = s->getMiddlePoint().valid ? s->getMiddlePoint() : s->getStartpoint();
                                shortest = length;
                            }
                        }
                    }
                    if (total <= c.eps) {
                        c.skip.insert(id);
                        if (zero) {
                            issueAt(c, "zero_length", e, polyline->getStartpoint(),
                                    QStringLiteral("%1 at %2 has zero length").arg(ref(e), fmt(polyline->getStartpoint())),
                                    QStringLiteral("delete it"), true);
                        }
                        break;
                    }
                    QList<Vertex> points;
                    if (zeroSegments > 0 && zero) {
                        issueAt(c, "zero_length", e, zeroAt,
                                QStringLiteral("%1 has %2 zero-length segment(s) (a vertex repeated), first at %3")
                                    .arg(ref(e)).arg(zeroSegments).arg(fmt(zeroAt)),
                                QStringLiteral("remove the repeated vertices"), vertices(polyline, points));
                    }
                    if (tinySegments > 0 && tiny) {
                        issueAt(c, "tiny_segment", e, tinyAt,
                                QStringLiteral("%1 has %2 segment(s) shorter than %3, the shortest %4 long at %5")
                                    .arg(ref(e)).arg(tinySegments).arg(fmt(c.gap), fmt(shortest), fmt(tinyAt)),
                                QStringLiteral("remove the extra vertices"), false);
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    // --- exact duplicates

    bool sameGeometry(RS_Entity* a, RS_Entity* b, const double eps) {
        if (a->rtti() != b->rtti()) {
            return false;
        }
        switch (a->rtti()) {
            case RS2::EntityLine:
                return (same(a->getStartpoint(), b->getStartpoint(), eps) && same(a->getEndpoint(), b->getEndpoint(), eps))
                       || (same(a->getStartpoint(), b->getEndpoint(), eps) && same(a->getEndpoint(), b->getStartpoint(), eps));
            case RS2::EntityCircle:
                return same(a->getCenter(), b->getCenter(), eps) && std::abs(a->getRadius() - b->getRadius()) <= eps;
            case RS2::EntityArc:
            case RS2::EntityEllipse:
                // three points fix an arc; either direction
                if (!same(a->getCenter(), b->getCenter(), eps) || !same(a->getMiddlePoint(), b->getMiddlePoint(), eps)) {
                    return false;
                }
                if (a->rtti() == RS2::EntityEllipse) {
                    const auto* ea = static_cast<RS_Ellipse*>(a);
                    const auto* eb = static_cast<RS_Ellipse*>(b);
                    if (std::abs(ea->getMajorRadius() - eb->getMajorRadius()) > eps
                        || std::abs(ea->getMinorRadius() - eb->getMinorRadius()) > eps) {
                        return false;
                    }
                }
                else if (std::abs(a->getRadius() - b->getRadius()) > eps) {
                    return false;
                }
                return (same(a->getStartpoint(), b->getStartpoint(), eps) && same(a->getEndpoint(), b->getEndpoint(), eps))
                       || (same(a->getStartpoint(), b->getEndpoint(), eps) && same(a->getEndpoint(), b->getStartpoint(), eps));
            case RS2::EntityPoint:
                return same(static_cast<RS_Point*>(a)->getPos(), static_cast<RS_Point*>(b)->getPos(), eps);
            case RS2::EntityPolyline: {
                auto* pa = static_cast<RS_Polyline*>(a);
                auto* pb = static_cast<RS_Polyline*>(b);
                if (pa->isClosed() != pb->isClosed()) {
                    return false;
                }
                QList<RS_Entity*> sa;
                QList<RS_Entity*> sb;
                for (RS_Entity* s : *pa) {
                    sa << s;
                }
                for (RS_Entity* s : *pb) {
                    sb << s;
                }
                if (sa.size() != sb.size() || sa.isEmpty()) {
                    return false;
                }
                bool forward = true;
                bool backward = true;
                for (int i = 0; i < sa.size() && (forward || backward); ++i) {
                    forward = forward && sameGeometry(sa[i], sb[i], eps);
                    backward = backward && sameGeometry(sa[i], sb[sb.size() - 1 - i], eps);
                }
                return forward || backward;
            }
            case RS2::EntityText: {
                const auto* ta = static_cast<RS_Text*>(a);
                const auto* tb = static_cast<RS_Text*>(b);
                return ta->getText() == tb->getText() && same(ta->getInsertionPoint(), tb->getInsertionPoint(), eps)
                       && std::abs(ta->getHeight() - tb->getHeight()) <= eps && std::abs(ta->getAngle() - tb->getAngle()) < 1e-9;
            }
            case RS2::EntityMText: {
                const auto* ta = static_cast<RS_MText*>(a);
                const auto* tb = static_cast<RS_MText*>(b);
                return ta->getText() == tb->getText() && same(ta->getInsertionPoint(), tb->getInsertionPoint(), eps)
                       && std::abs(ta->getHeight() - tb->getHeight()) <= eps && std::abs(ta->getAngle() - tb->getAngle()) < 1e-9;
            }
            case RS2::EntityInsert: {
                const auto* ia = static_cast<RS_Insert*>(a);
                const auto* ib = static_cast<RS_Insert*>(b);
                return ia->getName() == ib->getName() && same(ia->getInsertionPoint(), ib->getInsertionPoint(), eps)
                       && ia->getScale().distanceTo(ib->getScale()) < 1e-9 && std::abs(ia->getAngle() - ib->getAngle()) < 1e-9
                       && ia->getCols() == ib->getCols() && ia->getRows() == ib->getRows();
            }
            case RS2::EntityHatch:
            case RS2::EntityImage:
                return false;
            default: {
                // splines, dimensions, solids...: the same reference points in the same order
                if (!RS2::isDimensionalEntity(a->rtti()) && a->rtti() != RS2::EntitySpline && a->rtti() != RS2::EntitySplinePoints
                    && a->rtti() != RS2::EntitySolid) {
                    return false;
                }
                const RS_VectorSolutions ra = a->getRefPoints();
                const RS_VectorSolutions rb = b->getRefPoints();
                if (ra.size() != rb.size() || ra.size() == 0) {
                    return false;
                }
                for (size_t i = 0; i < ra.size(); ++i) {
                    if (!same(ra.get(i), rb.get(i), eps)) {
                        return false;
                    }
                }
                if (RS2::isDimensionalEntity(a->rtti())) {
                    const auto* da = dynamic_cast<RS_Dimension*>(a);
                    const auto* db = dynamic_cast<RS_Dimension*>(b);
                    return da != nullptr && db != nullptr && da->getText() == db->getText();
                }
                return true;
            }
        }
    }

    void checkDuplicates(Context& c) {
        struct Item {
            RS_Entity* e;
            RS_Vector min;
            RS_Vector max;
        };
        QList<Item> items;
        for (RS_Entity* e : std::as_const(c.scoped)) {
            Item item{e, {}, {}};
            if (!c.skip.contains(e->getId()) && bounds(e, item.min, item.max)) {
                items << item;
            }
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            return a.e->rtti() != b.e->rtti() ? a.e->rtti() < b.e->rtti() : a.min.x < b.min.x;
        });
        // copies of copies form one group: the oldest (lowest id) stays
        QList<int> parent(items.size());
        std::iota(parent.begin(), parent.end(), 0);
        std::function<int(int)> root = [&](const int i) { return parent[i] == i ? i : parent[i] = root(parent[i]); };
        for (int i = 0; i < items.size(); ++i) {
            for (int j = i + 1; j < items.size() && items[j].e->rtti() == items[i].e->rtti() && items[j].min.x - items[i].min.x <= c.eps;
                 ++j) {
                if (std::abs(items[j].max.x - items[i].max.x) <= c.eps && std::abs(items[j].min.y - items[i].min.y) <= c.eps
                    && std::abs(items[j].max.y - items[i].max.y) <= c.eps && sameGeometry(items[i].e, items[j].e, c.eps)) {
                    parent[root(j)] = root(i);
                }
            }
        }
        QMap<int, QList<RS_Entity*>> groups;
        for (int i = 0; i < items.size(); ++i) {
            groups[root(i)] << items[i].e;
        }
        for (QList<RS_Entity*> group : std::as_const(groups)) {
            if (group.size() < 2) {
                continue;
            }
            std::sort(group.begin(), group.end(), [](RS_Entity* a, RS_Entity* b) { return a->getId() < b->getId(); });
            RS_Entity* keep = group.first();
            for (int i = 1; i < group.size(); ++i) {
                RS_Entity* copy = group[i];
                c.skip.insert(copy->getId());
                if (!c.wants("duplicate")) {
                    continue;
                }
                const bool alike = sameAttributes(copy, keep);
                Issue issue;
                issue.rule = QStringLiteral("duplicate");
                issue.ids = {copy->getId(), keep->getId()};
                issue.position = centreOf(copy);
                issue.message = QStringLiteral("%1 at %2 is an exact copy of #%3%4")
                                    .arg(ref(copy), fmt(centreOf(copy))).arg(keep->getId())
                                    .arg(alike ? QString() : QStringLiteral(" (%1)").arg(attributeDifference(copy, keep)));
                issue.fixable = alike && editable(copy);
                issue.fix = alike ? unlockFirst(QStringLiteral("delete #%1").arg(copy->getId()), copy)
                                  : QStringLiteral("delete the copy on the wrong layer or with the wrong pen (#%1 or #%2)")
                                        .arg(copy->getId()).arg(keep->getId());
                c.add(issue);
            }
        }
    }

    // --- overlapping collinear lines, overlapping arcs

    void checkLineOverlaps(Context& c) {
        struct L {
            RS_Entity* e;
            RS_Vector s;
            RS_Vector t;
            double theta;
        };
        QList<L> lines;
        for (RS_Entity* e : std::as_const(c.scoped)) {
            if (e->rtti() != RS2::EntityLine || c.skip.contains(e->getId())) {
                continue;
            }
            const RS_Vector s = e->getStartpoint();
            const RS_Vector t = e->getEndpoint();
            if (s.distanceTo(t) <= c.eps) {
                continue;
            }
            double theta = std::fmod(std::atan2(t.y - s.y, t.x - s.x) + 2 * M_PI, M_PI); // a line either way round
            if (theta > M_PI - 1e-6) {
                theta -= M_PI; // near-horizontal lines both land near 0
            }
            lines << L{e, s, t, theta};
        }
        std::sort(lines.begin(), lines.end(), [](const L& a, const L& b) { return a.theta < b.theta; });
        const double distTol = c.eps * 10;
        QSet<QPair<Id, Id>> reported;
        int start = 0;
        while (start < lines.size()) {
            // lines with (nearly) the same direction
            int end = start + 1;
            while (end < lines.size() && lines[end].theta - lines[end - 1].theta <= 1e-6) {
                ++end;
            }
            if (end - start >= 2) {
                const double theta = lines[start].theta;
                const RS_Vector u(std::cos(theta), std::sin(theta));
                const RS_Vector n(-u.y, u.x);
                QList<L> parallel = lines.mid(start, end - start);
                std::sort(parallel.begin(), parallel.end(), [&n](const L& a, const L& b) { return n.dotP(a.s) < n.dotP(b.s); });
                int from = 0;
                while (from < parallel.size()) {
                    // on the same infinite line
                    int to = from + 1;
                    while (to < parallel.size() && n.dotP(parallel[to].s) - n.dotP(parallel[to - 1].s) <= distTol) {
                        ++to;
                    }
                    for (int i = from; i < to; ++i) {
                        for (int j = i + 1; j < to; ++j) {
                            const L& a = parallel[i];
                            const L& b = parallel[j];
                            // exactly collinear: both ends of each on the other's line
                            const RS_Vector da = (a.t - a.s).normalized();
                            const RS_Vector na(-da.y, da.x);
                            if (std::abs(na.dotP(b.s - a.s)) > distTol || std::abs(na.dotP(b.t - a.s)) > distTol) {
                                continue;
                            }
                            const double a0 = std::min(da.dotP(a.s - a.s), da.dotP(a.t - a.s));
                            const double a1 = std::max(da.dotP(a.s - a.s), da.dotP(a.t - a.s));
                            const double b0 = std::min(da.dotP(b.s - a.s), da.dotP(b.t - a.s));
                            const double b1 = std::max(da.dotP(b.s - a.s), da.dotP(b.t - a.s));
                            const double overlap = std::min(a1, b1) - std::max(a0, b0);
                            if (overlap <= c.eps) {
                                continue; // apart, or end to end
                            }
                            RS_Entity* keep = a.e->getId() < b.e->getId() ? a.e : b.e;
                            RS_Entity* other = keep == a.e ? b.e : a.e;
                            const QPair<Id, Id> key{keep->getId(), other->getId()};
                            if (reported.contains(key)) {
                                continue;
                            }
                            reported.insert(key);
                            const double lo = std::min(a0, b0);
                            const double hi = std::max(a1, b1);
                            const RS_Vector from0 = a.s + da * std::max(a0, b0);
                            const RS_Vector to0 = a.s + da * std::min(a1, b1);
                            const bool alike = sameAttributes(keep, other);
                            const bool otherInside = (keep == a.e ? b0 >= a0 - c.eps && b1 <= a1 + c.eps : a0 >= b0 - c.eps && a1 <= b1 + c.eps);
                            Issue issue;
                            issue.rule = QStringLiteral("overlap");
                            issue.ids = {keep->getId(), other->getId()};
                            issue.position = (from0 + to0) * 0.5;
                            issue.message = otherInside
                                                ? QStringLiteral("%1 lies entirely on #%2 line, from %3 to %4").arg(ref(other)).arg(keep->getId())
                                                      .arg(fmt(from0), fmt(to0))
                                                : QStringLiteral("%1 and #%2 line overlap for %3 along the same line, from %4 to %5")
                                                      .arg(ref(keep)).arg(other->getId()).arg(fmt(overlap), fmt(from0), fmt(to0));
                            if (!alike) {
                                issue.message += QStringLiteral(" (%1)").arg(attributeDifference(keep, other));
                            }
                            issue.fix = otherInside ? QStringLiteral("delete #%1").arg(other->getId())
                                                    : QStringLiteral("merge them into one line from %1 to %2")
                                                          .arg(fmt(a.s + da * lo), fmt(a.s + da * hi));
                            issue.fixable = alike && editable(keep) && editable(other);
                            c.add(issue);
                        }
                    }
                    from = to;
                }
            }
            start = end;
        }
    }

    void checkArcOverlaps(Context& c) {
        struct A {
            RS_Entity* e;
            RS_Vector centre;
            double radius;
        };
        QList<A> arcs;
        for (RS_Entity* e : std::as_const(c.scoped)) {
            if ((e->rtti() == RS2::EntityArc || e->rtti() == RS2::EntityCircle) && !c.skip.contains(e->getId()) && e->getRadius() > c.eps) {
                arcs << A{e, e->getCenter(), e->getRadius()};
            }
        }
        std::sort(arcs.begin(), arcs.end(), [](const A& a, const A& b) { return a.centre.x < b.centre.x; });
        const double distTol = c.eps * 10;
        for (int i = 0; i < arcs.size(); ++i) {
            for (int j = i + 1; j < arcs.size() && arcs[j].centre.x - arcs[i].centre.x <= distTol; ++j) {
                const A& a = arcs[i];
                const A& b = arcs[j];
                if (std::abs(a.centre.y - b.centre.y) > distTol || std::abs(a.radius - b.radius) > distTol) {
                    continue;
                }
                const bool aCircle = a.e->rtti() == RS2::EntityCircle;
                const bool bCircle = b.e->rtti() == RS2::EntityCircle;
                if (aCircle && bCircle) {
                    continue; // the same circle twice is a duplicate
                }
                Issue issue;
                issue.rule = QStringLiteral("overlap");
                issue.position = a.e->getMiddlePoint().valid && !aCircle ? a.e->getMiddlePoint() : b.e->getMiddlePoint();
                if (aCircle || bCircle) {
                    RS_Entity* circle = aCircle ? a.e : b.e;
                    RS_Entity* arc = aCircle ? b.e : a.e;
                    issue.ids = {circle->getId(), arc->getId()};
                    issue.position = arc->getMiddlePoint();
                    issue.message = QStringLiteral("%1 lies on #%2 circle (centre %3, radius %4)")
                                        .arg(ref(arc)).arg(circle->getId()).arg(fmt(circle->getCenter()), fmt(circle->getRadius()));
                    issue.fix = QStringLiteral("delete #%1").arg(arc->getId());
                    issue.fixable = sameAttributes(circle, arc) && editable(arc);
                }
                else {
                    double sa = 0;
                    double la = 0;
                    double sb = 0;
                    double lb = 0;
                    arcInterval(static_cast<RS_Arc*>(a.e), sa, la);
                    arcInterval(static_cast<RS_Arc*>(b.e), sb, lb);
                    const double overlap = arcOverlap(sa, la, sb, lb);
                    if (overlap * a.radius <= c.eps) {
                        continue;
                    }
                    RS_Entity* keep = a.e->getId() < b.e->getId() ? a.e : b.e;
                    RS_Entity* other = keep == a.e ? b.e : a.e;
                    issue.ids = {keep->getId(), other->getId()};
                    issue.message = QStringLiteral("%1 and #%2 arc overlap for %3 on the same circle (centre %4, radius %5)")
                                        .arg(ref(keep)).arg(other->getId()).arg(fmt(overlap * a.radius), fmt(a.centre), fmt(a.radius));
                    issue.fix = QStringLiteral("merge them into one arc");
                    issue.fixable = sameAttributes(keep, other) && editable(keep) && editable(other);
                }
                if (!sameAttributes(a.e, b.e)) {
                    issue.message += QStringLiteral(" (%1)").arg(attributeDifference(a.e, b.e));
                }
                c.add(issue);
            }
        }
    }

    // --- endpoints that nearly meet

    /** The entities whose boxes are near this point (within a grid cell around it), built on first use. */
    QList<RS_Entity*> entitiesNear(Context& c, const RS_Vector& p) {
        if (c.gridCell <= 0) {
            c.gridCell = std::max(c.size / 64.0, c.gap * 4);
            for (RS_Entity* e : std::as_const(c.all)) {
                RS_Vector min;
                RS_Vector max;
                if (!bounds(e, min, max)) {
                    continue;
                }
                const qint64 x0 = qint64(std::floor(min.x / c.gridCell));
                const qint64 x1 = qint64(std::floor(max.x / c.gridCell));
                const qint64 y0 = qint64(std::floor(min.y / c.gridCell));
                const qint64 y1 = qint64(std::floor(max.y / c.gridCell));
                if ((x1 - x0 + 1) * (y1 - y0 + 1) > 64) {
                    c.gridBig << e;
                    continue;
                }
                for (qint64 x = x0; x <= x1; ++x) {
                    for (qint64 y = y0; y <= y1; ++y) {
                        c.grid[{x, y}] << e;
                    }
                }
            }
        }
        // the cell and its neighbours: anything within a gap of the point (a cell is at least 4 gaps wide)
        const qint64 cx = qint64(std::floor(p.x / c.gridCell));
        const qint64 cy = qint64(std::floor(p.y / c.gridCell));
        QList<RS_Entity*> near = c.gridBig;
        QSet<RS_Entity*> seen(c.gridBig.begin(), c.gridBig.end());
        for (qint64 dx = -1; dx <= 1; ++dx) {
            for (qint64 dy = -1; dy <= 1; ++dy) {
                for (RS_Entity* e : c.grid.value({cx + dx, cy + dy})) {
                    if (!seen.contains(e)) {
                        seen.insert(e);
                        near << e;
                    }
                }
            }
        }
        return near;
    }

    /** A point lies on some other entity (a T-junction): it is connected, though not to an endpoint. */
    bool liesOnOther(Context& c, const RS_Vector& p, const RS_Entity* self) {
        for (RS_Entity* e : entitiesNear(c, p)) {
            if (e != self && e->rtti() != RS2::EntityText && e->rtti() != RS2::EntityMText && e->getDistanceToPoint(p) <= c.eps) {
                return true;
            }
        }
        return false;
    }

    /** Where an end can move without changing the shape of its entity: along its line, or around its arc's circle. */
    struct Path {
        enum Kind { None, Straight, Round } kind {None};
        RS_Vector a;   // Straight: a point of the line; Round: the centre
        RS_Vector dir; // Straight: the unit direction
        double radius {0};
    };

    Path pathOf(const RS_Entity* segment) {
        Path p;
        if (segment == nullptr) {
            return p;
        }
        if (segment->rtti() == RS2::EntityLine) {
            const RS_Vector d = segment->getEndpoint() - segment->getStartpoint();
            if (d.magnitude() > 0) {
                p.kind = Path::Straight;
                p.a = segment->getStartpoint();
                p.dir = d / d.magnitude();
            }
        }
        else if (segment->rtti() == RS2::EntityArc || segment->rtti() == RS2::EntityCircle) {
            p.kind = Path::Round;
            p.a = segment->getCenter();
            p.radius = segment->getRadius();
        }
        return p;
    }

    /** The path of one end of a line, an arc or a polyline (whose arc ends have none: moving them changes the bulge). */
    Path pathAt(RS_Entity* e, const bool start) {
        if (e->rtti() != RS2::EntityPolyline) {
            return e->rtti() == RS2::EntityCircle ? Path{} : pathOf(e);
        }
        RS_Entity* first = nullptr;
        RS_Entity* last = nullptr;
        for (RS_Entity* s : *static_cast<RS_Polyline*>(e)) {
            if (s != nullptr) {
                first = first != nullptr ? first : s;
                last = s;
            }
        }
        RS_Entity* segment = start ? first : last;
        return segment != nullptr && segment->rtti() == RS2::EntityLine ? pathOf(segment) : Path{};
    }

    /** An arc end: moving it anywhere but around its circle would change its radius or centre. */
    bool roundEnd(RS_Entity* e, const bool start) {
        return e->rtti() == RS2::EntityArc || (e->rtti() == RS2::EntityPolyline && pathAt(e, start).kind == Path::None);
    }

    /** Where two paths cross: none, one or two points (a line touching a circle within eps counts). */
    QList<RS_Vector> crossings(const Path& p, const Path& q, const double eps) {
        if (p.kind == Path::None || q.kind == Path::None) {
            return {};
        }
        if (p.kind == Path::Round && q.kind == Path::Straight) {
            return crossings(q, p, eps);
        }
        if (p.kind == Path::Straight && q.kind == Path::Straight) {
            const double sine = p.dir.x * q.dir.y - p.dir.y * q.dir.x;
            if (std::abs(sine) < 1e-9) {
                return {}; // parallel
            }
            const RS_Vector w = q.a - p.a;
            return {p.a + p.dir * ((w.x * q.dir.y - w.y * q.dir.x) / sine)};
        }
        if (p.kind == Path::Straight) { // a line and a circle
            const RS_Vector foot = p.a + p.dir * (q.a - p.a).dotP(p.dir);
            const double d = foot.distanceTo(q.a);
            if (d > q.radius + eps) {
                return {};
            }
            const double h = std::sqrt(std::max(0.0, q.radius * q.radius - d * d));
            return {foot + p.dir * h, foot - p.dir * h};
        }
        // two circles
        const double d = p.a.distanceTo(q.a);
        if (d <= eps || d > p.radius + q.radius + eps || d < std::abs(p.radius - q.radius) - eps) {
            return {};
        }
        const double x = (d * d + p.radius * p.radius - q.radius * q.radius) / (2 * d);
        const double h = std::sqrt(std::max(0.0, p.radius * p.radius - x * x));
        const RS_Vector u = (q.a - p.a) / d;
        const RS_Vector m = p.a + u * x;
        const RS_Vector n(-u.y, u.x);
        return {m + n * h, m - n * h};
    }

    QString endName(const bool start) {
        return start ? QStringLiteral("start") : QStringLiteral("end");
    }

    void checkGaps(Context& c) {
        struct End {
            RS_Entity* e;
            bool start;
            RS_Vector p;
        };
        QList<End> ends;
        for (RS_Entity* e : std::as_const(c.scoped)) {
            if (c.skip.contains(e->getId())) {
                continue;
            }
            const RS2::EntityType t = e->rtti();
            if (t == RS2::EntityLine || t == RS2::EntityArc || (t == RS2::EntityPolyline && !static_cast<RS_Polyline*>(e)->isClosed())) {
                ends << End{e, true, e->getStartpoint()} << End{e, false, e->getEndpoint()};
            }
        }
        const double cell = c.gap;
        QHash<QPair<qint64, qint64>, QList<int>> grid;
        const auto key = [cell](const RS_Vector& p) { return QPair<qint64, qint64>{qint64(std::floor(p.x / cell)), qint64(std::floor(p.y / cell))}; };
        for (int i = 0; i < ends.size(); ++i) {
            grid[key(ends[i].p)] << i;
        }
        QList<bool> connected(ends.size(), false);
        QList<int> nearest(ends.size(), -1);
        for (int i = 0; i < ends.size(); ++i) {
            const auto k = key(ends[i].p);
            double best = std::numeric_limits<double>::max();
            for (qint64 dx = -1; dx <= 1; ++dx) {
                for (qint64 dy = -1; dy <= 1; ++dy) {
                    for (const int j : grid.value({k.first + dx, k.second + dy})) {
                        if (ends[j].e == ends[i].e) {
                            continue;
                        }
                        const double d = ends[i].p.distanceTo(ends[j].p);
                        if (d <= c.eps) {
                            connected[i] = true;
                        }
                        else if (d <= c.gap && d < best) {
                            best = d;
                            nearest[i] = j;
                        }
                    }
                }
            }
        }
        // an end is attached when another end or another entity (a T-junction) is on it: it never moves
        QHash<int, bool> onOther;
        const auto attached = [&](const int k) {
            if (connected[k]) {
                return true;
            }
            if (!onOther.contains(k)) {
                onOther.insert(k, liesOnOther(c, ends[k].p, ends[k].e));
            }
            return onOther.value(k);
        };
        const auto rebuildable = [](RS_Entity* e) {
            QList<Vertex> points;
            return e->rtti() != RS2::EntityPolyline || vertices(static_cast<RS_Polyline*>(e), points);
        };
        QSet<QPair<int, int>> reported;
        QList<int> loose; // free ends with no other end close by: they may stop short of a line instead
        for (int i = 0; i < ends.size(); ++i) {
            const int j = nearest[i];
            if (j < 0) {
                if (!attached(i)) {
                    loose << i;
                }
                continue;
            }
            if (attached(i)) {
                continue;
            }
            const QPair<int, int> pair{std::min(i, j), std::max(i, j)};
            if (reported.contains(pair)) {
                continue;
            }
            reported.insert(pair);
            // the loose end moves onto the attached one; of two loose ends, a line's rather than an arc's (that would
            // change its radius), else the newer entity's
            const bool bothFree = !attached(j);
            int mover = i;
            int target = j;
            if (bothFree) {
                const bool iRound = roundEnd(ends[i].e, ends[i].start);
                const bool jRound = roundEnd(ends[j].e, ends[j].start);
                if (iRound != jRound ? iRound : ends[j].e->getId() > ends[i].e->getId()) {
                    mover = j;
                    target = i;
                }
            }
            const End& m = ends[mover];
            const End& t = ends[target];
            // Both free: they meet where their lines or arcs cross, if that is close by, so each keeps its direction or
            // its circle (two orthogonal lines stay orthogonal). Otherwise the loose end comes onto the other end.
            RS_Vector meet(false);
            if (bothFree) {
                double best = c.gap;
                for (const RS_Vector& p : crossings(pathAt(m.e, m.start), pathAt(t.e, t.start), c.eps)) {
                    const double far = std::max(p.distanceTo(m.p), p.distanceTo(t.p));
                    if (far <= best) {
                        best = far;
                        meet = p;
                    }
                }
            }
            const bool corner = meet.valid && meet.distanceTo(t.p) > c.eps;
            if (!meet.valid) {
                meet = t.p;
            }
            // an arc end only moves around its circle: one that would have to leave it is left to the user
            const Path mp = pathAt(m.e, m.start);
            const bool keepsShape = !roundEnd(m.e, m.start)
                                    || (mp.kind == Path::Round && std::abs(meet.distanceTo(mp.a) - mp.radius) <= c.eps);
            Issue issue;
            issue.rule = QStringLiteral("endpoint_gap");
            issue.ids = {m.e->getId(), t.e->getId()};
            issue.position = (m.p + t.p) * 0.5;
            issue.moveFrom = m.p;
            issue.moveTo = meet;
            issue.alsoFrom = corner ? t.p : RS_Vector(false);
            issue.message = QStringLiteral("%1 %2 %3 is %4 from #%5 %6 %7 %8 but does not meet it")
                                .arg(ref(m.e), endName(m.start), fmt(m.p), fmt(m.p.distanceTo(t.p)))
                                .arg(t.e->getId())
                                .arg(LC_AITools::typeName(t.e), endName(t.start), fmt(t.p));
            if (corner) {
                issue.fix = QStringLiteral("move #%1's %2 and #%3's %4 to where they meet, %5")
                                .arg(m.e->getId()).arg(endName(m.start)).arg(t.e->getId()).arg(endName(t.start), fmt(meet));
            }
            else if (keepsShape) {
                issue.fix = QStringLiteral("move #%1's %2 onto %3").arg(m.e->getId()).arg(endName(m.start), fmt(meet));
            }
            else {
                issue.fix = QStringLiteral("join them by hand (moving the arc's %1 onto %2 would change its radius)")
                                .arg(endName(m.start), fmt(meet));
            }
            issue.fixable = keepsShape && rebuildable(m.e) && editable(m.e) && (!corner || (rebuildable(t.e) && editable(t.e)));
            c.add(issue);
        }
        // a free end that stops just short of (or runs just past) the middle of a line or arc: it moves along its own
        // line or arc onto it
        for (const int i : std::as_const(loose)) {
            const End& m = ends[i];
            const Path mp = pathAt(m.e, m.start);
            if (mp.kind == Path::None) {
                continue;
            }
            RS_Entity* best = nullptr;
            RS_Vector meet(false);
            double bestDistance = c.gap;
            for (RS_Entity* e : entitiesNear(c, m.p)) {
                const RS2::EntityType type = e->rtti();
                if (e == m.e || c.skip.contains(e->getId())
                    || (type != RS2::EntityLine && type != RS2::EntityArc && type != RS2::EntityCircle && type != RS2::EntityPolyline)
                    || e->getDistanceToPoint(m.p) > c.gap) {
                    continue;
                }
                const RS_Entity* segment = type == RS2::EntityPolyline
                                               ? static_cast<RS_Polyline*>(e)->getNearestEntity(m.p, nullptr, RS2::ResolveNone)
                                               : e;
                for (const RS_Vector& p : crossings(mp, pathOf(segment), c.eps)) {
                    const double d = p.distanceTo(m.p);
                    if (d > c.eps && d <= bestDistance && segment->getDistanceToPoint(p) <= c.eps * 10) {
                        bestDistance = d;
                        best = e;
                        meet = p;
                    }
                }
            }
            if (best == nullptr) {
                continue;
            }
            Issue issue;
            issue.rule = QStringLiteral("endpoint_gap");
            issue.ids = {m.e->getId(), best->getId()};
            issue.position = (m.p + meet) * 0.5;
            issue.moveFrom = m.p;
            issue.moveTo = meet;
            issue.message = QStringLiteral("%1 %2 %3 is %4 from #%5 %6 but does not end on it")
                                .arg(ref(m.e), endName(m.start), fmt(m.p), fmt(bestDistance))
                                .arg(best->getId())
                                .arg(LC_AITools::typeName(best));
            issue.fix = QStringLiteral("move #%1's %2 onto #%3 at %4").arg(m.e->getId()).arg(endName(m.start)).arg(best->getId()).arg(fmt(meet));
            issue.fixable = rebuildable(m.e) && editable(m.e);
            c.add(issue);
        }
    }

    // --- polylines left open

    void checkNearlyClosed(Context& c) {
        for (RS_Entity* e : std::as_const(c.scoped)) {
            if (e->rtti() != RS2::EntityPolyline || c.skip.contains(e->getId())) {
                continue;
            }
            auto* polyline = static_cast<RS_Polyline*>(e);
            if (polyline->isClosed()) {
                continue;
            }
            bool hasArc = false;
            const int segments = segmentCount(polyline, &hasArc);
            if (segments < (hasArc ? 2 : 3)) {
                continue; // nothing to enclose
            }
            const double d = polyline->getStartpoint().distanceTo(polyline->getEndpoint());
            if (d > c.gap) {
                continue;
            }
            QList<Vertex> points;
            Issue issue;
            issue.rule = QStringLiteral("nearly_closed");
            issue.ids = {e->getId()};
            issue.position = polyline->getStartpoint();
            issue.moveFrom = polyline->getEndpoint();
            issue.moveTo = polyline->getStartpoint();
            issue.message = d <= c.eps ? QStringLiteral("%1 ends where it starts, at %2, but is not marked closed")
                                             .arg(ref(e), fmt(polyline->getStartpoint()))
                                       : QStringLiteral("%1 is open with a gap of %2 between its end %3 and its start %4")
                                             .arg(ref(e), fmt(d), fmt(polyline->getEndpoint()), fmt(polyline->getStartpoint()));
            issue.fix = d <= c.eps ? QStringLiteral("mark it closed") : QStringLiteral("close it (its last vertex moves onto the start)");
            issue.fixable = vertices(polyline, points) && editable(e);
            c.add(issue);
        }
    }

    // --- pens

    void checkByLayer(Context& c) {
        for (RS_Entity* e : std::as_const(c.scoped)) {
            const QStringList what = explicitPen(e);
            if (what.isEmpty()) {
                continue;
            }
            issueAt(c, "bylayer", e, centreOf(e),
                    QStringLiteral("%1 on layer %2 has its own %3").arg(ref(e), layerOf(e), what.join(", ")),
                    QStringLiteral("set it ByLayer (or put it on a layer that has that look)"), true);
        }
    }

    // --- text heights

    void checkTextHeights(Context& c) {
        struct T {
            RS_Entity* e;
            double h;
            QString text;
        };
        QList<T> texts;
        for (RS_Entity* e : std::as_const(c.scoped)) {
            if (e->rtti() == RS2::EntityText) {
                texts << T{e, static_cast<RS_Text*>(e)->getHeight(), static_cast<RS_Text*>(e)->getText()};
            }
            else if (e->rtti() == RS2::EntityMText) {
                texts << T{e, static_cast<RS_MText*>(e)->getHeight(), static_cast<RS_MText*>(e)->getText()};
            }
        }
        texts.erase(std::remove_if(texts.begin(), texts.end(), [](const T& t) { return !(t.h > 0) || !std::isfinite(t.h); }), texts.end());
        if (texts.size() < 2) {
            return;
        }
        std::sort(texts.begin(), texts.end(), [](const T& a, const T& b) { return a.h < b.h; });
        struct Cluster {
            double h {0};
            QList<T> members;
        };
        QList<Cluster> clusters;
        for (const T& t : std::as_const(texts)) {
            if (clusters.isEmpty() || t.h > clusters.last().members.last().h * (1 + TEXT_CLUSTER) + c.eps) {
                clusters << Cluster{};
            }
            clusters.last().members << t;
        }
        for (Cluster& cl : clusters) {
            // the height most of them have exactly
            QMap<double, int> counts;
            for (const T& t : std::as_const(cl.members)) {
                counts[t.h] += 1;
            }
            int best = 0;
            for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
                if (it.value() > best) {
                    best = it.value();
                    cl.h = it.key();
                }
            }
        }
        const int total = int(texts.size());
        const int rare = std::max(2, total / 10);
        for (const Cluster& cl : std::as_const(clusters)) {
            if (cl.members.size() > rare) {
                continue;
            }
            const Cluster* meant = nullptr;
            for (const Cluster& big : std::as_const(clusters)) {
                if (&big != &cl && big.members.size() >= 3 && big.members.size() >= 3 * cl.members.size()
                    && std::abs(big.h - cl.h) / big.h <= TEXT_NEIGHBOUR && (meant == nullptr || big.members.size() > meant->members.size())) {
                    meant = &big;
                }
            }
            if (meant == nullptr && roundHeight(cl.h)) {
                continue;
            }
            const Cluster* common = nullptr;
            for (const Cluster& big : std::as_const(clusters)) {
                if (common == nullptr || big.members.size() > common->members.size()) {
                    common = &big;
                }
            }
            for (const T& t : cl.members) {
                QString text = t.text.left(30).replace('\n', " / ");
                Issue issue;
                issue.rule = QStringLiteral("text_height");
                issue.ids = {t.e->getId()};
                issue.position = t.e->rtti() == RS2::EntityText ? static_cast<RS_Text*>(t.e)->getInsertionPoint()
                                                                : static_cast<RS_MText*>(t.e)->getInsertionPoint();
                if (meant != nullptr) {
                    issue.message = QStringLiteral("%1 \"%2\" is %3 high, next to %4 texts at %5: probably meant %5")
                                        .arg(ref(t.e), text, fmt(t.h)).arg(meant->members.size()).arg(fmt(meant->h));
                    issue.fix = QStringLiteral("set its height to %1").arg(fmt(meant->h));
                }
                else {
                    issue.message = QStringLiteral("%1 \"%2\" has an odd height %3 (most texts are %4)")
                                        .arg(ref(t.e), text, fmt(t.h), fmt(common->h));
                    issue.fix = QStringLiteral("use a standard height, e.g. %1").arg(fmt(common->h));
                }
                c.add(issue);
            }
        }
        if (clusters.size() > 4) {
            QStringList heights;
            QList<Id> ids;
            for (const Cluster& cl : std::as_const(clusters)) {
                heights << QStringLiteral("%1 (%2)").arg(fmt(cl.h)).arg(cl.members.size());
                if (cl.members.size() <= rare) {
                    for (const T& t : cl.members) {
                        ids << t.e->getId();
                    }
                }
            }
            Issue issue;
            issue.rule = QStringLiteral("text_height");
            issue.ids = ids;
            issue.message = QStringLiteral("%1 different text heights: %2").arg(clusters.size()).arg(heights.join(", "));
            issue.fix = QStringLiteral("settle on a few standard heights (e.g. 2.5, 3.5, 5, 7 mm on paper)");
            c.add(issue);
        }
    }

    // --- dimension styles

    void checkDimensions(Context& c) {
        QList<RS_Dimension*> dims;
        for (RS_Entity* e : std::as_const(c.scoped)) {
            if (auto* d = dynamic_cast<RS_Dimension*>(e); d != nullptr && e->rtti() != RS2::EntityDimLeader) {
                dims << d;
            }
        }
        if (dims.size() < 2) {
            return;
        }
        const auto styleOf = [](const RS_Dimension* d) { return d->getStyle().isEmpty() ? QStringLiteral("Standard") : d->getStyle(); };
        QMap<QString, QList<RS_Dimension*>> byStyle;
        for (RS_Dimension* d : std::as_const(dims)) {
            byStyle[styleOf(d)] << d;
        }
        QString main;
        for (auto it = byStyle.cbegin(); it != byStyle.cend(); ++it) {
            if (main.isEmpty() || it.value().size() > byStyle.value(main).size()) {
                main = it.key();
            }
        }
        for (auto it = byStyle.cbegin(); it != byStyle.cend(); ++it) {
            if (it.key() == main) {
                continue;
            }
            Issue issue;
            issue.rule = QStringLiteral("dim_style");
            for (const RS_Dimension* d : it.value()) {
                issue.ids << d->getId();
            }
            issue.position = centreOf(it.value().first());
            issue.message = QStringLiteral("%1 dimension(s) (%2) use style \"%3\" while %4 use \"%5\"")
                                .arg(it.value().size()).arg(idList(issue.ids), it.key()).arg(byStyle.value(main).size()).arg(main);
            issue.fix = QStringLiteral("switch them to \"%1\" (or the others to \"%2\" if that is the right one)").arg(main, it.key());
            c.add(issue);
        }
        // within a style, the size of text and arrows should agree: a difference is an override that drifted
        for (auto it = byStyle.cbegin(); it != byStyle.cend(); ++it) {
            if (it.value().size() < 2) {
                continue;
            }
            const auto look = [](RS_Dimension* d) {
                // its style with its overrides (the dimension's own getters only work while it is being drawn)
                LC_DimStyle* style = d->getEffectiveDimStyle();
                if (style == nullptr) {
                    return QString();
                }
                const QString text = QStringLiteral("text height %1, arrow size %2, scale %3")
                                         .arg(fmt(style->text()->height()), fmt(style->arrowhead()->size()), fmt(style->scaling()->scale()));
                if (d->getDimStyleOverride() != nullptr) {
                    delete style; // a merged copy made for us
                }
                return text;
            };
            QMap<QString, int> counts;
            for (RS_Dimension* d : it.value()) {
                counts[look(d)] += 1;
            }
            QString usual;
            for (auto k = counts.cbegin(); k != counts.cend(); ++k) {
                if (usual.isEmpty() || k.value() > counts.value(usual)) {
                    usual = k.key();
                }
            }
            for (RS_Dimension* d : it.value()) {
                if (look(d) == usual) {
                    continue;
                }
                Issue issue;
                issue.rule = QStringLiteral("dim_style");
                issue.ids = {d->getId()};
                issue.position = centreOf(d);
                issue.message = QStringLiteral("%1 has %2 while the other \"%3\" dimensions have %4%5")
                                    .arg(ref(d), look(d), it.key(), usual,
                                         d->getDimStyleOverride() != nullptr ? QStringLiteral(" (it overrides its style)") : QString());
                issue.fix = QStringLiteral("remove its style overrides so it matches the others");
                c.add(issue);
            }
        }
    }

    // --- layers

    QString layerKey(const QString& name) {
        QString k = name.toLower();
        k.remove(QRegularExpression(QStringLiteral("[\\s_\\-.]")));
        if (k.size() > 3 && k.endsWith('s')) {
            k.chop(1); // "Wall" and "Walls"
        }
        return k;
    }

    void checkLayers(Context& c) {
        RS_LayerList* list = c.graphic->getLayerList();
        if (c.wants("layer0") && list->count() > 1) {
            QList<Id> ids;
            RS_Vector min(false);
            RS_Vector max(false);
            for (RS_Entity* e : std::as_const(c.scoped)) {
                if (layerOf(e) != "0") {
                    continue;
                }
                ids << e->getId();
                RS_Vector a;
                RS_Vector b;
                if (!c.outliers.contains(e->getId()) && bounds(e, a, b)) { // a stray would put the spot far away
                    min = min.valid ? RS_Vector(std::min(min.x, a.x), std::min(min.y, a.y)) : a;
                    max = max.valid ? RS_Vector(std::max(max.x, b.x), std::max(max.y, b.y)) : b;
                }
            }
            if (!ids.isEmpty()) {
                QStringList others;
                for (RS_Layer* l : *list) {
                    if (l->getName() != "0") {
                        others << l->getName();
                    }
                }
                Issue issue;
                issue.rule = QStringLiteral("layer0");
                issue.ids = ids;
                issue.position = min.valid ? (min + max) * 0.5 : RS_Vector(false);
                issue.message = QStringLiteral("%1 entities (%2) are on layer 0 although the drawing has other layers (%3)")
                                    .arg(ids.size()).arg(idList(ids), others.join(", "));
                issue.fix = QStringLiteral("move them to the layers they belong on; keep layer 0 for block contents");
                c.add(issue);
            }
        }
        if (!c.wholeDrawing && c.layerScope.isEmpty()) {
            return; // a selection says nothing about the layer list
        }
        QList<RS_Layer*> layers;
        for (RS_Layer* l : *list) {
            if (c.wholeDrawing || l->getName() == c.layerScope) {
                layers << l;
            }
        }
        if (c.wants("empty_layer")) {
            QHash<QString, int> used;
            for (RS_Entity* e : std::as_const(c.all)) {
                used[layerOf(e)] += 1;
            }
            if (RS_BlockList* blocks = c.graphic->getBlockList()) {
                for (RS_Block* block : *blocks) {
                    for (RS_Entity* e : *block) {
                        if (live(e) && e->getLayer(false) != nullptr) {
                            used[e->getLayer(false)->getName()] += 1;
                        }
                    }
                }
            }
            for (RS_Layer* l : std::as_const(layers)) {
                if (l->getName() == "0" || l == list->getActive() || used.value(l->getName()) > 0) {
                    continue;
                }
                Issue issue;
                issue.rule = QStringLiteral("empty_layer");
                issue.message = QStringLiteral("layer \"%1\" has no entities").arg(l->getName());
                issue.fix = QStringLiteral("remove it if it is not needed");
                c.add(issue);
            }
        }
        if (c.wants("layer_name")) {
            // the case most layer names follow, if any
            int upper = 0;
            int lower = 0;
            int named = 0;
            for (RS_Layer* l : *list) {
                const QString n = l->getName();
                if (n == "0" || n.compare("defpoints", Qt::CaseInsensitive) == 0 || !n.contains(QRegularExpression("[A-Za-z]"))) {
                    continue;
                }
                ++named;
                upper += n == n.toUpper() ? 1 : 0;
                lower += n == n.toLower() ? 1 : 0;
            }
            const QString convention = named >= 3 && upper * 3 >= named * 2 ? QStringLiteral("upper")
                                       : named >= 3 && lower * 3 >= named * 2 ? QStringLiteral("lower") : QString();
            static const QRegularExpression special(QStringLiteral("[^A-Za-z0-9_\\-. ]"));
            for (RS_Layer* l : std::as_const(layers)) {
                const QString n = l->getName();
                if (n == "0" || n.compare("defpoints", Qt::CaseInsensitive) == 0) {
                    continue;
                }
                QStringList problems;
                if (n != n.trimmed()) {
                    problems << QStringLiteral("starts or ends with a space");
                }
                else if (n.contains(' ')) {
                    problems << QStringLiteral("has spaces");
                }
                if (const auto m = special.match(n); m.hasMatch()) {
                    QString chars;
                    for (auto it = special.globalMatch(n); it.hasNext();) {
                        const QString ch = it.next().captured(0);
                        if (!chars.contains(ch)) {
                            chars += ch;
                        }
                    }
                    problems << QStringLiteral("has special characters (%1)").arg(chars);
                }
                if (convention == "upper" && n != n.toUpper()) {
                    problems << QStringLiteral("is not in upper case like the other layers");
                }
                else if (convention == "lower" && n != n.toLower()) {
                    problems << QStringLiteral("is not in lower case like the other layers");
                }
                if (problems.isEmpty()) {
                    continue;
                }
                QString clean = n.trimmed();
                clean.replace(special, QString());
                clean.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral("_"));
                clean = convention == "upper" ? clean.toUpper() : convention == "lower" ? clean.toLower() : clean;
                Issue issue;
                issue.rule = QStringLiteral("layer_name");
                issue.message = QStringLiteral("layer \"%1\" %2").arg(n, problems.join(" and "));
                issue.fix = clean.isEmpty() || clean == n ? QStringLiteral("rename it") : QStringLiteral("rename it to \"%1\"").arg(clean);
                c.add(issue);
            }
            // names that differ only in case, spaces or a plural
            QMap<QString, QStringList> alike;
            for (RS_Layer* l : *list) {
                if (l->getName() != "0" && !layerKey(l->getName()).isEmpty()) {
                    alike[layerKey(l->getName())] << l->getName();
                }
            }
            for (auto it = alike.cbegin(); it != alike.cend(); ++it) {
                if (it.value().size() < 2) {
                    continue;
                }
                bool relevant = c.wholeDrawing;
                for (const QString& n : it.value()) {
                    relevant = relevant || n == c.layerScope;
                }
                if (!relevant) {
                    continue;
                }
                QStringList quoted;
                for (const QString& n : it.value()) {
                    quoted << "\"" + n + "\"";
                }
                Issue issue;
                issue.rule = QStringLiteral("layer_name");
                issue.message = QStringLiteral("layers %1 look like the same layer").arg(quoted.join(", "));
                issue.fix = QStringLiteral("merge them into one: move the entities over and remove the others");
                c.add(issue);
            }
        }
    }

    // --- block inserts

    void checkInserts(Context& c) {
        for (RS_Entity* e : std::as_const(c.scoped)) {
            if (e->rtti() != RS2::EntityInsert) {
                continue;
            }
            auto* insert = static_cast<RS_Insert*>(e);
            const RS_Vector s = insert->getScale();
            Issue issue;
            issue.rule = QStringLiteral("block_insert");
            issue.ids = {e->getId()};
            issue.position = insert->getInsertionPoint();
            if (insert->getBlockForInsert() == nullptr) {
                issue.severity = QStringLiteral("error");
                issue.message = QStringLiteral("%1 at %2 inserts block \"%3\", which does not exist")
                                    .arg(ref(e), fmt(insert->getInsertionPoint()), insert->getName());
                issue.fix = QStringLiteral("define the block again, or delete the insert");
            }
            else if (std::abs(s.x) <= 1e-9 || std::abs(s.y) <= 1e-9) {
                issue.severity = QStringLiteral("error");
                issue.message = QStringLiteral("%1 \"%2\" at %3 has zero scale (%4, %5): it shows nothing")
                                    .arg(ref(e), insert->getName(), fmt(insert->getInsertionPoint()), fmt(s.x), fmt(s.y));
                issue.fix = QStringLiteral("set its scale to 1, or delete it");
            }
            else if (s.x * s.y < 0) {
                issue.severity = QStringLiteral("note");
                issue.message = QStringLiteral("%1 \"%2\" at %3 is mirrored (scale %4, %5)")
                                    .arg(ref(e), insert->getName(), fmt(insert->getInsertionPoint()), fmt(s.x), fmt(s.y));
                issue.fix = QStringLiteral("fine if intended; otherwise mirror it back so the block reads the right way round");
            }
            else {
                continue;
            }
            c.add(issue);
        }
    }

    // --- strays

    void checkOutliers(Context& c) {
        for (RS_Entity* e : std::as_const(c.scoped)) {
            if (!c.outliers.contains(e->getId())) {
                continue;
            }
            RS_Vector min;
            RS_Vector max;
            bounds(e, min, max);
            const double dx = std::max({0.0, c.mainMin.x - max.x, min.x - c.mainMax.x});
            const double dy = std::max({0.0, c.mainMin.y - max.y, min.y - c.mainMax.y});
            issueAt(c, "outlier", e, centreOf(e),
                    QStringLiteral("%1 at %2 is %3 away from the rest of the drawing (%4 .. %5)")
                        .arg(ref(e), fmt(centreOf(e)), fmt(std::hypot(dx, dy)), fmt(c.mainMin), fmt(c.mainMax)),
                    QStringLiteral("delete it if it is a stray, or move it where it belongs (it also spoils Zoom to fit)"), false);
        }
    }

    /** Every wanted rule on the prepared context, in report order. */
    void runRules(Context& c) {
        c.issues.clear();
        c.skip.clear();
        checkLengths(c); // also notes the zero-length entities, which the pair rules leave out
        if (c.wants("duplicate") || c.wants("overlap") || c.wants("endpoint_gap")) {
            checkDuplicates(c); // the copies are left out of overlaps and gaps
        }
        if (c.wants("overlap")) {
            checkLineOverlaps(c);
            checkArcOverlaps(c);
        }
        if (c.wants("endpoint_gap")) {
            checkGaps(c);
        }
        if (c.wants("nearly_closed")) {
            checkNearlyClosed(c);
        }
        if (c.wants("outlier")) {
            checkOutliers(c);
        }
        if (c.wants("block_insert")) {
            checkInserts(c);
        }
        if (c.wants("text_height")) {
            checkTextHeights(c);
        }
        if (c.wants("dim_style")) {
            checkDimensions(c);
        }
        if (c.wants("layer0") || c.wants("empty_layer") || c.wants("layer_name")) {
            checkLayers(c);
        }
        if (c.wants("bylayer")) {
            checkByLayer(c);
        }
        // report order: by rule, as RULES lists them
        QHash<QString, int> order;
        int i = 0;
        for (const Rule& r : RULES) {
            order.insert(QString::fromLatin1(r.id), i++);
        }
        std::stable_sort(c.issues.begin(), c.issues.end(),
                         [&order](const Issue& a, const Issue& b) { return order.value(a.rule) < order.value(b.rule); });
    }

    Options optionsFrom(const QJsonObject& args) {
        Options o;
        o.scope = args.value("scope").toString(QStringLiteral("all"));
        o.layer = args.value("layer").toString();
        for (const QJsonValue& v : args.value("rules").toArray()) {
            o.rules << v.toString();
        }
        if (o.rules.isEmpty() && args.value("rules").isString()) {
            o.rules = args.value("rules").toString().split(',', Qt::SkipEmptyParts);
        }
        o.tolerance = args.value("tolerance").toDouble(0);
        return o;
    }

    // ------------------------------------------------------------------ the fixes

    /**
     * Applies the safe fixes as one undo step. Each kind is done in its own pass on the drawing as the previous
     * passes left it (a merged line may then be snapped to a neighbour), all inside one undo cycle.
     */
    struct Fixer {
        Context base;          // tolerances and scope of the first check
        QSet<Id> scopeIds;     // scope selection or layer: the entities in it (replacements join)
        QSet<Id> wanted;       // only issues touching these ids (empty: all); replacements join
        QStringList log;
        int changes {0};
        QHash<Id, RS_Entity*> byId;
        QSet<RS_Entity*> added; // what the passes so far put in the drawing (and is still there)

        Context detect(const char* rule) {
            Context c;
            c.graphic = base.graphic;
            collect(c, scopeIds);
            c.rules = {QString::fromLatin1(rule)};
            c.wholeDrawing = base.wholeDrawing;
            c.layerScope = base.layerScope;
            c.eps = base.eps;
            c.gap = base.gap;
            c.size = base.size;
            runRules(c);
            byId.clear();
            for (RS_Entity* e : std::as_const(c.all)) {
                byId.insert(e->getId(), e);
            }
            return c;
        }

        bool wants(const Issue& issue) const {
            if (!issue.fixable) {
                return false;
            }
            if (wanted.isEmpty()) {
                return true;
            }
            for (const Id id : issue.ids) {
                if (wanted.contains(id)) {
                    return true;
                }
            }
            return false;
        }

        void replaced(const RS_Entity* old, const RS_Entity* now) {
            if (!scopeIds.isEmpty() && scopeIds.contains(old->getId())) {
                scopeIds.insert(now->getId());
            }
            if (!wanted.isEmpty() && wanted.contains(old->getId())) {
                wanted.insert(now->getId());
            }
        }

        void note(const QString& line) {
            ++changes;
            log << line;
        }
    };

    /**
     * The changes of one pass. What an earlier pass of the same fix added and this one replaces or deletes is not
     * deleted in the undo step but taken back out of it: it was not in the drawing before the fix, and one Undo
     * must give back exactly that drawing (a line merged twice, a merged line then set ByLayer).
     */
    struct Edit {
        LC_DocumentModificationBatch& ctx;
        Fixer& f;
        QList<RS_Entity*> discard; // taken out after the pass's own work, which still reads them

        void remove(RS_Entity* e) {
            if (f.added.remove(e)) {
                discard << e;
            }
            else {
                ctx.remove(e);
            }
        }

        void replace(RS_Entity* e, RS_Entity* now) {
            remove(e);
            ctx.add(now);
            f.added.insert(now);
            f.replaced(e, now);
        }
    };

    RS_Entity* withPenByLayer(RS_Entity* e) {
        RS_Entity* clone = e->clone();
        RS_Pen pen = clone->getPen(false);
        if (!pen.getColor().isByLayer() && !pen.getColor().isByBlock()) {
            pen.setColor(RS_Color(RS2::FlagByLayer));
        }
        if (pen.getLineType() != RS2::LineByLayer && pen.getLineType() != RS2::LineByBlock) {
            pen.setLineType(RS2::LineByLayer);
        }
        if (pen.getWidth() != RS2::WidthByLayer && pen.getWidth() != RS2::WidthByBlock) {
            pen.setWidth(RS2::WidthByLayer);
        }
        clone->setPen(pen);
        if (RS2::isDimensionalEntity(clone->rtti()) || clone->rtti() == RS2::EntityInsert) {
            clone->update();
        }
        return clone;
    }

    /**
     * A copy of e with its end at `from` moved to `to`: a line's end, a polyline's end vertex, an arc's end around its
     * circle (nullptr when `to` is off the circle: that would change its radius).
     */
    RS_Entity* withEndMoved(RS_Entity* e, const RS_Vector& from, const RS_Vector& to, const double eps) {
        const bool atStart = from.distanceTo(e->getStartpoint()) <= from.distanceTo(e->getEndpoint());
        if (e->rtti() == RS2::EntityLine) {
            auto* line = static_cast<RS_Line*>(e->clone());
            atStart ? line->setStartpoint(to) : line->setEndpoint(to);
            return line;
        }
        if (e->rtti() == RS2::EntityArc) {
            auto* arc = static_cast<RS_Arc*>(e);
            if (std::abs(to.distanceTo(arc->getCenter()) - arc->getRadius()) > eps) {
                return nullptr;
            }
            arc = static_cast<RS_Arc*>(e->clone());
            const double angle = arc->getCenter().angleTo(to);
            atStart ? arc->setAngle1(angle) : arc->setAngle2(angle);
            return arc;
        }
        if (e->rtti() == RS2::EntityPolyline) {
            QList<Vertex> points;
            if (!vertices(static_cast<RS_Polyline*>(e), points)) {
                return nullptr;
            }
            (atStart ? points.first() : points.last()).point = to;
            return rebuiltPolyline(static_cast<RS_Polyline*>(e), points, false);
        }
        return nullptr;
    }

    /** The line through both, oriented like keep, or nullptr when keep already covers other. */
    RS_Entity* mergedLine(RS_Entity* keep, RS_Entity* other, const double eps) {
        const RS_Vector s = keep->getStartpoint();
        const RS_Vector d = (keep->getEndpoint() - s).normalized();
        double lo = 0;
        double hi = d.dotP(keep->getEndpoint() - s);
        const double t0 = d.dotP(other->getStartpoint() - s);
        const double t1 = d.dotP(other->getEndpoint() - s);
        const double newLo = std::min({lo, t0, t1});
        const double newHi = std::max({hi, t0, t1});
        if (newLo >= lo - eps && newHi <= hi + eps) {
            return nullptr;
        }
        auto* line = static_cast<RS_Line*>(keep->clone());
        line->setStartpoint(s + d * newLo);
        line->setEndpoint(s + d * newHi);
        return line;
    }

    /** The arc (or circle) covering both arcs, with keep's layer and pen; nullptr when keep already covers other. */
    RS_Entity* mergedArc(RS_Arc* keep, RS_Arc* other, const double eps) {
        double sa = 0;
        double la = 0;
        double sb = 0;
        double lb = 0;
        arcInterval(keep, sa, la);
        arcInterval(other, sb, lb);
        double start = 0;
        double sweep = 0;
        const double tiny = eps / std::max(keep->getRadius(), eps);
        if (!arcUnion(sa, la, sb, lb, tiny, start, sweep) || sweep <= la + tiny) {
            return nullptr;
        }
        RS_Entity* merged = nullptr;
        if (sweep >= 2 * M_PI - tiny) {
            merged = new RS_Circle(keep->getParent(), RS_CircleData(keep->getCenter(), keep->getRadius()));
        }
        else {
            merged = new RS_Arc(keep->getParent(), RS_ArcData(keep->getCenter(), keep->getRadius(), start, start + sweep, false));
        }
        merged->setLayer(keep->getLayer(false));
        merged->setPen(keep->getPen(false));
        merged->calculateBorders();
        return merged;
    }

    LC_AIToolResult fixIssues(const QJsonObject& args) {
        RS_GraphicView* view = LC_AITools::view();
        RS_Graphic* graphic = view != nullptr ? view->getGraphic() : nullptr;
        RS_Document* document = view != nullptr ? view->getDocument() : nullptr;
        if (graphic == nullptr || document == nullptr || view->getViewPort() == nullptr) {
            return LC_AIToolResult::error(NO_DRAWING);
        }
        Options options = optionsFrom(args);
        // only the rules it can fix; the others are named back
        QSet<QString> asked;
        if (const QString error = parseRules(options.rules, asked); !error.isEmpty()) {
            return LC_AIToolResult::error(error);
        }
        QStringList fixable;
        QStringList notFixable;
        for (const Rule& r : RULES) {
            if (asked.contains(QLatin1String(r.id))) {
                (r.fixable ? fixable : notFixable) << QString::fromLatin1(r.id);
            }
        }
        if (fixable.isEmpty()) {
            return LC_AIToolResult::error(QStringLiteral("None of these rules has an automatic fix (%1). Fixable: zero_length, duplicate, "
                                                         "overlap, endpoint_gap, nearly_closed, bylayer. Fix the others with "
                                                         "run_command, add_text or the toolbar buttons.").arg(notFixable.join(", ")));
        }
        options.rules = fixable;
        Fixer f;
        QString scopeText;
        if (const QString error = setUp(f.base, graphic, options, scopeText); !error.isEmpty()) {
            return LC_AIToolResult::error(error);
        }
        if (!f.base.wholeDrawing) {
            for (RS_Entity* e : std::as_const(f.base.scoped)) {
                f.scopeIds.insert(e->getId());
            }
            if (f.scopeIds.isEmpty()) {
                return LC_AIToolResult::text(QStringLiteral("%1 has no entities: nothing to fix.").arg(scopeText));
            }
        }
        for (const QJsonValue& v : args.value("ids").toArray()) {
            const Id id = v.isDouble() ? Id(v.toDouble()) : v.toString().remove('#').toULongLong();
            if (id != 0) {
                f.wanted.insert(id);
            }
        }
        runRules(f.base);
        int candidates = 0;
        for (const Issue& issue : std::as_const(f.base.issues)) {
            candidates += f.wants(issue) ? 1 : 0;
        }
        if (candidates == 0) {
            return LC_AIToolResult::text(f.wanted.isEmpty() ? QStringLiteral("Nothing to fix in %1: no %2 issues that can be fixed automatically.")
                                                                  .arg(scopeText, fixable.join(", "))
                                                            : QStringLiteral("None of the given ids has an issue that can be fixed automatically "
                                                                             "(%1). Run check_drawing to see the ids.").arg(fixable.join(", ")));
        }
        const QSet<QString> rules(fixable.begin(), fixable.end());
        LC_GraphicViewport* viewport = view->getViewPort();
        {
            // one undo cycle around all the passes (each is what RS_Document::undoableModify runs): one Undo puts the
            // drawing back as it was
            LC_UndoSection undo(document, viewport);
            const auto pass = [&undo, &f](const std::function<void(Edit&)>& work) {
                undo.undoableExecute([&undo, &f, &work](LC_DocumentModificationBatch& ctx) {
                    ctx.dontSetActiveLayerAndPen(); // replacements keep their own layer and pen
                    Edit edit{ctx, f, {}};
                    work(edit);
                    for (RS_Entity* e : std::as_const(edit.discard)) {
                        undo.undoableDiscard(e); // before the replacements go in: they take over its identity
                    }
                    return true;
                });
            };
            if (rules.contains("zero_length")) {
                const Context c = f.detect("zero_length");
                pass([&](Edit& edit) {
                    for (const Issue& issue : c.issues) {
                        RS_Entity* e = f.byId.value(issue.ids.first());
                        if (e == nullptr || !f.wants(issue)) {
                            continue;
                        }
                        QList<Vertex> points;
                        if (e->rtti() == RS2::EntityPolyline && e->getLength() > f.base.eps && vertices(static_cast<RS_Polyline*>(e), points)) {
                            auto* polyline = static_cast<RS_Polyline*>(e);
                            const bool closed = polyline->isClosed();
                            if (closed) {
                                points.removeLast(); // the closing segment comes back by itself
                            }
                            QList<Vertex> kept;
                            for (const Vertex& v : std::as_const(points)) {
                                if (!kept.isEmpty() && kept.last().point.distanceTo(v.point) <= f.base.eps) {
                                    kept.last().bulge = v.bulge; // the next segment starts here
                                    continue;
                                }
                                kept << v;
                            }
                            if (closed && kept.size() > 1 && kept.last().point.distanceTo(kept.first().point) <= f.base.eps) {
                                kept.removeLast();
                            }
                            RS_Polyline* rebuilt = rebuiltPolyline(polyline, kept, closed);
                            edit.replace(e, rebuilt);
                            f.note(QStringLiteral("removed %1 repeated vertex(es) from %2 (now #%3)")
                                       .arg(points.size() - kept.size()).arg(ref(e)).arg(rebuilt->getId()));
                        }
                        else {
                            edit.remove(e);
                            f.note(QStringLiteral("deleted zero-length %1").arg(ref(e)));
                        }
                    }
                });
            }
            if (rules.contains("duplicate")) {
                const Context c = f.detect("duplicate");
                pass([&](Edit& edit) {
                    for (const Issue& issue : c.issues) {
                        RS_Entity* e = f.byId.value(issue.ids.first());
                        if (e != nullptr && f.wants(issue)) {
                            edit.remove(e);
                            f.note(QStringLiteral("deleted %1 (a copy of #%2)").arg(ref(e)).arg(issue.ids.value(1)));
                        }
                    }
                });
            }
            if (rules.contains("overlap")) {
                // a chain of overlapping pieces merges one pair per round
                for (int round = 0; round < 25; ++round) {
                    const Context c = f.detect("overlap");
                    int applied = 0;
                    pass([&](Edit& edit) {
                        QSet<Id> used;
                        for (const Issue& issue : c.issues) {
                            RS_Entity* keep = f.byId.value(issue.ids.value(0));
                            RS_Entity* other = f.byId.value(issue.ids.value(1));
                            if (keep == nullptr || other == nullptr || !f.wants(issue) || used.contains(keep->getId())
                                || used.contains(other->getId())) {
                                continue;
                            }
                            used << keep->getId() << other->getId();
                            ++applied;
                            if (keep->rtti() == RS2::EntityCircle) {
                                edit.remove(other);
                                f.note(QStringLiteral("deleted %1 (it lay on #%2 circle)").arg(ref(other)).arg(keep->getId()));
                                continue;
                            }
                            RS_Entity* merged = keep->rtti() == RS2::EntityLine
                                                    ? mergedLine(keep, other, f.base.eps)
                                                    : mergedArc(static_cast<RS_Arc*>(keep), static_cast<RS_Arc*>(other), f.base.eps);
                            edit.remove(other);
                            if (merged == nullptr) {
                                f.note(QStringLiteral("deleted %1 (it lay on #%2)").arg(ref(other)).arg(keep->getId()));
                                continue;
                            }
                            edit.replace(keep, merged);
                            f.replaced(other, merged);
                            f.note(QStringLiteral("merged %1 and %2 into #%3 %4").arg(ref(keep), ref(other)).arg(merged->getId())
                                       .arg(LC_AITools::typeName(merged)));
                        }
                    });
                    if (applied == 0) {
                        break;
                    }
                }
            }
            if (rules.contains("nearly_closed")) {
                const Context c = f.detect("nearly_closed");
                pass([&](Edit& edit) {
                    for (const Issue& issue : c.issues) {
                        auto* polyline = static_cast<RS_Polyline*>(f.byId.value(issue.ids.first()));
                        QList<Vertex> points;
                        if (polyline == nullptr || !f.wants(issue) || !vertices(polyline, points) || points.size() < 3) {
                            continue;
                        }
                        const double gap = points.first().point.distanceTo(points.last().point);
                        points.removeLast(); // the closing segment ends at the start instead
                        RS_Polyline* closed = rebuiltPolyline(polyline, points, true);
                        edit.replace(polyline, closed);
                        f.note(gap <= f.base.eps ? QStringLiteral("marked %1 closed (now #%2)").arg(ref(polyline)).arg(closed->getId())
                                                 : QStringLiteral("closed %1, its ends were %2 apart (now #%3)")
                                                       .arg(ref(polyline), fmt(gap)).arg(closed->getId()));
                    }
                });
            }
            if (rules.contains("endpoint_gap")) {
                for (int round = 0; round < 5; ++round) {
                    const Context c = f.detect("endpoint_gap");
                    int applied = 0;
                    pass([&](Edit& edit) {
                        QSet<Id> used;
                        for (const Issue& issue : c.issues) {
                            RS_Entity* e = f.byId.value(issue.ids.first());
                            // a corner: the other entity's end moves to the meeting point too
                            RS_Entity* other = issue.alsoFrom.valid ? f.byId.value(issue.ids.value(1)) : nullptr;
                            if (e == nullptr || !f.wants(issue) || used.contains(e->getId()) || !issue.moveFrom.valid || !issue.moveTo.valid
                                || (issue.alsoFrom.valid && (other == nullptr || used.contains(other->getId())))) {
                                continue;
                            }
                            const bool atStart = issue.moveFrom.distanceTo(e->getStartpoint()) <= issue.moveFrom.distanceTo(e->getEndpoint());
                            RS_Entity* moved = withEndMoved(e, issue.moveFrom, issue.moveTo, f.base.eps);
                            RS_Entity* otherMoved = other != nullptr ? withEndMoved(other, issue.alsoFrom, issue.moveTo, f.base.eps) : nullptr;
                            if (moved == nullptr || (other != nullptr && otherMoved == nullptr)) {
                                delete moved;
                                delete otherMoved;
                                continue;
                            }
                            used << e->getId();
                            ++applied;
                            edit.replace(e, moved);
                            if (other == nullptr) {
                                f.note(QStringLiteral("moved the %1 of %2 from %3 onto #%4 at %5 (now #%6)")
                                           .arg(endName(atStart), ref(e), fmt(issue.moveFrom))
                                           .arg(issue.ids.value(1)).arg(fmt(issue.moveTo)).arg(moved->getId()));
                                continue;
                            }
                            const bool otherAtStart = issue.alsoFrom.distanceTo(other->getStartpoint()) <= issue.alsoFrom.distanceTo(other->getEndpoint());
                            used << other->getId();
                            edit.replace(other, otherMoved);
                            f.note(QStringLiteral("moved the %1 of %2 and the %3 of %4 to where they meet, %5 (now #%6 and #%7)")
                                       .arg(endName(atStart), ref(e), endName(otherAtStart), ref(other), fmt(issue.moveTo))
                                       .arg(moved->getId()).arg(otherMoved->getId()));
                        }
                    });
                    if (applied == 0) {
                        break;
                    }
                }
            }
            if (rules.contains("bylayer")) {
                const Context c = f.detect("bylayer");
                pass([&](Edit& edit) {
                    for (const Issue& issue : c.issues) {
                        RS_Entity* e = f.byId.value(issue.ids.first());
                        if (e == nullptr || !f.wants(issue)) {
                            continue;
                        }
                        const QStringList what = explicitPen(e);
                        RS_Entity* clone = withPenByLayer(e);
                        edit.replace(e, clone);
                        f.note(QStringLiteral("set %1 ByLayer (was %2; now #%3)").arg(ref(e), what.join(", ")).arg(clone->getId()));
                    }
                });
            }
        }
        view->redraw();
        QStringList out;
        if (f.changes == 0) {
            out << QStringLiteral("Nothing was changed (the issues were gone, or their entities are on locked layers).");
        }
        else {
            out << QStringLiteral("Fixed %1 issue(s) in %2 as one undo step (Undo reverts all of it):").arg(f.changes).arg(scopeText);
            for (int i = 0; i < f.log.size() && i < MAX_FIX_LINES; ++i) {
                out << "- " + f.log[i];
            }
            if (f.log.size() > MAX_FIX_LINES) {
                out << QStringLiteral("- ... and %1 more").arg(f.log.size() - MAX_FIX_LINES);
            }
        }
        // what is left, so the AI knows what still needs a decision
        Options all = options;
        all.rules.clear();
        const Report after = LC_AIDrawingChecker::check(graphic, all);
        QMap<QString, int> left;
        for (const Issue& issue : after.issues) {
            left[issue.rule] += 1;
        }
        if (left.isEmpty()) {
            out << QStringLiteral("check_drawing now finds no issues in %1.").arg(scopeText);
        }
        else {
            QStringList parts;
            for (auto it = left.cbegin(); it != left.cend(); ++it) {
                parts << QStringLiteral("%1 %2").arg(it.value()).arg(it.key());
            }
            out << QStringLiteral("Still in %1: %2. Run check_drawing for the details; those need a decision (ask the user when unsure).")
                       .arg(scopeText, parts.join(", "));
        }
        return LC_AIToolResult::text(out.join('\n'));
    }

    // ------------------------------------------------------------------ the tools

    QJsonObject stringArray(const QString& description) {
        return {{"type", "array"}, {"items", QJsonObject{{"type", "string"}}}, {"description", description}};
    }

    QJsonObject scopeSchema() {
        return {{"type", "string"}, {"enum", QJsonArray{"all", "selection", "layer"}},
                {"description", "What to check: the whole drawing (default), the selected entities, or one layer (give \"layer\")"}};
    }

    LC_AITool checkTool() {
        LC_AITool t;
        t.name = "check_drawing";
        t.label = "Check drawing";
        t.description =
            "Check the drawing for problems and list each with entity ids, a position and a suggested fix: zero-length "
            "geometry, tiny segments, exact duplicates, overlapping collinear lines and arcs, endpoints that nearly meet, "
            "polylines left open, entities far from the rest, block inserts with zero scale or mirroring, mixed or odd text "
            "heights, inconsistent dimension styles, entities on layer 0, empty or oddly named layers, and colour/line "
            "type/width set on entities instead of ByLayer. Read-only. Issues marked (fixable) can be fixed with "
            "fix_drawing_issues. Rules: " + allRuleIds() + ".";
        t.parameters = QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"scope", scopeSchema()},
                {"layer", QJsonObject{{"type", "string"}, {"description", "The layer to check, with scope \"layer\""}}},
                {"rules", stringArray("Only these rules (default all), or the groups geometry, layers, fixable")},
                {"tolerance", QJsonObject{{"type", "number"},
                                          {"description", "Gap tolerance in drawing units: endpoints closer than this should meet, "
                                                          "segments shorter are tiny (default 0.05% of the drawing's size)"}}},
                {"limit", QJsonObject{{"type", "integer"}, {"description", "Most issue lines to list (default 60)"}}}}}};
        t.execute = [](const QJsonObject& args) {
            RS_GraphicView* view = LC_AITools::view();
            RS_Graphic* graphic = view != nullptr ? view->getGraphic() : nullptr;
            if (graphic == nullptr) {
                return LC_AIToolResult::error(NO_DRAWING);
            }
            const Report report = LC_AIDrawingChecker::check(graphic, optionsFrom(args));
            if (!report.error.isEmpty()) {
                return LC_AIToolResult::error(report.error);
            }
            return LC_AIToolResult::text(LC_AIDrawingChecker::format(report, qBound(5, args.value("limit").toInt(60), 500)));
        };
        return t;
    }

    LC_AITool fixTool() {
        LC_AITool t;
        t.name = "fix_drawing_issues";
        t.label = "Fix drawing issues";
        t.description =
            "Apply the safe fixes for what check_drawing reports, as ONE undo step: delete zero-length entities and exact "
            "duplicates (same layer and pen), merge overlapping collinear lines and overlapping arcs, close nearly closed "
            "polylines, snap endpoints that nearly meet, and set explicit colour/line type/width to ByLayer. Give ids (from "
            "check_drawing) or rules to fix only some; nothing given fixes all of them. Entities on locked layers are left "
            "alone. Returns what changed and what is still left (those need a decision).";
        t.parameters = QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"ids", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "integer"}}},
                                    {"description", "Only the issues involving these entity ids"}}},
                {"rules", stringArray("Only these rules: zero_length, duplicate, overlap, endpoint_gap, nearly_closed, bylayer")},
                {"scope", scopeSchema()},
                {"layer", QJsonObject{{"type", "string"}, {"description", "The layer, with scope \"layer\""}}},
                {"tolerance", QJsonObject{{"type", "number"}, {"description", "Gap tolerance as for check_drawing"}}}}}};
        t.execute = [](const QJsonObject& args) { return fixIssues(args); };
        return t;
    }

    // ------------------------------------------------------------------ the card in the chat

    QColor severityColour(const QString& severity, const LC_AITheme& t) {
        if (severity == "error") {
            return QColor("#f87171");
        }
        if (severity == "warning") {
            return QColor("#fbbf24");
        }
        return t.textMuted;
    }

    /** A row that runs something when clicked (and shows the hand). */
    class ClickRow : public QFrame {
    public:
        ClickRow(std::function<void()> onClick, QWidget* parent)
            : QFrame(parent), m_onClick(std::move(onClick)) {
            setAttribute(Qt::WA_Hover);
            setCursor(Qt::PointingHandCursor);
        }

    protected:
        void mouseReleaseEvent(QMouseEvent* event) override {
            QFrame::mouseReleaseEvent(event);
            if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()) && m_onClick) {
                m_onClick();
            }
        }

    private:
        std::function<void()> m_onClick;
    };

    /** A label that shortens its text with "…" instead of widening the chat. */
    class ElidedLabel : public QLabel {
    public:
        using QLabel::QLabel;
        QSize minimumSizeHint() const override {
            return {fontMetrics().horizontalAdvance(QStringLiteral("Tex…")), QLabel::minimumSizeHint().height()};
        }

    protected:
        void paintEvent(QPaintEvent*) override {
            QPainter p(this);
            p.setPen(palette().color(foregroundRole()));
            p.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter,
                       fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
        }
    };

    /**
     * The task "Fix all" gives the AI: what was found, counted by rule (the bubble stays short; check_drawing gives
     * the AI the details), and how to go about it.
     */
    QString fixPrompt(const Report& r) {
        QMap<QString, int> counts;
        int fixable = 0;
        for (const Issue& issue : r.issues) {
            counts[issue.rule] += 1;
            fixable += issue.fixable ? 1 : 0;
        }
        // the rules by their titles, as the card shows them (the bubble is the user's message)
        QStringList found;
        for (const Rule& rule : RULES) {
            if (const int n = counts.value(QString::fromLatin1(rule.id)); n > 0) {
                const QString title = QObject::tr(rule.title);
                found << QStringLiteral("%1 (%2)").arg(title.left(1).toLower() + title.mid(1)).arg(n);
            }
        }
        const QString scope = r.scopeText == "the selection" ? QStringLiteral(" (scope \"selection\")") : QString();
        const QString what = r.issues.size() == 1 ? QStringLiteral("issue") : QStringLiteral("%1 issues").arg(r.issues.size());
        if (fixable == 0) {
            return QStringLiteral("Help me fix the %1 the drawing check found in %2: %3. None can be fixed automatically: run "
                                  "check_drawing%4 for the details, then ask me before each change.")
                .arg(what, r.scopeText, found.join(", "), scope);
        }
        return QStringLiteral("Fix the %1 the drawing check found in %2: %3. Use fix_drawing_issues%4 for the safe fixes, then "
                              "check_drawing%4 for what is left; ask me before anything that needs a decision.")
            .arg(what, r.scopeText, found.join(", "), scope);
    }

    /** Selects the issue's entities and zooms to where it is, in the drawing that was checked. */
    void showIssue(const Report& report, const Issue& issue) {
        RS_GraphicView* view = LC_AITools::view();
        RS_Graphic* graphic = view != nullptr ? view->getGraphic() : nullptr;
        LC_GraphicViewport* vp = view != nullptr ? view->getViewPort() : nullptr;
        if (graphic == nullptr || vp == nullptr || graphic != report.graphic) {
            return; // another drawing is open now
        }
        QList<RS_Entity*> entities;
        RS_Vector min(false);
        RS_Vector max(false);
        const QSet<Id> ids(issue.ids.begin(), issue.ids.end());
        for (RS_Entity* e : *graphic) {
            if (!live(e) || !ids.contains(e->getId())) {
                continue;
            }
            entities << e;
            RS_Vector a;
            RS_Vector b;
            if (bounds(e, a, b)) {
                min = min.valid ? RS_Vector(std::min(min.x, a.x), std::min(min.y, a.y)) : a;
                max = max.valid ? RS_Vector(std::max(max.x, b.x), std::max(max.y, b.y)) : b;
            }
        }
        // around the spot itself (a 0.1 gap at the corner of a long wall), with enough of the drawing to recognise it
        const RS_Vector centre = issue.position.valid ? issue.position : min.valid ? (min + max) * 0.5 : RS_Vector(false);
        if (centre.valid) {
            const double extent = min.valid && ids.size() <= 2 ? std::max(max.x - min.x, max.y - min.y) * 0.6 : 0.0;
            const double least = std::max(report.size * 0.04, report.tolerance * 20);
            double half = std::clamp(extent, least, std::max(report.size * 0.15, least));
            if (issue.moveFrom.valid && issue.moveTo.valid) {
                // a gap: close enough to see it
                half = std::max({issue.moveFrom.distanceTo(issue.moveTo) * 25, report.tolerance * 10, report.precision * 1000});
            }
            const RS_Vector a(centre.x - half, centre.y - half);
            const RS_Vector b(centre.x + half, centre.y + half);
            RS_Vector ucsMin(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
            RS_Vector ucsMax(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest());
            for (const RS_Vector& w : {a, b, RS_Vector(a.x, b.y), RS_Vector(b.x, a.y)}) {
                const RS_Vector u = vp->toUCS(w);
                ucsMin = RS_Vector(std::min(ucsMin.x, u.x), std::min(ucsMin.y, u.y));
                ucsMax = RS_Vector(std::max(ucsMax.x, u.x), std::max(ucsMax.y, u.y));
            }
            vp->zoomWindow(ucsMin, ucsMax, true);
        }
        RS_Selection::unselectAllInDocument(graphic, vp);
        if (!entities.isEmpty()) {
            RS_Selection::selectEntitiesList(graphic, vp, entities, true);
        }
        view->redraw();
    }

    /** The result of "Check Drawing" in the chat: the issues by rule (click one to see it), Fix all, Check again. */
    class CheckCard : public QFrame {
    public:
        CheckCard(const Report& report, LC_AIAssistantWidget* chat, const LC_AITheme& theme, QWidget* parent)
            : QFrame(parent), m_report(report), m_chat(chat) {
            setObjectName("checkCard");
            const LC_AITheme& t = theme;
            setStyleSheet(QStringLiteral(R"(
#checkCard { background: %1; border: 1px solid %2; border-radius: 10px; }
#checkTitle { font-size: 13px; font-weight: 500; color: %3; }
#checkCount { font-size: 12px; color: %4; }
#checkSub { font-size: 12px; color: %4; }
#ruleRow { border-radius: 7px; background: transparent; }
#ruleRow:hover { background: %5; }
#ruleName { font-size: 12.5px; color: %3; }
#ruleCount { font-size: 12px; color: %6; }
#fixTag { font-size: 11px; color: %9; }
#issueRow { border-radius: 6px; background: transparent; }
#issueRow:hover { background: %5; }
#issueText { font-size: 12px; color: %6; }
#issueFix { font-size: 11.5px; color: %4; }
#checkFix { background: %7; color: %8; border: none; border-radius: 8px; padding: 0 12px; min-height: 28px; font-size: 12.5px; font-weight: 500; }
#checkFix:disabled { background: %5; color: %4; }
#checkAgain { background: transparent; color: %3; border: 1px solid %2; border-radius: 8px; padding: 0 10px; min-height: 28px; font-size: 12.5px; }
#checkAgain:hover { background: %5; }
)")
                              .arg(t.bgElevated.name(), t.borderDefault.name(QColor::HexArgb), t.textPrimary.name(), t.textMuted.name(),
                                   t.bgHover.name(QColor::HexArgb), t.textSecondary.name(), t.textHeading.name(), t.bgRoot.name(),
                                   t.accentGreen.name()));
            const qreal dpr = devicePixelRatioF();
            auto* layout = new QVBoxLayout(this);
            layout->setContentsMargins(12, 10, 12, 10);
            layout->setSpacing(4);

            int fixable = 0;
            int errors = 0;
            for (const Issue& issue : std::as_const(report.issues)) {
                fixable += issue.fixable ? 1 : 0;
                errors += issue.severity == "error" ? 1 : 0;
            }
            const bool clean = report.error.isEmpty() && report.issues.isEmpty();
            auto* header = new QHBoxLayout();
            header->setSpacing(7);
            auto* icon = new QLabel(this);
            const QColor tone = !report.error.isEmpty() ? t.textMuted : clean ? t.accentGreen : errors > 0 ? QColor("#f87171") : QColor("#fbbf24");
            icon->setPixmap(LC_AIIcons::lucide(clean ? "shield-check" : "shield-alert", 15, tone, dpr));
            auto* title = new QLabel(QObject::tr("Drawing check"), this);
            title->setObjectName("checkTitle");
            auto* count = new QLabel(this);
            count->setObjectName("checkCount");
            count->setText(!report.error.isEmpty() ? QString()
                           : clean                 ? QObject::tr("no issues")
                           : report.issues.size() == 1 ? QObject::tr("1 issue")
                                                       : QObject::tr("%1 issues").arg(report.issues.size()));
            header->addWidget(icon);
            header->addWidget(title);
            header->addStretch(1);
            header->addWidget(count);
            layout->addLayout(header);

            auto* sub = new QLabel(this);
            sub->setObjectName("checkSub");
            sub->setWordWrap(true);
            sub->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            if (!report.error.isEmpty()) {
                sub->setText(report.error);
            }
            else {
                const QString scope = report.scopeText.left(1).toUpper() + report.scopeText.mid(1);
                QString text = report.checked == 1 ? QObject::tr("%1 · 1 entity").arg(scope)
                                                   : QObject::tr("%1 · %2 entities").arg(scope).arg(report.checked);
                text += QObject::tr(" · gaps under %1").arg(QString(fmt(report.tolerance) + " " + report.unit).trimmed());
                if (fixable > 0) {
                    text += QObject::tr(" · %1 fixable").arg(fixable);
                }
                sub->setText(text);
            }
            layout->addWidget(sub);
            layout->addSpacing(4);

            if (clean) {
                auto* row = new QHBoxLayout();
                row->setSpacing(7);
                auto* ok = new QLabel(this);
                ok->setPixmap(LC_AIIcons::lucide("check", 13, t.accentGreen, dpr));
                auto* text = new QLabel(QObject::tr("No problems found."), this);
                text->setObjectName("ruleName");
                row->addWidget(ok);
                row->addWidget(text, 1);
                layout->addLayout(row);
            }
            // one row per rule, opening to its issues
            QStringList order;
            for (const Rule& r : RULES) {
                order << QString::fromLatin1(r.id);
            }
            for (const QString& rule : std::as_const(order)) {
                QList<int> mine;
                for (int i = 0; i < report.issues.size(); ++i) {
                    if (report.issues[i].rule == rule) {
                        mine << i;
                    }
                }
                if (mine.isEmpty()) {
                    continue;
                }
                addGroup(rule, mine, t, dpr, layout);
            }

            { // the buttons: Check again on every card, also on "No drawing is open"
                layout->addSpacing(6);
                auto* buttons = new QHBoxLayout();
                buttons->setSpacing(8);
                if (!clean && report.error.isEmpty()) {
                    // none fixable by itself: the AI can still help, asking about each one
                    const QString label = fixable > 0 ? QObject::tr("Fix all") : QObject::tr("Ask the AI");
                    auto* fix = new QPushButton(label, this);
                    fix->setObjectName("checkFix");
                    fix->setIcon(LC_AIIcons::lucideIcon("wrench", 13, t.bgRoot));
                    fix->setCursor(Qt::PointingHandCursor);
                    fix->setToolTip(fixable > 0 ? QObject::tr("Ask the AI to fix these issues (safe fixes first, as one undo step)")
                                                : QObject::tr("None of these can be fixed automatically: ask the AI to help, "
                                                              "it asks you before each change"));
                    QObject::connect(fix, &QPushButton::clicked, this, [this, fix, label] {
                        if (!m_chat) {
                            return;
                        }
                        // an old card after its drawing was closed: there is nothing to fix now
                        const RS_GraphicView* view = LC_AITools::view();
                        if (view == nullptr || view->getGraphic() == nullptr) {
                            fix->setText(QObject::tr("No drawing is open"));
                            QTimer::singleShot(2500, fix, [fix, label] { fix->setText(label); });
                            return;
                        }
                        // sent as a message of its own, so a draft in the message box stays; while the AI works it
                        // waits for the run to end, and the button comes back if it is not sent after all
                        fix->setEnabled(false);
                        fix->setText(QObject::tr("Waiting for the AI..."));
                        const QPointer<QPushButton> button(fix);
                        m_chat->sendTask(fixPrompt(m_report), [button, label](const bool sent) {
                            if (button) {
                                button->setText(sent ? QObject::tr("Asked the AI") : label);
                                button->setEnabled(!sent);
                            }
                        });
                    });
                    buttons->addWidget(fix);
                }
                auto* again = new QPushButton(QObject::tr("Check again"), this);
                again->setObjectName("checkAgain");
                again->setIcon(LC_AIIcons::lucideIcon("refresh-cw", 13, t.textPrimary));
                again->setCursor(Qt::PointingHandCursor);
                QObject::connect(again, &QPushButton::clicked, this, [this] {
                    if (m_chat) {
                        LC_AIDrawingChecker::showInChat(m_chat);
                    }
                });
                buttons->addWidget(again);
                buttons->addStretch(1);
                layout->addLayout(buttons);
            }
        }

    private:
        void addGroup(const QString& rule, const QList<int>& indexes, const LC_AITheme& t, const qreal dpr, QVBoxLayout* layout) {
            const Rule* info = ruleInfo(rule);
            QString worst = QStringLiteral("note");
            int fixable = 0;
            for (const int i : indexes) {
                const QString s = m_report.issues[i].severity;
                worst = s == "error" || (s == "warning" && worst != "error") ? s : worst;
                fixable += m_report.issues[i].fixable ? 1 : 0;
            }
            auto* details = new QWidget(this);
            auto* chevron = new QLabel();
            auto* row = new ClickRow([details, chevron, t, dpr] {
                details->setVisible(!details->isVisible());
                chevron->setPixmap(LC_AIIcons::lucide(details->isVisible() ? "chevron-down" : "chevron-right", 12, t.textSecondary, dpr));
            }, this);
            row->setObjectName("ruleRow");
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(4, 3, 6, 3);
            rowLayout->setSpacing(6);
            chevron->setParent(row);
            chevron->setPixmap(LC_AIIcons::lucide("chevron-right", 12, t.textSecondary, dpr));
            auto* dot = new QLabel(row);
            dot->setPixmap(LC_AIIcons::lucide("circle-alert", 12, severityColour(worst, t), dpr));
            auto* name = new ElidedLabel(info != nullptr ? QObject::tr(info->title) : rule, row);
            name->setObjectName("ruleName");
            name->setToolTip(rule);
            auto* count = new QLabel(QString::number(indexes.size()), row);
            count->setObjectName("ruleCount");
            rowLayout->addWidget(chevron);
            rowLayout->addWidget(dot);
            rowLayout->addWidget(name, 1);
            if (fixable > 0) {
                auto* tag = new QLabel(fixable == indexes.size() ? QObject::tr("fixable") : QObject::tr("%1 fixable").arg(fixable), row);
                tag->setObjectName("fixTag");
                rowLayout->addWidget(tag);
            }
            rowLayout->addWidget(count);
            layout->addWidget(row);

            auto* list = new QVBoxLayout(details);
            list->setContentsMargins(22, 0, 0, 4);
            list->setSpacing(1);
            constexpr int MAX_ROWS = 30;
            for (int k = 0; k < indexes.size() && k < MAX_ROWS; ++k) {
                const int index = indexes[k];
                const Issue& issue = m_report.issues[index];
                auto* item = new ClickRow([this, index] { showIssue(m_report, m_report.issues[index]); }, details);
                item->setObjectName("issueRow");
                item->setToolTip(QObject::tr("Show it in the drawing"));
                auto* itemLayout = new QVBoxLayout(item);
                itemLayout->setContentsMargins(6, 3, 6, 3);
                itemLayout->setSpacing(1);
                auto* text = new QLabel(issue.message, item);
                text->setObjectName("issueText");
                text->setWordWrap(true);
                text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                text->setAttribute(Qt::WA_TransparentForMouseEvents);
                auto* fix = new QLabel(QObject::tr("Fix: %1").arg(issue.fix), item);
                fix->setObjectName("issueFix");
                fix->setWordWrap(true);
                fix->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                fix->setAttribute(Qt::WA_TransparentForMouseEvents);
                itemLayout->addWidget(text);
                itemLayout->addWidget(fix);
                list->addWidget(item);
            }
            if (indexes.size() > MAX_ROWS) {
                auto* more = new QLabel(QObject::tr("... and %1 more").arg(indexes.size() - MAX_ROWS), details);
                more->setObjectName("issueFix");
                more->setContentsMargins(6, 2, 6, 2);
                list->addWidget(more);
            }
            details->hide();
            layout->addWidget(details);
        }

        Report m_report;
        QPointer<LC_AIAssistantWidget> m_chat;
    };
}

// ---------------------------------------------------------------------------------------------- public

QList<QPair<QString, QString>> LC_AIDrawingChecker::rules() {
    QList<QPair<QString, QString>> out;
    for (const Rule& r : RULES) {
        out << qMakePair(QString::fromLatin1(r.id), QString::fromLatin1(r.title));
    }
    return out;
}

LC_AIDrawingChecker::Report LC_AIDrawingChecker::check(RS_Graphic* graphic, const Options& options) {
    Report report;
    report.graphic = graphic;
    if (graphic == nullptr) {
        report.error = NO_DRAWING;
        return report;
    }
    Context c;
    if (const QString error = setUp(c, graphic, options, report.scopeText); !error.isEmpty()) {
        report.error = error;
        return report;
    }
    runRules(c);
    report.issues = c.issues;
    report.checked = int(c.scoped.size());
    report.tolerance = c.gap;
    report.precision = c.eps;
    report.size = c.size;
    report.unit = graphic->getUnit() == RS2::None ? QString() : RS_Units::unitToString(graphic->getUnit());
    for (const Rule& r : RULES) {
        if (c.wants(r.id)) {
            report.rules << QString::fromLatin1(r.id);
        }
    }
    return report;
}

QString LC_AIDrawingChecker::format(const Report& report, const int limit) {
    if (!report.error.isEmpty()) {
        return report.error;
    }
    QStringList out;
    out << QStringLiteral("Checked %1: %2 entities (units: %3). Gaps under %4 should meet; points within %5 are the same point.")
               .arg(report.scopeText).arg(report.checked)
               .arg(report.unit.isEmpty() ? QStringLiteral("none set") : report.unit, fmt(report.tolerance), fmt(report.precision));
    int errors = 0;
    int warnings = 0;
    int fixable = 0;
    for (const Issue& issue : report.issues) {
        errors += issue.severity == "error" ? 1 : 0;
        warnings += issue.severity == "warning" ? 1 : 0;
        fixable += issue.fixable ? 1 : 0;
    }
    if (report.issues.isEmpty()) {
        out << QStringLiteral("No issues found (rules: %1).").arg(report.rules.join(", "));
        return out.join('\n');
    }
    out << QStringLiteral("Found %1 issue(s): %2 error(s), %3 warning(s), %4 note(s); %5 can be fixed with fix_drawing_issues.")
               .arg(report.issues.size()).arg(errors).arg(warnings).arg(int(report.issues.size()) - errors - warnings).arg(fixable);
    // every rule gets a share of the lines, so one noisy rule does not hide the rest
    QMap<QString, QList<const Issue*>> byRule;
    for (const Issue& issue : report.issues) {
        byRule[issue.rule] << &issue;
    }
    const int share = std::max(3, limit / std::max(1, int(byRule.size())));
    QStringList clean;
    for (const Rule& r : RULES) {
        const QString id = QString::fromLatin1(r.id);
        if (!report.rules.contains(id)) {
            continue;
        }
        const QList<const Issue*> issues = byRule.value(id);
        if (issues.isEmpty()) {
            clean << id;
            continue;
        }
        out << QString() << QStringLiteral("[%1] %2: %3").arg(id, QString::fromLatin1(r.title)).arg(issues.size());
        for (int i = 0; i < issues.size() && i < share; ++i) {
            const Issue* issue = issues[i];
            static const QRegularExpression hasPoint(QStringLiteral("\\(-?\\d"));
            out << QStringLiteral("- %1: %2%3. Fix: %4%5.")
                       .arg(issue->severity, issue->message,
                            issue->position.valid && !hasPoint.match(issue->message).hasMatch() ? QStringLiteral(" (at %1)").arg(fmt(issue->position))
                                                                                                 : QString(),
                            issue->fix, issue->fixable ? QStringLiteral(" (fixable)") : QString());
        }
        if (issues.size() > share) {
            QList<Id> rest;
            for (int i = share; i < issues.size(); ++i) {
                rest << issues[i]->ids.value(0);
            }
            out << QStringLiteral("- ... and %1 more of these: %2").arg(issues.size() - share).arg(idList(rest, 40));
        }
    }
    if (!clean.isEmpty()) {
        out << QString() << QStringLiteral("No issues for: %1.").arg(clean.join(", "));
    }
    out << QString() << QStringLiteral("fix_drawing_issues applies the fixable ones (all, or by ids/rules) as one undo step; the others need "
                                       "a decision: ask the user, or fix them with run_command, add_text or the toolbar buttons.");
    return out.join('\n');
}

QList<LC_AITool> LC_AIDrawingChecker::tools() {
    return {checkTool(), fixTool()};
}

void LC_AIDrawingChecker::showInChat(QWidget* window) {
    QWidget* top = window != nullptr ? window->window() : nullptr;
    if (top == nullptr) {
        return;
    }
    if (auto* toggle = top->findChild<QAction*>("AIAssistantToggle"); toggle != nullptr && !toggle->isChecked()) {
        toggle->setChecked(true); // opens the AI sidebar
    }
    auto* chat = top->findChild<LC_AIAssistantWidget*>();
    auto* transcript = chat != nullptr ? chat->findChild<LC_AIChatTranscript*>() : nullptr;
    if (transcript == nullptr) {
        return;
    }
    RS_GraphicView* view = LC_AITools::view();
    Report report;
    if (view == nullptr || view->getGraphic() == nullptr) {
        report.error = QObject::tr("No drawing is open: open or create one, then check it.");
    }
    else {
        report = check(view->getGraphic(), Options{});
    }
    transcript->addCard(new CheckCard(report, chat, LC_AITheme::current(), transcript));
}

QAction* LC_AIDrawingChecker::action(QWidget* window) {
    QWidget* top = window != nullptr ? window->window() : nullptr;
    if (top == nullptr) {
        return nullptr;
    }
    if (auto* existing = top->findChild<QAction*>("AICheckDrawing")) {
        return existing;
    }
    auto* action = new QAction(QObject::tr("Check &Drawing"), top);
    action->setObjectName("AICheckDrawing");
    action->setIcon(LC_AIIcons::lucideIcon("shield-check", 16, top->palette().color(QPalette::WindowText)));
    action->setStatusTip(QObject::tr("Find problems in the drawing (duplicates, gaps, open polylines, layers, pens...) and fix them with the AI"));
    action->setToolTip(action->statusTip());
    QObject::connect(action, &QAction::triggered, top, [top] { showInChat(top); });
    return action;
}
