/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aiagentprogress.h"

#include <QAction>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "lc_aiassistantwidget.h"
#include "lc_aipermissions.h"
#include "lc_aiprovidersdialog.h"
#include "lc_aitool.h"

namespace {
    enum Column { ColName, ColId, ColKind, ColAgentic, ColTested, ColNote, ColCount };

    QString clean(QString text) {
        text.remove('&');
        return text.simplified();
    }

    /** ✓ passed, ✗ failed, – skipped on purpose, ○ not tested yet. */
    QString mark(const LC_AIButtonTest* r) {
        if (r == nullptr || r->status.isEmpty()) {
            return QString::fromUtf8("○");
        }
        if (r->status == "passed") {
            return QString::fromUtf8("✓");
        }
        if (r->status == "skipped") {
            return QString::fromUtf8("–");
        }
        return QString::fromUtf8("✗");
    }

    QString testedText(const LC_AIButtonTest* r) {
        if (r == nullptr || r->status.isEmpty()) {
            return QObject::tr("not tested");
        }
        if (r->status == "passed") {
            return QObject::tr("passed");
        }
        if (r->status == "skipped") {
            return QObject::tr("not run");
        }
        return QObject::tr("failed");
    }

    QColor testedColor(const LC_AIButtonTest* r, const LC_AITheme& t) {
        if (r != nullptr && r->status == "passed") {
            return t.accentGreen;
        }
        if (r != nullptr && r->status == "failed") {
            return QColor("#f87171");
        }
        return t.textMuted;
    }

    QList<LC_AITool> aiTools(QWidget* window) {
        QWidget* top = window != nullptr ? window->window() : nullptr;
        if (top != nullptr) {
            if (auto* chat = top->findChild<LC_AIAssistantWidget*>()) {
                return chat->availableTools();
            }
        }
        return LC_AITools::all() + LC_AIToolbar::tools();
    }

    /** The toolbar a button is first found in ("Line", "Snap Selection", ...). */
    QString toolbarOf(const QJsonObject& button) {
        return button.value("toolbars").toArray().first().toString().section(" > ", 0, 0);
    }

    QString buttonTask(const QJsonObject& b) {
        const QString label = b.value("label").toString();
        const QString id = b.value("id").toString();
        if (b.value("kind").toString() == "list") {
            return QObject::tr("Using the %1 toolbar list (%2): ").arg(toolbarOf(b), id);
        }
        return QObject::tr("Using the “%1” toolbar button (%2, %3 toolbar): ").arg(label, id, toolbarOf(b));
    }

    QString toolTask(const LC_AITool& tool) {
        return QObject::tr("Using your tool “%1” (%2): ").arg(tool.label, tool.name);
    }

    QPointer<QMenu> g_menu;
}

// ------------------------------------------------------------------------------------------------ test runs

LC_AIAgentProgressHub& LC_AIAgentProgressHub::instance() {
    static LC_AIAgentProgressHub hub;
    return hub;
}

void LC_AIAgentProgress::startTests(QWidget* window, const QStringList& ids) {
    if (LC_AIToolbar::testsRunning()) {
        return;
    }
    QStringList chosen;
    const QStringList all = LC_AIToolbar::recipeIds();
    for (const QString& id : all) {
        if (ids.isEmpty() || ids.contains(id)) {
            chosen << id;
        }
    }
    if (chosen.isEmpty()) {
        QMessageBox::information(window, QObject::tr("Agent Progress"),
                                 QObject::tr("There is no test for the chosen buttons or tools yet."));
        return;
    }
    LC_AIAgentProgressHub& hub = LC_AIAgentProgressHub::instance();
    hub.m_done = 0;
    hub.m_total = chosen.size();
    emit hub.started(chosen.size());
    LC_AIToolbar::runTests(
        chosen,
        [](const int done, const int total, const LC_AIButtonTest& result) {
            LC_AIAgentProgressHub& h = LC_AIAgentProgressHub::instance();
            h.m_done = done;
            h.m_total = total;
            emit h.progressed(done, total, result);
        },
        [](const QList<LC_AIButtonTest>&) { emit LC_AIAgentProgressHub::instance().finished(); });
}

