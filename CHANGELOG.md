# Changelog

## 0.28.0 — Batch Rename

- Dodano zbiorczą zmianę nazw z podglądem: prefix/suffix, numerowanie, replace,
  case/extension i regex.
- Dodano ponowny preflight, ochronę przed kolizjami i podmianą inode oraz
  obsługę aktywnego panelu Split View.
- Kwalifikujące się lokalne partie liniowe mają jedno sesyjne Undo/Redo i
  produkcyjny journal v2; po awarii procesu startup dokańcza zapisany kierunek
  Execute, Undo albo Redo.
- Swapy i cykle zachowują payloady i oferują sesyjne Undo/Redo, lecz ich
  generator v2 oraz automatyczne recovery pozostają wyłączone.
- KIO fallback nie otrzymuje gwarancji local-linear. Wydanie nie deklaruje
  odporności na zanik zasilania ani eliminacji ingerencji zewnętrznych procesów
  i TOCTOU. Szczegóły: `RELEASE_NOTES_0.28.0.md`.

## 0.27.0 — Quick Look on Space

- Dodano duży, tymczasowy Quick Look otwierany spacją bez uruchamiania skojarzonej aplikacji.
- Ponowne naciśnięcie Space lub Esc zamyka podgląd.
- Quick Look działa wyłącznie z aktywnego widoku plików, nie przechwytuje fokusu i podąża za zaznaczeniem oraz aktywnym panelem Split View.
- Search, Ctrl+L, zmiana nazwy, popupy i dialogi zachowują normalną obsługę spacji.
- Współdzielony backend Preview Pane zapewnia te same formaty, limity, komunikaty i ochronę przed spóźnionymi wynikami bez duplikowania parserów.
- Alt+P Preview Pane pozostaje niezależny od Quick Look.
- Pełna regresja: 22 zestawy / 3598 asercji PASS.
- Focused: Quick Look 19, Preview Pane 106, pane routing 269 i Split Layout 99 asercji PASS.
- Wszystkie 9 ręcznych przypadków KDE/CachyOS potwierdzone przez użytkownika.

## 0.26.0 — Preview Pane

- Dodano panel podglądu z przełącznikiem Alt+P.
- Podgląd obrazów ze skalowaniem do dostępnego obszaru.
- Podgląd tekstu, Markdown, JSON i XML.
- Podgląd pierwszej strony PDF przez QtPdf.
- Asynchroniczne renderowanie PDF bez blokowania GUI.
- Naprawiono przezroczyste tło PDF: wynik renderowania jest
  kompozytowany na nieprzezroczystym białym tle.
- Dodano metadane audio/wideo przez TagLib.
- Dodano metadane EXIF przez Exiv2, bez zastępowania obrazu.
- Dodano bezpieczny podgląd zawartości archiwów ZIP, 7z,
  tar i tar.gz przez libarchive, bez rozpakowywania.
- Dodano podsumowania folderów.
- Poprawiono breadcrumbs prawego panelu Split View.
- Zachowano ochronę przed nieaktualnymi wynikami zadań
  asynchronicznych oraz limity przetwarzania.
- Rozszerzono testy Preview Pane i Split View.
- Pełna regresja: 20 zestawów PASS.
- Preview Pane: 106 asercji PASS.
- Split Layout: 99 asercji PASS.
- Ręczna akceptacja KDE/CachyOS potwierdzona przez użytkownika.

## 0.25.0 — Archives + richer New menu

- dodano tworzenie ZIP, 7z i tar.gz przez asynchroniczny libarchive job;
- zachowano „Wyślij do → ZIP”, dodano formaty 7z/tar.gz;
- publikacja bez nadpisywania, obsługa postępu/anulowania/błędów oraz testy;
- szczegóły i ograniczenia: `docs/ARCHIVE_STAGE3.md`.

## 0.24.0

