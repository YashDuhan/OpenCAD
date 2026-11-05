/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aicheckpoints.h"

#include <QHash>
#include <algorithm>
#include <functional>
#include <iterator>
#include <utility>

#include "lc_aicontrol.h"
#include "lc_aitool.h"
#include "lc_graphicviewport.h"
#include "lc_undosection.h"
#include "rs_block.h"
#include "rs_blocklist.h"
#include "rs_document.h"
#include "rs_entity.h"
#include "rs_graphic.h"
#include "rs_graphicview.h"
#include "rs_layer.h"
#include "rs_layerlist.h"

namespace {
    constexpr int MAX_RUNS = 50; // older checkpoints are dropped; their buttons then say so

    /**
     * RS_Undo keeps its position in the undo history protected. A member pointer taken in a derived class reads
     * it without changing the core class, which nearly every file includes.
     */
    struct UndoPosition : RS_Undo {
        static int undoSteps(RS_Undo* undo) { return (undo->*(&UndoPosition::countUndoCycles))(); }
    };

    QString entities(const int n) {
        return n == 1 ? QObject::tr("1 entity") : QObject::tr("%1 entities").arg(n);
    }

    QString quotedList(const QStringList& names) {
        QStringList quoted;
        for (const QString& name : names) {
            quoted << "\"" + name + "\"";
        }
        return quoted.join(", ");
    }

    /** Whether any entity, live or kept for undo, in the drawing or in one of its blocks, is on this layer. */
    bool layerInUse(RS_Graphic* graphic, const RS_Layer* layer) {
        const std::function<bool(const RS_EntityContainer*)> uses = [&uses, layer](const RS_EntityContainer* container) {
            for (const RS_Entity* e : *container) {
                if (e != nullptr && (e->getLayer(false) == layer
                                     || (e->isContainer() && uses(static_cast<const RS_EntityContainer*>(e))))) {
                    return true;
                }
            }
            return false;
        };
        if (uses(graphic)) {
            return true;
        }
        for (const RS_Block* block : *graphic->getBlockList()) {
            if (block != nullptr && uses(block)) {
                return true;
            }
        }
        return false;
    }