// ------------------------------------------------------------------------------------------------ the menu

QMenu* LC_AIAgentProgress::menu(QMainWindow* window) {
    if (g_menu) {
        return g_menu;
    }
    // one entry: the window lists every button and AI tool, tests them and hands them to the AI
    auto* menu = new QMenu(QObject::tr("Agent &Progress"), window);
    menu->setObjectName("agent_progress");
    g_menu = menu;
    QAction* open = menu->addAction(QObject::tr("Open Agent Progress..."));
    QObject::connect(open, &QAction::triggered, window, [window] { LC_AIAgentProgressDialog::showFor(window); });
    return menu;
}

// ------------------------------------------------------------------------------------------------ the window

LC_AIAgentProgressDialog::LC_AIAgentProgressDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Agent Progress"));
    setObjectName("agentProgress");
    resize(1040, 720);
    const LC_AITheme t = LC_AITheme::current();
    setStyleSheet(t.styleSheet() + QStringLiteral(R"(
#apTitle { font-size: 17px; font-weight: 500; color: %1; }
#apSummary { color: %2; }
#apSearch { background: %3; border: 1px solid %4; border-radius: 8px; padding: 5px 9px; }
QTreeWidget { background: %5; border: 1px solid %4; border-radius: 10px; padding: 4px; alternate-background-color: %6; }
QTreeWidget::item { padding: 3px 2px; }
QTreeWidget::item:selected { background: %7; color: %1; }
QHeaderView::section { background: transparent; border: none; border-bottom: 1px solid %4; padding: 5px 6px; color: %2; font-weight: 500; }
QTabWidget::pane { border: none; }
QTabBar::tab { background: transparent; color: %2; padding: 6px 14px; border: none; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: %1; border-bottom: 2px solid %8; }
QPushButton { background: %3; border: 1px solid %4; border-radius: 8px; padding: 6px 14px; }
QPushButton:hover { background: %7; }
QPushButton:disabled { color: %2; }
#apPrimary { background: %8; border: none; color: white; }
QProgressBar { background: %3; border: none; border-radius: 4px; max-height: 8px; }
QProgressBar::chunk { background: %8; border-radius: 4px; }
)")
                                     .arg(t.textHeading.name(), t.textMuted.name(), t.bgSubtle.name(QColor::HexArgb),
                                          t.borderSubtle.name(QColor::HexArgb), t.bgChrome.name(QColor::HexArgb),
                                          t.bgSubtle.name(QColor::HexArgb), t.bgActive.name(QColor::HexArgb), t.accentBlue.name()));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(10);
    auto* title = new QLabel(tr("Agent Progress"), this);
    title->setObjectName("apTitle");
    layout->addWidget(title);
    m_summary = new QLabel(this);
    m_summary->setObjectName("apSummary");
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    m_search = new QLineEdit(this);
    m_search->setObjectName("apSearch");
    m_search->setPlaceholderText(tr("Search buttons and tools"));
    m_search->setClearButtonEnabled(true);
    layout->addWidget(m_search);
    connect(m_search, &QLineEdit::textChanged, this, &LC_AIAgentProgressDialog::applyFilter);

    m_tabs = new QTabWidget(this);
    auto makeTree = [this](const QStringList& headers) {
        auto* tree = new QTreeWidget(this);
        tree->setColumnCount(headers.size());
        tree->setHeaderLabels(headers);
        tree->setAlternatingRowColors(true);
        tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
        tree->setUniformRowHeights(true);
        tree->header()->setStretchLastSection(true);
        tree->setRootIsDecorated(true);
        connect(tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) { askAI(item); });
        connect(tree, &QTreeWidget::itemSelectionChanged, this, [this] {
            m_ask->setEnabled(selectedIds().size() == 1);
            m_testSelected->setEnabled(!selectedIds().isEmpty() && !LC_AIToolbar::testsRunning());
        });
        return tree;
    };
    m_buttons = makeTree({tr("Button"), tr("Id"), tr("Kind"), tr("Agentic"), tr("Tested"), tr("Last result")});
    m_tools = makeTree({tr("Tool"), tr("Name"), tr("Permission"), tr("Agentic"), tr("Tested"), tr("Last result")});
    m_tabs->addTab(m_buttons, tr("Toolbar buttons"));
    m_tabs->addTab(m_tools, tr("AI tools"));
    layout->addWidget(m_tabs, 1);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this] { emit m_buttons->itemSelectionChanged(); });

    m_progress = new QProgressBar(this);
    m_progress->setTextVisible(false);
    m_progress->hide();
    layout->addWidget(m_progress);

    auto* bar = new QHBoxLayout();
    m_status = new QLabel(tr("Double-click a button or tool to give the AI a task with it."), this);
    m_status->setObjectName("apSummary");
    bar->addWidget(m_status, 1);
    m_ask = new QPushButton(tr("Ask AI to use it"), this);
    m_ask->setObjectName("apPrimary");
    m_ask->setEnabled(false);
    m_testSelected = new QPushButton(tr("Test selected"), this);
    m_testSelected->setEnabled(false);
    m_testShown = new QPushButton(tr("Test all shown"), this);
    m_stop = new QPushButton(tr("Stop"), this);
    m_stop->hide();
    auto* close = new QPushButton(tr("Close"), this);
    bar->addWidget(m_ask);
    bar->addWidget(m_testSelected);
    bar->addWidget(m_testShown);
    bar->addWidget(m_stop);
    bar->addWidget(close);
    layout->addLayout(bar);

    connect(m_ask, &QPushButton::clicked, this, [this] {
        QTreeWidget* tree = m_tabs->currentIndex() == 0 ? m_buttons : m_tools;
        askAI(tree->currentItem());
    });
    connect(m_testSelected, &QPushButton::clicked, this, [this] { LC_AIAgentProgress::startTests(this, selectedIds()); });
    connect(m_testShown, &QPushButton::clicked, this, [this] { LC_AIAgentProgress::startTests(this, visibleIds()); });
    connect(m_stop, &QPushButton::clicked, this, [] { LC_AIToolbar::stopTests(); });
    connect(close, &QPushButton::clicked, this, &QDialog::close);

    LC_AIAgentProgressHub& hub = LC_AIAgentProgressHub::instance();
    connect(&hub, &LC_AIAgentProgressHub::started, this, &LC_AIAgentProgressDialog::onStarted);
    connect(&hub, &LC_AIAgentProgressHub::progressed, this, &LC_AIAgentProgressDialog::onProgress);
    connect(&hub, &LC_AIAgentProgressHub::finished, this, &LC_AIAgentProgressDialog::onFinished);

    rebuild();
    if (LC_AIToolbar::testsRunning()) {
        onStarted(hub.total());
        m_progress->setValue(hub.done());
    }
}

