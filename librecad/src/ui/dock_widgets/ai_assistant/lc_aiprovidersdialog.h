/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIPROVIDERSDIALOG_H
#define LC_AIPROVIDERSDIALOG_H

#include <QColor>
#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class QWidget;
struct LC_AIProvider;

/** Colors of Cypher's desktop app (apps/desktop/src/styles.css), dark and light. */
struct LC_AITheme {
    QColor bgRoot, bgPage, bgChrome, bgCard, bgSubtle, bgHover, bgActive, bgElevated;
    QColor borderSubtle, borderDefault, borderOverlay, borderHover, borderStrong;
    QColor textHeading, textPrimary, textSecondary, textTertiary, textMuted, textFaint, textPlaceholder;
    QColor accentGreen, accentBlue, accentBlueBorder, accentBlueBg;
    bool dark {true};
    /** Dark or light, following the current application palette. */
    static LC_AITheme current();
    QString styleSheet() const;
};

/**
 * Port of Cypher's desktop Providers page (apps/desktop/src/components/Providers.tsx) as a large modal:
 * providers on the left (Connected first, then Available) with search, and the chosen provider's
 * connection, credential and models on the right. "Use in chat" picks the model the AI sidebar talks to.
 */
class LC_AIProvidersDialog : public QDialog {
    Q_OBJECT
public:
    explicit LC_AIProvidersDialog(QWidget* parent = nullptr);

private:
    enum class Tone { On, Off };
    struct Status {
        Tone tone;
        QString text;
    };

    void reloadProviders();
    void rebuildList();
    void showDetail(const QString& providerId);
    QWidget* listRow(const LC_AIProvider& provider);
    QWidget* detail(const LC_AIProvider& provider);
    QWidget* settingRow(const QString& title, const QString& hint, QWidget* control, bool first);
    QWidget* modelsGroup(const LC_AIProvider& provider);
    QPushButton* button(const QString& text, const QString& icon, bool accent);
    QLabel* sectionLabel(const QString& text, int count = -1);
    QLabel* dot(Tone tone);

    bool isConfigured(const LC_AIProvider& provider) const;
    Status statusOf(const LC_AIProvider& provider) const;
    QString credentialTitle(const LC_AIProvider& provider) const;
    QString credentialDetail(const LC_AIProvider& provider) const;

    void startKeyEntry(const LC_AIProvider& provider);
    void removeKey(const LC_AIProvider& provider);
    void useModel(const QString& providerId, const QString& modelId);

    LC_AITheme m_theme;
    QString m_selectedId;
    QLineEdit* m_search {nullptr};
    QWidget* m_listContent {nullptr};
    QVBoxLayout* m_listLayout {nullptr};
    QScrollArea* m_panel {nullptr};
};

/** Port of Cypher's LoginDialog for the API key flow: paste the key, Continue, then Close. */
class LC_AIKeyDialog : public QDialog {
    Q_OBJECT
public:
    LC_AIKeyDialog(const LC_AIProvider& provider, const LC_AITheme& theme, QWidget* parent = nullptr);
};

#endif