- ujednolicono oba panele Split View: osobne breadcrumbs, równorzędna nawigacja i jeden wspólny toolbar/sidebar routowany do aktywnego panelu;
- usunięto asymetryczny mini-toolbar prawego panelu i zachowano Swap panels oraz zamykanie Split View;
- dodano Drag & Drop plików i folderów do obsługiwanych celów sidebara z użyciem istniejącej ścieżki `FileActions`;
- dodano pionowe przewijanie sidebara, regulowaną szerokość 205–480 px i trwały zapis szerokości wybranej przez użytkownika;
- długie etykiety sidebara są elidowane po prawej stronie i udostępniają pełną nazwę w tooltipie bez poziomego scrollbara;
- Search ma niezależny stan, wyniki, filtry, postęp i zadania dla obu paneli;
- wspólne kontrolki Search, zakres, filtry, Stop i odświeżanie działają na aktywnym panelu;
- prawy panel obsługuje pełny widok `Ten komputer` wraz z kartami folderów i dysków;
- nawigacja Wstecz/Dalej/W górę, `Ctrl+L` i breadcrumbs zachowują Search URL wraz z parametrami;
- zamknięcie Split View zatrzymuje ukryte wyszukiwanie, a ponowne otwarcie zachowuje zapisany URL panelu;
- pełna regresja przechodzi **1022/1022 asercje**, a Stage 1–4 przeszły ręczną akceptację KDE/CachyOS.

## 0.23.0

- dodano natywny lokalny silnik copy/move z dokładnym Pause/Resume na granicach chunków;
- dodano bezpieczną publikację przez `.thispc-part`, cleanup po anulowaniu oraz kontrolowane błędy zapisu, disk-full i device-loss;
- dodano natywne Overwrite/Rename/Skip dla konfliktów pojedynczych plików;
- dodano asynchroniczne planowanie i wykonywanie katalogów, wielu źródeł oraz symlinków;
- zachowywane są permissions, nanosekundowe timestampy katalogów/plików oraz literalne cele symlinków;
- cross-filesystem Move publikuje dane przed usunięciem źródła i integruje się z Undo/Redo;
- natywna historia drzew i historia KIO zachowują wspólną kolejność poleceń;
- KIO pozostaje używane dla URL-i zdalnych i niewspieranych lokalnych przypadków;
- poprawiono widoczność zatrzymanych operacji w szczegółowym oknie;
- dodano pionową skalę prędkości wykresu oraz rozmiar pliku w tooltipie;
- pełna regresja przechodzi **842/842 asercji**;
- ręczna walidacja potwierdziła konflikty, katalogi, symlinki, metadata, wielokrotne Pause/Resume, równoległe transfery, Cancel, cross-filesystem Move, Undo/Redo, disk-full, device-loss i integralność SHA-256.

## 0.22.0

- dodano normalne, nieblokujące okno szczegółów aktywnych operacji, otwierane automatycznie po rozpoczęciu zadania;
- wiele równoległych operacji jest łączonych w jednym oknie o dynamicznej wysokości i ograniczonej wysokości przewijania;
- dodano bieżący plik, źródło/cel w podsumowaniu, ilość przetworzonych danych, duży procent, prędkość bieżącą i średnią oraz ETA;
- dodano wykres prędkości z ograniczoną historią 120 próbek;
- dodano zwijanie szczegółów z zachowaniem stanu podczas odświeżania;
- ujednolicono anulowanie zadań pomiędzy oknem szczegółów i kompaktowym panelem;
- okno szczegółów pokazuje wyłącznie aktywne zadania i zamyka się po zakończeniu ostatniego, a kompaktowy panel zachowuje historię;
- odświeżanie interfejsu jest grupowane i nie zastępuje przycisków w trakcie kliknięcia;
- świadomie usunięto pozorną pauzę opartą na `KIO::CopyJob::suspend()`; prawdziwa pauza wymaga planowanego lokalnego silnika transferów 0.23.0;
- zestaw automatyczny obejmuje osobny pakiet testów `OperationManager`.

## 0.21.0

- przeprowadzono zachowawczy refaktor architektury `thispc-view` bez celowych zmian zachowania;
- wydzielono `DirectoryView`, `SplitBrowserPane`, `Sidebar`, `SessionManager`, `OperationManager`, `UndoController`, `PropertiesDialog`, `SearchController` i `FileActions`;
- dodano wspólne helpery i typy w `browsercommon.h`;
- zredukowano `src/thispcview.cpp` z około 14 178 do około 7 250 linii;
- ograniczono duplikację logiki widoków i operacji pomiędzy panelem głównym i Split View;
- dodano testy `PropertiesDialog`, `SearchController` i `FileActions` oraz rozszerzono istniejące testy paneli i Drag & Drop kart;
- finalna walidacja automatyczna przechodzi 577 asercji, w tym realne KIO create/copy/move/rename/Trash, konflikty i Undo/Redo;
- pełny test manualny na KDE/CachyOS potwierdził brak regresji;
- zachowano format ustawień, stan sesji, Quick Access/Recent oraz layout nazw plików z poprzednich wydań.

