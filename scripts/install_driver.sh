#!/bin/bash

# OpenDriver - SteamVR Driver Installer (Linux)

DRIVER_NAME="opendriver"
# Ustawienie ROOT projektu na podstawie lokalizacji skryptu
SCRIPT_DIR=$(dirname "$(realpath "$0")")
PROJECT_ROOT=$(realpath "$SCRIPT_DIR/..")
DRIVER_PATH="$PROJECT_ROOT/drivers/$DRIVER_NAME"
BINARY_SOURCE="$PROJECT_ROOT/build/driver_opendriver.so"
BINARY_DEST="$DRIVER_PATH/bin/linux64/driver_opendriver.so"

echo "=== OpenDriver SteamVR Installer ==="

# 1. Sprawdzenie czy binarka istnieje
if [ ! -f "$BINARY_SOURCE" ]; then
    echo "ERROR: driver_opendriver.so not found in build directory!"
    echo "Please build the project first."
    exit 1
fi

# 2. Skopiowanie binarki do struktury sterownika
echo "Copying driver binary..."
mkdir -p "$DRIVER_PATH/bin/linux64"
cp "$BINARY_SOURCE" "$BINARY_DEST"

# 3. Wykrycie ścieżki SteamVR i folderu sterowników
STEAMVR_DIR=""
# Pobieramy prawdziwy dom użytkownika, nawet jeśli skrypt leci przez sudo
USER_HOME=$(getent passwd "$SUDO_USER" | cut -d: -f6)
if [ -z "$USER_HOME" ]; then USER_HOME=$HOME; fi

POSSIBLE_PATHS=(
    "$USER_HOME/.steam/steam/steamapps/common/SteamVR"
    "$USER_HOME/.local/share/Steam/steamapps/common/SteamVR"
    "$USER_HOME/.steam/debian-installation/steamapps/common/SteamVR"
    "/home/$SUDO_USER/.local/share/Steam/steamapps/common/SteamVR"
)

for path in "${POSSIBLE_PATHS[@]}"; do
    if [ -d "$path" ]; then
        STEAMVR_DIR="$path"
        break
    fi
done

if [ -z "$STEAMVR_DIR" ]; then
    echo "ERROR: SteamVR installation not found!"
    exit 1
fi

STEAM_DRIVERS_DIR="$STEAMVR_DIR/drivers"
DEST_DRIVER_DIR="$STEAM_DRIVERS_DIR/$DRIVER_NAME"

echo "Using SteamVR at: $STEAMVR_DIR"
echo "Target drivers directory: $DEST_DRIVER_DIR"

# 4. Czyszczenie starego sterownika (jeśli był przez vrpathreg)
VRPATHREG=$(find "$STEAMVR_DIR" -name vrpathreg -type f | head -n 1)
if [ -n "$VRPATHREG" ]; then
    echo "Cleaning up old vrpathreg entries..."
    "$VRPATHREG" removedriver "$DRIVER_PATH" 2>/dev/null
fi

# 5. Fizyczne kopiowanie sterownika do SteamVR
echo "Installing driver to SteamVR..."
mkdir -p "$DEST_DRIVER_DIR/bin/linux64"
mkdir -p "$DEST_DRIVER_DIR/resources"

rm -f "$DEST_DRIVER_DIR/bin/linux64/driver_opendriver.so"
rm -f "$DEST_DRIVER_DIR/bin/linux64/opendriver_gui"

cp "$BINARY_SOURCE" "$DEST_DRIVER_DIR/bin/linux64/driver_opendriver.so"
cp "$PROJECT_ROOT/build/opendriver_gui" "$DEST_DRIVER_DIR/bin/linux64/opendriver_gui"
cp "$PROJECT_ROOT/drivers/opendriver/driver.vrdrivermanifest" "$DEST_DRIVER_DIR/"
cp "$PROJECT_ROOT/drivers/opendriver/driver.vrresources" "$DEST_DRIVER_DIR/"
cp -r "$PROJECT_ROOT/drivers/opendriver/resources"/* "$DEST_DRIVER_DIR/resources/" 2>/dev/null

# 5b. Instalacja systemowa dla PATH (pomaga drajwerowi znaleźć GUI jeśli nie ma go w SteamVR bin)
echo "Installing GUI to system (/usr/local/bin)..."
sudo cp "$PROJECT_ROOT/build/opendriver_gui" /usr/local/bin/opendriver_gui 2>/dev/null && \
    sudo chmod +x /usr/local/bin/opendriver_gui 2>/dev/null && \
    echo "Success: GUI installed to /usr/local/bin" || \
    echo "Notice: Could not install to /usr/local/bin (no sudo or permission). It will still work from SteamVR folder."

# 6. Przygotowanie folderu DIY w Home (Konfiguracja + Pluginy)
CONFIG_DIR="$USER_HOME/.config/opendriver"
mkdir -p "$CONFIG_DIR/plugins"
echo "DIY workspace ensured at: $CONFIG_DIR"

echo "=== Installation Successful! ==="
echo "OpenDriver is now a native SteamVR driver."
echo "Your DIY plugins go here: $CONFIG_DIR/plugins"
echo "Restart SteamVR to load the driver."
