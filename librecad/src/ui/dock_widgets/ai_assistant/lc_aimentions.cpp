/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aimentions.h"

#include <QAction>
#include <QHash>
#include <QMap>
#include <QMenu>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <cmath>

#include "lc_aicomposerextras.h"
#include "lc_aiicons.h"
#include "lc_aiprovidersdialog.h"
#include "lc_aitool.h"
#include "lc_graphicviewport.h"
#include "qg_graphicview.h"
#include "rs_block.h"
#include "rs_blocklist.h"
#include "rs_entity.h"
#include "rs_graphic.h"
#include "rs_insert.h"
#include "rs_layer.h"
#include "rs_layerlist.h"
#include "rs_math.h"
#include "rs_selection.h"
#include "rs_units.h"

namespace {
    constexpr int MAX_LINES = 150;           // entities described per mention; list_entities reads the rest
    constexpr int MAX_CONTEXT_CHARS = 24000; // per mention
    constexpr int MAX_ROWS = 60;             // rows of the @ popup
    constexpr int ENTITY_SCAN = 5000;        // most recent entities the popup looks through (ids are searched in all)

    using Kind = LC_AIMention::Kind;

    RS_Graphic* currentGraphic() {
        RS_GraphicView* view = LC_AITools::view();
        return view != nullptr ? view->getGraphic() : nullptr;
    }

    QString fmt(const double v) {
        if (!std::isfinite(v)) {
            return QStringLiteral("n/a");
        }
        QString s = QString::number(v, 'f', 4);
        while (s.contains('.') && (s.endsWith('0') || s.endsWith('.'))) {
            s.chop(1);
        }
        return s == "-0" ? QStringLiteral("0") : s;
    }

    QString fmt(const RS_Vector& v) {
        return QStringLiteral("(%1, %2)").arg(fmt(v.x), fmt(v.y));
    }

    QString layerName(const RS_Entity* e) {
        return e->getLayer() != nullptr ? e->getLayer()->getName() : QStringLiteral("0");
    }

    QString plural(const int n, const QString& one, const QString& many) {
        return QStringLiteral("%1 %2").arg(n).arg(n == 1 ? one : many);
    }

    /** The drawing's unit for the model: in English, and "unitless" rather than "None". */
    QString unitsOf(const RS_Graphic* graphic) {
        const RS2::Unit unit = graphic->getUnit();
        return unit == RS2::None ? QStringLiteral("unitless") : RS_Units::unitToString(unit, false);
    }

