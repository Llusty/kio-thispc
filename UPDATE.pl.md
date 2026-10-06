[English](UPDATE.md) | [Polski](UPDATE.pl.md)

# Aktualizacja do 0.39.0

Wydanie dodaje natywne uruchamianie plików lokalnych oraz bogate, asynchroniczne podglądy przez KDE/KIO.

```bash
cd ~/Pobrane
tar -xzf kio-thispc-0.39.0.tar.gz
cd kio-thispc-0.39.0
chmod +x install.sh
./install.sh
```

## Zakres 0.39.0

- prawidłowe rozwiązywanie lokalnych adresów `file://` i natywny proces KDE Open/Execute;
- miniatury obrazów/wideo KIO, podglądy ikon plików wykonywalnych Windows i zawartości folderów;
- zapisywane per lokalizacja ustawienie **Pokaż podglądy** przez `DirectoryViewProfile`;
- asynchroniczne ograniczone partie, cache i tokeny generacji w `PreviewController`;
- planowanie podglądów wyłącznie dla widocznych elementów w dużych katalogach;
- wyłączone podglądy zdalne;
- unieważnianie przez F5 i deterministyczna obsługa cyklu życia zadań podglądu.

## Weryfikacja 0.39.0

- ręczny odbiór Stage 1–7: FULL MANUAL PASS;
- clean build, version CLI, focused regression i pełna regresja: patrz walidacja commita wydania.

---

# Aktualizacja do 0.36.0

Wydanie dodaje Network & Remote Locations przez natywne KIO: ręczne połączenia SMB, SFTP, FTP, WebDAV i WebDAVS, Saved Remote Locations, Recent/Reconnect, transakcyjne odtwarzanie sesji/historii oraz opcjonalne natywne discovery.

```bash
cd ~/Pobrane
tar -xzf kio-thispc-0.36.0.tar.gz
cd kio-thispc-0.36.0
chmod +x install.sh
./install.sh
```

## Zakres 0.36.0

- ręczne połączenia zdalne przez KIO i natywne uwierzytelnianie KIO/KWallet;
- Saved Remote Locations oraz potwierdzane sukcesem Recent/Reconnect;
- transakcyjne odtwarzanie zdalnej sesji oraz historii Primary/Split;
- opcjonalne discovery `remote:/` i `smb:/` bez własnego skanera;
- brak zapisu haseł przez ThisPC/QSettings.

## Weryfikacja 0.36.0

- ręczny odbiór Stage 1–5: PASS;
- build: PASS;
- pełna regresja: 39 zestawów / 9075 asercji PASS;
- CLI `thispc-view 0.36.0`, kod wyjścia 0: PASS;
- ścieżka z rzeczywistym wykrytym serwerem SMB: NOT TESTED — brak wykrywalnych usług LAN.

---

# Aktualizacja do 0.35.0

Wydanie wprowadza obsługę dysków i urządzeń wymiennych (Drives & Devices): dynamiczne wykrywanie urządzeń przez KDE Solid, prezentację odmontowanych woluminów, montowanie na żądanie, czyste odmontowywanie (filesystem-only), bezpieczne usuwanie (Safely Remove) z wyłączeniem zasilania oraz wysuwanie nośników (Eject).

```bash
cd ~/Pobrane
tar -xzf kio-thispc-0.35.0.tar.gz
cd kio-thispc-0.35.0
chmod +x install.sh
./install.sh
```

## Zakres 0.35.0

- dynamiczne wykrywanie dysków i urządzeń wymiennych przez Solid;
- eliminacja okresowego timera i migania „Odświeżanie…” (sterowany zdarzeniami `SolidDeviceMonitor` z debouncingiem 250 ms);
- prezentacja odmontowanych woluminów wymiennych na widoku domowym `thispc:/` jako „Niezamontowany”;
- montowanie na żądanie (`DeviceMountController`) po kliknięciu lub Enter z natychmiastowym wejściem do katalogu;
- pasek boczny prezentujący wyłącznie zamontowane woluminy;
- akcja „Odmontuj” wykonująca czyste odmontowanie systemu plików przez natywny asynchroniczny QtDBus `org.freedesktop.UDisks2.Filesystem.Unmount` (brak wyłączania zasilania nośnika);
- akcja „Bezpiecznie usuń” koordynująca odmontowanie wszystkich zamontowanych partycji dysku i asynchroniczny `org.freedesktop.UDisks2.Drive.PowerOff`;
- bezpieczne usuwanie działające również na już odmontowanych woluminach;
- akcja „Wysuń” dla napędów optycznych (`canEject`);
- automatyczne przekierowanie paneli Primary i Split z odmontowanego punktu montowania z powrotem do `thispc:/`.

