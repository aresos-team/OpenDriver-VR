# OpenDriver 1.2

Ten plik opisuje stan funkcjonalny systemu w wersji bazowej `1.2`. Ma sluzyc jako punkt odniesienia przy pisaniu changeloga dla `1.3`, wiec skupia sie na tym, co system realnie potrafi teraz, jak jest zlozony i jakie funkcje sa dostepne dla uzytkownika, developera i pluginow.

## Cel dokumentu

- opisac aktualny zakres funkcji OpenDriver
- zebrac obecne moduly i ich odpowiedzialnosci
- ulatwic porownanie `1.2 -> 1.3`
- ograniczyc ryzyko pisania changeloga "na pamiec"

## 1. Architektura systemu

OpenDriver sklada sie z trzech glownych warstw:

1. `driver_opendriver.dll`
   Sterownik ladowany przez SteamVR/OpenVR.

2. `opendriver_runner.exe`
   Runtime uzytkownika z GUI Qt, plugin loaderem, event bus i bridge IPC.

3. Pluginy opcjonalne
   Ladowane dynamicznie z katalogu konfiguracyjnego uzytkownika.

Model dzialania:

- SteamVR laduje `driver/driver.vrdrivermanifest`
- SteamVR laduje `driver/bin/win64/driver_opendriver.dll`
- driver uruchamia `opendriver_runner.exe`
- runner inicjalizuje runtime, config, logowanie i pluginy
- runtime komunikuje sie z driverem przez IPC
- pluginy rejestruja urzadzenia, pozycje, inputy i opcjonalne UI

## 2. Funkcje sterownika SteamVR

Obecny sterownik zapewnia:

- rejestracje jako natywny driver SteamVR przez `vrpathreg.exe`
- uruchamianie `opendriver_runner.exe` z tego samego katalogu co DLL
- obsluge IPC miedzy `vrserver` a runtime
- dynamiczne dodawanie urzadzen do SteamVR
- obsluge aktualizacji pose urzadzen
- obsluge aktualizacji inputow urzadzen
- odbior haptic feedback ze SteamVR i przekazywanie go do runtime
- utrzymywanie HMD aktywnego przez cykliczne odswiezanie proximity
- automatyczne proby reconnectu IPC po utracie polaczenia

## 3. Funkcje HMD w obecnym driverze

W kodzie jest obecna implementacja HMD oparta o `COpenDriverHMD`, ktora wspiera:

- rejestracje w SteamVR jako HMD
- ustawienie modelu, producenta, typu urzadzenia i profilu inputu
- publikacje parametrow wyswietlacza:
  - szerokosc
  - wysokosc
  - refresh rate
  - FOV left/right
- ustawienie wlasciwosci chaperone
- ustawienie domyslnego rotation-only pose
- publikacje proximity inputu
- obsluge wirtualnego display path
- uruchamianie pipeline video w tle

Na Windows driver uzywa sciezki Media Foundation / D3D11 dla enkodowania obrazu.

## 4. Rejestracja i typy urzadzen

System obsluguje nastepujace typy urzadzen:

- `HMD`
- `HAND_TRACKER`
- `GENERIC_TRACKER`
- `LIGHTHOUSE`
- `CONTROLLER`

Kazde urzadzenie moze zawierac:

- `id`
- `type`
- `name`
- `manufacturer`
- `serial_number`
- `owner_plugin`
- liste inputow
- parametry wyswietlacza dla HMD
- parametry trackingowe
- stan polaczenia
- stan baterii
- vendor/product id

Inputy urzadzen moga byc typu:

- `BOOLEAN`
- `SCALAR`

## 5. Device Registry

`DeviceRegistry` w wersji 1.2 zapewnia:

- rejestracje urzadzen
- usuwanie pojedynczego urzadzenia
- usuwanie wszystkich urzadzen nalezacych do pluginu
- czyszczenie calego rejestru
- pobieranie urzadzenia po ID
- pobieranie urzadzen po typie
- pobieranie wszystkich urzadzen
- update stanu urzadzenia
- update statusu `connected`
- sprawdzenie istnienia urzadzenia
- liczenie urzadzen globalnie i per typ

To jest centralny magazyn urzadzen po stronie runtime.

## 6. Runtime core

`Runtime` jest singletonem i stanowi glowne API systemu. Wersja 1.2 zawiera:

