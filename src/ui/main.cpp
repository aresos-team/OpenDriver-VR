#include <QApplication>
#include "main_window.h"
#include <opendriver/core/runtime.h>
#include <string>
#include <cstdlib>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    // Inicjalizacja silnika w tle
    auto& runtime = opendriver::core::Runtime::GetInstance();
    
    const char* home_env = std::getenv("HOME");
    std::string config_dir;
    if (home_env) {
        config_dir = std::string(home_env) + "/.config/opendriver";
    } else {
        config_dir = "test_config";
    }

    if (!runtime.Initialize(config_dir)) {
        return 1;
    }

    opendriver::ui::MainWindow w(&runtime);
    w.show();

    int result = app.exec();
    
    runtime.Shutdown();
    return result;
}
