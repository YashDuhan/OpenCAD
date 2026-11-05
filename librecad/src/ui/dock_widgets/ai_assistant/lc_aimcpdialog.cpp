/* ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * ********************************************************************************
 */
#include "lc_aimcpdialog.h"

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include "lc_aiicons.h"
#include "lc_mcpmanager.h"

namespace {
    QString quoteArg(const QString& a) {
        return a.isEmpty() || a.contains(' ') || a.contains('"') ? '"' + QString(a).replace('"', "\\\"") + '"' : a;
    }

    QString joinArgs(const QStringList& args) {
        QStringList out;
        for (const QString& a : args) {
            out << quoteArg(a);
        }
        return out.join(' ');
    }

    /** "KEY=value" (env) or "Name: value" (headers), one per line. */
    QMap<QString, QString> parsePairs(const QString& text, const QChar separator) {
        QMap<QString, QString> pairs;
        for (const QString& line : text.split('\n')) {
            const int at = line.indexOf(separator);
            if (at > 0) {
                pairs.insert(line.left(at).trimmed(), line.mid(at + 1).trimmed());
            }
        }
        return pairs;
    }

    QString joinPairs(const QMap<QString, QString>& pairs, const QString& separator) {
        QStringList lines;
        for (auto it = pairs.cbegin(); it != pairs.cend(); ++it) {
            lines << it.key() + separator + it.value();
        }
        return lines.join('\n');
    }

    struct Template {
        QString name;
        QString text;
        LC_MCPServerConfig config;
    };

    QList<Template> templates() {
        LC_MCPServerConfig local;
        local.name = "unicad";
        local.command = "uvx";
        local.args = {"--from", "/path/to/unicad", "unicad-mcp"};
        local.env = {{"UNICAD_WORKSPACE", QDir::homePath() + "/unicad-projects"}};
        local.description = "Solar EPC drawing sets (SLD, layouts, schedules) as DXF/PDF/DWG";
        LC_MCPServerConfig http;
        http.name = "unicad";
        http.http = true;
        http.url = "http://127.0.0.1:8765/mcp";
        http.headers = {{"Authorization", "Bearer ${UNICAD_TOKEN}"}};
        http.description = local.description;
        return {{"UniCAD (local)", "Runs unicad-mcp on this computer with uvx. Its DXF files can be opened in OpenCAD.", local},
                {"UniCAD (HTTP)", "Connects to unicad-mcp --http. Add X-UniCAD-Tenant only if the server runs with --tenants.", http}};
    }

    QColor stateColor(const LC_MCPClient* c, const LC_AITheme& t) {
        if (!c->config().enabled) {
            return t.textFaint;
        }
        switch (c->state()) {
            case LC_MCPClient::State::Connected:
                return t.accentGreen;
            case LC_MCPClient::State::Connecting:
                return QColor("#fbbf24");
            case LC_MCPClient::State::Failed:
                return QColor("#f87171");
            case LC_MCPClient::State::Disconnected:
                break;
        }
        return t.textFaint;
    }

    QLabel* dotLabel(const QColor& color) {
        auto* d = new QLabel();
        d->setFixedSize(8, 8);
        d->setStyleSheet(QStringLiteral("background: %1; border-radius: 4px;").arg(color.name()));
        return d;
    }

    QLabel* badge(const QString& text) {
        auto* b = new QLabel(text);
        b->setObjectName("mcpBadge");
        return b;
    }
}

