/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aitoolbar.h"

#include <QAbstractButton>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTextEdit>
#include <QHeaderView>
#include <QTableView>
#include <QTreeView>
#include <QStatusBar>
#include <QDockWidget>
#include <QMenuBar>
#include <QWheelEvent>
#include <QWindow>
#include <QKeySequence>
#include <QBuffer>
#include <QContextMenuEvent>
#include <QHelpEvent>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTextStream>
#include <QWidgetAction>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>

#include "lc_aicontrol.h"
#include "lc_graphicviewport.h"
#include "qc_applicationwindow.h"
#include "qc_mdiwindow.h"
#include "qg_commandwidget.h"
#include "rs_actioninterface.h"
#include "rs_arc.h"
#include "rs_block.h"
#include "rs_blocklist.h"
#include "rs_circle.h"
#include "rs_document.h"
#include "rs_ellipse.h"
#include "rs_entity.h"
#include "rs_graphic.h"
#include "rs_graphicview.h"
#include "rs_insert.h"
#include "rs_layer.h"
#include "rs_layerlist.h"
#include "rs_line.h"
#include "rs_point.h"
#include "rs_polyline.h"
#include "rs_selection.h"

namespace {
    using Done = std::function<void(const LC_AIToolResult&)>;

    constexpr int TRIGGER_SETTLE_MS = 350; // after pressing a button: dialogs open, tools start
    constexpr int STEP_SETTLE_MS = 60;     // after each typed line, click or key
    constexpr int DIALOG_SETTLE_MS = 300;  // after pressing a dialog button
    constexpr int MAX_LIST_ITEMS = 40;     // items of a list or combo box shown in a dialog description
    constexpr int MAX_REPORTED_ENTITIES = 12;

    // buttons the AI must not press: they end OpenCAD or close the AI itself
    const QSet<QString> REFUSED = {"FileQuit", "AIAssistantToggle"};

    // ------------------------------------------------------------------ schema helpers

    QJsonObject str(const QString& description) {
        return {{"type", "string"}, {"description", description}};
    }
    QJsonObject num(const QString& description) {
        return {{"type", "number"}, {"description", description}};
    }
    QJsonObject flag(const QString& description) {
        return {{"type", "boolean"}, {"description", description}};
    }
    QJsonObject schema(const QJsonObject& properties, const QStringList& required = {}) {
        QJsonObject s{{"type", "object"}, {"properties", properties}};
        if (!required.isEmpty()) {
            s.insert("required", QJsonArray::fromStringList(required));
        }
        return s;
    }

