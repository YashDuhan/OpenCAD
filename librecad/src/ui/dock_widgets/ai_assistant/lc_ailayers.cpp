/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_ailayers.h"

#include <QColor>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <cmath>

#include "lc_linetypenames.h"
#include "rs_document.h"
#include "rs_entity.h"
#include "rs_graphic.h"
#include "rs_graphicview.h"
#include "rs_layer.h"
#include "rs_layerlist.h"
#include "rs_pen.h"

namespace {
    QJsonObject str(const QString& description) {
        return QJsonObject{{"type", "string"}, {"description", description}};
    }

    QJsonObject flag(const QString& description) {
        return QJsonObject{{"type", "boolean"}, {"description", description}};
    }

    struct Drawing {
        RS_GraphicView* view {nullptr};
        RS_Graphic* graphic {nullptr};
        RS_Document* document {nullptr};
    };

    Drawing drawing() {
        Drawing d;
        d.view = LC_AITools::view();
        d.graphic = d.view != nullptr ? d.view->getGraphic() : nullptr;
        d.document = d.view != nullptr ? d.view->getDocument() : nullptr;
        return d;
    }

    QList<RS_Layer*> layersOf(RS_Graphic* graphic) {
        QList<RS_Layer*> out;
        for (RS_Layer* l : *graphic->getLayerList()) {
            if (l != nullptr) {
                out << l;
            }
        }
        return out;
    }

    /** The layer by its exact name, else ignoring case when only one layer matches that way. */
    RS_Layer* findLayer(RS_Graphic* graphic, const QString& name, QString& error) {
        const QString wanted = name.trimmed();
        if (wanted.isEmpty()) {
            error = QStringLiteral("Give the layer's name.");
            return nullptr;
        }
        QList<RS_Layer*> loose;
        for (RS_Layer* l : layersOf(graphic)) {
            if (l->getName() == wanted) {
                return l;
            }
            if (l->getName().compare(wanted, Qt::CaseInsensitive) == 0) {
                loose << l;
            }
        }
        if (loose.size() == 1) {
            return loose.first();
        }
        QStringList names;
        for (RS_Layer* l : layersOf(graphic)) {
            names << "\"" + l->getName() + "\"";
        }
        error = loose.size() > 1 ? QStringLiteral("Several layers are named \"%1\" apart from case: %2. Give the exact name.")
                                       .arg(wanted, names.join(", "))
                                 : QStringLiteral("There is no layer \"%1\". Layers: %2.").arg(wanted, names.join(", "));
        return nullptr;
    }

    /** The live entities of the drawing on this layer. */
    QList<RS_Entity*> entitiesOn(RS_Graphic* graphic, const RS_Layer* layer) {
        QList<RS_Entity*> out;
        for (RS_Entity* e : *graphic) {
            if (e != nullptr && !e->isDeleted() && e->getLayer() == layer) {
                out << e;
            }
        }
        return out;
    }

    QString lineTypeName(const RS2::LineType type) {
        const QString name = LC_LineTypeNames::lineTypeToName(type);
        return name.isEmpty() ? QStringLiteral("Continuous") : name;
    }

    QString describe(RS_Graphic* graphic, RS_Layer* layer) {
        QStringList flags;
        flags << (layer->isFrozen() ? "hidden" : "visible");
        if (layer->isLocked()) {
            flags << "locked";
        }
        if (!layer->isPrint()) {
            flags << "not printed";
        }
        if (layer->isConstruction()) {
            flags << "construction";
        }
        if (graphic->getActiveLayer() == layer) {
            flags << "current";
        }
        const RS_Pen pen = layer->getPen();
        return QStringLiteral("\"%1\": %2 entities; colour %3, width %4 mm, line type %5; %6")
            .arg(layer->getName())
            .arg(entitiesOn(graphic, layer).size())
            .arg(pen.getColor().toQColor().name(), QString::number(pen.getWidth() < 0 ? 0.0 : pen.getWidth() / 100.0, 'f', 2),
                 lineTypeName(pen.getLineType()), flags.join(", "));
    }