LC_AIMcpDialog::LC_AIMcpDialog(QWidget* parent)
    : QDialog(parent), m_theme(LC_AITheme::current()) {
    setWindowTitle(tr("MCP"));
    const LC_AITheme& t = m_theme;
    setStyleSheet(t.styleSheet() + QStringLiteral(R"(
#mcpCard, #mcpForm { background: %1; border: 1px solid %2; border-radius: 12px; }
#mcpName { font-size: 13.5px; font-weight: 500; color: %3; }
#mcpBadge { border: 1px solid %2; border-radius: 6px; padding: 0 6px; font-size: 11px; color: %4; }
#mcpTarget { font-family: "JetBrains Mono Variable", "JetBrains Mono", monospace; font-size: 12px; color: %5; }
#mcpState { font-size: 12.5px; color: %6; }
#mcpError { font-size: 12.5px; color: #f87171; }
#mcpPath { font-family: "JetBrains Mono Variable", "JetBrains Mono", monospace; font-size: 12px; color: %5; }
#mcpAside { background: %7; border: 1px solid %8; border-radius: 12px; }
#mcpAsideHead { font-size: 13px; font-weight: 500; color: %6; }
#mcpToolServer { font-size: 13px; font-weight: 500; color: %3; }
#mcpTool { font-family: "JetBrains Mono Variable", "JetBrains Mono", monospace; font-size: 12px; color: %4; }
#mcpFormTitle, #mcpEmptyTitle { font-size: 15px; font-weight: 600; color: %3; }
#mcpEmptyText, #mcpTemplateText { font-size: 12.5px; color: %6; }
QPushButton#mcpTemplate { background: %9; border: 1px solid %8; border-radius: 10px; min-height: 84px; max-height: 84px; padding: 0; }
QPushButton#mcpTemplate:hover { border-color: %10; }
#mcpTemplateName { font-size: 13px; font-weight: 500; color: %3; }
QPushButton[seg="true"] { background: transparent; border: 1px solid %2; color: %4; border-radius: 0; min-height: 26px; padding: 0 12px; }
QPushButton[seg="true"][on="true"] { background: %11; color: %3; }
QPushButton[segPos="left"] { border-top-left-radius: 8px; border-bottom-left-radius: 8px; }
QPushButton[segPos="right"] { border-top-right-radius: 8px; border-bottom-right-radius: 8px; border-left: none; }
QLineEdit#pvInput, QPlainTextEdit#pvInput { font-family: "JetBrains Mono Variable", "JetBrains Mono", monospace; font-size: 12.5px; }
QPlainTextEdit#pvInput { background: %9; border: 1px solid %2; border-radius: 8px; padding: 4px 6px; color: %3; }
QCheckBox { color: %4; font-size: 12.5px; }
)")
                                         .arg(t.bgElevated.name(), t.borderDefault.name(QColor::HexArgb), t.textPrimary.name(),
                                              t.textSecondary.name(), t.textFaint.name(), t.textMuted.name(), t.bgChrome.name(),
                                              t.borderSubtle.name(QColor::HexArgb), t.bgCard.name(QColor::HexArgb))
                                         .arg(t.accentBlueBorder.name(QColor::HexArgb), t.bgActive.name(QColor::HexArgb)));

    QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
    if (parent != nullptr && parent->screen() != nullptr) {
        screen = parent->screen()->availableGeometry();
    }
    resize(qMin(1120, int(screen.width() * 0.92)), qMin(760, int(screen.height() * 0.9)));
    setMinimumSize(760, 480);

    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(24, 18, 24, 22);
    page->setSpacing(14);

    // header
    auto* header = new QHBoxLayout();
    header->setSpacing(10);
    auto* titles = new QVBoxLayout();
    titles->setSpacing(4);
    auto* title = new QLabel(tr("MCP"), this);
    title->setObjectName("pvTitle");
    auto* intro = new QLabel(tr("Servers that give the AI more tools. They are set up once for OpenCAD, and their tools "
                                "work on whichever drawing is open."), this);
    intro->setObjectName("pvIntro");
    intro->setWordWrap(true);
    titles->addWidget(title);
    titles->addWidget(intro);
    header->addLayout(titles, 1);

    m_serversTab = new QPushButton(tr("Servers"), this);
    m_jsonTab = new QPushButton(tr("JSON"), this);
    for (QPushButton* b : {m_serversTab, m_jsonTab}) {
        b->setProperty("seg", true);
        b->setCursor(Qt::PointingHandCursor);
    }
    m_serversTab->setProperty("segPos", "left");
    m_jsonTab->setProperty("segPos", "right");
    auto* seg = new QHBoxLayout();
    seg->setSpacing(0);
    seg->addWidget(m_serversTab);
    seg->addWidget(m_jsonTab);
    header->addLayout(seg);
    connect(m_serversTab, &QPushButton::clicked, this, [this] { showJson(false); });
    connect(m_jsonTab, &QPushButton::clicked, this, [this] { showJson(true); });

    header->addWidget(iconButton("refresh-cw", tr("Reconnect all servers"), [] {
        for (const LC_MCPServerConfig& c : LC_MCPManager::instance().servers()) {
            LC_MCPManager::instance().reconnect(c.name);
        }
    }));
    QPushButton* import = button(tr("Import file..."), "upload", false);
    import->setToolTip(tr("Add the servers of an MCP config file (claude_desktop_config.json, .mcp.json, Cypher's mcp.json)"));
    connect(import, &QPushButton::clicked, this, &LC_AIMcpDialog::importFile);
    header->addWidget(import);
    QPushButton* add = button(tr("Add server"), "plus", true);
    connect(add, &QPushButton::clicked, this, [this] { showForm(LC_MCPServerConfig(), QString()); });
    header->addWidget(add);
    header->addWidget(iconButton("x", tr("Close"), [this] { accept(); }));
    page->addLayout(header);

    m_notice = new QLabel(this);
    m_notice->setObjectName("mcpState");
    m_notice->setWordWrap(true);
    m_notice->hide();
    page->addWidget(m_notice);

    m_views = new QStackedWidget(this);
    page->addWidget(m_views, 1);

    // servers view: cards | tools
    auto* serversView = new QWidget();
    auto* serversLayout = new QHBoxLayout(serversView);
    serversLayout->setContentsMargins(0, 0, 0, 0);
    serversLayout->setSpacing(14);
    auto* mainScroll = new QScrollArea(serversView);
    mainScroll->setWidgetResizable(true);
    mainScroll->setFrameShape(QFrame::NoFrame);
    mainScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* main = new QWidget();
    auto* mainLayout = new QVBoxLayout(main);
    mainLayout->setContentsMargins(0, 0, 6, 0);
    mainLayout->setSpacing(10);
    auto* path = new QLabel(tr("Config: %1").arg(LC_MCPManager::instance().configPath()), main);
    path->setObjectName("mcpPath");
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path->setToolTip(LC_MCPManager::instance().configPath());
    path->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // a long path must not widen the list
    mainLayout->addWidget(path);

    // add / edit form (hidden until "Add server" or Edit)
    m_form = new QFrame(main);
    m_form->setObjectName("mcpForm");
    auto* form = new QVBoxLayout(m_form);
    form->setContentsMargins(16, 14, 16, 14);
    form->setSpacing(8);
    m_formTitle = new QLabel(m_form);
    m_formTitle->setObjectName("mcpFormTitle");
    form->addWidget(m_formTitle);
    m_name = new QLineEdit(m_form);
    m_name->setObjectName("pvInput");
    m_name->setPlaceholderText(tr("Server name (letters, digits, _ and -)"));
    form->addWidget(m_name);
    m_stdioTab = new QPushButton(tr("Local command (stdio)"), m_form);
    m_httpTab = new QPushButton(tr("HTTP URL"), m_form);
    for (QPushButton* b : {m_stdioTab, m_httpTab}) {
        b->setProperty("seg", true);
        b->setCursor(Qt::PointingHandCursor);
    }
    m_stdioTab->setProperty("segPos", "left");
    m_httpTab->setProperty("segPos", "right");
    auto* transport = new QHBoxLayout();
    transport->setSpacing(0);
    transport->addWidget(m_stdioTab);
    transport->addWidget(m_httpTab);
    transport->addStretch(1);
    form->addLayout(transport);

    const auto withBrowse = [this](QLineEdit* edit, const QString& tip, const std::function<void()>& browse) {
        auto* row = new QHBoxLayout();
        row->setSpacing(6);
        row->addWidget(edit, 1);
        QPushButton* b = button(tr("Browse..."), "folder-open", false);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, browse);
        row->addWidget(b);
        return row;
    };
    m_stdioFields = new QWidget(m_form);
    auto* stdio = new QVBoxLayout(m_stdioFields);
    stdio->setContentsMargins(0, 0, 0, 0);
    stdio->setSpacing(8);
    m_command = new QLineEdit(m_stdioFields);
    m_command->setObjectName("pvInput");
    m_command->setPlaceholderText(tr("Command or path of the program (e.g. uvx, npx, node, /opt/server/run)"));
    stdio->addLayout(withBrowse(m_command, tr("Choose the program or script that starts the server"), [this] {
        const QString file = QFileDialog::getOpenFileName(this, tr("Program that starts the MCP server"), QDir::homePath());
        if (!file.isEmpty()) {
            m_command->setText(file);
        }
    }));
    m_args = new QLineEdit(m_stdioFields);
    m_args->setObjectName("pvInput");
    m_args->setPlaceholderText(tr("Arguments (space-separated; quote paths with spaces)"));
    stdio->addLayout(withBrowse(m_args, tr("Insert a folder path into the arguments (e.g. the UniCAD checkout)"), [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Folder to pass to the server"), QDir::homePath());
        if (dir.isEmpty()) {
            return;
        }
        QString text = m_args->text();
        if (text.contains("/path/to/unicad")) {
            text.replace("/path/to/unicad", quoteArg(dir));
        }
        else {
            text = QString(text.trimmed() + " " + quoteArg(dir)).trimmed();
        }
        m_args->setText(text);
    }));
    m_cwd = new QLineEdit(m_stdioFields);
    m_cwd->setObjectName("pvInput");
    m_cwd->setPlaceholderText(tr("Working directory (optional)"));
    stdio->addLayout(withBrowse(m_cwd, tr("Choose the folder the server starts in"), [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Working directory"), QDir::homePath());
        if (!dir.isEmpty()) {
            m_cwd->setText(dir);
        }
    }));
    m_env = new QPlainTextEdit(m_stdioFields);
    m_env->setObjectName("pvInput");
    m_env->setPlaceholderText(tr("Environment, one per line: KEY=value or KEY=${ENV_VAR}"));
    m_env->setFixedHeight(64);
    stdio->addWidget(m_env);
    form->addWidget(m_stdioFields);

    m_httpFields = new QWidget(m_form);
    auto* http = new QVBoxLayout(m_httpFields);
    http->setContentsMargins(0, 0, 0, 0);
    http->setSpacing(8);
    m_url = new QLineEdit(m_httpFields);
    m_url->setObjectName("pvInput");
    m_url->setPlaceholderText(tr("URL (e.g. http://127.0.0.1:8765/mcp)"));
    http->addWidget(m_url);
    m_headers = new QPlainTextEdit(m_httpFields);
    m_headers->setObjectName("pvInput");
    m_headers->setPlaceholderText(tr("Headers, one per line: Authorization: Bearer ${TOKEN}"));
    m_headers->setFixedHeight(64);
    http->addWidget(m_headers);
    form->addWidget(m_httpFields);

    m_description = new QLineEdit(m_form);
    m_description->setObjectName("pvInput");
    m_description->setPlaceholderText(tr("What the server offers, in a sentence (optional; shown to the AI)"));
    form->addWidget(m_description);
    m_formError = new QLabel(m_form);
    m_formError->setObjectName("mcpError");
    m_formError->setWordWrap(true);
    m_formError->hide();
    form->addWidget(m_formError);
    auto* formButtons = new QHBoxLayout();
    formButtons->addStretch(1);
    QPushButton* cancel = button(tr("Cancel"), QString(), false);
    connect(cancel, &QPushButton::clicked, this, &LC_AIMcpDialog::hideForm);
    QPushButton* save = button(tr("Save and connect"), "plug", true);
    connect(save, &QPushButton::clicked, this, &LC_AIMcpDialog::saveForm);
    formButtons->addWidget(cancel);
    formButtons->addWidget(save);
    form->addLayout(formButtons);
    m_form->hide();
    mainLayout->addWidget(m_form);
    const auto setTransport = [this](const bool isHttp) {
        m_http = isHttp;
        m_stdioTab->setProperty("on", !isHttp);
        m_httpTab->setProperty("on", isHttp);
        for (QPushButton* b : {m_stdioTab, m_httpTab}) {
            b->style()->unpolish(b);
            b->style()->polish(b);
        }
        m_stdioFields->setVisible(!isHttp);
        m_httpFields->setVisible(isHttp);
    };
    connect(m_stdioTab, &QPushButton::clicked, this, [setTransport] { setTransport(false); });
    connect(m_httpTab, &QPushButton::clicked, this, [setTransport] { setTransport(true); });
    setTransport(false);

    m_cards = new QWidget(main);
    m_cardsLayout = new QVBoxLayout(m_cards);
    m_cardsLayout->setContentsMargins(0, 0, 0, 0);
    m_cardsLayout->setSpacing(10);
    mainLayout->addWidget(m_cards);
    mainLayout->addStretch(1);
    mainScroll->setWidget(main);
    serversLayout->addWidget(mainScroll, 1);

    auto* aside = new QFrame(serversView);
    aside->setObjectName("mcpAside");
    aside->setFixedWidth(300);
    auto* asideLayout = new QVBoxLayout(aside);
    asideLayout->setContentsMargins(14, 12, 14, 12);
    auto* asideHead = new QLabel(tr("Tools"), aside);
    asideHead->setObjectName("mcpAsideHead");
    asideLayout->addWidget(asideHead);
    auto* toolsScroll = new QScrollArea(aside);
    toolsScroll->setWidgetResizable(true);
    toolsScroll->setFrameShape(QFrame::NoFrame);
    m_tools = new QWidget();
    m_toolsLayout = new QVBoxLayout(m_tools);
    m_toolsLayout->setContentsMargins(0, 6, 0, 0);
    m_toolsLayout->setSpacing(2);
    toolsScroll->setWidget(m_tools);
    asideLayout->addWidget(toolsScroll, 1);
    serversLayout->addWidget(aside);
    m_views->addWidget(serversView);

    // JSON view
    auto* jsonView = new QWidget();
    auto* jsonLayout = new QVBoxLayout(jsonView);
    jsonLayout->setContentsMargins(0, 0, 0, 0);
    jsonLayout->setSpacing(8);
    auto* jsonPath = new QLabel(LC_MCPManager::instance().configPath(), jsonView);
    jsonPath->setObjectName("mcpPath");
    jsonPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    jsonLayout->addWidget(jsonPath);
    m_json = new QPlainTextEdit(jsonView);
    m_json->setObjectName("pvInput");
    jsonLayout->addWidget(m_json, 1);
    m_jsonError = new QLabel(jsonView);
    m_jsonError->setObjectName("mcpError");
    m_jsonError->hide();
    jsonLayout->addWidget(m_jsonError);
    auto* jsonButtons = new QHBoxLayout();
    jsonButtons->addStretch(1);
    QPushButton* jsonSave = button(tr("Save"), QString(), true);
    connect(jsonSave, &QPushButton::clicked, this, [this] {
        const QString error = LC_MCPManager::instance().setConfigText(m_json->toPlainText());
        m_jsonError->setText(error);
        m_jsonError->setVisible(!error.isEmpty());
        if (error.isEmpty()) {
            showJson(false);
        }
    });
    jsonButtons->addWidget(jsonSave);
    jsonLayout->addLayout(jsonButtons);
    m_views->addWidget(jsonView);

    connect(&LC_MCPManager::instance(), &LC_MCPManager::changed, this, &LC_AIMcpDialog::rebuild);
    showJson(false);
    LC_MCPManager::instance().connectAll();
}