    /** The steps a button's prompts are answered with: typed lines, clicks in the drawing, keys. */
    QJsonObject stepsSchema() {
        const QJsonObject step = schema(
            {{"type", QJsonObject{{"type", "string"},
                                  {"enum", QJsonArray{"type", "click", "drag", "move", "key", "wait"}},
                                  {"description", "type: a line for the command line (a point \"x,y\", \"@dx,dy\", "
                                                  "\"@dist<angle\", a number, or an option word of the running tool); "
                                                  "click: a click in the drawing at x,y (drawing units); drag: press at "
                                                  "x,y, move with the button held and release at x2,y2 (window zoom, "
                                                  "panning, freehand); move: move the mouse to x,y without clicking (tools "
                                                  "that follow the cursor); key: a key pressed in the drawing (Enter, "
                                                  "Escape, Delete, Backspace, Space, Tab); wait: pause ms milliseconds"}}},
             {"text", str("For type: the line to type. For key: the key name.")},
             {"x", num("For click, drag, move: X in drawing units")},
             {"y", num("For click, drag, move: Y in drawing units")},
             {"x2", num("For drag: X where the button is released")},
             {"y2", num("For drag: Y where the button is released")},
             {"button", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"left", "right"}},
                                    {"description", "For click and drag: mouse button (default left)"}}},
             {"shift", flag("For click: hold Shift")},
             {"ctrl", flag("For click: hold Ctrl")},
             {"ms", num("For wait: milliseconds (at most 5000)")}},
            {"type"});
        return {{"type", "array"}, {"items", step},
                {"description", "What to do after pressing, in order, like a person answering the tool's prompts"}};
    }

    QString clean(QString text) {
        text.remove('&');
        return text.simplified();
    }

    QString norm(const QString& text) {
        QString t = clean(text).toLower();
        while (t.endsWith(':') || t.endsWith('.')) {
            t.chop(1);
        }
        return t.trimmed();
    }

    // ------------------------------------------------------------------ the application

    QC_ApplicationWindow* appWindow() {
        auto& app = QC_ApplicationWindow::getAppWindow();
        return app ? app.get() : nullptr;
    }

    QG_CommandWidget* commandWidget() {
        QC_ApplicationWindow* app = appWindow();
        return app != nullptr ? app->findChild<QG_CommandWidget*>() : nullptr;
    }

    QList<QC_MDIWindow*> drawingWindows() {
        QList<QC_MDIWindow*> list;
        if (QC_ApplicationWindow* app = appWindow()) {
            if (QMdiArea* area = app->getMdiArea()) {
                for (QMdiSubWindow* w : area->subWindowList()) {
                    if (auto* mdi = qobject_cast<QC_MDIWindow*>(w)) {
                        list << mdi;
                    }
                }
            }
        }
        return list;
    }

    // ------------------------------------------------------------------ toolbar buttons

    struct Button {
        QPointer<QAction> action;      // a button
        QPointer<QComboBox> combo;     // or a list in a toolbar
        QString id;                    // the action's object name, or "list:<toolbar>/<name>"
        QStringList places;            // "Toolbar" or "Toolbar > menu"
        QPointer<QToolButton> menuButton; // or a button whose menu lists things (named views, UCS, workspaces)
    };

    /** The items of a list button's menu, which it fills in when it opens. */
    QList<QAction*> menuItems(QToolButton* button) {
        QList<QAction*> items;
        if (button == nullptr || button->menu() == nullptr) {
            return items;
        }
        emit button->menu()->aboutToShow();
        for (QAction* a : button->menu()->actions()) {
            // the list buttons reuse their actions and hide the ones they do not need
            if (!a->isSeparator() && a->menu() == nullptr && a->isVisible()) {
                items << a;
            }
        }
        return items;
    }

    /** An item's name in a list button's menu (a UCS can have no name). */
    QString itemName(const QAction* a, const int index) {
        const QString text = clean(a->text());
        return text.isEmpty() ? QStringLiteral("(unnamed %1)").arg(index) : text;
    }

    QStringList optionsOf(const QComboBox* combo) {
        QStringList items;
        for (int i = 0; i < combo->count() && i < MAX_LIST_ITEMS; ++i) {
            items << combo->itemText(i);
        }
        if (combo->count() > MAX_LIST_ITEMS) {
            items << QStringLiteral("... %1 more").arg(combo->count() - MAX_LIST_ITEMS);
        }
        return items;
    }

    QString labelOf(const QAction* a) {
        return clean(a->text());
    }

    QString kindOf(const QAction* a) {
        if (a->property("RS2:actionType").isValid()) {
            return QStringLiteral("tool");
        }
        if (a->isCheckable()) {
            return QStringLiteral("toggle");
        }
        return QStringLiteral("command");
    }

    QString kindOf(const Button& b) {
        return b.combo || b.menuButton ? QStringLiteral("list") : b.action ? kindOf(b.action) : QString();
    }

    QString comboId(const QComboBox* combo, const QString& toolbarName) {
        QString name = combo->objectName();
        if (name.isEmpty() || name.startsWith("qt_")) {
            name = QString(combo->metaObject()->className());
        }
        return QStringLiteral("list:%1/%2").arg(toolbarName, name);
    }

    QList<Button> collectButtons() {
        QList<Button> buttons;
        QHash<QString, int> index;
        auto addAction = [&](QAction* a, const QString& place) {
            if (a == nullptr || a->isSeparator() || qobject_cast<QWidgetAction*>(a) != nullptr) {
                return;
            }
            const QString id = a->objectName();
            if (id.isEmpty()) {
                return; // a menu's own action, or an item of a list button (listed with the list)
            }
            if (const auto it = index.constFind(id); it != index.constEnd()) {
                if (!buttons[*it].places.contains(place)) {
                    buttons[*it].places << place;
                }
                return;
            }
            index.insert(id, buttons.size());
            buttons.append({a, nullptr, id, {place}, nullptr});
        };
        std::function<void(QMenu*, const QString&)> addMenu = [&](QMenu* menu, const QString& place) {
            if (menu == nullptr) {
                return;
            }
            for (QAction* a : menu->actions()) {
                if (a->menu() != nullptr) {
                    addMenu(a->menu(), place + " > " + labelOf(a));
                }
                else {
                    addAction(a, place);
                }
            }
        };
        QString toolbarName;
        auto addToolButton = [&](QToolButton* button, const QString& place) {
            QAction* main = button->defaultAction();
            if (main != nullptr) {
                addAction(main, place);
            }
            const QString cls = button->metaObject()->className();
            if (cls != "QToolButton" && (button->menu() != nullptr || cls.endsWith("ListButton"))) {
                // a list button: its menu lists named views, UCSs or workspaces
                QString name = QString(button->metaObject()->className()).remove("LC_").remove("Button");
                const QString id = QStringLiteral("list:%1/%2").arg(toolbarName, name);
                if (!index.contains(id)) {
                    index.insert(id, buttons.size());
                    buttons.append({nullptr, nullptr, id, {place}, button});
                }
                return;
            }
            const QString sub = place + " > " + (main != nullptr ? labelOf(main) : clean(button->toolTip()));
            for (QAction* a : button->actions()) {
                if (a != main) {
                    addAction(a, sub);
                }
            }
            addMenu(button->menu(), sub);
        };
        auto addCombo = [&](QComboBox* combo, const QString& toolbarName, const QString& place) {
            const QString id = comboId(combo, toolbarName);
            if (!index.contains(id)) {
                index.insert(id, buttons.size());
                buttons.append({nullptr, combo, id, {place}, nullptr});
            }
        };
        QC_ApplicationWindow* app = appWindow();
        if (app == nullptr) {
            return buttons;
        }
        for (QToolBar* toolbar : app->findChildren<QToolBar*>()) {
            const QString title = clean(toolbar->windowTitle());
            const QString name = toolbar->objectName();
            toolbarName = name;
            for (QAction* a : toolbar->actions()) {
                if (a->isSeparator()) {
                    continue;
                }
                QWidget* w = toolbar->widgetForAction(a);
                if (auto* button = qobject_cast<QToolButton*>(w)) {
                    if (button->defaultAction() == a || button->defaultAction() == nullptr) {
                        if (button->defaultAction() == a) {
                            addAction(a, title);
                        }
                    }
                    addToolButton(button, title);
                }
                else if (w != nullptr) {
                    // a widget in the toolbar: its own buttons and lists
                    if (auto* combo = qobject_cast<QComboBox*>(w)) {
                        addCombo(combo, name, title);
                    }
                    for (QToolButton* inner : w->findChildren<QToolButton*>()) {
                        if (inner->defaultAction() != nullptr || !inner->actions().isEmpty()) {
                            addToolButton(inner, title);
                        }
                    }
                    for (QComboBox* combo : w->findChildren<QComboBox*>()) {
                        addCombo(combo, name, title);
                    }
                }
                else {
                    addAction(a, title);
                }
            }
        }
        return buttons;
    }

    /** A button by id (case does not matter), also menu-only actions of the main window. */
    bool findButton(const QString& wanted, Button& found) {
        const QList<Button> buttons = collectButtons();
        for (const Button& b : buttons) {
            if (b.id.compare(wanted, Qt::CaseInsensitive) == 0) {
                found = b;
                return true;
            }
        }
        if (QC_ApplicationWindow* app = appWindow()) {
            for (QAction* a : app->findChildren<QAction*>()) {
                if (!a->objectName().isEmpty() && a->objectName().compare(wanted, Qt::CaseInsensitive) == 0) {
                    found = {a, nullptr, a->objectName(), {QStringLiteral("menu only")}, nullptr};
                    return true;
                }
            }
        }
        return false;
    }

    QStringList similarIds(const QString& wanted) {
        QStringList out;
        const QString w = wanted.toLower();
        for (const Button& b : collectButtons()) {
            const QString label = b.action ? labelOf(b.action).toLower() : QString();
            if (b.id.toLower().contains(w) || w.contains(b.id.toLower()) || (!label.isEmpty() && label.contains(w))) {
                out << b.id;
            }
            if (out.size() >= 12) {
                break;
            }
        }
        return out;
    }

    QString stateOf(const Button& b) {
        if (b.combo) {
            return QStringLiteral("value \"%1\"").arg(b.combo->currentText());
        }
        if (b.menuButton) {
            return {};
        }
        if (!b.action) {
            return {};
        }
        QStringList parts;
        if (!b.action->isVisible()) {
            parts << "hidden by OpenCAD";
        }
        else if (!b.action->isEnabled()) {
            parts << "disabled";
        }
        if (b.action->isCheckable() && kindOf(b.action) == "toggle") {
            parts << (b.action->isChecked() ? "on" : "off");
        }
        else if (b.action->isChecked()) {
            parts << "active";
        }
        return parts.join(", ");
    }

    // ------------------------------------------------------------------ the drawing's state, before and after

    struct Snapshot {
        QHash<unsigned long long, RS_Entity*> entities; // top-level, not deleted, by id
        QHash<QString, int> types;
        int selected {0};
        QStringList layers;
        QStringList blocks;
        QString activeLayer;
        bool modified {false};
        int windows {0};
        QString document;
        QString view;
        QString running;
        QString prompt;
        QString entityInfo;
        QSet<QWidget*> dialogs;
        // the visible area in drawing units, and drawing units per pixel
        double vx1 {0}, vy1 {0}, vx2 {0}, vy2 {0}, upp {0};
    };

    QSet<QWidget*> visibleDialogs() {
        QSet<QWidget*> set;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (w->isVisible() && (qobject_cast<QDialog*>(w) != nullptr || w->isModal())
                && !QString(w->metaObject()->className()).startsWith("LC_AI")) {
                set.insert(w);
            }
        }
        return set;
    }

    /** The dialog the AI works with: the modal one, else the newest visible dialog. */
    QWidget* currentDialog() {
        if (QWidget* modal = QApplication::activeModalWidget()) {
            if (!QString(modal->metaObject()->className()).startsWith("LC_AI")) {
                return modal;
            }
        }
        QWidget* last = nullptr;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (w->isVisible() && qobject_cast<QDialog*>(w) != nullptr
                && !QString(w->metaObject()->className()).startsWith("LC_AI")) {
                last = w;
            }
        }
        return last;
    }

    QString runningTool(RS_GraphicView* view) {
        if (view == nullptr || !view->hasAction()) {
            return {};
        }
        RS_ActionInterface* action = view->getCurrentAction();
        if (action == nullptr || action == view->getDefaultAction()) {
            return {};
        }
        const QString name = view->getCurrentActionName();
        return name.isEmpty() ? QStringLiteral("a tool") : clean(name);
    }

    Snapshot snapshot() {
        Snapshot s;
        RS_GraphicView* view = LC_AITools::view();
        s.windows = drawingWindows().size();
        s.dialogs = visibleDialogs();
        if (QG_CommandWidget* cmd = commandWidget()) {
            s.prompt = cmd->lCommand->text();
        }
        if (view == nullptr || view->getGraphic() == nullptr) {
            return s;
        }
        RS_Graphic* graphic = view->getGraphic();
        for (RS_Entity* e : *graphic) {
            if (e == nullptr || e->isDeleted()) {
                continue;
            }
            s.entities.insert(e->getId(), e);
            s.types[LC_AITools::typeName(e)]++;
            if (e->isSelected()) {
                s.selected++;
            }
        }
        if (RS_LayerList* layers = graphic->getLayerList()) {
            for (unsigned i = 0; i < layers->count(); ++i) {
                s.layers << layers->at(i)->getName();
            }
            if (layers->getActive() != nullptr) {
                s.activeLayer = layers->getActive()->getName();
            }
        }
        if (RS_BlockList* blocks = graphic->getBlockList()) {
            for (int i = 0; i < blocks->count(); ++i) {
                s.blocks << blocks->at(i)->getName();
            }
        }
        s.modified = graphic->isModified();
        s.document = graphic->getFilename();
        s.view = LC_AITools::describeView(view).section('\n', 0, 0);
        if (LC_GraphicViewport* vp = view->getViewPort(); vp != nullptr && vp->getWidth() > 0 && vp->getFactor().x > 0) {
            const RS_Vector lo = vp->toUCSFromGui(0, vp->getHeight());
            const RS_Vector hi = vp->toUCSFromGui(vp->getWidth(), 0);
            s.vx1 = lo.x;
            s.vy1 = lo.y;
            s.vx2 = hi.x;
            s.vy2 = hi.y;
            s.upp = 1.0 / vp->getFactor().x;
        }
        s.running = runningTool(view);
        return s;
    }

    QStringList listDiff(const QStringList& now, const QStringList& before) {
        QStringList out;
        for (const QString& s : now) {
            if (!before.contains(s)) {
                out << s;
            }
        }
        return out;
    }

    /** What changed between two snapshots, in words. */
    QStringList describeChanges(const Snapshot& before, const Snapshot& after) {
        QStringList lines;
        QList<RS_Entity*> added;
        int removed = 0;
        for (auto it = after.entities.cbegin(); it != after.entities.cend(); ++it) {
            if (!before.entities.contains(it.key())) {
                added << it.value();
            }
        }
        for (auto it = before.entities.cbegin(); it != before.entities.cend(); ++it) {
            if (!after.entities.contains(it.key())) {
                ++removed;
            }
        }
        std::sort(added.begin(), added.end(), [](RS_Entity* a, RS_Entity* b) { return a->getId() < b->getId(); });
        if (!added.isEmpty()) {
            lines << QStringLiteral("Added %1 entit%2:").arg(added.size()).arg(added.size() == 1 ? "y" : "ies");
            for (int i = 0; i < added.size() && i < MAX_REPORTED_ENTITIES; ++i) {
                lines << "  " + LC_AITools::describeEntity(added[i]);
            }
            if (added.size() > MAX_REPORTED_ENTITIES) {
                lines << QStringLiteral("  ... and %1 more (list_entities shows them)").arg(added.size() - MAX_REPORTED_ENTITIES);
            }
        }
        if (removed > 0) {
            lines << QStringLiteral("Removed %1 entit%2 (an edit replaces the entities it changes).")
                         .arg(removed).arg(removed == 1 ? "y" : "ies");
        }
        if (after.selected != before.selected) {
            lines << QStringLiteral("Selection: %1 entities (was %2).").arg(after.selected).arg(before.selected);
        }
        if (const QStringList l = listDiff(after.layers, before.layers); !l.isEmpty()) {
            lines << "New layers: " + l.join(", ");
        }
        if (const QStringList l = listDiff(before.layers, after.layers); !l.isEmpty()) {
            lines << "Removed layers: " + l.join(", ");
        }
        if (after.activeLayer != before.activeLayer) {
            lines << QStringLiteral("Active layer: %1.").arg(after.activeLayer);
        }
        if (const QStringList l = listDiff(after.blocks, before.blocks); !l.isEmpty()) {
            lines << "New blocks: " + l.join(", ");
        }
        if (const QStringList l = listDiff(before.blocks, after.blocks); !l.isEmpty()) {
            lines << "Removed blocks: " + l.join(", ");
        }
        if (after.windows != before.windows) {
            lines << QStringLiteral("Open drawings: %1 (was %2).").arg(after.windows).arg(before.windows);
        }
        if (after.document != before.document && !after.document.isEmpty()) {
            lines << QStringLiteral("Drawing file: %1.").arg(after.document);
        }
        if (after.view != before.view && !after.view.isEmpty()) {
            lines << after.view;
        }
        return lines;
    }

    // ------------------------------------------------------------------ dialogs

    struct Field {
        QPointer<QWidget> widget;
        QString kind;
        QString label;
    };

    QString labelForWidget(QWidget* w, QWidget* dialog) {
        for (QLabel* label : dialog->findChildren<QLabel*>()) {
            if (label->buddy() == w && !clean(label->text()).isEmpty()) {
                return clean(label->text());
            }
        }
        if (auto* box = qobject_cast<QAbstractButton*>(w); box != nullptr && !clean(box->text()).isEmpty() && clean(box->text()) != "...") {
            return clean(box->text());
        }
        if (QWidget* parent = w->parentWidget()) {
            if (QLayout* layout = parent->layout()) {
                std::function<QString(QLayout*)> search = [&](QLayout* l) -> QString {
                    if (auto* form = qobject_cast<QFormLayout*>(l)) {
                        if (QWidget* lab = form->labelForField(w)) {
                            if (auto* ql = qobject_cast<QLabel*>(lab)) {
                                return clean(ql->text());
                            }
                        }
                    }
                    const int i = l->indexOf(w);
                    if (i >= 0) {
                        if (auto* grid = qobject_cast<QGridLayout*>(l)) {
                            int row = 0, col = 0, rs = 0, cs = 0;
                            grid->getItemPosition(i, &row, &col, &rs, &cs);
                            for (int c = col - 1; c >= 0; --c) {
                                if (QLayoutItem* item = grid->itemAtPosition(row, c)) {
                                    if (auto* ql = qobject_cast<QLabel*>(item->widget()); ql != nullptr && !clean(ql->text()).isEmpty()) {
                                        return clean(ql->text());
                                    }
                                }
                            }
                        }
                        else if (i > 0) {
                            if (auto* ql = qobject_cast<QLabel*>(l->itemAt(i - 1)->widget()); ql != nullptr && !clean(ql->text()).isEmpty()) {
                                return clean(ql->text());
                            }
                        }
                        return {};
                    }
                    for (int k = 0; k < l->count(); ++k) {
                        if (QLayout* child = l->itemAt(k)->layout()) {
                            const QString found = search(child);
                            if (!found.isEmpty()) {
                                return found;
                            }
                        }
                    }
                    return {};
                };
                const QString found = search(layout);
                if (!found.isEmpty()) {
                    return found;
                }
            }
            if (auto* group = qobject_cast<QGroupBox*>(parent); group != nullptr && !clean(group->title()).isEmpty()) {
                return clean(group->title());
            }
        }
        if (auto* line = qobject_cast<QLineEdit*>(w); line != nullptr && !line->placeholderText().isEmpty()) {
            return clean(line->placeholderText());
        }
        if (!w->toolTip().isEmpty()) {
            return clean(w->toolTip()).left(60);
        }
        return {};
    }

    bool shownIn(QWidget* w, QWidget* dialog) {
        // visible as far as the dialog is concerned (on a hidden tab page counts as shown)
        for (QWidget* p = w; p != nullptr && p != dialog; p = p->parentWidget()) {
            if (p->isHidden()) {
                auto* tabs = qobject_cast<QTabWidget*>(p->parentWidget() != nullptr ? p->parentWidget()->parentWidget() : nullptr);
                if (tabs != nullptr && tabs->indexOf(p) >= 0) {
                    continue; // a page of a tab widget (a removed page is not)
                }
                return false;
            }
        }
        return true;
    }

    QList<Field> dialogFields(QWidget* dialog) {
        QList<Field> fields;
        for (QWidget* w : dialog->findChildren<QWidget*>()) {
            if (!shownIn(w, dialog)) {
                continue;
            }
            QString kind;
            if (qobject_cast<QLineEdit*>(w) != nullptr) {
                if (qobject_cast<QAbstractSpinBox*>(w->parentWidget()) != nullptr || qobject_cast<QComboBox*>(w->parentWidget()) != nullptr) {
                    continue; // the editor inside a spin box or combo box
                }
                kind = "text";
            }
            else if (qobject_cast<QDoubleSpinBox*>(w) != nullptr) {
                kind = "number";
            }
            else if (qobject_cast<QSpinBox*>(w) != nullptr) {
                kind = "integer";
            }
            else if (qobject_cast<QComboBox*>(w) != nullptr) {
                kind = "choice";
            }
            else if (qobject_cast<QCheckBox*>(w) != nullptr) {
                kind = "checkbox";
            }
            else if (qobject_cast<QRadioButton*>(w) != nullptr) {
                kind = "radio";
            }
            else if (qobject_cast<QPlainTextEdit*>(w) != nullptr || qobject_cast<QTextEdit*>(w) != nullptr) {
                kind = "multiline";
            }
            else if (qobject_cast<QListWidget*>(w) != nullptr) {
                kind = "list";
            }
            else if (qobject_cast<QTreeWidget*>(w) != nullptr) {
                kind = "tree";
            }
            else if (qobject_cast<QTabWidget*>(w) != nullptr) {
                kind = "tabs";
            }
            else if (qobject_cast<QAbstractSlider*>(w) != nullptr && qobject_cast<QAbstractSpinBox*>(w->parentWidget()) == nullptr
                     && QString(w->metaObject()->className()) == "QSlider") {
                kind = "slider";
            }
            else if (auto* box = qobject_cast<QGroupBox*>(w); box != nullptr && box->isCheckable()) {
                kind = "checkbox";
            }
            else if (auto* tb = qobject_cast<QToolButton*>(w); tb != nullptr && tb->isCheckable() && tb->defaultAction() == nullptr
                     && !tb->objectName().startsWith("qt_")) {
                kind = "toggle button";
            }
            else if (auto* view = qobject_cast<QAbstractItemView*>(w); view != nullptr && qobject_cast<QHeaderView*>(w) == nullptr
                     && view->model() != nullptr) {
                kind = qobject_cast<QTreeView*>(w) != nullptr && qobject_cast<QTableView*>(w) == nullptr ? "tree view" : "table";
            }
            if (!kind.isEmpty()) {
                fields.append({w, kind, labelForWidget(w, dialog)});
            }
        }
        return fields;
    }

    /** A button's name for the AI: its text, else its tooltip, else its object name (icon-only buttons). */
    QString buttonName(const QAbstractButton* b) {
        QString name = clean(b->text());
        if (name.isEmpty() || name == "...") {
            const QString tip = clean(QString(b->toolTip()).remove(QRegularExpression(QStringLiteral("<[^>]*>"))));
            if (!tip.isEmpty()) {
                name = tip.left(60);
            }
            else if (!b->objectName().isEmpty() && !b->objectName().startsWith("qt_")) {
                name = b->objectName();
            }
        }
        return name;
    }

    QList<QAbstractButton*> dialogButtons(QWidget* dialog) {
        QList<QAbstractButton*> list;
        for (QAbstractButton* b : dialog->findChildren<QAbstractButton*>()) {
            if (qobject_cast<QCheckBox*>(b) != nullptr || qobject_cast<QRadioButton*>(b) != nullptr) {
                continue;
            }
            if (auto* tb = qobject_cast<QToolButton*>(b); tb != nullptr && tb->isCheckable() && tb->defaultAction() == nullptr) {
                continue; // a toggle button: listed as a field
            }
            if (b->objectName().startsWith("qt_") || qobject_cast<QAbstractSpinBox*>(b->parentWidget()) != nullptr
                || QString(b->parentWidget() != nullptr ? b->parentWidget()->metaObject()->className() : "") == "QTabBar") {
                continue; // Qt's own parts (scroll arrows, toolbar extension)
            }
            if (!shownIn(b, dialog) || buttonName(b).isEmpty()) {
                continue;
            }
            list << b;
        }
        return list;
    }

    // ------------------------------------------------------------------ tables and trees (item views)

    /** What a cell shows: its text, else its state in words (icon columns), its tooltip, or a check box. */
    QString cellText(const QModelIndex& index) {
        QString text = clean(index.data(Qt::DisplayRole).toString());
        if (text.isEmpty()) {
            text = clean(index.data(Qt::AccessibleTextRole).toString());
        }
        if (text.isEmpty()) {
            text = clean(QString(index.data(Qt::ToolTipRole).toString()).remove(QRegularExpression(QStringLiteral("<[^>]*>")))).left(40);
        }
        const QVariant check = index.data(Qt::CheckStateRole);
        if (check.isValid()) {
            text = QString(check.toInt() == Qt::Checked ? "[x] " : check.toInt() == Qt::PartiallyChecked ? "[~] " : "[ ] ") + text;
        }
        return text;
    }

    QString headerText(const QAbstractItemModel* model, const int column) {
        const QString text = clean(model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString());
        return text == QString::number(column + 1) ? QString() : text; // Qt's default numbering says nothing
    }

    /** How a row is called: the text it shows (its name), else what its first cell says. */
    QString rowLabel(const QModelIndex& index) {
        const QAbstractItemModel* model = index.model();
        if (model == nullptr) {
            return {};
        }
        for (int c = model->columnCount(index.parent()) - 1; c >= 0; --c) {
            const QString text = clean(index.siblingAtColumn(c).data(Qt::DisplayRole).toString());
            if (!text.isEmpty()) {
                return text;
            }
        }
        return cellText(index.siblingAtColumn(0));
    }

    /** The rows of a table or tree: "r3: a | b | c", children indented, [selected] / [current] marked. */
    QString itemViewText(QAbstractItemView* view, const int maxRows) {
        QAbstractItemModel* model = view->model();
        if (model == nullptr) {
            return {};
        }
        auto* tree = qobject_cast<QTreeView*>(view);
        auto* table = qobject_cast<QTableView*>(view);
        QStringList headers;
        for (int c = 0; c < model->columnCount(); ++c) {
            if (table != nullptr && table->isColumnHidden(c)) {
                continue;
            }
            if (tree != nullptr && tree->isColumnHidden(c)) {
                continue;
            }
            const QString h = headerText(model, c);
            headers << QStringLiteral("c%1%2").arg(c).arg(h.isEmpty() ? QString() : "=" + h);
        }
        QStringList rows;
        int shown = 0;
        int total = 0;
        std::function<void(const QModelIndex&, int)> walk = [&](const QModelIndex& parent, const int depth) {
            for (int r = 0; r < model->rowCount(parent); ++r) {
                if (table != nullptr && table->isRowHidden(r)) {
                    continue;
                }
                if (tree != nullptr && tree->isRowHidden(r, parent)) {
                    continue;
                }
                ++total;
                const QModelIndex first = model->index(r, 0, parent);
                if (shown < maxRows) {
                    QStringList cells;
                    for (int c = 0; c < model->columnCount(parent); ++c) {
                        if ((table != nullptr && table->isColumnHidden(c)) || (tree != nullptr && tree->isColumnHidden(c))) {
                            continue;
                        }
                        cells << cellText(model->index(r, c, parent));
                    }
                    QStringList marks;
                    if (view->selectionModel() != nullptr && view->selectionModel()->isRowSelected(r, parent)) {
                        marks << "selected";
                    }
                    if (view->currentIndex().isValid() && view->currentIndex().row() == r && view->currentIndex().parent() == parent) {
                        marks << "current";
                    }
                    if (tree != nullptr && model->hasChildren(first)) {
                        marks << (tree->isExpanded(first) ? "expanded" : "collapsed");
                    }
                    rows << QStringLiteral("%1r%2: %3%4").arg(QString(depth * 2, ' ')).arg(r).arg(cells.join(" | "),
                                                                                            marks.isEmpty() ? QString() : "  [" + marks.join(", ") + "]");
                    ++shown;
                }
                if (tree != nullptr && model->hasChildren(first) && tree->isExpanded(first)) {
                    walk(first, depth + 1);
                }
            }
        };
        walk(QModelIndex(), 0);
        QString out = QStringLiteral("%1 row(s); columns %2").arg(total).arg(headers.join(", "));
        for (const QString& r : rows) {
            out += "\n      " + r;
        }
        if (total > shown) {
            out += QStringLiteral("\n      ... %1 more").arg(total - shown);
        }
        return out;
    }

    /**
     * A row by its text in any column (exact first, then the only partial match), a number ("3" = row 3), or a tree path
     * ("Group > Layer"); column by number or header text (default: the column holding the matched text).
     */
    QModelIndex findItem(QAbstractItemView* view, const QString& rowSpec, const QString& columnSpec, QString& error) {
        QAbstractItemModel* model = view->model();
        if (model == nullptr) {
            error = "this list has no rows";
            return {};
        }
        const auto columnOf = [&](const QModelIndex& parent) -> int {
            if (columnSpec.isEmpty()) {
                return -1;
            }
            bool isNumber = false;
            const int n = QString(columnSpec).remove('c').toInt(&isNumber);
            if (isNumber) {
                return n;
            }
            for (int c = 0; c < model->columnCount(parent); ++c) {
                if (norm(headerText(model, c)) == norm(columnSpec)) {
                    return c;
                }
            }
            return -2;
        };
        QModelIndex parent;
        const QStringList path = rowSpec.split(" > ", Qt::SkipEmptyParts);
        QModelIndex found;
        int foundColumn = 0;
        for (int level = 0; level < path.size(); ++level) {
            const QString wanted = path[level].trimmed();
            found = QModelIndex();
            bool isNumber = false;
            const int asNumber = QString(wanted).remove(QRegularExpression(QStringLiteral("^r"))).toInt(&isNumber);
            QList<QModelIndex> partial;
            for (int r = 0; !found.isValid() && r < model->rowCount(parent); ++r) {
                for (int c = 0; c < model->columnCount(parent); ++c) {
                    const QModelIndex idx = model->index(r, c, parent);
                    const QStringList candidates = {clean(idx.data(Qt::DisplayRole).toString()), clean(idx.data(Qt::UserRole).toString()),
                                                    clean(idx.data(Qt::AccessibleTextRole).toString())};
                    for (const QString& t : candidates) {
                        if (t.isEmpty()) {
                            continue;
                        }
                        if (norm(t) == norm(wanted)) {
                            found = idx;
                            foundColumn = c;
                            break;
                        }
                        if (norm(t).contains(norm(wanted))) {
                            partial << idx;
                        }
                    }
                    if (found.isValid()) {
                        break;
                    }
                }
            }
            if (!found.isValid() && isNumber && path.size() == 1 && asNumber >= 0 && asNumber < model->rowCount(parent)) {
                found = model->index(asNumber, 0, parent); // no row shows that text: a row number ("r3" or "3")
                partial.clear();
            }
            if (!found.isValid()) {
                QSet<int> rowsHit;
                for (const QModelIndex& idx : std::as_const(partial)) {
                    rowsHit.insert(idx.row());
                }
                if (rowsHit.size() == 1) {
                    found = partial.first();
                    foundColumn = found.column();
                }
            }
            if (!found.isValid()) {
                error = QStringLiteral("no row \"%1\"").arg(wanted);
                return {};
            }
            if (level + 1 < path.size()) {
                if (auto* tree = qobject_cast<QTreeView*>(view)) {
                    tree->expand(found.siblingAtColumn(0));
                }
                parent = found.siblingAtColumn(0);
            }
        }
        if (!found.isValid()) {
            error = "say which row";
            return {};
        }
        const int column = columnOf(found.parent());
        if (column == -2) {
            error = QStringLiteral("no column \"%1\"").arg(columnSpec);
            return {};
        }
        return found.siblingAtColumn(column >= 0 ? column : foundColumn);
    }

    /** Clicks a cell like a person: scrolls it into view, then mouse press and release (and double) at its middle. */
    bool clickItem(QAbstractItemView* view, const QModelIndex& index, const Qt::MouseButton button, const Qt::KeyboardModifiers mods,
                   const bool twice, QString& error) {
        view->scrollTo(index);
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        const QRect rect = view->visualRect(index);
        if (!rect.isValid() || !view->viewport()->rect().intersects(rect)) {
            error = "that row is not visible in the list";
            return false;
        }
        const LC_AIControl::AIInput aiInput;
        QWidget* port = view->viewport();
        const QPointF at = QRectF(rect).center();
        const QPointF global = port->mapToGlobal(at);
        QMouseEvent move(QEvent::MouseMove, at, global, Qt::NoButton, Qt::NoButton, mods);
        QApplication::sendEvent(port, &move);
        QMouseEvent press(QEvent::MouseButtonPress, at, global, button, button, mods);
        QApplication::sendEvent(port, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, at, global, button, Qt::NoButton, mods);
        QApplication::sendEvent(port, &release);
        if (twice) {
            QMouseEvent dbl(QEvent::MouseButtonDblClick, at, global, button, button, mods);
            QApplication::sendEvent(port, &dbl);
            QMouseEvent release2(QEvent::MouseButtonRelease, at, global, button, Qt::NoButton, mods);
            QApplication::sendEvent(port, &release2);
        }
        return true;
    }

    // ------------------------------------------------------------------ menus that pop up (right-click menus)

    /** The entries of a menu and its submenus: "Modify > Offset", disabled ones marked. */
    void menuEntries(QMenu* menu, const QString& prefix, QStringList& out, const int depth = 0) {
        if (menu == nullptr || depth > 4) {
            return;
        }
        emit menu->aboutToShow(); // some menus fill themselves when they open
        for (QAction* a : menu->actions()) {
            if (a->isSeparator() || !a->isVisible() || clean(a->text()).isEmpty()) {
                continue;
            }
            const QString path = prefix.isEmpty() ? clean(a->text()) : prefix + " > " + clean(a->text());
            if (a->menu() != nullptr) {
                menuEntries(a->menu(), path, out, depth + 1);
            }
            else {
                out << path + (a->isEnabled() ? QString() : " (disabled)")
                           + (a->isCheckable() ? (a->isChecked() ? " [on]" : " [off]") : QString());
            }
        }
    }

    void closePopups() {
        for (int i = 0; i < 6; ++i) {
            QWidget* popup = QApplication::activePopupWidget();
            if (popup == nullptr) {
                break;
            }
            popup->close();
        }
    }

    /**
     * Waits for a popup menu to open, then chooses path ("Modify > Offset") in it like a person: highlights each entry
     * and opens submenus with the keyboard, then presses Return on the last one (so menus that act on the entry under
     * the mouse, and actions owned by the menu, behave as for a click). Without a path it lists the entries and closes
     * the menu. done(error, entries) runs once.
     */
    void chooseInPopup(const QStringList& path, std::function<void(const QString&, const QStringList&)> done, const int level = 0,
                       const int tries = 30) {
        auto* popup = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (popup == nullptr) {
            if (tries <= 0) {
                done(QStringLiteral("no menu opened"), {});
                return;
            }
            QTimer::singleShot(50, qApp, [path, done, level, tries] { chooseInPopup(path, done, level, tries - 1); });
            return;
        }
        if (path.isEmpty()) {
            QStringList entries;
            menuEntries(popup, QString(), entries);
            closePopups();
            done(QString(), entries);
            return;
        }
        QAction* pick = nullptr;
        QList<QAction*> starts;
        for (QAction* a : popup->actions()) {
            if (a->isSeparator() || !a->isVisible()) {
                continue;
            }
            if (norm(a->text()) == norm(path[level])) {
                pick = a;
                break;
            }
            if (norm(a->text()).startsWith(norm(path[level]))) {
                starts << a;
            }
        }
        if (pick == nullptr && starts.size() == 1) {
            pick = starts.first();
        }
        if (pick == nullptr || !pick->isEnabled()) {
            QStringList entries;
            menuEntries(popup, QString(), entries);
            closePopups();
            done(pick == nullptr ? QStringLiteral("no entry \"%1\" in the menu").arg(path[level])
                                 : QStringLiteral("\"%1\" is disabled").arg(path[level]),
                 entries);
            return;
        }
        const LC_AIControl::AIInput aiInput;
        popup->setActiveAction(pick);
        if (pick->menu() != nullptr && level + 1 < path.size()) {
            QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
            QApplication::sendEvent(popup, &right);
            QPointer<QMenu> parentMenu = popup;
            QTimer::singleShot(80, qApp, [path, done, level, parentMenu] {
                if (QApplication::activePopupWidget() == parentMenu) { // the submenu did not open: open it by its action
                    if (parentMenu && parentMenu->activeAction() != nullptr && parentMenu->activeAction()->menu() != nullptr) {
                        parentMenu->activeAction()->menu()->popup(parentMenu->mapToGlobal(parentMenu->actionGeometry(parentMenu->activeAction()).topRight()));
                    }
                }
                chooseInPopup(path, done, level + 1, 20);
            });
            return;
        }
        // chosen from the event loop: an entry that opens a modal dialog does not return before the dialog closes, and
        // the caller has to be watching by then to report the dialog
        QPointer<QMenu> menu = popup;
        QTimer::singleShot(0, qApp, [menu] {
            if (menu) {
                const LC_AIControl::AIInput input;
                QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                QApplication::sendEvent(menu, &enter);
            }
        });
        done(QString(), {});
    }

    QString valueOf(const Field& f) {
        QWidget* w = f.widget;
        if (w == nullptr) {
            return {};
        }
        if (auto* e = qobject_cast<QLineEdit*>(w)) {
            return QStringLiteral("\"%1\"%2").arg(e->echoMode() == QLineEdit::Normal ? e->text() : QStringLiteral("(hidden)"),
                                                 e->isReadOnly() ? QStringLiteral(" (read-only)") : QString());
        }
        if (auto* d = qobject_cast<QDoubleSpinBox*>(w)) {
            return QStringLiteral("%1 (%2 .. %3)").arg(d->value()).arg(d->minimum()).arg(d->maximum());
        }
        if (auto* s = qobject_cast<QSpinBox*>(w)) {
            return QStringLiteral("%1 (%2 .. %3)").arg(s->value()).arg(s->minimum()).arg(s->maximum());
        }
        if (auto* c = qobject_cast<QComboBox*>(w)) {
            QStringList items;
            for (int i = 0; i < c->count() && i < MAX_LIST_ITEMS; ++i) {
                items << c->itemText(i);
            }
            if (c->count() > MAX_LIST_ITEMS) {
                items << QStringLiteral("... %1 more").arg(c->count() - MAX_LIST_ITEMS);
            }
            return QStringLiteral("\"%1\"%2 options: %3").arg(c->currentText(), c->isEditable() ? " (editable)" : "", items.join(" | "));
        }
        if (auto* g = qobject_cast<QGroupBox*>(w)) {
            return g->isChecked() ? "checked" : "unchecked";
        }
        if (auto* b = qobject_cast<QAbstractButton*>(w)) {
            return b->isChecked() ? "checked" : "unchecked";
        }
        if (auto* p = qobject_cast<QPlainTextEdit*>(w)) {
            return QStringLiteral("\"%1\"").arg(p->toPlainText().left(300));
        }
        if (auto* t = qobject_cast<QTextEdit*>(w)) {
            return QStringLiteral("\"%1\"").arg(t->toPlainText().left(300));
        }
        if (auto* l = qobject_cast<QListWidget*>(w)) {
            QStringList items;
            for (int i = 0; i < l->count() && i < MAX_LIST_ITEMS; ++i) {
                items << (l->item(i)->isSelected() ? "[" + l->item(i)->text() + "]" : l->item(i)->text());
            }
            return "items: " + items.join(" | ");
        }
        if (auto* t = qobject_cast<QTreeWidget*>(w)) {
            QStringList items;
            for (int i = 0; i < t->topLevelItemCount() && i < MAX_LIST_ITEMS; ++i) {
                items << t->topLevelItem(i)->text(0);
            }
            return "items: " + items.join(" | ");
        }
        if (auto* tabs = qobject_cast<QTabWidget*>(w)) {
            QStringList names;
            for (int i = 0; i < tabs->count(); ++i) {
                names << (i == tabs->currentIndex() ? "[" + clean(tabs->tabText(i)) + "]" : clean(tabs->tabText(i)));
            }
            return "tabs: " + names.join(" | ");
        }
        if (auto* s = qobject_cast<QAbstractSlider*>(w)) {
            return QStringLiteral("%1 (%2 .. %3)").arg(s->value()).arg(s->minimum()).arg(s->maximum());
        }
        if (auto* view = qobject_cast<QAbstractItemView*>(w)) {
            return itemViewText(view, MAX_LIST_ITEMS);
        }
        return {};
    }

    QString describeDialog(QWidget* dialog) {
        if (dialog == nullptr) {
            return QStringLiteral("No dialog is open.");
        }
        QStringList lines;
        lines << QStringLiteral("Dialog \"%1\" (%2%3) is open.")
                     .arg(clean(dialog->windowTitle()), dialog->metaObject()->className(), dialog->isModal() ? ", modal" : "");
        if (auto* box = qobject_cast<QMessageBox*>(dialog)) {
            lines << "Message: " + clean(box->text());
            if (!box->informativeText().isEmpty()) {
                lines << clean(box->informativeText());
            }
        }
        if (auto* files = qobject_cast<QFileDialog*>(dialog)) {
            lines << QStringLiteral("File dialog: folder %1, mode %2. Answer it with handle_dialog file_path, then press "
                                    "\"%3\".")
                         .arg(files->directory().absolutePath(),
                              files->acceptMode() == QFileDialog::AcceptSave ? "save" : "open",
                              files->acceptMode() == QFileDialog::AcceptSave ? "Save" : "Open");
            lines << "Filters: " + files->nameFilters().join(" | ");
            return lines.join('\n');
        }
        QStringList texts;
        QSet<QString> seen;
        for (QLabel* label : dialog->findChildren<QLabel*>()) {
            const QString t = clean(label->text());
            if (!shownIn(label, dialog) || label->buddy() != nullptr || t.isEmpty() || seen.contains(t) || t.size() > 300) {
                continue;
            }
            seen.insert(t);
            texts << t;
        }
        if (!texts.isEmpty()) {
            lines << "Texts: " + texts.mid(0, 30).join(" | ");
        }
        const QList<Field> fields = dialogFields(dialog);
        if (!fields.isEmpty()) {
            lines << "Fields (set them by field id or label):";
            for (int i = 0; i < fields.size(); ++i) {
                const Field& f = fields[i];
                QString name = f.widget->objectName();
                if (name.startsWith("qt_")) {
                    name.clear();
                }
                lines << QStringLiteral("  f%1 [%2] %3%4 = %5%6")
                             .arg(i)
                             .arg(f.kind, f.label.isEmpty() ? QStringLiteral("(no label)") : "\"" + f.label + "\"",
                                  name.isEmpty() ? QString() : " name=" + name, valueOf(f),
                                  f.widget->isEnabled() ? QString() : QStringLiteral(" (disabled)"));
            }
        }
        QStringList buttons;
        for (QAbstractButton* b : dialogButtons(dialog)) {
            const bool iconOnly = clean(b->text()).isEmpty() || clean(b->text()) == "...";
            buttons << "\"" + buttonName(b) + "\"" + (iconOnly && !b->objectName().isEmpty() && buttonName(b) != b->objectName() ? " (name " + b->objectName() + ")" : QString())
                           + (b->isEnabled() ? "" : " (disabled)");
        }
        if (!buttons.isEmpty()) {
            lines << "Buttons: " + buttons.join(", ");
        }
        return lines.join('\n');
    }

    bool setField(const Field& f, const QJsonValue& value, QString& error) {
        QWidget* w = f.widget;
        if (w == nullptr) {
            error = "the field is gone";
            return false;
        }
        const QString text = value.isString() ? value.toString() : value.isBool() ? (value.toBool() ? "true" : "false")
                                                                                   : QString::number(value.toDouble(), 'g', 15);
        auto truthy = [&] { return value.isBool() ? value.toBool() : QStringList{"true", "1", "yes", "on", "checked"}.contains(text.toLower()); };
        if (auto* e = qobject_cast<QLineEdit*>(w)) {
            if (e->isReadOnly()) {
                error = "this field is read-only";
                return false;
            }
            e->setFocus();
            e->setText(text);
            emit e->textEdited(text);
            emit e->editingFinished();
            return true;
        }
        if (auto* d = qobject_cast<QDoubleSpinBox*>(w)) {
            bool ok = false;
            const double v = value.isDouble() ? value.toDouble() : text.toDouble(&ok);
            if (!value.isDouble() && !ok) {
                error = "a number is needed";
                return false;
            }
            d->setValue(v);
            emit d->editingFinished();
            return true;
        }
        if (auto* s = qobject_cast<QSpinBox*>(w)) {
            bool ok = false;
            const int v = value.isDouble() ? qRound(value.toDouble()) : text.toInt(&ok);
            if (!value.isDouble() && !ok) {
                error = "a whole number is needed";
                return false;
            }
            s->setValue(v);
            emit s->editingFinished();
            return true;
        }
        if (auto* c = qobject_cast<QComboBox*>(w)) {
            // an exact option (text or data) first; an editable combo takes any text; a partial match only when it is
            // the only one; a position only as "#n" (a number is a value, never an index)
            int i = c->findText(text, Qt::MatchFixedString);
            if (i < 0) {
                i = c->findData(text, Qt::DisplayRole, Qt::MatchFixedString);
            }
            if (i < 0) {
                bool ok = false;
                const double number = text.toDouble(&ok);
                for (int k = 0; ok && i < 0 && k < c->count(); ++k) {
                    bool itemOk = false;
                    const double item = c->itemText(k).toDouble(&itemOk);
                    if (itemOk && qFuzzyCompare(item + 1.0, number + 1.0)) {
                        i = k; // "2" picks "2.0"
                    }
                }
            }
            if (i < 0 && text.startsWith('#')) {
                bool ok = false;
                const int k = text.mid(1).toInt(&ok);
                i = ok && k >= 0 && k < c->count() ? k : -1;
            }
            if (i < 0 && c->isEditable()) {
                c->setEditText(text);
                if (c->lineEdit() != nullptr) {
                    emit c->lineEdit()->editingFinished();
                }
                emit c->currentTextChanged(text);
                return true;
            }
            if (i < 0) {
                int found = -1;
                int hits = 0;
                for (int k = 0; k < c->count(); ++k) {
                    if (c->itemText(k).contains(text, Qt::CaseInsensitive)) {
                        found = k;
                        ++hits;
                    }
                }
                i = hits == 1 ? found : -1;
            }
            if (i < 0 || i >= c->count()) {
                QStringList options;
                for (int k = 0; k < c->count() && k < MAX_LIST_ITEMS; ++k) {
                    options << c->itemText(k);
                }
                error = QStringLiteral("\"%1\" is not one of the options (%2)").arg(text, options.join(" | "));
                return false;
            }
            c->setCurrentIndex(i);
            // activated may open a dialog (a colour box's "Custom..."): let it run from the event loop
            QPointer<QComboBox> box = c;
            QTimer::singleShot(0, c, [box, i] {
                if (box) {
                    emit box->activated(i);
                    emit box->textActivated(box->itemText(i));
                }
            });
            return true;
        }
        if (auto* g = qobject_cast<QGroupBox*>(w)) {
            g->setChecked(truthy());
            return true;
        }
        if (auto* r = qobject_cast<QRadioButton*>(w)) {
            if (truthy() && !r->isChecked()) {
                r->click();
            }
            return true;
        }
        if (auto* b = qobject_cast<QAbstractButton*>(w)) {
            if (b->isChecked() != truthy()) {
                b->click();
            }
            return true;
        }
        if (auto* p = qobject_cast<QPlainTextEdit*>(w)) {
            p->setPlainText(text);
            return true;
        }
        if (auto* t = qobject_cast<QTextEdit*>(w)) {
            t->setPlainText(text);
            return true;
        }
        if (auto* l = qobject_cast<QListWidget*>(w)) {
            const auto items = l->findItems(text, Qt::MatchFixedString);
            if (items.isEmpty()) {
                error = QStringLiteral("no item \"%1\"").arg(text);
                return false;
            }
            l->setCurrentItem(items.first());
            return true;
        }
        if (auto* t = qobject_cast<QTreeWidget*>(w)) {
            const auto items = t->findItems(text, Qt::MatchFixedString | Qt::MatchRecursive);
            if (items.isEmpty()) {
                error = QStringLiteral("no item \"%1\"").arg(text);
                return false;
            }
            t->setCurrentItem(items.first());
            return true;
        }
        if (auto* tabs = qobject_cast<QTabWidget*>(w)) {
            for (int i = 0; i < tabs->count(); ++i) {
                if (norm(tabs->tabText(i)) == norm(text)) {
                    tabs->setCurrentIndex(i);
                    return true;
                }
            }
            error = QStringLiteral("no tab \"%1\"").arg(text);
            return false;
        }
        if (auto* view = qobject_cast<QAbstractItemView*>(w)) {
            // choose the row the way a person does: a click on it
            const QModelIndex index = findItem(view, text, QString(), error);
            return index.isValid() && clickItem(view, index, Qt::LeftButton, Qt::NoModifier, false, error);
        }
        if (auto* s = qobject_cast<QAbstractSlider*>(w)) {
            s->setValue(qRound(value.isDouble() ? value.toDouble() : text.toDouble()));
            return true;
        }
        error = "this field cannot be set";
        return false;
    }

    /** A dialog button by its text ("OK", "&Save" or "save"), its object name, or a role word (ok, cancel, yes, no). */
    QAbstractButton* findDialogButton(QWidget* dialog, const QString& wanted) {
        const QString w = norm(wanted);
        const QList<QAbstractButton*> buttons = dialogButtons(dialog);
        for (QAbstractButton* b : buttons) {
            if (norm(b->text()) == w || norm(buttonName(b)) == w || b->objectName().compare(wanted, Qt::CaseInsensitive) == 0) {
                return b;
            }
        }
        for (QDialogButtonBox* box : dialog->findChildren<QDialogButtonBox*>()) {
            for (QAbstractButton* b : box->buttons()) {
                const QDialogButtonBox::ButtonRole role = box->buttonRole(b);
                if ((w == "ok" || w == "accept" || w == "save" || w == "open" || w == "yes" || w == "apply")
                    && (role == QDialogButtonBox::AcceptRole || role == QDialogButtonBox::YesRole)) {
                    return b;
                }
                if ((w == "cancel" || w == "close" || w == "no" || w == "reject")
                    && (role == QDialogButtonBox::RejectRole || role == QDialogButtonBox::NoRole)) {
                    return b;
                }
            }
        }
        for (QAbstractButton* b : buttons) {
            if (!w.isEmpty() && norm(buttonName(b)).startsWith(w)) {
                return b;
            }
        }
        return nullptr;
    }

    // Qt's own file dialogs can be filled in by the AI; the system's cannot
    bool g_forcedQtDialogs = false;
    bool g_nativeDialogsBefore = false;

    void useQtDialogs() {
        if (!g_forcedQtDialogs) {
            g_nativeDialogsBefore = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
            QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
            g_forcedQtDialogs = true;
        }
    }

    void restoreDialogsWhenDone() {
        if (g_forcedQtDialogs && currentDialog() == nullptr) {
            QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, g_nativeDialogsBefore);
            g_forcedQtDialogs = false;
        }
    }

    // ------------------------------------------------------------------ mouse and keys in the drawing

    /** Where a drawing point is on the view, in whole pixels (the view reads mouse positions as whole pixels). */
    bool toScreen(RS_GraphicView* view, const double x, const double y, QPointF& local, QString& error) {
        if (view == nullptr || view->getViewPort() == nullptr) {
            error = "No drawing is open.";
            return false;
        }
        if (!std::isfinite(x) || !std::isfinite(y)) {
            error = "x and y must be numbers.";
            return false;
        }
        LC_GraphicViewport* vp = view->getViewPort();
        double ux = 0;
        double uy = 0;
        vp->toUI(RS_Vector(x, y), ux, uy);
        constexpr int MARGIN = 4;
        if (ux < MARGIN || uy < MARGIN || ux >= view->width() - MARGIN || uy >= view->height() - MARGIN) {
            // scroll the drawing (same zoom) so the point is in the middle, as a person would before clicking;
            // this also copes with the user having panned away meanwhile
            vp->setOffsetAndFactor(vp->getOffsetX() + qRound(view->width() / 2.0 - ux), vp->getOffsetY() + qRound(uy - view->height() / 2.0),
                                   vp->getFactor().x);
            view->redraw(RS2::RedrawAll);
            vp->toUI(RS_Vector(x, y), ux, uy);
            if (ux < 0 || uy < 0 || ux >= view->width() || uy >= view->height()) {
                error = QStringLiteral("The point %1,%2 cannot be brought into view.").arg(x).arg(y);
                return false;
            }
        }
        local = QPointF(std::round(ux), std::round(uy));
        return true;
    }

    bool moveTo(RS_GraphicView* view, const double x, const double y, QString& error) {
        const LC_AIControl::AIInput aiInput; // passes the lock that holds back the user's input
        QPointF local;
        if (!toScreen(view, x, y, local, error)) {
            return false;
        }
        QMouseEvent move(QEvent::MouseMove, local, view->mapToGlobal(local), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(view, &move);
        return true;
    }

    /** Press at x1,y1, move there to x2,y2 with the button held (in small steps, like a hand), release. */
    bool dragAt(RS_GraphicView* view, const double x1, const double y1, const double x2, const double y2,
                const Qt::MouseButton button, const Qt::KeyboardModifiers mods, QString& error) {
        const LC_AIControl::AIInput aiInput; // passes the lock that holds back the user's input
        QPointF a;
        QPointF b;
        if (!toScreen(view, (x1 + x2) / 2, (y1 + y2) / 2, a, error)) { // brings the middle into view if needed
            return false;
        }
        if (!toScreen(view, x1, y1, a, error) || !toScreen(view, x2, y2, b, error)) {
            return false;
        }
        QMouseEvent hover(QEvent::MouseMove, a, view->mapToGlobal(a), Qt::NoButton, Qt::NoButton, mods);
        QApplication::sendEvent(view, &hover);
        QMouseEvent press(QEvent::MouseButtonPress, a, view->mapToGlobal(a), button, button, mods);
        QApplication::sendEvent(view, &press);
        constexpr int STEPS = 12;
        for (int i = 1; i <= STEPS; ++i) {
            const QPointF p(std::round(a.x() + (b.x() - a.x()) * i / STEPS), std::round(a.y() + (b.y() - a.y()) * i / STEPS));
            QMouseEvent move(QEvent::MouseMove, p, view->mapToGlobal(p), Qt::NoButton, button, mods);
            QApplication::sendEvent(view, &move);
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        }
        QMouseEvent release(QEvent::MouseButtonRelease, b, view->mapToGlobal(b), button, Qt::NoButton, mods);
        QApplication::sendEvent(view, &release);
        return true;
    }

    bool clickAt(RS_GraphicView* view, const double x, const double y, const Qt::MouseButton button,
                 const Qt::KeyboardModifiers mods, const bool twice, QString& error) {
        const LC_AIControl::AIInput aiInput; // passes the lock that holds back the user's input
        QPointF local;
        if (!toScreen(view, x, y, local, error)) {
            return false;
        }
        const QPointF global = view->mapToGlobal(local);
        QMouseEvent move(QEvent::MouseMove, local, global, Qt::NoButton, Qt::NoButton, mods);
        QApplication::sendEvent(view, &move);
        QMouseEvent press(QEvent::MouseButtonPress, local, global, button, button, mods);
        QApplication::sendEvent(view, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, local, global, button, Qt::NoButton, mods);
        QApplication::sendEvent(view, &release);
        if (twice) {
            QMouseEvent dbl(QEvent::MouseButtonDblClick, local, global, button, button, mods);
            QApplication::sendEvent(view, &dbl);
            QMouseEvent release2(QEvent::MouseButtonRelease, local, global, button, Qt::NoButton, mods);
            QApplication::sendEvent(view, &release2);
        }
        return true;
    }

    bool keyIn(QWidget* target, const QString& name, QString& error) {
        const LC_AIControl::AIInput aiInput; // passes the lock that holds back the user's input
        static const QHash<QString, int> keys = {
            {"enter", Qt::Key_Return}, {"return", Qt::Key_Return}, {"escape", Qt::Key_Escape}, {"esc", Qt::Key_Escape},
            {"delete", Qt::Key_Delete}, {"del", Qt::Key_Delete}, {"backspace", Qt::Key_Backspace}, {"space", Qt::Key_Space},
            {"tab", Qt::Key_Tab}, {"up", Qt::Key_Up}, {"down", Qt::Key_Down}, {"left", Qt::Key_Left}, {"right", Qt::Key_Right},
            {"shift", Qt::Key_Shift}, {"ctrl", Qt::Key_Control}};
        const int key = keys.value(name.trimmed().toLower(), 0);
        if (key == 0 || target == nullptr) {
            error = QStringLiteral("Unknown key \"%1\" (use Enter, Escape, Delete, Backspace, Space, Tab or an arrow).").arg(name);
            return false;
        }
        const QString text = key == Qt::Key_Return ? "\r" : key == Qt::Key_Space ? " " : key == Qt::Key_Tab ? "\t" : QString();
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
        QApplication::sendEvent(target, &press);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, text);
        QApplication::sendEvent(target, &release);
        return true;
    }

    // ------------------------------------------------------------------ the Entity Info panel and the Tool Options bar

    /** The text of the Entity Info panel (Entity Properties and the measuring tools write there). */
    QString entityInfoPanel() {
        QC_ApplicationWindow* app = appWindow();
        if (app == nullptr) {
            return {};
        }
        for (QWidget* panel : app->findChildren<QWidget*>()) {
            if (QString(panel->metaObject()->className()) == "LC_QuickInfoWidget") {
                if (auto* text = panel->findChild<QTextEdit*>("pteInfo")) {
                    return text->toPlainText().trimmed();
                }
            }
        }
        return {};
    }

    /** The bar where the running tool shows its settings (angle, factor, radius, count, modes...). */
    QToolBar* toolOptionsBar() {
        QC_ApplicationWindow* app = appWindow();
        return app != nullptr ? app->findChild<QToolBar*>("tooloptions_toolbar") : nullptr;
    }

    QString describeToolOptions(const bool brief) {
        QToolBar* bar = toolOptionsBar();
        if (bar == nullptr) {
            return {};
        }
        const QList<Field> fields = dialogFields(bar);
        QList<QAbstractButton*> buttons = dialogButtons(bar);
        if (fields.isEmpty() && buttons.isEmpty()) {
            return {};
        }
        QStringList lines;
        lines << (brief ? QStringLiteral("Tool options (set them with tool_options):")
                        : QStringLiteral("Tool options of the running tool (set them by field id or label):"));
        for (int i = 0; i < fields.size(); ++i) {
            const Field& f = fields[i];
            QString name = f.widget->objectName();
            if (name.startsWith("qt_")) {
                name.clear();
            }
            QString value = valueOf(f);
            if (brief && value.size() > 120) {
                value = value.left(120) + "...";
            }
            lines << QStringLiteral("  f%1 [%2] %3%4 = %5%6")
                         .arg(i)
                         .arg(f.kind, f.label.isEmpty() ? QStringLiteral("(no label)") : "\"" + f.label + "\"",
                              name.isEmpty() ? QString() : " name=" + name, value,
                              f.widget->isEnabled() ? QString() : QStringLiteral(" (disabled)"));
        }
        QStringList names;
        for (QAbstractButton* b : buttons) {
            names << "\"" + buttonName(b) + "\"" + (b->isEnabled() ? "" : " (disabled)");
        }
        if (!names.isEmpty()) {
            lines << "  Buttons: " + names.join(", ");
        }
        return lines.join('\n');
    }

    // ------------------------------------------------------------------ running a sequence of steps

    /** Presses a button (or nothing) and runs steps one by one, giving the application time in between. */
    struct Run : std::enable_shared_from_this<Run> {
        QPointer<QAction> action;
        QString id;
        QJsonArray steps;
        int next {0};
        bool capture {false};
        Snapshot before;
        QString historyBefore;
        QStringList log;
        QStringList skipped;
        bool wasChecked {false};
        bool failed {false};
        bool performing {false}; // a step is under way (a drag lets events through between its moves)
        QString header;
        Done done;

        void start(const int firstDelay) {
            auto self = shared_from_this();
            QTimer::singleShot(firstDelay, qApp, [self] { self->step(); });
        }

        void step() {
            auto self = shared_from_this();
            if (performing) {
                // the next step waits for this one: on a slow machine the timer can fire in the middle of a drag
                QTimer::singleShot(20, qApp, [self] { self->step(); });
                return;
            }
            if (auto* popup = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                // a right click without a tool opens the drawing's context menu: say what it offered, close it
                QStringList items;
                for (QAction* a : popup->actions()) {
                    if (!a->isSeparator() && !clean(a->text()).isEmpty()) {
                        items << clean(a->text());
                    }
                }
                log << "A context menu opened (" + items.mid(0, 15).join(", ") + ") and was closed; use the toolbar buttons instead";
                popup->close();
            }
            QWidget* dialog = currentDialog();
            if (dialog != nullptr && !before.dialogs.contains(dialog)) {
                for (int i = next; i < steps.size(); ++i) {
                    skipped << QString::fromUtf8(QJsonDocument(steps[i].toObject()).toJson(QJsonDocument::Compact));
                }
                finish(dialog);
                return;
            }
            if (next >= steps.size() || failed) {
                finish(nullptr);
                return;
            }
            const QJsonObject s = steps[next++].toObject();
            const int delay = s.value("type").toString().toLower() == "wait" ? qBound(0, s.value("ms").toInt(200), 5000) : STEP_SETTLE_MS;
            // the check comes from the event loop even when the step opens a dialog and waits in it
            QTimer::singleShot(0, qApp, [self, s] { self->perform(s); });
            QTimer::singleShot(delay, qApp, [self] { self->step(); });
        }

        void perform(const QJsonObject& s) {
            performing = true;
            const std::shared_ptr<Run> keep = shared_from_this();
            const auto idle = qScopeGuard([this] { performing = false; });
            const QString type = s.value("type").toString().toLower();
            RS_GraphicView* view = LC_AITools::view();
            QString error;
            Qt::KeyboardModifiers mods;
            QString held;
            if (s.value("shift").toBool()) {
                mods |= Qt::ShiftModifier;
                held += " with Shift";
            }
            if (s.value("ctrl").toBool()) {
                mods |= Qt::ControlModifier;
                held += " with Ctrl";
            }
            const Qt::MouseButton button = s.value("button").toString() == "right" ? Qt::RightButton : Qt::LeftButton;
            if (type == "type") {
                const QString text = s.value("text").toString().trimmed();
                QG_CommandWidget* cmd = commandWidget();
                if (cmd == nullptr) {
                    error = "the command line is not available";
                }
                else if (text.compare("esc", Qt::CaseInsensitive) == 0 || text.compare("escape", Qt::CaseInsensitive) == 0) {
                    cmd->escape();
                    log << "Escape";
                }
                else if (text.isEmpty()) {
                    error = "type needs text";
                }
                else {
                    cmd->handleCommand(text);
                    log << "Typed " + text;
                }
            }
            else if (type == "click") {
                if (clickAt(view, s.value("x").toDouble(NAN), s.value("y").toDouble(NAN), button, mods, s.value("double").toBool(), error)) {
                    log << QStringLiteral("%1 %2,%3%4%5").arg(s.value("double").toBool() ? "Double-clicked" : "Clicked")
                               .arg(s.value("x").toDouble()).arg(s.value("y").toDouble())
                               .arg(button == Qt::RightButton ? " (right button)" : "", held);
                }
            }
            else if (type == "drag") {
                if (dragAt(view, s.value("x").toDouble(NAN), s.value("y").toDouble(NAN), s.value("x2").toDouble(NAN),
                           s.value("y2").toDouble(NAN), button, mods, error)) {
                    log << QStringLiteral("Dragged %1,%2 to %3,%4%5%6").arg(s.value("x").toDouble()).arg(s.value("y").toDouble())
                               .arg(s.value("x2").toDouble()).arg(s.value("y2").toDouble())
                               .arg(button == Qt::RightButton ? " (right button)" : "", held);
                }
            }
            else if (type == "move") {
                if (moveTo(view, s.value("x").toDouble(NAN), s.value("y").toDouble(NAN), error)) {
                    log << QStringLiteral("Moved the mouse to %1,%2").arg(s.value("x").toDouble()).arg(s.value("y").toDouble());
                }
            }
            else if (type == "key") {
                if (keyIn(view, s.value("text").toString(), error)) {
                    log << "Pressed " + s.value("text").toString();
                }
            }
            else if (type == "wait") {
                log << QStringLiteral("Waited %1 ms").arg(qBound(0, s.value("ms").toInt(200), 5000));
            }
            else {
                error = QStringLiteral("unknown step type \"%1\" (use type, click, drag, move, key or wait)").arg(type);
            }
            if (!error.isEmpty()) {
                log << QStringLiteral("Step %1 failed: %2").arg(next).arg(error);
                failed = true;
            }
        }

        void finish(QWidget* dialog) {
            const Snapshot after = snapshot();
            RS_GraphicView* view = LC_AITools::view();
            QStringList lines;
            if (!header.isEmpty()) {
                lines << header;
            }
            if (!log.isEmpty()) {
                lines << "Did: " + log.join("; ") + ".";
            }
            if (action && action->isCheckable()
                && (kindOf(action) == "toggle" || (after.running.isEmpty() && action->isChecked() != wasChecked))) {
                lines << QStringLiteral("\"%1\" is now %2 (was %3).")
                             .arg(labelOf(action), action->isChecked() ? "on" : "off", wasChecked ? "on" : "off");
            }
            lines << describeChanges(before, after);
            if (const QString info = entityInfoPanel(); !info.isEmpty() && info != before.entityInfo) {
                lines << "Entity Info panel:\n" + info.left(1500);
            }
            if (QG_CommandWidget* cmd = commandWidget()) {
                const QString history = cmd->teHistory->toPlainText();
                QString output = history.startsWith(historyBefore) ? history.mid(historyBefore.size()).trimmed() : QString();
                if (output.size() > 2000) {
                    output = "..." + output.right(2000);
                }
                if (!output.isEmpty()) {
                    lines << "Command line said:\n" + output;
                }
            }
            if (!after.running.isEmpty()) {
                QString options;
                if (view != nullptr && view->getCurrentAction() != nullptr) {
                    QStringList commands = view->getCurrentAction()->getAvailableCommands();
                    commands.removeAll(QString()); // words OpenCAD does not know come back empty
                    if (!commands.isEmpty()) {
                        options = " Options you can type: " + commands.join(", ") + ".";
                    }
                }
                lines << QStringLiteral("Running tool: %1. Prompt: \"%2\".%3 Answer with run_command or click_drawing, "
                                        "Escape to finish it.")
                             .arg(after.running, after.prompt, options);
                if (const QString opts = describeToolOptions(true); !opts.isEmpty()) {
                    lines << opts;
                }
            }
            else {
                lines << QStringLiteral("No tool is running (prompt: \"%1\").").arg(after.prompt);
            }
            if (dialog != nullptr) {
                lines << describeDialog(dialog);
                lines << "Use handle_dialog to fill it in and press one of its buttons.";
                if (!skipped.isEmpty()) {
                    lines << "Not done yet because the dialog opened first: " + skipped.join(", ");
                }
            }
            restoreDialogsWhenDone();
            LC_AIToolResult result = LC_AIToolResult::text(lines.join('\n'));
            result.isError = failed;
            if (capture && view != nullptr) {
                result.content.prepend(LC_AITools::screenshot(view));
            }
            if (done) {
                Done d = std::move(done);
                d(result);
            }
        }
    };

    std::shared_ptr<Run> makeRun(const QJsonArray& steps, const bool capture, Done done) {
        auto run = std::make_shared<Run>();
        run->steps = steps;
        run->capture = capture;
        run->before = snapshot();
        run->before.entityInfo = entityInfoPanel();
        if (QG_CommandWidget* cmd = commandWidget()) {
            run->historyBefore = cmd->teHistory->toPlainText();
        }
        run->done = std::move(done);
        return run;
    }

    // ------------------------------------------------------------------ the tools

    LC_AITool listButtons() {
        LC_AITool t;
        t.name = "list_toolbar_buttons";
        t.label = "List toolbar buttons";
        t.description =
            "List OpenCAD's toolbar buttons and toolbar lists: id, label, toolbars, kind and state. Kinds: tool (starts an "
            "interactive drawing/editing tool that then asks for points, values or entities), toggle (switches a mode "
            "on or off: snaps, grid, panels), command (does something at once or opens a dialog), list (a choice in a "
            "toolbar: pen colour, width, line type, named view, UCS). Filter by toolbar or a search word; give an id to "
            "see one button in detail with an example of how to use it.";
        t.parameters = schema({{"toolbar", str("Only buttons of this toolbar (e.g. Line, Circle, Modify, Snap Selection)")},
                               {"search", str("Only buttons whose id or label contains this text")},
                               {"id", str("Show this one button in detail")}});
        t.execute = [](const QJsonObject& args) {
            const QList<Button> buttons = collectButtons();
            if (buttons.isEmpty()) {
                return LC_AIToolResult::error(QStringLiteral("OpenCAD's main window is not available."));
            }
            const QString id = args.value("id").toString().trimmed();
            if (!id.isEmpty()) {
                Button b;
                if (!findButton(id, b)) {
                    return LC_AIToolResult::error(QStringLiteral("No button \"%1\". Similar: %2").arg(id, similarIds(id).join(", ")));
                }
                QStringList lines;
                if (b.combo) {
                    lines << QStringLiteral("%1 — a list in %2. Current: \"%3\". Options: %4. Change it with set_toolbar_value.")
                                 .arg(b.id, b.places.join(", "), b.combo->currentText(), optionsOf(b.combo).join(" | "));
                    return LC_AIToolResult::text(lines.join('\n'));
                }
                if (b.menuButton) {
                    QStringList items;
                    const QList<QAction*> actions = menuItems(b.menuButton);
                    for (int i = 0; i < actions.size(); ++i) {
                        items << QStringLiteral("%1: %2").arg(i).arg(itemName(actions[i], i));
                    }
                    lines << QStringLiteral("%1 — a list in %2. Options (name or number): %3. Choose one with set_toolbar_value.")
                                 .arg(b.id, b.places.join(", "), items.isEmpty() ? QStringLiteral("(none saved yet)") : items.join(" | "));
                    return LC_AIToolResult::text(lines.join('\n'));
                }
                lines << QStringLiteral("%1 — \"%2\" (%3)%4").arg(b.id, labelOf(b.action), kindOf(b),
                                                              stateOf(b).isEmpty() ? QString() : ", " + stateOf(b));
                lines << "Where: " + b.places.join(", ");
                if (!clean(b.action->toolTip()).isEmpty() && clean(b.action->toolTip()) != labelOf(b.action)) {
                    lines << "Tooltip: " + clean(b.action->toolTip());
                }
                if (!b.action->shortcut().isEmpty()) {
                    lines << "Shortcut: " + b.action->shortcut().toString();
                }
                if (REFUSED.contains(b.id)) {
                    lines << "The AI does not press this button.";
                }
                else if (kindOf(b) == "tool") {
                    lines << "Use: press_toolbar_button with this id and steps that answer its prompts (the result shows "
                             "each prompt), e.g. typed points \"x,y\", clicks on entities, Enter, Escape when done. Tools "
                             "that change entities use the current selection (select_entities first) or ask you to pick.";
                }
                else if (kindOf(b) == "toggle") {
                    lines << "Use: press_toolbar_button switches it; the result says whether it is on.";
                }
                else {
                    lines << "Use: press_toolbar_button; if it opens a dialog, answer it with handle_dialog.";
                }
                return LC_AIToolResult::text(lines.join('\n'));
            }
            const QString toolbar = norm(args.value("toolbar").toString());
            const QString search = args.value("search").toString().trimmed().toLower();
            QMap<QString, QStringList> byToolbar;
            int shown = 0;
            for (const Button& b : buttons) {
                const QString first = b.places.value(0);
                if (!toolbar.isEmpty()) {
                    bool match = false;
                    for (const QString& p : b.places) {
                        match = match || norm(p.section(" > ", 0, 0)) == toolbar || norm(p).startsWith(toolbar);
                    }
                    if (!match) {
                        continue;
                    }
                }
                const QString label = b.action ? labelOf(b.action) : b.combo ? b.combo->objectName() + " list" : QStringLiteral("list");
                if (!search.isEmpty() && !b.id.toLower().contains(search) && !label.toLower().contains(search)) {
                    continue;
                }
                const QString state = stateOf(b);
                byToolbar[first.section(" > ", 0, 0)] << QStringLiteral("  %1 — %2 [%3%4]%5")
                                                             .arg(b.id, label, kindOf(b), state.isEmpty() ? QString() : ", " + state,
                                                                  first.contains(" > ") ? " (in " + first + ")" : QString());
                ++shown;
            }
            QStringList lines;
            lines << QStringLiteral("%1 button(s) and list(s):").arg(shown);
            for (auto it = byToolbar.cbegin(); it != byToolbar.cend(); ++it) {
                lines << it.key() + ":";
                lines << it.value();
            }
            return LC_AIToolResult::text(lines.join('\n'));
        };
        return t;
    }

    LC_AITool pressButton() {
        LC_AITool t;
        t.name = "press_toolbar_button";
        t.label = "Press toolbar button";
        t.description =
            "Press one of OpenCAD's toolbar buttons by its id (list_toolbar_buttons lists them), exactly like a person "
            "clicking it, then optionally answer the tool's prompts with steps: typed lines (points \"x,y\", \"@dx,dy\", "
            "\"@dist<angle\", numbers, option words), clicks in the drawing (to pick entities or give points), and keys "
            "(Enter to finish a selection or a polyline, Escape to end the tool). The result tells what the button did: "
            "the tool now running and its prompt, entities added or removed, toggles switched, the view, and any dialog "
            "it opened (answer that with handle_dialog). Tools that change entities (move, rotate, trim, ...) work on "
            "the current selection (select_entities) or ask you to pick entities with clicks.";
        t.parameters = schema({{"id", str("The button id, e.g. DrawLine, DrawCircle, ModifyMove, SnapGrid, LayersAdd")},
                               {"steps", stepsSchema()},
                               {"capture", flag("Also return a screenshot of the drawing afterwards (default false)")}},
                              {"id"});
        t.executeAsync = [](const QJsonObject& args, Done done) {
            const QString id = args.value("id").toString().trimmed();
            Button b;
            if (!findButton(id, b)) {
                done(LC_AIToolResult::error(QStringLiteral("No toolbar button \"%1\". Similar: %2. list_toolbar_buttons lists them all.")
                                                .arg(id, similarIds(id).join(", "))));
                return;
            }
            if (b.combo || b.menuButton) {
                done(LC_AIToolResult::error(QStringLiteral("\"%1\" is a list, not a button: use set_toolbar_value.").arg(b.id)));
                return;
            }
            if (REFUSED.contains(b.id)) {
                done(LC_AIToolResult::error(QStringLiteral("The AI does not press \"%1\" (%2); ask the user to.")
                                                .arg(b.id, labelOf(b.action))));
                return;
            }
            if (!b.action->isVisible()) {
                done(LC_AIToolResult::error(QStringLiteral("\"%1\" is hidden by OpenCAD and cannot be pressed.").arg(labelOf(b.action))));
                return;
            }
            if (!b.action->isEnabled()) {
                done(LC_AIToolResult::error(QStringLiteral("\"%1\" is disabled right now (it needs an open drawing, a "
                                                           "selection, or something to undo/paste).")
                                                .arg(labelOf(b.action))));
                return;
            }
            if (QWidget* open = currentDialog()) {
                done(LC_AIToolResult::error("A dialog is still open; close it with handle_dialog first.\n" + describeDialog(open)));
                return;
            }
            useQtDialogs();
            auto run = makeRun(args.value("steps").toArray(), args.value("capture").toBool(), std::move(done));
            run->action = b.action;
            run->id = b.id;
            run->wasChecked = b.action->isChecked();
            run->header = QStringLiteral("Pressed \"%1\" (%2, %3).").arg(labelOf(b.action), b.id, kindOf(b));
            QPointer<QAction> action = b.action;
            QTimer::singleShot(0, qApp, [action] {
                if (action) {
                    action->trigger(); // a dialog it opens runs its own event loop; the run goes on inside it
                }
            });
            run->start(TRIGGER_SETTLE_MS);
        };
        return t;
    }

    LC_AITool clickDrawing() {
        LC_AITool t;
        t.name = "click_drawing";
        t.label = "Click drawing";
        t.description =
            "Click a point of the drawing, as a person clicks with the mouse: gives the running tool a point or picks the "
            "entity under it (to select it, or to choose the entity a tool asks for: the line to trim, the circle to "
            "dimension, ...). x and y are drawing coordinates; the point must be in the visible area. Snapping applies "
            "like for the user. Without a running tool a click selects the entity under it.";
        t.parameters = schema({{"x", num("X in drawing units")},
                               {"y", num("Y in drawing units")},
                               {"button", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"left", "right"}},
                                                      {"description", "Mouse button (default left; right usually goes back)"}}},
                               {"shift", flag("Hold Shift")},
                               {"ctrl", flag("Hold Ctrl")},
                               {"double", flag("Double-click")},
                               {"to_x", num("Drag instead of click: release the button at this X (window zoom, panning, freehand)")},
                               {"to_y", num("Drag: release the button at this Y")},
                               {"capture", flag("Also return a screenshot afterwards (default false)")}},
                              {"x", "y"});
        t.executeAsync = [](const QJsonObject& args, Done done) {
            const bool drag = args.contains("to_x") && args.contains("to_y");
            QJsonObject step{{"type", drag ? "drag" : "click"}, {"x", args.value("x")}, {"y", args.value("y")},
                             {"button", args.value("button").toString("left")}, {"shift", args.value("shift").toBool()},
                             {"ctrl", args.value("ctrl").toBool()}, {"double", args.value("double").toBool()}};
            if (drag) {
                step.insert("x2", args.value("to_x"));
                step.insert("y2", args.value("to_y"));
            }
            useQtDialogs();
            auto run = makeRun(QJsonArray{step}, args.value("capture").toBool(), std::move(done));
            run->start(0);
        };
        return t;
    }

    LC_AITool selectEntities() {
        LC_AITool t;
        t.name = "select_entities";
        t.label = "Select entities";
        t.description =
            "Select entities of the drawing, for the buttons that work on a selection (move, copy, rotate, scale, "
            "mirror, delete, properties, block creation, ...). Choose entities by id (list_entities shows ids), by type, "
            "by layer, inside a window, or all; mode replace (default), add, remove, or clear to select nothing.";
        t.parameters = schema({{"ids", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "integer"}}},
                                                   {"description", "Entity ids"}}},
                               {"all", flag("All entities")},
                               {"type", str("Only entities of this type (line, circle, arc, polyline, text, ...)")},
                               {"layer", str("Only entities on this layer")},
                               {"x1", num("Window: X of one corner")}, {"y1", num("Window: Y of one corner")},
                               {"x2", num("Window: X of the opposite corner")}, {"y2", num("Window: Y of the opposite corner")},
                               {"mode", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"replace", "add", "remove", "clear"}},
                                                    {"description", "replace (default), add, remove, or clear"}}}});
        t.execute = [](const QJsonObject& args) {
            RS_GraphicView* view = LC_AITools::view();
            RS_Graphic* graphic = view != nullptr ? view->getGraphic() : nullptr;
            if (graphic == nullptr) {
                return LC_AIToolResult::error(QStringLiteral("No drawing is open."));
            }
            const QString mode = args.value("mode").toString("replace").toLower();
            if (mode == "clear") {
                RS_Selection::unselectAllInDocument(graphic, view->getViewPort());
                view->redraw(RS2::RedrawAll);
                return LC_AIToolResult::text(QStringLiteral("Nothing is selected now."));
            }
            QSet<unsigned long long> ids;
            for (const QJsonValue& v : args.value("ids").toArray()) {
                ids.insert(static_cast<unsigned long long>(v.toDouble()));
            }
            const bool all = args.value("all").toBool();
            const QString type = args.value("type").toString().trimmed().toLower();
            const QString layer = args.value("layer").toString();
            const bool window = args.contains("x1") && args.contains("y1") && args.contains("x2") && args.contains("y2");
            if (ids.isEmpty() && !all && type.isEmpty() && layer.isEmpty() && !window) {
                return LC_AIToolResult::error(QStringLiteral("Say which entities: ids, all, type, layer or a window."));
            }
            const RS_Vector lo(std::min(args.value("x1").toDouble(), args.value("x2").toDouble()),
                               std::min(args.value("y1").toDouble(), args.value("y2").toDouble()));
            const RS_Vector hi(std::max(args.value("x1").toDouble(), args.value("x2").toDouble()),
                               std::max(args.value("y1").toDouble(), args.value("y2").toDouble()));
            QList<RS_Entity*> chosen;
            for (RS_Entity* e : *graphic) {
                if (e == nullptr || e->isDeleted() || !e->isVisible()) {
                    continue;
                }
                if (!ids.isEmpty() && !ids.contains(e->getId())) {
                    continue;
                }
                if (!type.isEmpty() && LC_AITools::typeName(e) != type) {
                    continue;
                }
                if (!layer.isEmpty() && (e->getLayer() == nullptr || e->getLayer()->getName() != layer)) {
                    continue;
                }
                if (window && (e->getMin().x < lo.x || e->getMin().y < lo.y || e->getMax().x > hi.x || e->getMax().y > hi.y)) {
                    continue;
                }
                if (e->getLayer() != nullptr && e->getLayer()->isLocked()) {
                    continue;
                }
                chosen << e;
            }
            if (mode == "replace") {
                RS_Selection::unselectAllInDocument(graphic, view->getViewPort());
            }
            RS_Selection::selectEntitiesList(graphic, view->getViewPort(), chosen, mode != "remove");
            view->redraw(RS2::RedrawAll);
            int selected = 0;
            for (RS_Entity* e : *graphic) {
                if (e != nullptr && !e->isDeleted() && e->isSelected()) {
                    ++selected;
                }
            }
            QStringList lines;
            lines << QStringLiteral("%1 %2 entit%3; %4 selected now.")
                         .arg(mode == "remove" ? "Unselected" : "Selected").arg(chosen.size())
                         .arg(chosen.size() == 1 ? "y" : "ies").arg(selected);
            for (int i = 0; i < chosen.size() && i < MAX_REPORTED_ENTITIES; ++i) {
                lines << "  " + LC_AITools::describeEntity(chosen[i]);
            }
            if (chosen.size() > MAX_REPORTED_ENTITIES) {
                lines << QStringLiteral("  ... and %1 more").arg(chosen.size() - MAX_REPORTED_ENTITIES);
            }
            return LC_AIToolResult::text(lines.join('\n'));
        };
        return t;
    }

    LC_AITool handleDialog() {
        LC_AITool t;
        t.name = "handle_dialog";
        t.label = "Handle dialog";
        t.description =
            "Read, fill in and close the dialog a toolbar button opened (or any OpenCAD dialog that is open): without "
            "arguments it describes the dialog (texts, fields with ids f0, f1, ..., their values and options, buttons). "
            "fields sets values by field id, label or name; file_path answers a file dialog; tab switches a tab; button "
            "presses a button by its text (OK, Cancel, Save, Yes, ...). The result describes what is open afterwards.";
        const QJsonObject field = schema({{"field", str("Field id (f3), label or object name")},
                                          {"value", str("The value: text, number, true/false, or an option of a list")}},
                                         {"field", "value"});
        t.parameters = schema({{"fields", QJsonObject{{"type", "array"}, {"items", field}, {"description", "Values to set, in order"}}},
                               {"file_path", str("For a file dialog: the file to open or save (absolute path)")},
                               {"tab", str("Switch to this tab first")},
                               {"button", str("Then press this button (OK, Cancel, Save, Apply, Yes, No, or its text)")}});
        t.executeAsync = [](const QJsonObject& args, Done done) {
            QWidget* dialog = currentDialog();
            if (dialog == nullptr) {
                done(LC_AIToolResult::error(QStringLiteral("No dialog is open.")));
                return;
            }
            QStringList did;
            QStringList errors;
            if (const QString tab = args.value("tab").toString(); !tab.isEmpty()) {
                bool found = false;
                for (QTabWidget* tabs : dialog->findChildren<QTabWidget*>()) {
                    for (int i = 0; i < tabs->count() && !found; ++i) {
                        if (norm(tabs->tabText(i)) == norm(tab)) {
                            tabs->setCurrentIndex(i);
                            found = true;
                        }
                    }
                }
                (found ? did : errors) << QStringLiteral("tab \"%1\"").arg(tab);
            }
            auto* files = qobject_cast<QFileDialog*>(dialog);
            if (files != nullptr) {
                if (const QString path = args.value("file_path").toString(); !path.isEmpty()) {
                    const QFileInfo info(path);
                    files->setDirectory(info.absolutePath());
                    files->selectFile(info.absoluteFilePath());
                    if (auto* name = files->findChild<QLineEdit*>("fileNameEdit")) {
                        name->setText(info.fileName());
                    }
                    did << "file " + path;
                }
            }
            const QList<Field> fields = dialogFields(dialog);
            for (const QJsonValue& v : args.value("fields").toArray()) {
                const QJsonObject f = v.toObject();
                const QString key = f.value("field").toString().trimmed();
                const Field* target = nullptr;
                static const QRegularExpression fid(QStringLiteral("^f(\\d+)$"));
                if (const auto m = fid.match(key); m.hasMatch() && m.captured(1).toInt() < fields.size()) {
                    target = &fields[m.captured(1).toInt()];
                }
                for (int i = 0; target == nullptr && i < fields.size(); ++i) {
                    if (fields[i].widget && fields[i].widget->objectName().compare(key, Qt::CaseInsensitive) == 0) {
                        target = &fields[i];
                    }
                }
                for (int i = 0; target == nullptr && i < fields.size(); ++i) {
                    if (!fields[i].label.isEmpty() && norm(fields[i].label) == norm(key)) {
                        target = &fields[i];
                    }
                }
                for (int i = 0; target == nullptr && i < fields.size(); ++i) {
                    if (!fields[i].label.isEmpty() && norm(fields[i].label).contains(norm(key))) {
                        target = &fields[i];
                    }
                }
                if (target == nullptr) {
                    errors << QStringLiteral("no field \"%1\"").arg(key);
                    continue;
                }
                QString error;
                if (setField(*target, f.value("value"), error)) {
                    did << QStringLiteral("%1 = %2").arg(key, f.value("value").toVariant().toString());
                }
                else {
                    errors << QStringLiteral("%1: %2").arg(key, error);
                }
            }
            const QString buttonText = args.value("button").toString().trimmed();
            Snapshot before = snapshot();
            before.entityInfo = entityInfoPanel();
            before.dialogs.remove(dialog);
            if (buttonText.isEmpty()) {
                QStringList lines;
                if (!did.isEmpty()) {
                    lines << "Set: " + did.join("; ") + ".";
                }
                if (!errors.isEmpty()) {
                    lines << "Could not set: " + errors.join("; ") + ".";
                }
                lines << describeDialog(dialog);
                LC_AIToolResult r = LC_AIToolResult::text(lines.join('\n'));
                r.isError = !errors.isEmpty() && did.isEmpty();
                done(r);
                return;
            }
            QPointer<QAbstractButton> button = findDialogButton(dialog, buttonText);
            QPointer<QWidget> watched = dialog;
            const bool acceptWord = QStringList{"ok", "save", "open", "choose", "accept", "select"}.contains(norm(buttonText));
            if (files != nullptr && acceptWord) {
                // a file dialog takes the typed name when accepted, even before its button updates
                button = nullptr;
                QMetaObject::invokeMethod(files, "accept", Qt::QueuedConnection);
            }
            else if (button == nullptr && QStringList{"cancel", "close", "reject"}.contains(norm(buttonText))) {
                // no such button: close it the way Escape does
                if (auto* d = qobject_cast<QDialog*>(dialog)) {
                    QTimer::singleShot(0, d, &QDialog::reject);
                }
                else {
                    QTimer::singleShot(0, dialog, &QWidget::close);
                }
            }
            else if (button == nullptr) {
                QStringList names;
                for (QAbstractButton* b : dialogButtons(dialog)) {
                    names << "\"" + clean(b->text()) + "\"";
                }
                done(LC_AIToolResult::error(QStringLiteral("No button \"%1\" in the dialog. Buttons: %2").arg(buttonText, names.join(", "))));
                return;
            }
            else if (!button->isEnabled()) {
                done(LC_AIToolResult::error(QStringLiteral("The button \"%1\" is disabled (a field is missing or wrong).\n%2")
                                                .arg(clean(button->text()), describeDialog(dialog))));
                return;
            }
            else {
                QTimer::singleShot(0, button, &QAbstractButton::click);
            }
            auto run = std::make_shared<Run>();
            run->before = before;
            if (QG_CommandWidget* cmd = commandWidget()) {
                run->historyBefore = cmd->teHistory->toPlainText();
            }
            QStringList header;
            if (!did.isEmpty()) {
                header << "Set: " + did.join("; ") + ".";
            }
            if (!errors.isEmpty()) {
                header << "Could not set: " + errors.join("; ") + ".";
            }
            header << QStringLiteral("Pressed \"%1\".").arg(button ? clean(button->text()) : buttonText);
            run->header = header.join('\n');
            run->done = [watched, done](const LC_AIToolResult& r) {
                LC_AIToolResult out = r;
                if (watched && watched->isVisible() && currentDialog() == watched) {
                    out.content.append(LC_AIContent::makeText(QStringLiteral("The dialog is still open (a value may be "
                                                                             "missing or invalid):\n") + describeDialog(watched)));
                }
                done(out);
            };
            run->start(DIALOG_SETTLE_MS);
        };
        return t;
    }

    LC_AITool toolOptions() {
        LC_AITool t;
        t.name = "tool_options";
        t.label = "Tool options";
        t.description =
            "Read or change the settings of the running drawing/editing tool, shown in OpenCAD's Tool Options bar: angles, "
            "factors, radii, lengths, counts, check boxes and mode buttons (e.g. Rotate's angle, Scale's factor, Align's "
            "alignment, Slice/Divide's count, a circle's radius). Without arguments it describes them (fields f0, f1, ... "
            "with labels and values; buttons). fields sets values by field id, label or name (check boxes and mode "
            "buttons take true/false); button presses a button of the bar (for example a \"pick from drawing\" button). "
            "Set the options right after pressing the tool's button and before giving it points.";
        const QJsonObject field = schema({{"field", str("Field id (f3), label or object name")},
                                          {"value", str("The value: number, text, true/false, or an option of a list")}},
                                         {"field", "value"});
        t.parameters = schema({{"fields", QJsonObject{{"type", "array"}, {"items", field}, {"description", "Values to set, in order"}}},
                               {"button", str("Then press this button of the bar (its text, tooltip or name)")}});
        t.executeAsync = [](const QJsonObject& args, Done done) {
            QToolBar* bar = toolOptionsBar();
            if (bar == nullptr || describeToolOptions(false).isEmpty()) {
                done(LC_AIToolResult::error(QStringLiteral("No tool with options is running. Press a tool's button first.")));
                return;
            }
            const QList<Field> fields = dialogFields(bar);
            QStringList did;
            QStringList errors;
            for (const QJsonValue& v : args.value("fields").toArray()) {
                const QJsonObject f = v.toObject();
                const QString key = f.value("field").toString().trimmed();
                const Field* target = nullptr;
                static const QRegularExpression fid(QStringLiteral("^f(\\d+)$"));
                if (const auto m = fid.match(key); m.hasMatch() && m.captured(1).toInt() < fields.size()) {
                    target = &fields[m.captured(1).toInt()];
                }
                for (int i = 0; target == nullptr && i < fields.size(); ++i) {
                    if (fields[i].widget && fields[i].widget->objectName().compare(key, Qt::CaseInsensitive) == 0) {
                        target = &fields[i];
                    }
                }
                for (int i = 0; target == nullptr && i < fields.size(); ++i) {
                    if (!fields[i].label.isEmpty() && norm(fields[i].label) == norm(key)) {
                        target = &fields[i];
                    }
                }
                for (int i = 0; target == nullptr && i < fields.size(); ++i) {
                    if (!fields[i].label.isEmpty() && !norm(key).isEmpty() && norm(fields[i].label).contains(norm(key))) {
                        target = &fields[i];
                    }
                }
                if (target == nullptr) {
                    errors << QStringLiteral("no option \"%1\"").arg(key);
                    continue;
                }
                QString error;
                if (setField(*target, f.value("value"), error)) {
                    did << QStringLiteral("%1 = %2").arg(key, f.value("value").toVariant().toString());
                }
                else {
                    errors << QStringLiteral("%1: %2").arg(key, error);
                }
            }
            auto run = makeRun({}, false, std::move(done));
            QStringList header;
            if (!did.isEmpty()) {
                header << "Set: " + did.join("; ") + ".";
            }
            if (!errors.isEmpty()) {
                header << "Could not set: " + errors.join("; ") + ".";
                run->failed = did.isEmpty();
            }
            if (const QString wanted = args.value("button").toString().trimmed(); !wanted.isEmpty()) {
                QPointer<QAbstractButton> button = findDialogButton(bar, wanted);
                if (button == nullptr) {
                    header << QStringLiteral("No button \"%1\" in the tool options.").arg(wanted);
                    run->failed = true;
                }
                else {
                    header << QStringLiteral("Pressed \"%1\".").arg(buttonName(button));
                    QTimer::singleShot(0, button, &QAbstractButton::click);
                }
            }
            if (header.isEmpty()) {
                header << describeToolOptions(false);
            }
            run->header = header.join('\n');
            run->start(STEP_SETTLE_MS);
        };
        return t;
    }

    // ------------------------------------------------------------------ panels, menus, the whole window

    /** Sets field values (by field id f3, object name or label) on any dialog, panel or bar. */
    void applyFields(QWidget* container, const QJsonArray& values, QStringList& did, QStringList& errors) {
        const QList<Field> fields = dialogFields(container);
        for (const QJsonValue& v : values) {
            const QJsonObject f = v.toObject();
            const QString key = f.value("field").toString().trimmed();
            const Field* target = nullptr;
            static const QRegularExpression fid(QStringLiteral("^f(\\d+)$"));
            if (const auto m = fid.match(key); m.hasMatch() && m.captured(1).toInt() < fields.size()) {
                target = &fields[m.captured(1).toInt()];
            }
            for (int i = 0; target == nullptr && i < fields.size(); ++i) {
                if (fields[i].widget && fields[i].widget->objectName().compare(key, Qt::CaseInsensitive) == 0) {
                    target = &fields[i];
                }
            }
            for (int i = 0; target == nullptr && i < fields.size(); ++i) {
                if (!fields[i].label.isEmpty() && norm(fields[i].label) == norm(key)) {
                    target = &fields[i];
                }
            }
            for (int i = 0; target == nullptr && i < fields.size(); ++i) {
                if (!fields[i].label.isEmpty() && !norm(key).isEmpty() && norm(fields[i].label).contains(norm(key))) {
                    target = &fields[i];
                }
            }
            if (target == nullptr) {
                errors << QStringLiteral("no field \"%1\"").arg(key);
                continue;
            }
            QString error;
            if (setField(*target, f.value("value"), error)) {
                did << QStringLiteral("%1 = %2").arg(key, f.value("value").toVariant().toString());
            }
            else {
                errors << QStringLiteral("%1: %2").arg(key, error);
            }
        }
    }

    /** The table or tree an "item" request means: a field id (f3), an object name, or the first one. */
    QAbstractItemView* itemViewIn(QWidget* container, const QString& wanted) {
        const QList<Field> fields = dialogFields(container);
        static const QRegularExpression fid(QStringLiteral("^f(\\d+)$"));
        if (const auto m = fid.match(wanted); m.hasMatch() && m.captured(1).toInt() < fields.size()) {
            return qobject_cast<QAbstractItemView*>(fields[m.captured(1).toInt()].widget.data());
        }
        QAbstractItemView* first = nullptr;
        for (const Field& f : fields) {
            auto* view = qobject_cast<QAbstractItemView*>(f.widget.data());
            if (view == nullptr) {
                continue;
            }
            if (!wanted.isEmpty() && (view->objectName().compare(wanted, Qt::CaseInsensitive) == 0 || norm(f.label) == norm(wanted))) {
                return view;
            }
            if (first == nullptr) {
                first = view;
            }
        }
        return wanted.isEmpty() ? first : nullptr;
    }

    /**
     * {view, row, column, click: left | double | right, ctrl, shift, menu}: clicks a row or cell of a table or tree like a
     * person; a right click opens its menu, where "menu" chooses an entry ("Add Sub-Layer > Dimensions") or, without it, the
     * entries are listed. next(what was done, error) follows.
     */
    void itemAction(QWidget* container, const QJsonObject& item, std::function<void(const QString&, const QString&)> next) {
        QAbstractItemView* view = itemViewIn(container, item.value("view").toString());
        if (view == nullptr) {
            next({}, QStringLiteral("no table or tree \"%1\" here").arg(item.value("view").toString()));
            return;
        }
        QString error;
        const QModelIndex index = findItem(view, item.value("row").toVariant().toString(), item.value("column").toVariant().toString(), error);
        if (!index.isValid()) {
            next({}, error);
            return;
        }
        Qt::KeyboardModifiers mods;
        if (item.value("ctrl").toBool()) {
            mods |= Qt::ControlModifier;
        }
        if (item.value("shift").toBool()) {
            mods |= Qt::ShiftModifier;
        }
        const QString click = item.value("click").toString("left").toLower();
        const QString header = headerText(view->model(), index.column());
        const QString what = QStringLiteral("%1 row \"%2\" column %3%4").arg(click == "double" ? "Double-clicked" : click == "right" ? "Right-clicked" : "Clicked",
                                                                             rowLabel(index))
                                 .arg(index.column()).arg(header.isEmpty() ? QString() : " (" + header + ")");
        if (click != "right") {
            if (!clickItem(view, index, Qt::LeftButton, mods, click == "double", error)) {
                next({}, error);
                return;
            }
            next(what, {});
            return;
        }
        if (!clickItem(view, index, Qt::RightButton, mods, false, error)) {
            next({}, error);
            return;
        }
        // the menu: a context-menu event where the right click was, then choose in the menu that opens
        const QStringList path = item.value("menu").toString().split(" > ", Qt::SkipEmptyParts);
        QPointer<QWidget> port = view->viewport();
        const QPoint at = view->visualRect(index).center();
        chooseInPopup(path, [next, what, path](const QString& err, const QStringList& entries) {
            if (!err.isEmpty() || path.isEmpty()) {
                next(what + (entries.isEmpty() ? QString() : "; its menu offers: " + entries.join(" | ")), path.isEmpty() ? QString() : err);
                return;
            }
            next(what + "; chose \"" + path.join(" > ") + "\"", {});
        });
        QTimer::singleShot(0, qApp, [port, at] {
            if (port) {
                const LC_AIControl::AIInput aiInput;
                QContextMenuEvent menuEvent(QContextMenuEvent::Mouse, at, port->mapToGlobal(at));
                QApplication::sendEvent(port, &menuEvent);
            }
        });
    }

    /** Keys typed into the focused widget: "Ctrl+Shift+G", "Return", "Escape", or several separated by ", ". */
    bool sendKeys(QWidget* target, const QString& keys, QString& error) {
        if (target == nullptr) {
            error = "nothing has the keyboard focus";
            return false;
        }
        const QKeySequence sequence = QKeySequence::fromString(keys, QKeySequence::PortableText);
        if (sequence.isEmpty()) {
            error = QStringLiteral("cannot read the keys \"%1\"").arg(keys);
            return false;
        }
        const LC_AIControl::AIInput aiInput;
        for (int i = 0; i < sequence.count(); ++i) {
            const QKeyCombination combo = sequence[i];
            const bool plain = !(combo.keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
            const QString name = QKeySequence(combo.key()).toString();
            const QString text = plain && name.size() == 1 ? (combo.keyboardModifiers() & Qt::ShiftModifier ? name : name.toLower()) : QString();
            QKeyEvent press(QEvent::KeyPress, combo.key(), combo.keyboardModifiers(), text);
            QApplication::sendEvent(target, &press);
            QKeyEvent release(QEvent::KeyRelease, combo.key(), combo.keyboardModifiers(), text);
            QApplication::sendEvent(target, &release);
        }
        return true;
    }

    /** Text typed into the focused widget character by character, as from the keyboard. */
    void typeText(QWidget* target, const QString& text) {
        const LC_AIControl::AIInput aiInput;
        for (const QChar c : text) {
            const int key = c == '\n' ? Qt::Key_Return : c.toUpper().unicode();
            QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, c == '\n' ? QStringLiteral("\r") : QString(c));
            QApplication::sendEvent(target, &press);
            QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, c == '\n' ? QStringLiteral("\r") : QString(c));
            QApplication::sendEvent(target, &release);
        }
    }

    // ---- panels

    QList<QDockWidget*> panelDocks() {
        QList<QDockWidget*> list;
        if (QC_ApplicationWindow* app = appWindow()) {
            for (QDockWidget* dock : app->findChildren<QDockWidget*>()) {
                if (!clean(dock->windowTitle()).isEmpty() && !QString(dock->widget() != nullptr ? dock->widget()->metaObject()->className() : "").startsWith("LC_AI")) {
                    list << dock;
                }
            }
        }
        return list;
    }

    QDockWidget* findPanel(const QString& wanted) {
        const QList<QDockWidget*> docks = panelDocks();
        for (QDockWidget* d : docks) {
            if (d->objectName().compare(wanted, Qt::CaseInsensitive) == 0 || norm(d->windowTitle()) == norm(wanted)) {
                return d;
            }
        }
        QList<QDockWidget*> partial;
        for (QDockWidget* d : docks) {
            if (norm(d->windowTitle()).contains(norm(wanted)) || d->objectName().contains(wanted, Qt::CaseInsensitive)) {
                partial << d;
            }
        }
        return partial.size() == 1 ? partial.first() : nullptr;
    }

    QString panelsList() {
        QStringList lines;
        lines << "Panels (use_panel panel=<name>):";
        for (QDockWidget* d : panelDocks()) {
            QStringList state;
            state << (d->isVisible() ? "shown" : "hidden");
            if (d->isFloating()) {
                state << "floating";
            }
            lines << QStringLiteral("  %1 (%2) [%3]").arg(clean(d->windowTitle()), d->objectName(), state.join(", "));
        }
        lines << "  Status bar (statusbar): the coordinates, angles, selection, layer and grid fields at the bottom";
        return lines.join('\n');
    }

    QString describePanel(QDockWidget* dock) {
        QWidget* content = dock->widget() != nullptr ? dock->widget() : dock;
        QString body = describeDialog(content).section('\n', 1); // the dialog description without its first line
        QString head = QStringLiteral("Panel \"%1\" (%2).").arg(clean(dock->windowTitle()), dock->objectName());
        if (body.trimmed().isEmpty()) {
            body = "(nothing to read: it may need an open drawing)";
        }
        return head + "\n" + body
               + "\nTables and trees are fields: click their rows with item {view: \"fN\", row: \"<text>\", click: left|double|right, menu: \"<entry>\"}.";
    }

    // ---- the status bar (its fields live in small toolbars at the bottom)

    QList<QWidget*> statusBarParts() {
        QList<QWidget*> parts;
        QC_ApplicationWindow* app = appWindow();
        if (app == nullptr) {
            return parts;
        }
        for (QToolBar* tb : app->findChildren<QToolBar*>()) {
            if (app->toolBarArea(tb) == Qt::BottomToolBarArea && tb->isVisible()) {
                parts << tb;
            }
        }
        if (app->statusBar() != nullptr) {
            parts << app->statusBar();
        }
        return parts;
    }

    bool isClickable(const QWidget* w) {
        return qobject_cast<const QAbstractButton*>(w) == nullptr && w->metaObject()->indexOfSignal("clicked()") >= 0;
    }

    QString describeStatusBar() {
        QStringList lines;
        lines << "Status bar:";
        QC_ApplicationWindow* app = appWindow();
        for (QWidget* part : statusBarParts()) {
            QStringList texts;
            for (QLabel* label : part->findChildren<QLabel*>()) {
                if (!label->isVisible() || clean(label->text()).isEmpty()) {
                    continue;
                }
                const QString tip = clean(QString(label->toolTip()).remove(QRegularExpression(QStringLiteral("<[^>]*>")))).left(50);
                texts << (tip.isEmpty() ? clean(label->text()) : tip + ": " + clean(label->text()));
            }
            QStringList clickable;
            for (QWidget* w : part->findChildren<QWidget*>()) {
                if (w->isVisible() && isClickable(w)) {
                    const QString tip = clean(QString(w->toolTip()).remove(QRegularExpression(QStringLiteral("<[^>]*>")))).left(50);
                    clickable << QStringLiteral("\"%1\"").arg(tip.isEmpty() ? w->objectName() : tip);
                }
            }
            if (texts.isEmpty() && clickable.isEmpty()) {
                continue;
            }
            const QString title = qobject_cast<QToolBar*>(part) != nullptr ? clean(part->windowTitle()) : QStringLiteral("Messages");
            lines << QStringLiteral("  %1: %2%3").arg(title, texts.join(" | "),
                                                      clickable.isEmpty() ? QString() : "  (clickable: " + clickable.join(", ") + ")");
        }
        if (app != nullptr && app->statusBar() != nullptr && !app->statusBar()->currentMessage().isEmpty()) {
            lines << "  Message: " + clean(app->statusBar()->currentMessage());
        }
        return lines.join('\n');
    }

    QWidget* findStatusWidget(const QString& wanted) {
        for (QWidget* part : statusBarParts()) {
            for (QWidget* w : part->findChildren<QWidget*>()) {
                if (!w->isVisible() || !isClickable(w)) {
                    continue;
                }
                const QString tip = clean(QString(w->toolTip()).remove(QRegularExpression(QStringLiteral("<[^>]*>"))));
                if (w->objectName().compare(wanted, Qt::CaseInsensitive) == 0 || norm(tip).startsWith(norm(wanted))) {
                    return w;
                }
            }
        }
        return nullptr;
    }

    void clickWidget(QWidget* w, const Qt::MouseButton button = Qt::LeftButton) {
        const LC_AIControl::AIInput aiInput;
        const QPointF at = QRectF(w->rect()).center();
        QMouseEvent press(QEvent::MouseButtonPress, at, w->mapToGlobal(at), button, button, Qt::NoModifier);
        QApplication::sendEvent(w, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, at, w->mapToGlobal(at), button, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(w, &release);
    }

    LC_AITool usePanel() {
        LC_AITool t;
        t.name = "use_panel";
        t.label = "Use panel";
        t.description =
            "Read and use OpenCAD's side panels (Layers, Layers Tree, Blocks, Library Browser, Pens Palette, Pen Wizard, Named "
            "Views, User Coordinate Systems, Entity Info, Command Line, the CAD tool panels...) and the status bar, the way a "
            "person does. Without panel: lists the panels. With panel only: shows it (opening its tab) and describes it: "
            "fields f0, f1... with values, its tables and trees with every row and the state of each column in words (Layers: "
            "visible/hidden, locked/unlocked, printed/not printed, construction, colour, name), and its buttons. fields sets "
            "values (the filter box, check boxes...); item clicks a row or cell: {view: \"f3\", row: \"Walls\", column: 0 or "
            "header, click: left|double|right, ctrl, shift, menu: \"Add Sub-Layer > Dimensions\"} (in Layers, a left click on "
            "column 0 of a row toggles that layer's visibility, column 1 its lock, 2 printing, 3 construction; a click on the "
            "name selects it, a double click makes it current); button presses a panel button by text, tooltip or name (Add "
            "layer, Remove, Show all...); keys types keys into the focused field. panel \"statusbar\" reads the status bar "
            "and button clicks its clickable fields (Grid Status, Base Angle). The result describes the panel afterwards and "
            "any dialog that opened (answer it with handle_dialog).";
        const QJsonObject field = schema({{"field", str("Field id (f3), label or object name")},
                                          {"value", str("The value: text, number, true/false, an option, or a row of a table")}},
                                         {"field", "value"});
        const QJsonObject item = schema({{"view", str("The table or tree: its field id (f3) or name; default the first")},
                                         {"row", str("The row: its text in any column, a number, or a tree path \"Group > Layer\"")},
                                         {"column", str("Column number or header (default: the column with that text)")},
                                         {"click", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"left", "double", "right"}},
                                                               {"description", "left (default), double, or right (opens the row's menu)"}}},
                                         {"ctrl", flag("Hold Ctrl (add to the selection)")},
                                         {"shift", flag("Hold Shift (select a range)")},
                                         {"menu", str("With a right click: the menu entry to choose, \"Sub menu > Entry\"; without it, the entries are listed")}},
                                        {"row"});
        t.parameters = schema({{"panel", str("The panel: its title or object name; \"statusbar\"; or \"dialog\" for the open dialog (its tables, trees and keys)")},
                               {"fields", QJsonObject{{"type", "array"}, {"items", field}, {"description", "Values to set, in order"}}},
                               {"item", item},
                               {"keys", str("Keys for the focused field, e.g. \"Return\" or \"Ctrl+A\"")},
                               {"button", str("Then press this button of the panel (its text, tooltip or name)")},
                               {"capture", flag("Also return a picture of the panel")}});
        t.executeAsync = [](const QJsonObject& args, Done done) {
            const QString wanted = args.value("panel").toString().trimmed();
            if (wanted.isEmpty()) {
                done(LC_AIToolResult::text(panelsList()));
                return;
            }
            if (norm(wanted) == "statusbar" || norm(wanted) == "status bar") {
                const QString buttonText = args.value("button").toString().trimmed();
                if (buttonText.isEmpty()) {
                    done(LC_AIToolResult::text(describeStatusBar()));
                    return;
                }
                QWidget* w = findStatusWidget(buttonText);
                if (w == nullptr) {
                    done(LC_AIToolResult::error(QStringLiteral("No clickable \"%1\" in the status bar.\n%2").arg(buttonText, describeStatusBar())));
                    return;
                }
                useQtDialogs();
                auto run = makeRun({}, false, std::move(done));
                run->header = QStringLiteral("Clicked \"%1\" in the status bar.").arg(buttonText);
                QPointer<QWidget> target = w;
                QTimer::singleShot(0, qApp, [target] {
                    if (target) {
                        clickWidget(target);
                    }
                });
                run->start(DIALOG_SETTLE_MS);
                return;
            }
            const bool isDialog = norm(wanted) == "dialog";
            QDockWidget* dock = isDialog ? nullptr : findPanel(wanted);
            QWidget* openDialog = isDialog ? currentDialog() : nullptr;
            if (!isDialog && dock == nullptr) {
                done(LC_AIToolResult::error(QStringLiteral("No panel \"%1\".\n%2").arg(wanted, panelsList())));
                return;
            }
            if (isDialog && openDialog == nullptr) {
                done(LC_AIToolResult::error(QStringLiteral("No dialog is open.")));
                return;
            }
            QC_ApplicationWindow* app = appWindow();
            const bool needsShowing = dock != nullptr && (!dock->isVisible() || dock->visibleRegion().isEmpty());
            if (needsShowing && app != nullptr) {
                app->requestDockVisible(dock); // opens it and brings its tab to the front
                dock->raise();
            }
            // a panel's content, or (panel "dialog") the open dialog: tables, trees and keys work there too
            QPointer<QWidget> panel = dock != nullptr ? static_cast<QWidget*>(dock) : openDialog;
            const QJsonObject a = args;
            QTimer::singleShot(needsShowing ? 250 : 0, qApp, [panel, a, done, isDialog] {
                if (!panel) {
                    done(LC_AIToolResult::error(QStringLiteral("The panel went away.")));
                    return;
                }
                auto* asDock = qobject_cast<QDockWidget*>(panel.data());
                QWidget* content = asDock != nullptr && asDock->widget() != nullptr ? asDock->widget() : panel.data();
                const bool acts = a.contains("fields") || a.contains("item") || a.contains("keys") || a.contains("button");
                const bool capture = a.value("capture").toBool();
                auto finishWith = [panel, capture, done, isDialog](const LC_AIToolResult& r) {
                    LC_AIToolResult out = r;
                    if (panel && !isDialog && currentDialog() == nullptr) {
                        out.content.append(LC_AIContent::makeText(describePanel(qobject_cast<QDockWidget*>(panel.data()))));
                    }
                    if (panel && isDialog && panel->isVisible()) {
                        out.content.append(LC_AIContent::makeText(describeDialog(panel)));
                    }
                    if (panel && capture) {
                        QByteArray png;
                        QBuffer buffer(&png);
                        buffer.open(QIODevice::WriteOnly);
                        panel->grab().save(&buffer, "PNG");
                        out.content.prepend(LC_AIContent::makeImage(png, QStringLiteral("image/png")));
                    }
                    done(out);
                };
                if (!acts) {
                    finishWith(LC_AIToolResult::text(QString()));
                    return;
                }
                useQtDialogs();
                auto run = makeRun({}, false, finishWith);
                QStringList did;
                QStringList errors;
                applyFields(content, a.value("fields").toArray(), did, errors);
                auto afterItem = [run, content, a, did, errors](const QString& itemDone, const QString& itemError) mutable {
                    QStringList header;
                    if (!did.isEmpty()) {
                        header << "Set: " + did.join("; ") + ".";
                    }
                    if (!itemDone.isEmpty()) {
                        header << itemDone + ".";
                    }
                    if (!itemError.isEmpty()) {
                        errors << itemError;
                    }
                    if (const QString keys = a.value("keys").toString(); !keys.isEmpty()) {
                        QString error;
                        if (sendKeys(QApplication::focusWidget(), keys, error)) {
                            header << "Keys: " + keys + ".";
                        }
                        else {
                            errors << error;
                        }
                    }
                    if (const QString wanted = a.value("button").toString().trimmed(); !wanted.isEmpty()) {
                        QPointer<QAbstractButton> button = findDialogButton(content, wanted);
                        if (button == nullptr) {
                            errors << QStringLiteral("no button \"%1\"").arg(wanted);
                        }
                        else if (!button->isEnabled()) {
                            errors << QStringLiteral("the button \"%1\" is disabled").arg(buttonName(button));
                        }
                        else {
                            header << QStringLiteral("Pressed \"%1\".").arg(buttonName(button));
                            QTimer::singleShot(0, button, &QAbstractButton::click);
                        }
                    }
                    if (!errors.isEmpty()) {
                        header << "Could not: " + errors.join("; ") + ".";
                        run->failed = did.isEmpty() && itemDone.isEmpty() && !header.join("").contains("Pressed");
                    }
                    run->header = header.join('\n');
                    run->start(DIALOG_SETTLE_MS);
                };
                if (a.contains("item")) {
                    itemAction(content, a.value("item").toObject(), afterItem);
                }
                else {
                    afterItem({}, {});
                }
            });
        };
        return t;
    }

    // ---- the menu bar

    struct MenuEntry {
        QString path;
        QPointer<QAction> action;
        QString top;
    };

    QList<MenuEntry> menuBarEntries() {
        QList<MenuEntry> out;
        QC_ApplicationWindow* app = appWindow();
        if (app == nullptr || app->menuBar() == nullptr) {
            return out;
        }
        std::function<void(QMenu*, const QString&, const QString&, int)> walk = [&](QMenu* menu, const QString& prefix, const QString& top, int depth) {
            if (menu == nullptr || depth > 5) {
                return;
            }
            emit menu->aboutToShow(); // menus that fill themselves when opened (Workspace, Windows, Recent files)
            for (QAction* a : menu->actions()) {
                if (a->isSeparator() || !a->isVisible() || clean(a->text()).isEmpty()) {
                    continue;
                }
                const QString path = prefix + " > " + clean(a->text());
                if (a->menu() != nullptr) {
                    walk(a->menu(), path, top, depth + 1);
                }
                else {
                    out.append({path, a, top});
                }
            }
        };
        for (QAction* topAction : app->menuBar()->actions()) {
            if (topAction->menu() != nullptr && topAction->isVisible()) {
                walk(topAction->menu(), clean(topAction->text()), clean(topAction->text()), 0);
            }
        }
        return out;
    }

    /** Why the AI does not choose this menu entry, or empty. */
    QString menuRefusal(const MenuEntry& e) {
        if (!e.action) {
            return QStringLiteral("it went away");
        }
        if (REFUSED.contains(e.action->objectName()) || e.action->objectName() == "MainMenu") {
            return QStringLiteral("the AI does not quit OpenCAD, hide the menu bar or close its own panel");
        }
        if (norm(e.top) == "help" && !norm(e.path).contains("about") && !norm(e.path).contains("license")) {
            return QStringLiteral("it opens your web browser; the AI looks things up in its own browser window instead");
        }
        if (norm(e.path).endsWith("> ai") && e.action->isCheckable()) {
            return QStringLiteral("it hides the AI's own toolbar");
        }
        return {};
    }

    LC_AITool useMenu() {
        LC_AITool t;
        t.name = "use_menu";
        t.label = "Use menu";
        t.description =
            "Use OpenCAD's menu bar, including entries that are not toolbar buttons: recent files, plugins, showing or hiding "
            "panels and toolbars, workspaces, drawing windows (tile, cascade, window mode), named views, UCS and options. "
            "Without path it lists every entry as \"Menu > Submenu > Entry\" (with its button id when it has one, and on/off "
            "for switches; search narrows the list). With path it chooses that entry like a click and reports what happened, "
            "including a dialog it opened (answer it with handle_dialog).";
        t.parameters = schema({{"path", str("The entry to choose, e.g. \"File > Recent Files > 1\" or \"Workspace > Widgets > Layers\"")},
                               {"search", str("When listing: only entries containing this text")}});
        t.executeAsync = [](const QJsonObject& args, Done done) {
            const QString path = args.value("path").toString().trimmed();
            const QList<MenuEntry> entries = menuBarEntries();
            if (path.isEmpty()) {
                const QString search = args.value("search").toString().trimmed().toLower();
                QStringList lines;
                for (const MenuEntry& e : entries) {
                    if (!search.isEmpty() && !e.path.toLower().contains(search)) {
                        continue;
                    }
                    QString line = "  " + e.path;
                    if (!e.action->objectName().isEmpty()) {
                        line += " [" + e.action->objectName() + "]";
                    }
                    if (e.action->isCheckable()) {
                        line += e.action->isChecked() ? " (on)" : " (off)";
                    }
                    if (!e.action->isEnabled()) {
                        line += " (disabled)";
                    }
                    lines << line;
                }
                lines.prepend(QStringLiteral("%1 menu entries:").arg(lines.size()));
                done(LC_AIToolResult::text(lines.join('\n')));
                return;
            }
            const QStringList wantedParts = path.split(">", Qt::SkipEmptyParts);
            QString wanted;
            for (const QString& p : wantedParts) {
                wanted += (wanted.isEmpty() ? "" : " > ") + norm(p);
            }
            const MenuEntry* pick = nullptr;
            QList<const MenuEntry*> starts;
            for (const MenuEntry& e : entries) {
                QString have;
                for (const QString& p : e.path.split(" > ")) {
                    have += (have.isEmpty() ? "" : " > ") + norm(p);
                }
                if (have == wanted) {
                    pick = &e;
                    break;
                }
                if (have.startsWith(wanted)) {
                    starts << &e;
                }
            }
            if (pick == nullptr && starts.size() == 1) {
                pick = starts.first();
            }
            if (pick == nullptr) {
                QStringList near;
                const QString last = norm(wantedParts.value(wantedParts.size() - 1));
                for (const MenuEntry& e : entries) {
                    if (norm(e.path).contains(last)) {
                        near << e.path;
                    }
                }
                done(LC_AIToolResult::error(QStringLiteral("No menu entry \"%1\"%2").arg(path,
                    starts.size() > 1 ? QStringLiteral(" (it matches several)") : QString())
                    + (near.isEmpty() ? QString() : "\nSimilar: " + near.mid(0, 15).join(" | "))));
                return;
            }
            if (const QString why = menuRefusal(*pick); !why.isEmpty()) {
                done(LC_AIToolResult::error(QStringLiteral("The AI does not choose \"%1\": %2.").arg(pick->path, why)));
                return;
            }
            if (!pick->action->isEnabled()) {
                done(LC_AIToolResult::error(QStringLiteral("\"%1\" is disabled right now.").arg(pick->path)));
                return;
            }
            useQtDialogs();
            auto run = makeRun({}, false, std::move(done));
            run->action = pick->action;
            run->wasChecked = pick->action->isChecked();
            run->header = QStringLiteral("Chose \"%1\".").arg(pick->path);
            QPointer<QAction> action = pick->action;
            QTimer::singleShot(0, qApp, [action] {
                if (action) {
                    action->trigger();
                }
            });
            run->start(TRIGGER_SETTLE_MS);
        };
        return t;
    }

    // ---- the drawing's right-click menu

    LC_AITool contextMenu() {
        LC_AITool t;
        t.name = "context_menu";
        t.label = "Right-click menu";
        t.description =
            "Right-click the drawing at x,y (drawing units) and use the menu that opens, like a person: on an entity it offers "
            "that entity's commands (Modify Line > Offset, dimension Copy value, Properties...), on empty space the general "
            "menu, and your custom menus. Without item it lists the menu's entries; with item (\"Modify Line > Offset\") it "
            "chooses that entry, then answer the tool's prompts with run_command or click_drawing. A running tool is ended "
            "first (a right click would only go back in it).";
        t.parameters = schema({{"x", num("X in drawing units")}, {"y", num("Y in drawing units")},
                               {"item", str("The entry to choose, \"Submenu > Entry\"; without it the entries are listed")},
                               {"capture", flag("Also return a screenshot afterwards")}},
                              {"x", "y"});
        t.executeAsync = [](const QJsonObject& args, Done done) {
            RS_GraphicView* view = LC_AITools::view();
            if (view == nullptr) {
                done(LC_AIToolResult::error(QStringLiteral("No drawing is open.")));
                return;
            }
            QString ended;
            if (!runningTool(view).isEmpty()) {
                ended = runningTool(view);
                view->killAllActions();
                QApplication::processEvents();
            }
            QPointF local;
            QString error;
            const double x = args.value("x").toDouble(NAN);
            const double y = args.value("y").toDouble(NAN);
            if (!toScreen(view, x, y, local, error)) {
                done(LC_AIToolResult::error(error));
                return;
            }
            useQtDialogs();
            const QStringList path = args.value("item").toString().split(" > ", Qt::SkipEmptyParts);
            auto run = makeRun({}, args.value("capture").toBool(), std::move(done));
            QStringList header;
            if (!ended.isEmpty()) {
                header << QStringLiteral("Ended the running %1 first.").arg(ended);
            }
            header << QStringLiteral("Right-clicked %1,%2.").arg(x).arg(y);
            run->header = header.join('\n');
            chooseInPopup(path, [run, path](const QString& err, const QStringList& entries) {
                if (path.isEmpty()) {
                    run->log << (entries.isEmpty() ? QStringLiteral("no menu opened (with one entry it runs at once)")
                                                   : "The menu offers: " + entries.join(" | "));
                }
                else if (!err.isEmpty()) {
                    run->log << err + (entries.isEmpty() ? QString() : "; the menu offers: " + entries.join(" | "));
                    run->failed = true;
                }
                else {
                    run->log << "Chose \"" + path.join(" > ") + "\"";
                }
                run->start(TRIGGER_SETTLE_MS);
            });
            QPointer<RS_GraphicView> target = view;
            QTimer::singleShot(0, qApp, [target, x, y] {
                QString e;
                if (target) {
                    clickAt(target, x, y, Qt::RightButton, Qt::NoModifier, false, e);
                }
            });
        };
        return t;
    }

    // ---- anywhere in the window

    /** The window the AI looks at: the main window, the open dialog, or a window by its title. */
    QWidget* appTarget(const QString& wanted) {
        if (wanted.isEmpty() || norm(wanted) == "main") {
            return appWindow();
        }
        if (norm(wanted) == "dialog") {
            return currentDialog();
        }
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (w->isVisible() && norm(w->windowTitle()).contains(norm(wanted))) {
                return w;
            }
        }
        return nullptr;
    }

    constexpr int MAX_APP_SHOT = 1600;

    /** How much the screenshot of this window is scaled down (image px * scale = window px). */
    double shotScale(const QWidget* w) {
        const int edge = std::max(w->width(), w->height());
        return edge > MAX_APP_SHOT ? double(edge) / MAX_APP_SHOT : 1.0;
    }

    bool isAIPart(const QWidget* w) {
        for (const QWidget* p = w; p != nullptr; p = p->parentWidget()) {
            const QString cls = p->metaObject()->className();
            if (cls.startsWith("LC_AI") || p->objectName() == "aiBrowserFloat" || p->objectName() == "aiControlBanner") {
                return true;
            }
        }
        return false;
    }

    LC_AITool lookAtApp() {
        LC_AITool t;
        t.name = "look_at_app";
        t.label = "Look at app";
        t.description =
            "Take a screenshot of OpenCAD's whole window (menus, toolbars, panels, status bar, the drawing) or of an open "
            "dialog, to see what a person sees. press_in_app then clicks or types at x,y measured in THIS image. Use it for "
            "anything the other tools cannot reach.";
        t.parameters = schema({{"window", str("\"main\" (default), \"dialog\" (the open dialog), or a window title")}});
        t.execute = [](const QJsonObject& args) {
            QWidget* w = appTarget(args.value("window").toString().trimmed());
            if (w == nullptr) {
                return LC_AIToolResult::error(QStringLiteral("No such window is open."));
            }
            QPixmap shot = w->grab();
            const double scale = shotScale(w);
            if (scale > 1.0) {
                shot = shot.scaled(qRound(w->width() / scale), qRound(w->height() / scale), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            shot.save(&buffer, "PNG");
            QStringList others;
            for (QWidget* top : QApplication::topLevelWidgets()) {
                if (top != w && top->isVisible() && qobject_cast<QDialog*>(top) != nullptr && !isAIPart(top)) {
                    others << "\"" + clean(top->windowTitle()) + "\"";
                }
            }
            LC_AIToolResult result = LC_AIToolResult::text(
                QStringLiteral("Screenshot of \"%1\": %2 x %3 px (coordinates for press_in_app are in this image).%4")
                    .arg(clean(w->windowTitle()).isEmpty() ? QStringLiteral("OpenCAD") : clean(w->windowTitle()))
                    .arg(shot.width()).arg(shot.height())
                    .arg(others.isEmpty() ? QString() : " Other open windows: " + others.join(", ") + "."));
            result.content.prepend(LC_AIContent::makeImage(png, QStringLiteral("image/png")));
            return result;
        };
        return t;
    }

    LC_AITool pressInApp() {
        LC_AITool t;
        t.name = "press_in_app";
        t.label = "Press in app";
        t.description =
            "Use any part of OpenCAD's window like a person with mouse and keyboard, at x,y of the latest look_at_app image of "
            "that window: click, double, right (opens menus; choose an entry with menu), drag (to to_x,to_y), hover, scroll "
            "(delta: positive up), type (text into the focused field; click the field first) or key (\"Ctrl+S\", \"Return\", "
            "\"Escape\"). The result says what is under the point, what changed, and any dialog or menu that opened; set "
            "capture to see the window again. Prefer the specific tools (press_toolbar_button, use_panel, use_menu, "
            "handle_dialog, click_drawing) when they fit; this reaches everything else.";
        t.parameters = schema({{"x", num("X in the look_at_app image")}, {"y", num("Y in the look_at_app image")},
                               {"window", str("The window the image was of: \"main\" (default), \"dialog\" or a title")},
                               {"action", QJsonObject{{"type", "string"},
                                                      {"enum", QJsonArray{"click", "double", "right", "drag", "hover", "scroll", "type", "key"}},
                                                      {"description", "What to do (default click)"}}},
                               {"to_x", num("For drag: where to release (image X)")}, {"to_y", num("For drag: where to release (image Y)")},
                               {"text", str("For type: the text; for key: the keys")},
                               {"menu", str("With right: the menu entry to choose, \"Submenu > Entry\"")},
                               {"delta", num("For scroll: steps, positive up (default 1)")},
                               {"capture", flag("Also return a new screenshot of the window afterwards")}});
        t.executeAsync = [](const QJsonObject& args, Done done) {
            const QString windowName = args.value("window").toString().trimmed();
            QWidget* win = appTarget(windowName);
            if (win == nullptr) {
                done(LC_AIToolResult::error(QStringLiteral("No such window is open; use look_at_app first.")));
                return;
            }
            const QString action = args.value("action").toString("click").toLower();
            const double scale = shotScale(win);
            QPoint at(qRound(args.value("x").toDouble() * scale), qRound(args.value("y").toDouble() * scale));
            QWidget* target = action == "type" || action == "key" ? QApplication::focusWidget() : win->childAt(at);
            if (target == nullptr && action != "type" && action != "key") {
                target = win;
            }
            if (target == nullptr) {
                done(LC_AIToolResult::error(QStringLiteral("Nothing has the keyboard focus: click a field first.")));
                return;
            }
            if (isAIPart(target)) {
                done(LC_AIToolResult::error(QStringLiteral("That point is on the AI's own panel; the AI does not press there.")));
                return;
            }
            const QString what = QStringLiteral("%1 (%2%3)").arg(target->metaObject()->className(),
                                                               target->objectName().isEmpty() ? QString() : target->objectName(),
                                                               target->toolTip().isEmpty() ? QString() : " \"" + clean(QString(target->toolTip()).remove(QRegularExpression(QStringLiteral("<[^>]*>")))).left(40) + "\"");
            useQtDialogs();
            const bool capture = args.value("capture").toBool();
            QPointer<QWidget> shotOf = win;
            auto run = makeRun({}, false, [done, capture, shotOf, windowName](const LC_AIToolResult& r) {
                LC_AIToolResult out = r;
                if (capture) {
                    QWidget* w = appTarget(windowName);
                    if (w == nullptr && shotOf) {
                        w = shotOf;
                    }
                    if (w != nullptr) {
                        QPixmap shot = w->grab();
                        const double s = shotScale(w);
                        if (s > 1.0) {
                            shot = shot.scaled(qRound(w->width() / s), qRound(w->height() / s), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                        }
                        QByteArray png;
                        QBuffer buffer(&png);
                        buffer.open(QIODevice::WriteOnly);
                        shot.save(&buffer, "PNG");
                        out.content.prepend(LC_AIContent::makeImage(png, QStringLiteral("image/png")));
                    }
                }
                done(out);
            });
            QPointer<QWidget> w = target;
            const QPoint local = target->mapFrom(win, at);
            if (action == "type") {
                typeText(target, args.value("text").toString());
                run->header = QStringLiteral("Typed \"%1\" into %2.").arg(args.value("text").toString().left(80), what);
                run->start(STEP_SETTLE_MS);
                return;
            }
            if (action == "key") {
                QString error;
                if (!sendKeys(target, args.value("text").toString(), error)) {
                    run->failed = true;
                    run->header = error;
                }
                else {
                    run->header = QStringLiteral("Pressed %1 in %2.").arg(args.value("text").toString(), what);
                }
                run->start(STEP_SETTLE_MS * 2);
                return;
            }
            run->header = QStringLiteral("%1 at %2,%3 on %4.").arg(action == "click" ? "Clicked" : action == "double" ? "Double-clicked"
                                                                 : action == "right" ? "Right-clicked" : action == "drag" ? "Dragged from"
                                                                 : action == "hover" ? "Hovered" : "Scrolled")
                              .arg(args.value("x").toDouble()).arg(args.value("y").toDouble()).arg(what);
            const QPoint toAt(qRound(args.value("to_x").toDouble() * scale), qRound(args.value("to_y").toDouble() * scale));
            const int delta = args.contains("delta") ? qRound(args.value("delta").toDouble()) : 1;
            const QStringList menuPath = args.value("menu").toString().split(" > ", Qt::SkipEmptyParts);
            QPointer<QWidget> window = win;
            auto menuDone = std::make_shared<bool>(false);
            if (action == "right") {
                chooseInPopup(menuPath, [run, menuPath, menuDone](const QString& err, const QStringList& entries) {
                    *menuDone = true;
                    if (!entries.isEmpty() && menuPath.isEmpty()) {
                        run->log << "The menu offers: " + entries.join(" | ");
                    }
                    else if (!err.isEmpty() && !menuPath.isEmpty()) {
                        run->log << err + (entries.isEmpty() ? QString() : "; the menu offers: " + entries.join(" | "));
                        run->failed = true;
                    }
                    else if (!menuPath.isEmpty()) {
                        run->log << "Chose \"" + menuPath.join(" > ") + "\"";
                    }
                    run->start(TRIGGER_SETTLE_MS);
                }, 0, 12);
            }
            QTimer::singleShot(0, qApp, [w, window, action, local, toAt, delta, menuDone] {
                if (!w || !window) {
                    return;
                }
                const LC_AIControl::AIInput aiInput;
                const QPointF p(local);
                const QPointF g = w->mapToGlobal(p);
                if (action == "hover") {
                    QMouseEvent move(QEvent::MouseMove, p, g, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(w, &move);
                    QHelpEvent help(QEvent::ToolTip, local, g.toPoint());
                    QApplication::sendEvent(w, &help);
                    return;
                }
                if (action == "scroll") {
                    QWheelEvent wheel(p, g, QPoint(), QPoint(0, 120 * delta), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
                    QApplication::sendEvent(w, &wheel);
                    return;
                }
                const Qt::MouseButton button = action == "right" ? Qt::RightButton : Qt::LeftButton;
                QMouseEvent move(QEvent::MouseMove, p, g, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(w, &move);
                QMouseEvent press(QEvent::MouseButtonPress, p, g, button, button, Qt::NoModifier);
                QApplication::sendEvent(w, &press);
                if (action == "drag") {
                    const QPointF end(w->mapFrom(window, toAt));
                    for (int i = 1; i <= 10; ++i) {
                        const QPointF q = p + (end - p) * (i / 10.0);
                        QMouseEvent m(QEvent::MouseMove, q, w->mapToGlobal(q), Qt::NoButton, button, Qt::NoModifier);
                        QApplication::sendEvent(w, &m);
                    }
                    QMouseEvent release(QEvent::MouseButtonRelease, end, w->mapToGlobal(end), button, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(w, &release);
                    return;
                }
                QMouseEvent release(QEvent::MouseButtonRelease, p, g, button, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(w, &release);
                if (action == "double") {
                    QMouseEvent dbl(QEvent::MouseButtonDblClick, p, g, button, button, Qt::NoModifier);
                    QApplication::sendEvent(w, &dbl);
                    QMouseEvent release2(QEvent::MouseButtonRelease, p, g, button, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(w, &release2);
                }
                if (action == "right" && w && !*menuDone && QApplication::activePopupWidget() == nullptr) {
                    // widgets that open their menu on the context-menu event rather than on the click
                    QContextMenuEvent menuEvent(QContextMenuEvent::Mouse, local, g.toPoint());
                    QApplication::sendEvent(w, &menuEvent);
                }
            });
            if (action != "right") {
                run->start(DIALOG_SETTLE_MS);
            }
        };
        return t;
    }

    // ------------------------------------------------------------------ the rest of the window
    // Menu entries, commands that only have a keyboard shortcut, the panels and their buttons, and the status bar:
    // listed (Agent Progress, the checklist) and tested like the toolbar buttons. Each gets a test made from it unless a
    // written recipe has its id: the AI's own tool reaches it and it really goes off ("fired"). What a command then does
    // is tested with the toolbar button that runs the same command; right-click menus, the Properties panel and the
    // drawing preferences have written recipes ("context:", "properties:", "prefs:").

    struct Surface {
        QString id;
        QString group; // Menus, Keyboard shortcuts, Panels, Status bar (and the groups of written recipes)
        QString label;
        QString kind;  // tool, toggle, command, dialog, panel, button, status
        QString how;   // the tool call that uses it
        QString task;  // the start of a task for the AI chat
        QString why;   // why the AI does not use it, or empty
        QJsonObject recipe; // the test made from it (no "id" yet)
    };

    QString quoted(const QString& s) {
        return "“" + s + "”";
    }

    /** Menu entries that come and go with what is open (recent files, drawing windows, saved workspaces). */
    bool comesAndGoes(const MenuEntry& e) {
        if (!e.action->objectName().isEmpty()) {
            return false;
        }
        if (e.action->data().isValid()) {
            return true; // a recent file, a drawing window or a saved workspace carries its index or name
        }
        if (qobject_cast<QDockWidget*>(e.action->parent()) != nullptr || qobject_cast<QToolBar*>(e.action->parent()) != nullptr) {
            return false; // shows or hides a panel or a toolbar (Workspace > Widgets, Toolbars)
        }
        const QString p = norm(e.path);
        return p.contains("recent") || (norm(e.top) == "workspace" && !p.contains("drawings"));
    }

    /**
     * Entries whose test would close or save the user's drawings or rearrange their window: listed, and not run from
     * the Agent Progress window. The developer self-test (offscreen, throwaway settings and drawings) runs them too.
     */
    QString riskyToTest(const QString& label, const QString& objectName) {
        if (qEnvironmentVariableIsSet("OPENCAD_AI_TOOLBAR_TEST")) {
            return {};
        }
        const QString l = norm(label);
        static const QStringList words = {"close", "quit", "exit", "save all", "restart", "fullscreen", "full screen"};
        for (const QString& w : words) {
            if (l.contains(w)) {
                return QStringLiteral("not run from here: it would %1 your own drawings or window (the offscreen self-test runs it)")
                    .arg(w.startsWith("full") ? "resize" : w);
            }
        }
        if (l.contains("re-dock") || l.contains("redock") || l == "tile" || l.contains("cascade") || l.contains("window mode")
            || l.contains("tab mode") || objectName.startsWith("WorkspaceRemove") || objectName.startsWith("WorkspaceRestore")) {
            return QStringLiteral("not run from here: it rearranges your panels or drawing windows (the offscreen self-test runs it)");
        }
        return {};
    }

    QList<Surface> collectSurfaces() {
        QList<Surface> out;
        QC_ApplicationWindow* app = appWindow();
        if (app == nullptr) {
            return out;
        }
        QSet<QString> seen;
        QSet<QString> reachable; // actions a toolbar or the menu bar already offers
        for (const Button& b : collectButtons()) {
            reachable.insert(b.id);
        }
        QHash<QString, QString> buttonOf; // action -> its toolbar button, for the notes
        for (const Button& b : collectButtons()) {
            if (b.action) {
                buttonOf.insert(b.action->objectName(), b.id);
            }
        }

        // the menu bar
        for (const MenuEntry& e : menuBarEntries()) {
            if (!e.action || comesAndGoes(e)) {
                continue;
            }
            Surface s;
            s.id = "menu:" + e.path;
            if (seen.contains(s.id)) {
                continue;
            }
            seen.insert(s.id);
            reachable.insert(e.action->objectName());
            s.group = QStringLiteral("Menus");
            s.label = e.path;
            s.kind = clean(e.action->text()).endsWith("...") ? QStringLiteral("dialog") : kindOf(e.action);
            s.how = QStringLiteral("use_menu path=\"%1\"").arg(e.path);
            if (const QString button = buttonOf.value(e.action->objectName()); !button.isEmpty()) {
                s.how += QStringLiteral(" (also toolbar button %1)").arg(button);
            }
            s.task = QObject::tr("Using the menu entry %1 (use_menu): ").arg(quoted(e.path));
            s.why = menuRefusal(e);
            QJsonObject recipe{{"press", false},
                               {"then", QJsonArray{QJsonObject{{"tool", "use_menu"}, {"args", QJsonObject{{"path", e.path}}}}}},
                               {"expect", QJsonObject{{"fired", QJsonObject{{"menu", e.path}}}}}};
            if (e.action->isCheckable()) {
                recipe.insert("restore", true);
            }
            if (const QString risky = riskyToTest(clean(e.action->text()), e.action->objectName()); !risky.isEmpty()) {
                recipe.insert("skip", risky);
            }
            s.recipe = recipe;
            out << s;
        }

        // commands that only have a keyboard shortcut (Options > Keyboard Shortcuts lists them with the others)
        QList<QAction*> actions = app->findChildren<QAction*>();
        std::sort(actions.begin(), actions.end(), [](const QAction* a, const QAction* b) { return a->objectName() < b->objectName(); });
        for (QAction* a : std::as_const(actions)) {
            const QString name = a->objectName();
            if (name.isEmpty() || name.startsWith("qt_") || reachable.contains(name) || seen.contains("shortcut:" + name)
                || a->isSeparator() || a->menu() != nullptr || clean(a->text()).isEmpty()
                || (!a->property("RS2:actionType").isValid() && a->shortcut().isEmpty())) {
                continue;
            }
            Surface s;
            s.id = "shortcut:" + name;
            seen.insert(s.id);
            s.group = QStringLiteral("Keyboard shortcuts");
            s.label = labelOf(a) + (a->shortcut().isEmpty() ? QString() : " (" + a->shortcut().toString(QKeySequence::NativeText) + ")");
            s.kind = kindOf(a);
            s.how = QStringLiteral("press_toolbar_button id=%1").arg(name);
            s.task = QObject::tr("Using the command %1 (%2, press_toolbar_button): ").arg(quoted(labelOf(a)), name);
            s.why = LC_AIToolbar::notAgenticReason(name);
            QJsonObject recipe{{"press", false},
                               {"then", QJsonArray{QJsonObject{{"tool", "press_toolbar_button"}, {"args", QJsonObject{{"id", name}}}}}},
                               {"expect", QJsonObject{{"fired", QJsonObject{{"action", name}}}}}};
            if (a->isCheckable()) {
                recipe.insert("restore", true);
            }
            if (const QString risky = riskyToTest(clean(a->text()), name); !risky.isEmpty()) {
                recipe.insert("skip", risky);
            }
            s.recipe = recipe;
            out << s;
        }

        // the panels and their buttons
        for (QDockWidget* dock : panelDocks()) {
            const QString title = clean(dock->windowTitle());
            Surface s;
            s.id = "panel:" + dock->objectName();
            if (seen.contains(s.id)) {
                continue;
            }
            seen.insert(s.id);
            s.group = QStringLiteral("Panels");
            s.label = title;
            s.kind = QStringLiteral("panel");
            s.how = QStringLiteral("use_panel panel=\"%1\"").arg(title);
            s.task = QObject::tr("Using the %1 panel (use_panel): ").arg(quoted(title));
            s.recipe = QJsonObject{{"press", false},
                                   {"then", QJsonArray{QJsonObject{{"tool", "use_panel"}, {"args", QJsonObject{{"panel", dock->objectName()}}}}}},
                                   {"expect", QJsonObject{{"text", QString("Panel \"" + QRegularExpression::escape(title) + "\"")}}}};
            out << s;
            QWidget* content = dock->widget() != nullptr ? dock->widget() : dock;
            for (QAbstractButton* b : dialogButtons(content)) {
                const QString name = buttonName(b);
                Surface button;
                button.id = s.id + " > " + name;
                if (seen.contains(button.id)) {
                    continue;
                }
                seen.insert(button.id);
                button.group = s.group;
                button.label = title + " > " + name;
                button.kind = QStringLiteral("button");
                button.how = QStringLiteral("use_panel panel=\"%1\" button=\"%2\"").arg(title, name);
                button.task = QObject::tr("Using the %1 button of the %2 panel (use_panel): ").arg(quoted(name), quoted(title));
                QJsonObject recipe{{"press", false},
                                   {"then", QJsonArray{QJsonObject{{"tool", "use_panel"},
                                                                   {"args", QJsonObject{{"panel", dock->objectName()}, {"button", name}}}}}},
                                   {"expect", QJsonObject{{"fired", QJsonObject{{"panel", dock->objectName()}, {"button", name}}}}}};
                if (const QString risky = riskyToTest(name, b->objectName()); !risky.isEmpty()) {
                    recipe.insert("skip", risky);
                }
                button.recipe = recipe;
                out << button;
            }
        }

        // the status bar: its fields (read) and the ones a click opens
        for (QWidget* part : statusBarParts()) {
            auto* toolbar = qobject_cast<QToolBar*>(part);
            const QString title = toolbar != nullptr ? clean(part->windowTitle()) : QStringLiteral("Messages");
            // a field to read (a toolbar of buttons at the bottom, like the snap toolbar, is listed with the toolbar buttons)
            bool ofButtons = false;
            if (toolbar != nullptr) {
                for (QAction* a : toolbar->actions()) {
                    ofButtons = ofButtons || (!a->objectName().isEmpty() && reachable.contains(a->objectName()));
                }
            }
            if (toolbar != nullptr && !ofButtons && !part->findChildren<QLabel*>().isEmpty()) {
                Surface s;
                s.id = "status:" + (part->objectName().isEmpty() ? title : part->objectName());
                if (!seen.contains(s.id)) {
                    seen.insert(s.id);
                    s.group = QStringLiteral("Status bar");
                    s.label = title;
                    s.kind = QStringLiteral("status");
                    s.how = QStringLiteral("use_panel panel=statusbar");
                    s.task = QObject::tr("Using the status bar's %1 (use_panel panel=statusbar): ").arg(quoted(title));
                    s.recipe = QJsonObject{{"press", false},
                                           {"then", QJsonArray{QJsonObject{{"tool", "use_panel"}, {"args", QJsonObject{{"panel", "statusbar"}}}}}},
                                           {"expect", QJsonObject{{"text", QString(QRegularExpression::escape(title) + ":")}}}};
                    out << s;
                }
            }
            for (QWidget* w : part->findChildren<QWidget*>()) {
                if (!isClickable(w)) {
                    continue;
                }
                const QString tip = clean(QString(w->toolTip()).remove(QRegularExpression(QStringLiteral("<[^>]*>"))));
                const QString name = tip.isEmpty() ? w->objectName() : tip.section('.', 0, 0).left(40).trimmed();
                if (name.isEmpty()) {
                    continue;
                }
                Surface s;
                s.id = "status:" + name;
                if (seen.contains(s.id)) {
                    continue;
                }
                seen.insert(s.id);
                s.group = QStringLiteral("Status bar");
                s.label = title + " > " + name;
                s.kind = QStringLiteral("button");
                s.how = QStringLiteral("use_panel panel=statusbar button=\"%1\"").arg(name);
                s.task = QObject::tr("Using the status bar's %1 field (use_panel panel=statusbar): ").arg(quoted(name));
                s.recipe = QJsonObject{{"press", false},
                                       {"then", QJsonArray{QJsonObject{{"tool", "use_panel"},
                                                                       {"args", QJsonObject{{"panel", "statusbar"}, {"button", name}}}}}},
                                       {"expect", QJsonObject{{"fired", QJsonObject{{"status", name}}}}}};
                out << s;
            }
        }
        return out;
    }

    /** Group and label of a written recipe for a part of the window ("context:", "properties:", "prefs:"). */
    void describeWrittenRecipe(const QJsonObject& recipe, QString& group, QString& label) {
        const QString id = recipe.value("id").toString();
        const QString prefix = id.section(':', 0, 0);
        group = recipe.value("group").toString();
        if (group.isEmpty()) {
            group = prefix == "context" ? QStringLiteral("Right-click menus")
                  : prefix == "properties" ? QStringLiteral("Properties panel")
                  : prefix == "prefs" ? QStringLiteral("Drawing preferences")
                  : prefix == "menu" ? QStringLiteral("Menus")
                  : prefix == "panel" ? QStringLiteral("Panels")
                  : prefix == "status" ? QStringLiteral("Status bar")
                  : QStringLiteral("Keyboard shortcuts");
        }
        label = recipe.value("label").toString();
        if (label.isEmpty()) {
            label = id.section(':', 1).replace('_', ' ');
            if (!label.isEmpty()) {
                label[0] = label[0].toUpper();
            }
        }
    }

    LC_AITool setToolbarValue() {
        LC_AITool t;
        t.name = "set_toolbar_value";
        t.label = "Set toolbar value";
        t.description = "Choose a value in one of the toolbar lists (list_toolbar_buttons shows them as kind list): the pen "
                        "colour, width and line type for new entities, a named view, a UCS, a workspace.";
        t.parameters = schema({{"id", str("The list id, e.g. list:pen_toolbar/colorbox")},
                               {"value", str("The option to choose (its text, or its position from 0)")}},
                              {"id", "value"});
        t.executeAsync = [](const QJsonObject& args, Done done) {
            Button b;
            if (!findButton(args.value("id").toString().trimmed(), b) || (!b.combo && !b.menuButton)) {
                done(LC_AIToolResult::error(QStringLiteral("No toolbar list \"%1\". Lists: %2")
                                                .arg(args.value("id").toString(), similarIds("list:").join(", "))));
                return;
            }
            if (b.menuButton) {
                const QString wanted = args.value("value").toVariant().toString();
                const QList<QAction*> items = menuItems(b.menuButton);
                QAction* pick = nullptr;
                for (int i = 0; i < items.size() && pick == nullptr; ++i) {
                    if (norm(itemName(items[i], i)) == norm(wanted)) {
                        pick = items[i];
                    }
                }
                bool isIndex = false;
                const int index = wanted.toInt(&isIndex);
                if (pick == nullptr && isIndex && index >= 0 && index < items.size()) {
                    pick = items[index];
                }
                if (pick == nullptr) {
                    QStringList names;
                    for (int i = 0; i < items.size(); ++i) {
                        names << QStringLiteral("%1: %2").arg(i).arg(itemName(items[i], i));
                    }
                    done(LC_AIToolResult::error(QStringLiteral("No \"%1\" in %2. Options: %3")
                                                    .arg(wanted, b.id, names.isEmpty() ? QStringLiteral("(none)") : names.join(" | "))));
                    return;
                }
                useQtDialogs();
                auto run = makeRun({}, false, std::move(done));
                run->header = QStringLiteral("%1: chose \"%2\".").arg(b.id, itemName(pick, items.indexOf(pick)));
                QPointer<QAction> chosen = pick;
                QTimer::singleShot(0, qApp, [chosen] {
                    if (chosen) {
                        chosen->trigger();
                    }
                });
                run->start(TRIGGER_SETTLE_MS);
                return;
            }
            Field f{b.combo, "choice", {}};
            QString error;
            const QString before = b.combo->currentText();
            useQtDialogs();
            if (!setField(f, args.value("value"), error)) {
                done(LC_AIToolResult::error(QStringLiteral("%1. %2").arg(error, valueOf(f))));
                return;
            }
            auto run = makeRun({}, false, std::move(done));
            run->header = QStringLiteral("%1: \"%2\" (was \"%3\").").arg(b.id, b.combo->currentText(), before);
            run->start(TRIGGER_SETTLE_MS); // some choices open a dialog ("Custom..."), the run reports it
        };
        return t;
    }

    // ------------------------------------------------------------------ recipes and the self-test

    struct FixtureIds {
        QHash<QString, unsigned long long> byName;
    };

    /**
     * The drawing every recipe starts from (all coordinates in drawing units):
     *  L1 line 0,0 - 100,0     L2 line 0,0 - 0,100      L3 line 150,-20 - 150,120   L4 line 120,50 - 200,50
     *  C1 circle 300,50 r40    C2 circle 400,50 r20     A1 arc 500,50 r40 0..180°    E1 ellipse 300,250 major 60 ratio 0.5
     *  P1 closed polyline 0,200 100,200 100,300 0,300   PT1 point 450,250   T1 text "LABEL" at 200,200 height 10
     *  I1 insert of block "FixtureBlock" (circle r10 at its origin) at 600,50     L5 line 0,-60 - 100,-40 (slanted)
     */
    FixtureIds buildFixture(RS_GraphicView* view) {
        FixtureIds ids;
        RS_Graphic* g = view->getGraphic();
        auto add = [&](const QString& name, RS_Entity* e) {
            e->setLayerToActive();
            e->setPenToActive();
            g->addEntity(e);
            ids.byName.insert(name, e->getId());
        };
        add("L1", new RS_Line(g, RS_LineData(RS_Vector(0, 0), RS_Vector(100, 0))));
        add("L2", new RS_Line(g, RS_LineData(RS_Vector(0, 0), RS_Vector(0, 100))));
        add("L3", new RS_Line(g, RS_LineData(RS_Vector(150, -20), RS_Vector(150, 120))));
        add("L4", new RS_Line(g, RS_LineData(RS_Vector(120, 50), RS_Vector(200, 50))));
        add("L5", new RS_Line(g, RS_LineData(RS_Vector(0, -60), RS_Vector(100, -40))));
        add("C1", new RS_Circle(g, RS_CircleData(RS_Vector(300, 50), 40)));
        add("C2", new RS_Circle(g, RS_CircleData(RS_Vector(400, 50), 20)));
        add("A1", new RS_Arc(g, RS_ArcData(RS_Vector(500, 50), 40, 0, M_PI, false)));
        RS_EllipseData ed;
        ed.center = RS_Vector(300, 250);
        ed.majorP = RS_Vector(60, 0);
        ed.ratio = 0.5;
        add("E1", new RS_Ellipse(g, ed));
        auto* poly = new RS_Polyline(g, RS_PolylineData(RS_Vector(false), RS_Vector(false), true));
        poly->addVertex(RS_Vector(0, 200));
        poly->addVertex(RS_Vector(100, 200));
        poly->addVertex(RS_Vector(100, 300));
        poly->addVertex(RS_Vector(0, 300));
        poly->setClosed(true);
        add("P1", poly);
        add("PT1", new RS_Point(g, RS_PointData(RS_Vector(450, 250))));
        auto* block = new RS_Block(g, RS_BlockData("FixtureBlock", RS_Vector(0, 0), false));
        block->addEntity(new RS_Circle(block, RS_CircleData(RS_Vector(0, 0), 10)));
        g->addBlock(block);
        auto* insert = new RS_Insert(g, RS_InsertData("FixtureBlock", RS_Vector(600, 50), RS_Vector(1, 1), 0, 1, 1, RS_Vector(0, 0)));
        add("I1", insert);
        insert->update();
        // text through the add_text tool, as the AI places labels
        for (const LC_AITool& tool : LC_AITools::all()) {
            if (tool.name == "add_text") {
                tool.execute(QJsonObject{{"labels", QJsonArray{QJsonObject{{"text", "LABEL"}, {"x", 200}, {"y", 200}, {"height", 10}}}}});
                for (RS_Entity* e : *g) {
                    if (e != nullptr && !e->isDeleted() && (e->rtti() == RS2::EntityText || e->rtti() == RS2::EntityMText)) {
                        ids.byName.insert("T1", e->getId());
                    }
                }
            }
        }
        g->calculateBorders();
        RS_Selection::unselectAllInDocument(g, view->getViewPort());
        view->getViewPort()->zoomAuto();
        view->redraw(RS2::RedrawAll);
        return ids;
    }

    QJsonArray loadRecipes(QString& error, const bool generated = true) {
        QString path = qEnvironmentVariable("OPENCAD_AI_TOOLBAR_RECIPES");
        if (path.isEmpty()) {
            path = QStringLiteral(":/ai/toolbar_recipes.json");
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            error = QStringLiteral("Cannot read the button recipes (%1).").arg(path);
            return {};
        }
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (doc.isNull()) {
            error = QStringLiteral("The button recipes are not valid JSON: %1").arg(parseError.errorString());
            return {};
        }
        QJsonArray recipes = doc.isArray() ? doc.array() : doc.object().value("recipes").toArray();
        if (generated && (path.startsWith(':') || QFileInfo(path).fileName() == "toolbar_recipes.json")) { // OpenCAD's own recipes
            // the parts of the window without a written recipe get the test made from them
            QSet<QString> written;
            for (const QJsonValue& r : std::as_const(recipes)) {
                written.insert(r.toObject().value("id").toString());
            }
            for (const Surface& sf : collectSurfaces()) {
                if (!written.contains(sf.id) && sf.why.isEmpty()) {
                    QJsonObject r = sf.recipe;
                    r.insert("id", sf.id);
                    r.insert("generated", true);
                    recipes.append(r);
                }
            }
        }
        return recipes;
    }

    QString scratchDir() {
        static const QString dir = [] {
            const QString d = QDir::temp().filePath(QStringLiteral("opencad-ai-button-test-%1").arg(QCoreApplication::applicationPid()));
            QDir().mkpath(d);
            // an image for the buttons that insert one
            QImage image(64, 48, QImage::Format_RGB32);
            image.fill(QColor(70, 130, 180));
            image.save(QDir(d).filePath("test-image.png"));
            return d;
        }();
        return dir;
    }

    /** Recipe strings may say ${TMP} for a scratch folder. */
    QJsonValue expand(const QJsonValue& v) {
        if (v.isString()) {
            QString s = v.toString();
            s.replace(QStringLiteral("${TMP}"), scratchDir());
            s.replace(QStringLiteral("${TEST_IMAGE}"), QDir(scratchDir()).filePath("test-image.png"));
            return s;
        }
        if (v.isArray()) {
            QJsonArray a;
            for (const QJsonValue& x : v.toArray()) {
                a.append(expand(x));
            }
            return a;
        }
        if (v.isObject()) {
            QJsonObject o;
            const QJsonObject in = v.toObject();
            for (auto it = in.begin(); it != in.end(); ++it) {
                o.insert(it.key(), expand(it.value()));
            }
            return o;
        }
        return v;
    }

    using TestResult = LC_AIButtonTest;

    bool g_testsRunning = false;
    bool g_stopTests = false;

    QString savedResultsPath() {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(dir);
        return QDir(dir).filePath(QStringLiteral("ai-button-tests.json"));
    }

    /** Keeps the latest result per id, so the Agent Progress menu shows them in later sessions too. */
    void saveResults(const QList<TestResult>& results) {
        QFile file(savedResultsPath());
        QJsonObject all;
        if (file.open(QIODevice::ReadOnly)) {
            all = QJsonDocument::fromJson(file.readAll()).object();
            file.close();
        }
        const QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
        for (const TestResult& r : results) {
            all.insert(r.id, QJsonObject{{"status", r.status}, {"detail", r.detail.left(4000)}, {"when", now}});
        }
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QJsonDocument(all).toJson());
        }
    }

    /**
     * Runs the recipes one by one, each on a fresh scratch drawing with the fixture, and checks the result.
     * A recipe: {"id", "select": [fixture names], "before": [{"tool", "args"}] (setup, not checked), "steps": [...],
     * "press": false (do not press: a toolbar list), "then": [{"tool", "args"}], "expect": {...},
     * "restore": true (press again afterwards: toggles), "fixture": false (start from an empty drawing), "skip": "why"}.
     * expect: added {type: n} (at least), removed n (at least), dialog "regex" (a dialog opened at some point),
     * running "regex", toggled true, selected n, layers_added n, blocks_added n, windows n (change), view_changed true,
     * view_center [x, y, tol], view_shows [x1, y1, x2, y2], view_scale_ratio [min, max] (units/px after / before),
     * view_center_shift [dx, dy, tol], file "path" (exists), text "regex" (in the tool results), no_error true (default).
     * Every test starts at the same scale and centre; toggles are put back afterwards.
     */
    struct Tester : std::enable_shared_from_this<Tester> {
        QJsonArray recipes;
        int next {0};
        QList<TestResult> results;
        std::function<void(const QList<TestResult>&)> finished;
        std::function<void(int, int, const TestResult&)> progress;

        void record(const TestResult& r) {
            results.append(r);
            if (progress) {
                progress(results.size(), recipes.size(), r);
            }
        }
        QList<LC_AITool> toolSet;
        QPointer<QMdiSubWindow> userWindow;
        // per test
        QJsonObject recipe;
        QStringList outputs;
        bool anyError {false};
        QStringList dialogsSeen;
        QSet<QC_MDIWindow*> windowsBefore;
        Snapshot start;
        bool startChecked {false};
        QPointer<QAction> action;
        int thenIndex {0};
        int beforeIndex {0};
        QHash<QString, bool> savedToggles;
        QList<std::pair<QPointer<QAction>, bool>> savedDocks; // the show/hide switches of the panels and toolbars
        bool savedStatusBar {true};
        // "fired": the menu entry, panel button or status bar field of the recipe really went off
        bool firedSeen {false};
        QList<QMetaObject::Connection> firedConnections;
        QPointer<QWidget> firedWidget;
        std::unique_ptr<QObject> firedWatcher;
        QTimer* watchdog {nullptr};
        bool inTest {false};

        LC_AITool* tool(const QString& name) {
            for (LC_AITool& t : toolSet) {
                if (t.name == name) {
                    return &t;
                }
            }
            return nullptr;
        }

        void begin() {
            if (QC_ApplicationWindow* app = appWindow()) {
                userWindow = app->getMdiArea()->activeSubWindow();
            }
            watchdog = new QTimer(qApp);
            watchdog->setSingleShot(true);
            auto self = shared_from_this();
            QObject::connect(watchdog, &QTimer::timeout, qApp, [self] { self->timedOut(); });
            closeDialogs(3, [self] { self->nextTest(); }); // whatever was open before (a welcome dialog, ...)
        }

        void nextTest() {
            auto self = shared_from_this();
            if (next >= recipes.size() || g_stopTests) {
                QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, g_nativeDialogsBefore);
                g_testsRunning = false;
                g_stopTests = false;
                saveResults(results);
                if (userWindow) {
                    if (QC_ApplicationWindow* app = appWindow()) {
                        app->getMdiArea()->setActiveSubWindow(userWindow);
                    }
                }
                watchdog->deleteLater();
                finished(results);
                return;
            }
            recipe = expand(recipes[next++]).toObject();
            outputs.clear();
            dialogsSeen.clear();
            anyError = false;
            thenIndex = 0;
            const QString id = recipe.value("id").toString();
            if (!recipe.value("skip").toString().isEmpty()) {
                record({id, "skipped", recipe.value("skip").toString(), {}});
                QTimer::singleShot(0, qApp, [self] { self->nextTest(); });
                return;
            }
            Button b;
            if (LC_AIToolbar::isToolRecipe(id)) {
                b = Button{}; // one of the AI's own tools, or a panel, menu or other part of the app: its recipe calls tools in "then"
            }
            else if (!findButton(id, b)) {
                record({id, "failed", "no such button in this build", {}});
                QTimer::singleShot(0, qApp, [self] { self->nextTest(); });
                return;
            }
            action = b.action; // none for a toolbar list: its recipe uses set_toolbar_value
            inTest = true;
            watchdog->start(25000);
            // a fresh drawing with the fixture
            for (QC_MDIWindow* w : drawingWindows()) {
                windowsBefore.insert(w);
            }
            QC_ApplicationWindow* app = appWindow();
            app->slotFileNewFromDefaultTemplate();
            waitForScratch(40);
        }

        /** Until the new scratch drawing is the current one (OpenCAD may still be opening others). */
        void waitForScratch(const int tries) {
            auto self = shared_from_this();
            QC_ApplicationWindow* app = appWindow();
            QC_MDIWindow* current = app != nullptr ? app->getCurrentMDIWindow() : nullptr;
            if (current != nullptr && !windowsBefore.contains(current) && LC_AITools::view() != nullptr) {
                QTimer::singleShot(100, qApp, [self] { self->prepare(); });
                return;
            }
            if (tries <= 0) {
                fail("no scratch drawing could be created");
                return;
            }
            QTimer::singleShot(100, qApp, [self, tries] { self->waitForScratch(tries - 1); });
        }

        void prepare() {
            auto self = shared_from_this();
            RS_GraphicView* view = LC_AITools::view();
            if (view == nullptr) {
                fail("no scratch drawing could be created");
                return;
            }
            FixtureIds ids;
            if (recipe.value("fixture").toBool(true)) {
                ids = buildFixture(view);
            }
            QList<RS_Entity*> select;
            for (const QJsonValue& v : recipe.value("select").toArray()) {
                const unsigned long long id = ids.byName.value(v.toString(), 0);
                for (RS_Entity* e : *view->getGraphic()) {
                    if (e != nullptr && e->getId() == id) {
                        select << e;
                    }
                }
            }
            if (!select.isEmpty()) {
                RS_Selection::selectEntitiesList(view->getGraphic(), view->getViewPort(), select, true);
            }
            standardView(view);
            view->getGraphic()->setModified(false);
            view->redraw(RS2::RedrawAll);
            savedToggles = toggleStates();
            savedDocks = dockStates();
            beforeIndex = 0;
            runBefore();
        }

        /**
         * Every test sees the drawing at the same scale and centre (the fixture fitted to a 740 x 533 px view),
         * so clicks, snap distances in pixels and zooms behave alike in any window size.
         */
        static void standardView(RS_GraphicView* view) {
            LC_GraphicViewport* vp = view->getViewPort();
            if (vp == nullptr || vp->getWidth() <= 0) {
                return;
            }
            constexpr double factor = 72.0 / 61.0; // 610 units of fixture over 720 px
            const int ox = qRound(vp->getWidth() / 2.0 - 305.0 * factor);
            const int oy = qRound(vp->getHeight() / 2.0 - 120.7292 * factor);
            vp->setOffsetAndFactor(ox, oy, factor);
        }

        /** The on/off state of every toggle button (snaps, view modes, panels...), to put back after a test. */
        static QHash<QString, bool> toggleStates() {
            QHash<QString, bool> states;
            for (const Button& b : collectButtons()) {
                if (b.action && kindOf(b.action) == "toggle" && !REFUSED.contains(b.id)) {
                    states.insert(b.id, b.action->isChecked());
                }
            }
            return states;
        }

        void restoreToggles() {
            for (int pass = 0; pass < 3; ++pass) { // exclusive groups: one switch can flip another
                bool changed = false;
                for (const Button& b : collectButtons()) {
                    if (b.action && savedToggles.contains(b.id) && b.action->isEnabled() && b.action->isVisible()
                        && b.action->isChecked() != savedToggles.value(b.id)) {
                        b.action->trigger();
                        changed = true;
                    }
                }
                if (!changed) {
                    break;
                }
            }
        }

        /** Setup calls of the recipe ("before"), then the press itself. */
        void runBefore() {
            auto self = shared_from_this();
            const QJsonArray before = recipe.value("before").toArray();
            if (beforeIndex < before.size()) {
                const QJsonObject c = before[beforeIndex++].toObject();
                call(c.value("tool").toString(), c.value("args").toObject(), [self] { self->runBefore(); });
                return;
            }
            outputs.clear(); // only what the button did counts
            dialogsSeen.clear();
            anyError = false;
            QString greyedOut;
            if (!armFired(greyedOut)) {
                fail("cannot find what the recipe uses: "
                     + QString::fromUtf8(QJsonDocument(recipe.value("expect").toObject().value("fired").toObject()).toJson(QJsonDocument::Compact)));
                return;
            }
            if (!greyedOut.isEmpty() && recipe.value("generated").toBool()) {
                // a test made from the entry: it needs a drawing where it applies (a written recipe sets that up)
                record({recipe.value("id").toString(), "skipped", greyedOut + "; it needs a written recipe that sets up a drawing where it applies", {}});
                cleanup();
                return;
            }
            start = snapshot();
            startChecked = action ? action->isChecked() : false;
            if (!action || !recipe.value("press").toBool(true) || LC_AIToolbar::isToolRecipe(recipe.value("id").toString())) {
                runThen();
                return;
            }
            QJsonObject args{{"id", recipe.value("id")}, {"steps", recipe.value("steps").toArray()}};
            call("press_toolbar_button", args, [self] { self->runThen(); });
        }

        /** Clicks on a widget that is not a button (status bar fields). */
        struct ClickWatcher : QObject {
            bool* seen {nullptr};
            bool eventFilter(QObject* watched, QEvent* event) override {
                if (event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick) {
                    *seen = true;
                }
                return QObject::eventFilter(watched, event);
            }
        };

        /** Starts watching what "expect.fired" names; false when it cannot be found. greyedOut says why it cannot be used now. */
        bool armFired(QString& greyedOut) {
            firedSeen = false;
            const QJsonObject fired = recipe.value("expect").toObject().value("fired").toObject();
            if (fired.isEmpty()) {
                return true;
            }
            QAction* target = nullptr;
            if (fired.contains("menu")) {
                for (const MenuEntry& e : menuBarEntries()) {
                    if (e.path == fired.value("menu").toString()) {
                        target = e.action;
                        break;
                    }
                }
            }
            else if (fired.contains("action")) {
                Button b;
                target = findButton(fired.value("action").toString(), b) ? b.action.data() : nullptr;
            }
            if (target != nullptr) {
                if (!target->isEnabled()) {
                    greyedOut = QStringLiteral("\"%1\" is greyed out on the test drawing").arg(clean(target->text()));
                }
                firedConnections << QObject::connect(target, &QAction::triggered, qApp, [this] { firedSeen = true; });
                if (!action) {
                    action = target; // a switch is put back afterwards ("restore")
                }
                return true;
            }
            if (fired.contains("panel")) {
                QDockWidget* dock = findPanel(fired.value("panel").toString());
                QWidget* content = dock != nullptr && dock->widget() != nullptr ? dock->widget() : dock;
                if (content == nullptr) {
                    return false;
                }
                for (QAbstractButton* b : dialogButtons(content)) {
                    if (buttonName(b) == fired.value("button").toString()) {
                        if (!b->isEnabled()) {
                            greyedOut = QStringLiteral("\"%1\" is greyed out on the test drawing").arg(buttonName(b));
                        }
                        // pressed comes before the click's own work (which may be a dialog that waits for an answer)
                        firedConnections << QObject::connect(b, &QAbstractButton::pressed, qApp, [this] { firedSeen = true; });
                        return true;
                    }
                }
                return false;
            }
            if (fired.contains("status")) {
                QWidget* w = findStatusWidget(fired.value("status").toString());
                if (w == nullptr) {
                    return false;
                }
                auto* watcher = new ClickWatcher;
                watcher->seen = &firedSeen;
                firedWatcher.reset(watcher);
                firedWidget = w;
                w->installEventFilter(watcher);
                return true;
            }
            return false;
        }

        void disarmFired() {
            for (const QMetaObject::Connection& c : std::as_const(firedConnections)) {
                QObject::disconnect(c);
            }
            firedConnections.clear();
            if (firedWidget && firedWatcher) {
                firedWidget->removeEventFilter(firedWatcher.get());
            }
            firedWatcher.reset();
            firedWidget = nullptr;
        }

        /** Which panels and toolbars are shown: a test may show or hide some (use_panel, the Workspace menu). */
        QList<std::pair<QPointer<QAction>, bool>> dockStates() {
            QList<std::pair<QPointer<QAction>, bool>> states;
            if (QC_ApplicationWindow* app = appWindow()) {
                for (QDockWidget* dock : app->findChildren<QDockWidget*>()) {
                    states.append({dock->toggleViewAction(), dock->toggleViewAction()->isChecked()});
                }
                for (QToolBar* toolbar : app->findChildren<QToolBar*>()) {
                    states.append({toolbar->toggleViewAction(), toolbar->toggleViewAction()->isChecked()});
                }
                savedStatusBar = app->statusBar() == nullptr || !app->statusBar()->isHidden();
            }
            return states;
        }

        void restoreDocks() {
            for (const auto& [toggle, shown] : std::as_const(savedDocks)) {
                if (toggle && toggle->isChecked() != shown) {
                    toggle->trigger();
                }
            }
            if (QC_ApplicationWindow* app = appWindow(); app != nullptr && app->statusBar() != nullptr && app->statusBar()->isHidden() == savedStatusBar) {
                app->statusBar()->setVisible(savedStatusBar);
            }
            savedDocks.clear();
        }

        void call(const QString& name, const QJsonObject& args, std::function<void()> then) {
            LC_AITool* t = tool(name);
            if (t == nullptr) {
                outputs << "unknown tool " + name;
                anyError = true;
                then();
                return;
            }
            auto self = shared_from_this();
            auto collect = [self, name, then](const LC_AIToolResult& r) {
                if (!self->inTest) {
                    return;
                }
                const QString text = r.joinedText();
                self->outputs << name + ": " + text;
                self->anyError = self->anyError || r.isError;
                static const QRegularExpression dialogLine(QStringLiteral("Dialog \"([^\"]*)\""));
                for (auto it = dialogLine.globalMatch(text); it.hasNext();) {
                    self->dialogsSeen << it.next().captured(1);
                }
                QTimer::singleShot(0, qApp, then);
            };
            if (t->executeAsync) {
                t->executeAsync(args, collect);
            }
            else {
                collect(t->execute(args));
            }
        }

        void runThen() {
            auto self = shared_from_this();
            const QJsonArray then = recipe.value("then").toArray();
            if (thenIndex >= then.size()) {
                evaluate();
                return;
            }
            const QJsonObject c = then[thenIndex++].toObject();
            call(c.value("tool").toString(), c.value("args").toObject(), [self] { self->runThen(); });
        }

        void evaluate() {
            const Snapshot end = snapshot();
            const QJsonObject expect = recipe.value("expect").toObject();
            QStringList problems;
            QHash<unsigned long long, bool> startIds;
            for (auto it = start.entities.cbegin(); it != start.entities.cend(); ++it) {
                startIds.insert(it.key(), true);
            }
            QHash<QString, int> addedTypes;
            int added = 0;
            int removed = 0;
            for (auto it = end.entities.cbegin(); it != end.entities.cend(); ++it) {
                if (!startIds.contains(it.key())) {
                    addedTypes[LC_AITools::typeName(it.value())]++;
                    ++added;
                }
            }
            for (auto it = start.entities.cbegin(); it != start.entities.cend(); ++it) {
                if (!end.entities.contains(it.key())) {
                    ++removed;
                }
            }
            const QJsonObject addedWanted = expect.value("added").toObject();
            for (auto it = addedWanted.begin(); it != addedWanted.end(); ++it) {
                const int want = it.value().toInt(1);
                const int got = it.key() == "any" ? added : addedTypes.value(it.key());
                if (got < want) {
                    problems << QStringLiteral("expected %1 new %2, got %3").arg(want).arg(it.key()).arg(got);
                }
            }
            if (expect.contains("removed") && removed < expect.value("removed").toInt()) {
                problems << QStringLiteral("expected %1 removed, got %2").arg(expect.value("removed").toInt()).arg(removed);
            }
            if (expect.contains("dialog")) {
                const QRegularExpression re(expect.value("dialog").toString(), QRegularExpression::CaseInsensitiveOption);
                bool seen = false;
                for (const QString& title : std::as_const(dialogsSeen)) {
                    seen = seen || re.match(title).hasMatch();
                }
                if (!seen) {
                    problems << QStringLiteral("expected a dialog /%1/, saw: %2").arg(expect.value("dialog").toString(),
                                                                                     dialogsSeen.isEmpty() ? "none" : dialogsSeen.join(", "));
                }
            }
            if (expect.contains("running")) {
                const QRegularExpression re(expect.value("running").toString(), QRegularExpression::CaseInsensitiveOption);
                const QString all = outputs.join('\n');
                static const QRegularExpression runLine(QStringLiteral("Running tool: ([^\\n]*)"));
                bool seen = false;
                for (auto it = runLine.globalMatch(all); it.hasNext();) {
                    const QString tool = it.next().captured(1);
                    seen = seen || re.match(tool).hasMatch();
                }
                if (!seen) {
                    problems << QStringLiteral("expected the tool /%1/ to run").arg(expect.value("running").toString());
                }
            }
            if (expect.value("toggled").toBool() && action && action->isChecked() == startChecked) {
                problems << "the toggle did not switch";
            }
            if (expect.contains("selected") && end.selected != expect.value("selected").toInt()) {
                problems << QStringLiteral("expected %1 selected, got %2").arg(expect.value("selected").toInt()).arg(end.selected);
            }
            if (expect.contains("layers_added") && end.layers.size() - start.layers.size() < expect.value("layers_added").toInt()) {
                problems << QStringLiteral("expected %1 new layer(s), got %2").arg(expect.value("layers_added").toInt())
                                .arg(end.layers.size() - start.layers.size());
            }
            if (expect.contains("blocks_added") && end.blocks.size() - start.blocks.size() < expect.value("blocks_added").toInt()) {
                problems << QStringLiteral("expected %1 new block(s), got %2").arg(expect.value("blocks_added").toInt())
                                .arg(end.blocks.size() - start.blocks.size());
            }
            if (expect.contains("windows") && end.windows - start.windows != expect.value("windows").toInt()) {
                problems << QStringLiteral("expected the open drawings to change by %1, got %2").arg(expect.value("windows").toInt())
                                .arg(end.windows - start.windows);
            }
            if (expect.value("view_changed").toBool() && end.view == start.view) {
                problems << "the view did not change";
            }
            const QString area = QStringLiteral("visible %1,%2 .. %3,%4 at %5 units/px")
                                     .arg(end.vx1, 0, 'f', 1).arg(end.vy1, 0, 'f', 1).arg(end.vx2, 0, 'f', 1).arg(end.vy2, 0, 'f', 1)
                                     .arg(end.upp, 0, 'f', 4);
            if (const QJsonArray c = expect.value("view_center").toArray(); c.size() >= 2) {
                const double tol = c.size() > 2 ? c[2].toDouble() : 2.0;
                const double cx = (end.vx1 + end.vx2) / 2;
                const double cy = (end.vy1 + end.vy2) / 2;
                if (std::abs(cx - c[0].toDouble()) > tol || std::abs(cy - c[1].toDouble()) > tol) {
                    problems << QStringLiteral("expected the view centred at %1,%2, got %3,%4 (%5)")
                                    .arg(c[0].toDouble()).arg(c[1].toDouble()).arg(cx, 0, 'f', 2).arg(cy, 0, 'f', 2).arg(area);
                }
            }
            if (const QJsonArray r = expect.value("view_shows").toArray(); r.size() == 4) {
                const double x1 = std::min(r[0].toDouble(), r[2].toDouble()), x2 = std::max(r[0].toDouble(), r[2].toDouble());
                const double y1 = std::min(r[1].toDouble(), r[3].toDouble()), y2 = std::max(r[1].toDouble(), r[3].toDouble());
                if (end.vx1 > x1 + 1 || end.vy1 > y1 + 1 || end.vx2 < x2 - 1 || end.vy2 < y2 - 1) {
                    problems << QStringLiteral("expected the view to show %1,%2 .. %3,%4, %5").arg(x1).arg(y1).arg(x2).arg(y2).arg(area);
                }
            }
            if (const QJsonArray r = expect.value("view_scale_ratio").toArray(); r.size() == 2 && start.upp > 0) {
                const double ratio = end.upp / start.upp;
                if (ratio < r[0].toDouble() || ratio > r[1].toDouble()) {
                    problems << QStringLiteral("expected the zoom to change the scale by %1..%2, got %3 (%4)")
                                    .arg(r[0].toDouble()).arg(r[1].toDouble()).arg(ratio, 0, 'f', 3).arg(area);
                }
            }
            if (const QJsonArray d = expect.value("view_center_shift").toArray(); d.size() >= 2) {
                const double tol = d.size() > 2 ? d[2].toDouble() : 2.0;
                const double dx = (end.vx1 + end.vx2 - start.vx1 - start.vx2) / 2;
                const double dy = (end.vy1 + end.vy2 - start.vy1 - start.vy2) / 2;
                if (std::abs(dx - d[0].toDouble()) > tol || std::abs(dy - d[1].toDouble()) > tol) {
                    problems << QStringLiteral("expected the view to move by %1,%2, it moved by %3,%4")
                                    .arg(d[0].toDouble()).arg(d[1].toDouble()).arg(dx, 0, 'f', 2).arg(dy, 0, 'f', 2);
                }
            }
            if (expect.contains("file") && !QFileInfo::exists(expect.value("file").toString())) {
                problems << "no file " + expect.value("file").toString();
            }
            if (expect.contains("text")) {
                const QRegularExpression re(expect.value("text").toString(), QRegularExpression::CaseInsensitiveOption);
                if (!re.match(outputs.join('\n')).hasMatch()) {
                    problems << QStringLiteral("expected /%1/ in the results").arg(expect.value("text").toString());
                }
            }
            // an entry whose own work is a modal dialog announces itself only when the dialog closes: the dialog is the proof;
            // a menu that is rebuilt each time it opens (Workspace > Drawings) has new entries: its switch flipping is the proof
            static const QRegularExpression flipped(QStringLiteral("\" is now (on|off) \\(was (off|on)\\)"));
            const bool switched = expect.value("fired").toObject().contains("menu") && flipped.match(outputs.join('\n')).hasMatch();
            if (expect.contains("fired") && !firedSeen && !switched && dialogsSeen.isEmpty() && QApplication::activeModalWidget() == nullptr) {
                problems << "the entry was not set off: " + QString::fromUtf8(QJsonDocument(expect.value("fired").toObject()).toJson(QJsonDocument::Compact));
            }
            if (expect.value("no_error").toBool(true) && anyError) {
                problems << "a tool call failed";
            }
            QString detail = problems.join("; ");
            if (!problems.isEmpty()) {
                QString out = outputs.join("\n---\n");
                if (out.size() > 12000) {
                    out = out.left(12000) + " ...";
                }
                detail += "\n" + out;
            }
            else {
                QStringList summary;
                if (added > 0) {
                    summary << QStringLiteral("+%1 entities").arg(added);
                }
                if (removed > 0) {
                    summary << QStringLiteral("-%1 entities").arg(removed);
                }
                if (!dialogsSeen.isEmpty()) {
                    summary << "dialog " + dialogsSeen.join(", ");
                }
                detail = summary.join(", ");
            }
            record({recipe.value("id").toString(), problems.isEmpty() ? "passed" : "failed", detail, {}});
            cleanup();
        }

        void fail(const QString& why) {
            record({recipe.value("id").toString(), "failed", why + "\n" + outputs.join("\n---\n").left(12000), {}});
            cleanup();
        }

        void timedOut() {
            if (!inTest) {
                return;
            }
            fail("timed out (a tool call never answered)");
        }

        void cleanup() {
            inTest = false;
            watchdog->stop();
            disarmFired();
            auto self = shared_from_this();
            // close what the test left open: dialogs (several rounds: closing one may open another), tools, toggles
            closeDialogs(6, [self] {
                if (RS_GraphicView* view = LC_AITools::view()) {
                    view->killAllActions();
                }
                if (self->recipe.value("restore").toBool() && self->action && self->action->isEnabled()
                    && self->action->isChecked() != self->startChecked) {
                    self->action->trigger();
                }
                self->restoreToggles(); // no test leaves a snap or view mode changed for the next one
                self->savedToggles.clear();
                self->restoreDocks();
                QTimer::singleShot(80, qApp, [self] {
                    self->closeDialogs(6, [self] { self->closeTestWindows(); });
                });
            });
        }

        void closeDialogs(const int rounds, std::function<void()> then) {
            QWidget* dialog = currentDialog();
            if (dialog == nullptr || rounds <= 0) {
                then();
                return;
            }
            if (auto* box = qobject_cast<QMessageBox*>(dialog)) {
                QAbstractButton* no = box->button(QMessageBox::No);
                QAbstractButton* cancel = box->button(QMessageBox::Cancel);
                QAbstractButton* pick = no != nullptr ? no : cancel != nullptr ? cancel : box->escapeButton();
                if (pick != nullptr) {
                    QTimer::singleShot(0, pick, &QAbstractButton::click);
                }
                else {
                    QTimer::singleShot(0, box, &QDialog::reject);
                }
            }
            else if (auto* d = qobject_cast<QDialog*>(dialog)) {
                QTimer::singleShot(0, d, &QDialog::reject);
            }
            else {
                QTimer::singleShot(0, dialog, &QWidget::close);
            }
            auto self = shared_from_this();
            QTimer::singleShot(150, qApp, [self, rounds, then] { self->closeDialogs(rounds - 1, then); });
        }

        void closeTestWindows() {
            auto self = shared_from_this();
            QC_ApplicationWindow* app = appWindow();
            for (QC_MDIWindow* w : drawingWindows()) {
                if (!windowsBefore.contains(w) && app != nullptr) {
                    if (w->getDocument() != nullptr) {
                        w->getDocument()->setModified(false);
                    }
                    app->closeWindow(w);
                }
            }
            windowsBefore.clear();
            QTimer::singleShot(60, qApp, [self] {
                self->closeDialogs(3, [self] { self->nextTest(); });
            });
        }
    };

    void runRecipes(const QJsonArray& recipes, const QList<LC_AITool>& toolSet, std::function<void(const QList<TestResult>&)> finished,
                    std::function<void(int, int, const TestResult&)> progress = {}) {
        auto tester = std::make_shared<Tester>();
        tester->recipes = recipes;
        tester->toolSet = toolSet;
        tester->finished = std::move(finished);
        tester->progress = std::move(progress);
        g_testsRunning = true;
        g_stopTests = false;
        g_nativeDialogsBefore = g_forcedQtDialogs ? g_nativeDialogsBefore : QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        tester->begin();
    }

    LC_AITool testButtons() {
        LC_AITool t;
        t.name = "test_toolbar_buttons";
        t.label = "Test toolbar buttons";
        t.description =
            "Test OpenCAD's toolbar buttons: each one is pressed with a known recipe on its own scratch drawing (opened "
            "and closed again; your drawings are not touched), and the result is checked. Give ids or a toolbar to test "
            "some, or nothing to test all of them (that takes a few minutes). Returns passed / failed / skipped per button.";
        t.parameters = schema({{"ids", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}},
                                                   {"description", "Button ids to test"}}},
                               {"toolbar", str("Test the buttons of this toolbar")}});
        t.executeAsync = [](const QJsonObject& args, Done done) {
            if (g_testsRunning) {
                done(LC_AIToolResult::error(QStringLiteral("Button tests are already running.")));
                return;
            }
            QString error;
            const QJsonArray all = loadRecipes(error);
            if (all.isEmpty()) {
                done(LC_AIToolResult::error(error.isEmpty() ? QStringLiteral("There are no button recipes.") : error));
                return;
            }
            QSet<QString> wanted;
            for (const QJsonValue& v : args.value("ids").toArray()) {
                wanted.insert(v.toString().toLower());
            }
            const QString toolbar = norm(args.value("toolbar").toString());
            if (!toolbar.isEmpty()) {
                for (const Button& b : collectButtons()) {
                    for (const QString& p : b.places) {
                        if (norm(p.section(" > ", 0, 0)) == toolbar) {
                            wanted.insert(b.id.toLower());
                        }
                    }
                }
            }
            QJsonArray chosen;
            for (const QJsonValue& r : all) {
                if (wanted.isEmpty() || wanted.contains(r.toObject().value("id").toString().toLower())) {
                    chosen.append(r);
                }
            }
            if (chosen.isEmpty()) {
                done(LC_AIToolResult::error(QStringLiteral("No recipe for those buttons.")));
                return;
            }
            runRecipes(chosen, LC_AIToolbar::tools() + LC_AITools::all(), [done](const QList<TestResult>& results) {
                int passed = 0, failed = 0, skipped = 0;
                QStringList lines;
                for (const TestResult& r : results) {
                    if (r.status == "passed") {
                        ++passed;
                        lines << QStringLiteral("PASS %1 %2").arg(r.id, r.detail.isEmpty() ? QString() : "(" + r.detail + ")");
                    }
                    else if (r.status == "skipped") {
                        ++skipped;
                        lines << QStringLiteral("SKIP %1: %2").arg(r.id, r.detail);
                    }
                    else {
                        ++failed;
                        lines << QStringLiteral("FAIL %1: %2").arg(r.id, r.detail.left(600));
                    }
                }
                lines.prepend(QStringLiteral("%1 passed, %2 failed, %3 skipped.").arg(passed).arg(failed).arg(skipped));
                LC_AIToolResult result = LC_AIToolResult::text(lines.join('\n'));
                result.isError = failed > 0;
                done(result);
            });
        };
        return t;
    }

    // ------------------------------------------------------------------ developer mode: test all, write the checklist

    void writeChecklist(const QString& path, const QJsonArray& inventory, const QHash<QString, TestResult>& results,
                        const QHash<QString, QJsonObject>& recipes) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            return;
        }
        int agentic = 0, tested = 0, failedCount = 0, total = 0;
        QStringList rows;
        QString lastToolbar;
        for (const QJsonValue& v : inventory) {
            const QJsonObject b = v.toObject();
            const QString id = b.value("id").toString();
            const QString toolbar = b.value("toolbars").toArray().first().toString().section(" > ", 0, 0);
            if (toolbar != lastToolbar) {
                rows << "" << QStringLiteral("## %1").arg(toolbar) << ""
                     << "| Button | Id | Kind | Agentic | Tested | Notes |" << "|---|---|---|---|---|---|";
                lastToolbar = toolbar;
            }
            ++total;
            const QString notAgentic = LC_AIToolbar::notAgenticReason(id);
            const bool refused = !notAgentic.isEmpty();
            const bool isAgentic = !refused;
            agentic += isAgentic ? 1 : 0;
            QString testedMark = "[ ]";
            QString note;
            if (const auto it = results.constFind(id); it != results.constEnd() && !refused) {
                if (it->status == "passed") {
                    testedMark = "[x]";
                    ++tested;
                    note = it->detail;
                }
                else if (it->status == "skipped") {
                    testedMark = "[-]";
                    note = "not run: " + it->detail;
                }
                else {
                    testedMark = "[!]";
                    ++failedCount;
                    note = "FAILED: " + it->detail.section('\n', 0, 0);
                }
            }
            if (refused) {
                testedMark = "[-]";
                note = notAgentic;
            }
            else if (!recipes.contains(id)) {
                note = "no recipe yet";
            }
            const QJsonArray places = b.value("toolbars").toArray();
            QStringList where;
            for (const QJsonValue& p : places) {
                if (p.toString() != places.first().toString()) {
                    where << p.toString();
                }
            }
            if (!where.isEmpty()) {
                note += (note.isEmpty() ? "" : "; ") + QStringLiteral("also in ") + where.join(", ");
            }
            note.replace('|', '/');
            rows << QStringLiteral("| %1 | %2 | %3 | %4 | %5 | %6 |")
                        .arg(b.value("label").toString().replace('|', '/'), id, b.value("kind").toString(),
                             isAgentic ? "[x]" : "[-]", testedMark, note);
        }
        // the rest of the window, group by group (ids "menu:", "shortcut:", "panel:", "status:", "context:", ...)
        int partsTotal = 0, partsAgentic = 0, partsPassed = 0, partsFailed = 0;
        QString lastGroup;
        for (const QJsonValue& v : LC_AIToolbar::surfaces()) {
            const QJsonObject part = v.toObject();
            const QString id = part.value("id").toString();
            const QString group = part.value("group").toString();
            if (group != lastGroup) {
                rows << "" << QStringLiteral("## %1").arg(group) << ""
                     << "| Entry | Id | Kind | Agentic | Tested | Notes |" << "|---|---|---|---|---|---|";
                lastGroup = group;
            }
            ++partsTotal;
            const QString why = part.value("why").toString();
            partsAgentic += why.isEmpty() ? 1 : 0;
            QString testedMark = "[ ]";
            QString note = part.value("how").toString();
            if (!why.isEmpty()) {
                testedMark = "[-]";
                note = why;
            }
            else if (const auto it = results.constFind(id); it != results.constEnd()) {
                testedMark = it->status == "passed" ? "[x]" : it->status == "skipped" ? "[-]" : "[!]";
                partsPassed += it->status == "passed" ? 1 : 0;
                partsFailed += it->status == "failed" ? 1 : 0;
                if (it->status == "skipped") {
                    note = it->detail;
                }
                else if (it->status == "failed") {
                    note = "FAILED: " + it->detail.section('\n', 0, 0);
                }
            }
            note.replace('|', '/').replace('\n', ' ');
            rows << QStringLiteral("| %1 | %2 | %3 | %4 | %5 | %6 |")
                        .arg(part.value("label").toString().replace('|', '/'), QString(id).replace('|', '/'), part.value("kind").toString(),
                             why.isEmpty() ? "[x]" : "[-]", testedMark, note);
        }
        // the AI's own tools, tested the same way (ids "tool:<name>")
        int toolsTotal = 0, toolsPassed = 0;
        rows << "" << "## AI tools (what the AI calls)" << "" << "| Tool | Name | Agentic | Tested | Notes |" << "|---|---|---|---|---|";
        QStringList toolNames;
        QHash<QString, QString> toolLabels;
        for (const LC_AITool& t : LC_AITools::all() + LC_AIToolbar::tools()) {
            toolNames << t.name;
            toolLabels.insert(t.name, t.label);
        }
        for (auto it = recipes.cbegin(); it != recipes.cend(); ++it) {
            if (it.key().startsWith("tool:") && !toolNames.contains(it.key().mid(5))) {
                toolNames << it.key().mid(5); // tools of the chat (questions, browser) the runner does not call
            }
        }
        for (const QString& name : std::as_const(toolNames)) {
            ++toolsTotal;
            QString testedMark = "[ ]";
            QString note = "no recipe yet";
            if (const auto it = results.constFind("tool:" + name); it != results.constEnd()) {
                testedMark = it->status == "passed" ? "[x]" : it->status == "skipped" ? "[-]" : "[!]";
                note = it->status == "skipped" ? "not run: " + it->detail : it->status == "passed" ? it->detail : "FAILED: " + it->detail.section('\n', 0, 0);
                toolsPassed += it->status == "passed" ? 1 : 0;
            }
            note.replace('|', '/');
            rows << QStringLiteral("| %1 | %2 | [x] | %3 | %4 |").arg(toolLabels.value(name, name), name, testedMark, note);
        }
        QStringList head;
        head << "# OpenCAD: what the AI can use (checklist)" << ""
             << QStringLiteral("Generated by OpenCAD's self-test (OPENCAD_AI_TOOLBAR_TEST) on %1.")
                    .arg(QDateTime::currentDateTime().toString(Qt::ISODate))
             << ""
             << "Agentic: the AI can use it. Toolbar buttons: list_toolbar_buttons, press_toolbar_button, then run_command, "
                "click_drawing, select_entities, handle_dialog; lists with set_toolbar_value. Menus: use_menu. Panels and the "
                "status bar: use_panel. Right-click menus: context_menu (drawing) or use_panel item click right (panels). "
                "Properties panel: use_properties. Anything else: look_at_app and press_in_app."
             << "Tested: [x] used with its recipe in the real app and the result checked, [!] the check failed, "
                "[-] not run on purpose (see notes), [ ] not tested yet. Menu entries, panel buttons, status bar fields and "
                "shortcut-only commands are tested by using them with the AI's tool and checking they went off; what the "
                "command then does is tested with the toolbar button that runs it."
             << ""
             << QStringLiteral("Toolbar buttons: total %1 | agentic %2 | tested and passed %3 | failed %4")
                    .arg(total).arg(agentic).arg(tested).arg(failedCount)
             << ""
             << QStringLiteral("Menus, panels, status bar, right-click menus, Properties and preferences: total %1 | agentic %2 | "
                               "tested and passed %3 | failed %4")
                    .arg(partsTotal).arg(partsAgentic).arg(partsPassed).arg(partsFailed)
             << ""
             << QStringLiteral("AI tools: total %1 | tested and passed %2").arg(toolsTotal).arg(toolsPassed)
             << ""
             << "Run it again: scripts/ai/test_toolbar_buttons.sh (or Agent Progress > Open Agent Progress... in OpenCAD).";
        QTextStream out(&file);
        out << (head + rows).join('\n') << '\n';
    }

    void developerSelfTest() {
        const QString report = qEnvironmentVariable("OPENCAD_AI_TOOLBAR_TEST");
        if (report.isEmpty()) {
            return;
        }
        auto* poll = new QTimer(qApp);
        poll->setInterval(500);
        QObject::connect(poll, &QTimer::timeout, qApp, [poll, report] {
            static int tries = 0;
            QC_ApplicationWindow* app = appWindow();
            if ((app == nullptr || collectButtons().isEmpty() || drawingWindows().isEmpty()) && ++tries < 60) {
                return; // OpenCAD opens its first drawing a moment after the window
            }
            poll->stop();
            poll->deleteLater();
            const QJsonArray inventory = LC_AIToolbar::inventory();
            if (const QString dump = qEnvironmentVariable("OPENCAD_AI_TOOLBAR_INVENTORY"); !dump.isEmpty()) {
                QFile f(dump);
                if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    f.write(QJsonDocument(inventory).toJson());
                }
            }
            if (const QString dump = qEnvironmentVariable("OPENCAD_AI_SURFACES_INVENTORY"); !dump.isEmpty()) {
                QFile f(dump); // the menus, panels and other parts of the window
                if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    f.write(QJsonDocument(LC_AIToolbar::surfaces()).toJson());
                }
            }
            QString error;
            QJsonArray recipes = loadRecipes(error);
            if (const QString only = qEnvironmentVariable("OPENCAD_AI_TOOLBAR_ONLY"); !only.isEmpty()) {
                const QStringList ids = only.split(',', Qt::SkipEmptyParts);
                QJsonArray chosen;
                for (const QJsonValue& r : recipes) {
                    const QString id = r.toObject().value("id").toString();
                    // "menu:*" chooses every id starting with "menu:"
                    if (ids.contains(id) || std::any_of(ids.cbegin(), ids.cend(), [&id](const QString& w) {
                            return w.endsWith('*') && id.startsWith(w.chopped(1));
                        })) {
                        chosen.append(r);
                    }
                }
                recipes = chosen;
            }
            if (qEnvironmentVariableIsSet("OPENCAD_AI_TOOLBAR_DUMP_ONLY")) {
                recipes = {};
            }
            // the AI chat opens with OpenCAD; the recipes were written without it taking room from the drawing
            if (auto* chat = app->findChild<QAction*>("AIAssistantToggle"); chat != nullptr && chat->isChecked()) {
                chat->setChecked(false);
                QApplication::processEvents();
            }
            // the same view size as the recipes were written with (a fresh settings folder opens a small window)
            for (int i = 0; i < 6; ++i) {
                RS_GraphicView* view = LC_AITools::view();
                if (view == nullptr || (view->width() == 740 && view->height() == 533)) {
                    break;
                }
                app->resize(app->width() + 740 - view->width(), app->height() + 533 - view->height());
                QApplication::processEvents();
                QApplication::processEvents();
            }
            if (RS_GraphicView* view = LC_AITools::view()) {
                fprintf(stderr, "AI button self-test: view %d x %d px\n", view->width(), view->height());
            }
            QHash<QString, QJsonObject> byId;
            for (const QJsonValue& r : recipes) {
                byId.insert(r.toObject().value("id").toString(), r.toObject());
            }
            runRecipes(recipes, LC_AIToolbar::tools() + LC_AITools::all(), [report, inventory, byId](const QList<TestResult>& results) {
                QJsonArray out;
                QHash<QString, TestResult> byResult;
                for (const TestResult& r : results) {
                    out.append(QJsonObject{{"id", r.id}, {"status", r.status}, {"detail", r.detail}});
                    byResult.insert(r.id, r);
                }
                QFile f(report);
                if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    f.write(QJsonDocument(out).toJson());
                    f.close();
                }
                if (const QString checklist = qEnvironmentVariable("OPENCAD_AI_TOOLBAR_CHECKLIST"); !checklist.isEmpty()) {
                    writeChecklist(checklist, inventory, byResult, byId);
                }
                fflush(stdout);
                std::_Exit(0); // the scratch drawings are closed; skip the save prompts of a normal exit
            });
        });
        poll->start();
    }
}

