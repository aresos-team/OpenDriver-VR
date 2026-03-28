#pragma once

class QWidget;

namespace opendriver::core {

/**
 * @brief Interfejs pozwalający pluginom na dostarczenie własnych widżetów do Dashboardu.
 */
class IUIProvider {
public:
    virtual ~IUIProvider() = default;

    /**
     * @brief Tworzy widżet ustawień/statystyk pluginu.
     * @param parent Rodzic widżetu (zwykle QTabWidget lub QScrollArea).
     * @return Wskaźnik do nowego widżetu.
     */
    virtual QWidget* CreateSettingsWidget(QWidget* parent) = 0;

    /**
     * @brief Wywoływane okresowo przez Dashboard, aby plugin mógł odświeżyć statystyki na widżecie.
     */
    virtual void RefreshUI() = 0;
};

} // namespace opendriver::core
