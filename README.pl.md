[English](README.md) | [Polski](README.pl.md)

# kio-thispc 0.38.0

## 0.38.0 — Narzędzia pamięci masowej (Storage Tools)

- **Silnik skanowania pamięci masowej (Storage Scan Core):** Wysokowydajny, asynchroniczny silnik skanowania katalogów w tle (`StorageScanJob`, `StorageScanWorker`) z licznikami na żywo, anulowaniem, zatrzymywaniem na granicach punktów montowania (`st_dev`), ignorowaniem dowiązań symbolicznych, deduplikacją twardych dowiązań i raportowaniem błędów.
- **Największe pliki i foldery (Top-N):** Analiza zajętości przestrzeni z konfigurowalnym limitem (Top 25/50/100/Wszystkie), sortowaniem według rozmiaru logicznego lub przydzielonego na dysku, wchodzeniem do podfolderów i akcją „Pokaż w folderze”.
- **Narzędzia sum kontrolnych (Hash Utilities):** Samodzielne beztrybowe okno sum kontrolnych (`HashUtilitiesDialog`) dla SHA-256, SHA-1 i MD5 ze strumieniowym hashowaniem w wątku roboczym, paskiem postępu, kopiowaniem do schowka i wykrywaniem równoległych modyfikacji.
- **Wyszukiwarka duplikatów (Duplicate Finder):** Szybkie wyszukiwanie duplikatów (`DuplicateFinderJob`) poprzez grupowanie po rozmiarze, deduplikację tożsamości fizycznej (hardlinks) i sekwencyjną weryfikację SHA-256 tylko dla kolidujących bajtów.
- **Bezpieczny przegląd i akcje (Safe Review & Actions):** Interaktywny kontroler rozwiązywania duplikatów (`DuplicateActionController`) zintegrowany z `FileActions` i `OperationManager`. Bezpieczne usuwanie do kosza i przenoszenie z potwierdzeniem, rewalidacją migawki przed akcją, pełnym cofaniem `KIO::FileUndoManager` i wykluczaniem zmienionych plików.
- **Interaktywna mapa zajętości (Treemap):** Pamięciowy silnik Squarified Treemap (`StorageTreemapLayout`, `StorageTreemapWidget`) wizualizujący strukturę katalogu bez dodatkowych operacji I/O i bez próbkowania typów MIME. Kanoniczna własność fizyczna dla trybu allocated, tryb logiczny, drill-down, tooltipy i nawigacja klawiaturą.
- Finalna automatyczna regresja: 52 zestawy / 32282+ asercji PASS; build, CLI `thispc-view 0.38.0` i odbiór manualny Stage 1–6 PASS.

---

## 0.37.0 — Właściwości (Properties 2.0)

- **Kompleksowe Właściwości 2.0:** Asynchroniczny model danych właściwości dla plików, katalogów oraz woluminów dyskowych.
- **Informacje ogólne:** Pełna ścieżka i lokalizacja, logiczny rozmiar vs. zajęte miejsce na dysku, numer i-węzła (inode), właściciel/grupa, znaczniki czasu (w tym czas utworzenia `statx` birth time) oraz bezpieczne odczytywanie celu symlinków.
- **Właściwości dysków:** Beztrybowy inspektor woluminów prezentujący typ systemu plików, opcje montowania, paski pojemności/zajętości/wolnego miejsca, UUID, nadrzędną magistralę dysku (NVMe/SATA/USB) i powiadomienie o bezpiecznym usunięciu.
- **Uprawnienia POSIX i edytor ACL:** Standardowa siatka praw z weryfikacją zapisu oraz edytor oparty o natywne `libacl` z obsługą nazwanych użytkowników/grup, maski, praw efektywnych i reguł dziedziczenia w katalogach (Default ACL).
- **Sumy kontrolne SHA-256:** Dedykowany kalkulator obliczający skróty asynchronicznie w blokach z animowanym paskiem postępu, anulowaniem/restartem, kopiowaniem do schowka i wykrywaniem zmian pliku w trakcie hashowania.
- **Metadane KDE:** Leniwy podgląd metadanych w trybie tylko do odczytu biblioteką KF6 `KFileMetaData` dla grafiki (EXIF), tagów audio, strumieni wideo i dokumentów PDF/biurowych.
- **Modeless i wiele okien:** Właściwości plików, folderów i dysków działają beztrybowo; wiele okien może być otwartych równocześnie między panelem Primary a Split; weryfikacja tożsamości `st_dev`/`st_ino` chroni przed zapisem do usuniętych lub zastąpionych plików.
- Finalna automatyczna regresja: 46 zestawów / 9690 asercji PASS; build, CLI `thispc-view 0.37.0` i odbiór manualny Stage 1–6 PASS.