## 0.20.0

- dodano `Szybki dostęp` / Favorites do lewego panelu;
- dodano przypinanie i odpinanie folderów z menu kontekstowego;
- dodano zmianę kolejności przypiętych folderów przez Drag & Drop;
- lista i kolejność przypiętych folderów są zapisywane przez `QSettings`;
- dodano sekcję `Ostatnie` z ostatnio odwiedzanymi lokalizacjami;
- historia ostatnich lokalizacji jest zapisywana między uruchomieniami;
- zachowano działanie nawigacji głównego panelu i Split View oraz stabilny layout nazw z 0.19.0.4;
- opcjonalne `Recent files` pozostawiono poza zakresem 0.20.0.

## 0.19.0.4

- Sztywna, jednolita geometria siatki (`setGridSize` + `setUniformItemSizes(true)`) w IconMode — zaznaczenie elementu nigdy nie zmienia `sizeHint()` ani nie przesuwa innych rzędów.
- Tekst nazwy pliku renderowany przez `QTextLayout` z `QTextOption::WrapAtWordBoundaryOrAnywhere`; poprawne zawijanie dla nazw bez spacji (znaki `_`, `-`, cyfry, Unicode).
- W normalnym trybie ikon: maksymalnie 2 linie tekstu z elipsą `…` na ostatniej linii.
- W trybie „Pełne nazwy": jednakowa, stała wysokość wszystkich kafelków z maksymalnie 4 liniami tekstu.
- Pełna nazwa zaznaczonego elementu wyświetlana jako callout rysowany na poziomie viewportu w `DirectoryListWidget::paintEvent()` — po bazowym `QListWidget::paintEvent()`.
- Callout zakotwiczony do `visualItemRect()`, ograniczony do granic viewportu, zawsze nad innymi elementami, bez przechwytywania myszy.
- Usunięto `QLabel#selectedNameOverlay`, `selectedNameNeedsOverlay()`, `updateNameOverlay()` oraz magiczne stałe (124 px tekstWidth).
- Tryby ListMode i DetailsMode bez zmian.


## 0.19.0.3

- Usunięto dynamiczne zwiększanie wysokości pojedynczego zaznaczonego kafelka.
- Pełna nazwa zaznaczonego elementu jest wyświetlana jako nakładka, bez przesuwania sąsiednich rzędów.
- Tryb `Pełne nazwy` ma stałą wysokość kafelków na trzy linie tekstu.
- Nakładka śledzi przewijanie, zmianę rozmiaru oraz bieżące zaznaczenie.


## 0.19.0.3

- poprawiono pełną nazwę zaznaczonego elementu: sizeHint sprawdza rzeczywisty selectionModel, więc bardzo długie nazwy dostają wysokość potrzebną do zawijania;
- poprawiono geometrię po przywróceniu sesji: pasek kart i aktywny panel są ponownie układane po pierwszym przebiegu event loop, dzięki czemu niebieska linia jest od razu na właściwej wysokości;
- przywracanie kart, Split View i przełącznik „Pełne nazwy” pozostają bez zmian.


## 0.19.0.1

- hotfix pełnych nazw zaznaczonych elementów w widoku ikon/listy;
- delegat rysuje element bezpośrednio przez styl Qt, dzięki czemu `ElideNone` i `WrapText` nie są ponownie nadpisywane przez bazowy `QStyledItemDelegate::paint()`;
- bardzo długie nazwy po zaznaczeniu mogą teraz rozwinąć się na tyle wierszy, ile potrzebują;
- przełącznik **Pełne nazwy** oraz przywracanie sesji pozostają bez zmian.


## 0.19.0

