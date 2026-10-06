[English](CHANGELOG.md) | [Polski](CHANGELOG.pl.md)

# Changelog

## 0.39.0 — Natywna integracja plików i bogate podglądy

### Dodano
- Natywną obsługę KDE/KIO Open/Execute dla plików lokalnych, z prawidłowym rozwiązywaniem adresów `file://` i obsługą lokalnych plików wykonywalnych przez skojarzenia systemowe.
- Miniatury obrazów i wideo oparte na KIO, natywne podglądy ikon plików wykonywalnych Windows oraz podglądy zawartości folderów ze standardowymi ikonami MIME/folderu jako fallbackiem.
- Ustawienie **Widok → Pokaż podglądy**, zapisywane osobno dla każdej lokalizacji przez `DirectoryViewProfile` w panelu głównym, Split View i wynikach wyszukiwania.

### Zmieniono
- Dodano asynchroniczne przetwarzanie partiami, ograniczony cache, tokeny generacji i planowanie wyłącznie widocznych elementów w dużych katalogach przez `PreviewController`.
- Generowanie podglądów pozostaje wyłącznie lokalne; lokalizacje zdalne nie planują zadań podglądu.

### Naprawiono
- F5 unieważnia stan podglądów przed odświeżeniem, zapobiegając użyciu nieaktualnych miniatur z cache.
- Anulowanie zadań podglądu i cykl życia obiektów są deterministyczne podczas nawigacji, odświeżania i zamykania aplikacji.

## 0.38.0 — Narzędzia pamięci masowej (Storage Tools)

### Dodano
- **Silnik skanowania pamięci masowej (Storage Scan Core):** Wysokowydajny, asynchroniczny silnik skanowania katalogów w tle (`StorageScanJob`, `StorageScanWorker`) z licznikami w czasie rzeczywistym, responsywnym anulowaniem, zatrzymywaniem na granicach punktów montowania (`st_dev`), ścisłym niepodążaniem za dowiązaniami symbolicznymi (symlinks), deduplikacją twardych dowiązań (hardlinks: `st_dev` + `st_ino`) oraz agregowanym raportowaniem błędów.
- **Największe pliki i foldery (Top-N):** Natychmiastowa analiza zajętości przestrzeni prezentująca największe pliki i podkatalogi z konfigurowalnym limitem (Top 25/50/100/Wszystkie), sortowaniem według rozmiaru logicznego lub przydzielonego na dysku, przechodzeniem do podfolderów oraz akcją „Pokaż w folderze”.
- **Narzędzia sum kontrolnych (Hash Utilities):** Beztrybowe okno sum kontrolnych (`HashUtilitiesDialog`) obsługujące SHA-256, SHA-1 i MD5 przy użyciu blokowego, anulowalnego hashowania strumieniowego w wątku roboczym (`ChecksumJob`), z paskiem postępu na żywo, kopiowaniem do schowka i wykrywaniem równoległej modyfikacji pliku.
- **Wyszukiwarka duplikatów (Duplicate Finder):** Wysoce wydajne wyszukiwanie zduplikowanej zawartości (`DuplicateFinderJob`) wykorzystujące filtrowanie po rozmiarze, deduplikację tożsamości fizycznej (hardlinks) oraz sekwencyjną weryfikację skrótem SHA-256 wyłącznie dla kolidujących grup bajtów.
- **Bezpieczny przegląd i akcje (Safe Review & Actions):** Interaktywny kontroler rozwiązywania duplikatów (`DuplicateActionController`) zintegrowany z natywnym `FileActions` i `OperationManager`. Obsługuje przenoszenie do kosza oraz do innego katalogu z obowiązkowymi dialogami potwierdzenia, ścisłą rewalidacją migawki przed wykonaniem akcji, pełnym wsparciem cofania (Undo) przez `KIO::FileUndoManager` i wykluczaniem nieaktualnych/zmodyfikowanych plików.
- **Interaktywna mapa zajętości (Treemap):** Czysto pamięciowy silnik układu Squarified Treemap (`StorageTreemapLayout`, `StorageTreemapWidget`) wizualizujący przestrzeń katalogu bez ponownego skanowania systemu plików i bez próbkowania typów MIME. Oferuje kanoniczną własność fizyczną dla trybu przydzielonego miejsca (allocated), tryb rozmiaru logicznego, interaktywne zagłębianie się w hierarchię, etykiety tooltip oraz nawigację klawiaturą.

