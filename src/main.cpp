#include <opendriver/core/runtime.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <string>
#include <cstdlib>

using namespace opendriver::core;
namespace fs = std::filesystem;

int main() {
    const char* home_env = std::getenv("HOME");
    std::string config_dir;
    if (home_env) {
        config_dir = std::string(home_env) + "/.config/opendriver";
    } else {
        config_dir = (fs::current_path() / "config").string();
    }
    
    std::cout << "Starting OpenDriver Runtime in " << config_dir << std::endl;
    
    if (!Runtime::GetInstance().Initialize(config_dir)) {
        std::cerr << "Failed to initialize runtime" << std::endl;
        return 1;
    }
    
    // Create dummy plugin folder and copy it there if it exists
    std::string plugins_dir = (fs::path(config_dir) / "plugins" / "dummy").string();
    fs::create_directories(plugins_dir);
    
    // Note: In a real test, CMake would build dummy_plugin.so and we'd copy it here.
    // For now, let's just test the logic.
    
    std::cout << "Runtime initialized. Press Ctrl+C to stop." << std::endl;
    
    auto start = std::chrono::steady_clock::now();
    while (true) {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - start).count();
        start = now;
        
        Runtime::GetInstance().Tick(dt);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    Runtime::GetInstance().Shutdown();
    std::cout << "Runtime stopped." << std::endl;
    
    return 0;
}