QPushButton* LC_AIMcpDialog::button(const QString& text, const QString& icon, const bool accent) {
    auto* b = new QPushButton(text);
    b->setProperty("kind", accent ? "accent" : "ghost");
    b->setCursor(Qt::PointingHandCursor);
    b->setAutoDefault(false);
    if (!icon.isEmpty()) {
        b->setIcon(LC_AIIcons::lucideIcon(icon, 14, accent ? m_theme.bgRoot : m_theme.textSecondary));
        b->setIconSize(QSize(14, 14));
    }
    return b;
}

QWidget* LC_AIMcpDialog::iconButton(const QString& icon, const QString& tip, const std::function<void()>& onClick) {
    auto* b = new QToolButton();
    b->setObjectName("iconBtn");
    b->setIcon(LC_AIIcons::lucideIcon(icon, 15, m_theme.textTertiary));
    b->setIconSize(QSize(15, 15));
    b->setToolTip(tip);
    b->setCursor(Qt::PointingHandCursor);
    connect(b, &QToolButton::clicked, this, onClick);
    return b;
}

void LC_AIMcpDialog::showJson(const bool json) {
    m_serversTab->setProperty("on", !json);
    m_jsonTab->setProperty("on", json);
    for (QPushButton* b : {m_serversTab, m_jsonTab}) {
        b->style()->unpolish(b);
        b->style()->polish(b);
    }
    if (json) {
        m_json->setPlainText(LC_MCPManager::instance().configText());
        m_jsonError->hide();
    }
    m_views->setCurrentIndex(json ? 1 : 0);
    rebuild();
}