### Zmieniono
- Uogólniono backend obliczania sum kontrolnych, unifikując hashowanie między oknem Właściwości a samodzielnymi Narzędziami sum kontrolnych.
- Ujednolicono przepływ beztrybowych okien narzędzi pamięci masowej ze spójnym stylem i responsywnym układem.

### Naprawiono
- Rozwiązano problem zamykania procesu, w którym beztrybowe dialogi mogły omijać `WA_DeleteOnClose` i uniemożliwiać zakończenie pętli zdarzeń przez `QApplication::lastWindowClosed`.

## 0.37.0 — Właściwości (Properties 2.0)

- **Model danych i informacje ogólne:** Zastąpiono statyczny dialog asynchronicznym modelem danych (`PropertiesDataProvider`). Prezentacja informacji lokalnych i zdalnych, rozmiar logiczny oraz faktyczny rozmiar na dysku, numer i-węzła (inode), właściciel i grupa, typ systemu plików, znaczniki czasu (dostęp, modyfikacja, zmiana atrybutów oraz czas utworzenia birth time z `statx` tylko gdy wspierany przez dany system plików) oraz bezpieczne odczytywanie celu dowiązań symbolicznych ze wskaźnikiem uszkodzonego linku.
- **Właściwości dysków i systemów plików:** Dedykowany modeless dialog właściwości woluminów i partycji. Prezentuje typ systemu plików, punkt montowania, aktywne opcje montowania, pojemność / zajęte / wolne miejsce z paskiem zużycia, UUID, ścieżkę urządzenia, nadrzędny dysk fizyczny (NVMe, SATA, USB), status wymienności/hotplug oraz banner informacyjny o odłączeniu urządzenia w trakcie otwartego okna właściwości.
- **Uprawnienia POSIX i edytor ACL:** Standardowa siatka praw POSIX z weryfikacją zapisu; wbudowany edytor oparty o natywne `libacl` obsługujący wpisy nazwanych użytkowników i grup, obliczanie maski, prezentację praw efektywnych, domyślne reguły ACL katalogów (Default ACL) oraz reguły dziedziczenia.
- **Sumy kontrolne SHA-256:** Osobna zakładka sum kontrolnych ze strumieniowym, blokowym obliczaniem skrótu SHA-256 w wątku roboczym, paskiem postępu, natychmiastowym anulowaniem i restartem, kopiowaniem do schowka oraz wykrywaniem modyfikacji pliku w trakcie hashowania.
- **Integracja metadanych KDE KFileMetaData:** Leniwa, asynchroniczna ekstrakcja metadanych biblioteką KF6 `KFileMetaData`, uruchamiana wyłącznie po wejściu na zakładkę Metadane. Podgląd w trybie tylko do odczytu dla formatów graficznych (EXIF), dźwiękowych, wideo oraz dokumentów.
- **Modeless i obsługa wielu okien:** Unifikacja właściwości plików, folderów i dysków do architektury beztrybowej (modeless). Możliwość jednoczesnego otwarcia wielu niezależnych okien bez blokowania okna głównego ThisPC; pełna symetria między panelem głównym a Split View; bezpieczne zamykanie okien podrzędnych przez `PropertiesLifecycle`; ochrona przed zapisem do usuniętych lub zastąpionych plików (`PropertiesIdentity` / `st_dev` / `st_ino`).
- **Odbiór ręczny:** Stage 1–6 FULL MANUAL PASS.
- **Finalna automatyczna regresja:** 46 zestawów / 9690 asercji PASS; build, CLI `thispc-view 0.37.0` i kod wyjścia 0 PASS.

## 0.36.0 — Network & Remote Locations

