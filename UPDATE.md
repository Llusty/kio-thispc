# Aktualizacja do 0.31.0

Wydanie wprowadza bezpieczne, nieinwazyjne porównywanie oraz synchronizację folderów w trybie podziału okna (Split View).

Poniższe polecenia instalują wydanie z nowego katalogu, bez usuwania istniejącego repozytorium:

```bash
cd ~/Pobrane
tar -xzf kio-thispc-0.31.0.tar.gz
cd kio-thispc-0.31.0
chmod +x install.sh
./install.sh
```

## Nowości

- Porównywanie i synchronizacja paneli Split View (skrót `Ctrl+Alt+C`).
- **Stage 1 (Porównanie paneli):** asynchroniczne porównywanie bezpośrednich elementów lewego i prawego panelu z klasyfikacją (Takie same, Tylko po lewej, Tylko po prawej, Zmienione) na podstawie metadanych KIO (rozmiar, mtime, typ wpisu), z dopasowaniem nazw Unicode i wielkości liter.
- **Stage 2 (Podgląd planu):** deterministyczny generator planu synchronizacji dla kierunków Lewy → Prawy oraz Prawy → Lewy z klasyfikacją akcji (Bez zmian, Kopiuj, Zaktualizuj, Konflikt, Nieobsługiwane).
- **Stage 3 (Bezpieczne wykonanie):** asynchroniczne kopiowanie i aktualizacja plików (`LocalFileCopyJob`) z zerem operacji usuwania (brak Delete/Mirror), pomijaniem rekurencji katalogów oraz ochroną konfliktów i nieobsługiwanych wpisów.
- **Bezpieczeństwo operacji dyskowych:** atomowy preflight bezpośrednio przed mutacją każdego pliku (walidacja ścieżek, uprawnień, odrzucanie `.` i `..`).
- **Deterministyczny cykl życia Anulowania (Cancel):** eliminacja wyścigu liczników (brak błędu off-by-one), oczekiwanie na sygnał terminalny aktywnego wątku roboczego i 100% zgodność raportu UI z fizycznym stanem systemu plików (potwierdzona audytem na 20 000 plików).
- **Automatyczne odświeżanie:** natychmiastowe ponowne porównanie paneli po zakończeniu synchronizacji.

## Weryfikacja przed wydaniem

- Ręczny odbiór Stage 1, Stage 2 oraz Stage 3: PASS.
- Test anulowania na 20 000 plików: 100% spójność UI ze stanem dysku (3330 skopiowano / 16670 anulowano / 0 błędów, brak częściowych plików) — PASS.
- Końcowy build, focused tests, pełna regresja 25 zestawów / 6569 asercji PASS, `git diff --check` (korekta arytmetyczna po wydaniu: 6278 + 291; tag bez zmian).

---

# Aktualizacja do 0.30.0

Wydanie dodaje zapamiętywane ustawienia widoku per folder oraz grupowanie
Explorer-like według typu, daty modyfikacji i rozmiaru.

Poniższe polecenia instalują wydanie z nowego katalogu, bez usuwania istniejącego repozytorium:

```bash
cd ~/Pobrane
tar -xzf kio-thispc-0.30.0.tar.gz
cd kio-thispc-0.30.0
chmod +x install.sh
./install.sh
```

## Nowości

- Icons/List/Details/Compact są zapamiętywane według znormalizowanego URL folderu
  i współdzielone przez panel główny, Split View, karty i przywracanie sesji.
- Menu Widok zawiera uporządkowane tryby widoku, podmenu Pokaż i cztery rozmiary
  ikon: 96/64/48/32 px, również zapamiętywane per folder.
- Compact używa gęstego układu wielokolumnowego z pionowym wypełnianiem.
- Grupowanie: Brak / Typ / Data modyfikacji / Rozmiar. Bieżące sortowanie jest
  zachowane wewnątrz każdej grupy.
- Data używa lokalnego kalendarza i osobnych grup Future/Today/Yesterday/This week/
  Last week oraz dalszych zakresów; brakująca data trafia do Unknown date.
