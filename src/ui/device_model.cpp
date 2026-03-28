#include "device_model.h"

namespace opendriver::ui {

DeviceModel::DeviceModel(opendriver::core::DeviceRegistry* registry, QObject* parent)
    : QAbstractListModel(parent), m_registry(registry) {}

int DeviceModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(m_registry->GetAll().size());
}

QVariant DeviceModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return QVariant();

    auto devices = m_registry->GetAll();
    if (index.row() >= static_cast<int>(devices.size())) return QVariant();

    const auto* device = devices[index.row()];

    if (role == Qt::DisplayRole) {
        return QString("[%1] %2").arg(QString::fromStdString(device->id), QString::fromStdString(device->name));
    }

    return QVariant();
}

void DeviceModel::refresh() {
    beginResetModel();
    endResetModel();
}

} // namespace opendriver::ui