- inicjalizacje katalogu config
- ladowanie `config.json`
- inicjalizacje logowania
- tworzenie katalogu `plugins`
- ladowanie pluginow z katalogu konfiguracyjnego
- inicjalizacje bridge IPC
- publikacje eventu `CORE_STARTUP`
- shutdown runtime
- publikacje eventu `CORE_SHUTDOWN`
- kolejke callbackow `PostToMainThread`
- glowny tick runtime
- okresowe skanowanie katalogu pluginow
- API do recznego load/unload pluginow
- API do enable/disable/reload pluginow
- API do rejestracji urzadzen
- API do wysylania pose i inputow
- API do logowania

Sciezka pluginow na Windows:

```text
%APPDATA%\opendriver\plugins
```

Sciezka logu na Windows:

```text
%APPDATA%\opendriver\opendriver.log
```

## 7. Event Bus

`EventBus` w 1.2 zapewnia mechanizm publish/subscribe dla calego runtime.

Obslugiwane grupy eventow:

- lifecycle core:
  - `CORE_STARTUP`
  - `CORE_SHUTDOWN`
- lifecycle pluginow:
  - `PLUGIN_LOADED`
  - `PLUGIN_UNLOADED`
  - `PLUGIN_ERROR`
  - `PLUGIN_WARNING`
  - `PLUGIN_INFO`
- logowanie:
  - `LOG_TRACE`
  - `LOG_DEBUG`
  - `LOG_INFO`
  - `LOG_WARN`
  - `LOG_ERROR`
  - `LOG_CRITICAL`
- konfiguracja:
  - `CONFIG_CHANGED`
- urzadzenia:
  - `DEVICE_CONNECTED`
  - `DEVICE_DISCONNECTED`
  - `POSE_UPDATE`
  - `INPUT_UPDATE`
  - `HAPTIC_ACTION`
- wideo:
  - `VIDEO_FRAME`
- zakres user-defined:
  - `0x8000+`

Funkcje EventBus:

- subscribe
- unsubscribe
- publish
- cache ostatniego eventu danego typu
- odczyt ostatniego eventu
- liczenie subskrybentow
- czyszczenie cache eventow

## 8. Bridge IPC

`Bridge` laczy EventBus i DeviceRegistry z warstwa IPC miedzy runtime a driverem.

Funkcje outbound do drivera:

- wysylanie `DEVICE_ADDED`
- wysylanie `POSE_UPDATE`
- wysylanie `INPUT_UPDATE`

Funkcje inbound z drivera:

- odbior `HAPTIC_EVENT`
- odbior `VIDEO_PACKET`

Dodatkowe funkcje bridge:

- snapshot istniejacych urzadzen dla nowego klienta IPC
- liczniki ramek video
- licznik bajtow video
- obliczanie FPS strumienia video

## 9. Plugin system

Plugin system w 1.2 jest opcjonalny, ale w pelni obecny w runtime.

Plugin:

- jest biblioteka dynamiczna `.dll`
- eksportuje `CreatePlugin`
- eksportuje `DestroyPlugin`
- implementuje `IPlugin`
- moze miec folder z `plugin.json`

Plugin API wspiera:

- metadata:
  - nazwa
  - wersja
  - opis
  - autor
- lifecycle:
  - `OnInitialize`
  - `OnShutdown`
- runtime update:
  - `OnTick`
- event handling:
  - `OnEvent`
- health/status:
  - `IsActive`
  - `GetStatus`
- hot reload:
  - `ExportState`
  - `ImportState`
- UI:
  - `GetUIProvider`

Plugin context daje dostep do:

- EventBus
- ConfigManager
- logowania
- rejestracji i usuwania urzadzen
- pose update
- input update
- lookup innych pluginow
- `PostToMainThread`

## 10. Plugin Loader

`PluginLoader` w 1.2 zapewnia:

- ladowanie pojedynczego pluginu z pliku DLL
- odczyt `CreatePlugin` i `DestroyPlugin`
- `OnInitialize(context)`
- wykrywanie duplikatow nazw pluginow
- ladowanie pluginow z katalogu
- honorowanie pola `enabled` z `plugin.json`
- unload pojedynczego pluginu
- unload wszystkich pluginow
- liste pluginow zaladowanych
- liste pluginow dostepnych
- skan katalogu bez ladowania DLL
- hot reload na podstawie zmiany czasu modyfikacji pliku
- wylaczanie pluginu po exception/crash w `OnTick`
- publikacje eventow typu `PLUGIN_LOADED`, `PLUGIN_UNLOADED`, `PLUGIN_ERROR`