- Dodano ręczne połączenia zdalne SMB, SFTP, FTP, WebDAV i bezpieczny WebDAV przez natywne zadania i uwierzytelnianie KIO.
- Dodano Saved Remote Locations z dodawaniem, zmianą nazwy, usuwaniem i nawigacją do aktywnego panelu.
- Poświadczenia pozostają pod kontrolą natywnego KIO/KPasswdServer/KWallet; ThisPC nie zapisuje haseł w QSettings, zapisanych i ostatnich lokalizacjach ani historii sesji.
- Dodano potwierdzane sukcesem Recent Locations i reconnect, w tym transakcyjną nawigację zdalną oraz odtwarzanie sesji/historii z niezależną historią Split zachowywaną między kartami i restartami.
- Dodano opcjonalne natywne discovery przez `remote:/` i `smb:/`, bez własnego skanera sieci i bez uzależniania ręcznych połączeń od discovery.
- Odbiór ręczny: Stage 1–5 PASS. Otwarcie pustego `smb:/` może chwilę czekać na timeouty natywnego backendu, ale nie blokuje interfejsu. Ścieżka z rzeczywistym wykrytym serwerem SMB pozostała **NOT TESTED — no discoverable LAN services available**.
- Finalna automatyczna regresja: 39 zestawów / 9075 asercji PASS; build i CLI `thispc-view 0.36.0` PASS.

## 0.35.0 — Drives & Devices

- Stage 1: dynamiczne wykrywanie dysków i urządzeń pamięci masowej w oparciu o KDE Solid; sterowany zdarzeniami `SolidDeviceMonitor` z debouncingiem (250 ms) reaguje na hotplug oraz zmiany zamontowania bez okresowego pollingu i bez migania statusu „Odświeżanie…”.
- Stage 2: wykrywanie i prezentacja odmontowanych woluminów wymiennych na widoku domowym `thispc:/` z dedykowaną ikoną `drive-removable-media` i etykietą „Niezamontowany”; kontroler `DeviceMountController` umożliwia montowanie na żądanie (kliknięcie lub enter) z automatycznym przejściem do zamontowanego katalogu; pasek boczny wyświetla wyłącznie zamontowane woluminy.
- Stage 3: pełna asynchroniczna obsługa cyklu życia urządzeń wymiennych (`DeviceRemovalController`):
  - akcja „Odmontuj” wykonuje czyste odmontowanie systemu plików wyłącznie poprzez natywne asynchroniczne wywołanie QtDBus `org.freedesktop.UDisks2.Filesystem.Unmount`, nie wyłączając zasilania nośnika i zachowując widoczność woluminu;
  - akcja „Bezpiecznie usuń” koordynuje odmontowanie wszystkich zamontowanych partycji danego dysku fizycznego, a po ich pomyślnym odmontowaniu asynchronicznie wywołuje `org.freedesktop.UDisks2.Drive.PowerOff`;
  - obsługa bezpiecznego usuwania działa również dla woluminów już odmontowanych;
  - obsługa wysuwania nośników optycznych („Wysuń”) na urządzeniach ze zdolnością `canEject`;
  - bezpieczne przekierowanie paneli Primary i Split z odmontowanego lub odłączonego punktu montowania z powrotem do `thispc:/`, bez naruszania stanu panelu niepowiązanego.
- Finalna automatyczna regresja: 35 zestawów / 8562 asercje PASS; build, CLI `thispc-view 0.35.0` i manual acceptance PASS.

## 0.34.0 — Explorer UX / View & Navigation Polish

