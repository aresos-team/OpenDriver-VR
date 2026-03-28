#include "plugin_model.h"
#include <QMessageBox>

namespace opendriver::ui {

PluginModel::PluginModel(opendriver::core::Runtime* runtime, QObject* parent)
    : QAbstractTableModel(parent), m_runtime(runtime) {
    refresh();
}

int PluginModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(m_plugins.size());
}

int PluginModel::columnCount(const QModelIndex& parent) const {
    return 5; // Enabled, Name, Version, Status, Logs
}

QVariant PluginModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return QVariant();
    
    switch (section) {
        case 0: return "Enabled";
        case 1: return "Plugin Name";
        case 2: return "Version";
        case 3: return "Status";
        case 4: return "Logs";
        default: return QVariant();
    }
}

QVariant PluginModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= (int)m_plugins.size()) return QVariant();

    const auto& p = m_plugins[index.row()];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case 1: return QString::fromStdString(p.name);
            case 2: return QString::fromStdString(p.version);
            case 3: return p.is_loaded ? "Active" : "Inactive";
            case 4: return "Show Logs";
        }
    } else if (role == Qt::CheckStateRole && index.column() == 0) {
        return p.is_loaded ? Qt::Checked : Qt::Unchecked;
    }

    return QVariant();
}

bool PluginModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (index.isValid() && role == Qt::CheckStateRole && index.column() == 0) {
        bool enable = (value.toInt() == Qt::Checked);
        
        QMessageBox::StandardButton reply;
        reply = QMessageBox::warning(nullptr, "Warning: SteamVR Stability",
            "Przełączanie wtyczek w locie może spowodować crash środowiska SteamVR (zwłaszcza dla wtyczek HMD). "
            "Kliknij Apply by zatwierdzić ryzyko lub Cancel, aby anulować.",
            QMessageBox::Apply | QMessageBox::Cancel);
            
        if (reply == QMessageBox::Apply) {
            if (enable) {
                m_runtime->EnablePlugin(m_plugins[index.row()].name);
            } else {
                m_runtime->DisablePlugin(m_plugins[index.row()].name);
            }
            refresh();
            return true;
        } else {
            return false;
        }
    }
    return false;
}

Qt::ItemFlags PluginModel::flags(const QModelIndex& index) const {
    if (index.column() == 0) return Qt::ItemIsEnabled | Qt::ItemIsUserCheckable;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

void PluginModel::refresh() {
    beginResetModel();
    m_runtime->ScanPlugins();
    m_plugins = m_runtime->GetAvailablePlugins();
    endResetModel();
}

} // namespace opendriver::ui
