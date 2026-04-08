#pragma once

#include <QMainWindow>
#include <QTimer>
#include <opendriver/core/runtime.h>
#include "device_model.h"
#include "plugin_model.h"

class QListView;
class QTableView;
class QLineEdit;
class QTextEdit;
class QLabel;
class QItemSelection;
class QSpinBox;
class QComboBox;
class QSlider;
class QCheckBox;

namespace opendriver::ui {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(opendriver::core::Runtime* runtime, QWidget* parent = nullptr);
    ~MainWindow();

private slots:
    void onRefreshTimer();
    void onRefreshPlugins();
    void onEnableAll();
    void onDisableAll();
    void onPluginSelected(const QItemSelection& selected, const QItemSelection& deselected);
    void onLogReceived(const opendriver::core::LogEntry& entry);
    void onPluginTableClicked(const QModelIndex& index);
    
    // Video encoding settings
    void onBitrateChanged(int value);
    void onEncoderTypeChanged(int index);
    void onQualityPresetChanged(int index);
    void onApplyVideoSettings();
    void onLoadVideoSettings();

private:
    opendriver::core::Runtime* m_runtime;
    QTimer* m_refreshTimer;
    
    // Models
    DeviceModel* m_deviceModel;
    PluginModel* m_pluginModel;
    
    // Views
    QListView* m_deviceListView;
    QTableView* m_pluginTableView;
    QLabel* m_logStatusLabel;
    QTextEdit* m_logView;
    QLineEdit* m_searchEdit;
    
    std::string m_selectedPlugin;
    
    // Video encoding UI
    QSpinBox* m_bitrateSpinBox;
    QComboBox* m_encoderTypeCombo;
    QComboBox* m_qualityPresetCombo;
    QLabel* m_bitrateValueLabel;
    QLabel* m_encodingStatsLabel;
    
    void setupUI();
};

} // namespace opendriver::ui