    /** Names of layers or blocks, longest first, so "Wall outlines" is found before "Wall". */
    QStringList longestFirst(QStringList names) {
        std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) { return a.size() > b.size(); });
        return names;
    }

    bool isWordChar(const QChar c) {
        return c.isLetterOrNumber() || c == '_';
    }

    QStringList words(const QString& text) {
        static const QRegularExpression separators(QStringLiteral(R"([^\p{L}\p{N}_]+)"));
        return text.toLower().split(separators, Qt::SkipEmptyParts);
    }

    /** Every typed word starts a word of the row (so "lay wal" finds "@layer Walls" and "12" finds "@entity #12"). */
    bool matches(const QStringList& terms, const QStringList& rowWords) {
        for (const QString& term : terms) {
            bool found = false;
            for (const QString& w : rowWords) {
                if (w.startsWith(term)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return false;
            }
        }
        return true;
    }

    /** Whether the open drawing has an entity with this id (no drawing: it cannot be told, so yes). */
    bool entityExists(const QString& idText) {
        RS_Graphic* graphic = currentGraphic();
        if (graphic == nullptr) {
            return true;
        }
        bool ok = false;
        const unsigned long long id = idText.toULongLong(&ok);
        if (!ok) {
            return false;
        }
        for (RS_Entity* e : *graphic) {
            if (e != nullptr && !e->isDeleted() && e->getId() == id) {
                return true;
            }
        }
        return false;
    }

    QString truncate(const QString& text) {
        return text.size() > MAX_CONTEXT_CHARS ? text.left(MAX_CONTEXT_CHARS) + QStringLiteral("\n... (cut)") : text;
    }

    QString describeAll(const QList<RS_Entity*>& entities, const int total, const QString& more) {
        QStringList lines;
        for (RS_Entity* e : entities) {
            lines << LC_AITools::describeEntity(e);
        }
        if (total > entities.size()) {
            lines << QStringLiteral("... and %1 more (%2).").arg(total - entities.size()).arg(more);
        }
        return lines.join('\n');
    }

    QString selectionContext(RS_Graphic* graphic) {
        QList<RS_Entity*> shown;
        int count = 0;
        for (RS_Entity* e : *graphic) {
            if (e != nullptr && !e->isDeleted() && e->isSelected()) {
                if (shown.size() < MAX_LINES) {
                    shown << e;
                }
                ++count;
            }
        }
        if (count == 0) {
            return QStringLiteral("Nothing is selected in the drawing.");
        }
        return QStringLiteral("%1 selected (units: %2):\n").arg(plural(count, "entity", "entities"), unitsOf(graphic))
               + describeAll(shown, count, QStringLiteral("list_entities with selected_only reads them"));
    }

    QString layerContext(RS_Graphic* graphic, const QString& name) {
        RS_Layer* layer = graphic->getLayerList()->find(name);
        if (layer == nullptr) {
            return QStringLiteral("There is no layer \"%1\" in the drawing any more.").arg(name);
        }
        QMap<QString, int> byType;
        QList<RS_Entity*> shown;
        int count = 0;
        for (RS_Entity* e : *graphic) {
            if (e == nullptr || e->isDeleted() || layerName(e) != layer->getName()) {
                continue;
            }
            byType[LC_AITools::typeName(e)] += 1;
            if (shown.size() < MAX_LINES) {
                shown << e;
            }
            ++count;
        }
        QStringList flags;
        if (layer == graphic->getLayerList()->getActive()) {
            flags << "current";
        }
        if (layer->isFrozen()) {
            flags << "hidden";
        }
        if (layer->isLocked()) {
            flags << "locked";
        }
        if (!layer->isPrint()) {
            flags << "not printed";
        }
        if (layer->isConstruction()) {
            flags << "construction";
        }
        QStringList types;
        for (auto it = byType.cbegin(); it != byType.cend(); ++it) {
            types << QStringLiteral("%1 %2").arg(it.value()).arg(it.key());
        }
        QString text = QStringLiteral("Layer \"%1\"%2: %3%4 (units: %5).")
                           .arg(layer->getName(), flags.isEmpty() ? QString() : " [" + flags.join(", ") + "]",
                                plural(count, "entity", "entities"), types.isEmpty() ? QString() : " (" + types.join(", ") + ")",
                                unitsOf(graphic));
        if (count > 0) {
            text += "\n" + describeAll(shown, count, QStringLiteral("list_entities with layer \"%1\" reads them").arg(layer->getName()));
        }
        return text;
    }

    QString blockContext(RS_Graphic* graphic, const QString& name) {
        RS_Block* block = graphic->getBlockList()->find(name);
        if (block == nullptr) {
            return QStringLiteral("There is no block \"%1\" in the drawing any more.").arg(name);
        }
        QStringList inserts;
        int insertCount = 0;
        for (RS_Entity* e : *graphic) {
            if (e != nullptr && !e->isDeleted() && e->rtti() == RS2::EntityInsert && static_cast<RS_Insert*>(e)->getName() == block->getName()) {
                if (inserts.size() < 40) {
                    inserts << QStringLiteral("#%1 at %2").arg(e->getId()).arg(fmt(static_cast<RS_Insert*>(e)->getInsertionPoint()));
                }
                ++insertCount;
            }
        }
        QList<RS_Entity*> shown;
        int count = 0;
        for (RS_Entity* e : *block) {
            if (e != nullptr && !e->isDeleted()) {
                if (shown.size() < MAX_LINES) {
                    shown << e;
                }
                ++count;
            }
        }
        QString text = QStringLiteral("Block \"%1\": base point %2, %3; inserted %4%5.")
                           .arg(block->getName(), fmt(block->getBasePoint()), plural(count, "entity", "entities"),
                                insertCount == 1 ? QStringLiteral("once") : QStringLiteral("%1 times").arg(insertCount),
                                inserts.isEmpty() ? QString() : " (" + inserts.join(", ") + (insertCount > inserts.size() ? ", ..." : "") + ")");
        if (count > 0) {
            text += "\nIts entities, in block coordinates:\n" + describeAll(shown, count, QStringLiteral("not listed"));
        }
        return text;
    }

    QString entityContext(RS_Graphic* graphic, const QString& idText) {
        bool ok = false;
        const unsigned long long id = idText.toULongLong(&ok);
        RS_Entity* found = nullptr;
        for (RS_Entity* e : *graphic) {
            if (ok && e != nullptr && !e->isDeleted() && e->getId() == id) {
                found = e;
                break;
            }
        }
        if (found == nullptr) {
            return QStringLiteral("Entity #%1 is not in the drawing (it may have been deleted).").arg(idText);
        }
        QString text = LC_AITools::describeEntity(found) + QStringLiteral(" (units: %1)").arg(unitsOf(graphic));
        if (found->rtti() == RS2::EntityInsert) {
            const auto* insert = static_cast<RS_Insert*>(found);
            text += QStringLiteral("\nAn insert of block \"%1\" at %2, scale %3 x %4, rotation %5°.")
                        .arg(insert->getName(), fmt(insert->getInsertionPoint()), fmt(insert->getScale().x), fmt(insert->getScale().y),
                             fmt(RS_Math::rad2deg(insert->getAngle())));
        }
        else if (found->rtti() == RS2::EntityPolyline) {
            QList<RS_Entity*> segments;
            int count = 0;
            for (RS_Entity* e : *static_cast<RS_EntityContainer*>(found)) {
                if (e != nullptr && !e->isDeleted()) {
                    if (segments.size() < MAX_LINES) {
                        segments << e;
                    }
                    ++count;
                }
            }
            text += "\nIts segments:\n" + describeAll(segments, count, QStringLiteral("not listed"));
        }
        return text;
    }

    QString drawingContext() {
        for (const LC_AITool& tool : LC_AITools::all()) {
            if (tool.name == "get_drawing_info" && tool.execute) {
                return tool.execute({}).joinedText();
            }
        }
        return {};
    }
}