Q_COREAPP_STARTUP_FUNCTION(developerSelfTest)

namespace {
    void e2eStart() {
        LC_AIE2E::start();
    }
}
Q_COREAPP_STARTUP_FUNCTION(e2eStart)

QJsonArray LC_AIToolbar::inventory() {
    QJsonArray out;
    for (const Button& b : collectButtons()) {
        QJsonObject o{{"id", b.id}, {"toolbars", QJsonArray::fromStringList(b.places)}, {"kind", kindOf(b)}};
        if (b.action) {
            o.insert("label", labelOf(b.action));
            o.insert("tooltip", clean(b.action->toolTip()));
            o.insert("checkable", b.action->isCheckable());
            o.insert("enabled", b.action->isEnabled());
            o.insert("state", stateOf(b));
            if (b.action->property("RS2:actionType").isValid()) {
                o.insert("actionType", b.action->property("RS2:actionType").toInt());
            }
        }
        else if (b.combo) {
            o.insert("label", QStringLiteral("%1 list").arg(b.combo->objectName()));
            o.insert("state", b.combo->currentText());
        }
        else if (b.menuButton) {
            o.insert("label", QString(b.id.section(QChar(0x2F), 1) + " list"));
        }
        out.append(o);
    }
    return out;
}

QJsonArray LC_AIToolbar::surfaces() {
    QJsonArray out;
    QSet<QString> listed;
    for (const Surface& sf : collectSurfaces()) {
        out.append(QJsonObject{{"id", sf.id}, {"group", sf.group}, {"label", sf.label}, {"kind", sf.kind}, {"how", sf.how},
                               {"task", sf.task}, {"why", sf.why}});
        listed.insert(sf.id);
    }
    // parts only written recipes reach (right-click menus, the Properties panel, the drawing preferences), by group
    QString error;
    QList<QJsonObject> written;
    for (const QJsonValue& v : loadRecipes(error, false)) {
        const QJsonObject recipe = v.toObject();
        const QString id = recipe.value("id").toString();
        if (LC_AIToolbar::isToolRecipe(id) && !id.startsWith("tool:") && !listed.contains(id)) {
            written << recipe;
        }
    }
    std::stable_sort(written.begin(), written.end(), [](const QJsonObject& a, const QJsonObject& b) {
        QString ga, gb, la, lb;
        describeWrittenRecipe(a, ga, la);
        describeWrittenRecipe(b, gb, lb);
        return ga < gb;
    });
    for (const QJsonObject& recipe : std::as_const(written)) {
        QString group, label;
        describeWrittenRecipe(recipe, group, label);
        QStringList tools;
        for (const QJsonValue& c : recipe.value("then").toArray()) {
            const QString name = c.toObject().value("tool").toString();
            if (!tools.contains(name)) {
                tools << name;
            }
        }
        const QString id = recipe.value("id").toString();
        out.append(QJsonObject{{"id", id}, {"group", group}, {"label", label}, {"kind", QStringLiteral("check")},
                               {"how", tools.join(", ")}, {"note", recipe.value("note").toString()},
                               {"task", QObject::tr("Using the %1: %2 (%3): ").arg(group, label, tools.join(", "))},
                               {"why", QString()}});
    }
    return out;
}

