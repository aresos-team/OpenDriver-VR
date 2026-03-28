cmake_minimum_required(VERSION 3.16)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
project(opendriver)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Qt6 Configuration
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 COMPONENTS Widgets REQUIRED)

# ────────────────────────────────────────────────────────────────────
# DEPENDENCIES
# ────────────────────────────────────────────────────────────────────

include(FetchContent)
set(FETCHCONTENT_QUIET OFF) # Show download progress

# nlohmann_json
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.2
    GIT_PROGRESS TRUE
    GIT_SHALLOW TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
message(STATUS "Fetching nlohmann_json...")
FetchContent_MakeAvailable(json)

# spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.11.0
    GIT_PROGRESS TRUE
    GIT_SHALLOW TRUE
)
message(STATUS "Fetching spdlog...")
FetchContent_MakeAvailable(spdlog)

# OpenVR (Using headers from openvr/headers)
set(OPENVR_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/openvr/headers)
if(NOT EXISTS ${OPENVR_INCLUDE_DIR}/openvr.h)
    message(WARNING "OpenVR headers NOT FOUND in ${OPENVR_INCLUDE_DIR}. Please run: git submodule update --init --recursive")
endif()

# ────────────────────────────────────────────────────────────────────
# CORE LIBRARY
# ────────────────────────────────────────────────────────────────────

set(CORE_SOURCES
    src/core/event_bus.cpp
    src/core/device_registry.cpp
    src/core/logger.cpp
    src/core/config_manager.cpp
    src/core/plugin_loader.cpp
    src/core/runtime.cpp
    src/core/ipc_linux.cpp
)

add_library(opendriver_core STATIC ${CORE_SOURCES})

target_include_directories(opendriver_core PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${OPENVR_INCLUDE_DIR}
)

target_link_libraries(opendriver_core PUBLIC 
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    dl
)

# ────────────────────────────────────────────────────────────────────
# EXECUTABLE (Test/Runner)
# ────────────────────────────────────────────────────────────────────

add_executable(opendriver_runner src/main.cpp)
target_link_libraries(opendriver_runner PRIVATE opendriver_core)

# ────────────────────────────────────────────────────────────────────
# STEAMVR DRIVER (The Bridge)
# ────────────────────────────────────────────────────────────────────
add_library(driver_opendriver SHARED src/driver/driver_opendriver.cpp)
target_include_directories(driver_opendriver PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/include 
    ${OPENVR_INCLUDE_DIR}
)
target_link_libraries(driver_opendriver PRIVATE opendriver_core)
set_target_properties(driver_opendriver PROPERTIES PREFIX "")

# ────────────────────────────────────────────────────────────────────
# GUI DASHBOARD (Qt6)
# ────────────────────────────────────────────────────────────────────
set(GUI_SOURCES
    src/ui/main.cpp
    src/ui/main_window.cpp
    src/ui/device_model.cpp
    src/ui/plugin_model.cpp
)

add_executable(opendriver_gui ${GUI_SOURCES})
target_link_libraries(opendriver_gui PRIVATE 
    opendriver_core 
    Qt6::Widgets
)

# ────────────────────────────────────────────────────────────────────