---

## 0.36.0 — Network & Remote Locations

- Ręczne połączenia SMB, SFTP, FTP, WebDAV i WebDAVS przez natywne KIO oraz Saved Remote Locations i potwierdzane sukcesem Recent/Reconnect.
- Transakcyjne odtwarzanie zdalnej sesji i historii, w tym niezależnej historii Split.
- Opcjonalne natywne discovery `remote:/` i `smb:/`; ręczne połączenia nie zależą od discovery.
- ThisPC nie zapisuje haseł w QSettings; poświadczenia pozostają w KIO/KWallet.
- Finalna regresja: 39 zestawów / 9075 asercji PASS; build, CLI `thispc-view 0.36.0` i ręczny odbiór Stage 1–5 PASS. Ścieżka z rzeczywistym wykrytym serwerem SMB: NOT TESTED (brak wykrywalnych usług LAN).

## 0.35.0 — Drives & Devices

- Stage 1: dynamiczne wykrywanie dysków i urządzeń pamięci masowej przez KDE Solid; sterowany zdarzeniami `SolidDeviceMonitor` z debouncingiem 250 ms eliminuje okresowy polling i miganie „Odświeżanie…”.
- Stage 2: wykrywanie i prezentacja odmontowanych woluminów wymiennych na `thispc:/` jako „Niezamontowany”; montowanie na żądanie (`DeviceMountController`) po kliknięciu lub Enter z natychmiastowym wejściem do katalogu; sidebar prezentuje wyłącznie zamontowane woluminy.
- Stage 3: pełna asynchroniczna obsługa cyklu życia urządzeń (`DeviceRemovalController`):
  - akcja „Odmontuj” wykonuje czyste odmontowanie systemu plików wyłącznie przez natywny QtDBus `org.freedesktop.UDisks2.Filesystem.Unmount` (brak wyłączania zasilania);
  - akcja „Bezpiecznie usuń” koordynuje odmontowanie wszystkich zamontowanych partycji dysku i asynchroniczny `org.freedesktop.UDisks2.Drive.PowerOff`;
  - obsługa bezpiecznego usuwania dla woluminów już odmontowanych;
  - obsługa wysuwania nośników optycznych („Wysuń”) na urządzeniach ze zdolnością `canEject`;
  - bezpieczne przekierowanie paneli Primary i Split z odmontowanego punktu montowania z powrotem do `thispc:/`.
- Finalna automatyczna regresja: 35 zestawów / 8562 asercje PASS; build, CLI version i manual acceptance PASS.

## 0.31.0 — Split View Synchronization

- Wprowadzono bezpieczne, nieinwazyjne porównywanie i synchronizację folderów w trybie podziału okna (Split View).
- **Stage 1 (Porównanie paneli):** asynchroniczne porównywanie bezpośrednich elementów lewego i prawego panelu z klasyfikacją (Takie same, Tylko po lewej, Tylko po prawej, Zmienione) na podstawie metadanych KIO (rozmiar, mtime, typ wpisu), z dopasowaniem nazw Unicode i wielkości liter. Wirtualne ścieżki (`thispc:/`, `thispcsearch:/`) są bezpiecznie blokowane.
- **Stage 2 (Podgląd planu):** deterministyczny generator planu synchronizacji dla kierunków Lewy → Prawy oraz Prawy → Lewy z klasyfikacją akcji (Bez zmian, Kopiuj, Zaktualizuj, Konflikt, Nieobsługiwane).
- **Stage 3 (Bezpieczne wykonanie):** asynchroniczne kopiowanie i aktualizacja plików (`LocalFileCopyJob`) z zerem operacji usuwania (brak Delete/Mirror), pomijaniem rekurencji katalogów oraz ochroną konfliktów i nieobsługiwanych wpisów.
- **Bezpieczeństwo operacji dyskowych:** atomowy preflight bezpośrednio przed mutacją każdego pliku (walidacja ścieżek, uprawnień, odrzucanie `.` i `..`).
- **Deterministyczny cykl życia Anulowania (Cancel):** eliminacja wyścigu liczników (brak błędu off-by-one), oczekiwanie na sygnał terminalny aktywnego wątku roboczego i 100% zgodność raportu UI z fizycznym stanem systemu plików (potwierdzona audytem na 20 000 plików).
- **Automatyczne odświeżanie:** natychmiastowe ponowne porównanie paneli po zakończeniu synchronizacji.

