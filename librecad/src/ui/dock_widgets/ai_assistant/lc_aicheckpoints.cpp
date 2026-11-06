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

#include <QFileInfo>
#include <QHash>
#include <QMdiSubWindow>
#include <QTimer>
#include <algorithm>
#include <functional>
#include <iterator>
#include <utility>

#include "lc_aicontrol.h"
#include "lc_aitool.h"
#include "lc_graphicviewport.h"
#include "lc_undosection.h"
#include "rs_actioninterface.h"
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

    /** The drawing's file name, or its window's title while it is not saved. */
    QString drawingName(RS_GraphicView* view) {
        const RS_Graphic* graphic = view != nullptr ? view->getGraphic() : nullptr;
        if (graphic != nullptr && !graphic->getFilename().isEmpty()) {
            return QFileInfo(graphic->getFilename()).fileName();
        }
        for (const QWidget* w = view; w != nullptr; w = w->parentWidget()) {
            if (qobject_cast<const QMdiSubWindow*>(w) != nullptr) {
                QString title = w->windowTitle();
                title.remove(QStringLiteral("[*]"));
                if (!title.trimmed().isEmpty()) {
                    return title.trimmed();
                }
                break;
            }
        }
        return QObject::tr("Unsaved drawing");
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

/** Listens to the undo history of the drawings with checkpoints: every edit, Undo and Redo goes through it. */
class LC_AICheckpoints::Watch : public LC_DocumentModificationListener {
public:
    explicit Watch(LC_AICheckpoints* owner) : m_owner(owner) {}
    void graphicModified(const RS_Graphic*, bool) override {}
    void undoStateChanged(const RS_Document*, bool, bool) override { m_owner->scheduleUpdate(); }

private:
    LC_AICheckpoints* m_owner;
};

LC_AICheckpoints& LC_AICheckpoints::instance() {
    static LC_AICheckpoints* checkpoints = new LC_AICheckpoints(); // lives as long as the application
    return *checkpoints;
}

LC_AICheckpoints::LC_AICheckpoints() {
    m_watch = new Watch(this); // lives as long as the application, as the drawings may still call it
    // a run is what LC_AIControl marks: begin() when the chat sends, end() when the answer is complete
    connect(&LC_AIControl::instance(), &LC_AIControl::stateChanged, this, [this](const LC_AIControl::State state) {
        if (state == LC_AIControl::State::Idle) {
            end();
        }
        else if (m_current == 0) {
            begin(); // Working, the first time; Paused and back are the same run
        }
    });
    // the AI opened another drawing or switched to it: what it does there is recorded too
    connect(&LC_AIControl::instance(), &LC_AIControl::runViewChanged, this, &LC_AICheckpoints::addDrawing);
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
    start(run, run, LC_AITools::view()); // the drawing the AI's tools act on
    emit runStarted(run);
}

void LC_AICheckpoints::addDrawing(RS_GraphicView* view) {
    if (m_current == 0 || view == nullptr || view->getDocument() == nullptr) {
        return;
    }
    for (const Checkpoint& checkpoint : std::as_const(m_runs)) {
        if (checkpoint.group == m_current && alive(checkpoint) && checkpoint.document == view->getDocument()) {
            return; // back to a drawing of this run: its checkpoint holds
        }
    }
    start(m_next++, m_current, view);
}

void LC_AICheckpoints::start(const int run, const int group, RS_GraphicView* view) {
    Checkpoint checkpoint;
    checkpoint.changes.run = run;
    checkpoint.group = group;
    checkpoint.view = view;
    checkpoint.document = view != nullptr ? view->getDocument() : nullptr;
    checkpoint.before = snapshot(checkpoint.document);
    m_runs.insert(run, checkpoint);
    while (m_runs.size() > MAX_RUNS) {
        m_runs.erase(m_runs.begin());
    }
    RS_Document* document = checkpoint.document;
    if (document != nullptr && !m_watched.contains(document)) {
        document->setModificationListener(m_watch, true);
        m_watched << document;
        // the document goes with its view; another one may get its address later
        connect(view, &QObject::destroyed, this, [this, document] { m_watched.removeAll(document); });
    }
}

void LC_AICheckpoints::end() {
    const int run = std::exchange(m_current, 0);
    if (run == 0) {
        return;
    }
    // one checkpoint per drawing the run worked in, in the order it came to them
    QList<int> parts;
    for (auto it = m_runs.begin(); it != m_runs.end(); ++it) {
        if (it->group == run) {
            record(it.value());
            parts << it.key();
        }
    }
    for (const int part : std::as_const(parts)) {
        m_last = part;
        emit runRecorded(part);
    }
}

void LC_AICheckpoints::record(Checkpoint& checkpoint) {
    checkpoint.ended = true;
    checkpoint.endedAt = ++m_clock;
    if (alive(checkpoint)) {
        checkpoint.name = drawingName(checkpoint.view);
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

QString LC_AICheckpoints::drawingLabelOf(const int run) const {
    const auto it = m_runs.constFind(run);
    if (it == m_runs.cend() || !it->ended) {
        return {};
    }
    int changed = 0;
    for (const Checkpoint& checkpoint : m_runs) {
        changed += checkpoint.group == it->group && !checkpoint.changes.isEmpty() ? 1 : 0;
    }
    return changed > 1 ? it->name : QString();
}

bool LC_AICheckpoints::isKept(const int run) const {
    return m_runs.contains(run);
}

bool LC_AICheckpoints::isReverted(const int run) const {
    const auto it = m_runs.constFind(run);
    return it != m_runs.cend() && it->reverted != Reverted::No;
}

int LC_AICheckpoints::revertedCount(const int run) const {
    const auto it = m_runs.constFind(run);
    return it != m_runs.cend() && it->reverted != Reverted::No ? it->revertedCount : 0;
}

QString LC_AICheckpoints::revertMessage(const int run) const {
    const auto it = m_runs.constFind(run);
    return it != m_runs.cend() && it->reverted != Reverted::No ? it->revertMessage : QString();
}

// ------------------------------------------------------------------------------------------------ changes after a revert

void LC_AICheckpoints::scheduleUpdate() {
    // the undo history tells before the entities have changed: look once the edit, Undo or Redo is done
    if (!m_updateQueued) {
        m_updateQueued = true;
        QTimer::singleShot(0, this, [this] {
            m_updateQueued = false;
            update();
        });
    }
}

void LC_AICheckpoints::update() {
    QList<int> back;
    if (m_current == 0) {
        for (auto it = m_runs.begin(); it != m_runs.end(); ++it) {
            Checkpoint& checkpoint = it.value();
            if (checkpoint.reverted == Reverted::No || !alive(checkpoint)) {
                continue;
            }
            // Undo of the revert's own step, or Redo of the steps it undid: the changes are back as they were
            // (a revert that only took out a layer leaves the history as it was: nothing tells it came back)
            const Snapshot& from = checkpoint.beforeRevert;
            if (from.undoSteps == checkpoint.afterRevert.undoSteps && from.entities == checkpoint.afterRevert.entities) {
                continue;
            }
            const Snapshot now = snapshot(checkpoint.document);
            if (now.undoSteps == from.undoSteps && now.entities == from.entities) {
                addRemovedLayers(checkpoint);
                checkpoint.reverted = Reverted::No;
                back << it.key();
            }
        }
    }
    for (const int run : std::as_const(back)) {
        emit runReverted(run, false);
    }
    emit drawingChanged();
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
    const QString drawn = c.toRemove == 1 ? tr("delete the 1 entity it drew that is still there")
                                          : tr("delete the %1 it drew that are still there").arg(entities(c.toRemove));
    if (c.toRemove > 0 && c.toRestore > 0) {
        own = tr("%1 and bring back the %2 it deleted").arg(drawn, entities(c.toRestore));
    }
    else if (c.toRemove > 0) {
        own = drawn;
    }
    else {
        own = tr("bring back the %1 it deleted").arg(entities(c.toRestore));
    }
    // "later changes stay" only when there are any (an undo history the AI changed is not a later change)
    lines << (now.entities != checkpoint.after.entities
                  ? tr("You can revert only this answer's own changes instead: %1, as one undo step. Later changes stay.")
                  : tr("You can revert only this answer's own changes instead: %1, as one undo step."))
                 .arg(own);
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
    bool otherReverted = false; // reverting another answer since changes the drawing too; it is not the user's doing
    for (auto it = m_runs.cbegin(); it != m_runs.cend(); ++it) {
        otherReverted = otherReverted || (it.key() != run && it->document == checkpoint.document && it->reverted != Reverted::No
                                          && it->revertedAt > checkpoint.endedAt);
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
    const QString who = laterRun && otherReverted ? tr("by you, a later AI answer or reverting another answer")
                        : laterRun                ? tr("by you or a later AI answer")
                        : otherReverted           ? tr("by you or by reverting another answer")
                                                  : tr("by you");
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
    const QString ended = endCommand(checkpoint.view); // a command still waiting for points keeps its own undo steps
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
    checkpoint.revertedAt = ++m_clock;
    checkpoint.beforeRevert = before;
    checkpoint.afterRevert = snapshot(document);
    checkpoint.revertedCount = int(checkpoint.changes.added.size() + checkpoint.changes.removed.size() + checkpoint.removedLayers.size());
    QStringList lines;
    if (!ended.isEmpty()) {
        lines << ended;
    }
    if (const QString leftOver = leftOverText(checkpoint); !leftOver.isEmpty()) {
        lines << leftOver;
    }
    message = lines.join(' ');
    checkpoint.revertMessage = message;
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
    int gone = 0; // what it drew that was changed or deleted since
    int lost = 0; // what it deleted whose undo step was dropped: it cannot come back
    for (const unsigned long long id : checkpoint.changes.added) {
        if (before.entities.contains(id)) {
            toDelete << all.value(id);
        }
        else {
            ++gone;
        }
    }
    for (const unsigned long long id : checkpoint.changes.removed) {
        if (!all.contains(id)) {
            ++lost;
        }
        else if (!before.entities.contains(id)) {
            toRestore << all.value(id); // deleted, but still kept for undo: the same entity comes back
        }
    }
    const QString ended = endCommand(view);
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
    checkpoint.revertedAt = ++m_clock;
    checkpoint.beforeRevert = before;
    checkpoint.afterRevert = snapshot(document);
    checkpoint.revertedCount = int(toDelete.size()) - kept + int(toRestore.size()) + int(checkpoint.removedLayers.size());
    // say what did not come back, and that later changes stayed only when there were any
    QStringList lines;
    if (!ended.isEmpty()) {
        lines << ended;
    }
    lines << (before.entities != checkpoint.after.entities ? tr("Reverted as one undo step; changes made after this answer stayed.")
                                                           : tr("Reverted as one undo step."));
    if (kept > 0) {
        lines << tr("%1 on locked layers stayed.").arg(entities(kept));
    }
    if (gone > 0) {
        lines << (gone == 1 ? tr("1 entity it drew had been changed or deleted since and stays as it is.")
                            : tr("%1 it drew had been changed or deleted since and stay as they are.").arg(entities(gone)));
    }
    if (lost > 0) {
        lines << (lost == 1 ? tr("1 entity it deleted could not be brought back: its undo step is gone.")
                            : tr("%1 it deleted could not be brought back: their undo step is gone.").arg(entities(lost)));
    }
    if (const QString leftOver = leftOverText(checkpoint); !leftOver.isEmpty()) {
        lines << leftOver;
    }
    message = lines.join(' ');
    checkpoint.revertMessage = message;
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
    const QString ended = endCommand(checkpoint.view);
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
    message = ended;
    emit runReverted(run, false);
    return true;
}

QString LC_AICheckpoints::endCommand(RS_GraphicView* view) {
    // undo and redo under a command that waits for points would leave it with stale ones: it ends first, and says so
    // when it had some input already (a command still waiting for its first point loses nothing)
    RS_ActionInterface* action = view->getCurrentAction();
    const bool midway = action != nullptr && action != view->getDefaultAction() && action->getStatus() > 0;
    const QString name = midway ? view->getCurrentActionName() : QString();
    view->killAllActions();
    if (!midway) {
        return {};
    }
    return name.isEmpty() ? tr("The command in progress was ended first.")
                          : tr("The command in progress (%1) was ended first.").arg(name);
}