QList<LC_AITool> LC_AIToolbar::tools() {
    return {listButtons(), pressButton(), clickDrawing(), selectEntities(), toolOptions(), handleDialog(), setToolbarValue(),
            usePanel(),    useMenu(),     contextMenu(),  lookAtApp(),      pressInApp(),  testButtons()};
}

QStringList LC_AIToolbar::recipeIds() {
    QString error;
    QStringList ids;
    for (const QJsonValue& r : loadRecipes(error)) {
        ids << r.toObject().value("id").toString();
    }
    ids.removeDuplicates();
    return ids;
}

void LC_AIToolbar::runTests(const QStringList& ids, std::function<void(int, int, const LC_AIButtonTest&)> progress,
                            std::function<void(const QList<LC_AIButtonTest>&)> finished) {
    if (g_testsRunning) {
        return;
    }
    QString error;
    QJsonArray chosen;
    for (const QJsonValue& r : loadRecipes(error)) {
        if (ids.isEmpty() || ids.contains(r.toObject().value("id").toString())) {
            chosen.append(r);
        }
    }
    if (chosen.isEmpty()) {
        if (finished) {
            finished({});
        }
        return;
    }
    runRecipes(chosen, LC_AIToolbar::tools() + LC_AITools::all(), std::move(finished), std::move(progress));
}