void LC_AIMcpDialog::rebuild() {
    const auto clear = [](QVBoxLayout* layout) {
        while (QLayoutItem* item = layout->takeAt(0)) {
            if (QWidget* w = item->widget()) {
                w->hide();
                w->deleteLater(); // may be rebuilt from one of its own buttons
            }
            delete item;
        }
    };
    clear(m_cardsLayout);
    clear(m_toolsLayout);
    LC_MCPManager& mcp = LC_MCPManager::instance();
    const QList<LC_MCPServerConfig> servers = mcp.servers();
    if (servers.isEmpty() && !m_form->isVisible()) {
        m_cardsLayout->addWidget(emptyState());
    }
    for (const LC_MCPServerConfig& s : servers) {
        if (LC_MCPClient* c = mcp.client(s.name)) {
            m_cardsLayout->addWidget(card(c));
        }
    }

    // aside: each server with its tools
    for (const LC_MCPServerConfig& s : servers) {
        LC_MCPClient* c = mcp.client(s.name);
        if (c == nullptr) {
            continue;
        }
        auto* head = new QWidget(m_tools);
        auto* headLayout = new QHBoxLayout(head);
        headLayout->setContentsMargins(0, 8, 0, 2);
        headLayout->setSpacing(7);
        headLayout->addWidget(dotLabel(stateColor(c, m_theme)));
        auto* name = new QLabel(s.name, head);
        name->setObjectName("mcpToolServer");
        headLayout->addWidget(name, 1);
        headLayout->addWidget(badge(QString::number(c->tools().size())), 0, Qt::AlignVCenter);
        m_toolsLayout->addWidget(head);
        for (const LC_MCPClient::Tool& tool : c->tools()) {
            auto* label = new QLabel(tool.name, m_tools);
            label->setObjectName("mcpTool");
            label->setToolTip(tool.description);
            label->setContentsMargins(15, 0, 0, 0);
            m_toolsLayout->addWidget(label);
        }
    }
    if (servers.isEmpty()) {
        auto* none = new QLabel(tr("No servers"), m_tools);
        none->setObjectName("pvEmptyNote");
        m_toolsLayout->addWidget(none);
    }
    m_toolsLayout->addStretch(1);
}

