/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aiproperties.h"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QJsonArray>
#include <QLabel>
#include <QMap>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
#include <cmath>
#include <optional>

#include "lc_aitoolbar.h"
#include "lc_convert.h"
#include "lc_entity_property_containerprovider.h"
#include "lc_enum_descriptor.h"
#include "lc_peninforegistry.h"
#include "lc_properties_sheet.h"
#include "lc_properties_sheet_model.h"
#include "lc_property_action.h"
#include "lc_property_action_link_view.h"
#include "lc_property_bool.h"
#include "lc_property_bool_combobox_view.h"
#include "lc_property_container.h"
#include "lc_property_double.h"
#include "lc_property_double_interactivepick_view.h"
#include "lc_property_enum.h"
#include "lc_property_int.h"
#include "lc_property_layer.h"
#include "lc_property_linetype.h"
#include "lc_property_linetype_combobox_view.h"
#include "lc_property_linewidth.h"
#include "lc_property_linewidth_combobox_view.h"
#include "lc_property_multi.h"
#include "lc_property_qstring.h"
#include "lc_property_qstring_file_view.h"
#include "lc_property_qstring_font_combobox_view.h"
#include "lc_property_qstring_list_combobox_view.h"
#include "lc_property_rect.h"
#include "lc_property_rscolor.h"
#include "lc_property_rscolor_combobox_view.h"
#include "lc_property_rsvector.h"
#include "lc_property_view.h"
#include "lc_propertysheetwidget.h"
#include "qc_applicationwindow.h"
#include "qg_colorbox.h"
#include "qg_commandwidget.h"
#include "qg_fontbox.h"
#include "qg_linetypebox.h"
#include "qg_widthbox.h"
#include "rs_actioninterface.h"
#include "rs_entity.h"
#include "rs_graphic.h"
#include "rs_graphicview.h"
#include "rs_layer.h"
#include "rs_layerlist.h"
#include "rs_math.h"

namespace {
    using Done = std::function<void(const LC_AIToolResult&)>;
    using Item = LC_PropertiesSheetModel::PropertyItem;
    using InputType = LC_ActionContext::InteractiveInputInfo::InputType;

    constexpr int EDIT_SETTLE_MS = 220;  // the panel applies an edit 30 ms after it, then rebuilds itself
    constexpr int CLICK_SETTLE_MS = 400; // links run 10 ms after the click; dialogs open, tools start
    constexpr int MAX_INLINE_OPTIONS = 12; // more choices than this are listed when one row is read
    constexpr int MAX_REPORTED = 12;
    constexpr int MAX_CHANGED_ROWS = 24;

    const QString NO_DRAWING = QStringLiteral("No drawing is open in OpenCAD. Ask the user to open or create one.");

    // ------------------------------------------------------------------ schema helpers

    QJsonObject str(const QString& description) {
        return {{"type", "string"}, {"description", description}};
    }
    QJsonObject schema(const QJsonObject& properties) {
        return {{"type", "object"}, {"properties", properties}};
    }

    QString clean(QString text) {
        text.remove('&');
        return text.simplified();
    }

    QString norm(const QString& text) {
        QString t = clean(text).toLower();
        while (t.endsWith(':')) {
            t.chop(1);
        }
        return t.trimmed();
    }

    QString num(const double v) {
        if (!std::isfinite(v)) {
            return QStringLiteral("n/a");
        }
        QString s = QString::number(v, 'f', 6);
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

    QString quoted(QString text) {
        text.replace('\n', QStringLiteral("\\n"));
        if (text.size() > 200) {
            text = text.left(200) + "...";
        }
        return "\"" + text + "\"";
    }

    /** A value the AI gave (string, number, boolean) as the text a person would type. */
    QString typed(const QJsonValue& v) {
        if (v.isBool()) {
            return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        }
        if (v.isDouble()) {
            return QString::number(v.toDouble(), 'g', 15);
        }
        if (v.isArray()) {
            QStringList parts;
            for (const QJsonValue& x : v.toArray()) {
                parts << typed(x);
            }
            return parts.join(',');
        }
        if (v.isObject()) {
            const QJsonObject o = v.toObject();
            if (o.contains("x") && o.contains("y")) {
                return typed(o.value("x")) + "," + typed(o.value("y"));
            }
        }
        return v.toString();
    }

    // ------------------------------------------------------------------ the panel

    QC_ApplicationWindow* appWindow() {
        auto& app = QC_ApplicationWindow::getAppWindow();
        return app ? app.get() : nullptr;
    }

    struct Panel {
        QPointer<LC_PropertySheetWidget> widget;
        QPointer<LC_PropertiesSheet> sheet;
        QString note;  // what was done to get at it (opened the dock)
        QString error;
    };

    /** The Properties panel, shown and filled as a person opens it (a hidden panel builds no rows). */
    Panel panel() {
        Panel p;
        QC_ApplicationWindow* app = appWindow();
        if (app == nullptr) {
            p.error = QStringLiteral("OpenCAD's main window is not available.");
            return p;
        }
        if (LC_AITools::view() == nullptr) {
            p.error = NO_DRAWING;
            return p;
        }
        p.widget = app->getPropertySheetWidget();
        if (!p.widget) {
            p.error = QStringLiteral("OpenCAD has no Properties panel.");
            return p;
        }
        if (!p.widget->isVisible()) {
            if (auto* dock = app->findChild<QDockWidget*>(QStringLiteral("property_sheet"))) {
                app->requestDockVisible(dock);
                if (!dock->isVisible()) {
                    dock->show();
                }
                dock->raise();
                for (int i = 0; i < 4 && !p.widget->isVisible(); ++i) {
                    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
                }
                p.note = QStringLiteral("Opened the Properties panel (it was hidden).");
            }
        }
        if (!p.widget->isVisible()) {
            p.error = QStringLiteral("The Properties panel could not be shown.");
            return p;
        }
        p.sheet = p.widget->findChild<LC_PropertiesSheet*>();
        if (!p.sheet) {
            p.error = QStringLiteral("The Properties panel has no property sheet.");
            return p;
        }
        if (p.sheet->getPropertyContainer() == nullptr) {
            p.widget->refill();
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        }
        if (p.sheet->getPropertyContainer() == nullptr || p.sheet->getModel()->getRootItem() == nullptr) {
            p.error = QStringLiteral("The Properties panel is empty (no drawing is attached to it).");
        }
        return p;
    }

    QComboBox* typeList(const Panel& p) {
        return p.widget ? p.widget->findChild<QComboBox*>(QStringLiteral("cbSelection")) : nullptr;
    }

    /** The tool options header shows instead of the selection header while a tool shows its options there. */
    QString toolOptionsOf(const Panel& p) {
        if (!p.widget) {
            return {};
        }
        auto* header = p.widget->findChild<QWidget*>(QStringLiteral("headerAction"));
        if (header == nullptr || header->isHidden()) {
            return {};
        }
        auto* name = p.widget->findChild<QLabel*>(QStringLiteral("lblActionName"));
        const QString text = name != nullptr ? clean(name->text()) : QString();
        return text.isEmpty() ? QStringLiteral("the running tool") : text;
    }

    QString runningTool() {
        RS_GraphicView* view = LC_AITools::view();
        if (view == nullptr || !view->hasAction()) {
            return {};
        }
        RS_ActionInterface* action = view->getCurrentAction();
        if (action == nullptr || action == view->getDefaultAction()) {
            return {};
        }
        const QString name = clean(view->getCurrentActionName());
        return name.isEmpty() ? QStringLiteral("a tool") : name;
    }

    QString prompt() {
        QC_ApplicationWindow* app = appWindow();
        auto* cmd = app != nullptr ? app->findChild<QG_CommandWidget*>() : nullptr;
        return cmd != nullptr ? cmd->lCommand->text() : QString();
    }

    // ------------------------------------------------------------------ dialogs

    bool isAIWidget(const QWidget* w) {
        return QString(w->metaObject()->className()).startsWith("LC_AI");
    }

    QSet<QWidget*> visibleDialogs() {
        QSet<QWidget*> set;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (w->isVisible() && (qobject_cast<QDialog*>(w) != nullptr || w->isModal()) && !isAIWidget(w)) {
                set.insert(w);
            }
        }
        return set;
    }