## 0.30.0 — Grouping and per-folder view settings

- Tryb Icons/List/Details/Compact jest zapamiętywany osobno dla każdego folderu i
  odtwarzany w panelu głównym, Split View, kartach i przywróconej sesji.
- Preferencje obejmują lokalne oraz zdalne URL-e KIO i nie wymagają skanowania
  folderu ani dodatkowych metadanych.
- Menu **Widok** grupuje Icons/List/Details/Compact, a podmenu **Pokaż** zawiera tylko
  działające przełączniki: ukryte elementy, miniatury, panel podglądu i pełne nazwy.
- Podmenu **Rozmiar ikon** udostępnia działające poziomy Bardzo duże (96 px),
  Duże (64 px), Średnie (48 px) i Małe (32 px), zapamiętywane per folder.
- Widok kompaktowy używa gęstych wierszy 20 px i kolumn wypełnianych od góry do
  dołu, z elidowaniem nazw, natywną nawigacją kierunkową Qt i obsługą obu paneli.
- Grupowanie według typu, daty modyfikacji i rozmiaru działa w Icons, List,
  Details i Compact, w obu panelach, kartach oraz wynikach Search/KIO.
- Nagłówki grup są prezentacyjne: nie uczestniczą w zaznaczeniu, akcjach plikowych,
  schowku, DnD, Preview ani Quick Look. Sortowanie wewnątrz grup zachowuje bieżący
  klucz i kierunek.
- Grupowanie dat używa lokalnego kalendarza (m.in. Today/Yesterday/This week), a
  grupowanie rozmiaru korzysta wyłącznie z rozmiaru już dostarczonego przez KIO;
  foldery i brakujące rozmiary mają oddzielne deterministyczne grupy.
- Ręczny odbiór Stage 1/2 zakończył się PASS. Poprawiono również obrys zaznaczenia
  oraz zalegający stan hover w widoku pogrupowanym, który wizualnie przypominał
  drugie zaznaczenie.

## 0.29.0 — Inteligentne nazwy nowych elementów

- Nowy plik, folder i element z szablonu otrzymuje przed otwarciem edytowalnego
  dialogu pierwszą wolną nazwę, z uzupełnianiem luk numeracji.
- Numer jest wstawiany przed pełnym rozszerzeniem; zachowane są nazwy ukryte,
  wiele kropek, Unicode, ręczna edycja i właściwy panel Split View.
- Po zatwierdzeniu nazwa jest sprawdzana ponownie. Operacje tworzenia nie żądają
  `Overwrite` ani `Resume`, a obsługiwane operacje zachowują Undo/Redo.
- Dla zdalnego KIO oba listingi są asynchroniczne i fail-closed. Nie jest to
  atomowa rezerwacja: ostateczna ochrona zależy od no-overwrite gwarantowanego
  przez konkretny worker i protokół. Listing niewiarygodny lub nieobsługiwany,
  błędy uwierzytelnienia i niejednoznaczne błędy nie są traktowane jako wolna
  nazwa. Szczegóły: `RELEASE_NOTES_0.29.0.md`.
- Podstawowy ręczny odbiór Stage 1 i jednorazowej lokalizacji zdalnej Stage 2
  został potwierdzony. Nie testowano wszystkich protokołów ani awarii połączenia.

## 0.28.0 — Batch Rename

- Podgląd przed wykonaniem: prefix/suffix, numerowanie, zamiana tekstu, zmiana
  wielkości liter i rozszerzeń oraz wyrażenia regularne.
- Bezpieczny preflight kolizji i zmiany inode, obsługa aktywnego panelu Split
  View oraz wspólne Undo/Redo dla kwalifikujących się lokalnych partii.
- Produkcyjne auto-recovery v2 obejmuje wyłącznie kwalifikujące się lokalne,
  liniowe Execute/Undo/Redo i po restarcie dokańcza zapisany kierunek.
- Swapy i cykle mają działające wykonanie oraz sesyjne Undo/Redo, ale ich
  generator v2 i automatyczne recovery po awarii pozostają wyłączone.
- KIO fallback (w tym zdalne URL-e) nie ma gwarancji local-linear. Brak
  gwarancji odporności na zanik zasilania; SIGKILL testuje tylko awarię procesu.
- Procesy zewnętrzne, które nie respektują blokady, oraz nieredukowalne okna
  TOCTOU pozostają możliwe. Pełny kontrakt i wyniki: `RELEASE_NOTES_0.28.0.md`.