- Stage 2: `--version` działa przed inicjalizacją GUI i kończy się kodem 0; Split ma parity przełączania adres/breadcrumb, a karty dysków zachowują kontrolowane odstępy.
- Stage 3: ukryte pliki i foldery są wizualnie przygaszone bez utraty czytelności selection, hover i focus.
- Stage 4: F2, Rename z menu kontekstowego i wolny drugi click korzystają ze wspólnego inline rename; Batch Rename pozostaje osobną ścieżką.
- Stage 5: dodano routing nawigacji klawiaturą oraz stabilny current/focus dla kart `thispc:/`.
- Stage 6: profile view mode, icon size, sort i grouping są zapisywane per URL; Apply/Remove to subfolders używa snapshotowych reguł przodka bez skanowania drzewa.
- Stage 7: Icons oferuje dziewięć stopni rozmiaru, kontrolki menu oraz `Ctrl++`, `Ctrl+=` i `Ctrl+-` w aktywnym panelu.
- Finalny Split View visual parity polish współdzieli semantykę nagłówków i metryki widoków, utrzymuje deterministyczne kolumny Details, symetryczne profile per-URL i praktycznie identyczną runtime geometry paska adresu.
- Nieblokujący follow-up: dopracować około 1 px/sub-pixel pionowego wyrównania nagłówka/paska oraz położenie active-pane accent line względem szarej ramki.
- Nieblokujący future polish: zmienić `PointingHandCursor` kart folderów i dysków `thispc:/`/Split Home na standardowy `ArrowCursor`, spójny ze zwykłymi listingami.
- Wykrywanie pendrive/removable pozostaje odłożone do 0.35 Drives & Devices. Double-click separatora Split przywracający 50/50 pozostaje przyszłym pomysłem polishowym.
- Finalna automatyczna regresja: 32 zestawy / 8317 asercji PASS; build, finalne CLI `thispc-view 0.34.0` i izolowany install smoke PASS.

## 0.33.0 — Architecture Cleanup Continuation

- Kontynuowano behavior-preserving modularizację bez dodawania nowych funkcji użytkowych.
- Wydzielono `LocationPresentation`, `NavigationHistory`, `PreviewCoordinator`, `ActionStateController`, `DirectoryListingCore`, `PaneMenuController` i `DriveHomeCoordinator` z dotychczasowych ścieżek okna i paneli.
- Panel główny i Split View korzystają ze wspólnego `DirectoryListingCore`, zachowując własny stan oraz istniejącą semantykę KIO.
- Poprawiono cykl życia menu kontekstowego sidebara podczas odświeżania dysków, eliminując crash od nieaktualnych akcji.
- Poprawiono aktywację inline rename: szybki double-click nadal otwiera element, a wolny drugi click uruchamia edycję nazwy.
- Finalna regresja po poprawce inline rename: 31 zestawów testowych / 7852 asercje PASS.
- Wspólny `BrowserPane` oraz odłożone elementy UX pozostają poza zakresem 0.33.0.

## 0.32.0 — Architecture Cleanup

- Zakończono behavior-preserving cleanup architektury bez dodawania nowych funkcji użytkowych.
- Rozdzielono Split Sync na acykliczne modele, executor i dialogi, zachowując semantykę 0.31.0.
- Wydzielono współdzielone widgety aplikacji i arkusz stylów oraz utwardzono granice test harnessu.
- Dodano jawny kontrakt aktywnego panelu przez `PaneContext` i `PaneAdapter`.
- Wydzielono `TabController`, `SearchUiController` i `SelectionMenuController` z `ThisPcWindow`.
- Wydzielono rzeczywisty widget `PrimaryBrowserPane`, który posiada stan URL, listing KIO, renderowanie i cache miniatur panelu głównego.
- Poprawki regresji objęły hover/focus oraz crash wykryty podczas ekstrakcji; nie zmieniają deklarowanego zakresu funkcjonalnego.
- Wspólny `BrowserPane` (Stage 9) świadomie przeniesiono do 0.33; nie jest częścią 0.32.0.
- Finalna regresja po Stage 8: 26 zestawów testowych / 6662 asercje PASS.
- Korekta arytmetyczna po wydaniu 0.31.0: pełna regresja obejmuje 25 zestawów i 6569 asercji (6278 wcześniejszych + 291 `split_compare`), a nie 6618. Tag i commit wydania pozostają bez zmian.

## 0.31.0 — Split View Synchronization