Pluginy sa bezpieczniej sprzatane przez `owner_plugin` i `UnregisterDevicesByPlugin`.

## 11. Config system

`ConfigManager` w wersji 1.2 obsluguje:

- `Load`
- `Save`
- `Reload`
- gettery:
  - `GetString`
  - `GetInt`
  - `GetFloat`
  - `GetBool`
- settery:
  - `SetString`
  - `SetInt`
  - `SetFloat`
  - `SetBool`
- plugin-specific config:
  - `GetPluginConfig`
  - `IsPluginEnabled`
  - `SetPluginEnabled`
- dump calego configu do tekstu

Config jest wspolny dla runtime i pluginow.

## 12. GUI / Dashboard

`opendriver_runner.exe` uruchamia GUI Qt z `MainWindow`.

Funkcje GUI obecne w 1.2:

- lista urzadzen
- tabela pluginow
- odswiezanie runtime
- reczne odswiezanie listy pluginow
- globalne enable all pluginow
- globalne disable all pluginow
- wybor pluginu i podglad logow
- dynamiczne zakladki UI dostarczane przez pluginy
- usuwanie pluginu z dysku z GUI
- panel ustawien video:
  - bitrate
  - typ enkodera
  - preset jakosci
  - apply/load ustawien
- status i podglad logow

GUI korzysta z modeli:

- `DeviceModel`
- `PluginModel`

## 13. Logowanie i diagnostyka

System logowania w 1.2 wspiera:

- logi runtime
- logi pluginow
- rozne poziomy logowania
- propagacje logow do EventBus
- prezentacje logow w GUI

Poziomy logow:

- Trace
- Debug
- Info
- Warn
- Error
- Critical

SteamVR logi pomocnicze mozna sprawdzac w:

```text
%LOCALAPPDATA%\Steam\logs
```

## 14. Instalacja i deployment

Obecnie dostepne sa dwa skrypty instalacyjne na Windows:

### `scripts/install_driver_only.ps1`

Funkcje:

- opcjonalny build `Release`
- wykrycie sciezki SteamVR
- walidacja obecnosci sterownika i zasobow
- utworzenie katalogu `%APPDATA%\opendriver`
- utworzenie katalogu `%APPDATA%\opendriver\plugins`
- rejestracja folderu `driver` w SteamVR
- brak deployu pluginow

### `scripts/install_driver.ps1`

Funkcje:

- pelniejsza instalacja historycznego flow
- build `Release`
- walidacja drivera i runnera
- deploy pluginow do katalogu konfiguracyjnego
- rejestracja drivera w SteamVR

## 15. Obecny workflow uzytkownika

Wariant minimalny:

1. Build projektu
2. Rejestracja `driver` w SteamVR
3. Start SteamVR
4. Driver uruchamia runtime
5. Runtime dziala nawet bez pluginow

Wariant rozszerzony:

1. Umieszczenie pluginow w `%APPDATA%\opendriver\plugins`
2. Runtime skanuje i laduje pluginy
3. Pluginy rejestruja urzadzenia do SteamVR

## 16. Obecne ograniczenia i uwagi dla changeloga 1.3

Przy pisaniu changeloga 1.3 warto porownywac szczegolnie te obszary:

- czy zmienil sie format lub zakres `driver.vrdrivermanifest`
- czy zmienil sie sposob startu `opendriver_runner.exe`
- czy zmienil sie model pluginow
- czy doszly nowe typy urzadzen
- czy zmienilo sie GUI
- czy doszly nowe eventy lub payloady IPC
- czy zmienilo sie zachowanie HMD/video pipeline
- czy zmienil sie sposob instalacji drivera
- czy pluginy nadal sa opcjonalne
- czy zmienil sie config schema

## 17. Szybkie podsumowanie wersji 1.2

OpenDriver 1.2 to:

- natywny driver SteamVR
- osobny runtime z GUI
- plugin system z hot reload
- event bus i bridge IPC
- rejestr urzadzen
- konfigurowalny system pluginow
- podstawy video/HMD path
- instalacja driver-only lub driver+plugins

Jesli w `1.3` cokolwiek zmieni ktorykolwiek z tych punktow, warto to opisac w changelogu jako zmiane funkcjonalna, architektoniczna albo instalacyjna.