## 0.27.0 — Quick Look on Space

- Duży, tymczasowy podgląd otwierany spacją w aktywnym widoku plików.
- Space lub Esc zamyka Quick Look; fokus pozostaje w widoku, więc strzałki nadal zmieniają zaznaczenie.
- Podgląd podąża za aktywnym panelem Split View i aktualnym pojedynczym zaznaczeniem.
- Search, Ctrl+L, zmiana nazwy, popupy i dialogi zachowują normalne działanie spacji.
- Quick Look współdzieli formaty, limity i asynchroniczną ochronę Preview Pane; Alt+P nadal działa niezależnie.
- Pełna regresja: 22 zestawy / 3598 asercji PASS; wszystkie 9 przypadków ręcznych KDE/CachyOS potwierdzone.

## 0.26.0 — Preview Pane

- Panel podglądu włączany skrótem Alt+P.
- Obrazy, tekst, Markdown, JSON i XML.
- Asynchroniczny podgląd pierwszej strony PDF przez QtPdf.
- Metadane audio/wideo przez TagLib.
- Metadane zdjęć EXIF przez Exiv2.
- Bezpieczny podgląd zawartości ZIP, 7z, tar i tar.gz bez rozpakowywania.
- Podsumowania folderów.
- Poprawione breadcrumbs w Split View.
- Zabezpieczenie przed spóźnionymi wynikami podglądu.
- Pełna regresja: 20 zestawów PASS, Preview Pane 106 asercji PASS.
- Ręczna akceptacja KDE/CachyOS zakończona pomyślnie.

## 0.25.0 — Archiwa i rozbudowane menu Nowy

- Menu Nowy: Markdown i szablony użytkownika; opróżnianie Kosza z potwierdzeniem.
- Rozpakowywanie ZIP, 7z, tar i tar.gz: Wypakuj tutaj / Wypakuj do; walidacja zawartości, izolacja procesu i publikacja bez nadpisywania.
- Tworzenie ZIP, 7z i tar.gz: obsługa wielu zaznaczonych elementów, postępu, błędów i anulowania.
- Długie ścieżki w Split View nie zmieniają ręcznie ustawionych proporcji paneli.
- Testy automatyczne Stage 3: 20 zestawów, 3470 asercji PASS; ręczna akceptacja zgłoszona przez użytkownika.
- Wypakowywanie wymaga Ark, Landlock ABI 3+, /proc i systemu plików obsługującego atomową publikację bez nadpisywania; szczegóły w `docs/ARCHIVE_STAGE2.md`.
- Szczegóły tworzenia archiwów: `docs/ARCHIVE_STAGE3.md`.

## 0.24.0 — Sidebar & Split View UX

- Split View ma dwa równorzędne panele z osobnymi breadcrumbs i wspólnym toolbar/sidebarem działającym na aktywnym panelu.
- Sidebar obsługuje Drag & Drop do Places, urządzeń i Szybkiego dostępu bez omijania istniejącego FileActions.
- Sidebar jest pionowo przewijalny i regulowany w zakresie 205–480 px, a ustawiona szerokość jest zapisywana między uruchomieniami.
- Długie etykiety są elidowane po prawej stronie i pokazują pełną nazwę w tooltipie bez poziomego scrollbara.
- Search ma niezależny stan dla obu paneli: zapytania, filtry, wyniki, postęp i URL z parametrami pozostają przypisane do właściwego panelu.
- Oba panele pokazują pełny widok `Ten komputer`/`thispc:/`, a wspólne akcje nawigacyjne działają na aktywnym panelu.
- Pełna regresja przechodzi **1022/1022 asercje**, a Stage 1–4 zostały ręcznie zaakceptowane na KDE/CachyOS.
- Brak migracji ustawień użytkownika.

## 0.23.0 — natywny lokalny silnik transferów

- Obsługiwane lokalne kopie i przenoszenia korzystają z natywnego silnika z rzeczywistym Pause/Resume na znanym offsecie; KIO pozostaje backendem dla zdalnych i niewspieranych przypadków.
- Bezpieczna publikacja przez pliki `.thispc-part`, cleanup po anulowaniu oraz kontrolowana obsługa błędów zapisu, braku miejsca i utraty urządzenia.
- Natywna obsługa konfliktów pojedynczych plików: **Nadpisz / Zmień nazwę / Pomiń**.
- Natywne wykonywanie katalogów, wielu źródeł i symlinków z sumarycznym postępem i bieżącym plikiem.
- Zachowanie uprawnień, timestampów i literalnych celów linków symbolicznych.
- Cross-filesystem Move, bezpieczne Undo/Redo dla plików i drzew katalogów oraz wspólna kolejność historii z operacjami KIO.
- Okno operacji obsługuje prawdziwe Pause/Resume, pionową skalę prędkości na wykresie oraz pozostawia zatrzymane zadania widoczne.
- Tooltip pliku pokazuje również jego rozmiar.
- Pełny zestaw automatyczny przechodzi **842/842 asercji**, a pełna akceptacja manualna na KDE/CachyOS została zakończona pomyślnie.