QWidget* LC_AIMcpDialog::card(LC_MCPClient* client) {
    const LC_MCPServerConfig& s = client->config();
    auto* card = new QFrame(m_cards);
    card->setObjectName("mcpCard");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 12, 12, 12);
    layout->setSpacing(4);

    auto* head = new QHBoxLayout();
    head->setSpacing(8);
    head->addWidget(dotLabel(stateColor(client, m_theme)));
    auto* name = new QLabel(s.name, card);
    name->setObjectName("mcpName");
    head->addWidget(name);
    head->addWidget(badge(s.http ? "HTTP" : "STDIO"), 0, Qt::AlignVCenter);
    head->addWidget(badge(tr("%n tools", nullptr, int(client->tools().size()))), 0, Qt::AlignVCenter);
    head->addStretch(1);
    auto* enabled = new QCheckBox(tr("Enabled"), card);
    enabled->setChecked(s.enabled);
    const QString serverName = s.name;
    connect(enabled, &QCheckBox::toggled, this, [serverName](const bool on) {
        LC_MCPManager::instance().setEnabled(serverName, on);
    });
    head->addWidget(enabled);
    head->addWidget(iconButton("rotate-ccw", tr("Reconnect"), [serverName] { LC_MCPManager::instance().reconnect(serverName); }));
    head->addWidget(iconButton("pencil", tr("Edit"), [this, s] { showForm(s, s.name); }));
    head->addWidget(iconButton("trash-2", tr("Delete server"), [this, serverName] {
        if (QMessageBox::question(this, tr("Delete MCP server"), tr("Delete the MCP server \"%1\"?").arg(serverName))
            == QMessageBox::Yes) {
            LC_MCPManager::instance().removeServer(serverName);
        }
    }));
    layout->addLayout(head);

    auto* target = new QLabel(s.target(), card);
    target->setObjectName("mcpTarget");
    target->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    target->setTextInteractionFlags(Qt::TextSelectableByMouse);
    target->setWordWrap(true);
    layout->addWidget(target);
    QString state = client->stateText();
    if (!client->serverInfo().isEmpty() && client->state() == LC_MCPClient::State::Connected) {
        state += " · " + client->serverInfo();
    }
    if (!s.description.isEmpty()) {
        state += " · " + s.description;
    }
    auto* stateLabel = new QLabel(state, card);
    stateLabel->setObjectName("mcpState");
    stateLabel->setWordWrap(true);
    layout->addWidget(stateLabel);
    if (!client->error().isEmpty()) {
        auto* error = new QLabel(client->error(), card);
        error->setObjectName("mcpError");
        error->setWordWrap(true);
        error->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(error);
    }
    return card;
}

