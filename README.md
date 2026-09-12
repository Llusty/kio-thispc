# kio-thispc 0.20.0


## 0.20.0 — Szybki dostęp / Ulubione / Ostatnie

- Dodano sekcję **Szybki dostęp** w lewym panelu.
- Foldery można przypinać i odpinać z menu kontekstowego.
- Przypięte foldery można porządkować metodą Drag & Drop.
- Kolejność i lista przypiętych folderów są zapisywane w `QSettings` i przywracane po ponownym uruchomieniu.
- Dodano sekcję **Ostatnie** z ostatnio odwiedzanymi lokalizacjami.
- Historia ostatnich lokalizacji jest zapisywana między uruchomieniami i aktualizowana przez normalną nawigację aplikacji.
- Funkcje 0.19.0.4, w tym stały układ nazw w widoku ikon i pełna nazwa zaznaczonego elementu bez przesuwania rzędów, pozostają bez zmian.

## 0.19.0.4 — poprawka układu nazw w widoku ikon

- Sztywna, jednolita siatka kafelków w IconMode: zaznaczenie elementu nigdy nie przesuwa sąsiednich rzędów.
- Własny layout tekstu przez `QTextLayout` z `WrapAtWordBoundaryOrAnywhere` — nazwy bez spacji (np. `VID_20260122.mp4`, `kio-thispc-0.19.0.4.zip`) zawijają się poprawnie bez obcinania z boku.
- W trybie normalnym: maksymalnie 2 linie tekstu, ostatnia elidowana `…`.
- W trybie **Pełne nazwy**: jednakowa, stała wysokość wszystkich kafelków z maksymalnie 4 liniami.
- Pełna nazwa zaznaczonego elementu pokazywana jako callout rysowany bezpośrednio na viewporcie po bazowym `paintEvent` — bez osobnego widgetu QLabel.
- Callout trzyma się granic viewportu (prawy i dolny margines), nie przechwytuje myszy.
- Usunięto `QLabel#selectedNameOverlay` i całą poprzednią logikę nakładki.
- Tryby **Lista** i **Szczegóły** bez zmian.


## 0.19.0.3 — sesja i pełne nazwy

- Aplikacja zapisuje i przywraca otwarte karty, aktywną kartę oraz historię Wstecz/Dalej.
- Stan Split View jest przechowywany per karta: lokalizacja, tryb widoku, sortowanie i włączenie panelu.
- Szerokości paneli są odtwarzane z zapisanego stanu splittera.
- Zaznaczony element w widoku ikon pokazuje pełną, zawijaną nazwę.
- Przełącznik **Pełne nazwy** na pasku czynności wymusza pełne nazwy dla wszystkich elementów i zapamiętuje ustawienie.
- Menu **Widok** zawiera przełącznik **Przywracaj poprzednią sesję**.
- Jawne uruchomienie z argumentem ścieżki/URL otwiera wskazaną lokalizację zamiast poprzedniej sesji.


## 0.18.0 — inteligentna obsługa konfliktów

Operacje kopiowania/przenoszenia korzystają z interaktywnego handlera konfliktów KIOWidgets. Dzięki temu istniejący plik lub katalog nie kończy całej operacji błędem. Dostępne decyzje zależą od rodzaju konfliktu i liczby elementów, ale obejmują m.in. zastąpienie, pominięcie, zmianę lub zasugerowanie nowej nazwy oraz warianty stosowane do wszystkich. KIO przekazuje dialogowi rozmiar i czasy źródła/celu, a konflikty zagnieżdżone w kopiowanych katalogach są wykrywane przez sam `CopyJob`.

Mechanizm jest wspólny dla wklejania ze schowka, Drag & Drop, akcji „Wyślij do” oraz zmiany nazwy. Operation manager i Undo/Redo pozostają aktywne. Anulowanie konfliktu jest traktowane jak normalne anulowanie zadania.


## 0.17.0.5 — test pełnego Drag & Drop

Ta wersja jest kontynuacją działających etapów 0.17 rozwijanych na bazie stabilnego 0.16.0.1.

Najważniejsze zmiany:

- poprawione Drag & Drop na tło katalogu i na foldery w widokach Ikony/Lista/Szczegóły;
- Drag & Drop między głównym panelem i Split View;
- pełniejsze skróty, toolbar i menu PPM dla aktywnego panelu Split View;
- Drag & Drop na zakładki z aktywacją po ok. 650 ms hover;
- `Ctrl+drag` wymusza kopiowanie;
- `Shift+drag` wymusza przenoszenie;
- bez modifiera kursor sugeruje Move na tym samym storage i Copy między różnymi storage, a po Drop pozostaje menu `Kopiuj tutaj / Przenieś tutaj`;
- blokada kopiowania/przenoszenia folderu do niego samego lub jego potomka;
- operacje nadal korzystają z KIO, menedżera operacji i `KIO::FileUndoManager`.

To wydanie jest przeznaczone do testu na rzeczywistym KDE Plasma przed zamknięciem finalnego 0.17.0.

0.16.0.1 dodaje natywne **Cofnij / Ponów** dla operacji plikowych, oparte na `KIO::FileUndoManager` z KDE Frameworks.

## Cofnij / Ponów

Obsługiwane są operacje wykonywane przez `thispc-view`:

- kopiowanie;
- przenoszenie;
- zmiana nazwy;
- przenoszenie do Kosza i przywracanie przy cofnięciu;
- tworzenie folderu;
- tworzenie nowego pliku.

Historia jest przechowywana w bieżącej sesji aplikacji. KIO sam zapisuje dane potrzebne do bezpiecznego odwrócenia operacji. Przy cofaniu kopiowania potrafi również ostrzec, gdy skopiowany plik został później zmodyfikowany.

## Sterowanie

Na górnym pasku poleceń znajdują się dwie nowe ikony:

- **Cofnij** — `Ctrl+Z`;
- **Ponów** — `Ctrl+Y` lub `Ctrl+Shift+Z`.

Przyciski aktywują się tylko wtedy, gdy dana operacja jest dostępna, a ich podpowiedzi pokazują aktualną akcję KIO do cofnięcia lub ponowienia. Po zakończeniu cofania/ponawiania oba panele katalogów są automatycznie odświeżane.

Funkcje z 0.15.4 — kompaktowy popup operacji, dynamiczna wysokość, anulowanie zadań, numer wersji oraz poprawki NTFS/fuseblk — pozostają bez zmian.

## Wymagania

0.16.0.1 wymaga **KDE Frameworks 6.17 lub nowszego**, ponieważ obsługa `redo()` w `KIO::FileUndoManager` jest dostępna od KF 6.17. Na testowanej konfiguracji KF 6.29 warunek jest spełniony.

## Instalacja

```bash
cd ~/Pobrane
rm -rf kio-thispc
unzip kio-thispc-0.20.0.zip
cd kio-thispc
chmod +x install.sh
./install.sh
```

Uruchom:

```bash
thispc-view
```


## Hotfix 0.16.0.1

Naprawiono cofanie zmiany nazwy. Operacja rename jest rejestrowana jako `KIO::moveAs()` przez `FileUndoManager::recordCopyJob()`, dzięki czemu Undo odtwarza poprzednią nazwę zamiast przechodzić do wcześniejszej operacji.


## 0.19.0.4

Poprawiono układ nazw plików w widoku ikon: sztywna siatka, `QTextLayout`, viewport-level callout. Opis zmian — patrz sekcja na górze.