    /** Every entity of the drawing by id, the deleted ones too (they stay while the undo history holds them). */
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

QString LC_AIRunChanges::summary() const {
    QStringList parts;
    if (!added.isEmpty() || !removed.isEmpty()) {
        if (removed.isEmpty()) {
            parts << QObject::tr("%1 added").arg(entities(int(added.size())));
        }
        else if (added.isEmpty()) {
            parts << QObject::tr("%1 removed").arg(entities(int(removed.size())));
        }
        else {
            parts << QObject::tr("%1 added, %2 removed").arg(entities(int(added.size()))).arg(removed.size());
        }
    }
    const auto names = [&parts](const QStringList& list, const QString& one, const QString& many) {
        if (!list.isEmpty()) {
            parts << (list.size() == 1 ? one : many).arg(quotedList(list));
        }
    };
    names(layersAdded, QObject::tr("layer %1 added"), QObject::tr("layers %1 added"));
    names(layersRemoved, QObject::tr("layer %1 removed"), QObject::tr("layers %1 removed"));
    names(blocksAdded, QObject::tr("block %1 added"), QObject::tr("blocks %1 added"));
    names(blocksRemoved, QObject::tr("block %1 removed"), QObject::tr("blocks %1 removed"));
    return parts.join("; ");
}

LC_AICheckpoints& LC_AICheckpoints::instance() {
    static LC_AICheckpoints* checkpoints = new LC_AICheckpoints(); // lives as long as the application
    return *checkpoints;
}

LC_AICheckpoints::LC_AICheckpoints() {
    // a run is what LC_AIControl marks: begin() when the chat sends, end() when the answer is complete
    connect(&LC_AIControl::instance(), &LC_AIControl::stateChanged, this, [this](const LC_AIControl::State state) {
        if (state == LC_AIControl::State::Idle) {
            end();
        }
        else if (m_current == 0) {
            begin(); // Working, the first time; Paused and back are the same run
        }
    });
}

LC_AICheckpoints::Snapshot LC_AICheckpoints::snapshot(RS_Document* document) {
    Snapshot s;
    if (document == nullptr) {
        return s;
    }
    for (RS_Entity* e : *document) {
        if (e != nullptr && !e->isDeleted()) {
            s.entities.insert(e->getId());
        }
    }
    if (RS_LayerList* layers = document->getLayerList()) {
        for (RS_Layer* layer : *layers) {
            s.layers << layer->getName();
        }
        if (layers->getActive() != nullptr) {
            s.activeLayer = layers->getActive()->getName();
        }
    }
    if (RS_BlockList* blocks = document->getBlockList()) {
        for (RS_Block* block : *blocks) {
            s.blocks << block->getName();
        }
    }
    s.undoSteps = UndoPosition::undoSteps(document);
    return s;
}

bool LC_AICheckpoints::alive(const Checkpoint& checkpoint) {
    // the document goes with its window; the view tells whether it is still open
    return checkpoint.document != nullptr && checkpoint.view && checkpoint.view->getDocument() == checkpoint.document;
}

void LC_AICheckpoints::begin() {
    const int run = m_next++;
    m_current = run;
    Checkpoint checkpoint;
    checkpoint.changes.run = run;
    RS_GraphicView* view = LC_AITools::view(); // the drawing the AI's tools act on
    checkpoint.view = view;
    checkpoint.document = view != nullptr ? view->getDocument() : nullptr;
    checkpoint.before = snapshot(checkpoint.document);
    m_runs.insert(run, checkpoint);
    while (m_runs.size() > MAX_RUNS) {
        m_runs.erase(m_runs.begin());
    }
    emit runStarted(run);
}

void LC_AICheckpoints::end() {
    const int run = std::exchange(m_current, 0);
    if (run == 0 || !m_runs.contains(run)) {
        return;
    }
    Checkpoint& checkpoint = m_runs[run];
    checkpoint.ended = true;
    if (alive(checkpoint)) {
        checkpoint.after = snapshot(checkpoint.document);
        const Snapshot& before = checkpoint.before;
        const Snapshot& after = checkpoint.after;
        LC_AIRunChanges& changes = checkpoint.changes;
        for (const unsigned long long id : after.entities) {
            if (!before.entities.contains(id)) {
                changes.added << id;
            }
        }
        for (const unsigned long long id : before.entities) {
            if (!after.entities.contains(id)) {
                changes.removed << id;
            }
        }
        std::sort(changes.added.begin(), changes.added.end());
        std::sort(changes.removed.begin(), changes.removed.end());
        for (const QString& layer : after.layers) {
            if (!before.layers.contains(layer)) {
                changes.layersAdded << layer;
            }
        }
        for (const QString& layer : before.layers) {
            if (!after.layers.contains(layer)) {
                changes.layersRemoved << layer;
            }
        }
        for (const QString& block : after.blocks) {
            if (!before.blocks.contains(block)) {
                changes.blocksAdded << block;
            }
        }
        for (const QString& block : before.blocks) {
            if (!after.blocks.contains(block)) {
                changes.blocksRemoved << block;
            }
        }
    }
    m_last = run;
    emit runRecorded(run);
}

LC_AIRunChanges LC_AICheckpoints::changesOfLastRun() const {
    return changesOf(m_last);
}

LC_AIRunChanges LC_AICheckpoints::changesOf(const int run) const {
    const auto it = m_runs.constFind(run);
    return it != m_runs.cend() && it->ended ? it->changes : LC_AIRunChanges();
}

RS_GraphicView* LC_AICheckpoints::viewOf(const int run) const {
    const auto it = m_runs.constFind(run);
    return it != m_runs.cend() && alive(*it) ? it->view.data() : nullptr;
}

// ------------------------------------------------------------------------------------------------ what a revert would do

LC_AICheckpoints::Check LC_AICheckpoints::check(const int run) const {
    Check c;
    const auto it = m_runs.constFind(run);
    if (it == m_runs.cend() || !it->ended) {
        c.text = tr("This checkpoint is no longer kept, so these changes cannot be reverted from here.");
        return c;
    }
    if (m_current != 0) {
        c.text = tr("The AI is working on the drawing. Revert when it has finished.");
        return c;
    }
    const Checkpoint& checkpoint = *it;
    if (!alive(checkpoint)) {
        c.text = tr("The drawing these changes were made in is closed.");
        return c;
    }
    const Snapshot now = snapshot(checkpoint.document);
    if (now.undoSteps == checkpoint.after.undoSteps && now.entities == checkpoint.after.entities && !checkpoint.stepsBroken
        && checkpoint.after.undoSteps >= checkpoint.before.undoSteps) {
        c.situation = Situation::Unchanged;
        c.text = tr("Undo back to before this answer: %1.").arg(checkpoint.changes.summary());
        return c;
    }
    // only the run's own changes: what it drew that is still there, what it deleted that can come back
    const QHash<unsigned long long, RS_Entity*> all = entitiesById(checkpoint.document);
    int gone = 0;
    int lost = 0;
    for (const unsigned long long id : checkpoint.changes.added) {
        if (now.entities.contains(id)) {
            ++c.toRemove;
        }
        else {
            ++gone;
        }
    }
    for (const unsigned long long id : checkpoint.changes.removed) {
        if (!all.contains(id)) {
            ++lost; // its undo step was dropped (undone, then something new was done)
        }
        else if (!now.entities.contains(id)) {
            ++c.toRestore;
        }
    }
    if (c.toRemove + c.toRestore == 0) {
        c.situation = Situation::NothingLeft;
        c.text = tr("Nothing of these changes is left to revert: they were undone or deleted after this answer.");
        return c;
    }
    c.situation = Situation::ChangedSince;
    QStringList lines;
    lines << changedSinceText(run, checkpoint, now);
    QString own;
    if (c.toRemove > 0 && c.toRestore > 0) {
        own = tr("delete the %1 it drew that are still there and bring back the %2 it deleted")
                  .arg(entities(c.toRemove), entities(c.toRestore));
    }
    else if (c.toRemove > 0) {
        own = tr("delete the %1 it drew that are still there").arg(entities(c.toRemove));
    }
    else {
        own = tr("bring back the %1 it deleted").arg(entities(c.toRestore));
    }
    lines << tr("You can revert only this answer's own changes instead: %1, as one undo step. Later changes stay.").arg(own);
    if (gone > 0) {
        lines << (gone == 1 ? tr("1 entity it drew was changed or deleted since; it stays as it is.")
                            : tr("%1 it drew were changed or deleted since; they stay as they are.").arg(entities(gone)));
    }
    if (lost > 0) {
        lines << tr("%1 it deleted can no longer be brought back.").arg(entities(lost));
    }
    c.text = lines.join("\n\n");
    return c;
}

QString LC_AICheckpoints::changedSinceText(const int run, const Checkpoint& checkpoint, const Snapshot& now) const {
    if (checkpoint.stepsBroken || checkpoint.after.undoSteps < checkpoint.before.undoSteps) {
        return tr("These changes cannot be undone step by step: the undo history was changed while the AI worked.");
    }
    int addedSince = 0;
    int removedSince = 0;
    for (const unsigned long long id : now.entities) {
        addedSince += checkpoint.after.entities.contains(id) ? 0 : 1;
    }
    for (const unsigned long long id : checkpoint.after.entities) {
        removedSince += now.entities.contains(id) ? 0 : 1;
    }
    bool laterRun = false; // a later answer of the AI changed the same drawing
    for (auto it = std::next(m_runs.constFind(run)); it != m_runs.cend(); ++it) {
        laterRun = laterRun || (it->ended && it->document == checkpoint.document && !it->changes.isEmpty());
    }
    if (addedSince == 0 && now.undoSteps < checkpoint.after.undoSteps && now.undoSteps >= checkpoint.before.undoSteps) {
        return tr("Part of this answer's changes was undone already.");
    }
    QString what;
    if (addedSince > 0 && removedSince > 0) {
        what = tr("%1 added and %2 removed since").arg(entities(addedSince)).arg(removedSince);
    }
    else if (addedSince > 0) {
        what = tr("%1 added since").arg(entities(addedSince));
    }
    else if (removedSince > 0) {
        what = tr("%1 removed or undone since").arg(entities(removedSince));
    }
    const QString who = laterRun ? tr("by you or a later AI answer") : tr("by you");
    const QString head = what.isEmpty() ? tr("The drawing's undo history changed after this answer (%1).").arg(who)
                                        : tr("The drawing changed after this answer: %1 (%2).").arg(what, who);
    return head + " " + tr("Undoing back to before it would undo those changes too.");
}

QString LC_AICheckpoints::leftOverText(const Checkpoint& checkpoint) const {
    // layers and blocks are not in LibreCAD's undo history: new layers with nothing on them went, the others stay
    const LC_AIRunChanges& changes = checkpoint.changes;
    QStringList layersLeft = changes.layersAdded;
    for (const auto& layer : checkpoint.removedLayers) {
        layersLeft.removeAll(layer->getName());
    }
    QStringList lines;
    if (!layersLeft.isEmpty()) {
        lines << (layersLeft.size() == 1
                      ? tr("The layer %1 it added stays (layers are not part of undo); delete it in the Layer List if you do not need it.")
                      : tr("The layers %1 it added stay (layers are not part of undo); delete them in the Layer List if you do not need them."))
                     .arg(quotedList(layersLeft));
    }
    if (!changes.blocksAdded.isEmpty()) {
        lines << (changes.blocksAdded.size() == 1 ? tr("The block %1 it added stays in the Block List.")
                                                  : tr("The blocks %1 it added stay in the Block List."))
                     .arg(quotedList(changes.blocksAdded));
    }
    if (!changes.layersRemoved.isEmpty() || !changes.blocksRemoved.isEmpty()) {
        lines << tr("Layers and blocks it removed do not come back (they are not part of undo).");
    }
    return lines.join(' ');
}

// ------------------------------------------------------------------------------------------------ revert and restore

void LC_AICheckpoints::refresh(const Checkpoint& checkpoint) {
    // as the Undo button does after its undo
    if (RS_Graphic* graphic = checkpoint.document->getGraphic()) {
        graphic->addBlockNotification();
        graphic->setModified(true);
    }
    checkpoint.document->updateInserts();
    checkpoint.view->redraw(RS2::RedrawDrawing);
}

void LC_AICheckpoints::activateLayer(RS_Document* document, const QString& from, const QString& to) {
    // the AI's choice of current layer goes back too, unless the user has picked another one since
    RS_Graphic* graphic = document->getGraphic();
    RS_Layer* active = graphic != nullptr ? graphic->getActiveLayer() : nullptr;
    if (active == nullptr || from == to || active->getName() != from || graphic->findLayer(to) == nullptr) {
        return;
    }
    graphic->activateLayer(to, true);
}

void LC_AICheckpoints::removeEmptyLayers(Checkpoint& checkpoint) {
    // a layer the run added can go when nothing at all is on it, not even entities kept for undo or redo:
    // then no undo step can need it again (Restore puts it back)
    checkpoint.removedLayers.clear();
    RS_Graphic* graphic = checkpoint.document->getGraphic();
    if (graphic == nullptr) {
        return;
    }
    for (const QString& name : std::as_const(checkpoint.changes.layersAdded)) {
        RS_Layer* layer = graphic->findLayer(name);
        if (layer != nullptr && name != "0" && !layerInUse(graphic, layer)) {
            checkpoint.removedLayers << std::shared_ptr<RS_Layer>(layer->clone());
            graphic->removeLayer(layer);
        }
    }
}

void LC_AICheckpoints::addRemovedLayers(Checkpoint& checkpoint) {
    RS_Graphic* graphic = checkpoint.document->getGraphic();
    for (const auto& layer : std::as_const(checkpoint.removedLayers)) {
        if (graphic != nullptr && graphic->findLayer(layer->getName()) == nullptr) {
            graphic->addLayer(layer->clone());
        }
    }
    checkpoint.removedLayers.clear();
}

bool LC_AICheckpoints::revert(const int run, QString& message) {
    const Check c = check(run);
    if (c.situation != Situation::Unchanged) {
        message = c.text;
        return false;
    }
    Checkpoint& checkpoint = m_runs[run];
    RS_Document* document = checkpoint.document;
    checkpoint.view->killAllActions(); // a command still waiting for points keeps its own undo steps
    const Snapshot before = snapshot(document);
    const int steps = checkpoint.after.undoSteps - checkpoint.before.undoSteps;
    int undone = 0;
    while (undone < steps && document->undo()) {
        ++undone;
    }
    if (snapshot(document).entities != checkpoint.before.entities) {
        // the history under the run was changed while it worked (it undid past its start): put everything back
        for (int i = 0; i < undone; ++i) {
            document->redo();
        }
        refresh(checkpoint);
        checkpoint.stepsBroken = true;
        message = tr("Undoing back to the checkpoint did not give the drawing as it was before this answer, so the "
                     "drawing was left as it is.");
        return false;
    }
    activateLayer(document, checkpoint.after.activeLayer, checkpoint.before.activeLayer);
    removeEmptyLayers(checkpoint);
    refresh(checkpoint);
    checkpoint.reverted = Reverted::Undone;
    checkpoint.beforeRevert = before;
    checkpoint.afterRevert = snapshot(document);
    message = leftOverText(checkpoint);
    emit runReverted(run, true);
    return true;
}

bool LC_AICheckpoints::revertOwnChanges(const int run, QString& message) {
    const Check c = check(run);
    if (c.situation != Situation::ChangedSince && c.situation != Situation::Unchanged) {
        message = c.text;
        return false;
    }
    Checkpoint& checkpoint = m_runs[run];
    RS_Document* document = checkpoint.document;
    RS_GraphicView* view = checkpoint.view;
    const Snapshot before = snapshot(document);
    const QHash<unsigned long long, RS_Entity*> all = entitiesById(document);
    QList<RS_Entity*> toDelete;
    QList<RS_Entity*> toRestore;
    for (const unsigned long long id : checkpoint.changes.added) {
        if (before.entities.contains(id)) {
            toDelete << all.value(id);
        }
    }
    for (const unsigned long long id : checkpoint.changes.removed) {
        if (!before.entities.contains(id) && all.contains(id)) {
            toRestore << all.value(id); // deleted, but still kept for undo: the same entity comes back
        }
    }
    view->killAllActions();
    {
        const LC_UndoSection section(document, view->getViewPort());
        section.undoableExecute([&toDelete](LC_DocumentModificationBatch& ctx) {
            ctx.remove(toDelete); // entities on locked layers stay, as with any delete
            return true;
        });
        for (RS_Entity* e : std::as_const(toRestore)) {
            e->markLive();
            section.addUndoable(e); // undo deletes it again
        }
    }
    const Snapshot after = snapshot(document);
    int kept = 0;
    for (const RS_Entity* e : std::as_const(toDelete)) {
        kept += after.entities.contains(e->getId()) ? 1 : 0;
    }
    if (after.entities == before.entities) {
        refresh(checkpoint);
        message = tr("Nothing could be reverted: the entities are on locked layers.");
        return false;
    }
    activateLayer(document, checkpoint.after.activeLayer, checkpoint.before.activeLayer);
    removeEmptyLayers(checkpoint);
    refresh(checkpoint);
    checkpoint.reverted = Reverted::OwnStep;
    checkpoint.beforeRevert = before;
    checkpoint.afterRevert = snapshot(document);
    QStringList lines;
    lines << tr("Reverted as one undo step; later changes stayed.");
    if (kept > 0) {
        lines << tr("%1 on locked layers stayed.").arg(entities(kept));
    }
    if (const QString leftOver = leftOverText(checkpoint); !leftOver.isEmpty()) {
        lines << leftOver;
    }
    message = lines.join(' ');
    emit runReverted(run, true);
    return true;
}

bool LC_AICheckpoints::canRestore(const int run) const {
    const auto it = m_runs.constFind(run);
    if (it == m_runs.cend() || it->reverted == Reverted::No || m_current != 0 || !alive(*it)) {
        return false;
    }
    const Snapshot now = snapshot(it->document);
    return now.undoSteps == it->afterRevert.undoSteps && now.entities == it->afterRevert.entities;
}

bool LC_AICheckpoints::restore(const int run, QString& message) {
    if (!canRestore(run)) {
        message = tr("The drawing changed after the revert, so the changes cannot be restored from here. Undo and "
                     "Redo in the Edit menu still can.");
        return false;
    }
    Checkpoint& checkpoint = m_runs[run];
    RS_Document* document = checkpoint.document;
    checkpoint.view->killAllActions();
    const bool layersTakenOut = !checkpoint.removedLayers.isEmpty();
    addRemovedLayers(checkpoint);
    // the revert was either undo steps (redo them) or one step of its own (undo it)
    const bool undone = checkpoint.reverted == Reverted::Undone;
    const int steps = undone ? checkpoint.beforeRevert.undoSteps - checkpoint.afterRevert.undoSteps : 1;
    int done = 0;
    while (done < steps && (undone ? document->redo() : document->undo())) {
        ++done;
    }
    if (snapshot(document).entities != checkpoint.beforeRevert.entities) {
        for (int i = 0; i < done; ++i) {
            undone ? document->undo() : document->redo();
        }
        if (layersTakenOut) {
            removeEmptyLayers(checkpoint);
        }
        refresh(checkpoint);
        message = tr("The changes could not be restored exactly, so the drawing was left as it is.");
        return false;
    }
    activateLayer(document, checkpoint.afterRevert.activeLayer, checkpoint.beforeRevert.activeLayer);
    refresh(checkpoint);
    checkpoint.reverted = Reverted::No;
    message.clear();
    emit runReverted(run, false);
    return true;
}