- Rozmiar używa wyłącznie metadanych KIO, bez rekurencyjnego skanowania lub
  synchronicznych statów; foldery i nieznane rozmiary mają osobne grupy.
- Nagłówki grup nie są plikami i nie trafiają do zaznaczenia, menu pliku, DnD,
  schowka, Preview ani Quick Look.
- Naprawiono obrys zaznaczenia nazw oraz zalegający hover w widoku pogrupowanym,
  który mógł wyglądać jak drugie zaznaczenie mimo pojedynczej selekcji modelu.

## Weryfikacja przed wydaniem

- Ręczny odbiór Stage 1 oraz grupowania Typ/Data/Rozmiar: PASS.
- Icons/List/Details/Compact, Split View, Search, trwałość per-folder, sortowanie,
  Ctrl/Shift multi-select, nagłówki grup i obrys zaznaczenia: PASS ręczny.
- Końcowy build, focused tests, pełna regresja, `git diff --check` oraz archiwum
  z SHA-256 są wykonywane ponownie na finalnym kandydacie 0.30.0 przed commit/tag/install.

---


# Aktualizacja do 0.26.0

Wydanie wprowadza Preview Pane z przełącznikiem Alt+P.

Poniższe polecenia instalują wydanie z nowego katalogu, bez usuwania istniejącego repozytorium:

```bash
cd ~/Pobrane
unzip kio-thispc-0.26.0.zip
cd kio-thispc-0.26.0
chmod +x install.sh
./install.sh
```

## Nowości

- Podgląd obrazów, tekstu, Markdown, JSON i XML.
- Asynchroniczny podgląd pierwszej strony PDF.
- Metadane audio/wideo oraz EXIF.
- Podgląd zawartości ZIP, 7z, tar i tar.gz.
- Podsumowania folderów.
- Poprawione breadcrumbs w Split View.
- Naprawione białe tło renderowanych PDF.

## Weryfikacja

- Kompilacja: PASS.
- Pełna regresja: 20 zestawów PASS.
- Preview Pane: 106 asercji PASS.
- Split Layout: 99 asercji PASS.
- Ręczna akceptacja KDE/CachyOS: potwierdzona.

---


# Aktualizacja do 0.25.0

Poniższe polecenia instalują z nowego katalogu, bez usuwania istniejącego repozytorium:

```bash
cd ~/Pobrane
mkdir -p kio-thispc-0.25.0
unzip kio-thispc-0.25.0.zip -d kio-thispc-0.25.0
cd kio-thispc-0.25.0
chmod +x install.sh
./install.sh
```

## 0.25.0 — archiwa i menu Nowy

- Markdown i szablony użytkownika w menu Nowy oraz opróżnianie Kosza z potwierdzeniem.
- Bezpieczne rozpakowywanie ZIP, 7z, tar i tar.gz; tworzenie ZIP, 7z i tar.gz.
- Stabilne proporcje Split View przy długich ścieżkach.
- Automatyczna regresja: 20 zestawów, 3470 asercji PASS; Stage 3 zaakceptowany ręcznie.
- Wypakowywanie wymaga Ark, Linux Landlock ABI 3+ i wsparcia publikacji bez nadpisywania. Patrz `docs/ARCHIVE_STAGE2.md` i `docs/ARCHIVE_STAGE3.md`.

---

# Aktualizacja do 0.24.0

```bash
cd ~/Pobrane
rm -rf kio-thispc
unzip kio-thispc-0.24.0.zip
cd kio-thispc
chmod +x install.sh
./install.sh
```

## 0.24.0 — Sidebar & Split View UX

- równorzędne panele Split View z breadcrumbs dopasowanymi do szerokości paneli;
- wspólny toolbar i sidebar działają na aktualnie aktywnym panelu;
- Drag & Drop plików i folderów do obsługiwanych miejsc w sidebarze;
- pionowo przewijalny i regulowany sidebar z trwałym zapisem preferowanej szerokości;
- długie etykiety sidebara z prawostronnym `…` i pełną nazwą w tooltipie;
- niezależny Search w obu panelach, w tym filtry, zakres, wyniki, postęp i Stop;
- pełny widok `Ten komputer` dostępny również w prawym panelu;
- pełna regresja: **1022/1022 asercje**;
- pełna ręczna akceptacja Stage 1–4 na KDE/CachyOS;
- brak migracji ustawień użytkownika.