## Weryfikacja 0.35.0

- ręczny odbiór etapów Stage 1, Stage 2 i Stage 3 na rzeczywistym sprzęcie USB: PASS;
- build: PASS;
- pełna regresja: 35 zestawów / 8562 asercje PASS;
- CLI `thispc-view 0.35.0`, kod wyjścia 0: PASS;
- brak periodic pollingu i brak blokowania wątku głównego.

---

# Aktualizacja do 0.34.0

Wydanie kończy Explorer UX / View & Navigation Polish: ujednolica obsługę widoków i nawigacji między Primary i Split, zachowując dotychczasową semantykę KIO.

```bash
cd ~/Pobrane
tar -xzf kio-thispc-0.34.0.tar.gz
cd kio-thispc-0.34.0
chmod +x install.sh
./install.sh
```

## Zakres 0.34.0

- stabilne CLI `--version`, Split address parity i spacing kart dysków;
- wizualne przygaszenie ukrytych elementów;
- wspólny inline rename;
- nawigacja klawiaturą i stabilny current/focus w `thispc:/`;
- profile widoku per-folder z Apply/Remove to subfolders;
- dziewięć stopni rozmiaru ikon i skróty klawiaturowe;
- finalny visual parity polish Primary/Split.
- Drobny follow-up 1 px/accent line nie blokuje wydania. Pendrive/removable
  pozostaje w 0.35, a double-click separatora do 50/50 jest przyszłym pomysłem.

## Weryfikacja 0.34.0

- ręczny odbiór etapów funkcjonalnych oraz finalnego visual parity polish: PASS;
- finalny build: PASS;
- pełna regresja: 32 zestawy / 8317 asercji PASS;
- CLI `thispc-view 0.34.0`, kod wyjścia 0: PASS;
- izolowany install smoke: PASS; instalacja systemowa oczekuje na ręczne podanie
  hasła `sudo`;
- finalny manual release smoke: PASS; dwa znane cosmetic follow-upy (1 px/accent
  line oraz kursor kart `thispc:/`) są świadomie nieblokujące.

---

# Aktualizacja do 0.33.0

Wydanie kontynuuje porządkowanie architektury bez dodawania nowych funkcji użytkowych. Zachowanie 0.32.0 pozostaje zachowane, wraz z poprawkami regresji wykrytymi podczas ekstrakcji.

```bash
cd ~/Pobrane
tar -xzf kio-thispc-0.33.0.tar.gz
cd kio-thispc-0.33.0
chmod +x install.sh
./install.sh
```

## Zakres 0.33.0

- `LocationPresentation` i `NavigationHistory`;
- `PreviewCoordinator` i `ActionStateController`;
- wspólny dla Primary/Split `DirectoryListingCore`;
- `PaneMenuController` i `DriveHomeCoordinator`;
- poprawka crashu menu kontekstowego sidebara podczas odświeżania dysków;
- poprawka aktywacji inline rename;
- wspólny `BrowserPane` i odłożony UX pozostają poza zakresem wydania.

## Weryfikacja 0.33.0

- ręczny odbiór Stage 1–8 i poprawki inline rename: PASS;
- baseline po poprawce inline rename: 31 zestawów / 7852 asercje PASS;
- finalny build: PASS;
- pełna regresja: 31 zestawów / 7852 asercje PASS;
- izolowany install/launch smoke: PASS;
- `git diff --check`: PASS.

---

# Aktualizacja do 0.32.0

Wydanie kończy porządkowanie architektury bez dodawania nowych funkcji użytkowych. Zachowanie 0.31.0 pozostaje zachowane, wraz z poprawkami regresji wykrytymi podczas refaktoru.

```bash
cd ~/Pobrane
tar -xzf kio-thispc-0.32.0.tar.gz
cd kio-thispc-0.32.0
chmod +x install.sh
./install.sh
```

## Zakres 0.32.0

- acykliczne moduły Split Sync;
- wydzielone widgety i styl aplikacji oraz utwardzony test harness;
- `PaneContext` / `PaneAdapter`;
- `TabController`, `SearchUiController` i `SelectionMenuController`;
- `PrimaryBrowserPane` z własnym stanem, listingiem KIO, renderowaniem i cache miniatur;
- Stage 9 / wspólny `BrowserPane` przeniesiony do 0.33.

## Weryfikacja 0.32.0

- ręczny odbiór wszystkich etapów 0.32: PASS;
- baseline po Stage 8: 26 zestawów / 6662 asercje PASS;
- końcowy build: PASS;
- focused regression: 10 zestawów / 1738 asercji PASS;
- pełna regresja: 26 zestawów / 6662 asercje PASS;
- `git diff --check`: PASS.

---

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