QWidget* LC_AIMcpDialog::emptyState() {
    auto* empty = new QWidget(m_cards);
    auto* layout = new QVBoxLayout(empty);
    layout->setContentsMargins(0, 30, 0, 0);
    layout->setSpacing(10);
    auto* icon = new QLabel(empty);
    icon->setPixmap(LC_AIIcons::lucide("plug", 28, m_theme.textMuted, devicePixelRatioF()));
    icon->setAlignment(Qt::AlignCenter);
    auto* title = new QLabel(tr("No MCP servers yet"), empty);
    title->setObjectName("mcpEmptyTitle");
    title->setAlignment(Qt::AlignCenter);
    auto* text = new QLabel(tr("Add a server with \"Add server\", import a config file, or start from a template."), empty);
    text->setObjectName("mcpEmptyText");
    text->setAlignment(Qt::AlignCenter);
    layout->addWidget(icon);
    layout->addWidget(title);
    layout->addWidget(text);
    auto* row = new QHBoxLayout();
    row->setSpacing(10);
    row->addStretch(1);
    for (const Template& tpl : templates()) {
        auto* b = new QPushButton(empty);
        b->setObjectName("mcpTemplate");
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedSize(280, 84);
        auto* bl = new QVBoxLayout(b);
        bl->setContentsMargins(12, 10, 12, 10);
        auto* name = new QLabel(tpl.name, b);
        name->setObjectName("mcpTemplateName");
        auto* desc = new QLabel(tpl.text, b);
        desc->setObjectName("mcpTemplateText");
        desc->setWordWrap(true);
        for (QLabel* l : {name, desc}) {
            l->setAttribute(Qt::WA_TransparentForMouseEvents);
            bl->addWidget(l);
        }
        const LC_MCPServerConfig config = tpl.config;
        connect(b, &QPushButton::clicked, this, [this, config] { showForm(config, QString()); });
        row->addWidget(b);
    }
    row->addStretch(1);
    layout->addLayout(row);
    return empty;
}

