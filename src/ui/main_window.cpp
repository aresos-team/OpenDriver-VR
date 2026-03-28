#include "main_window.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTabWidget>
#include <QPushButton>
#include <QListView>
#include <QTableView>
#include <QLineEdit>
#include <QHeaderView>
#include <QSplitter>
#include <QTextEdit>
#include <opendriver/core/logger.h>
#include <QDateTime>
#include <QMessageBox>

namespace opendriver::ui {

MainWindow::MainWindow(opendriver::core::Runtime* runtime, QWidget* parent)
    : QMainWindow(parent), m_runtime(runtime) {
    
    m_deviceModel = new DeviceModel(&m_runtime->GetDeviceRegistry(), this);
    m_pluginModel = new PluginModel(m_runtime, this);
    
    setupUI();
    
    // Subskrypcja logów
    opendriver::core::Logger::GetInstance().AddListener([this](const opendriver::core::LogEntry& entry){
        // Używamy invokeMethod aby zapewnić bezpieczeństwo wątkowe (logi przychodzą z różnych wątków)
        QMetaObject::invokeMethod(this, [this, entry](){
            this->onLogReceived(entry);
        }, Qt::QueuedConnection);
    });

    m_refreshTimer = new QTimer(this);
    connect(m_refreshTimer, &QTimer::timeout, this, &MainWindow::onRefreshTimer);
    m_refreshTimer->start(500); // 2Hz wystarczy dla listy
}

MainWindow::~MainWindow() {}

void MainWindow::setupUI() {
    setWindowTitle("OpenDriver Dashboard");
    resize(900, 700);

    auto* central = new QWidget(this);
    auto* mainLayout = new QVBoxLayout(central);

    // Nagłówek
    auto* header = new QLabel("OpenDriver Core Runtime", this);
    header->setStyleSheet("font-size: 20px; font-weight: bold; color: #2c3e50; margin: 10px;");
    mainLayout->addWidget(header);

    // Zakładki
    auto* tabs = new QTabWidget(this);
    mainLayout->addWidget(tabs);

    // --- KARTA: OVERVIEW ---
    auto* overviewTab = new QWidget();
    auto* overviewLayout = new QVBoxLayout(overviewTab);
    overviewLayout->addWidget(new QLabel("<b>Connected Devices:</b>"));
    m_deviceListView = new QListView(this);
    m_deviceListView->setModel(m_deviceModel);
    overviewLayout->addWidget(m_deviceListView);
    tabs->addTab(overviewTab, "Overview");

    // --- KARTA: PLUGIN MANAGER ---
    auto* pluginTab = new QWidget();
    auto* pluginLayout = new QVBoxLayout(pluginTab);
    
    // Toolbar
    auto* toolbar = new QHBoxLayout();
    m_searchEdit = new QLineEdit();
    m_searchEdit->setPlaceholderText("Search plugins...");
    toolbar->addWidget(m_searchEdit);
    
    auto* btnEnableAll = new QPushButton("Enable All");
    auto* btnDisableAll = new QPushButton("Disable All");
    toolbar->addWidget(btnEnableAll);
    toolbar->addWidget(btnDisableAll);
    pluginLayout->addLayout(toolbar);

    // Splitter dla tabeli i logów
    auto* splitter = new QSplitter(Qt::Vertical, this);
    pluginLayout->addWidget(splitter);

    // Tabela pluginów
    m_pluginTableView = new QTableView();
    m_pluginTableView->setModel(m_pluginModel);
    m_pluginTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_pluginTableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pluginTableView->setSelectionMode(QAbstractItemView::SingleSelection);
    splitter->addWidget(m_pluginTableView);
    
    // Panel Logów (Kontener)
    auto* logContainer = new QWidget();
    auto* logLayout = new QVBoxLayout(logContainer);
    logLayout->setContentsMargins(0, 5, 0, 0);
    
    auto* logHeader = new QHBoxLayout();
    m_logStatusLabel = new QLabel("<b>Logs:</b> Showing all", this);
    auto* btnClearLogs = new QPushButton("Clear Logs", this);
    logHeader->addWidget(m_logStatusLabel);
    logHeader->addStretch();
    logHeader->addWidget(btnClearLogs);
    logLayout->addLayout(logHeader);

    m_logView = new QTextEdit();
    m_logView->setReadOnly(true);
    m_logView->setPlaceholderText("Select a plugin to see its logs...");
    m_logView->setStyleSheet("background-color: #1e1e1e; color: #d4d4d4; font-family: 'Courier New';");
    logLayout->addWidget(m_logView);
    splitter->addWidget(logContainer);

    // Toolbar (Refresh)
    auto* btnRefresh = new QPushButton("Refresh Plugins");
    toolbar->addWidget(btnRefresh);
    
    tabs->addTab(pluginTab, "Plugins");

    connect(btnRefresh, &QPushButton::clicked, this, &MainWindow::onRefreshPlugins);
    connect(btnClearLogs, &QPushButton::clicked, m_logView, &QTextEdit::clear);
    connect(m_pluginTableView, &QTableView::clicked, this, &MainWindow::onPluginTableClicked);
    connect(m_pluginTableView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::onPluginSelected);

    // Dynamiczne zakładki z pluginów (opcje pluginów)
    // UWAGA: Te zakładki mogą się pojawiać/znikać w onRefreshTimer
    
    connect(btnEnableAll, &QPushButton::clicked, this, &MainWindow::onEnableAll);
    connect(btnDisableAll, &QPushButton::clicked, this, &MainWindow::onDisableAll);

    setCentralWidget(central);
}

void MainWindow::onRefreshTimer() {
    m_deviceModel->refresh();
    m_pluginModel->refresh();

    // Można tu dodać dynamiczne dodawanie zakładek jeśli plugin został włączony
}

void MainWindow::onEnableAll() {
    auto reply = QMessageBox::warning(this, "Warning: SteamVR Stability",
        "Włączenie wszystkich wtyczek w locie może uszkodzić sesję SteamVR. "
        "Kontynuować (Apply)?",
        QMessageBox::Apply | QMessageBox::Cancel);
    if (reply == QMessageBox::Apply) {
        m_runtime->SetAllPluginsState(true);
        m_pluginModel->refresh();
    }
}

void MainWindow::onDisableAll() {
    auto reply = QMessageBox::warning(this, "Warning: SteamVR Stability",
        "Wyłączenie wszystkich wtyczek w locie może uszkodzić sesję SteamVR. "
        "Kontynuować (Apply)?",
        QMessageBox::Apply | QMessageBox::Cancel);
    if (reply == QMessageBox::Apply) {
        m_runtime->SetAllPluginsState(false);
        m_pluginModel->refresh();
    }
}

void MainWindow::onRefreshPlugins() {
    m_runtime->ReloadPlugins();
    m_pluginModel->refresh();
}

void MainWindow::onPluginSelected(const QItemSelection& selected, const QItemSelection& /*deselected*/) {
    if (selected.isEmpty()) {
        m_selectedPlugin = "";
        m_logStatusLabel->setText("<b>Logs:</b> Showing all");
        return;
    }
    
    int row = selected.indexes().first().row();
    m_selectedPlugin = m_pluginModel->index(row, 1).data().toString().toStdString(); // Index 1 is name
    
    m_logStatusLabel->setText(QString("<b>Logs:</b> Filtering by [%1]").arg(QString::fromStdString(m_selectedPlugin)));
    m_logView->append(QString("<br><i style='color: #6272a4'>--- Showing logs for [%1] ---</i>").arg(QString::fromStdString(m_selectedPlugin)));
}

void MainWindow::onPluginTableClicked(const QModelIndex& index) {
    if (index.column() == 4) { // Kolumna "Logs"
        m_pluginTableView->selectRow(index.row());
        m_logView->setFocus();
    }
}

void MainWindow::onLogReceived(const opendriver::core::LogEntry& entry) {
    // Filtrowanie (lub pokazuj wszystko jeśli nic nie wybrano)
    if (!m_selectedPlugin.empty() && entry.source != m_selectedPlugin && entry.source != "core") {
        return;
    }

    QString timeStr = QDateTime::fromMSecsSinceEpoch(entry.timestamp).toString("hh:mm:ss.zzz");
    QString color = "#d4d4d4"; // default
    
    if (entry.level >= opendriver::core::LogLevel::ERROR) color = "#ff5555";
    else if (entry.level == opendriver::core::LogLevel::WARN) color = "#ffb86c";

    QString html = QString("<span style='color: #6272a4'>[%1]</span> <span style='color: %2'><b>[%3]</b> %4</span>")
        .arg(timeStr)
        .arg(color)
        .arg(QString::fromStdString(entry.source))
        .arg(QString::fromStdString(entry.message).toHtmlEscaped());

    m_logView->append(html);
    
    // Auto-scroll
    m_logView->moveCursor(QTextCursor::End);
}

} // namespace opendriver::ui