## 0.22.0 — zaawansowane okno transferów

- Aktywne operacje automatycznie otwierają wspólne, niewielkie okno szczegółów.
- Okno pokazuje bieżący plik, postęp, ilość danych, prędkość bieżącą i średnią, ETA oraz wykres prędkości (120 próbek).
- Wysokość dopasowuje się do liczby równoległych operacji, a po zakończeniu ostatniej okno zamyka się automatycznie.
- Szczegóły można zwijać i rozwijać, a każdą obsługiwaną operację można anulować.
- Kompaktowy panel na pasku narzędzi pozostaje miejscem podglądu historii.
- Prawdziwa pauza została przeniesiona do planowanego lokalnego silnika transferów 0.23.0; `KIO::CopyJob::suspend()` nie zatrzymuje niezawodnie bieżącego transferu lokalnego.


## 0.21.0 — refaktor architektury i stabilizacja

- Rozbito monolityczny `src/thispcview.cpp` na mniejsze, odpowiedzialne moduły bez zmiany zachowania użytkowego.
- Wydzielono: `DirectoryView`, `SplitBrowserPane`, `Sidebar`, `SessionManager`, `OperationManager`, `UndoController`, `PropertiesDialog`, `SearchController` i `FileActions`.
- Wspólne typy i helpery przeniesiono do `browsercommon.h`, ograniczając duplikację między panelem głównym i Split View.
- `src/thispcview.cpp` zmniejszył się z około 14 178 do około 7 250 linii.
- Dodano testy dla `PropertiesDialog`, wyszukiwania i operacji plikowych oraz rozszerzono testy paneli i Drag & Drop kart.
- Finalny zestaw automatyczny przechodzi 577 asercji, w tym realne operacje KIO, konflikty, Trash i Undo/Redo.
- Pełny test manualny na KDE/CachyOS został zakończony bez regresji.
- Zachowano wszystkie funkcje 0.20.0 oraz stabilny układ nazw z 0.19.0.4.

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

### Sterowanie

Na górnym pasku poleceń znajdują się dwie nowe ikony:

- **Cofnij** — `Ctrl+Z`;
- **Ponów** — `Ctrl+Y` lub `Ctrl+Shift+Z`.

Przyciski aktywują się tylko wtedy, gdy dana operacja jest dostępna, a ich podpowiedzi pokazują aktualną akcję KIO do cofnięcia lub ponowienia. Po zakończeniu cofania/ponawiania oba panele katalogów są automatycznie odświeżane.

Funkcje z 0.15.4 — kompaktowy popup operacji, dynamiczna wysokość, anulowanie zadań, numer wersji oraz poprawki NTFS/fuseblk — pozostają bez zmian.

---

## Wymagania

0.16.0.1 wymaga **KDE Frameworks 6.17 lub nowszego**, ponieważ obsługa `redo()` w `KIO::FileUndoManager` jest dostępna od KF 6.17. Na testowanej konfiguracji KF 6.29 warunek jest spełniony.

---

## Instalacja

```bash
cd ~/Pobrane
tar -xzf kio-thispc-0.37.0.tar.gz
cd kio-thispc-0.37.0
chmod +x install.sh
./install.sh
```

Uruchom:

```bash
thispc-view
```

---

## Historia poprawek (Hotfix)

- **0.16.0.1:** Naprawiono cofanie zmiany nazwy. Operacja rename jest rejestrowana jako `KIO::moveAs()` przez `FileUndoManager::recordCopyJob()`, dzięki czemu Undo odtwarza poprzednią nazwę zamiast przechodzić do wcześniejszej operacji.
- **0.19.0.4:** Poprawiono układ nazw plików w widoku ikon (sztywna siatka, `QTextLayout`, viewport callout).

---

## Licencja

ThisPC jest udostępniany na warunkach Licencji MIT.
Copyright (c) 2026 Sebastian Harasim.
Szczegóły w pliku `LICENSE`.