void LC_AIAgentProgressDialog::showFor(QWidget* window) {
    QWidget* top = window != nullptr ? window->window() : nullptr;
    auto* dialog = top != nullptr ? top->findChild<LC_AIAgentProgressDialog*>() : nullptr;
    if (dialog == nullptr) {
        dialog = new LC_AIAgentProgressDialog(top);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void LC_AIAgentProgressDialog::rebuild() {
    m_results = LC_AIToolbar::savedResults();
    m_items.clear();
    m_buttons->clear();
    m_tools->clear();
    const LC_AITheme t = LC_AITheme::current();
    QWidget* top = parentWidget() != nullptr ? parentWidget()->window() : nullptr;

    QHash<QString, QTreeWidgetItem*> groups;
    for (const QJsonValue& v : LC_AIToolbar::inventory()) {
        const QJsonObject b = v.toObject();
        const QString toolbar = toolbarOf(b);
        QTreeWidgetItem* group = groups.value(toolbar);
        if (group == nullptr) {
            group = new QTreeWidgetItem(m_buttons, {toolbar});
            QFont f = group->font(ColName);
            f.setBold(true);
            group->setFont(ColName, f);
            group->setFirstColumnSpanned(false);
            groups.insert(toolbar, group);
        }
        const QString id = b.value("id").toString();
        auto* item = new QTreeWidgetItem(group);
        item->setText(ColName, b.value("label").toString());
        item->setText(ColId, id);
        item->setText(ColKind, b.value("kind").toString());
        item->setData(ColName, Qt::UserRole, id);
        item->setData(ColName, Qt::UserRole + 1, QJsonDocument(b).toJson(QJsonDocument::Compact));
        if (top != nullptr) {
            if (auto* action = top->findChild<QAction*>(id)) {
                item->setIcon(ColName, action->icon());
            }
        }
        const QString why = LC_AIToolbar::notAgenticReason(id);
        item->setText(ColAgentic, why.isEmpty() ? QString::fromUtf8("✓ yes") : QString::fromUtf8("– no"));
        item->setForeground(ColAgentic, why.isEmpty() ? t.accentGreen : t.textMuted);
        if (!why.isEmpty()) {
            item->setText(ColNote, why);
        }
        m_items.insert(id, item);
    }
    for (QTreeWidgetItem* group : std::as_const(groups)) {
        group->setExpanded(true);
    }

    for (const LC_AITool& tool : aiTools(this)) {
        auto* item = new QTreeWidgetItem(m_tools);
        item->setText(ColName, tool.label);
        item->setText(ColId, tool.name);
        const LC_AIPermissions::Permission p = LC_AIPermissions::permission(tool.name);
        item->setText(ColKind, p == LC_AIPermissions::Permission::Allow ? tr("allowed")
                               : p == LC_AIPermissions::Permission::Deny ? tr("denied") : tr("asks first"));
        item->setText(ColAgentic, QString::fromUtf8("✓ yes"));
        item->setForeground(ColAgentic, t.accentGreen);
        item->setToolTip(ColName, tool.description);
        item->setData(ColName, Qt::UserRole, QString("tool:" + tool.name));
        item->setData(ColName, Qt::UserRole + 2, tool.label);
        m_items.insert("tool:" + tool.name, item);
    }
    for (auto it = m_results.cbegin(); it != m_results.cend(); ++it) {
        setResult(it.value());
    }
    for (QTreeWidgetItem* item : std::as_const(m_items)) {
        if (item->text(ColTested).isEmpty()) {
            item->setText(ColTested, QString::fromUtf8("○ ") + tr("not tested"));
            item->setForeground(ColTested, t.textMuted);
        }
    }
    for (QTreeWidget* tree : {m_buttons, m_tools}) {
        tree->header()->resizeSection(ColName, 250);
        tree->header()->resizeSection(ColId, 190);
        tree->header()->resizeSection(ColKind, 80);
        tree->header()->resizeSection(ColAgentic, 80);
        tree->header()->resizeSection(ColTested, 100);
    }
    applyFilter();
    updateSummary();
}

void LC_AIAgentProgressDialog::setResult(const LC_AIButtonTest& result) {
    m_results.insert(result.id, result);
    QTreeWidgetItem* item = m_items.value(result.id);
    if (item == nullptr) {
        return;
    }
    const LC_AITheme t = LC_AITheme::current();
    item->setText(ColTested, mark(&result) + " " + testedText(&result));
    item->setForeground(ColTested, testedColor(&result, t));
    QString note = result.detail.section('\n', 0, 0).left(160);
    if (!result.when.isEmpty()) {
        note = result.when.left(16).replace('T', ' ') + (note.isEmpty() ? QString() : "  " + note);
    }
    item->setText(ColNote, note);
    item->setToolTip(ColNote, result.detail.left(2000));
}

void LC_AIAgentProgressDialog::applyFilter() {
    const QString needle = m_search->text().trimmed().toLower();
    for (int g = 0; g < m_buttons->topLevelItemCount(); ++g) {
        QTreeWidgetItem* group = m_buttons->topLevelItem(g);
        int shown = 0;
        int passed = 0;
        for (int i = 0; i < group->childCount(); ++i) {
            QTreeWidgetItem* item = group->child(i);
            const bool match = needle.isEmpty() || item->text(ColName).toLower().contains(needle)
                               || item->text(ColId).toLower().contains(needle) || group->text(ColName).toLower().contains(needle);
            item->setHidden(!match);
            shown += match ? 1 : 0;
            passed += m_results.value(item->text(ColId)).status == "passed" ? 1 : 0;
        }
        group->setHidden(shown == 0);
        group->setText(ColTested, QStringLiteral("%1/%2").arg(passed).arg(group->childCount()));
    }
    for (int i = 0; i < m_tools->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_tools->topLevelItem(i);
        item->setHidden(!needle.isEmpty() && !item->text(ColName).toLower().contains(needle)
                        && !item->text(ColId).toLower().contains(needle));
    }
}

void LC_AIAgentProgressDialog::updateSummary() {
    int buttons = 0, agentic = 0, passed = 0, failed = 0;
    for (int g = 0; g < m_buttons->topLevelItemCount(); ++g) {
        QTreeWidgetItem* group = m_buttons->topLevelItem(g);
        for (int i = 0; i < group->childCount(); ++i) {
            const QString id = group->child(i)->text(ColId);
            ++buttons;
            agentic += LC_AIToolbar::notAgenticReason(id).isEmpty() ? 1 : 0;
            passed += m_results.value(id).status == "passed" ? 1 : 0;
            failed += m_results.value(id).status == "failed" ? 1 : 0;
        }
    }
    int tools = m_tools->topLevelItemCount();
    int toolsPassed = 0;
    for (int i = 0; i < tools; ++i) {
        toolsPassed += m_results.value("tool:" + m_tools->topLevelItem(i)->text(ColId)).status == "passed" ? 1 : 0;
    }
    m_summary->setText(tr("%1 toolbar buttons in %2 toolbars: %3 agentic, %4 passed their test, %5 failed, %6 not tested.   "
                          "%7 AI tools: %8 passed.")
                           .arg(buttons).arg(m_buttons->topLevelItemCount()).arg(agentic).arg(passed).arg(failed)
                           .arg(buttons - passed - failed).arg(tools).arg(toolsPassed));
}

QStringList LC_AIAgentProgressDialog::selectedIds() const {
    QStringList ids;
    QTreeWidget* tree = m_tabs->currentIndex() == 0 ? m_buttons : m_tools;
    for (QTreeWidgetItem* item : tree->selectedItems()) {
        if (item->childCount() > 0) {
            for (int i = 0; i < item->childCount(); ++i) {
                ids << item->child(i)->data(ColName, Qt::UserRole).toString();
            }
        }
        else if (!item->data(ColName, Qt::UserRole).toString().isEmpty()) {
            ids << item->data(ColName, Qt::UserRole).toString();
        }
    }
    ids.removeDuplicates();
    return ids;
}

QStringList LC_AIAgentProgressDialog::visibleIds() const {
    QStringList ids;
    if (m_tabs->currentIndex() == 0) {
        for (int g = 0; g < m_buttons->topLevelItemCount(); ++g) {
            QTreeWidgetItem* group = m_buttons->topLevelItem(g);
            for (int i = 0; i < group->childCount(); ++i) {
                if (!group->child(i)->isHidden()) {
                    ids << group->child(i)->data(ColName, Qt::UserRole).toString();
                }
            }
        }
    }
    else {
        for (int i = 0; i < m_tools->topLevelItemCount(); ++i) {
            if (!m_tools->topLevelItem(i)->isHidden()) {
                ids << m_tools->topLevelItem(i)->data(ColName, Qt::UserRole).toString();
            }
        }
    }
    return ids;
}

void LC_AIAgentProgressDialog::askAI(QTreeWidgetItem* item) {
    if (item == nullptr || item->childCount() > 0) {
        return;
    }
    const QString id = item->data(ColName, Qt::UserRole).toString();
    if (id.isEmpty() || !LC_AIToolbar::notAgenticReason(id).isEmpty()) {
        return;
    }
    QString prompt;
    if (id.startsWith("tool:")) {
        LC_AITool tool;
        tool.name = id.mid(5);
        tool.label = item->data(ColName, Qt::UserRole + 2).toString();
        prompt = toolTask(tool);
    }
    else {
        prompt = buttonTask(QJsonDocument::fromJson(item->data(ColName, Qt::UserRole + 1).toByteArray()).object());
    }
    LC_AIAssistantWidget::openWithTask(parentWidget(), prompt);
    m_status->setText(tr("The task is in the AI chat: finish the sentence and send it."));
}

void LC_AIAgentProgressDialog::onStarted(const int total) {
    m_progress->setRange(0, qMax(1, total));
    m_progress->setValue(0);
    m_progress->show();
    m_stop->show();
    m_testSelected->setEnabled(false);
    m_testShown->setEnabled(false);
    m_status->setText(tr("Testing 0 of %1... OpenCAD opens and closes scratch drawings meanwhile; please wait.").arg(total));
}

void LC_AIAgentProgressDialog::onProgress(const int done, const int total, const LC_AIButtonTest& result) {
    LC_AIButtonTest r = result;
    r.when = QDateTime::currentDateTime().toString(Qt::ISODate);
    setResult(r);
    m_progress->setRange(0, qMax(1, total));
    m_progress->setValue(done);
    m_status->setText(tr("Testing %1 of %2: %3 %4").arg(done).arg(total).arg(result.id, testedText(&result)));
    if (QTreeWidgetItem* item = m_items.value(result.id)) {
        item->treeWidget()->scrollToItem(item);
    }
    applyFilter();
    updateSummary();
}

void LC_AIAgentProgressDialog::onFinished() {
    m_progress->hide();
    m_stop->hide();
    m_testShown->setEnabled(true);
    m_testSelected->setEnabled(!selectedIds().isEmpty());
    rebuild();
    m_status->setText(tr("Testing finished. Double-click a button or tool to give the AI a task with it."));
}

// developer check: OPENCAD_AI_AGENT_PROGRESS_SHOT=<file.png> opens the Agent Progress window, saves a picture of it
// and quits (offscreen screenshots for reviews)
#include <QApplication>
#include <QTimer>
#include <cstdlib>
namespace {
    void agentProgressShot() {
        const QString path = qEnvironmentVariable("OPENCAD_AI_AGENT_PROGRESS_SHOT");
        if (path.isEmpty()) {
            return;
        }
        QTimer::singleShot(6000, qApp, [path] {
            QMainWindow* window = nullptr;
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (auto* main = qobject_cast<QMainWindow*>(w); main != nullptr && main->findChild<QMenu*>("agent_progress") != nullptr) {
                    window = main;
                }
            }
            if (window == nullptr) {
                std::_Exit(2);
            }
            LC_AIAgentProgressDialog::showFor(window);
            QTimer::singleShot(800, qApp, [window, path] {
                if (auto* dialog = window->findChild<LC_AIAgentProgressDialog*>()) {
                    dialog->grab().save(path);
                }
                std::_Exit(0);
            });
        });
    }
}
Q_COREAPP_STARTUP_FUNCTION(agentProgressShot)
