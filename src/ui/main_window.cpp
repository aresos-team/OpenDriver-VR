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
#include <QSpinBox>
#include <QComboBox>
#include <QSlider>
#include <QCheckBox>
#include <QGroupBox>
#include <opendriver/core/logger.h>
#include <opendriver/core/platform.h>
#include <QDateTime>
#include <QMessageBox>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <cstdlib>

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

    // --- KARTA: VIDEO ENCODING SETTINGS ---
    auto* videoTab = new QWidget();
    auto* videoLayout = new QVBoxLayout(videoTab);
    
    // Video encoding group
    auto* videoGroup = new QGroupBox("H.264 Video Encoding", videoTab);
    auto* videoGroupLayout = new QVBoxLayout(videoGroup);
    
    // Encoder type
    auto* encoderLayout = new QHBoxLayout();
    encoderLayout->addWidget(new QLabel("Encoder Type:"));
    m_encoderTypeCombo = new QComboBox();
    m_encoderTypeCombo->addItem("H.264 (x264) - Recommended");
    m_encoderTypeCombo->addItem("H.265 (x265) - Lower bandwidth");
    m_encoderTypeCombo->setEnabled(true);
    encoderLayout->addWidget(m_encoderTypeCombo);
    encoderLayout->addStretch();
    videoGroupLayout->addLayout(encoderLayout);
    
    // Quality preset
    auto* qualityLayout = new QHBoxLayout();
    qualityLayout->addWidget(new QLabel("Quality Preset:"));
    m_qualityPresetCombo = new QComboBox();
    m_qualityPresetCombo->addItem("ultrafast (Lowest latency)");
    m_qualityPresetCombo->addItem("superfast");
    m_qualityPresetCombo->addItem("veryfast");
    m_qualityPresetCombo->addItem("faster");
    m_qualityPresetCombo->setCurrentIndex(0);  // Default ultrafast
    qualityLayout->addWidget(m_qualityPresetCombo);
    qualityLayout->addStretch();
    videoGroupLayout->addLayout(qualityLayout);
    
    // Bitrate control
    auto* bitrateLayout = new QHBoxLayout();
    bitrateLayout->addWidget(new QLabel("Target Bitrate (Mbps):"));
    m_bitrateSpinBox = new QSpinBox();
    m_bitrateSpinBox->setMinimum(5);
    m_bitrateSpinBox->setMaximum(100);
    m_bitrateSpinBox->setValue(30);
    m_bitrateSpinBox->setSingleStep(1);
    m_bitrateSpinBox->setPrefix("");
    m_bitrateSpinBox->setSuffix(" Mbps");
    bitrateLayout->addWidget(m_bitrateSpinBox);
    
    m_bitrateValueLabel = new QLabel("(30 Mbps)");
    bitrateLayout->addWidget(m_bitrateValueLabel);
    bitrateLayout->addStretch();
    videoGroupLayout->addLayout(bitrateLayout);
    
    // Bitrate slider
    auto* sliderLayout = new QHBoxLayout();
    auto* slider = new QSlider(Qt::Horizontal);
    slider->setMinimum(5);
    slider->setMaximum(100);
    slider->setValue(30);
    sliderLayout->addWidget(slider);
    videoGroupLayout->addLayout(sliderLayout);
    
    connect(m_bitrateSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onBitrateChanged);
    connect(slider, &QSlider::valueChanged, m_bitrateSpinBox, &QSpinBox::setValue);
    connect(m_bitrateSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), slider, &QSlider::setValue);
    
    // Encoding stats
    m_encodingStatsLabel = new QLabel("Stats: Waiting for data...");
    m_encodingStatsLabel->setStyleSheet("background-color: #f0f0f0; padding: 10px; border-radius: 5px;");
    videoGroupLayout->addWidget(m_encodingStatsLabel);
    
    // Apply button
    auto* btnApply = new QPushButton("Apply Settings");
    connect(btnApply, &QPushButton::clicked, this, &MainWindow::onApplyVideoSettings);
    videoGroupLayout->addWidget(btnApply);
    
    videoGroupLayout->addStretch();
    videoLayout->addWidget(videoGroup);
    
    tabs->addTab(videoTab, "Video Encoding");

    setCentralWidget(central);
    
    // Load initial video settings
    onLoadVideoSettings();
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

