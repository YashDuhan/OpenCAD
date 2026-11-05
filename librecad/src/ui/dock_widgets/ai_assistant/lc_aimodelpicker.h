/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIMODELPICKER_H
#define LC_AIMODELPICKER_H

#include <QFrame>
#include <QList>

#include "lc_aiprovidersdialog.h"

class QLineEdit;
class QListWidget;
class QToolButton;
class QVBoxLayout;

/**
 * Port of Cypher's ModelPicker (apps/desktop/src/components/ModelPicker.tsx), shown as a popup above
 * the composer's model button: a rail of providers that have credentials (Favorites first), a search
 * box, and the models of the chosen provider. Typing searches every provider; the star pins a model to
 * Favorites; Ctrl+1..9 pick the first nine rows; the arrow keys and Enter move through the list.
 */
class LC_AIModelPicker : public QFrame {
    Q_OBJECT
public:
    struct Entry {
        QString provider;
        QString id;
        QString name;
        qint64 contextWindow {0};
        bool reasoning {false};
        bool images {false};
        QString key() const { return provider + "/" + id; }
    };

    explicit LC_AIModelPicker(const LC_AITheme& theme, QWidget* parent = nullptr);
    /** Opens above (or below, when there is no room) the anchor, right edges aligned. */
    void popup(QWidget* anchor);

signals:
    void picked(const QString& providerId, const QString& modelId);
    void providersRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildEntries();
    void buildRail();
    void setTab(const QString& tab);
    void refreshList();
    void pick(int row);
    void toggleFavorite(int row);

    LC_AITheme m_theme;
    QList<Entry> m_entries;
    QList<int> m_visible;         // indexes into m_entries, in list order
    QStringList m_favorites;
    QString m_tab;
    QString m_currentKey;
    QVBoxLayout* m_railLayout {nullptr};
    QList<QToolButton*> m_railButtons;
    QLineEdit* m_search {nullptr};
    QListWidget* m_list {nullptr};

    friend class LC_AIModelPickerDelegate;
};

#endif