    QString listing(RS_Graphic* graphic) {
        QStringList lines;
        const QList<RS_Layer*> layers = layersOf(graphic);
        lines << QStringLiteral("%1 layer(s):").arg(layers.size());
        for (RS_Layer* l : layers) {
            lines << "- " + describe(graphic, l);
        }
        return lines.join('\n');
    }

    bool parseColor(const QString& text, RS_Color& out) {
        const QColor c = QColor::fromString(text.trimmed().toLower()); // #rrggbb, or an SVG colour name (red, cyan, gray...)
        if (!c.isValid()) {
            return false;
        }
        out = RS_Color(c);
        return true;
    }

    /** "0.35", "0.35mm" or "0.35mmISO": the nearest of OpenCAD's line widths. */
    bool parseWidth(const QString& text, RS2::LineWidth& out) {
        QString t = text.trimmed().toLower();
        t.remove("iso").remove("mm");
        bool ok = false;
        const double mm = t.toDouble(&ok);
        if (!ok || mm < 0 || mm > 2.2) {
            return false;
        }
        static const int widths[] = {0, 5, 9, 13, 15, 18, 20, 25, 30, 35, 40, 50, 53, 60, 70, 80, 90, 100, 106, 120, 140, 158, 200, 211};
        int best = 0;
        for (const int w : widths) {
            if (std::abs(w - mm * 100) < std::abs(best - mm * 100)) {
                best = w;
            }
        }
        out = static_cast<RS2::LineWidth>(best);
        return true;
    }

    bool parseLineType(const QString& text, RS2::LineType& out) {
        const QString t = text.trimmed();
        if (t.compare("continuous", Qt::CaseInsensitive) == 0 || t.compare("solid", Qt::CaseInsensitive) == 0) {
            out = RS2::SolidLine;
            return true;
        }
        if (!LC_LineTypeNames::isKnownName(t) && !LC_LineTypeNames::isKnownName(t.toUpper())) {
            return false;
        }
        out = LC_LineTypeNames::nameToLineType(LC_LineTypeNames::isKnownName(t) ? t : t.toUpper());
        return out != RS2::LineByLayer && out != RS2::LineByBlock;
    }

    /** Sets look and switches from args on a copy of the layer; false with an error when a value cannot be read. */
    bool applySettings(const QJsonObject& args, RS_Layer& layer, QStringList& did, QString& error) {
        RS_Pen pen = layer.getPen();
        if (args.contains("color")) {
            RS_Color c;
            if (!parseColor(args.value("color").toString(), c)) {
                error = QStringLiteral("Cannot read the colour \"%1\" (use a name such as red, cyan, gray, or #rrggbb).")
                            .arg(args.value("color").toString());
                return false;
            }
            pen.setColor(c);
            did << "colour " + c.toQColor().name();
        }
        if (args.contains("width")) {
            RS2::LineWidth w;
            if (!parseWidth(args.value("width").toVariant().toString(), w)) {
                error = QStringLiteral("Cannot read the width \"%1\" (millimetres, e.g. 0.35).").arg(args.value("width").toVariant().toString());
                return false;
            }
            pen.setWidth(w);
            did << QStringLiteral("width %1 mm").arg(w / 100.0, 0, 'f', 2);
        }
        if (args.contains("linetype")) {
            RS2::LineType type;
            if (!parseLineType(args.value("linetype").toString(), type)) {
                error = QStringLiteral("Cannot read the line type \"%1\" (Continuous, DASHED, DOT, CENTER, HIDDEN, DASHDOT, PHANTOM, "
                                       "BORDER, DIVIDE...).").arg(args.value("linetype").toString());
                return false;
            }
            pen.setLineType(type);
            did << "line type " + lineTypeName(type);
        }
        layer.setPen(pen);
        if (args.contains("visible")) {
            layer.freeze(!args.value("visible").toBool());
            did << (args.value("visible").toBool() ? "visible" : "hidden");
        }
        if (args.contains("locked")) {
            layer.lock(args.value("locked").toBool());
            did << (args.value("locked").toBool() ? "locked" : "unlocked");
        }
        if (args.contains("printed")) {
            layer.setPrint(args.value("printed").toBool());
            did << (args.value("printed").toBool() ? "printed" : "not printed");
        }
        if (args.contains("construction")) {
            layer.setConstruction(args.value("construction").toBool());
            did << (args.value("construction").toBool() ? "construction layer" : "not construction");
        }
        return true;
    }