void MainWindow::onBitrateChanged(int value) {
    m_bitrateValueLabel->setText(QString("(%1 Mbps)").arg(value));
}

void MainWindow::onEncoderTypeChanged(int index) {
    // Future: switch between x264 and x265
}

void MainWindow::onQualityPresetChanged(int index) {
    // Future: adjust encoding preset dynamically
}

void MainWindow::onLoadVideoSettings() {
    try {
        namespace fs = std::filesystem;
        std::string config_dir  = opendriver::core::GetDefaultConfigDir();
        std::string config_file = config_dir + "/config.json";
        
        if (fs::exists(config_file)) {
            std::ifstream f(config_file);
            nlohmann::json j = nlohmann::json::parse(f);
            
            if (j.contains("video_encoding")) {
                auto& ve = j["video_encoding"];
                int bitrate = ve.value("bitrate_mbps", 30);
                m_bitrateSpinBox->setValue(bitrate);
                
                std::string encoder = ve.value("encoder", "h264");
                if (encoder == "h265") {
                    m_encoderTypeCombo->setCurrentIndex(1);
                } else {
                    m_encoderTypeCombo->setCurrentIndex(0);
                }
                
                std::string preset = ve.value("preset", "ultrafast");
                int presetIdx = 0;
                if (preset == "superfast") presetIdx = 1;
                else if (preset == "veryfast") presetIdx = 2;
                else if (preset == "faster") presetIdx = 3;
                m_qualityPresetCombo->setCurrentIndex(presetIdx);
            }
        }
    } catch (const std::exception& e) {
        // Ignore errors - use defaults
    }
}

void MainWindow::onApplyVideoSettings() {
    try {
        namespace fs = std::filesystem;
        std::string config_dir  = opendriver::core::GetDefaultConfigDir();
        std::string config_file = config_dir + "/config.json";
        
        // Load existing config or create new
        nlohmann::json j;
        if (fs::exists(config_file)) {
            std::ifstream f(config_file);
            j = nlohmann::json::parse(f);
        }
        
        // Update video encoding settings
        j["video_encoding"]["bitrate_mbps"] = m_bitrateSpinBox->value();
        j["video_encoding"]["encoder"] = (m_encoderTypeCombo->currentIndex() == 0) ? "h264" : "h265";
        
        // Map preset index to name
        const char* presets[] = {"ultrafast", "superfast", "veryfast", "faster"};
        j["video_encoding"]["preset"] = presets[m_qualityPresetCombo->currentIndex()];
        
        // Ensure directory exists
        if (!fs::exists(config_dir)) {
            fs::create_directories(config_dir);
        }
        
        // Write config
        std::ofstream f(config_file);
        f << j.dump(2);
        f.close();
        
        QMessageBox::information(this, "Success", 
            QString("Video settings saved!\nBitrate: %1 Mbps\nPreset: %2")
            .arg(m_bitrateSpinBox->value())
            .arg(m_qualityPresetCombo->currentText()));
        
        // Logging
        opendriver::core::Logger::GetInstance().Info("UI", 
            "Video settings updated: bitrate=" + std::to_string(m_bitrateSpinBox->value()) + " Mbps");
    } catch (const std::exception& e) {
        QMessageBox::critical(this, "Error", QString("Failed to save settings: %1").arg(e.what()));
        opendriver::core::Logger::GetInstance().Error("UI", std::string("Failed to save video settings: ") + e.what());
    }
}

} // namespace opendriver::ui