- dodano przywracanie sesji: karty, aktywna karta i historia nawigacji;
- stan Split View jest zapisywany per karta wraz z lokalizacją, trybem widoku i sortowaniem;
- zapisany stan splittera przywraca szerokości paneli;
- jawny argument startowy pomija przywracanie poprzedniej sesji;
- zaznaczony element w widoku ikon rozwija pełną nazwę;
- dodano zapamiętywany przełącznik `Pełne nazwy` na pasku czynności;
- dodano przełącznik `Przywracaj poprzednią sesję` w menu Widok;
- zachowano funkcje 0.18.0, w tym natywną obsługę konfliktów KIO.


## 0.18.0

- włączono interaktywną obsługę konfliktów KIO dla kopiowania i przenoszenia;
- konflikty są obsługiwane również podczas wklejania, Drag & Drop i „Wyślij do”;
- natywny dialog KIO może zaoferować zastąpienie, pominięcie, zmianę/sugerowanie nowej nazwy oraz warianty „dla wszystkich”;
- dialog konfliktu otrzymuje metadane źródła i celu, m.in. rozmiary oraz daty;
- zmiana nazwy na istniejącą nazwę korzysta z tego samego mechanizmu konfliktów;
- anulowanie dialogu konfliktu jest rozpoznawane jako anulowanie, bez dodatkowego komunikatu o błędzie;
- zachowano operation manager oraz rejestrację `KIO::FileUndoManager`;
- 0.17.0.5 pozostaje bazą Drag & Drop i parity Split View.


## 0.17.0.5

- przejęto działające Etapy A/B z lokalnej gałęzi roboczej;
- naprawiono parity Split View: skróty, toolbar i rozbudowane menu kontekstowe korzystają z aktywnego panelu;
- dodano działający Drag & Drop katalog ↔ katalog oraz główny panel ↔ Split View;
- dodano `ExplorerTabBar` z hover 650 ms i Drop na zakładki;
- dokończono przekazywanie `Qt::KeyboardModifiers` przez cały przepływ Drop;
- `Ctrl+drag` wymusza Copy, `Shift+drag` wymusza Move;
- domyślna akcja kursora to Move na tym samym lokalnym storage i Copy między różnymi storage;
- zwykły Drop bez modifiera zachowuje menu wyboru Copy/Move;
- dodano ochronę przed kopiowaniem/przenoszeniem folderu do niego samego lub jego potomka;
- zachowano rejestrację operacji w `KIO::FileUndoManager`;
- rozszerzono testy syntetyczne polityki Drop i zabezpieczenia cykli.

## 0.16.0.1

- hotfix Cofnij/Ponów dla zmiany nazwy;
- rename używa teraz `KIO::moveAs()` + `FileUndoManager::recordCopyJob()`;
- poprawka obejmuje zmianę nazwy z głównego widoku i z okna Właściwości;
- Kosz, kopiowanie, przenoszenie i pozostałe funkcje 0.16.0 bez zmian.


## 0.16.0

- dodano natywne Cofnij/Ponów przez `KIO::FileUndoManager`;
- nowe ikony Cofnij i Ponów na górnym pasku poleceń;
- skróty `Ctrl+Z`, `Ctrl+Y` oraz `Ctrl+Shift+Z`;
- historia obejmuje kopiowanie, przenoszenie, zmianę nazwy, Kosz, tworzenie folderów i tworzenie plików;
- przenoszenie do Kosza jest rejestrowane jako `FileUndoManager::Trash`, dzięki czemu może zostać przywrócone;
- kopiowanie/przenoszenie jest rejestrowane bezpośrednio z `KIO::CopyJob`, dzięki czemu KIO zachowuje informacje potrzebne do bezpiecznego cofania;
- akcje Cofnij/Ponów automatycznie pokazują dostępność i opis bieżącej operacji;
- po undo/redo odświeżany jest główny widok i aktywny split view;
- dodano `QCoreApplication::applicationVersion()` = `0.16.0`;
- minimalna wersja KDE Frameworks została podniesiona do 6.17 ze względu na natywne `redo()`;
- zachowano cały menedżer operacji z 0.15.4 oraz poprawki NTFS/fuseblk.

## 0.15.4

- popup operacji wysuwany bardziej na prawo; przy dostępnej przestrzeni tylko ok. 96 px zachodzi na okno główne;
- automatyczne ograniczenie pozycji do dostępnego obszaru ekranu;
- czytelny numer wersji w prawym dolnym rogu (`palette(window-text)`).

