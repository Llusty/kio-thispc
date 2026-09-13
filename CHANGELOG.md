# Changelog

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