    /** Moves entities to a layer as one undo step (each is replaced by a copy on the new layer, as edits are). */
    int moveEntities(const Drawing& d, const QList<RS_Entity*>& entities, RS_Layer* to) {
        if (entities.isEmpty()) {
            return 0;
        }
        int moved = 0;
        d.document->undoableModify(d.view->getViewPort(), [&](LC_DocumentModificationBatch& ctx) -> bool {
            ctx.setActiveLayerAndPen(false, false); // the copies keep the layer set here, not the current one
            for (RS_Entity* e : entities) {
                RS_Entity* copy = e->clone();
                copy->setLayer(to);
                copy->clearSelectionFlag();
                ctx -= e;
                ctx += copy;
                ++moved;
            }
            return true;
        });
        return moved;
    }

    LC_AITool layersTool() {
        LC_AITool t;
        t.name = "layers";
        t.label = "Layers";
        t.description =
            "Work with the drawing's layers by name. action list (default) shows every layer with its entity count, look and "
            "switches. create {name, color, width, linetype, ...} adds a layer and makes it current (current: false keeps the "
            "current one). switch {name} makes a layer current (new entities go on it; use this, not cnlayer, which creates a "
            "layer). rename {name, new_name}. set {name, color, width, linetype, visible, locked, printed, construction}. move "
            "{to, from or ids} moves entities to a layer as one undo step. remove {names} removes layers; a layer with "
            "entities needs entities: \"move:<layer>\" (moved first, undoable) or \"delete\" (deleted with it; the removal "
            "itself cannot be undone). Layer 0 always stays. Names match exactly, else ignoring case when unambiguous.";
        t.parameters = QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"action", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"list", "create", "switch", "rename", "set", "move", "remove"}},
                                       {"description", "What to do (default list)"}}},
                {"name", str("The layer (create, switch, rename, set)")},
                {"names", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}}, {"description", "Layers to remove"}}},
                {"new_name", str("rename: the new name")},
                {"color", str("A colour name (red, cyan, gray...) or #rrggbb")},
                {"width", str("Line width in mm, e.g. 0.35")},
                {"linetype", str("Continuous, DASHED, DOT, CENTER, HIDDEN, DASHDOT, PHANTOM, BORDER, DIVIDE...")},
                {"visible", flag("Shown (false hides it)")},
                {"locked", flag("Locked against changes")},
                {"printed", flag("Printed")},
                {"construction", flag("A construction layer (shown, never printed)")},
                {"current", flag("create: make the new layer current (default true)")},
                {"to", str("move: the layer to move the entities to")},
                {"from", str("move: every entity of this layer")},
                {"ids", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "integer"}}}, {"description", "move: these entities"}}},
                {"entities", str("remove: what happens to a layer's entities: \"move:<layer>\" or \"delete\"")},
            }}};
        t.execute = [](const QJsonObject& args) -> LC_AIToolResult {
            const Drawing d = drawing();
            if (d.graphic == nullptr || d.document == nullptr || d.view->getViewPort() == nullptr) {
                return LC_AIToolResult::error(QStringLiteral("No drawing is open in OpenCAD."));
            }
            const QString action = args.value("action").toString("list").trimmed().toLower();
            QString error;
            if (action == "list" || action.isEmpty()) {
                return LC_AIToolResult::text(listing(d.graphic));
            }
            if (action == "create") {
                const QString name = args.value("name").toString().trimmed();
                if (name.isEmpty()) {
                    return LC_AIToolResult::error(QStringLiteral("Give the new layer's name."));
                }
                for (RS_Layer* l : layersOf(d.graphic)) {
                    if (l->getName().compare(name, Qt::CaseInsensitive) == 0) {
                        return LC_AIToolResult::error(QStringLiteral("A layer \"%1\" exists already; use switch, set or rename.").arg(l->getName()));
                    }
                }
                auto* layer = new RS_Layer(name);
                QStringList did;
                if (!applySettings(args, *layer, did, error)) {
                    delete layer;
                    return LC_AIToolResult::error(error);
                }
                d.graphic->addLayer(layer);
                if (args.value("current").toBool(true)) {
                    d.graphic->activateLayer(layer, true);
                }
                d.view->redraw();
                return LC_AIToolResult::text(QStringLiteral("Created layer %1%2.\n%3")
                                                 .arg(describe(d.graphic, layer), did.isEmpty() ? QString() : " (" + did.join(", ") + ")",
                                                      listing(d.graphic)));
            }
            if (action == "move") {
                RS_Layer* to = findLayer(d.graphic, args.value("to").toString(), error);
                if (to == nullptr) {
                    return LC_AIToolResult::error(error);
                }
                QList<RS_Entity*> entities;
                if (args.contains("from")) {
                    RS_Layer* from = findLayer(d.graphic, args.value("from").toString(), error);
                    if (from == nullptr) {
                        return LC_AIToolResult::error(error);
                    }
                    entities = entitiesOn(d.graphic, from);
                }
                else {
                    QHash<unsigned long long, RS_Entity*> byId;
                    for (RS_Entity* e : *d.graphic) {
                        if (e != nullptr && !e->isDeleted()) {
                            byId.insert(e->getId(), e);
                        }
                    }
                    QStringList missing;
                    for (const QJsonValue& v : args.value("ids").toArray()) {
                        if (RS_Entity* e = byId.value(static_cast<unsigned long long>(v.toDouble()))) {
                            entities << e;
                        }
                        else {
                            missing << QString::number(v.toDouble(), 'f', 0);
                        }
                    }
                    if (!missing.isEmpty()) {
                        return LC_AIToolResult::error(QStringLiteral("No entities #%1.").arg(missing.join(", #")));
                    }
                }
                if (entities.isEmpty()) {
                    return LC_AIToolResult::error(QStringLiteral("Nothing to move: give from (a layer with entities) or ids."));
                }
                if (to->isLocked()) {
                    return LC_AIToolResult::error(QStringLiteral("Layer \"%1\" is locked.").arg(to->getName()));
                }
                const int moved = moveEntities(d, entities, to);
                d.view->redraw();
                return LC_AIToolResult::text(QStringLiteral("Moved %1 entities to layer \"%2\" as one undo step (each is now a copy with "
                                                            "a new id).\n%3").arg(moved).arg(to->getName(), listing(d.graphic)));
            }
            if (action == "remove") {
                QStringList names;
                for (const QJsonValue& v : args.value("names").toArray()) {
                    names << v.toString();
                }
                if (names.isEmpty() && args.contains("name")) {
                    names << args.value("name").toString();
                }
                if (names.isEmpty()) {
                    return LC_AIToolResult::error(QStringLiteral("Give the layers to remove in names."));
                }
                const QString entities = args.value("entities").toString().trimmed();
                RS_Layer* moveTo = nullptr;
                if (entities.startsWith("move:", Qt::CaseInsensitive)) {
                    moveTo = findLayer(d.graphic, entities.mid(5), error);
                    if (moveTo == nullptr) {
                        return LC_AIToolResult::error(error);
                    }
                }
                // check them all first: nothing is removed when one of them cannot be
                QList<RS_Layer*> layers;
                for (const QString& n : names) {
                    RS_Layer* l = findLayer(d.graphic, n, error);
                    if (l == nullptr) {
                        return LC_AIToolResult::error(error);
                    }
                    if (l->getName() == "0") {
                        return LC_AIToolResult::error(QStringLiteral("Layer 0 can never be removed."));
                    }
                    if (l == moveTo) {
                        return LC_AIToolResult::error(QStringLiteral("Cannot move the entities of \"%1\" to itself.").arg(l->getName()));
                    }
                    const int count = int(entitiesOn(d.graphic, l).size());
                    if (count > 0 && moveTo == nullptr && entities.compare("delete", Qt::CaseInsensitive) != 0) {
                        return LC_AIToolResult::error(QStringLiteral("Layer \"%1\" has %2 entities: say entities: \"move:<layer>\" to keep them "
                                                                     "on another layer, or \"delete\" to delete them with it.")
                                                          .arg(l->getName()).arg(count));
                    }
                    if (!layers.contains(l)) {
                        layers << l;
                    }
                }
                QStringList did;
                for (RS_Layer* l : layers) {
                    if (moveTo != nullptr) {
                        const int moved = moveEntities(d, entitiesOn(d.graphic, l), moveTo);
                        if (moved > 0) {
                            did << QStringLiteral("moved %1 entities from \"%2\" to \"%3\"").arg(moved).arg(l->getName(), moveTo->getName());
                        }
                    }
                    const QString name = l->getName();
                    const int deleted = int(entitiesOn(d.graphic, l).size());
                    if (d.graphic->getActiveLayer() == l) {
                        d.graphic->activateLayer("0", true);
                    }
                    d.graphic->removeLayer(l); // deletes what is still on it (that part can be undone)
                    did << (deleted > 0 ? QStringLiteral("removed \"%1\" and deleted its %2 entities").arg(name).arg(deleted)
                                        : QStringLiteral("removed \"%1\"").arg(name));
                }
                d.view->redraw();
                return LC_AIToolResult::text(did.join("; ") + ".\n" + listing(d.graphic));
            }
            RS_Layer* layer = findLayer(d.graphic, args.value("name").toString(), error);
            if (layer == nullptr) {
                return LC_AIToolResult::error(error);
            }
            if (action == "switch") {
                if (layer->isFrozen()) {
                    RS_Layer copy(*layer);
                    copy.freeze(false);
                    d.graphic->editLayer(layer, copy); // new entities on a hidden layer would vanish
                }
                d.graphic->activateLayer(layer, true);
                return LC_AIToolResult::text(QStringLiteral("Layer \"%1\" is current now: new entities go on it.").arg(layer->getName()));
            }
            if (action == "rename") {
                const QString newName = args.value("new_name").toString().trimmed();
                if (newName.isEmpty()) {
                    return LC_AIToolResult::error(QStringLiteral("Give new_name."));
                }
                if (layer->getName() == "0") {
                    return LC_AIToolResult::error(QStringLiteral("Layer 0 cannot be renamed."));
                }
                for (RS_Layer* l : layersOf(d.graphic)) {
                    if (l != layer && l->getName() == newName) {
                        return LC_AIToolResult::error(QStringLiteral("A layer \"%1\" exists already.").arg(newName));
                    }
                }
                const QString old = layer->getName();
                RS_Layer copy(*layer);
                copy.setName(newName);
                d.graphic->editLayer(layer, copy);
                d.view->redraw();
                return LC_AIToolResult::text(QStringLiteral("Renamed \"%1\" to \"%2\".").arg(old, layer->getName()));
            }
            if (action == "set") {
                RS_Layer copy(*layer);
                QStringList did;
                if (!applySettings(args, copy, did, error)) {
                    return LC_AIToolResult::error(error);
                }
                if (did.isEmpty()) {
                    return LC_AIToolResult::error(QStringLiteral("Nothing to set: give color, width, linetype, visible, locked, printed or construction."));
                }
                if (layer->getName() == "0" && copy.isFrozen() && d.graphic->getActiveLayer() == layer) {
                    return LC_AIToolResult::error(QStringLiteral("The current layer cannot be hidden; switch to another layer first."));
                }
                d.graphic->editLayer(layer, copy);
                d.view->redraw();
                return LC_AIToolResult::text(QStringLiteral("Layer \"%1\": %2.\n%3").arg(layer->getName(), did.join(", "), describe(d.graphic, layer)));
            }
            return LC_AIToolResult::error(QStringLiteral("Unknown action \"%1\": use list, create, switch, rename, set, move or remove.").arg(action));
        };
        return t;
    }
}

QList<LC_AITool> LC_AILayers::tools() {
    return {layersTool()};
}