## 0.15.4

- przebudowano popup operacji z `QMenu/QWidgetAction` na własny `Qt::Popup`;
- dynamiczna wysokość jest liczona z rzeczywistych kart operacji;
- lista rośnie do ok. 560 px, a następnie używa pionowego przewijania;
- szerokość popupu pozostaje ok. 410 px;
- nagłówek i stopka pozostają zawsze widoczne;
- dodano oznaczenie `v0.15.4` w prawym dolnym rogu paska stanu;
- brak zmian w backendzie KIO/KJob i logice NTFS/fuseblk.

## 0.15.2

- dynamiczna wysokość popupu operacji;
- lista rośnie wraz z liczbą zadań do ok. 560 px;
- po osiągnięciu limitu pojawia się pionowy suwak;
- szerokość popupu pozostaje bez zmian.

## 0.15.1

- zastąpiono stały dolny panel operacji kompaktowym popupem;
- dodano ikonę operacji po prawej stronie górnego paska poleceń;
- dodano licznik aktywnych zadań na ikonie;
- popup zawiera historię aktywnych i zakończonych operacji;
- zachowano postęp, transfer, prędkość, anulowanie pojedyncze i zbiorcze;
- zachowano opcję czyszczenia zakończonych operacji;
- popup zamyka się po kliknięciu poza nim;
- brak zmian w backendzie KIO ani logice uprawnień NTFS/fuseblk.

## 0.15.0

- dodano dolny, zwijany panel menedżera operacji plikowych;
- aktywne zadania KIO są prezentowane jako lista/kolejka operacji;
- osobny pasek postępu dla każdego zadania oraz zbiorczy pasek postępu;
- wyświetlanie przetworzonych danych, całkowitego rozmiaru i prędkości, jeśli KIO je raportuje;
- wykorzystanie opisu KIO do pokazania źródła i celu operacji;
- anulowanie pojedynczej operacji przez `KJob::kill(KJob::EmitResult)`;
- przycisk anulowania wszystkich aktywnych operacji;
- rozróżnienie stanów: zakończono, anulowano, błąd;
- historia zakończonych operacji z możliwością wyczyszczenia;
- ostrzeżenie przy zamykaniu aplikacji, jeżeli nadal trwają operacje;
- kopiowanie, przenoszenie, drop, wklejanie, Kosz, tworzenie i zmiana nazwy są podłączone do menedżera;
- zachowano poprawki NTFS/fuseblk z 0.14.2.3.


## 0.14.2.3

- usunięto ostrzeganie o NTFS/fuseblk przed wykonaniem jakiejkolwiek zmiany;
- `fuseblk`, `ntfs` i `ntfs3` nie są traktowane jako automatycznie ograniczone;
- powodzenie zmiany praw jest oceniane na podstawie rzeczywistego `chmod` i ponownego odczytu;
- po udanym zapisie na NTFS/fuseblk aplikacja potwierdza, że zmiana została faktycznie zachowana;
- dopiero po odrzuceniu lub rozbieżności pojawia się wskazówka dotycząca opcji montowania;
- dla `ntfs-3g` komunikat wskazuje `permissions`, `.NTFS-3G/UserMapping` oraz problem stałych `uid/gid/umask`;
- zachowano `admin://`/PolicyKit i rekurencyjne `KIO::ChmodJob`.

## 0.14.2.2

- poprawka kompilacji z `QT_USE_QSTRINGBUILDER`;
- naprawiono typ zwracany przez komunikat o systemie plików.

## 0.14.2.1

- hotfix kompilacji 0.14.2;
- usunięto błędne dosłowne sekwencje `\n` w wygenerowanym C++.

## 0.14.2

- NTFS/ntfs3/fuseblk nie są automatycznie blokowane w zakładce Uprawnienia;
- odczyt praw przez KIO i kontrola właściciela;
- `Odblokuj jako administrator` dla elementów należących do innego użytkownika;
- zapis przez zwykłe KIO lub `admin://`/PolicyKit;
- weryfikacja zmian przez ponowny odczyt;
- rekurencyjne uprawnienia folderów przez `KIO::ChmodJob`.

## 0.14.1

- ujednolicony wygląd i zachowanie split view.
