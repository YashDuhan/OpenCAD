/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIAPPROVALS_H
#define LC_AIAPPROVALS_H

#include <QDialog>
#include <QFrame>
#include <QPushButton>
#include <functional>

#include "lc_aiinferencemanager.h"
#include "lc_aipermissions.h"
#include "lc_aiprovidersdialog.h"

class QLabel;
class QScrollArea;
class QVBoxLayout;

/**
 * Tool calls waiting for the user, shown above the composer like Cypher's PendingApprovals
 * (apps/desktop/src/components/Harness.tsx): tool, reason, what it will do, and Allow once /
 * Allow for this session / Allow always / Deny.
 */
class LC_AIApprovalBox : public QFrame {
    Q_OBJECT
public:
    enum class Choice { Once, Session, Always, Deny };
    using Answer = std::function<void(Choice)>;

    explicit LC_AIApprovalBox(const LC_AITheme& theme, QWidget* parent = nullptr);
    void add(const LC_AIToolCall& call, const QString& label, const LC_AIPermissions::Decision& decision, const Answer& answer);
    void clear();
    int pending() const { return m_count; }

private:
    void updateHead();
    void fitHeight();
    QPushButton* approveButton(const QString& text, const QString& icon, const QColor& color, const QString& kind);

    LC_AITheme m_theme;
    QLabel* m_head {nullptr};
    QVBoxLayout* m_items {nullptr};
    QScrollArea* m_scroll {nullptr};
    int m_count {0};
};

/** The composer's approval mode button and menu (Cypher's ApprovalModeSelect), plus "Tool permissions...". */
class LC_AIApprovalModeButton : public QPushButton {
    Q_OBJECT
public:
    explicit LC_AIApprovalModeButton(const LC_AITheme& theme, QWidget* parent = nullptr);
    void refresh();

signals:
    void permissionsRequested();

private:
    void openMenu();

    LC_AITheme m_theme;
};

/** Allow / Ask / Deny for every tool the AI has (built-in and MCP), as on Cypher's tools settings. */
class LC_AIToolPermissionsDialog : public QDialog {
    Q_OBJECT
public:
    LC_AIToolPermissionsDialog(const QList<LC_AITool>& tools, const LC_AITheme& theme, QWidget* parent = nullptr);
};

#endif