    QWidget* currentDialog() {
        if (QWidget* modal = QApplication::activeModalWidget(); modal != nullptr && !isAIWidget(modal)) {
            return modal;
        }
        QWidget* last = nullptr;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (w->isVisible() && qobject_cast<QDialog*>(w) != nullptr && !isAIWidget(w)) {
                last = w;
            }
        }
        return last;
    }

    /** The dialog as handle_dialog describes it (fields, buttons), so the AI can go on with handle_dialog. */
    QString describeDialog(QWidget* dialog) {
        QString text;
        for (const LC_AITool& t : LC_AIToolbar::tools()) {
            if (t.name == "handle_dialog" && t.executeAsync) {
                t.executeAsync(QJsonObject{}, [&text](const LC_AIToolResult& r) {
                    if (!r.isError) {
                        text = r.joinedText();
                    }
                });
            }
        }
        if (text.isEmpty()) {
            text = QStringLiteral("Dialog \"%1\" (%2%3) is open.").arg(clean(dialog->windowTitle()), dialog->metaObject()->className(),
                                                                       dialog->isModal() ? ", modal" : "");
        }
        return text;
    }

    // ------------------------------------------------------------------ rows

    struct Row {
        QString id;          // index path in the panel's tree: p1, p1.2, p1.2.0
        QStringList path;    // display names, section first
        QString name;        // the property's own name
        Item* item {nullptr};
        int depth {0};       // 0: a section
        bool inCollapsed {false}; // hidden in the panel: a section or row above it is collapsed
        bool underContainer {true}; // false: a part of a row (X of a point)
    };

    /** A property's own name: OpenCAD stores "<parent>$<name>" (and "<point>#x" for the parts of a point). */
    QString ownName(const LC_Property* p) {
        QString name = p->getName();
        name = name.section('$', -1);
        return name.section('#', -1);
    }

    QString displayName(LC_Property* p) {
        QString n = clean(p->getDisplayName());
        if (n.isEmpty()) {
            n = clean(p->getDescription()); // links have a description, no name
        }
        if (n.isEmpty()) {
            n = ownName(p);
        }
        return n;
    }

    void collect(Item* item, const QString& id, const QStringList& path, const int depth, const bool collapsed, QList<Row>& rows) {
        for (size_t i = 0; i < item->children.size(); ++i) {
            Item* child = item->children[i].get();
            if (child == nullptr || child->property == nullptr || !child->property->isVisible()) {
                continue;
            }
            Row r;
            r.id = id.isEmpty() ? QStringLiteral("p%1").arg(i) : QStringLiteral("%1.%2").arg(id).arg(i);
            r.path = path + QStringList{displayName(child->property)};
            r.name = ownName(child->property);
            r.item = child;
            r.depth = depth;
            r.inCollapsed = collapsed;
            r.underContainer = item->property == nullptr || item->property->isContainer();
            rows << r;
            collect(child, r.id, r.path, depth + 1, collapsed || child->property->isCollapsed(), rows);
        }
    }

    QList<Row> allRows(const Panel& p) {
        QList<Row> rows;
        if (p.sheet && p.sheet->getModel()->getRootItem() != nullptr) {
            collect(p.sheet->getModel()->getRootItem(), {}, {}, 0, false, rows);
        }
        return rows;
    }

    /** A row by id (p2.1), path ("Geometry / Radius", "Start / X") or name ("Radius", "radius"); -1 and why if none. */
    int findRow(const QList<Row>& rows, const QString& wanted, QString& error) {
        const QString w = wanted.trimmed();
        if (w.isEmpty()) {
            error = QStringLiteral("Say which row: its id (p2.1), its path (\"Geometry / Radius\") or its name (\"Radius\").");
            return -1;
        }
        static const QRegularExpression idPattern(QStringLiteral("^p\\d+(\\.\\d+)*$"), QRegularExpression::CaseInsensitiveOption);
        if (idPattern.match(w).hasMatch()) {
            for (int i = 0; i < rows.size(); ++i) {
                if (rows[i].id.compare(w, Qt::CaseInsensitive) == 0) {
                    return i;
                }
            }
            error = QStringLiteral("No row %1 in the panel now (ids change with what the panel shows; read it again).").arg(w);
            return -1;
        }
        QStringList parts;
        for (const QString& part : w.split('/', Qt::SkipEmptyParts)) {
            parts << norm(part);
        }
        QList<int> exact;
        QList<int> suffix;
        QList<int> byName;
        for (int i = 0; i < rows.size(); ++i) {
            QStringList path;
            for (const QString& part : rows[i].path) {
                path << norm(part);
            }
            if (path == parts) {
                exact << i;
            }
            else if (parts.size() < path.size() && path.mid(path.size() - parts.size()) == parts) {
                suffix << i;
            }
            if (parts.size() == 1 && norm(rows[i].name) == parts.first()) {
                byName << i;
            }
        }
        for (const QList<int>* found : {&exact, &suffix, &byName}) {
            if (found->size() == 1) {
                return found->first();
            }
            if (found->size() > 1) {
                QStringList names;
                for (int i : *found) {
                    names << QStringLiteral("%1 \"%2\"").arg(rows[i].id, rows[i].path.join(" / "));
                }
                error = QStringLiteral("\"%1\" matches several rows: %2. Give the id or the whole path.").arg(w, names.mid(0, 10).join(", "));
                return -1;
            }
        }
        QStringList names;
        for (const Row& r : rows) {
            if (r.underContainer && !r.inCollapsed) {
                names << r.path.join(" / ");
            }
        }
        error = QStringLiteral("No row \"%1\" in the panel. Rows: %2").arg(w, names.mid(0, 40).join("; "));
        return -1;
    }

    // ------------------------------------------------------------------ values

    LC_PropertyAtomic* firstAtomic(LC_Property* p) {
        if (auto* multi = qobject_cast<LC_PropertyMulti*>(p)) {
            return multi->getProperties().empty() ? nullptr : multi->getFirstProperty();
        }
        return p->asAtomic();
    }

    template <typename T>
    T attribute(const LC_Property* p, const QByteArray& name, const T& fallback) {
        const LC_PropertyViewDescriptor* d = p != nullptr ? p->getViewDescriptor() : nullptr;
        return d != nullptr ? d->attr<T>(name, fallback) : fallback;
    }

    QByteArray viewNameOf(const LC_Property* p) {
        const LC_PropertyViewDescriptor* d = p != nullptr ? p->getViewDescriptor() : nullptr;
        return d != nullptr ? d->viewName : QByteArray();
    }

    QString enumText(const LC_EnumDescriptor* d, const LC_PropertyEnumValueType value) {
        const LC_EnumValueDescriptor* v = d != nullptr ? d->findByValue(value) : nullptr;
        if (v == nullptr) {
            return QString::number(value);
        }
        return v->getDisplayName().isEmpty() ? v->getName() : v->getDisplayName();
    }

    QString colorText(const RS_Color& c) {
        const QString name = LC_PenInfoRegistry::instance()->getColorName(c, LC_PenInfoRegistry::NATURAL);
        if (c.isByLayer() || c.isByBlock() || name.startsWith('#')) {
            return name;
        }
        return QStringLiteral("%1 (%2)").arg(name, c.toQColor().name());
    }

    /** One property's value as the AI reads it: numbers in full, angles in degrees, points as x, y (UCS). */
    QString atomicValue(LC_PropertyAtomic* a) {
        if (a == nullptr) {
            return {};
        }
        if (auto* p = qobject_cast<LC_PropertyRSVector*>(a)) {
            const RS_Vector v = p->value();
            return num(v.x) + ", " + num(v.y);
        }
        if (auto* p = qobject_cast<LC_PropertyRect*>(a)) {
            const LC_MarginsRect r = p->value();
            return QStringLiteral("%1, %2, %3, %4").arg(num(r.left), num(r.top), num(r.right), num(r.bottom));
        }
        if (auto* p = qobject_cast<LC_PropertyDouble*>(a)) {
            const double v = p->value();
            return p->getInteractiveInputType() == LC_ActionContext::InteractiveInputInfo::ANGLE ? num(RS_Math::rad2deg(v)) + "°" : num(v);
        }
        if (auto* p = qobject_cast<LC_PropertyInt*>(a)) {
            return QString::number(p->value());
        }
        if (auto* p = qobject_cast<LC_PropertyBool*>(a)) {
            return p->value() ? QStringLiteral("true") : QStringLiteral("false");
        }
        if (auto* p = qobject_cast<LC_PropertyEnum*>(a)) {
            return enumText(p->getEnumDescriptor(), p->value());
        }
        if (auto* p = qobject_cast<LC_PropertyLayer*>(a)) {
            const RS_Layer* layer = p->value();
            return layer != nullptr ? layer->getName() : QStringLiteral("(none)");
        }
        if (auto* p = qobject_cast<LC_PropertyRSColor*>(a)) {
            return colorText(p->value());
        }
        if (auto* p = qobject_cast<LC_PropertyLineType*>(a)) {
            return LC_PenInfoRegistry::instance()->getLineTypeText(p->value());
        }
        if (auto* p = qobject_cast<LC_PropertyLineWidth*>(a)) {
            return LC_PenInfoRegistry::instance()->getLineWidthText(p->value());
        }
        if (auto* p = qobject_cast<LC_PropertyQString*>(a)) {
            return quoted(p->value());
        }
        return {};
    }

    QString kindOf(LC_PropertyAtomic* a) {
        if (qobject_cast<LC_PropertyAction*>(a) != nullptr) {
            return QStringLiteral("links");
        }
        if (qobject_cast<LC_PropertyRSVector*>(a) != nullptr) {
            return QStringLiteral("point x,y");
        }
        if (qobject_cast<LC_PropertyRect*>(a) != nullptr) {
            return QStringLiteral("margins left,top,right,bottom");
        }
        if (auto* p = qobject_cast<LC_PropertyDouble*>(a)) {
            switch (p->getInteractiveInputType()) {
                case LC_ActionContext::InteractiveInputInfo::DISTANCE:
                    return QStringLiteral("distance");
                case LC_ActionContext::InteractiveInputInfo::ANGLE:
                    return QStringLiteral("angle in degrees");
                case LC_ActionContext::InteractiveInputInfo::POINT_X:
                    return QStringLiteral("x");
                case LC_ActionContext::InteractiveInputInfo::POINT_Y:
                    return QStringLiteral("y");
                default:
                    return QStringLiteral("number");
            }
        }
        if (qobject_cast<LC_PropertyInt*>(a) != nullptr) {
            return QStringLiteral("whole number");
        }
        if (qobject_cast<LC_PropertyBool*>(a) != nullptr) {
            return QStringLiteral("true/false");
        }
        if (qobject_cast<LC_PropertyEnum*>(a) != nullptr) {
            return QStringLiteral("choice");
        }
        if (qobject_cast<LC_PropertyLayer*>(a) != nullptr) {
            return QStringLiteral("layer");
        }
        if (qobject_cast<LC_PropertyRSColor*>(a) != nullptr) {
            return QStringLiteral("colour");
        }
        if (qobject_cast<LC_PropertyLineType*>(a) != nullptr) {
            return QStringLiteral("line type");
        }
        if (qobject_cast<LC_PropertyLineWidth*>(a) != nullptr) {
            return QStringLiteral("line width");
        }
        if (qobject_cast<LC_PropertyQString*>(a) != nullptr) {
            const QByteArray view = viewNameOf(a);
            if (view == LC_PropertyQStringListComboBoxView::VIEW_NAME) {
                return QStringLiteral("choice");
            }
            if (view == LC_PropertyQStringFontComboboxView::VIEW_NAME) {
                return QStringLiteral("font");
            }
            if (view == LC_PropertyQStringFileView::VIEW_NAME) {
                return QStringLiteral("file path");
            }
            return QStringLiteral("text");
        }
        return clean(a->metaObject()->className());
    }

    using PenItems = QList<QPair<QString, int>>;

    /** The texts and values of a pen list as OpenCAD's own list box shows them (By Layer and By Block first). */
    template <typename Box>
    PenItems boxItems() {
        Box box(true, false, nullptr);
        PenItems items;
        for (int i = 0; i < box.count(); ++i) {
            items.append({box.itemText(i), box.itemData(i).toInt()});
        }
        return items;
    }

    PenItems lineTypeItems(const bool showByLayer) {
        static const PenItems all = boxItems<QG_LineTypeBox>();
        PenItems items;
        for (const auto& item : all) {
            if (showByLayer || (item.second != RS2::LineByLayer && item.second != RS2::LineByBlock)) {
                items << item;
            }
        }
        return items;
    }

    PenItems lineWidthItems(const bool showByLayer) {
        static const PenItems all = boxItems<QG_WidthBox>();
        PenItems items;
        for (const auto& item : all) {
            if (showByLayer || (item.second != RS2::WidthByLayer && item.second != RS2::WidthByBlock)) {
                items << item;
            }
        }
        return items;
    }

    /** The named colours of OpenCAD's colour list (Red, Dark Red, ..., Black / White, Gray...). */
    const QList<QPair<QString, QColor>>& colorItems() {
        static const QList<QPair<QString, QColor>> items = [] {
            QG_ColorBox box(true, false, nullptr);
            QList<QPair<QString, QColor>> list;
            for (int i = 0; i < box.count(); ++i) {
                const QVariant data = box.itemData(i);
                if (data.canConvert<QColor>() && data.value<QColor>().isValid() && !list.contains({box.itemText(i), data.value<QColor>()})) {
                    list.append({box.itemText(i), data.value<QColor>()});
                }
            }
            return list;
        }();
        return items;
    }

    /** The fonts OpenCAD's font list offers for text. */
    const QStringList& fontNames() {
        static const QStringList names = [] {
            QG_FontBox box(nullptr);
            box.init();
            QStringList list;
            for (int i = 0; i < box.count(); ++i) {
                list << box.itemText(i);
            }
            return list;
        }();
        return names;
    }

    QStringList optionsOf(LC_PropertyAtomic* a) {
        QStringList options;
        if (auto* p = qobject_cast<LC_PropertyEnum*>(a)) {
            if (const LC_EnumDescriptor* d = p->getEnumDescriptor()) {
                d->doProcessEachEnumValue([&options](const LC_EnumValueDescriptor& v) {
                    if (!(v.getState() & EnumValueStateHidden)) {
                        options << (v.getDisplayName().isEmpty() ? v.getName() : v.getDisplayName());
                    }
                    return true;
                });
            }
        }
        else if (auto* p = qobject_cast<LC_PropertyLayer*>(a)) {
            if (RS_LayerList* list = p->layerList()) {
                for (RS_Layer* layer : *list) {
                    options << layer->getName();
                }
            }
        }
        else if (qobject_cast<LC_PropertyLineType*>(a) != nullptr) {
            for (const auto& [text, value] : lineTypeItems(attribute(a, LC_PropertyLineTypeComboboxView::ATTR_SHOW_BY_LAYER, true))) {
                options << text;
            }
        }
        else if (qobject_cast<LC_PropertyLineWidth*>(a) != nullptr) {
            for (const auto& [text, value] : lineWidthItems(attribute(a, LC_PropertyLineWidthComboboxView::ATTR_SHOW_BY_LAYER, true))) {
                options << text;
            }
        }
        else if (qobject_cast<LC_PropertyRSColor*>(a) != nullptr) {
            if (attribute(a, LC_PropertyRSColorComboBoxView::ATTR_SHOW_BY_LAYER, true)) {
                options << QObject::tr("By Layer") << QObject::tr("By Block");
            }
            for (const auto& [text, color] : colorItems()) {
                if (!options.contains(text)) {
                    options << text;
                }
            }
            options << QStringLiteral("#rrggbb");
        }
        else if (qobject_cast<LC_PropertyQString*>(a) != nullptr && viewNameOf(a) == LC_PropertyQStringListComboBoxView::VIEW_NAME) {
            options = attribute(a, LC_PropertyQStringListComboBoxView::ATTR_ITEMS, QStringList());
        }
        else if (qobject_cast<LC_PropertyQString*>(a) != nullptr && viewNameOf(a) == LC_PropertyQStringFontComboboxView::VIEW_NAME) {
            options = fontNames();
        }
        return options;
    }

    /** The "Visible layer" row of the Active Layer section reports isFrozen(): the opposite of its name. */
    bool visibleLayerRow(LC_Property* prop, bool& visible, bool& inverted) {
        if (ownName(prop) != QLatin1String("layerVisible")) {
            return false;
        }
        auto* b = qobject_cast<LC_PropertyBool*>(firstAtomic(prop));
        RS_GraphicView* view = LC_AITools::view();
        RS_Graphic* graphic = view != nullptr ? view->getGraphic() : nullptr;
        const RS_Layer* layer = graphic != nullptr ? graphic->getActiveLayer() : nullptr;
        if (b == nullptr || layer == nullptr) {
            return false;
        }
        visible = !layer->isFrozen();
        inverted = b->value() != visible;
        return true;
    }

    struct Info {
        QString kind;
        QString value;
        QStringList options;
        QStringList links;   // link rows: their titles
        bool editable {false};
        bool pickable {false};
        bool link {false};
        QString note;
    };

    bool canPick(LC_PropertyAtomic* a) {
        if (auto* d = qobject_cast<LC_PropertyDouble*>(a)) {
            return d->getInteractiveInputType() != LC_ActionContext::InteractiveInputInfo::NOTNEEDED && d->getFormatter() != nullptr;
        }
        if (auto* v = qobject_cast<LC_PropertyRSVector*>(a)) {
            return v->getInteractiveInputType() != LC_ActionContext::InteractiveInputInfo::NOTNEEDED && v->getActionContext() != nullptr
                && v->getInteractiveInputRequestor() != nullptr;
        }
        return false;
    }

    QStringList linkTitles(LC_PropertyAction* a, QList<bool>* enabled = nullptr) {
        QStringList titles;
        titles << clean(attribute(a, LC_PropertyActionLinkView::ATTR_TITLE, QString()));
        titles << clean(attribute(a, LC_PropertyActionLinkView::ATTR_TITLE_RIGHT, QString()));
        if (enabled != nullptr) {
            *enabled = {attribute(a, LC_PropertyActionLinkView::ATTR_ENABLED_LEFT, true),
                        attribute(a, LC_PropertyActionLinkView::ATTR_ENABLED_RIGHT, true)};
        }
        return titles;
    }

    Info describe(LC_Property* prop, const bool withOptions = true) {
        Info info;
        LC_PropertyAtomic* a = firstAtomic(prop);
        if (a == nullptr) {
            return info;
        }
        info.kind = kindOf(a);
        info.editable = prop->isEditableByUser();
        if (auto* action = qobject_cast<LC_PropertyAction*>(a)) {
            info.link = true;
            QList<bool> enabled;
            const QStringList titles = linkTitles(action, &enabled);
            for (int i = 0; i < titles.size(); ++i) {
                if (!titles[i].isEmpty()) {
                    info.links << QStringLiteral("\"%1\"%2").arg(titles[i], enabled.value(i, true) ? "" : " (disabled)");
                }
            }
            return info;
        }
        auto* multi = qobject_cast<LC_PropertyMulti*>(prop);
        if (multi != nullptr && multi->isMultiValue()) {
            QStringList distinct;
            for (LC_PropertyAtomic* p : multi->getProperties()) {
                const QString t = atomicValue(p);
                if (!distinct.contains(t)) {
                    distinct << t;
                }
            }
            info.value = QStringLiteral("(VARIES: %1%2)").arg(distinct.mid(0, 5).join(" | "), distinct.size() > 5 ? " | ..." : "");
        }
        else {
            info.value = atomicValue(a);
        }
        bool visible = false;
        bool inverted = false;
        if (visibleLayerRow(prop, visible, inverted)) {
            info.value = visible ? QStringLiteral("true") : QStringLiteral("false");
            if (inverted) {
                info.note = QStringLiteral("(the real visibility; the panel's check box for this row shows the opposite)");
            }
        }
        if (withOptions) {
            info.options = optionsOf(a);
        }
        info.pickable = info.editable && canPick(a);
        return info;
    }

    int countRowsUnder(const QList<Row>& rows, const int index) {
        int n = 0;
        for (int i = index + 1; i < rows.size() && rows[i].depth > rows[index].depth; ++i) {
            n += rows[i].underContainer && !rows[i].item->property->isContainer() ? 1 : 0;
        }
        return n;
    }

    QString rowLine(const QList<Row>& rows, const int index, const bool detail, const bool withPath) {
        const Row& r = rows[index];
        LC_Property* prop = r.item->property;
        const QString indent(detail ? 0 : 2 * r.depth, ' ');
        const QString name = withPath ? r.path.join(" / ") : r.path.last();
        if (prop->isContainer()) {
            const bool collapsed = prop->isCollapsed();
            if (!collapsed || detail) {
                return QStringLiteral("%1%2 %3%4:").arg(indent, r.id, name, collapsed ? QStringLiteral(" (collapsed in the panel)") : QString());
            }
            return QStringLiteral("%1%2 %3 (collapsed: %4 rows hidden; read it with row \"%3\" or expand it)")
                .arg(indent, r.id, name).arg(countRowsUnder(rows, index));
        }
        const Info info = describe(prop);
        if (info.link) {
            return QStringLiteral("%1%2 %3 [links] %4").arg(indent, r.id, name, info.links.join(" | "));
        }
        QString line = QStringLiteral("%1%2 %3 [%4%5] = %6").arg(indent, r.id, name, info.kind, info.editable ? QString() : ", read-only",
                                                                info.value);
        if (!info.note.isEmpty()) {
            line += " " + info.note;
        }
        if (info.editable && !info.options.isEmpty()) {
            if (detail || info.options.size() <= MAX_INLINE_OPTIONS) {
                line += " options: " + info.options.join(" | ");
            }
            else {
                line += QStringLiteral(" (%1 options: read this row to list them)").arg(info.options.size());
            }
        }
        if (detail && info.pickable) {
            line += " (pickable from the drawing)";
        }
        if (detail && r.item->property->isCollapsed() && !r.item->children.empty()) {
            line += " (collapsed in the panel)";
        }
        return line;
    }

    QString modeText(const Panel& p) {
        const QString tool = toolOptionsOf(p);
        if (!tool.isEmpty()) {
            return QStringLiteral("it shows the options of the running tool \"%1\" (changing them changes how the tool works; "
                                  "not undoable)").arg(tool);
        }
        const LC_PropertyContainer* container = p.sheet ? p.sheet->getPropertyContainer() : nullptr;
        const int tag = container != nullptr ? container->getTag() : -1;
        QComboBox* types = typeList(p);
        const QString shown = types != nullptr ? clean(types->currentText()) : QString();
        if (tag == LC_EntityPropertyContainerProvider::TAG_CONTAINER_NO_SELECTION) {
            return QStringLiteral("nothing is selected, so it shows the drawing's settings (pen for new entities, active layer, "
                                  "grid, units, paper...); changes apply at once and are not undoable");
        }
        if (tag == LC_EntityPropertyContainerProvider::TAG_CONTAINER_SELECTION_ONE) {
            return QStringLiteral("it shows the selected entity (%1); each change is one undo step").arg(shown);
        }
        return QStringLiteral("it shows the selected entities: %1; a change applies to all of them as one undo step").arg(shown);
    }

    QStringList header(const Panel& p) {
        QStringList lines;
        if (!p.note.isEmpty()) {
            lines << p.note;
        }
        lines << QStringLiteral("Properties panel: %1.").arg(modeText(p));
        if (QComboBox* types = typeList(p); types != nullptr && types->count() > 0 && toolOptionsOf(p).isEmpty()) {
            QStringList items;
            for (int i = 0; i < types->count(); ++i) {
                items << (i == types->currentIndex() ? "[" + clean(types->itemText(i)) + "]" : clean(types->itemText(i)));
            }
            lines << "Selected types (choose_type): " + items.join(" | ");
        }
        if (const QString tool = runningTool(); !tool.isEmpty()) {
            lines << QStringLiteral("A tool is running (%1): the panel ignores selection changes until it ends "
                                    "(press_toolbar_button EditKillAllActions ends it).").arg(tool);
        }
        return lines;
    }

    /** The panel as text: every row, or one row or section in detail. */
    QString readPanel(const Panel& p, const QString& wanted, QString& error) {
        const QList<Row> rows = allRows(p);
        QStringList lines = header(p);
        if (!wanted.trimmed().isEmpty()) {
            const int index = findRow(rows, wanted, error);
            if (index < 0) {
                return {};
            }
            const Row& r = rows[index];
            lines << rowLine(rows, index, true, true);
            if (!r.item->property->getDescription().isEmpty() && !r.item->property->isContainer()) {
                lines << "  About: " + clean(r.item->property->getDescription());
            }
            for (int i = index + 1; i < rows.size() && rows[i].depth > r.depth; ++i) {
                lines << "  " + rowLine(rows, i, true, false);
            }
            return lines.join('\n');
        }
        if (rows.isEmpty()) {
            lines << "The panel has no rows.";
            return lines.join('\n');
        }
        lines << "Rows (id name [type] = value; set one with action set and row id or path):";
        for (int i = 0; i < rows.size(); ++i) {
            const Row& r = rows[i];
            if (!r.underContainer) {
                continue; // a part of a row (X and Y of a point): listed when the row is read
            }
            if (r.inCollapsed) {
                continue; // in a collapsed section: summarised there
            }
            lines << rowLine(rows, i, false, false);
        }
        lines << "A point row's X and Y are the rows <id>.0 and <id>.1 (path \"... / X\"); read a row to see its parts and all its "
                 "options.";
        return lines.join('\n');
    }

    // ------------------------------------------------------------------ before and after

    struct State {
        QHash<unsigned long long, QString> entities; // id -> description (without the selection mark)
        QHash<unsigned long long, RS_Entity*> pointers;
        int selected {0};
        QStringList layers;
        QString activeLayer;
        QSet<QWidget*> dialogs;
        QMap<QString, QString> rows; // path -> value
        QString mode;
        QString running;
    };

    QString withoutSelectionMark(QString d) {
        if (d.endsWith(QLatin1String(" [selected]"))) {
            d.chop(11);
        }
        return d;
    }

    State stateNow(const Panel& p) {
        State s;
        s.dialogs = visibleDialogs();
        s.running = runningTool();
        if (RS_GraphicView* view = LC_AITools::view(); view != nullptr && view->getGraphic() != nullptr) {
            RS_Graphic* graphic = view->getGraphic();
            for (RS_Entity* e : *graphic) {
                if (e == nullptr || e->isDeleted()) {
                    continue;
                }
                s.entities.insert(e->getId(), withoutSelectionMark(LC_AITools::describeEntity(e)));
                s.pointers.insert(e->getId(), e);
                s.selected += e->isSelected() ? 1 : 0;
            }
            if (RS_LayerList* layers = graphic->getLayerList()) {
                for (RS_Layer* l : *layers) {
                    s.layers << l->getName();
                }
                if (layers->getActive() != nullptr) {
                    s.activeLayer = layers->getActive()->getName();
                }
            }
        }
        if (p.sheet) {
            s.mode = modeText(p);
            for (const Row& r : allRows(p)) {
                if (r.item->property->isContainer() || !r.underContainer) {
                    continue; // sections, and the parts of a row (X of a point): the row itself is compared
                }
                const Info info = describe(r.item->property, false);
                if (info.link) {
                    continue;
                }
                QString key = r.path.join(" / ");
                for (int n = 2; s.rows.contains(key); ++n) {
                    key = r.path.join(" / ") + QStringLiteral(" #%1").arg(n);
                }
                s.rows.insert(key, info.value);
            }
        }
        return s;
    }

    struct Changes {
        QStringList lines;
        int entitiesChanged {0};
    };

    Changes compare(const State& before, const State& after) {
        Changes c;
        // the panel's rows
        if (after.mode != before.mode) {
            c.lines << QStringLiteral("The panel now: %1.").arg(after.mode);
        }
        else {
            QStringList changed;
            for (auto it = after.rows.cbegin(); it != after.rows.cend(); ++it) {
                const auto old = before.rows.constFind(it.key());
                if (old != before.rows.cend() && old.value() != it.value()) {
                    changed << QStringLiteral("%1: %2 -> %3").arg(it.key(), old.value(), it.value());
                }
            }
            if (!changed.isEmpty()) {
                c.lines << "Panel rows that changed:";
                for (int i = 0; i < changed.size() && i < MAX_CHANGED_ROWS; ++i) {
                    c.lines << "  " + changed[i];
                }
                if (changed.size() > MAX_CHANGED_ROWS) {
                    c.lines << QStringLiteral("  ... and %1 more").arg(changed.size() - MAX_CHANGED_ROWS);
                }
            }
        }
        // the drawing
        QList<unsigned long long> added;
        QList<unsigned long long> removed;
        QList<unsigned long long> modified;
        for (auto it = after.entities.cbegin(); it != after.entities.cend(); ++it) {
            const auto old = before.entities.constFind(it.key());
            if (old == before.entities.cend()) {
                added << it.key();
            }
            else if (old.value() != it.value()) {
                modified << it.key();
            }
        }
        for (auto it = before.entities.cbegin(); it != before.entities.cend(); ++it) {
            if (!after.entities.contains(it.key())) {
                removed << it.key();
            }
        }
        std::sort(added.begin(), added.end());
        std::sort(removed.begin(), removed.end());
        std::sort(modified.begin(), modified.end());
        const auto now = [&after](const unsigned long long id) {
            RS_Entity* e = after.pointers.value(id);
            return e != nullptr ? LC_AITools::describeEntity(e) : after.entities.value(id);
        };
        if (!added.isEmpty() && added.size() == removed.size()) {
            // an edit replaces each entity it changes with an edited copy
            c.entitiesChanged = added.size();
            c.lines << QStringLiteral("Drawing: changed %1 entit%2 (each edited entity is replaced by an edited copy with a new id):")
                           .arg(added.size()).arg(added.size() == 1 ? "y" : "ies");
            for (int i = 0; i < added.size() && i < MAX_REPORTED; ++i) {
                QString was = before.entities.value(removed[i]);
                was = was.section(' ', 1); // without the old id
                c.lines << QStringLiteral("  %1\n    was #%2 %3").arg(now(added[i])).arg(removed[i]).arg(was);
            }
            if (added.size() > MAX_REPORTED) {
                c.lines << QStringLiteral("  ... and %1 more (list_entities shows them)").arg(added.size() - MAX_REPORTED);
            }
        }
        else {
            if (!added.isEmpty()) {
                c.entitiesChanged += added.size();
                c.lines << QStringLiteral("Drawing: added %1 entit%2:").arg(added.size()).arg(added.size() == 1 ? "y" : "ies");
                for (int i = 0; i < added.size() && i < MAX_REPORTED; ++i) {
                    c.lines << "  " + now(added[i]);
                }
            }
            if (!removed.isEmpty()) {
                c.entitiesChanged += removed.size();
                c.lines << QStringLiteral("Drawing: removed %1 entit%2.").arg(removed.size()).arg(removed.size() == 1 ? "y" : "ies");
            }
        }
        if (!modified.isEmpty()) {
            c.entitiesChanged += modified.size();
            c.lines << QStringLiteral("Drawing: %1 entit%2 changed in place:").arg(modified.size()).arg(modified.size() == 1 ? "y" : "ies");
            for (int i = 0; i < modified.size() && i < MAX_REPORTED; ++i) {
                c.lines << "  " + now(modified[i]);
            }
        }
        if (after.selected != before.selected) {
            c.lines << QStringLiteral("Selection: %1 entities (was %2).").arg(after.selected).arg(before.selected);
        }
        for (const QString& l : after.layers) {
            if (!before.layers.contains(l)) {
                c.lines << "New layer: " + l;
            }
        }
        for (const QString& l : before.layers) {
            if (!after.layers.contains(l)) {
                c.lines << "Removed layer: " + l;
            }
        }
        if (after.activeLayer != before.activeLayer) {
            c.lines << QStringLiteral("Active layer: %1 (was %2).").arg(after.activeLayer, before.activeLayer);
        }
        return c;
    }

    /** What is going on after a click: a dialog to finish, a tool to answer. */
    QStringList situation(const State& before) {
        QStringList lines;
        QWidget* dialog = currentDialog();
        if (dialog != nullptr && !before.dialogs.contains(dialog)) {
            lines << describeDialog(dialog);
            lines << "Use handle_dialog to fill it in and press one of its buttons.";
        }
        else if (dialog != nullptr) {
            lines << QStringLiteral("A dialog is still open: \"%1\" (handle_dialog reads and closes it).").arg(clean(dialog->windowTitle()));
        }
        if (const QString tool = runningTool(); !tool.isEmpty()) {
            lines << QStringLiteral("Running tool: %1. Prompt: \"%2\". Answer with click_drawing (or run_command); Escape ends it.")
                         .arg(tool, prompt());
        }
        return lines;
    }

    LC_AIToolResult finish(QStringList lines, const bool capture, const bool error = false) {
        LC_AIToolResult result = LC_AIToolResult::text(lines.join('\n'));
        result.isError = error;
        if (capture) {
            if (RS_GraphicView* view = LC_AITools::view()) {
                result.content.prepend(LC_AITools::screenshot(view));
            }
        }
        return result;
    }

    // ------------------------------------------------------------------ parsing values the way the panel's editors do

    std::optional<bool> parseBool(const QString& text, const LC_Property* p) {
        const QString t = norm(text);
        const QString labelTrue = norm(attribute(p, LC_PropertyBoolComboBoxView::ATTR_LABEL_TRUE, QString()));
        const QString labelFalse = norm(attribute(p, LC_PropertyBoolComboBoxView::ATTR_LABEL_FALSE, QString()));
        if (QStringList{"true", "yes", "on", "1", "checked", "y"}.contains(t) || (!labelTrue.isEmpty() && t == labelTrue)) {
            return true;
        }
        if (QStringList{"false", "no", "off", "0", "unchecked", "n"}.contains(t) || (!labelFalse.isEmpty() && t == labelFalse)) {
            return false;
        }
        return std::nullopt;
    }

    bool parseNumber(QString text, double& v) {
        text = text.trimmed();
        text.remove(QChar(0xB0)); // a degree sign
        return LC_Convert::toDouble(text, v, 0.0, false);
    }

    /** "x,y", "(x, y)", "x y" -> numbers. */
    bool parseNumbers(QString text, const int count, QList<double>& out) {
        text = text.trimmed();
        text.remove('(').remove(')').remove('[').remove(']');
        static const QRegularExpression separators(QStringLiteral("\\s*[,;]\\s*|\\s+"));
        const QStringList parts = text.split(separators, Qt::SkipEmptyParts);
        if (parts.size() != count) {
            return false;
        }
        out.clear();
        for (const QString& part : parts) {
            double v = 0;
            if (!parseNumber(part, v)) {
                return false;
            }
            out << v;
        }
        return true;
    }

    std::optional<RS_Color> parseColor(const QString& text, const bool byLayerAllowed) {
        const QString t = norm(text);
        if (t == "by layer" || t == "bylayer") {
            return byLayerAllowed ? std::optional<RS_Color>(RS_Color(RS2::FlagByLayer)) : std::nullopt;
        }
        if (t == "by block" || t == "byblock") {
            return byLayerAllowed ? std::optional<RS_Color>(RS_Color(RS2::FlagByBlock)) : std::nullopt;
        }
        for (const auto& [name, color] : colorItems()) {
            if (norm(name) == t) {
                return RS_Color(color);
            }
        }
        static const QRegularExpression rgb(QStringLiteral("^rgb\\(\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)\\s*\\)$"));
        if (const auto m = rgb.match(t); m.hasMatch()) {
            return RS_Color(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt());
        }
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        const QColor c = QColor::fromString(text.trimmed()); // #rrggbb or an SVG colour name
#else
        const QColor c(text.trimmed());
#endif
        if (c.isValid()) {
            return RS_Color(c);
        }
        return std::nullopt;
    }

    /** The value of the item whose text is this (case and spaces do not matter); By Layer values are negative. */
    std::optional<int> matchItem(const PenItems& items, const QString& text) {
        const QString t = norm(text);
        for (const auto& [name, value] : items) {
            if (norm(name) == t) {
                return value;
            }
        }
        // "0.50mm (ISO)" as the panel shows a width, "0.50mmISO" as its list does
        static const QRegularExpression spacing(QStringLiteral("[\\s()]"));
        QString compact = t;
        compact.remove(spacing);
        for (const auto& [name, value] : items) {
            QString item = norm(name);
            item.remove(spacing);
            if (item == compact) {
                return value;
            }
        }
        return std::nullopt;
    }

    /**
     * Sets a row as its editor would: the value goes to the first entity's property with the reason an edit
     * gives, the row passes it on to the other selected entities, and the panel hears it (and applies the edit to
     * the drawing as one undo step 30 ms later). Returns the value as set, or an error.
     */
    bool applyValue(const Panel& p, const Row& row, const QJsonValue& value, QString& shown, QString& error) {
        LC_Property* prop = row.item->property;
        const QString path = row.path.join(" / ");
        if (prop->isContainer()) {
            error = QStringLiteral("\"%1\" is a section, not a value (expand or collapse it, or set one of its rows).").arg(path);
            return false;
        }
        auto* multi = qobject_cast<LC_PropertyMulti*>(prop);
        LC_PropertyAtomic* atomic = firstAtomic(prop);
        if (atomic == nullptr) {
            error = QStringLiteral("\"%1\" has no value to set.").arg(path);
            return false;
        }
        if (qobject_cast<LC_PropertyAction*>(atomic) != nullptr) {
            error = QStringLiteral("\"%1\" is a row of links; use action click_link.").arg(path);
            return false;
        }
        if (!prop->isEditableByUser()) {
            error = QStringLiteral("\"%1\" is read-only in the panel (a calculated or fixed value); change the rows it comes from "
                                   "instead.").arg(path);
            return false;
        }
        if (value.isNull() || value.isUndefined()) {
            error = QStringLiteral("Give the new value for \"%1\" in value.").arg(path);
            return false;
        }
        const QString text = typed(value);
        const LC_PropertyChangeReason reason = row.item->view ? row.item->view->changeReasonDueToEdit()
                                                              : LC_PropertyChangeReason(PropertyChangeReasonEdit);
        std::function<bool()> apply;
        const QStringList options = optionsOf(atomic);
        const auto notAnOption = [&](const QString& what) {
            error = QStringLiteral("\"%1\" is not %2 for \"%3\". Options: %4").arg(text, what, path, options.join(" | "));
        };

        if (auto* d = qobject_cast<LC_PropertyDouble*>(atomic)) {
            const bool positiveOnly = attribute(atomic, LC_PropertyDoubleInteractivePickView::ATTR_POSITIVIE_VALUES_ONLY, false);
            const double notMeaningful = attribute(atomic, LC_PropertyDoubleInteractivePickView::ATTR_NON_MEANINGFUL_DISTANCE, 0.0);
            QString t = text.trimmed();
            t.remove(QChar(0xB0));
            double v = 0;
            bool ok = false;
            switch (d->getInteractiveInputType()) {
                case LC_ActionContext::InteractiveInputInfo::ANGLE:
                    ok = LC_Convert::toDoubleAngleRad(t, v, 0.0, positiveOnly); // degrees typed, radians kept
                    break;
                case LC_ActionContext::InteractiveInputInfo::DISTANCE:
                    ok = LC_Convert::toDouble(t, v, notMeaningful, positiveOnly);
                    break;
                default:
                    ok = LC_Convert::toDouble(t, v, 0.0, positiveOnly);
                    break;
            }
            if (!ok) {
                error = QStringLiteral("\"%1\" is not a number for \"%2\"%3.").arg(text, path,
                    d->getInteractiveInputType() == LC_ActionContext::InteractiveInputInfo::ANGLE ? " (an angle in degrees)" : "");
                return false;
            }
            if (v < d->minValue() || v > d->maxValue()) {
                error = QStringLiteral("%1 is out of range for \"%2\" (%3 .. %4).").arg(num(v), path, num(d->minValue()), num(d->maxValue()));
                return false;
            }
            shown = d->getInteractiveInputType() == LC_ActionContext::InteractiveInputInfo::ANGLE ? num(RS_Math::rad2deg(v)) + "°" : num(v);
            apply = [d, v, reason] { return d->setValue(v, reason); };
        }
        else if (auto* vp = qobject_cast<LC_PropertyRSVector*>(atomic)) {
            QList<double> xy;
            if (!parseNumbers(text, 2, xy)) {
                error = QStringLiteral("Give a point as \"x,y\" for \"%1\" (got \"%2\").").arg(path, text);
                return false;
            }
            const RS_Vector v(xy[0], xy[1]);
            shown = num(v.x) + ", " + num(v.y);
            apply = [vp, v, reason] { return vp->setValue(v, reason); };
        }
        else if (auto* rp = qobject_cast<LC_PropertyRect*>(atomic)) {
            QList<double> m;
            if (!parseNumbers(text, 4, m)) {
                error = QStringLiteral("Give \"left,top,right,bottom\" for \"%1\" (got \"%2\").").arg(path, text);
                return false;
            }
            LC_MarginsRect r;
            r.left = m[0];
            r.top = m[1];
            r.right = m[2];
            r.bottom = m[3];
            shown = QStringLiteral("%1, %2, %3, %4").arg(num(r.left), num(r.top), num(r.right), num(r.bottom));
            apply = [rp, r, reason] { return rp->setValue(r, reason); };
        }
        else if (auto* ip = qobject_cast<LC_PropertyInt*>(atomic)) {
            double v = 0;
            if (!parseNumber(text, v) || std::abs(v - std::round(v)) > 1e-9) {
                error = QStringLiteral("\"%1\" is not a whole number for \"%2\".").arg(text, path);
                return false;
            }
            const int i = static_cast<int>(std::lround(v));
            if (i < ip->minValue() || i > ip->maxValue()) {
                error = QStringLiteral("%1 is out of range for \"%2\" (%3 .. %4).").arg(i).arg(path).arg(ip->minValue()).arg(ip->maxValue());
                return false;
            }
            shown = QString::number(i);
            apply = [ip, i, reason] { return ip->setValue(i, reason); };
        }
        else if (auto* bp = qobject_cast<LC_PropertyBool*>(atomic)) {
            const std::optional<bool> wanted = parseBool(text, atomic);
            if (!wanted.has_value()) {
                error = QStringLiteral("Give true or false for \"%1\" (got \"%2\").").arg(path, text);
                return false;
            }
            shown = *wanted ? QStringLiteral("true") : QStringLiteral("false");
            const bool varies = multi != nullptr && multi->isMultiValue();
            bool current = bp->value();
            bool visible = false;
            bool inverted = false;
            const bool layerRow = visibleLayerRow(prop, visible, inverted);
            if (layerRow) {
                current = visible;
            }
            if (!varies && current == *wanted) {
                apply = [] { return true; }; // already so: some of these rows toggle whatever value they get
            }
            else {
                // the Active Layer rows toggle the layer on any change; send the value that differs from the row's own
                const bool send = layerRow && inverted ? !*wanted : *wanted;
                apply = [bp, send, reason] { return bp->setValue(send, reason); };
            }
        }
        else if (auto* ep = qobject_cast<LC_PropertyEnum*>(atomic)) {
            const LC_EnumDescriptor* d = ep->getEnumDescriptor();
            const LC_EnumValueDescriptor* found = d != nullptr ? d->findByDisplayName(text.trimmed(), Qt::CaseInsensitive) : nullptr;
            if (found == nullptr && d != nullptr) {
                d->doProcessEachEnumValue([&](const LC_EnumValueDescriptor& v) {
                    if (found == nullptr && (norm(v.getName()) == norm(text) || norm(v.getDisplayName()) == norm(text))) {
                        found = &v;
                    }
                    return true;
                });
            }
            if (found == nullptr) {
                notAnOption(QStringLiteral("a choice"));
                return false;
            }
            const LC_PropertyEnumValueType v = found->getValue();
            shown = found->getDisplayName().isEmpty() ? found->getName() : found->getDisplayName();
            apply = [ep, v, reason] { return ep->setValue(v, reason); };
        }
        else if (auto* lp = qobject_cast<LC_PropertyLayer*>(atomic)) {
            RS_Layer* layer = nullptr;
            if (RS_LayerList* list = lp->layerList()) {
                layer = list->find(text.trimmed());
                for (RS_Layer* l : *list) {
                    if (layer == nullptr && l->getName().compare(text.trimmed(), Qt::CaseInsensitive) == 0) {
                        layer = l;
                    }
                }
            }
            if (layer == nullptr) {
                notAnOption(QStringLiteral("a layer of the drawing"));
                return false;
            }
            shown = layer->getName();
            apply = [lp, layer, reason] { return lp->setValue(layer, reason); };
        }
        else if (auto* cp = qobject_cast<LC_PropertyRSColor*>(atomic)) {
            const std::optional<RS_Color> color = parseColor(text, attribute(atomic, LC_PropertyRSColorComboBoxView::ATTR_SHOW_BY_LAYER, true));
            if (!color.has_value()) {
                notAnOption(QStringLiteral("a colour"));
                return false;
            }
            const RS_Color c = *color;
            shown = colorText(c);
            apply = [cp, c, reason] { return cp->setValue(c, reason); };
        }
        else if (auto* tp = qobject_cast<LC_PropertyLineType*>(atomic)) {
            const std::optional<int> v = matchItem(lineTypeItems(attribute(atomic, LC_PropertyLineTypeComboboxView::ATTR_SHOW_BY_LAYER, true)), text);
            if (!v.has_value()) {
                notAnOption(QStringLiteral("a line type"));
                return false;
            }
            const auto lineType = static_cast<RS2::LineType>(*v);
            shown = LC_PenInfoRegistry::instance()->getLineTypeText(lineType);
            apply = [tp, lineType, reason] { return tp->setValue(lineType, reason); };
        }
        else if (auto* wp = qobject_cast<LC_PropertyLineWidth*>(atomic)) {
            const std::optional<int> v = matchItem(lineWidthItems(attribute(atomic, LC_PropertyLineWidthComboboxView::ATTR_SHOW_BY_LAYER, true)), text);
            if (!v.has_value()) {
                notAnOption(QStringLiteral("a line width"));
                return false;
            }
            const auto width = static_cast<RS2::LineWidth>(*v);
            shown = LC_PenInfoRegistry::instance()->getLineWidthText(width);
            apply = [wp, width, reason] { return wp->setValue(width, reason); };
        }
        else if (auto* sp = qobject_cast<LC_PropertyQString*>(atomic)) {
            QString s = value.isString() ? value.toString() : text;
            const QByteArray view = viewNameOf(atomic);
            const bool fixedList = (view == LC_PropertyQStringListComboBoxView::VIEW_NAME
                                    && !attribute(atomic, LC_PropertyQStringListComboBoxView::ATTR_EDITABLE, false))
                                   || view == LC_PropertyQStringFontComboboxView::VIEW_NAME;
            if (fixedList && !options.isEmpty()) {
                QString match;
                for (const QString& o : options) {
                    if (norm(o) == norm(s)) {
                        match = o;
                    }
                }
                if (match.isEmpty()) {
                    notAnOption(QStringLiteral("a choice"));
                    return false;
                }
                s = match;
            }
            shown = quoted(s);
            apply = [sp, s, reason] { return sp->setValue(s, reason); };
        }
        else {
            error = QStringLiteral("\"%1\" holds a %2, which this tool cannot set.").arg(path, atomic->metaObject()->className());
            return false;
        }

        LC_PropertiesSheet* sheet = p.sheet;
        sheet->stopInplaceEdit();
        sheet->setActiveProperty(prop, true); // the row a person clicks into
        if (multi != nullptr) {
            multi->markEdited(true); // passes the value on to the other selected entities
        }
        sheet->connectOnPropertyChange(prop, true);
        const bool accepted = apply();
        sheet->connectOnPropertyChange(prop, false);
        if (multi != nullptr) {
            multi->markEdited(false);
        }
        if (!accepted) {
            error = QStringLiteral("OpenCAD did not accept %1 for \"%2\" (not allowed for this row).").arg(shown, path);
            return false;
        }
        return true;
    }

    // ------------------------------------------------------------------ the actions

    void doSet(const Panel& p, const QJsonObject& args, Done done) {
        const bool capture = args.value("capture").toBool();
        const QList<Row> rows = allRows(p);
        QString error;
        const int index = findRow(rows, args.value("row").toString(), error);
        if (index < 0) {
            done(LC_AIToolResult::error(error));
            return;
        }
        const Row row = rows[index];
        const QString path = row.path.join(" / ");
        const State before = stateNow(p);
        const QString old = row.item->property->isContainer() ? QString() : describe(row.item->property, false).value;
        QString shown;
        if (!applyValue(p, row, args.value("value"), shown, error)) {
            done(LC_AIToolResult::error(error));
            return;
        }
        const QString note = p.note;
        const QString toolMode = toolOptionsOf(p);
        QTimer::singleShot(EDIT_SETTLE_MS, qApp, [before, path, old, shown, note, toolMode, capture, done] {
            const Panel now = panel();
            QStringList lines;
            if (!note.isEmpty()) {
                lines << note;
            }
            lines << QStringLiteral("Set \"%1\" to %2%3.").arg(path, shown, old.isEmpty() ? QString() : " (was " + old + ")");
            if (!now.error.isEmpty()) {
                lines << now.error;
                done(finish(lines, capture));
                return;
            }
            const State after = stateNow(now);
            const Changes changes = compare(before, after);
            lines << changes.lines;
            const QList<Row> rowsNow = allRows(now);
            QString ignored;
            const int i = findRow(rowsNow, path, ignored);
            const bool rowChanged = i >= 0 && describe(rowsNow[i].item->property, false).value != old;
            if (changes.entitiesChanged > 0) {
                lines << "This edit is one undo step: press_toolbar_button EditUndo reverts it.";
            }
            else if (rowChanged || after.mode != before.mode || !changes.lines.isEmpty()) {
                lines << (toolMode.isEmpty() ? QStringLiteral("A drawing setting: applied at once, not on the undo stack.")
                                             : QStringLiteral("An option of the running tool \"%1\".").arg(toolMode));
            }
            else {
                lines << QStringLiteral("Nothing changed (\"%1\" already was %2).").arg(path, old.isEmpty() ? shown : old);
            }
            if (i >= 0) {
                lines << "Now: " + rowLine(rowsNow, i, false, true).trimmed();
            }
            lines << situation(before);
            done(finish(lines, capture));
        });
    }

    /** Runs something that may open a dialog or start a tool, then reports what it did. */
    void clickAndReport(const Panel& p, const QString& what, std::function<void()> click, const bool capture, Done done) {
        const State before = stateNow(p);
        const QString note = p.note;
        QTimer::singleShot(0, qApp, std::move(click)); // a dialog it opens runs its own event loop; the report comes from inside it
        QTimer::singleShot(CLICK_SETTLE_MS, qApp, [before, what, note, capture, done] {
            const Panel now = panel();
            QStringList lines;
            if (!note.isEmpty()) {
                lines << note;
            }
            lines << what;
            const State after = now.error.isEmpty() ? stateNow(now) : State{};
            if (now.error.isEmpty()) {
                lines << compare(before, after).lines;
            }
            lines << situation(before);
            done(finish(lines, capture));
        });
    }

    void doClickLink(const Panel& p, const QJsonObject& args, Done done) {
        const QList<Row> rows = allRows(p);
        const QString link = clean(args.value("link").toString());
        int index = -1;
        int linkIndex = args.contains("index") ? args.value("index").toInt() : 0;
        QString error;
        if (!link.isEmpty()) {
            QStringList all;
            for (int i = 0; i < rows.size() && index < 0; ++i) {
                auto* action = qobject_cast<LC_PropertyAction*>(firstAtomic(rows[i].item->property));
                if (action == nullptr) {
                    continue;
                }
                const QStringList titles = linkTitles(action);
                for (int k = 0; k < titles.size(); ++k) {
                    if (titles[k].isEmpty()) {
                        continue;
                    }
                    all << "\"" + titles[k] + "\"";
                    if (norm(titles[k]) == norm(link) && (args.value("row").toString().isEmpty() || rows[i].id == args.value("row").toString()
                                                          || rows[i].path.join(" / ").compare(args.value("row").toString(), Qt::CaseInsensitive) == 0)) {
                        index = i;
                        linkIndex = k;
                        break;
                    }
                }
            }
            if (index < 0) {
                done(LC_AIToolResult::error(QStringLiteral("No link \"%1\" in the panel. Links: %2").arg(link, all.join(", "))));
                return;
            }
        }
        else {
            index = findRow(rows, args.value("row").toString(), error);
            if (index < 0) {
                done(LC_AIToolResult::error(error.isEmpty() ? QStringLiteral("Give link (its title) or row.") : error));
                return;
            }
        }
        const Row& row = rows[index];
        auto* action = qobject_cast<LC_PropertyAction*>(firstAtomic(row.item->property));
        if (action == nullptr) {
            done(LC_AIToolResult::error(QStringLiteral("\"%1\" has no links; use action set to change it.").arg(row.path.join(" / "))));
            return;
        }
        QList<bool> enabled;
        const QStringList titles = linkTitles(action, &enabled);
        if (linkIndex < 0 || linkIndex >= titles.size() || titles[linkIndex].isEmpty()) {
            done(LC_AIToolResult::error(QStringLiteral("\"%1\" has no link %2. Its links: %3")
                                            .arg(row.path.join(" / ")).arg(linkIndex).arg(describe(row.item->property).links.join(" | "))));
            return;
        }
        if (!enabled.value(linkIndex, true)) {
            done(LC_AIToolResult::error(QStringLiteral("The link \"%1\" is disabled.").arg(titles[linkIndex])));
            return;
        }
        p.sheet->stopInplaceEdit();
        p.sheet->setActiveProperty(row.item->property, true);
        QPointer<LC_PropertyAction> target = action;
        clickAndReport(p, QStringLiteral("Clicked the link \"%1\" (%2 %3).").arg(titles[linkIndex], row.id, row.path.join(" / ")),
                       [target, linkIndex] {
                           if (target) {
                               target->invokeClick(linkIndex); // the panel runs the link's command 10 ms later
                           }
                       },
                       args.value("capture").toBool(), std::move(done));
    }

    void doPick(const Panel& p, const QJsonObject& args, Done done) {
        const QList<Row> rows = allRows(p);
        QString error;
        const int index = findRow(rows, args.value("row").toString(), error);
        if (index < 0) {
            done(LC_AIToolResult::error(error));
            return;
        }
        const Row& row = rows[index];
        LC_Property* prop = row.item->property;
        LC_PropertyAtomic* atomic = prop->isContainer() ? nullptr : firstAtomic(prop);
        if (atomic == nullptr || !prop->isEditableByUser() || !canPick(atomic)) {
            done(LC_AIToolResult::error(QStringLiteral("\"%1\" cannot be picked from the drawing (only editable points, coordinates, "
                                                       "distances and angles can); use action set.").arg(row.path.join(" / "))));
            return;
        }
        QString what;
        if (auto* d = qobject_cast<LC_PropertyDouble*>(atomic)) {
            switch (d->getInteractiveInputType()) {
                case LC_ActionContext::InteractiveInputInfo::DISTANCE:
                    what = QStringLiteral("a distance: click its two points");
                    break;
                case LC_ActionContext::InteractiveInputInfo::ANGLE:
                    what = QStringLiteral("an angle: click the points the prompt asks for");
                    break;
                default:
                    what = QStringLiteral("a coordinate: click the point");
                    break;
            }
        }
        else {
            what = QStringLiteral("a point: click it");
        }
        p.sheet->stopInplaceEdit();
        p.sheet->setActiveProperty(prop, true);
        QPointer<LC_PropertyAtomic> target = atomic;
        clickAndReport(p, QStringLiteral("Started picking \"%1\" (%2) from the drawing: %3 with click_drawing. The row takes the "
                                         "value when the pick ends (one undo step for an entity).").arg(row.path.join(" / "), row.id, what),
                       [target] {
                           if (auto* d = qobject_cast<LC_PropertyDouble*>(target.data())) {
                               d->requestInteractiveInput();
                           }
                           else if (auto* v = qobject_cast<LC_PropertyRSVector*>(target.data())) {
                               v->requestInteractiveInput();
                           }
                       },
                       args.value("capture").toBool(), std::move(done));
    }

    void doChooseType(const Panel& p, const QJsonObject& args, Done done) {
        QComboBox* types = typeList(p);
        if (!toolOptionsOf(p).isEmpty() || types == nullptr || !types->isVisible()) {
            done(LC_AIToolResult::error(QStringLiteral("The panel shows the running tool's options; it has no list of selected types now.")));
            return;
        }
        const QString wanted = norm(args.value("type").toString());
        if (wanted.isEmpty()) {
            done(LC_AIToolResult::error(QStringLiteral("Give type: one of the panel's selected types (e.g. Line, Circle or All).")));
            return;
        }
        static const QRegularExpression count(QStringLiteral("\\s*\\(\\d+\\)$"));
        int found = -1;
        QStringList items;
        for (int i = 0; i < types->count(); ++i) {
            const QString item = norm(types->itemText(i));
            items << clean(types->itemText(i));
            QString bare = item;
            bare.remove(count);
            if (found < 0 && (item == wanted || bare == wanted)) {
                found = i;
            }
        }
        for (int i = 0; i < types->count() && found < 0; ++i) {
            if (norm(types->itemText(i)).startsWith(wanted)) {
                found = i;
            }
        }
        if (found < 0) {
            done(LC_AIToolResult::error(QStringLiteral("No type \"%1\" in the panel's list: %2.").arg(args.value("type").toString(), items.join(" | "))));
            return;
        }
        const bool same = found == types->currentIndex();
        types->setCurrentIndex(found); // the panel rebuilds its rows for that type
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        const Panel now = panel();
        QString error;
        QStringList lines;
        lines << (same ? QStringLiteral("The panel already showed \"%1\".") : QStringLiteral("The panel now shows \"%1\"."))
                     .arg(clean(types->itemText(found)));
        lines << readPanel(now, {}, error);
        done(finish(lines, args.value("capture").toBool()));
    }

    void doFold(const Panel& p, const QJsonObject& args, const bool collapse, Done done) {
        const QList<Row> rows = allRows(p);
        QString error;
        const int index = findRow(rows, args.value("row").toString(), error);
        if (index < 0) {
            done(LC_AIToolResult::error(error));
            return;
        }
        const Row& row = rows[index];
        if (row.item->children.empty()) {
            done(LC_AIToolResult::error(QStringLiteral("\"%1\" has nothing to %2.").arg(row.path.join(" / "), collapse ? "collapse" : "expand")));
            return;
        }
        LC_Property* state = row.item->view ? row.item->view->getStateProperty() : row.item->property;
        const bool was = state->isCollapsed();
        state->switchState(PropertyStateCollapsed, collapse); // as clicking the row's arrow does
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        const Panel now = panel();
        QStringList lines;
        lines << (was == collapse ? QStringLiteral("\"%1\" already was %2.") : QStringLiteral("%2 \"%1\"."))
                     .arg(row.path.join(" / "), was == collapse ? (collapse ? "collapsed" : "expanded") : (collapse ? "Collapsed" : "Expanded"));
        lines << readPanel(now, row.id, error);
        done(finish(lines, args.value("capture").toBool()));
    }

    void doSettings(const Panel& p, const QJsonObject& args, Done done) {
        QPointer<QToolButton> button = p.widget->findChild<QToolButton*>(QStringLiteral("tbSettings"));
        if (!button || !button->isEnabled()) {
            done(LC_AIToolResult::error(QStringLiteral("The panel's settings button is not available.")));
            return;
        }
        clickAndReport(p, QStringLiteral("Clicked the panel's settings button."),
                       [button] {
                           if (button) {
                               button->click(); // opens the options dialog and waits in it
                           }
                       },
                       args.value("capture").toBool(), std::move(done));
    }

    LC_AITool useProperties() {
        LC_AITool t;
        t.name = "use_properties";
        t.label = "Use properties panel";
        t.description =
            "Read and change OpenCAD's Properties panel exactly as a person does. It shows the selected entities' properties "
            "(layer, colour, line type, line width; geometry such as start/end points, centre, radius, angles; text content, "
            "alignment...), the drawing's settings when nothing is selected (pen for new entities, active layer, grid, units, "
            "paper...), or the running tool's options. Select entities first (select_entities or click_drawing).\n"
            "action read (default): the rows with ids (p2.1), type and value, choices, links and the list of selected types; "
            "with row it reads one row or section in full (all options, X/Y parts of a point).\n"
            "action set: change ONE row (row: id, path like \"Geometry / Radius\" or name) to value. Lengths in drawing units, "
            "angles in degrees, points \"x,y\" (current UCS), colours \"By Layer\", \"By Block\", a name (Red) or #rrggbb, "
            "layers / line types / line widths / choices by the name the panel shows, true/false, text as is. One call per "
            "change. An entity edit is one undo step (press_toolbar_button EditUndo reverts it); drawing settings apply at once. "
            "Read-only rows (calculated values like a line's length) are refused: change the rows they come from. The result "
            "lists the panel rows and the entities that changed.\n"
            "action click_link: click one of the panel's links by its title (link, e.g. \"Add layer...\", \"Select all\", "
            "\"Move / Copy\") or by row and index 0/1. It may open a dialog (finish it with handle_dialog) or start a tool "
            "(answer it with click_drawing or run_command).\n"
            "action choose_type: with several entity types selected, choose which the panel shows (type: Line, Circle, All...; "
            "All shows what they have in common). action collapse / expand: fold a section (row). action settings: open the "
            "panel's options dialog (finish it with handle_dialog). action pick: pick a row's point, coordinate, distance or "
            "angle from the drawing, then click it with click_drawing.\n"
            "While a drawing tool runs, the panel ignores selection changes (press_toolbar_button EditKillAllActions ends the tool).";
        t.parameters = schema({
            {"action", QJsonObject{{"type", "string"},
                                   {"enum", QJsonArray{"read", "set", "click_link", "choose_type", "collapse", "expand", "settings", "pick"}},
                                   {"description", "What to do (default read)"}}},
            {"row", str("The row: its id from read (p2.1), its path (\"Geometry / Radius\", \"Geometry / Start / X\", \"General / "
                        "Color\") or its name (\"Radius\"). For read: one row or section to read in full.")},
            {"value", str("For set: the new value as the panel takes it: a number, an angle in degrees, \"x,y\", a colour, a layer, "
                          "a line type or width, a choice, true/false, or text")},
            {"link", str("For click_link: the link's title, e.g. \"Add layer...\"")},
            {"index", QJsonObject{{"type", "integer"}, {"description", "For click_link with row: 0 the left link, 1 the right one"}}},
            {"type", str("For choose_type: the selected type to show, e.g. Line, Circle, All")},
            {"capture", QJsonObject{{"type", "boolean"}, {"description", "Also return a screenshot of the drawing afterwards (default false)"}}}});
        t.executeAsync = [](const QJsonObject& args, Done done) {
            const QString action = args.value("action").toString(QStringLiteral("read")).trimmed().toLower();
            const Panel p = panel();
            if (!p.error.isEmpty()) {
                done(LC_AIToolResult::error(p.error));
                return;
            }
            if (action.isEmpty() || action == "read") {
                QString error;
                const QString text = readPanel(p, args.value("row").toString(), error);
                done(error.isEmpty() ? finish({text}, args.value("capture").toBool()) : LC_AIToolResult::error(error));
                return;
            }
            if (QWidget* dialog = currentDialog()) {
                done(LC_AIToolResult::error("A dialog is open; finish it with handle_dialog first.\n" + describeDialog(dialog)));
                return;
            }
            if (action == "set") {
                doSet(p, args, std::move(done));
            }
            else if (action == "click_link" || action == "click") {
                doClickLink(p, args, std::move(done));
            }
            else if (action == "choose_type") {
                doChooseType(p, args, std::move(done));
            }
            else if (action == "collapse" || action == "expand") {
                doFold(p, args, action == "collapse", std::move(done));
            }
            else if (action == "settings") {
                doSettings(p, args, std::move(done));
            }
            else if (action == "pick") {
                doPick(p, args, std::move(done));
            }
            else {
                done(LC_AIToolResult::error(QStringLiteral("Unknown action \"%1\": use read, set, click_link, choose_type, collapse, "
                                                           "expand, settings or pick.").arg(action)));
            }
        };
        return t;
    }
}

QList<LC_AITool> LC_AIProperties::tools() {
    return {useProperties()};
}