## 0.23.0 — natywny lokalny silnik transferów

- rzeczywiste Pause/Resume dla obsługiwanych lokalnych transferów;
- bezpieczne pliki częściowe `.thispc-part` i kontrolowany cleanup;
- natywne konflikty Nadpisz / Zmień nazwę / Pomiń;
- katalogi, wiele źródeł i symlinki z agregowanym postępem;
- zachowanie permissions, timestampów i celów symlinków;
- cross-filesystem Move oraz natywne Undo/Redo plików i drzew;
- KIO pozostaje backendem dla zdalnych i niewspieranych przypadków;
- obsługa disk-full, device-loss i błędów I/O bez crasha;
- pionowa skala prędkości histogramu i rozmiar pliku w tooltipie;
- pełna walidacja automatyczna: **842/842 asercji**;
- pełna walidacja manualna KDE/CachyOS zakończona pomyślnie;
- brak migracji ustawień użytkownika.

## 0.22.0 — zaawansowane okno transferów

- automatyczne, osobne okno szczegółów dla aktywnych operacji;
- wspólna prezentacja wielu równoległych zadań i wysokość dopasowana do ich liczby;
- bieżący plik, źródło/cel w podsumowaniu, rozmiar, czytelny procent, prędkość bieżąca i średnia oraz ETA;
- wykres prędkości z ograniczoną historią próbek;
- zwijanie szczegółów i anulowanie pojedynczych operacji;
- automatyczne zamknięcie po zakończeniu ostatniego zadania;
- dotychczasowy kompaktowy panel nadal przechowuje historię operacji;
- brak migracji ustawień użytkownika.

## 0.21.0 — refaktor architektury i stabilizacja

- wydzielono główne komponenty z `src/thispcview.cpp` do osobnych modułów;
- ograniczono duplikację pomiędzy panelem głównym i Split View;
- zachowano kompatybilność ustawień i przywracania sesji;
- dodano/rozszerzono testy paneli, Drag & Drop kart, `PropertiesDialog`, wyszukiwania i `FileActions`;
- finalny zestaw automatyczny: 577 zaliczonych asercji;
- pełny test manualny KDE/CachyOS zakończony bez regresji;
- brak zmian wymagających migracji ustawień użytkownika.

## 0.20.0 — Szybki dostęp / Ulubione / Ostatnie

- nowa sekcja `Szybki dostęp` w lewym panelu;
- przypinanie i odpinanie folderów z menu kontekstowego;
- zmiana kolejności przypiętych folderów przez Drag & Drop;
- trwały zapis przypiętych folderów i ich kolejności przez `QSettings`;
- nowa sekcja `Ostatnie` z ostatnio odwiedzanymi lokalizacjami;
- trwały zapis historii ostatnich lokalizacji między uruchomieniami;
- zachowany stabilny układ nazw plików z 0.19.0.4.

## 0.19.0.4 — stabilny układ nazw w widoku ikon

- stała, sztywna geometria siatki w IconMode (`gridSize` + `uniformItemSizes`);
- zaznaczenie elementu nigdy nie zmienia `sizeHint()` ani nie przesuwa rzędów poniżej;
- formatowanie tekstu przez `QTextLayout` z obsługą `WrapAtWordBoundaryOrAnywhere`;
- maksymalnie 2 linie w trybie standardowym (ostatnia linia poprawnie elidowana);
- maksymalnie 4 linie w trybie „Pełne nazwy” z zachowaniem jednolitej wysokości kafelków;
- pełna, nieobcięta nazwa zaznaczonego elementu rysowana jako callout na poziomie viewportu po bazowym `paintEvent`;
- całkowite usunięcie starej nakładki QLabel;
- brak zmian w trybach ListMode oraz DetailsMode.