bool LC_AIToolbar::testsRunning() {
    return g_testsRunning;
}

void LC_AIToolbar::stopTests() {
    if (g_testsRunning) {
        g_stopTests = true;
    }
}

QHash<QString, LC_AIButtonTest> LC_AIToolbar::savedResults() {
    QHash<QString, LC_AIButtonTest> out;
    // first the results of the full test this build shipped with, then this computer's own runs on top
    QFile baseline(QStringLiteral(":/ai/toolbar_test_report.json"));
    if (baseline.open(QIODevice::ReadOnly)) {
        const QJsonObject report = QJsonDocument::fromJson(baseline.readAll()).object();
        const QString when = report.value("when").toString() + " (build test)";
        for (const QJsonValue& v : report.value("results").toArray()) {
            const QJsonObject o = v.toObject();
            out.insert(o.value("id").toString(), {o.value("id").toString(), o.value("status").toString(), o.value("detail").toString(), when});
        }
    }
    QFile file(savedResultsPath());
    if (!file.open(QIODevice::ReadOnly)) {
        return out;
    }
    const QJsonObject all = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = all.begin(); it != all.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        out.insert(it.key(), {it.key(), o.value("status").toString(), o.value("detail").toString(), o.value("when").toString()});
    }
    return out;
}

bool LC_AIToolbar::isToolRecipe(const QString& id) {
    static const QStringList prefixes = {"tool:", "properties:", "menu:", "panel:", "context:", "status:", "prefs:", "shortcut:"};
    for (const QString& p : prefixes) {
        if (id.startsWith(p)) {
            return true;
        }
    }
    return false;
}

QString LC_AIToolbar::notAgenticReason(const QString& id) {
    if (id == "FileQuit") {
        return QStringLiteral("the AI never quits OpenCAD");
    }
    if (id == "AIAssistantToggle") {
        return QStringLiteral("the AI's own sidebar button");
    }
    Button b;
    if (findButton(id, b) && b.action && !b.action->isVisible()) {
        return QStringLiteral("hidden and disabled by OpenCAD itself");
    }
    return {};
}