- Dodano bezpieczne porównywanie i synchronizację paneli Split View (skrót `Ctrl+Alt+C`, akcja w menu Narzędzia / Widok).
- **Stage 1 (Porównanie paneli):** asynchroniczne porównywanie bezpośrednich elementów lewego i prawego panelu z klasyfikacją (Takie same, Tylko po lewej, Tylko po prawej, Zmienione) na podstawie metadanych KIO (rozmiar, mtime, typ wpisu), z dopasowaniem nazw Unicode i wielkości liter. Wirtualne ścieżki (`thispc:/`, `thispcsearch:/`) są bezpiecznie blokowane.
- **Stage 2 (Podgląd planu):** deterministyczny generator planu synchronizacji dla kierunków Lewy → Prawy oraz Prawy → Lewy z klasyfikacją akcji (Bez zmian, Kopiuj, Zaktualizuj, Konflikt, Nieobsługiwane).
- **Stage 3 (Bezpieczne wykonanie):** asynchroniczne kopiowanie i aktualizacja plików (`LocalFileCopyJob`) z zerem operacji usuwania (brak Delete/Mirror), pomijaniem rekurencji katalogów oraz ochroną konfliktów i nieobsługiwanych wpisów.
- **Bezpieczeństwo operacji dyskowych:** atomowy preflight bezpośrednio przed mutacją każdego pliku (walidacja ścieżek, uprawnień, odrzucanie `.` i `..`).
- **Deterministyczny cykl życia Anulowania (Cancel):** eliminacja wyścigu liczników (brak błędu off-by-one), oczekiwanie na sygnał terminalny aktywnego wątku roboczego i 100% zgodność raportu UI z fizycznym stanem systemu plików (potwierdzona audytem na 20 000 plików).
- **Automatyczne odświeżanie:** natychmiastowe ponowne porównanie paneli po zakończeniu synchronizacji.
- Weryfikacja: focused `split_compare` 291 asercji PASS; focused pane actions 741 asercji PASS; pełna regresja: 25 zestawów testowych / 6569 asercji PASS (korekta arytmetyczna po wydaniu); 100% manual acceptance PASS dla Stage 1, Stage 2 i Stage 3 (w tym audyt Cancel na 20 000 plików).

## 0.30.0 — Grouping and per-folder view settings

- Zapamiętywanie Icons/List/Details/Compact per znormalizowany URL folderu,
  współdzielone przez panel główny, Split View, karty i restore session.
- Uporządkowane menu Widok, cztery rozmiary ikon 96/64/48/32 px i gęsty widok Compact.
- Grupowanie według typu, daty modyfikacji i rozmiaru w Icons/List/Details/Compact,
  z prezentacyjnymi nagłówkami wyłączonymi z akcji plikowych.
- Data używa lokalnych granic kalendarza i lekkiego przeliczenia po północy;
  rozmiar używa istniejących metadanych KIO bez rekurencyjnego skanowania.
- Bieżący klucz i kierunek sortowania są zachowane wewnątrz grup; osobne grupy
  obsługują brakujące metadane, a Size rozdziela foldery i nieznane rozmiary.
- Poprawiono obrys zaznaczenia w Icons oraz zalegający hover w widoku pogrupowanym,
  który wizualnie przypominał drugie zaznaczenie mimo prawidłowego selection modelu.
- Ręczny odbiór Type/Date/Size, wszystkich czterech trybów, Split View, Search,
  trwałości, sortowania i zaznaczania został potwierdzony przed finalnym RC.

## 0.29.0 — Inteligentne nazwy nowych elementów

- Pliki, foldery i elementy z szablonów proponują pierwszą wolną nazwę przed
  otwarciem edytowalnego dialogu; numeracja uzupełnia luki i zachowuje pełne
  rozszerzenia, nazwy ukryte oraz Unicode.
- Drugi preflight po dialogu blokuje wykrytą kolizję. Końcowe operacje nie
  używają `Overwrite` ani `Resume` i zachowują dotychczasową rejestrację Undo/Redo.
- Zdalne KIO używa dwóch asynchronicznych listingów, zachowuje cel z panelu,
  który rozpoczął akcję, i odrzuca błędy listowania w trybie fail-closed.
- Zdalny preflight nie rezerwuje atomowo nazwy. Ostateczna ochrona zależy od
  kontraktu no-overwrite workera/protokołu; nie deklarujemy obsługi wszystkich
  protokołów, odporności na awarię połączenia ani wyeliminowania TOCTOU.
- Podstawowy odbiór ręczny Stage 1 i Stage 2 został potwierdzony; szczegółowy
  zakres i ograniczenia opisuje `RELEASE_NOTES_0.29.0.md`.

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