QString LC_AIMention::text(const Kind kind, const QString& argument) {
    switch (kind) {
        case Kind::Selection:
            return QStringLiteral("@selection");
        case Kind::View:
            return QStringLiteral("@view");
        case Kind::Drawing:
            return QStringLiteral("@drawing");
        case Kind::Layer:
            return QStringLiteral("@layer ") + argument;
        case Kind::Block:
            return QStringLiteral("@block ") + argument;
        case Kind::Entity:
            return QStringLiteral("@entity #") + argument;
    }
    return {};
}

QList<LC_AIMention> LC_AIMentions::find(const QString& text) {
    QList<LC_AIMention> found;
    if (!text.contains('@')) {
        return found;
    }
    // an @ that starts a word (not the middle of an e-mail address)
    static const QRegularExpression head(QStringLiteral(R"((?<![\w@.])@(selection|view|drawing|layer|block|entity)(?![\w]))"),
                                         QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression entityId(QStringLiteral(R"([ \t]*#?(\d+)(?!\w))"));
    QStringList layers;
    QStringList blocks;
    bool namesRead = false;
    for (auto it = head.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const QString word = m.captured(1).toLower();
        LC_AIMention mention;
        mention.start = int(m.capturedStart());
        if (word == "selection" || word == "view" || word == "drawing") {
            mention.kind = word == "selection" ? Kind::Selection : word == "view" ? Kind::View : Kind::Drawing;
            mention.length = int(m.capturedLength());
            found << mention;
            continue;
        }
        if (word == "entity") {
            const QRegularExpressionMatch id = entityId.match(text, m.capturedEnd(), QRegularExpression::NormalMatch,
                                                              QRegularExpression::AnchorAtOffsetMatchOption);
            // an id that is not in the drawing stays plain text, as an unknown layer name does: the pill would promise context
            if (id.hasMatch() && entityExists(id.captured(1))) {
                mention.kind = Kind::Entity;
                mention.argument = id.captured(1);
                mention.length = int(id.capturedEnd() - mention.start);
                found << mention;
            }
            continue;
        }
        // @layer / @block and one space: the longest name of the drawing that follows
        const int nameStart = int(m.capturedEnd()) + 1;
        if (nameStart > text.size() || text.at(nameStart - 1) != ' ') {
            continue;
        }
        if (!namesRead) {
            namesRead = true;
            if (RS_Graphic* graphic = currentGraphic()) {
                for (RS_Layer* l : *graphic->getLayerList()) {
                    layers << l->getName();
                }
                for (RS_Block* b : *graphic->getBlockList()) {
                    blocks << b->getName();
                }
                layers = longestFirst(layers);
                blocks = longestFirst(blocks);
            }
        }
        for (const QString& name : word == "layer" ? layers : blocks) {
            const int end = nameStart + int(name.size());
            if (!name.isEmpty() && text.mid(nameStart, name.size()).compare(name, Qt::CaseInsensitive) == 0
                && (end >= text.size() || !isWordChar(text.at(end)) || !isWordChar(name.back()))) {
                mention.kind = word == "layer" ? Kind::Layer : Kind::Block;
                mention.argument = name;
                mention.length = end - mention.start;
                found << mention;
                break;
            }
        }
    }
    return found;
}

QList<LC_AIMentions::Candidate> LC_AIMentions::candidates(const QString& query) {
    QStringList terms = words(query);
    const bool browsing = terms.isEmpty(); // just "@": a few of each kind
    QList<Candidate> rows;
    const auto offer = [&rows, &terms](const Candidate& c, const QString& extraWords, int& budget) {
        if (budget <= 0 || rows.size() >= MAX_ROWS || !matches(terms, words(c.text + " " + extraWords))) {
            return;
        }
        rows << c;
        --budget;
    };

    RS_GraphicView* view = LC_AITools::view();
    RS_Graphic* graphic = view != nullptr ? view->getGraphic() : nullptr;
    int selected = 0;
    QHash<QString, int> perLayer;
    QHash<QString, int> insertsOf;
    if (graphic != nullptr) {
        for (RS_Entity* e : *graphic) {
            if (e == nullptr || e->isDeleted()) {
                continue;
            }
            selected += e->isSelected() ? 1 : 0;
            perLayer[layerName(e)] += 1;
            if (e->rtti() == RS2::EntityInsert) {
                insertsOf[static_cast<RS_Insert*>(e)->getName()] += 1;
            }
        }
    }

    int budget = 3;
    offer({LC_AIMention::text(Kind::Selection),
           selected > 0 ? QObject::tr("%1 selected").arg(selected) : QObject::tr("nothing selected"), "mouse-pointer-2",
           QObject::tr("Context")}, {}, budget);
    offer({LC_AIMention::text(Kind::View), QObject::tr("visible area + screenshot"), "scan", QObject::tr("Context")}, {}, budget);
    offer({LC_AIMention::text(Kind::Drawing), QObject::tr("layers, blocks, counts"), "drafting-compass", QObject::tr("Context")}, {}, budget);
    if (graphic == nullptr) {
        return rows;
    }

    budget = browsing ? 6 : 40;
    for (RS_Layer* layer : *graphic->getLayerList()) {
        QString detail = plural(perLayer.value(layer->getName()), QObject::tr("entity"), QObject::tr("entities"));
        if (layer == graphic->getLayerList()->getActive()) {
            detail += QObject::tr(" · current");
        }
        if (layer->isFrozen()) {
            detail += QObject::tr(" · hidden");
        }
        offer({LC_AIMention::text(Kind::Layer, layer->getName()), detail, "layers", QObject::tr("Layers")}, {}, budget);
    }

    budget = browsing ? 4 : 40;
    for (RS_Block* block : *graphic->getBlockList()) {
        offer({LC_AIMention::text(Kind::Block, block->getName()),
               plural(int(block->count()), QObject::tr("entity"), QObject::tr("entities")) + " · "
                   + plural(insertsOf.value(block->getName()), QObject::tr("insert"), QObject::tr("inserts")),
               "box", QObject::tr("Blocks")}, {}, budget);
    }

    // entities: the selected ones first, then the newest; an id typed in full or in part is looked for everywhere
    budget = browsing ? 5 : 40;
    QSet<unsigned long long> offered;
    const auto offerEntity = [&](RS_Entity* e) {
        if (offered.contains(e->getId())) {
            return;
        }
        const QString type = LC_AITools::typeName(e);
        QString detail = QStringLiteral("%1 · %2").arg(type, layerName(e));
        if (e->isSelected()) {
            detail += QObject::tr(" · selected");
        }
        const int before = int(rows.size());
        offer({LC_AIMention::text(Kind::Entity, QString::number(e->getId())), detail, "hash", QObject::tr("Entities")},
              type + " " + layerName(e), budget);
        if (rows.size() > before) {
            offered.insert(e->getId());
        }
    };
    const int count = graphic->count();
    for (int i = count - 1; i >= 0 && budget > 0; --i) {
        RS_Entity* e = graphic->entityAt(i);
        if (e != nullptr && !e->isDeleted() && e->isSelected()) {
            offerEntity(e);
        }
    }
    bool digits = false;
    for (const QString& t : std::as_const(terms)) {
        digits = digits || (!t.isEmpty() && t.front().isDigit());
    }
    for (int i = count - 1, scanned = 0; i >= 0 && budget > 0 && (digits || scanned < ENTITY_SCAN); --i, ++scanned) {
        RS_Entity* e = graphic->entityAt(i);
        if (e != nullptr && !e->isDeleted()) {
            offerEntity(e);
        }
    }
    return rows;
}

LC_AIMentions::Expansion LC_AIMentions::expand(const QList<LC_AIMention>& mentions) {
    Expansion out;
    RS_GraphicView* view = LC_AITools::view();
    RS_Graphic* graphic = view != nullptr ? view->getGraphic() : nullptr;
    QStringList blocks;
    QSet<QString> done;
    for (const LC_AIMention& m : mentions) {
        const QString name = m.text();
        if (done.contains(name)) {
            continue; // mentioned twice: described once
        }
        done.insert(name);
        QString body;
        if (graphic == nullptr) {
            body = QStringLiteral("No drawing is open in OpenCAD.");
        }
        else {
            switch (m.kind) {
                case Kind::Selection:
                    body = selectionContext(graphic);
                    break;
                case Kind::View: {
                    body = LC_AITools::describeView(view) + QStringLiteral("\nA screenshot of the view is attached.");
                    const LC_AIContent shot = LC_AITools::screenshot(view);
                    LC_AIAttachment a;
                    a.kind = LC_AIAttachment::Kind::Image;
                    a.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                    a.name = QStringLiteral("@view");
                    a.mimeType = shot.mimeType;
                    a.data = shot.data;
                    const QImage image = QImage::fromData(shot.data);
                    a.size = image.size();
                    a.preview = image.width() > 480 ? image.scaledToWidth(480, Qt::SmoothTransformation) : image;
                    a.fromMention = true;
                    out.attachments << a;
                    break;
                }
                case Kind::Drawing:
                    body = drawingContext();
                    break;
                case Kind::Layer:
                    body = layerContext(graphic, m.argument);
                    break;
                case Kind::Block:
                    body = blockContext(graphic, m.argument);
                    break;
                case Kind::Entity:
                    body = entityContext(graphic, m.argument);
                    break;
            }
        }
        // a layer named 6" pipes or <new> must not end the attribute or the wrapper early
        QString inside = truncate(body.trimmed());
        inside.replace(QStringLiteral("</mention>"), QStringLiteral("&lt;/mention>"), Qt::CaseInsensitive);
        blocks << QStringLiteral("<mention name=\"%1\">\n%2\n</mention>").arg(name.toHtmlEscaped(), inside);
    }
    if (!blocks.isEmpty()) {
        out.context = QStringLiteral("Context from the open drawing for the @-mentions above, read when this message was sent:\n\n")
                      + blocks.join("\n\n");
    }
    return out;
}

QString LC_AIMentions::toHtml(const QString& text, const LC_AITheme& theme) {
    const QList<LC_AIMention> mentions = find(text);
    if (mentions.isEmpty()) {
        return {};
    }
    const QColor c = theme.accentBlue;
    const QString pill = QStringLiteral("<span style=\"background-color: rgba(%1, %2, %3, %4); color: %5;\">")
                             .arg(c.red()).arg(c.green()).arg(c.blue()).arg(theme.dark ? 0.2 : 0.14).arg(c.name());
    QString html;
    int last = 0;
    for (const LC_AIMention& m : mentions) {
        html += text.mid(last, m.start - last).toHtmlEscaped();
        html += pill + text.mid(m.start, m.length).toHtmlEscaped() + QStringLiteral("</span>");
        last = m.start + m.length;
    }
    html += text.mid(last).toHtmlEscaped();
    return QStringLiteral("<p style=\"white-space: pre-wrap; margin: 0;\">") + html + QStringLiteral("</p>");
}

void LC_AIMentions::addAskAI(QMenu* menu, QG_GraphicView* view, RS_Entity* entity) {
    QWidget* window = view != nullptr ? view->window() : nullptr;
    if (menu == nullptr || window == nullptr || window->findChild<QAction*>("AIAssistantToggle") == nullptr) {
        return; // this window has no AI sidebar
    }
    auto* ask = new QAction(LC_AIIcons::lucideIcon("sparkles", 16, menu->palette().color(QPalette::Text)), QObject::tr("Ask AI about this"), menu);
    ask->setObjectName("AskAIAboutThis");
    ask->setToolTip(entity != nullptr ? QObject::tr("Select this entity and ask the AI about it")
                                      : QObject::tr("Ask the AI about what is in view"));
    menu->addAction(ask);
    menu->addSeparator();
    const bool hasEntity = entity != nullptr;
    const unsigned long long id = hasEntity ? entity->getId() : 0;
    QPointer<QG_GraphicView> guard(view);
    QObject::connect(ask, &QAction::triggered, view, [guard, hasEntity, id] {
        // after the menu has closed and taken back its temporary highlight of the entity
        QTimer::singleShot(0, guard.data(), [guard, hasEntity, id] {
            if (!guard) {
                return;
            }
            if (!hasEntity) {
                LC_AIComposerExtras::openWithMention(guard, LC_AIMention::text(Kind::View));
                return;
            }
            RS_Graphic* graphic = guard->getGraphic();
            RS_Entity* target = nullptr;
            if (graphic != nullptr) {
                for (RS_Entity* e : *graphic) {
                    if (e != nullptr && !e->isDeleted() && e->getId() == id) {
                        target = e;
                        break;
                    }
                }
            }
            if (target == nullptr) {
                return;
            }
            if (!target->isSelected()) { // a selection it belongs to stays; otherwise it becomes the selection
                RS_Selection::unselectAllInDocument(graphic, guard->getViewPort());
                RS_Selection::selectEntitiesList(graphic, guard->getViewPort(), {target}, true);
                guard->redraw(RS2::RedrawAll);
            }
            LC_AIComposerExtras::openWithMention(guard, LC_AIMention::text(Kind::Entity, QString::number(id)));
        });
    });
}
