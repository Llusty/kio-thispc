# Zależności

## Arch Linux / CachyOS / EndeavourOS

```bash
sudo pacman -S --needed base-devel cmake extra-cmake-modules qt6-base kio solid libarchive zlib
```

## Fedora KDE

```bash
sudo dnf install gcc-c++ cmake extra-cmake-modules qt6-qtbase-devel kf6-kio-devel kf6-solid-devel libarchive-devel zlib-devel
```

## Debian / Ubuntu z Plasma 6

Nazwy pakietów zależą od wydania, typowo:

```bash
sudo apt install build-essential cmake extra-cmake-modules qt6-base-dev libkf6kio-dev libkf6solid-dev libarchive-dev zlib1g-dev
```

CMake musi znaleźć:

- Qt6 Core / Gui / Widgets / PrintSupport;
- KDE Frameworks **6.17+**: KIO Core + KIO Widgets;
- KF6 Solid;
- ECM;
- libarchive i zlib (walidacja archiwów przed uruchomieniem Ark).

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
ark                           wypakowywanie ZIP / 7z / tar / tar.gz
```

Na Arch/CachyOS/EndeavourOS:

```bash
sudo pacman -S --needed bluedevil xdg-utils zip okular
```

Brak któregoś programu nie blokuje uruchomienia aplikacji — odpowiadająca mu funkcja jest po prostu niedostępna.


## Wypakowywanie — 0.25.0 Stage 2

Oprócz Ark wymagane są Linux z aktywnym **Landlock ABI 3+** oraz /proc.
Do kompilacji potrzebne są nagłówki Linux udostępniające Landlock ABI 3 (6.2+).
System plików celu musi obsługiwać atomowe renameat2(RENAME_NOREPLACE).
Brak ochrony zapisu lub tej operacji powoduje odmowę bez nadpisywania.
Na zweryfikowanym środowisku: Ark 26.08.1, libarchive 3.8.9, zlib 1.3.1, Landlock ABI 10.

Obsługa archiwów szyfrowanych, dowiązań wewnątrz archiwum, scalania i nadpisywania
nie jest częścią Stage 2. Pełne zasady i testy: docs/ARCHIVE_STAGE2.md.