void LC_AIMcpDialog::showForm(const LC_MCPServerConfig& config, const QString& editing) {
    m_editing = editing;
    m_formTitle->setText(editing.isEmpty() ? tr("Add MCP server") : tr("Edit %1").arg(editing));
    m_name->setText(config.name);
    m_command->setText(config.command);
    m_args->setText(joinArgs(config.args));
    m_cwd->setText(config.cwd);
    m_env->setPlainText(joinPairs(config.env, "="));
    m_url->setText(config.url);
    m_headers->setPlainText(joinPairs(config.headers, ": "));
    m_description->setText(config.description);
    (config.http ? m_httpTab : m_stdioTab)->click();
    m_formError->hide();
    m_form->show();
    m_name->setFocus();
    rebuild();
}

void LC_AIMcpDialog::hideForm() {
    m_form->hide();
    m_editing.clear();
    rebuild();
}

void LC_AIMcpDialog::saveForm() {
    LC_MCPServerConfig c;
    if (!m_editing.isEmpty()) {
        for (const LC_MCPServerConfig& existing : LC_MCPManager::instance().servers()) {
            if (existing.name == m_editing) {
                c = existing; // keeps fields the form does not show (timeout, enabled)
            }
        }
    }
    c.name = m_name->text().trimmed();
    c.http = m_http;
    c.command = m_command->text().trimmed();
    c.args = QProcess::splitCommand(m_args->text());
    c.cwd = m_cwd->text().trimmed();
    c.env = parsePairs(m_env->toPlainText(), '=');
    c.url = m_url->text().trimmed();
    c.headers = parsePairs(m_headers->toPlainText(), ':');
    c.description = m_description->text().trimmed();
    if (c.http) {
        c.command.clear();
        c.args.clear();
        c.cwd.clear();
        c.env.clear();
    }
    else {
        c.url.clear();
        c.headers.clear();
    }
    const QString error = LC_MCPManager::instance().saveServer(c, m_editing);
    if (!error.isEmpty()) {
        m_formError->setText(error);
        m_formError->show();
        return;
    }
    hideForm();
}

void LC_AIMcpDialog::importFile() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Import MCP servers"), QDir::homePath(),
                                                      tr("MCP config (*.json);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QStringList added;
    const QString error = LC_MCPManager::instance().importFile(path, added);
    m_notice->setText(error.isEmpty() ? tr("Imported: %1").arg(added.join(", ")) : error);
    m_notice->setStyleSheet(error.isEmpty() ? QString() : QStringLiteral("color: #f87171;"));
    m_notice->show();
    showJson(false);
}
