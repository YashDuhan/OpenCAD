/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AICHECKPOINTS_H
#define LC_AICHECKPOINTS_H

#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <memory>

class RS_Document;
class RS_GraphicView;
class RS_Layer;

/** What one AI run changed in the drawing it worked on. Entity ids are the ones list_entities shows. */
struct LC_AIRunChanges {
    int run {0};                       // the checkpoint: 1, 2, ... in the order they were taken; 0: no run
    QList<unsigned long long> added;   // entities the run drew that were still there when it ended
    QList<unsigned long long> removed; // entities it deleted (an edit deletes the entities it changes and adds new ones)
    QStringList layersAdded;
    QStringList layersRemoved;
    QStringList blocksAdded;
    QStringList blocksRemoved;

    int count() const {
        return int(added.size() + removed.size() + layersAdded.size() + layersRemoved.size() + blocksAdded.size()
                   + blocksRemoved.size());
    }
    bool isEmpty() const { return count() == 0; }
    /** "5 entities added, 1 removed; layer Notes added", for tooltips and messages. */
    QString summary() const;
};

/**
 * Checkpoints of the drawing around every AI run, so what the AI did can be taken back. When a run starts
 * (LC_AIControl leaves Idle) the drawing it works on is recorded: its position in the undo history and the
 * ids of its entities, its layers and blocks; when the run ends, what the run added and removed.
 * A run that moves to another drawing (one the AI opened or switched to) gets a checkpoint there too, with a
 * number of its own: each drawing is reverted on its own, and the numbers below name checkpoints.
 * A run is reverted by undoing back to its checkpoint when nothing changed after it; otherwise only its own
 * changes can be taken back, as one new undo step that leaves later changes alone. Nothing goes to disk.
 * Undo and Redo in the Edit menu can bring reverted changes back: the checkpoint then counts as not reverted.
 */
class LC_AICheckpoints : public QObject {
    Q_OBJECT
public:
    /** What reverting a run would do now. */
    enum class Situation {
        Unchanged,    // the drawing is as the run left it: undo back to the checkpoint
        ChangedSince, // the drawing changed after the run: only the run's own changes can be taken back
        NothingLeft,  // the run's changes are gone already (undone, or deleted since)
        Unavailable   // the drawing is closed, the checkpoint was dropped, or the AI is working
    };
    struct Check {
        Situation situation {Situation::Unavailable};
        QString text;      // the situation in words, for the user
        int toRemove {0};  // reverting only the run's own changes: entities it drew that are still there
        int toRestore {0}; // ... and entities it deleted that can come back
    };

    static LC_AICheckpoints& instance();

    /** The changes of the last run that ended (run 0 while none has ended). A change preview highlights these. */
    LC_AIRunChanges changesOfLastRun() const;
    LC_AIRunChanges changesOf(int run) const;
    /** The drawing a run worked on, or nullptr once it is closed (or when no drawing was open). */
    RS_GraphicView* viewOf(int run) const;
    /** The drawing's name when the run changed more than one drawing (so each bar says which), else empty. */
    QString drawingLabelOf(int run) const;
    /** The run in progress, 0 when the AI is not working. */
    int currentRun() const { return m_current; }
    /** Only the last checkpoints are kept; older ones cannot be reverted any more. */
    bool isKept(int run) const;
    bool isReverted(int run) const;
    /** What the run's revert took back (less than changesOf().count() when some changes could not come back)... */
    int revertedCount(int run) const;
    /** ... and what it said about that. */
    QString revertMessage(int run) const;

    Check check(int run) const;
    /** Undoes back to the run's checkpoint; only while nothing changed after the run (check() says Unchanged). */
    bool revert(int run, QString& message);
    /** Takes back only what the run itself changed, as one new undo step; changes made after it stay. */
    bool revertOwnChanges(int run, QString& message);
    /** A reverted run can come back while the drawing is still as the revert left it. */
    bool canRestore(int run) const;
    /** The run was reverted (and not restored since); for a revert bar made again (LC_AIRevertBar::addForRun). */
    bool isRunReverted(int run) const {
        const auto it = m_runs.constFind(run);
        return it != m_runs.cend() && it->reverted != Reverted::No;
    }
    bool restore(int run, QString& message);

signals:
    void runStarted(int run);
    /** The run ended and what it changed is known (also when it changed nothing). */
    void runRecorded(int run);
    /** A run was reverted (true) or restored (false), also by Undo or Redo in the Edit menu. */
    void runReverted(int run, bool reverted);
    /** A drawing with checkpoints changed (an edit, Undo, Redo): whether a run can be restored may have changed. */
    void drawingChanged();

private:
    class Watch; // the drawings' undo history tells it about every change
    struct Snapshot {
        QSet<unsigned long long> entities; // the live entities, by id
        QStringList layers;
        QStringList blocks;
        QString activeLayer;
        int undoSteps {0};                 // undo cycles before the current position in the history
    };
    enum class Reverted { No, Undone, OwnStep };
    struct Checkpoint {
        LC_AIRunChanges changes;
        int group {0};                     // the run it belongs to: the number of the run's first checkpoint
        QString name;                      // the drawing's name when the run ended
        QPointer<RS_GraphicView> view;
        RS_Document* document {nullptr};   // only used while view still shows it
        Snapshot before;
        Snapshot after;
        bool ended {false};
        bool stepsBroken {false};          // undoing back to the checkpoint did not give the drawing before the run
        Reverted reverted {Reverted::No};
        Snapshot beforeRevert;             // for Restore: the drawing just before and just after the revert
        Snapshot afterRevert;
        QList<std::shared_ptr<RS_Layer>> removedLayers; // empty layers it added, taken out by the revert
        int revertedCount {0};
        QString revertMessage;
        int endedAt {0};                   // when the run ended and when it was reverted, in m_clock's ticks
        int revertedAt {0};
    };

    LC_AICheckpoints();
    void begin();
    void addDrawing(RS_GraphicView* view);
    void start(int run, int group, RS_GraphicView* view);
    void end();
    void record(Checkpoint& checkpoint);
    void scheduleUpdate();
    void update();
    static Snapshot snapshot(RS_Document* document);
    static bool alive(const Checkpoint& checkpoint);
    static void refresh(const Checkpoint& checkpoint);
    static void activateLayer(RS_Document* document, const QString& from, const QString& to);
    static void removeEmptyLayers(Checkpoint& checkpoint);
    static void addRemovedLayers(Checkpoint& checkpoint);
    static QString endCommand(RS_GraphicView* view);
    QString changedSinceText(int run, const Checkpoint& checkpoint, const Snapshot& now) const;
    QString leftOverText(const Checkpoint& checkpoint) const;

    QMap<int, Checkpoint> m_runs;
    int m_current {0};
    int m_last {0};
    int m_next {1};
    int m_clock {0};                    // counts ends and reverts, to tell which came first
    Watch* m_watch {nullptr};
    QList<RS_Document*> m_watched;      // drawings m_watch listens to (taken out when their view goes)
    bool m_updateQueued {false};
};

#endif
