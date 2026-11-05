/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIAGENTPROGRESS_H
#define LC_AIAGENTPROGRESS_H

#include <QDialog>
#include <QHash>

#include "lc_aitoolbar.h"

class QLabel;
class QLineEdit;
class QMainWindow;
class QMenu;
class QProgressBar;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

/**
 * "Agent Progress" in the menu bar: every toolbar button and every tool of the AI, whether the AI can use it
 * (agentic) and whether it passed its test. Clicking one opens the AI chat with a task for it; tests run from
 * here too (on scratch drawings that are opened and closed again).
 */
namespace LC_AIAgentProgress {
    /** The menu for the menu bar of this window (made once). */
    QMenu* menu(QMainWindow* window);
    /** Starts testing these ids (all when empty); progress shows in the menu and the Agent Progress window. */
    void startTests(QWidget* window, const QStringList& ids);
}

/** Tells the Agent Progress menu and window how a test run goes. */
class LC_AIAgentProgressHub : public QObject {
    Q_OBJECT
public:
    static LC_AIAgentProgressHub& instance();
    int done() const { return m_done; }
    int total() const { return m_total; }

signals:
    void started(int total);
    void progressed(int done, int total, const LC_AIButtonTest& result);
    void finished();

private:
    friend void LC_AIAgentProgress::startTests(QWidget*, const QStringList&);
    int m_done {0};
    int m_total {0};
};

/** The Agent Progress window: buttons and AI tools with their state, to test them or give the AI a task. */
class LC_AIAgentProgressDialog : public QDialog {
    Q_OBJECT
public:
    explicit LC_AIAgentProgressDialog(QWidget* parent = nullptr);
    static void showFor(QWidget* window);

private:
    void rebuild();
    void applyFilter();
    void updateSummary();
    void setResult(const LC_AIButtonTest& result);
    void askAI(QTreeWidgetItem* item);
    QStringList selectedIds() const;
    QStringList visibleIds() const;
    void onStarted(int total);
    void onProgress(int done, int total, const LC_AIButtonTest& result);
    void onFinished();

    QLabel* m_summary {nullptr};
    QLineEdit* m_search {nullptr};
    QTabWidget* m_tabs {nullptr};
    QTreeWidget* m_buttons {nullptr};
    QTreeWidget* m_tools {nullptr};
    QProgressBar* m_progress {nullptr};
    QLabel* m_status {nullptr};
    QPushButton* m_ask {nullptr};
    QPushButton* m_testSelected {nullptr};
    QPushButton* m_testShown {nullptr};
    QPushButton* m_stop {nullptr};
    QHash<QString, QTreeWidgetItem*> m_items; // by id, in both trees
    QHash<QString, LC_AIButtonTest> m_results;
};

#endif
