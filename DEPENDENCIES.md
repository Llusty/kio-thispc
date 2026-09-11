# Zależności

## Arch Linux / CachyOS / EndeavourOS

```bash
sudo pacman -S --needed base-devel cmake extra-cmake-modules qt6-base kio solid
```

## Fedora KDE

```bash
sudo dnf install gcc-c++ cmake extra-cmake-modules qt6-qtbase-devel kf6-kio-devel kf6-solid-devel
```

## Debian / Ubuntu z Plasma 6

Nazwy pakietów zależą od wydania, typowo:

```bash
sudo apt install build-essential cmake extra-cmake-modules qt6-base-dev libkf6kio-dev libkf6solid-dev
```

CMake musi znaleźć:

- Qt6 Core / Gui / Widgets / PrintSupport;
- KDE Frameworks **6.17+**: KIO Core + KIO Widgets;
- KF6 Solid;
- ECM.

### Dlaczego KF6 6.17+?

0.16.0 używa `KIO::FileUndoManager::redo()`, `isRedoAvailable()` i sygnałów redo, które są dostępne od KDE Frameworks 6.17.

## Opcjonalne: tryb administratora

Do funkcji `admin://` / PolicyKit potrzebny jest KDE `kio-admin`.

Na Arch/CachyOS/EndeavourOS:

```bash
sudo pacman -S --needed kio-admin
```

## Opcjonalne narzędzia runtime

```text
okular                        drukowanie PDF
plasma-apply-wallpaperimage   ustawianie tapety Plasma
bluedevil-sendfile            Bluetooth
xdg-email                     domyślny klient e-mail
zip                           tworzenie archiwów ZIP
```

Na Arch/CachyOS/EndeavourOS:

```bash
sudo pacman -S --needed bluedevil xdg-utils zip okular
```

Brak któregoś programu nie blokuje uruchomienia aplikacji — odpowiadająca mu funkcja jest po prostu niedostępna.
