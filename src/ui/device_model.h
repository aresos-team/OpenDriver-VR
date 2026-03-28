#pragma once

#include <QAbstractListModel>
#include <opendriver/core/device_registry.h>

namespace opendriver::ui {

/**
 * @brief Model dla listy urządzeń z DeviceRegistry, umożliwiający ich wyświetlenie w QListView.
 */
class DeviceModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit DeviceModel(opendriver::core::DeviceRegistry* registry, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    
    void refresh();

private:
    opendriver::core::DeviceRegistry* m_registry;
};

} // namespace opendriver::ui
