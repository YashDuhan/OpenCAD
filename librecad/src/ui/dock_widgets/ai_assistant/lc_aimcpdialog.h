/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#ifndef LC_AIMCPDIALOG_H
#define LC_AIMCPDIALOG_H

#include <QDialog>
#include <functional>

#include "lc_aiprovidersdialog.h"
#include "lc_mcpclient.h"

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;
class QWidget;

/**
 * Port of Cypher's MCP page (apps/desktop/src/components/Mcp.tsx) as a large modal: server cards with
 * state, tools and errors; an add/edit form with Browse buttons for the command and its folder;
 * Import file to bring in the "mcpServers" of another config; templates for UniCAD; and the raw JSON.
 */
class LC_AIMcpDialog : public QDialog {
    Q_OBJECT
public:
    explicit LC_AIMcpDialog(QWidget* parent = nullptr);

private:
    void rebuild();
    QWidget* card(LC_MCPClient* client);
    QWidget* emptyState();
    void showForm(const LC_MCPServerConfig& config, const QString& editing);
    void hideForm();
    void saveForm();
    void importFile();
    void showJson(bool json);
    QPushButton* button(const QString& text, const QString& icon, bool accent);
    QWidget* iconButton(const QString& icon, const QString& tip, const std::function<void()>& onClick);

    LC_AITheme m_theme;
    QStackedWidget* m_views {nullptr};
    QWidget* m_cards {nullptr};
    QVBoxLayout* m_cardsLayout {nullptr};
    QWidget* m_tools {nullptr};
    QVBoxLayout* m_toolsLayout {nullptr};
    QPushButton* m_serversTab {nullptr};
    QPushButton* m_jsonTab {nullptr};
    QPlainTextEdit* m_json {nullptr};
    QLabel* m_jsonError {nullptr};
    QLabel* m_notice {nullptr};

    // add / edit form
    QWidget* m_form {nullptr};
    QLabel* m_formTitle {nullptr};
    QString m_editing;
    QLineEdit* m_name {nullptr};
    QPushButton* m_stdioTab {nullptr};
    QPushButton* m_httpTab {nullptr};
    QWidget* m_stdioFields {nullptr};
    QWidget* m_httpFields {nullptr};
    QLineEdit* m_command {nullptr};
    QLineEdit* m_args {nullptr};
    QLineEdit* m_cwd {nullptr};
    QPlainTextEdit* m_env {nullptr};
    QLineEdit* m_url {nullptr};
    QPlainTextEdit* m_headers {nullptr};
    QLineEdit* m_description {nullptr};
    QLabel* m_formError {nullptr};
    bool m_http {false};
};

#endif
