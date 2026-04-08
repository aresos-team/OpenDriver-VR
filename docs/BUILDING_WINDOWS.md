# Budowanie OpenDriver na Windows

## Wymagania

### Narzędzia (zainstaluj w tej kolejności)

1. **Visual Studio 2022** (Community lub wyżej)
   - Workload: **Desktop development with C++**
   - W opcjach zaznacz też: **C++ CMake tools for Windows**
   - Pobierz: https://visualstudio.microsoft.com/

2. **Git for Windows** — https://git-scm.com/download/win

3. **CMake 3.16+** — https://cmake.org/download/  
   (zaznacz "Add CMake to PATH" w instalatorze)

4. **Qt6** (opcjonalnie, tylko jeśli chcesz GUI dashboard)
   - Pobierz Qt Online Installer: https://www.qt.io/download-qt-installer
   - Zainstaluj: **Qt 6.x → MSVC 2019 64-bit**

5. **vcpkg** (do zarządzania zależnościami Windows)
   ```bat
   git clone https://github.com/microsoft/vcpkg %USERPROFILE%\vcpkg
   cd %USERPROFILE%\vcpkg
   bootstrap-vcpkg.bat
   vcpkg integrate install
   ```

---

## Kroki kompilacji

### 1. Sklonuj repo

```bat
git clone <adres-repo> C:\opendriver
cd C:\opendriver
```

### 2. Pobierz submodule OpenVR SDK

```bat
git submodule update --init --recursive
```
lub ręcznie:
```bat
winget install ValveSoftware.Steam          REM jeśli nie masz SDK
mkdir openvr\headers
:: skopiuj headery z Steam SDK lub: https://github.com/ValveSoftware/openvr/tree/master/headers
```

### 3. Konfiguracja CMake (w Developer Command Prompt for VS 2022)

```bat
:: Otwórz: Start → Visual Studio 2022 → Developer Command Prompt

cd C:\opendriver

cmake -B build ^
  -G "Visual Studio 17 2022" ^
  -A x64 ^
  -DCMAKE_BUILD_TYPE=Release
```

#### Z Qt6 (GUI):
```bat
cmake -B build ^
  -G "Visual Studio 17 2022" ^
  -A x64 ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DQt6_DIR="C:\Qt\6.8.0\msvc2019_64\lib\cmake\Qt6"
```

### 4. Kompilacja

```bat
cmake --build build --config Release --parallel
```

Lub otwórz `build\opendriver.sln` w Visual Studio i naciśnij **Ctrl+Shift+B**.

### 5. Wyniki kompilacji

Po kompilacji w `build\Release\` znajdziesz:

| Plik | Co to |
|------|-------|
| `driver_opendriver.dll` | Sterownik SteamVR (załadowany przez vrserver) |
| `opendriver_runner.exe` | Runtime z pluginami |
| `opendriver_gui.exe` | Dashboard Qt6 (jeśli Qt6 zainstalowane) |

---

## Rejestracja sterownika w SteamVR

### Opcja A — przez openvrpaths.vrpath

Edytuj `%LOCALAPPDATA%\openvr\openvrpaths.vrpath` i dodaj ścieżkę:

```json
{
  "external_drivers": [
    "C:\\opendriver\\steamvr_driver"
  ]
}
```

### Opcja B — przez SteamVR

1. Otwórz SteamVR
2. Menu → Settings → Manage Add-Ons
3. Dodaj ścieżkę do folderu `steamvr_driver\`

### Struktura folderu sterownika SteamVR

```
steamvr_driver\
  driver.vrdrivermanifest     ← wymagany plik
  bin\win64\
    driver_opendriver.dll     ← twój .dll
    opendriver_runner.exe     ← runtime
```

Plik `driver.vrdrivermanifest` (utwórz ręcznie):
```json
{
  "alwaysActivate": false,
  "name": "opendriver",
  "directory": "",
  "resourceOnly": false,
  "hmd_presence": []
}
```

---

## Co działa na Windows vs Linux

| Funkcja | Linux | Windows |
|---------|-------|---------|
| Runner (core, pluginy) | ✅ | ✅ |
| IPC (Named Pipes) | ✅ Unix socket | ✅ Named Pipes |
| GUI Dashboard (Qt6) | ✅ | ✅ |
| Rejestracja HMD w SteamVR | ✅ | ✅ |
| Plugin system (LoadLibrary) | ✅ dlopen | ✅ LoadLibrary |
| Video enkodowanie (H264) | ✅ x264 + DMABUF | ⚠️ Stub (patrz niżej) |
| DRM lease shim | ✅ LD_PRELOAD | N/A (nie potrzebne) |

### ⚠️ Video na Windows — obecny stan

Na Windows **Present() zwraca natychmiast** (stub) — SteamVR podaje ramki jako `D3D11 shared texture handle`, nie jako `DMABUF fd`. Żeby zaimplementować enkodowanie na Windows potrzeba:

1. `ID3D11Device` + `ID3D11Texture2D::Map()` zamiast `mmap(DMABUF)`
2. Enkoder: **NVENC** (NVIDIA) lub **MFT** (Media Foundation / Windows wbudowane)
3. opcjonalnie: **AMF** (AMD) lub **QuickSync** (Intel)

To jest zaplanowane jako następny krok dla pełnego portu Windows.

---

## Debugowanie

### Logi SteamVR (Windows)
```
%LOCALAPPDATA%\Steam\logs\vrserver.txt
```

### Logi OpenDriver Runner
```
%APPDATA%\opendriver\opendriver.log
```

### Uruchomienie runnera ręcznie (do testowania bez SteamVR)
```bat
build\Release\opendriver_runner.exe
```

---

## Znane problemy Windows

1. **`driver.vrdrivermanifest` jest wymagany** — SteamVR nie załaduje DLL bez tego pliku
2. **MSVC runtime** — `driver_opendriver.dll` wymaga `vcruntime140.dll` — zainstaluj Visual C++ Redistributable
3. **AntiVirus** może blokować `opendriver_runner.exe` — dodaj wyjątek
