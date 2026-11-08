[English](ROADMAP.md) | [Polski](ROADMAP.pl.md)

# Plan rozwoju (ROADMAP)

Kolejność prac priorytetyzuje poprawność działania, bezpieczeństwo danych i łatwość utrzymania podstawowych funkcji menedżera plików przed zaawansowanymi udogodnieniami.

## Ukończony stan bazowy
- **0.16.0.1 — Cofnij / Ponów (Undo / Redo)** ✅ potwierdzone przez użytkownika
  - Kopiowanie, przenoszenie, zmiana nazwy, przenoszenie do Kosza, tworzenie nowego folderu i nowego pliku tam, gdzie obsługuje to KIO;
  - Skróty `Ctrl+Z`, `Ctrl+Y`, `Ctrl+Shift+Z`;
  - Poprawka cofania zmiany nazwy z wykorzystaniem `KIO::moveAs()` + `recordCopyJob()`.
- **0.17.0.5 — Pełne Przeciągnij i upuść (Drag & Drop) oraz parytet Split View** ✅ potwierdzone przez użytkownika
  - Upuszczanie na tło katalogu, foldery i karty; transfery między panelem głównym a Split View (`Ctrl` = Kopiuj, `Shift` = Przenieś);
  - Aktywacja karty po przytrzymaniu kursora podczas przeciągania;
  - Skróty klawiszowe i menu kontekstowe kierowane do aktywnego panelu;
  - Ochrona przed cyklami katalogów (kopiowaniem/przenoszeniem folderu do samego siebie lub potomka) oraz integracja z Cofnij/Ponów.
- **0.18.0 — Obsługa konfliktów kopiowania i przenoszenia** ✅ ukończone, wydane (`v0.18.0-stable`)
  - Interaktywne okno rozwiązywania konfliktów KIO/KIOWidgets dla schowka, przeciągania i upuszczania (Drag & Drop) oraz menu „Wyślij do”;
  - Opcje nadpisania, pominięcia, zmiany nazwy lub użycia automatycznie zaproponowanej nowej nazwy;
  - Zapamiętywanie decyzji przy wielu konfliktach w ramach operacji;
  - Porównanie rozmiaru i czasu modyfikacji pliku źródłowego oraz docelowego;
  - Obsługa konfliktów zagnieżdżonych przez `KIO::CopyJob`;
  - Czyste anulowanie operacji bez wyświetlania wtórnego okna błędu.

---

## Ukończone wydania

### 0.19.0.4 — Przywracanie sesji i pełne nazwy plików ✅ ukończone, wydane (v0.19.0.4)
- Przywracanie otwartych kart, aktywnej karty, lokalizacji oraz historii nawigacji panelu głównego;
- Przywracanie stanu widoku podzielonego (Split View) osobno dla każdej karty, w tym lokalizacji, trybu widoku i sortowania prawego panelu;
- Przywracanie proporcji podziału (szerokości separatora);
- Opcja w menu Widok pozwalająca ponownie otworzyć poprzednią sesję;
- Rozwijanie zaznaczonej ikony w celu wyświetlenia pełnej, zawijanej nazwy pliku;
- Przełącznik paska narzędzi `Pełne nazwy` (`Full names`) trwale zapisywany między uruchomieniami;
- Naprawiona sztywna siatka widoku ikon (`IconMode`) i ograniczony układ wielowierszowych nazw plików;
- Rozwinięcie pełnej nazwy zaznaczonego elementu nie zmienia geometrii wiersza ani pozycji sąsiednich ikon.

### 0.20.0 — Szybki dostęp / Ulubione / Ostatnie lokalizacje ✅ ukończone, wydane (v0.20.0)
- Przypinanie i odpinanie folderów;
- Zmiana kolejności metodą przeciągnij i upuść z trwałym zapisem porządku;
- Sekcja Szybki dostęp / Ulubione w panelu bocznym;
- Trwała lista ostatnio odwiedzanych lokalizacji;
- Opcjonalna lista ostatnich plików odłożona do późniejszego etapu.

### 0.21.0 — Refaktoryzacja architektury i stabilizacja ✅ ukończone, wydane (v0.21.0)
- Wydzielenie modułów `DirectoryView`, `SplitBrowserPane`, `Sidebar`, `SessionManager`, `OperationManager`, `UndoController`, `PropertiesDialog`, `SearchController` oraz `FileActions`;
- Przeniesienie wspólnych funkcji pomocniczych i typów przeglądarki do `browsercommon.h`;
- Redukcja duplikacji kodu pomiędzy panelem głównym a widokiem podzielonym (Split View);
- Zmniejszenie rozmiaru pliku `src/thispcview.cpp` z około 14 178 do około 7250 wierszy;
- Dodanie i rozszerzenie testów regresyjnych dla akcji paneli, przeciągania na karty, okna Właściwości, wyszukiwania i operacji plikowych;
- 577 asercji automatycznych zakończonych wynikiem PASS;
- Pełny odbiór manualny na KDE/CachyOS zakończony bez regresji.

### 0.22.0 — Zaawansowane okno transferów ✅ ukończone, wydane (v0.22.0)
- Zwykłe okno z możliwością przesuwania i minimalizacji otwierane automatycznie dla aktywnych operacji;
- Prezentacja bieżącego pliku, podsumowania źródła/celu, przetworzonego i całkowitego rozmiaru, wyraźnego postępu procentowego, aktualnej/średniej prędkości oraz szacowanego czasu (ETA);
- Ograniczony wykres prędkości w funkcji czasu;
- Obsługa wielu jednoczesnych operacji w jednym dynamicznie dopasowywanym oknie;
- Responsywne rozwijane szczegóły oraz anulowanie poszczególnych operacji;
- Automatyczne zamykanie okna po zakończeniu ostatniej aktywnej operacji;
- Kompaktowe okienko podręczne (popup) zachowane jako podgląd historii operacji;
- Mylące wstrzymywanie oparte na `KIO::CopyJob::suspend()` zostało odrzucone podczas odbioru manualnego i usunięte.

### 0.23.0 — Natywny lokalny silnik transferów ✅ ukończone, wydane (v0.23.0)
- Blokowe kopiowanie lokalne z rzeczywistym wstrzymywaniem i wznawianiem (Pause/Resume) na znanym przesunięciu bajtowym;
- Przenoszenie między różnymi urządzeniami (cross-device move) oparte na zweryfikowanym kopiowaniu zakończonym usunięciem źródła;
- Bezpieczna obsługa plików częściowych oraz sprzątanie/odzyskiwanie po anulowaniu lub błędzie;
- Obsługa konfliktów, decyzji nadpisz/zmień nazwę/pomiń, katalogów, dowiązań symbolicznych, uprawnień i znaczników czasu;
- Obsługa zapełnienia dysku, odłączenia nośnika oraz błędów wejścia/wyjścia (I/O);
- Integracja postępu ze szczegółowym oknem z wersji 0.22.0 oraz kompaktową historią operacji;
- Integracja z Cofnij/Ponów (Undo/Redo) tam, gdzie zakończona operacja jest bezpiecznie odwracalna;
- Zachowanie KIO dla adresów zdalnych i operacji, w których pozostaje ono właściwym backendem;
- Testy integralności i przerywania transferów przed zastąpieniem jakiejkolwiek lokalnej ścieżki KIO;
- Pełna regresja automatyczna: **842/842 asercji PASS**;
- Pełny odbiór manualny na KDE/CachyOS potwierdzony dla konfliktów, drzew katalogów, dowiązań symbolicznych, metadanych, Pause/Resume, Undo/Redo, przepełnienia dysku, utraty urządzenia i integralności SHA-256.

### 0.24.0 — Ulepszenia UX panelu bocznego i Split View ✅ ukończone, wydane (v0.24.0)
- Równe panele Split View z wyrównanymi paskami ścieżek (breadcrumbs) w każdym panelu oraz jednym wspólnym paskiem narzędzi i panelem bocznym kierowanym do aktywnego panelu;
- Przeciąganie i upuszczanie (Drag & Drop) na obsługiwane Miejsca, urządzenia i elementy Szybkiego dostępu w panelu bocznym;
- Pionowo przewijany panel boczny o zmiennej szerokości z trwałym zapisem szerokości wybranej przez użytkownika oraz skracaniem długich etykiet z podpowiedziami (tooltips);
- Niezależny stan wyszukiwania, wyniki, filtry, postęp i zadania w tle dla obu paneli;
- Pełna prezentacja kart `thispc:/` (Ten komputer) w obu panelach;
- Pełna regresja automatyczna: **1022/1022 asercji PASS**;
- Pełny odbiór manualny etapów 1–4 na KDE/CachyOS zakończony pomyślnie.

### 0.25.0 — Obsługa archiwów i rozbudowane menu Nowy ✅ ukończone, wydane (v0.25.0)

Archiwa:
- Natywna integracja zorientowana na KDE/Ark;
- Obsługa formatów ZIP / 7z / tar.gz tam, gdzie dostępny jest odpowiedni backend;
- Akcje „Wypakuj tutaj”, „Wypakuj do…” oraz „Dodaj do archiwum…”;
- Awaryjne użycie narzędzi CLI wyłącznie w uzasadnionych przypadkach.

Menu Nowy:
- Tworzenie folderu, pustego pliku, dokumentu tekstowego i pliku Markdown;
- Obsługa szablonów użytkownika;
- Akcja „Opróżnij Kosz” z oknem potwierdzenia;
- Poprawka szerokości Split View przy długich ścieżkach;
- Regresja automatyczna Etapu 3: 20 zestawów / 3470 asercji PASS; odbiór manualny zakończony pomyślnie.

### 0.26.0 — Panel podglądu (Preview Pane) ✅ ukończone, wydane (v0.26.0)
- Podgląd obrazów z proporcjonalnym skalowaniem;
- Podgląd plików tekstowych, Markdown, JSON i XML;
- Asynchroniczny podgląd pierwszej strony dokumentów PDF przy użyciu `QtPdf`;
- Naprawa nieprzezroczystego białego tła dla dokumentów PDF;
- Prezentacja metadanych audio i wideo przy użyciu biblioteki `TagLib`;
- Prezentacja metadanych EXIF obrazów przy użyciu biblioteki `Exiv2`;
- Bezpieczny podgląd spisu zawartości archiwów przy użyciu `libarchive`;
- Podsumowania informacji o folderach;
- Skrót `Alt+P` przełączający widoczność panelu podglądu;
- Usprawnienia paska ścieżki (breadcrumbs) w widoku podzielonym (Split View);
- Ochrona przed nieaktualnymi wynikami zadań asynchronicznych;
- Pełna regresja automatyczna: 20 zestawów PASS (Panel podglądu: 106 asercji PASS; układ Split View: 99 asercji PASS);
- Odbiór manualny na KDE/CachyOS potwierdzony przez użytkownika.

### 0.27.0 — Szybki podgląd pod klawiszem Spacja (Quick Look) ✅ ukończone, wydane (v0.27.0)
- Duże tymczasowe okno podglądu pliku bez otwierania powiązanej aplikacji zewnętrznej;
- Klawisz `Spacja` otwiera i zamyka Quick Look wyłącznie wtedy, gdy fokus znajduje się w aktywnym widoku plików; klawisz `Esc` zamyka okno podglądu;
- Zmiana zaznaczonego elementu oraz przełączenie aktywnego panelu Split View automatycznie aktualizują zawartość podglądu przy zachowaniu fokusu w widoku plików;
- Pole wyszukiwania, edycja paska adresu, zmiana nazwy w miejscu (inline rename), okienka podręczne i okna dialogowe zachowują standardowe działanie klawisza `Spacja`;
- Współdzielony backend z Panelem podglądu, spójna lista obsługiwanych formatów, limity bezpieczeństwa oraz ochrona przed nieaktualnymi wynikami;
- Panel podglądu (`Alt+P`) pozostaje w pełni niezależny;
- Pełna regresja automatyczna: **22 zestawy / 3598 asercji PASS** (Quick Look: 19, Panel podglądu: 106, routing paneli: 269, układ Split View: 99);
- Wszystkie 9 scenariuszy odbioru manualnego na KDE/CachyOS potwierdzone przez użytkownika.

### 0.28.0 — Masowa zmiana nazw (Batch Rename) ✅ ukończone, wydane (v0.28.0, wariant A)
- Przedrostek/przyrostek (prefix/suffix), numerowanie, znajdź i zastąp, wyrażenia regularne (regex), zmiana rozszerzeń i wielkości liter oraz podgląd i diagnostyka planu.
- Lokalne łańcuchy i zamiany nazw; obsługa cykli 3+ elementów jest wieloetapowa, a nie atomowa dla całej partii.
- Grupowe Cofnij/Ponów (Undo/Redo) w obsługiwanych scenariuszach; asynchroniczne wykonywanie zamian (swap) i blokada współpracujących modyfikacji między kolejnymi wymianami.
- Ochrona przed nadpisaniem, weryfikacja tożsamości plików i ponowna walidacja preflight.
- Testy wydania: kompilacja PASS, testy ukierunkowane 2234 asercje PASS (w tym Batch Rename 1790), pełna regresja 23 zestawy / 5409 asercji PASS, `git diff --check` PASS.
- Odbiór użytkownika po instalacji: brak zgłoszonych błędów dla przedrostka, przyrostka, znajdź/zastąp i `Ctrl+Z` dla wielu plików. **Nie oznacza to pełnego ręcznego odbioru wszystkich scenariuszy GUI.**
- Produkcyjne automatyczne odzyskiwanie (recovery v2) obejmuje wyłącznie kwalifikujące się lokalne operacje liniowe Execute/Undo/Redo. Automatyczne odzyskiwanie zamian (swap) i cykli nie jest jeszcze włączone; brak gwarancji przy zaniku zasilania, dla zdalnych adresów URL/KIO oraz przy równoległej ingerencji niekooperujących procesów.
- Szczegółowy zakres i ograniczenia: `RELEASE_NOTES_0.28.0.md`; przypadki i status testów: `TEST_CHECKLIST.md`; historia etapów: `docs/BATCH_RENAME_STAGE1.md`, `docs/BATCH_RENAME_STAGE2.md`, `docs/BATCH_RENAME_STAGE3.md` i `docs/BATCH_RENAME_CRASH_RECOVERY_STAGE*.md`.

### 0.29.0 — Inteligentne tworzenie plików i folderów ✅ ukończone, wydane (v0.29.0)

**Cel:** przy tworzeniu nowego elementu wykrywać zajętą nazwę przed otwarciem okna i od razu wstawiać pierwszą wolną propozycję do pola nazwy.

**Stan wydania:** Etap 1 (lokalny) i Etap 2 (zdalne KIO) zostały zaimplementowane, a ich podstawowy przepływ manualny został potwierdzony przez użytkownika. Etap 2 używa dwóch asynchronicznych listingów i końcowych operacji bez flag `Overwrite` ani `Resume`. Nie oznacza to testu awarii połączenia, wszystkich protokołów ani atomowej rezerwacji; granice opisuje `docs/NEW_ITEM_NAMING_STAGE2.md`. Wersja 0.29.0 została wydana jako commit `db85e9c` / `e56acee`, oznaczona tagiem `v0.29.0`, zainstalowana i ręcznie potwierdzona przez użytkownika. Pełna regresja wydania: 23 zestawy / 5430 asercji PASS.

- Dla zajętej nazwy proponować `Nowy dokument (1).txt`, następnie `(2)`, `(3)` itd.; przy braku kolizji pozostawiać nazwę domyślną.
- Numer umieszczać przed rozszerzeniem; poprawnie obsługiwać nazwy katalogów, szablony, pliki z wieloma kropkami oraz pliki ukryte (np. `.gitignore`).
- Wpisywać propozycję w edytowalne pole jeszcze przed zatwierdzeniem. Po ręcznej edycji nie zastępować wpisanej nazwy bez wiedzy użytkownika.
- Przed utworzeniem ponownie sprawdzać kolizję i stosować operację niewymuszającą nadpisania. Jeśli inny proces zajmie nazwę w międzyczasie, przedstawić nową propozycję lub czytelny komunikat konfliktu; nigdy nie nadpisywać automatycznie.
- Uwzględnić zarówno pliki, foldery, jak i pozycje tworzone z szablonów, o ile ich backend zapewnia bezpieczną obsługę konfliktu. Dla zdalnych KIO osobno określić zachowanie i ograniczenia zamiast zakładać gwarancje lokalnego systemu plików.
- Działać względem właściwego katalogu i aktywnego panelu Split View, niezależnie od trybu ikon/listy/szczegółów.
- Testy: brak konfliktu, luki w numeracji, zajęte kolejne numery, rozszerzenia i Unicode, foldery, szablony, dowiązania symboliczne, ręczna edycja, jednoczesne utworzenie nazwy przez inny proces, oba panele i brak utraty istniejących danych.
- Kryterium odbioru: użytkownik widzi wolną nazwę od razu, może ją zmienić, a żaden test kolizji nie nadpisuje istniejącego elementu.

### 0.30.0 — Grupowanie i ustawienia widoku dla poszczególnych folderów ✅ ukończone, wydane (v0.30.0)

**Etap 1 — tryb widoku przypisany do folderu: zaimplementowany i ręcznie zaakceptowany**
- Zapamiętywanie trybów Ikony/Lista/Szczegóły według znormalizowanego adresu URL katalogu;
- Wspólna preferencja dla panelu głównego, Split View, kart i przywracania sesji;
- Obsługa lokalnych i zdalnych adresów URL KIO, bez skanowania zawartości i bez zależności od metadanych;
- Bezpieczny powrót (fallback) do dotychczasowego ustawienia globalnego;
- Regresja ukierunkowana po Etapie 1b: 15 asercji ustawień + 300 asercji paneli/menu PASS;
- Szczegóły i granice: `docs/GROUPING_VIEW_SETTINGS_STAGE1.md`.

**Etap 1b — porządkowanie menu Widok: zaimplementowany i ręcznie zaakceptowany**
- Ikony/Lista/Szczegóły pozostają jedną grupą przełączników (radio) i są kierowane do aktywnego panelu;
- Podmenu Pokaż zawiera wyłącznie działające opcje: ukryte elementy, miniatury, panel podglądu i pełne nazwy, z zachowaniem stanów oraz skrótów `Ctrl+H` / `Alt+P`;
- Ten sam układ obowiązuje w menu kontekstowym Widok; sortowanie, wyszukiwanie, KIO, karty, Split View, sesja i zapamiętywanie ustawień dla folderu zachowują dotychczasowy kontrakt;
- Kompilacja PASS; pełna regresja: 24 zestawy / 5456 asercji PASS.

**Etap 1c — rozmiary ikon: zaimplementowany i ręcznie zaakceptowany**
- Cztery rzeczywiste poziomy: Bardzo duże (96 px), Duże (64 px), Średnie (48 px) i Małe (32 px);
- Rozmiar Duże zachowuje dotychczasową geometrię i stanowi bezpieczne ustawienie domyślne;
- Rozmiar jest zapamiętywany według znormalizowanego adresu URL folderu i współdzielony przez panel główny, Split View, karty oraz przywracanie sesji;
- Menu główne i kontekstowe kierują zmianę do aktywnego panelu, a stan przełącznika podąża za panelem; Lista i Szczegóły zachowują dotychczasowe rozmiary;
- Każda wielkość ikon ma stałą siatkę; rozwinięcie pełnej nazwy zaznaczonego elementu nie zmienia geometrii elementów ani położenia sąsiadów;
- Kompilacja PASS; regresja ukierunkowana: 23 asercje ustawień + 305 asercji paneli/menu PASS; pełna regresja: 24 zestawy / 5469 asercji PASS.

**Etap 1d — widok kompaktowy: zaimplementowany i ręcznie zaakceptowany**
- Czwarty tryb widoku przypisywany do folderu, dostępny w panelu głównym i Split View;
- Gęste, jednowierszowe elementy z ikoną 20 px, rozmieszczane w kolumnach wypełnianych od góry;
- Stabilna szerokość komórki i skracanie nazw zapewniają przewidywalny koszt układu;
- Natywna nawigacja wizualna Qt, zaznaczanie, aktywacja, menu kontekstowe i przeciąganie (DnD) korzystają z tych samych ścieżek co Lista; rozwinięcie pełnej nazwy pozostaje wyłączne dla widoku Ikon.

**Suwak rozmiaru ikon — przyszłe oddzielne zadanie po 0.30.0 (zrealizowane w 0.34.0)**
- Około 8–10 stopni, wyłącznie dla trybu Ikon;
- Ustawienie dla folderu i aktywnego panelu, opcjonalnie `Ctrl` + kółko myszy;
- Suwak nie przełącza widoku na Listę, Szczegóły ani Kompaktowy.

**Dalsze funkcje menu Widok — osobny backlog poza Etapem 2 grupowania**
- Widoki Kafelki/Zawartość (Tiles/Content) wymagają nowych układów i testów wszystkich backendów;
- Panel szczegółów jest odrębny od istniejącego Panelu podglądu;
- Pola wyboru (checkboxes) i przełącznik rozszerzeń plików wymagają nowych zachowań modelu i prezentacji nazw;
- Panel nawigacji istnieje, lecz przełącznik jego widoczności z trwałym stanem znajduje się w backlogu pomysłów przyszłościowych.

**Nawigacja klawiaturą — przyszłe zadanie po 0.30.0 (zrealizowane w 0.34.0)**
- Obsługa strzałek w widoku `thispc:/` obok zwykłych folderów;
- Klawisz `Enter` otwiera folder lub uruchamia plik, `Backspace` przechodzi do katalogu nadrzędnego, `Alt+Left` / `Alt+Right` obsługują historię, a `Alt+Up` przechodzi do katalogu nadrzędnego;
- Skróty działają w aktywnym panelu Split View, na kartach oraz w trybach Ikony/Lista/Szczegóły/Kompaktowy z poprawnym odtwarzaniem fokusu po nawigacji;
- Pola wyszukiwania i paska adresu, zmiana nazwy w miejscu (inline rename) oraz okna dialogowe zachowują własną obsługę klawiatury.

**Etap 2 — grupowanie: zaimplementowany i ręcznie zaakceptowany**
- Grupowanie według typu używa rzeczywistych kategorii modelu KDE w widokach Ikony/Lista/Kompaktowy oraz niewybieralnych wierszy sekcji w widoku Szczegóły; nagłówki nie mają adresu URL ani roli pliku i nie trafiają do akcji plikowych, schowka, przeciągania (DnD), Panelu podglądu, Quick Look ani menu elementu;
- Ustawienie `Brak / Typ / Data / Rozmiar` jest zapamiętywane dla znormalizowanego adresu URL i działa w panelu głównym, Split View, kartach, wynikach wyszukiwania, lokalizacjach KIO oraz przy przywracaniu sesji;
- Bieżące sortowanie jest wykonywane przed stabilnym podziałem na grupy, dzięki czemu kolejność wewnątrz każdej sekcji pozostaje zgodna z kluczem i kierunkiem sortowania;
- Grupowanie według daty używa czasu modyfikacji z metadanych KIO i rozłącznych przedziałów lokalnego kalendarza: Przyszłe, Dzisiaj, Wczoraj, Ten tydzień, Poprzedni tydzień, wcześniejsze okresy oraz Nieznana data (tydzień zaczyna się w poniedziałek);
- Osobny zegar każdego panelu przelicza kategorie daty po następnej lokalnej północy (z uwzględnieniem zmian czasu DST 23/25 h) bez synchronicznego skanowania dysku;
- Grupowanie według rozmiaru używa `UDS_SIZE` dostarczonego przez KIO; granice przedziałów są binarne, rozłączne i udokumentowane, a foldery oraz elementy o nieznanym rozmiarze mają osobne grupy;
- Grupowanie nie wykonuje rekurencyjnego skanowania, synchronicznego `stat` ani dodatkowych zdalnych żądań KIO;
- Ręczny odbiór grupowania według typu, daty i rozmiaru potwierdził działanie w trybach Ikony/Lista/Szczegóły/Kompaktowy, Split View, wyszukiwaniu, menu kontekstowym nagłówków, sortowaniu, trwałości ustawień dla folderu i zaznaczaniu;
- Poprawiono obrys zaznaczenia i zalegające podświetlenie (hover) w widoku pogrupowanym przez wyliczanie stanu spod kursora myszy;
- Finalny stan wydania: kompilacja PASS, testy ukierunkowane 778 asercji PASS, pełna regresja 24 zestawy / 6278 asercji PASS, `git diff --check` i audyt wersji PASS. Wydanie zaakceptowane przez użytkownika.

### 0.31.0 — Synchronizacja widoku podzielonego (Split View Synchronization) ✅ ukończone, wydane (v0.31.0)

**Etap 1 — porównywanie paneli w trybie tylko do odczytu: zaimplementowany i zaakceptowany ✅**
- Porównywanie bieżącego folderu lewego i prawego panelu Split View (wyłącznie bezpośrednie elementy potomne, bez rekurencji);
- Klasy wyniku: Identyczne (`Same`), Tylko po lewej (`Only left`), Tylko po prawej (`Only right`), Zmienione (`Changed`);
- Dopasowanie nazw z rozróżnianiem wielkości liter i pełną obsługą Unicode;
- Porównywanie metadanych KIO (rozmiar, czas modyfikacji `mtime`, typ wpisu) bez obliczania skrótów (hashy);
- Foldery o tej samej nazwie klasyfikowane jako `Same` na poziomie obecności w bieżącym katalogu (bez porównania rekurencyjnego);
- Asynchroniczne listowanie KIO z unieważnianiem spóźnionych sygnałów i anulowaniem poprzednich generacji;
- Blokada porównywania dla `thispc:/` oraz wyników `thispcsearch:/` z czytelnym komunikatem;
- Całkowity brak modyfikacji plików (100% tylko do odczytu);
- Szczegóły i semantyka: `docs/SPLIT_SYNC_STAGE1_COMPARE.md`.

**Etap 2 — podgląd planu synchronizacji: zaimplementowany i zaakceptowany ✅**
- Czyste i deterministyczne planowanie akcji synchronizacji dla kierunków Lewy → Prawy oraz Prawy → Lewy;
- Klasy planu: Bez zmian (`NoAction`), Kopiuj (`CopyFile`), Zaktualizuj (`UpdateFile`), Konflikt (`Conflict`), Nieobsługiwane (`Unsupported`);
- Całkowity brak usuwania (brak `Delete` / brak trybu lustrzanego);
- Brak rekurencyjnej synchronizacji katalogów (katalogi po obu stronach = `NoAction`, katalog źródłowy = `Unsupported`);
- Brak dodatkowego wejścia/wyjścia (I/O) i obliczania skrótów (bazuje na modelu `CompareEntry`);
- Zachowanie informacji o duplikatach i brakujących metadanych;
- Modalne okno podglądu planu (`SyncPlanPreviewDialog`) powiązane z numerem generacji;
- Podgląd w 100% tylko do odczytu z przejściem do bezpiecznego wykonania w Etapie 3;
- Szczegóły i semantyka: `docs/SPLIT_SYNC_STAGE2_PLAN_PREVIEW.md`.

**Etap 3 — bezpieczne wykonanie synchronizacji: zaimplementowany i zaakceptowany ✅**
- Bezpieczne asynchroniczne wykonanie planu synchronizacji oparte na `LocalFileCopyJob`;
- Wykonywane są wyłącznie operacje `CopyFile` i `UpdateFile`;
- **Zero usuwania:** brak operacji `Delete` i brak flag trybu lustrzanego;
- **Brak rekurencji:** brak synchronizacji poddrzew katalogów;
- Wpisy konfliktowe (`Conflict`) i nieobsługiwane (`Unsupported`) pozostają w 100% nietknięte;
- Atomowa walidacja preflight bezpośrednio przed mutacją każdego pliku (odrzucanie `.` i `..`, weryfikacja uprawnień);
- Deterministyczna obsługa anulowania (Cancel): eliminacja wyścigu liczników, oczekiwanie na zakończenie aktywnego zadania wątku roboczego i dokładna zgodność raportu UI ze stanem dysku;
- Automatyczne ponowne porównanie paneli po zakończeniu synchronizacji;
- Szczegóły i semantyka: `docs/SPLIT_SYNC_STAGE3_EXECUTION.md`.

**Wyniki testów i weryfikacji 0.31.0:**
- Testy ukierunkowane `split_compare`: 291/291 asercji PASS;
- Testy ukierunkowane akcji paneli (`split_layout`, `split_compare`, `local_transfer`, `transfer_plan`, `local_move`, `actions`, `operations`): 741 asercji PASS;
- Pełna regresja (`run-pane-actions.py --all`): 25 zestawów testowych / 6569 asercji PASS (100%);
- Manualny test akceptacyjny anulowania (20 000 plików): raport 3330 skopiowano / 16 670 anulowano / 0 błędów, na dysku dokładnie 3330 plików, brak plików tymczasowych/częściowych — PASS.

**0.31.0 — UKOŃCZONE**

### 0.32.0 — Porządkowanie architektury (Architecture Cleanup) ✅ ukończone, wydane (v0.32.0)
- Usunięcie cykli zależności w modułach synchronizacji Split View;
- Wydzielenie widżetów i arkuszy stylów aplikacji oraz wzmocnienie punktów rozszerzeń dla zestawów testowych;
- Jawny kontrakt `PaneContext` / `PaneAdapter`;
- Wydzielenie kontrolerów `TabController`, `SearchUiController` oraz `SelectionMenuController`;
- Wydzielenie stanu listowania i renderowania panelu głównego do klasy `PrimaryBrowserPane`;
- Pełne zachowanie dotychczasowego zachowania z poprawkami regresyjnymi i bez zmian funkcjonalnych widocznych dla użytkownika;
- Końcowy stan bazowy Etapu 8: **26 zestawów / 6662 asercje PASS**;
- Dalsze ujednolicanie powłok UI oceniono w wersji 0.33.0.

### 0.33.0 — Kontynuacja porządkowania architektury ✅ ukończone, wydane (v0.33.0)
- Wydzielenie komponentów `LocationPresentation`, `NavigationHistory`, `PreviewCoordinator` oraz `ActionStateController`;
- Współdzielony rdzeń listowania katalogów `DirectoryListingCore` wykorzystywany przez panel główny i Split View;
- Wydzielenie kontrolerów `PaneMenuController` oraz `DriveHomeCoordinator`;
- Naprawienie błędu czasu życia menu kontekstowego dysków w panelu bocznym;
- Naprawienie aktywacji zmiany nazwy w miejscu (inline rename) bez naruszania szybkiego otwierania dwuklikiem;
- Końcowy stan bazowy wydania: **31 zestawów / 7852 asercje PASS**;
- Commit wydania `a3710e466920542aa2a52ac90cdd9a4d3b5c2312`, tag `v0.33.0`;
- Model `DirectoryListingCore` + osobne powłoki UI został przyjęty jako docelowe rozwiązanie architektoniczne.

### 0.34.0 — Dopracowanie UX Eksploratora, widoków i nawigacji ✅ ukończone, wydane (v0.34.0)
- Etap 2 ukończony: stabilna obsługa przełącznika CLI `--version`, parytet paska adresu/ścieżki w Split View oraz uporządkowane odstępy kart dysków;
- Etap 3 ukończony: czytelna prezentacja wizualna ukrytych plików i folderów;
- Etap 4 ukończony: wspólny mechanizm zmiany nazwy w miejscu (inline rename) dla klawisza `F2`, menu kontekstowego „Zmień nazwę” i wolnego drugiego kliknięcia; masowa zmiana nazw (Batch Rename) przy zaznaczeniu wielu elementów pozostaje osobną ścieżką;
- Etap 5 ukończony: pełna nawigacja klawiaturą oraz stabilny fokus w widoku `thispc:/`, kierowane do aktywnego panelu;
- Etap 6 ukończony: profile widoku dla poszczególnych folderów oraz operacje zastosowania/usunięcia profilu dla podfolderów bez rekurencyjnego skanowania KIO i z pierwszeństwem jawnego ustawienia katalogu potomnego;
- Etap 7 ukończony: dziewięć poziomów rozmiaru ikon, kontrolki menu oraz skróty `Ctrl++`, `Ctrl+=` i `Ctrl+-` kierowane do aktywnego panelu;
- Końcowe dopracowanie spójności wizualnej Split View: wspólne nagłówki i metryki widoków Ikony/Lista/Szczegóły/Kompaktowy, deterministyczne kolumny widoku Szczegóły, symetryczne profile dla adresów URL oraz spójna geometria pasków adresu;
- Nieblokujące drobne uwagi kosmetyczne (ok. 1 px różnicy pionowego wyrównania nagłówka oraz ujednolicenie kursora kart `thispc:/` z `PointingHandCursor` do `ArrowCursor`) świadomie odłożone poza zakres 0.34.0;
- Wykrywanie pamięci wymiennych/pendrive'ów zrealizowane w 0.35.0;
- Dwuklik separatora Split View przywracający proporcje 50/50 przeniesiony do sekcji *Pomysły do rozważenia w przyszłości*;
- Pełna regresja: 32 zestawy / 8317 asercji PASS; CLI `thispc-view 0.34.0`, kod wyjścia 0: PASS; commit wydania `2555547`, tag `v0.34.0`.

### 0.35.0 — Dyski i urządzenia (Drives & Devices) ✅ ukończone, wydane (v0.35.0)
- Etap 1 ukończony: dynamiczne wykrywanie dysków i urządzeń przez KDE Solid; sterowany zdarzeniami `SolidDeviceMonitor` z buforowaniem 250 ms eliminuje okresowe odpytywanie (polling) i migotanie komunikatu „Odświeżanie…”;
- Etap 2 ukończony: wykrywanie i prezentacja odmontowanych woluminów wymiennych w widoku `thispc:/` jako „Niezamontowany”; montowanie na żądanie (`DeviceMountController`) po kliknięciu lub naciśnięciu `Enter` z natychmiastowym wejściem do katalogu; panel boczny prezentuje wyłącznie zamontowane woluminy;
- Etap 3 ukończony: pełna asynchroniczna obsługa cyklu życia urządzeń (`DeviceRemovalController`):
  - Akcja „Odmontuj” wykonuje czyste odmontowanie systemu plików przez natywne wywołanie QtDBus `org.freedesktop.UDisks2.Filesystem.Unmount` (bez wyłączania zasilania);
  - Akcja „Bezpiecznie usuń” koordynuje odmontowanie wszystkich zamontowanych partycji dysku oraz asynchroniczne wywołanie `org.freedesktop.UDisks2.Drive.PowerOff`;
  - Obsługa bezpiecznego usuwania dla woluminów już odmontowanych;
  - Obsługa wysuwania nośników optycznych („Wysuń”) na urządzeniach z właściwością `canEject`;
  - Bezpieczne przekierowanie panelu głównego i Split View z odmontowanego punktu montowania z powrotem do `thispc:/`.
- Dla partycji NTFS zachowano zweryfikowane zachowanie zamiast ogólnych założeń;
- Pełna regresja: 35 zestawów / 8562 asercje PASS; CLI `thispc-view 0.35.0`, kod wyjścia 0: PASS.

### 0.36.0 — Sieć i lokalizacje zdalne (Network & Remote Locations) ✅ ukończone, wydane (v0.36.0)
- Obsługa protokołów SMB, SFTP, FTP i WebDAV przez KIO;
- Zapisane lokalizacje zdalne;
- Ostatnie lokalizacje zdalne oraz ponowne łączenie (reconnect) tam, gdzie jest to użyteczne;
- Integracja z usługą KWallet dla poświadczeń za pośrednictwem natywnego KIO (ThisPC nigdy samodzielnie nie przechowuje haseł);
- Wykrywanie usług sieciowych wyłącznie tam, gdzie działa niezawodnie, bez blokowania ręcznego połączenia;
- Odbiór manualny Etapów 1–5: PASS (ścieżka z wykrytym fizycznym serwerem SMB: NOT TESTED z powodu braku usług LAN w środowisku testowym);
- Pełna regresja: 39 zestawów / 9075 asercji PASS; CLI `thispc-view 0.36.0`, kod wyjścia 0: PASS.

### 0.37.0 — Właściwości (Properties) ✅ ukończone, wydane (v0.37.0)
- Etap 1: model danych właściwości + informacje ogólne ✅ ukończone
- Etap 2: właściwości dysków i systemów plików ✅ ukończone
- Etap 3: uprawnienia POSIX + edytor list kontroli dostępu (ACL) ✅ ukończone
- Etap 4: sumy kontrolne SHA-256 ✅ ukończone
- Etap 5: metadane KDE / `KFileMetaData` ✅ ukończone
- Etap 6: niemodalne okna właściwości pliku, folderu i dysku ✅ ukończone
- Odbiór manualny Etapów 1–6: FULL MANUAL PASS.
- Pełna regresja: 46 zestawów / 9690 asercji PASS; CLI `thispc-view 0.37.0`, kod wyjścia 0: PASS.

### 0.38.0 — Narzędzia pamięci masowej (Storage Tools) ✅ ukończone, wydane (v0.38.0)
- Największe katalogi i pliki;
- Skanowanie dysku w tle;
- Widoki wykorzystania przestrzeni Top-N;
- Narzędzia sum kontrolnych SHA-256 / SHA-1 / MD5;
- Wykrywanie duplikatów;
- Bezpieczny przegląd przed usunięciem lub przeniesieniem;
- Interaktywna mapa zajętości (Treemap) z układem squarified, kanoniczną własnością fizyczną i zagłębianiem;
- Naprawa cyklu życia i zamykania niemodalnych okien dialogowych.
- Etap 1: Rdzeń skanowania pamięci masowej (Storage Scan Core) ✅ ukończone
- Etap 2: Największe pliki i katalogi / Top-N ✅ ukończone
- Etap 3: Narzędzia skrótów (Hash Utilities) ✅ ukończone
- Etap 4: Wyszukiwarka duplikatów (Duplicate Finder) ✅ ukończone
- Etap 5: Bezpieczny przegląd i akcje (Safe Review & Actions) ✅ ukończone
- Etap 6: Mapa zajętości, szlif narzędzi pamięci masowej i naprawa cyklu życia ✅ ukończone
- Odbiór ręczny Etapów 1–6: FULL MANUAL PASS.
- Pełna automatyczna regresja: 52 zestawy PASS; kompilacja, CLI `thispc-view 0.38.0`, kod wyjścia 0: PASS.

**Zasady globalne i bezpieczeństwo:**
- **Tylko lokalne ścieżki (Local Only):** rekurencyjne skanowanie pamięci masowej i wyszukiwanie duplikatów w 0.38 dotyczą wyłącznie lokalnych systemów plików. Zdalne zasoby KIO (SFTP, SMB, FTP, WebDAV) są bezwzględnie poza zakresem rekurencyjnego skanowania;
- **Brak podążania za symlinkami (No Symlink Follow):** domyślnie skaner nie podąża rekurencyjnie za dowiązaniami symbolicznymi. Symlink może być uwzględniony jako pojedynczy wpis, lecz jego cel nie jest rekurencyjnie skanowany (zapobieganie pętlom oraz wychodzeniu poza katalog bazowy skanu);
- **Świadomość hardlinków (Hardlink Aware):** dowiązania twarde są śledzone przez stabilną lokalną tożsamość (`st_dev` + `st_ino`). Pojedynczy fizyczny plik nie może być wielokrotnie wliczany do fizycznego zużycia miejsca, a hardlinki wskazujące na ten sam i-węzeł nie mogą być traktowane jako zwykłe duplikaty zawartości;
- **Granice systemów plików (Mount Boundaries):** domyślnie skan nie schodzi automatycznie na inne zamontowane systemy plików znajdujące się pod ścieżką początkową. Polityka opiera się o rzeczywistą tożsamość systemu plików (`st_dev`), a nie heurystykę ścieżek;
- **Rozmiar logiczny vs. alokowany (Logical vs. Allocated Size):** rozróżnianie rozmiaru logicznego oraz rozmiaru alokowanego na dysku (bloki) tam, gdzie system plików udostępnia te dane. Pliki rzadkie (sparse files) są reprezentowane poprawnie, bez zakładania tożsamości rozmiaru logicznego z fizyczną alokacją;
- **Pliki ukryte (Hidden Files):** ukryte pliki i katalogi są domyślnie uwzględniane w obliczaniu całkowitej zajętości przestrzeni; ewentualne ukrycie ich w widoku prezentacji nie zmienia rzeczywistych sum;
- **Błędy i diagnostyka:** odmowa dostępu, zniknięcie pliku w trakcie skanu, brak możliwości odczytu czy przejściowe błędy `stat` są raportowane w zagregowanych licznikach (pominięte, niedostępne, zniknięte, błędy) zamiast wyświetlania uciążliwych okien dialogowych dla każdego pliku;
- **Najpierw tylko do odczytu i brak automatycznego usuwania:** etapy 1–4 to wyłącznie analiza w trybie tylko do odczytu. Jakiekolwiek operacje na plikach są dopuszczalne dopiero w Etapie 5 po jawnym przeglądzie użytkownika. Automatyczne usuwanie („usuń wszystkie duplikaty oprócz jednego”) bez świadomego wyboru jest zabronione.

**Etap 1 — Rdzeń skanowania pamięci masowej (Storage Scan Core):**
- Asynchroniczny silnik rekurencyjnego skanowania lokalnego, zachowujący pełną responsywność interfejsu użytkownika;
- Wyłącznie lokalne ścieżki i systemy plików; zdalne lokalizacje KIO (SFTP, SMB, FTP, WebDAV) są wyłączone z rekurencyjnego skanowania;
- Raportowanie postępu, obsługa anulowania, licznik przeskanowanych elementów oraz licznik przetworzonych bajtów;
- Bezpieczeństwo symlinków: uwzględnienie wpisu bez przechodzenia do celu; ochrona przed pętlami i ucieczką poza korzeń skanu;
- Obsługa hardlinków: stabilna identyfikacja (`st_dev` + `st_ino`) zapobiega zawyżaniu fizycznego zużycia miejsca;
- Kontrola granic montowania: domyślne pozostawanie w obrębie początkowego systemu plików (`st_dev`);
- Metryki rozmiaru: rozmiar logiczny obok alokacji dyskowej (uwzględnienie plików rzadkich);
- Pliki i foldery ukryte domyślnie wliczane do sumarycznej zajętości;
- Zbiorcze raportowanie błędów i pominięć bez wyskakujących okienek;
- Tryb wyłącznie do odczytu; brak jakichkolwiek mutacji na systemie plików.

**Etap 2 — Największe pliki i katalogi / Top-N (Largest Files & Directories / Top-N):**
- Widoki największych plików oraz największych katalogów oparte bezpośrednio na danych z silnika Etapu 1;
- Ranking Top-N, interaktywne sortowanie kolumn oraz przechodzenie w głąb katalogów (drill-down);
- Akcja „Pokaż w folderze” oraz przejście do lokalizacji wybranego elementu;
- Prezentacja rozmiaru logicznego względem alokowanego;
- Ścisła spójność danych z wynikami Storage Scan Core.

**Etap 3 — Narzędzia sum kontrolnych (Hash Utilities):**
- Wspierane algorytmy: SHA-256 (domyślny), SHA-1 (zgodność / weryfikacja integralności), MD5 (zgodność / weryfikacja integralności);
- SHA-1 i MD5 wyraźnie oznaczone jako algorytmy wyłącznie do weryfikacji integralności/zgodności, nigdy jako bezpieczne skróty kryptograficzne;
- Asynchroniczne obliczanie sum kontrolnych dla lokalnych zwykłych plików z paskiem postępu, możliwością anulowania oraz kopiowaniem wyniku do schowka;
- Ponowne wykorzystanie sprawdzonych wzorców z implementacji sum kontrolnych w oknie Właściwości (0.37).

**Etap 4 — Wyszukiwanie duplikatów (Duplicate Finder):**
- Wieloetapowy potok wykrywania:
  1. Wyłącznie lokalne zwykłe pliki (regular files);
  2. Grupowanie kandydatów według dokładnego rozmiaru w bajtach;
  3. Obliczanie skrótu tylko dla grup o identycznym rozmiarze (brak operacji I/O dla plików o unikalnym rozmiarze);
  4. Potwierdzenie duplikatów przez porównanie sumy kontrolnej zawartości.
- Nazwa pliku, rozszerzenie ani znaczniki czasu nigdy nie stanowią dowodu na duplikat zawartości;
- Rozpoznawanie dowiązań twardych: pliki o tym samym i-węźle są identyfikowane i nie są prezentowane jako duplikaty zawartości;
- Minimalizacja obciążenia wejścia/wyjścia (I/O).

**Etap 5 — Bezpieczny przegląd i akcje (Safe Review & Actions):**
- Obowiązkowy interaktywny przegląd przed jakąkolwiek mutacją: użytkownik świadomie zaznacza konkretne elementy; brak automatycznego kasowania „wszystkich prócz jednego”;
- Bezpieczne operacje: Kosz (Trash), Przeniesienie (Move) oraz Pokaż w folderze (Show in folder);
- Wykorzystanie istniejącej, bezpiecznej infrastruktury ThisPC: `FileActions`, `OperationManager`, Cofnij/Ponów (Undo/Redo) oraz zweryfikowanych ścieżek transferu i usuwania (brak surowego, bezpośredniego `unlink()`);
- Wstępna ponowna walidacja (preflight revalidation) celów, chroniąca przed operacjami na plikach usuniętych lub podmienionych w międzyczasie.

**Etap 6 — Końcowy UX narzędzi pamięci masowej i regresja (Final Storage Tools UX & Regression):**
- Dopracowanie spójności interfejsu, układu oraz parytetu między panelem głównym a Split View;
- Dopracowanie obsługi anulowania i płynności paska postępu;
- Przejrzyste podsumowanie diagnostyki i błędów;
- Weryfikacja wydajności i stabilności zużycia pamięci przy operacjach na bardzo dużych strukturach katalogów;
- Pełna automatyczna regresja testowa oraz manualna akceptacja w środowisku KDE/CachyOS;
- **Mapa zajętości (Treemap):** czysto pamięciowa wizualizacja Squarified Treemap z kanoniczną własnością fizyczną dla dowiązań twardych, przełączaniem metryki logicznej, zagłębianiem w poddrzewa, etykietami informacyjnymi i nawigacją klawiaturą;
- **Naprawa cyklu życia i zamykania dialogów niemodalnych:** deterministyczne kaskadowe zamykanie dialogów i obsługa `done(r)` → `close()`, zapewniające poprawne działanie `WA_DeleteOnClose` oraz naturalne zakończenie pętli zdarzeń Qt.

### 0.39.0 — Natywna integracja plików i bogate podglądy (Native File Integration & Rich Previews) ✅ ukończone, wydane (v0.39.0)

**Etap 1 — Uruchamianie plików lokalnych i integracja KIO (Local File Launch / KIO Integration)** ✅ ukończone (testy automatyczne i manualne PASS):
- Standardowy proces KDE otwierania i uruchamiania przez `KIO::OpenUrlJob` (`setShowOpenOrExecuteDialog(true)`, `setRunExecutables(false)`);
- Lokalne pliki `.exe` i binarne respektują skojarzenia systemowe MIME (np. Wine/Bottles) bez błędu blokady `ERR_ACCESS_DENIED`;
- Prawidłowa semantyka adresów lokalnych `file://` oraz przekierowanie wpisów katalogów (`thispc:/` → lokalny punkt montowania);
- Czysty deterministyczny komponent `LaunchUrlResolver` wymuszający ścisłe dopasowanie `drive.id`, leksykalną blokadę wyjścia poza ścieżkę i zachowanie protokołów zdalnych;
- Pełna parzystość między panelem głównym, Split View i wynikami wyszukiwania;
- Bezpieczna polityka dla plików zdalnych; brak własnego launchera Wine, brak wywołań powłoki/`QProcess`, brak omijania zabezpieczeń.

**Etap 2 — Rdzeń podglądów i architektura miniatur (Preview Core / Thumbnail Architecture)** ✅ ukończone (testy automatyczne i manualne PASS):
- Dedykowany moduł koordynacji podglądów i miniatur (`src/thumbnailcontroller.h`);
- Asynchroniczne planowanie zadań przy użyciu natywnego stosu KDE (`KIO::PreviewJob`);
- Priorytetyzacja widocznych elementów i śledzenie widoku paneli;
- Natychmiastowe anulowanie i tokeny generacji przy zmianie katalogu;
- Pamięć podręczna miniatur w pamięci RAM zintegrowana ze standardowym cache KDE;
- Bezpieczne zamykanie okna i obsługa wielu niezależnych instancji.

**Etap 3 — Miniatury obrazów i wideo (Image & Video Thumbnails)** ✅ ukończone (testy automatyczne i manualne PASS):
- Podglądy obrazów: PNG, JPEG, WebP oraz formaty obsługiwane przez systemowe wtyczki;
- Klatki miniatur wideo przez infrastrukturę podglądu KDE (`ffmpegthumbs` / systemowe wtyczki miniatur);
- Respektowanie współczynnika DPI ekranu i rozmiaru ikon bez blokowania wątku interfejsu;
- Płynny powrót (fallback) do standardowej ikony MIME w przypadku braku miniatury.

**Etap 4 — Ikony plików wykonywalnych i binariów (Executable / File Icon Previews)** ✅ ukończone (testy automatyczne i manualne PASS):
- Pobieranie natywnych ikon dla plików Windows `.exe` i binariów PE zgodnie z prezentacją Dolphina;
- Wykorzystanie istniejącej infrastruktury wtyczek miniatur KDE;
- Deterministyczny łańcuch wyboru: miniatura → natywna ikona systemowa → ikona MIME.

**Etap 5 — Podgląd zawartości folderów (Folder Content Previews)** ✅ ukończone (testy automatyczne i manualne PASS):
- Miniaturowy podgląd zawartości folderów (np. foldery ze zdjęciami) w stylu Dolphina;
- Integracja ze standardowym systemem podglądu katalogów KDE bez własnego skanowania dysku;
- Powrót do standardowej ikony katalogu w przypadku braku podglądu lub pustego folderu.

**Etap 6 — Integracja z widokami i UX (View Integration & UX)** ✅ ukończone (testy automatyczne i manualne PASS):
- Jednolita prezentacja podglądów w panelu głównym, panelu podzielonym i wynikach wyszukiwania;
- Obsługa we wszystkich trybach: Ikony, Lista, Szczegóły, Kompaktowy;
- Opcja widoku: `Widok -> Pokaż podglądy` (`View -> Show Previews`);
- Spójność z istniejącym systemem ustawień widoku dla poszczególnych folderów i modelem zapisu.

**Etap 7 — Wydajność, cykl życia i szlif końcowy (Performance / Lifetime / Final Polish)** ✅ ukończone (testy automatyczne i manualne PASS):
- Całkowity brak blokowania wątku GUI i synchronicznego wejścia/wyjścia;
- Kontrola zużycia pamięci; brak generowania miniatur dla tysięcy elementów naraz;
- Podglądy włączone dla systemów lokalnych, domyślnie wyłączone dla lokalizacji zdalnych (`sftp:`, `smb:`, `fish:`);
- Kompletny zestaw testów automatycznych i manualna akceptacja w środowisku KDE Plasma 6.

### 0.40.0 — Rozszerzone właściwości — dalszy rozwój (Advanced Properties Follow-up) ✅ ukończone, wydane (v0.40.0)

Publikacja potwierdzona na GitHubie 2026-10-08: [v0.40.0](https://github.com/Llusty/kio-thispc/releases/tag/v0.40.0), commit `4c87d21651819e18b88c688404ef3625ab7f3352`. Końcowe testy ukierunkowane: 15 zestawów / 4359 sprawdzeń; pełna regresja: 64 unikalne zestawy / 35 064 sprawdzenia / 0 błędów / kod wyjścia 0; `version-cli` 6/6, budowanie pakietu oraz izolowana instalacja/deinstalacja PASS. Zobacz [raport wydania](docs/PROPERTIES_RELEASE_040.md).

- **Etap 1 — Warstwa możliwości (capabilities) Właściwości:** rozpoznawanie backendu i lokalności systemu plików, jawna polityka dowiązań symbolicznych, zrzut tożsamości i ponowna walidacja przed zapisem — testy automatyczne + FULL MANUAL PASS (59 zestawów / 32 873 sprawdzenia / 0 błędów; rzeczywisty test manualny zdalnego KIO: NOT TESTED z powodu braku środowiska zdalnego, nieblokujący przy pełnym pokryciu automatycznym braku lokalnych wywołań systemowych);
- **Etap 2 — Przeglądarka atrybutów rozszerzonych (`xattr`) tylko do odczytu:** odczyt przestrzeni nazw `xattr`, rozróżnianie tekstu i danych binarnych, kopiowanie wartości — testy automatyczne + FULL MANUAL PASS (60 zestawów / 32 968 sprawdzeń / 0 błędów; limity bezpieczeństwa: 64 KiB na wartość, 256 KiB łącznie, 256 wpisów, 1024 jednostki UTF-16 podglądu tekstowego, 64 bajty podglądu binarnego);
- **Etap 3 — Bezpieczna edycja atrybutów `xattr` (`user.*`):** dodawanie, edycja i usuwanie lokalnych atrybutów `user.*` przez zweryfikowany deskryptor pliku, ścisła walidacja UTF-8/hex, blokada edycji przestrzeni `security.*`, `trusted.*`, `system.*` oraz blokada zapisu dla dowiązań symbolicznych i zasobów zdalnych — testy automatyczne (`properties_xattr_edit`: 379 asercji; pełna regresja: 61 zestawów / 33 347 sprawdzeń PASS) + FULL MANUAL PASS;
- **Etap 4 — Numeryczne uprawnienia POSIX (`0755`):** pole oktalne synchronizowane dwukierunkowo z przełącznikami GUI, walidacja wejścia oraz edycja bitów `rwx` z zachowaniem istniejących bitów specjalnych (`setuid`, `setgid`, `sticky`) — testy automatyczne (`properties_posix_mode`: 163 asercje; pełna regresja: 62 zestawy / 33 510 sprawdzeń PASS) + FULL MANUAL PASS;
- **Etap 5 — Semantyka atrybutu ukrycia (Hidden semantics):** audyt systemów Unix, NTFS i innych backendów, prezentacja rzeczywistego stanu ukrycia oraz zapis wyłącznie tam, gdzie semantyka jest jednoznaczna i bezpieczna (w pozostałych przypadkach kontrolka pozostaje nieaktywna / tylko do odczytu) — testy automatyczne (63 zestawy / 34 457 sprawdzeń PASS) + FULL MANUAL PASS (`docs/PROPERTIES_HIDDEN_SEMANTICS.md`);
- **Etap 6 — Końcowa integracja i regresja:** spójność w panelu głównym, Split View i wynikach wyszukiwania, obsługa znikających plików i odmontowania woluminu, testy ext4/Btrfs/NTFS oraz czasu życia okien — AUTOMATED PASS (64 zestawy / 35 064 sprawdzenia PASS) + FULL MANUAL PASS (`docs/PROPERTIES_FINAL_AUDIT_040.md`, `docs/PROPERTIES_STAGE6_INTEGRATION.md`).

Poza zakresem 0.40: edycja etykiet SELinux, `chattr`/immutable, Linux file capabilities, większa przebudowa ACL oraz ponawianie przez `admin://`.

#### Oficjalny kontrakt 0.40 — Rozszerzone właściwości
Intencja wydania: rozbudowa okna Właściwości bez rozszerzania projektu na narzędzie administracji systemem.
- **Etap 1 — Warstwa możliwości Właściwości:** rozpoznawanie backendu/systemu plików, wykrywanie obsługiwanych funkcji, jawna polityka symlinków, weryfikacja tożsamości przed zapisem. Ukończony: testy automatyczne + FULL MANUAL PASS.
- **Etap 2 — Przeglądarka atrybutów rozszerzonych (`xattr`):** odczyt `xattr`, przestrzenie nazw, tekst a dane binarne, kopiowanie wartości, tryb tylko do odczytu. Ukończony: testy automatyczne + FULL MANUAL PASS.
- **Etap 3 — Bezpieczna edycja `xattr`:** lokalne dodawanie/edycja/usuwanie `user.*`, czytelna obsługa błędów, blokada edycji `security.*`, `trusted.*`, `system.*`. Ukończony: testy automatyczne + FULL MANUAL PASS.
- **Etap 4 — Numeryczne uprawnienia POSIX:** pole oktalne, dwukierunkowa synchronizacja z obecnym GUI, walidacja, edycja `rwx` z zachowaniem bitów specjalnych. Ukończony: testy automatyczne + FULL MANUAL PASS.
- **Etap 5 — Semantyka ukrywania plików:** audyt Unix/NTFS/innych backendów, prezentacja rzeczywistego stanu ukrycia, zapis wyłącznie tam, gdzie semantyka jest jednoznaczna i bezpieczna. Ukończony: testy automatyczne + FULL MANUAL PASS.
- **Etap 6 — Końcowa integracja i regresja:** Primary/Split/Search, znikające pliki, odmontowanie woluminu, ext4/Btrfs/NTFS, czas życia okien, pełna akceptacja automatyczna i manualna (`docs/PROPERTIES_FINAL_AUDIT_040.md`).

### 0.40.1 — Właściwości wielu elementów i masowa zmiana uprawnień (Multi-Selection Properties & Bulk Permissions) ✅ ukończone, wydane (v0.40.1)
- Okno właściwości dla wielu zaznaczonych plików i katalogów (`AggregatePropertiesDialog`) w panelu głównym, Split View oraz wynikach wyszukiwania (`Alt+Enter` / menu kontekstowe), wydane jako commit `c0e2f29` (tag `v0.40.1`);
- Zakładka **Ogólne (General)** zliczająca łączną liczbę plików, folderów i dowiązań symbolicznych, całkowity rozmiar logiczny oraz fizyczną alokację bloków systemu plików (`st_blocks`) z deduplikacją dowiązań twardych (`st_dev` + `st_ino`) w asynchronicznym skanerze z obsługą anulowania;
- Zakładka **Uprawnienia (Permissions)** umożliwiająca selektywną, trójstanową modyfikację pojedynczych bitów `rwx` dla bezpośrednio zaznaczonych lokalnych zwykłych plików i katalogów bez rekurencji, z zachowaniem nienaruszonych bitów `rwx` oraz bitów specjalnych (`setuid`, `setgid`, `sticky`) i uwzględnieniem maski ACL jądra;
- Ścisłe bramki bezpieczeństwa: zaznaczenia mieszane z dowiązaniami symbolicznymi (aktywnymi lub uszkodzonymi), zdalnymi adresami KIO lub nieobsługiwanymi systemami plików pozostają w trybie tylko do odczytu; deskryptory `O_PATH | O_NOFOLLOW` weryfikują niezmienność i-węzła oraz punktu montowania przed i po zapisie z obowiązkowym oknem potwierdzenia (domyślny przycisk „Nie”).

### 0.41.0 — Wyszukiwanie zaawansowane i responsywny interfejs wyszukiwania ✅ ukończone, wydane (v0.41.0)

**Zrealizowany zakres w wydaniu 0.41.0 (tag `v0.41.0`, commit `292b8bb`):**
- Filtry nazwy pliku, kategorii, typu MIME, rozszerzenia, zakresu dat modyfikacji i rozmiaru oraz wybór tylko pliki / tylko foldery;
- Jawny zakres wyszukiwania: tylko bieżący folder, bieżący folder + podfoldery lub wybrana lokalizacja/wolumin — bez automatycznego skanowania wszystkich dysków;
- Aktywne filtry łączone przez AND; alternatywne wartości wewnątrz jednego filtra łączone przez OR (np. `jpg`/`png`); nieaktywne filtry nie ograniczają wyników;
- Sortowalne wyniki i spójna integracja Primary/Split/Search z zachowaniem niezależnych zapytań, zadań i stanu `PaneSearchState` podczas akcji „Zamień panele”;
- Asynchroniczne wykonanie na niezależnym silniku workera wyszukiwania (obsługującym również nieindeksowane lokalizacje, wymienne woluminy i partycje NTFS), anulowanie (Cancel), ochrona przed nieaktualnymi wynikami po zmianie zapytania/nawigacji i bezpieczna obsługa utraty montowania.

**Podsumowanie etapów i pozycji odłożonych:**
1. **Etap 1 — Model zapytania i architektura (✅ FULL PASS, wydane w 0.41.0):** Zdefiniowanie kontraktu zapytania, zakresu i filtrów (`SearchQuery`, `SearchPlan`, `SearchMatcher`) oraz oddzielenie modelu zapytania, backendu wykonania i koordynacji UI z pełną akceptacją automatyczną i manualną.
2. **Etap 2 — Filtry zaawansowane (✅ FULL PASS, wydane w 0.41.0):** Wdrożenie filtrów wielokryterialnych oraz logiki AND i wewnątrzfiltrowego OR w niezależnej ścieżce workera z odbiorem automatycznym i manualnym.
3. **Etap 3 — Integracja UI i wyników wyszukiwania (✅ FULL PASS — Etapy 3A–3D.4 zaakceptowane, wydane w 0.41.0):** Kompaktowe, nierozszerzające się pole **Szukaj** (320 px) oraz responsywny panel **SearchPopover (`Zakres | Tryb | Filtry`)** z trybami dopasowania nazwy (`Auto / Tekst / Maska glob`), wskaźnikiem aktywnych filtrów (ikona lejka), osobnym przyciskiem czyszczenia `X`, responsywnością Wayland/HiDPI, pełną nawigacją klawiaturą i parytetem Primary/Split.
4. **Etap 4 — Zapisane wyszukiwania (⏸ odłożone poza 0.41.0):** Moduł trwałego zapisu zapytań, zakresów i filtrów nie został wdrożony w 0.41.0 i pozostaje w sekcji *Dalszy rozwój wyszukiwania — bez przypisanej wersji*.
5. **Etap 5 — Warunkowa integracja Baloo (⏸ odłożone poza 0.41.0):** Opcjonalny adapter Baloo został odłożony poza 0.41.0; wydanie 0.41.0 opiera się na niezależnym silniku workera, a ewentualna integracja Baloo pozostaje w sekcji *Dalszy rozwój wyszukiwania — bez przypisanej wersji*.
6. **Etap 6 — Wydajność, bezpieczeństwo i regresja (⏸ częściowo pokryte w regresji Etapów 1–3; osobny cykl Etapu 6 nieuruchomiony przed `v0.41.0`):** Automatyczna regresja dla wydanego zakresu Etapów 1–3 (`search_query`, `search_controller`: granice modułów, anulowanie, ochrona przed nieaktualnymi wynikami, utrata montowania oraz routing Primary/Split) i odbiór manualny Etapów 3A–3D.4 zostały wykonane w commicie `292b8bb` (`v0.41.0`); osobny cykl testów obciążeniowych i między-systemowych (`ext4`/Btrfs/NTFS/nośniki wymienne) Etapu 6 nie był realizowany po odłożeniu Etapów 4–5.

Szybki filtr bieżącego folderu w miejscu (in-place), wyszukiwanie po zawartości plików, zaawansowana logika zapytań AND/OR/NOT, zapisane wyszukiwania oraz opcjonalna integracja Baloo pozostają w backlogu bez przypisanej wersji.

### 0.41.1 — Wykrywanie urządzeń MTP i rozmiary folderów we Właściwościach ✅ ukończone, wydane (v0.41.1)

Wydanie uzupełniające (tag `v0.41.1`, commit `3cd2f31`) obejmujące integrację urządzeń mobilnych MTP oraz asynchroniczne metryki rozmiaru katalogów:

- **Etap 1 — Wykrywanie urządzeń MTP i obsługa hotplug (✅ ukończone):**
  - Automatyczne wykrywanie urządzeń MTP (smartfony Android, aparaty) w widoku „Ten komputer” i panelu bocznym, przejrzyste przekierowanie surowych adresów UDI (`mtp:udi=...`) na czytelne ścieżki `mtp:/<Nazwa urządzenia>`, monitorowanie podłączania/odłączania na gorąco (`Solid` + `org.kde.kmtpd5`), baner rozłączenia z przyciskiem „Połącz ponownie” oraz pełna zgodność z widokiem Primary/Split.
- **Etap 2 — Rozmiar folderu i zajętość dysku w oknie Właściwości (✅ ukończone):**
  - Asynchroniczne, nieblokujące rekurencyjne obliczanie sumy logicznych rozmiarów plików, raportowanej alokacji bloków (`st_blocks * 512`) z podpowiedziami dokładności dla Btrfs/NTFS oraz dokładnej liczby plików, podfolderów i dowiązań symbolicznych z poprawną polską odmianą liczebników, ochroną przed pętlami symlinków (`NoFollow`), deduplikacją dowiązań twardych, izolacją granic montowania i natychmiastowym anulowaniem przy zamknięciu okna.

### 0.41.2 — Automatyczne odświeżanie katalogów i trwałe usuwanie na MTP ✅ ukończone, wydane (v0.41.2)
- Automatyczne monitorowanie katalogów przez `KDirWatch` (pliki lokalne) i `org.kde.KDirNotify` (D-Bus KDE Plasma) z cichym odświeżaniem w miejscu (Quiet Refresh) zachowującym zaznaczenie, fokus, pozycję przewinięcia, aktywną edycję nazwy (`EditingState`) i filtry wyszukiwania (buforowanie zdarzeń 150 ms / maks. 500 ms);
- Gwarantowane odświeżanie katalogów źródłowych i docelowych po operacjach wewnętrznych (Wytnij/Kopiuj/Wklej, Przeciągnij i upuść, Usuń, Zmień nazwę, Kosz, Przywróć) bez konieczności naciskania `F5`;
- Dedykowana akcja „Usuń trwale…” (`Shift+Delete`) oraz inteligentny fallback klawisza `Delete` dla urządzeń MTP (`mtp:/`) z ochroną katalogów głównych i partycji telefonu (wymagana znormalizowana głębokość ścieżki `>= 3`), wydane jako commit `061e0a0` (tag `v0.41.2`).

### 0.42.0 — Optymalizacja wydajności i pamięci ✅ ukończone, wydane (v0.42.0)
- Optymalizacja sortowania typów MIME (transformacja Schwartza `Decorate-Sort-Undecorate`, do 21,2x szybsze sortowanie dla 10 000 elementów);
- Leniwa inicjalizacja nieaktywnych modeli w `DirectoryView` (elementy alokowane wyłącznie w aktywnej reprezentacji widoku Primary/Split, oszczędność ~15–18 MB RSS na 10k elementów w widoku pojedynczym i ~39,4 MB RSS w sesji dwupanelowej);
- Zmniejszenie narzutu pamięciowego wierszy i zachowanie zaznaczenia, fokusu oraz pozycji przewinięcia przy przełączaniu trybów widoku;
- Zakończone procedury odbioru automatycznego i manualnego FULL PASS (69/69 zestawów, 114 308 asercji);
- Oficjalne wydanie opublikowane i zainstalowane w systemie (commit `23b1db6`, tag `v0.42.0`).

### 0.43.0 — Ponawianie operacji z uprawnieniami administratora (Administrator Fallback) ✅ ukończone, wydane (v0.43.0)
Gdy zwykła operacja plikowa zakończy się błędem braku uprawnień (Permission Denied), ThisPC oferuje ukierunkowane ponowienie operacji za pośrednictwem `admin://` i PolicyKit, bez konieczności uruchamiania całej aplikacji jako `root`.
- **Brak globalnego trybu administratora:** ThisPC nigdy nie uruchamia okna GUI jako `root`; podniesienie uprawnień jest ściśle ograniczone do pojedynczej, jawnie potwierdzonej operacji przez worker KIO `admin://` i PolicyKit (Polkit), z jednorazowym tokenem zgody `AdminUserConsent` (ważnym 60 s) oraz blokadą pętli ponowień (`AlreadyRetriedAsAdmin`).
- **Obsługiwane operacje GUI (centralna allowlista `isGuiAdminRetryAllowed`):**
  - *Copy File To Protected (`file://` → `admin://`):* Kopiowanie pojedynczego zwykłego pliku lokalnego do chronionego katalogu bez automatycznego nadpisywania.
  - *Copy File From Protected (`admin://` → `file://`):* Odczyt pojedynczego chronionego pliku przez `admin://` i zapis do nieuprzywilejowanego celu `file://`, dzięki czemu plik docelowy należy do zwykłego użytkownika (`uid == getuid()`) i otrzymuje rygorystyczne uprawnienia `0600` (`rw-------`).
  - *Create Protected File (`admin://`):* Tworzenie pojedynczego pliku przez `KIO::storedPut` z zachowaniem zawartości szablonu (`thispcFileContents`) oraz strukturalną weryfikacją błędu `KIO::ERR_CANNOT_TRUNCATE` (kod 177) wyłącznie przy potwierdzonym braku praw zapisu w katalogu docelowym i braku wcześniejszych plików/artefaktów `.part`.
  - *Create Protected Folder (`admin://`):* Tworzenie pojedynczego katalogu przez `KIO::mkdir`.
- **Operacje zablokowane i ograniczenia backendu KIO:**
  - *Administracyjne Rename i Move (`BLOCKED` w GUI):* Audyt bezpieczeństwa KDE `kio-admin` (`RenameCommand::execute()` wywołujący `QFile::rename()` → `rename(2)` bez flagi `RENAME_NOREPLACE`) wykazał brak atomowej gwarancji nie-nadpisywania celu na poziomie jądra po walidacji preflight. W konsekwencji administracyjne `Rename` oraz `Move` pozostają bezwzględnie zablokowane w GUI (`OperationDisallowedInGui`).
  - *Operacje niszczące, wieloplikowe i nielokalne (`BLOCKED`):* Trwałe usuwanie (`Delete`), operacje Kosza (`Trash`/`EmptyTrash`), operacje wieloplikowe (batch), przenoszenie katalogów, dowiązania symboliczne i uszkodzone symlinki, twarde dowiązania (hardlinks), archiwa oraz protokoły wirtualne/zdalne (`thispc:/`, `trash:/`, `mtp:/`, `smb:/`, `sftp:/`) są odrzucane w trybie fail-closed.
- **Strukturalna klasyfikacja błędów, lokalizacja i obsługa Polkit:**
  - Odróżnianie rzeczywistej odmowy uprawnień (`EACCES`/`EPERM`, `ERR_ACCESS_DENIED`, `ERR_WRITE_ACCESS_DENIED`, `ERR_CANNOT_MKDIR`, `ERR_CANNOT_OPEN_FOR_READING`) od systemów plików tylko do odczytu (`ST_RDONLY`/`EROFS`), braku miejsca (`ENOSPC`/`EDQUOT`), kolizji nazw (`EEXIST`) i brakujących ścieżek (`ENOENT`).
  - Spolszczone komunikaty diagnostyczne błędów dla nieatomowego przeniesienia (`Nie można przenieść pliku atomowo.`), tworzenia tymczasowego pliku docelowego (`Nie można utworzyć tymczasowego pliku docelowego.`) oraz błędu KIO 177 (`ERR_CANNOT_TRUNCATE`), przy całkowitej niezależności kwalifikacji błędów od tekstu tłumaczenia.
  - Czysta obsługa anulowania lub odmowy autoryzacji Polkit jako `AuthorizationCancelledOrDenied` / `UserCancelled` bez zawieszania GUI i bez ponownych pytań.
- **Zasada zachowania danych i artefaktów po błędach (`AdminArtifactOwnershipAudit`):**
  - Automatyczne usuwanie (`::unlink`) plików docelowych po nieudanych lub przerwanych transferach jest wyłączone, ponieważ zewnętrzny worker KIO nie przekazuje klientowi GUI kryptograficznie ani atomowo zweryfikowanego dowodu utworzenia i-węzła.
  - Każdy częściowy lub niezweryfikowany artefakt (w tym `.part`) zostaje zachowany na dysku, przechodzi 9-punktowy audyt własności i bezpieczeństwa, jest jawnie komunikowany użytkownikowi oraz chroniony przez globalny rejestr aktywnych ścieżek docelowych.
- **Integralność Undo/Redo oraz Primary/Split View:**
  - Operacje uprzywilejowane nie dodają pozornych wpisów do nieuprzywilejowanego `UndoController` ani `LocalMoveHistory`, zachowując jednocześnie nienaruszoną historię zwykłych operacji Undo/Redo.
  - Kontekst `FileOperationContext` pozostaje niemutowalny przy przełączaniu lub zamianie paneli Primary/Split, zmianie schowka czy zamknięciu Split View, a `OperationManager` poprawnie śledzi postęp i anulowanie.
- **Podsumowanie etapów wdrożenia:**
  - Etap 0 — Audyt architektury i bezpieczeństwa ✅ FULL PASS
  - Etap 1 — Kontrakt ponawiania administracyjnego ✅ FULL PASS (67 asercji)
  - Etap 2 — Bezpieczne kopiowanie i tworzenie ✅ FULL PASS (43 testy / 177 asercji + odbiór manualny)
  - Etap 3 — Bramka bezpieczeństwa Rename i Move ✅ CONDITIONAL PASS (43 testy / 71 asercji; zablokowane w GUI z powodu braku atomowego no-overwrite w KIO)
  - Etap 4 — Integracja GUI i bramka bezpieczeństwa artefaktów ✅ FULL PASS (72 testy / 228 asercji + 12/12 scenariuszy odbioru manualnego GUI)
  - Etap 5 — Końcowa integracja, regresja bezpieczeństwa i gotowość wydania ✅ FULL PASS (17/17 scenariuszy automatycznych / 161 asercji + 3/3 manualne testy kontrolne GUI; pełna regresja 74/74 zestawy PASS; commit wydania `33f7b3f`, tag `v0.43.0`)


### 0.44.0 — Kolejka i kontrola transferów (Transfer Queue & Control) ✅ ukończone, wydane (v0.44.0)
- **Domyślna aktywacja produkcyjnej kolejki transferów:** Standardowe operacje Kopiuj i Przenieś z panelu głównego (Primary), widoku podzielonego (Split View), przeciągania i upuszczania (Drag & Drop), schowka (Wytnij/Kopiuj/Wklej) oraz synchronizacji Split View są domyślnie kierowane przez `TransferQueueController` od startu aplikacji (`activateTransferQueueProductionDispatchByDefault()`, ze zmienną `THISPC_TRANSFER_QUEUE_PRODUCTION=0` zachowaną jako przełącznik diagnostyczny);
- **Tryby transferu Szeregowo vs Równolegle:** Dynamiczne przełączanie między trybem szeregowym (1 aktywny transfer) a równoległym z konfigurowalnym limitem jednoczesnych operacji (`1..8`, domyślnie `2`) i ochroną przed zagłodzeniem zadań;
- **Priorytetyzacja i zmiana kolejności zadań w kolejce:** Priorytety `Wysoki` (`High`), `Normalny` (`Normal`) i `Niski` (`Low`) oraz ręczna zmiana pozycji zadań oczekujących (`Przesuń wyżej`, `Przesuń niżej`, `Na początek kolejki`, `Na koniec kolejki`) z deterministycznym porządkiem FIFO;
- **Wstrzymywanie, wznawianie i anulowanie pojedynczych zadań oraz całej kolejki:** Rzetelne raportowanie możliwości wstrzymania dla każdego backendu oraz potwierdzenie przy zamykaniu okna z aktywnymi lub oczekującymi transferami;
- **Opcjonalne ograniczanie przepustowości (Bandwidth Throttling):** Wielowątkowy limiter oparty na wspólnym zasobniku tokenów (`TransferBandwidthLimiter`) w `KiB/s` lub `MiB/s` dla lokalnych transferów blokowych (`LocalFileCopyWorker`), z jawnym oznaczeniem `Brak limitu` (`No limit`) dla backendów nieobsługujących opóźniania bloków;
- **Zintegrowany Menedżer Operacji (Unified Operations Manager):** Jednolite okno operacji z podsumowaniem postępu, rozwijanym wykresem prędkości w czasie rzeczywistym, szczegółowymi kartami zadań (bajty, prędkość, ETA, indywidualny wykres prędkości), historią sesji dla zadań ukończonych/anulowanych/nieudanych (w tym tradycyjnych zadań KIO, takich jak Kosz, usuwanie czy archiwizacja), akcją **Wyczyść historię**, automatycznym otwieraniem i zamykaniem (`CloseWindowOnComplete`), trwałym zapisem ustawień (`QSettings`), przewijanym panelem ustawień (`QScrollArea` przy `480 × 290 px`) oraz zatrzymywaniem odtwarzania multimediów Quick Look przy zamknięciu okna;
- **Pliki tymczasowe `.thispc-part`, weryfikacja tożsamości i blokada bezpieczeństwa MTP Move (`BLOCK MOVE`):** Odroczone tworzenie zadań (`jobFactory`), rewalidacja preflight przed uruchomieniem, rejestr rezerwacji ścieżek (`TransferReservationRegistry`), zapis etapowy `.thispc-part`, weryfikacja `LocalFileIdentity`, bariera aktywnej kolejki w `UndoController`, obsługa kopiowania i hotplug MTP oraz ścisła blokada wydaniowa (`MtpMoveVerdict::RefusedUnsupported`) odmawiająca wszystkich kierunków przenoszenia (`Move`) obejmujących `mtp:/` (`MTP → lokalny`, `lokalny → MTP`, `MTP → MTP` oraz wewnątrz urządzenia MTP), dzięki czemu pliki źródłowe nigdy nie są usuwane bez odpornej na wyścigi weryfikacji po transferze.

**Zasady architektoniczne i reguły bezpieczeństwa:**
1. **Oddzielenie harmonogramu od prezentacji:** Oddzielić harmonogram zadań i koordynację stanu kolejki od ich prezentacji w `OperationManager`.
2. **Zachowanie istniejących bezpiecznych silników transferu:** Zachować istniejące, przetestowane silniki transferów (`LocalFileCopyJob`, `LocalFileMoveJob`, `KIO::CopyJob`) bez wprowadzania regresji.
3. **Zabezpieczenie ścieżek docelowych przed współbieżnym dostępem:** Wykluczyć jednoczesne operacje piszące, przenoszące lub modyfikujące identyczne lub nakładające się ścieżki docelowe.
4. **Wstępna rewalidacja (preflight) bezpośrednio przed uruchomieniem:** Rewalidować źródła, cele, wolne miejsce i uprawnienia bezpośrednio przed faktycznym przejściem zadania ze stanu oczekującego do uruchomionego.
5. **Brak pozorowanej pauzy:** Nie przedstawiać nieobsługiwanej pauzy jako skutecznej, gdy backend nie potrafi jej faktycznie zapewnić (np. workerzy KIO).
6. **Bezpieczne zamykanie aplikacji z aktywnymi operacjami:** Zapewnić bezpieczny cykl życia aplikacji przy trwających lub oczekujących zadaniach bez ryzyka uszkodzenia danych.
7. **Ochrona Undo/Redo i operacji konfliktowych:** Chronić spójność historii Undo/Redo i kolejność rozwiązywania konfliktów przed wyścigami współbieżnego wykonywania.
8. **Izolacja Administrator Retry od kolejki:** Nie umieszczać opóźnionych operacji Administrator Retry w ogólnej kolejce transferów.
9. **Brak trwałości / odzyskiwania kolejki po restarcie w 0.44:** Nie implementować zapisu kolejki na dysku ani odzyskiwania zadań po restarcie w wersji 0.44.
10. **Ścisła granica zakresu z 0.45:** Nie przenosić powiadomień systemowych (Notifications), trwałego dziennika historii ani akcji „Pokaż w folderze” z wersji 0.45 do 0.44.
11. **Ograniczanie przepustowości jako opcja warunkowa:** Ograniczanie przepustowości pozostawić opcjonalne, wyłącznie dla backendów realnie wspierających opóźnianie bloków (`LocalFileCopyWorker`).
12. **Zweryfikowane limity współbieżności:** Domyślny limit 2 oraz konfigurowalny zakres 1–8 zweryfikowane w testach automatycznych i manualnych.

**Podsumowanie etapów wdrożenia:**
- **Etap 0 — Audyt architektury i bezpieczeństwa:** ✅ FULL PASS — Szczegółowy audyt istniejących dispatcherów, możliwości backendów, maszyn stanów, bezpieczeństwa współbieżności, Undo/Redo i cyklu życia.
- **Etap 1 — Rdzeń kolejki i harmonogram (Scheduler):** ✅ FULL PASS — Niezależny silnik koordynacji kolejki, model stanów zadań, bezpieczny kontrakt zlecania bez przedwczesnego tworzenia zadań i rezerwacja slotów.
- **Etap 2 — Tryby Szeregowo / Równolegle i limity:** ✅ FULL PASS — Tryby szeregowy i równoległy, konfigurowalny limit współbieżności (`1..8`, domyślnie `2`), dynamiczna zmiana limitów i ochrona przed zagłodzeniem (starvation protection).
- **Etap 3 — Interfejs kolejki i ustawienia:** ✅ FULL PASS — Wizualizacja kolejki w oknie `OperationManager`, rozróżnienie zadań oczekujących i aktywnych, ustawienia i dwujęzyczny interfejs PL/EN.
- **Etap 4 — Priorytety i zmiana kolejności:** ✅ FULL PASS — Priorytety Wysoki / Normalny / Niski, ręczna zmiana kolejności oczekujących zadań i determinizm kolejkowania FIFO.
- **Etap 5 — Globalne Wstrzymaj / Wznów / Anuluj:** ✅ FULL PASS — Sterowanie całą kolejką, rzetelna prezentacja stanu pauzy dla każdego backendu, bezpieczne anulowanie kaskadowe i potwierdzenie przy zamykaniu okna.
- **Etap 6 — Opcjonalne ograniczanie przepustowości:** ✅ FULL PASS — Wspólny limiter token-bucket (`TransferBandwidthLimiter`), opóźnianie bloków w `LocalFileCopyWorker`, dynamiczna zmiana limitów oraz rzetelne oznaczenia na kartach zadań.
- **Etap 7 — Integracja produkcyjna, Zintegrowany Menedżer Operacji, bramka bezpieczeństwa MTP i końcowa regresja:** ✅ FULL PASS — Integracja end-to-end z widokami Primary/Split, Drag & Drop, Wklej, synchronizacją Split View, Zintegrowanym Menedżerem Operacji (`U1–U8.1`, `UX-A..D`), zapisem etapowym `.thispc-part`, kopiowaniem i hotplugiem MTP, blokadą wydaniową MTP Move (`BLOCK MOVE`), weryfikacją ASan/UBSan, pełną regresją (76/76 zestawów PASS, 2335 asercji `transfer_queue` PASS), odbiorem manualnym R3 (8/8 PASS), zweryfikowaną instalacją systemową (R6 6/6 PASS), commitami wydania `109106e` / `d515815`, tagiem `v0.44.0` oraz opublikowanym oficjalnym wydaniem na GitHubie.

---

## Planowane wydania

**Zaplanowane:** przyjęte przyszłe prace przypisane do docelowych wersji (`0.45.0–0.48.0`), jeszcze niezaimplementowane.

### 0.45.0 — Powiadomienia i rozbudowa historii operacji (Notifications + Operation History Extensions)
- **Etap 1 — Powiadomienia pulpitu KDE Plasma:** ✅ CLOSED / IMPLEMENTED / AUTOMATED PASS / MANUAL ACCEPTED (7/7)
  - Natywne powiadomienia pulpitu KDE Plasma przez `KF6::Notifications` (`TransferNotificationManager`) informujące o ukończonych lub zakończonych błędem operacjach Kopiuj/Przenieś działających w tle;
  - Precyzyjne wykrywanie obserwacji okna (`isUserObserving()`): powiadomienia sukcesu wyciszane przy aktywnym oknie ThisPC, wysyłane przy oknie nieaktywnym lub zminimalizowanym;
  - Powiadomienia o błędach wysyłane bezwarunkowo niezależnie od fokusu; operacje anulowane nie generują powiadomień;
  - Agregowanie powiadomień dla szybkich transferów w oknie 250 ms, chroniące pulpit przed zalewaniem osobnymi komunikatami;
  - Pełna konfiguracja użytkownika w opcjach kolejki transferów (`DesktopNotificationsEnabled`) z trwałym zapisem w `QSettings`;
  - Standardowe zdarzenia awaryjne `DefaultEvent` i definicja `data/org.kde.thispcview.notifyrc` powiązana z `desktop-entry` `org.kde.thispcview`;
  - 13/13 scenariuszy testów automatycznych PASS (74 asercje), pełna regresja 78/78 PASS;
  - Odbiór manualny użytkownika: 7/7 PASS (A: kopiowanie w tle, B: wyciszenie przy aktywnym oknie, C: zminimalizowane okno, D: błąd transferu, E: brak powiadomienia przy anulowaniu, F: przełączanie i trwałość ustawienia, G: agregacja 6 transferów).
- **Etap 2 — Trwały i rozszerzony dziennik historii operacji:** ✅ ZAMKNIĘTY / IMPLEMENTACJA I ODBIÓR MANUALNY ZAKOŃCZONE
  - **Etap 2.1 — Model trwałej historii i bezpieczny magazyn:** ✅ IMPLEMENTED / AUTOMATED PASS (20/20 zestawów testów, 1637 asercji)
    - Deskryptor wartościowy `TransferHistoryRecord` oraz wielowątkowy, nieblokujący `TransferHistoryManager`;
    - Serializacja JSON z wersjonowaniem schematu (`schemaVersion: 1`), standardowa lokalizacja XDG State (`~/.local/state/kio-thispc/thispc-view/operations_history.json`);
    - Bufor pierścieniowy FIFO (200 rekordów), atomowy zapis przez `QSaveFile`, blokada wieloprocesowa przez `QLockFile` (limit 500 ms), uprawnienia prywatne 0600;
    - Bezpieczeństwo i bramki: mechanizm epok/generacji (`historyEpoch`), blokada symlinków, zachowanie uszkodzonych plików (`.corrupted.<ts>.<N>`), synchronizacja deltowa CRDT.
  - **Etap 2.2 — Integracja z produkcyjną kolejką transferów:** ✅ IMPLEMENTED / AUTOMATED PASS (36/36 zestawów testów, 1804 asercje) / MANUAL ACCEPTED (6/6)
    - Bezpieczne podłączenie `TransferHistoryManager` do produkcyjnego przepływu `TransferQueueController`;
    - Pełne metadane temporalne: `enqueuedTimeUtc`, `startTimeUtc`, `endTimeUtc`, dokładność 64-bitowych bajtów (`totalBytes`, `processedBytes`), `durationMs`, średnia prędkość, kody błędów KIO i opisy diagnostyczne;
    - Ścisła idempotencja (flaga `historyRecorded`) oraz pełna izolacja błędów magazynu (awaria zapisu historii nie przerywa ani nie unieważnia transferu Copy/Move);
    - Zweryfikowana bramka Security Gate A: `clear()` w procesie A podbija epokę na dysku, uniemożliwiając procesowi B wskrzeszenie wyczyszczonej historii;
    - Bezpieczne zamykanie: synchroniczne opróżnienie zapisu (`flushSync()`) przy zamykaniu okna i niszczeniu kontrolera;
    - Odbiór manualny użytkownika 6/6 PASS: A (pojedynczy Copy Completed), B (wieloplikowy Copy Completed, itemCount=2), C (Copy Failed, errorCode=100 z powiadomieniem KDE), D (3 rekordy zachowane po restarcie), E (Move Completed z trwałym rekordem), F (Copy Cancelled, trwały rekord, źródło nienaruszone, brak częściowego celu i stagingu).
  - **Etap 2.3 — Integracja trwałej historii z OperationWindow i szlify UX:** ✅ IMPLEMENTED / AUTOMATED PASS / MANUAL ACCEPTED (6/6)
    - Integracja trwałych rekordów historii z istniejącym oknem `OperationWindow` (bez tworzenia osobnego dedykowanego okna bez odrębnej decyzji użytkownika);
    - Karty archiwalne renderowane z `TransferHistoryManager` z czytelną plakietką `operationHistoryBadge` („Historia” / „History”);
    - Deduplikacja zadań sesyjnych i archiwalnych w oknie (powiązanie `task.historyUuid`);
    - Pełna wizualizacja stanów terminalnych (Ukończono, Anulowano, Błąd z opisem błędu), dat/czasu zakończenia, czasów trwania i rozmiarów;
    - Bezpieczeństwo kontrolek UI: brak aktywnych przycisków sterowania (Wstrzymaj/Anuluj/Wznów) na kartach archiwalnych;
    - Bezpieczny przycisk ukrycia (`detailedOperationDismissButton`) usuwający kartę wyłącznie z bieżącego widoku bez modyfikacji bazy na dysku;
    - Zero blokujących operacji I/O i zapytań stat przy renderowaniu kart z odłączonych nośników USB/MTP;
    - Rozwiązanie **Zagadnienia UX A**: kompaktowy rozmiar początkowy okna (580×380 px) zgodny z zaakceptowanym zrzutem bez pustej przestrzeni, z zachowaniem ręcznego skalowania, przewijania wielu zadań, Wayland, HiDPI oraz trwałego przywracania geometrii przez `QSettings`;
    - Rozwiązanie **Zagadnienia UX B**: eliminacja komunikatu „Oczekiwanie na próbki prędkości…” po szybkich transferach (< 500 ms) i jednoznaczny status ukończenia („Transfer ukończony natychmiastowo” / „Transfer anulowany” / „Transfer zakończony niepowodzeniem”);
    - Zestaw testów automatycznych: 10 nowych zestawów (łącznie 46 zestawów historii, 1863 asercje PASS), pełna regresja 79/79 PASS;
    - Odbiór manualny użytkownika 6/6 PASS: A (5 kart archiwalnych po starcie), B (szczegóły, ukrywanie i przywrócenie po restarcie), C (deduplikacja przy kopiowaniu), D (kompaktowy rozmiar 580×380 px i zapamiętywanie geometrii), E (wykres prędkości przy transferze natychmiastowym i 20 MB), F (bezpieczne rozwijanie, odłączone nośniki, przewijanie i integralność bazy).
  - **Etap 2.4 — Integracja historii z OperationPopup i UX C:** ✅ IMPLEMENTED / AUTOMATED PASS / MANUAL ACCEPTED (6/6)
    - Pełna integracja trwałej historii (`TransferHistoryManager`) z wysuwanym panelem paska narzędzi `OperationPopup`;
    - Rozwiązanie **Zagadnienia UX C**: całkowite wyeliminowanie mylącego komunikatu „Brak historii” w `OperationPopup`, gdy w bazie i oknie szczegółowym istnieją operacje;
    - Zunifikowany model deduplikacji (`historyUuid`) gwarantujący precyzyjne zliczanie operacji aktywnych, oczekujących i archiwalnych bez duplikatów;
    - Neutralna terminologia: prezentacja etykiety „Historia: N” / „History: N” dla mieszanych statusów końcowych (Completed, Failed, Cancelled) bez błędnego nazywania wszystkiego „Zakończone”;
    - Precyzyjna obsługa pustego stanu: komunikat „Brak operacji plikowych i historii.” wyświetlany wyłącznie wtedy, gdy magazyn historii jest faktycznie pusty;
    - Reaktywne, nieblokujące odświeżanie podsumowania w odpowiedzi na sygnały `recordsChanged` i `historyManagerChanged` bez odczytów dyskowych przy otwieraniu panelu;
    - Pełna spójność między `OperationPopup` i `OperationWindow`, z płynnym otwieraniem okna szczegółowego przez `m_showDetailsButton`;
    - Zachowanie kompaktowych wymiarów panelu podręcznego i ograniczonej wysokości (~180–200 px w stanie spoczynku);
    - Zestaw testów automatycznych: 8 nowych zestawów (łącznie 54 zestawy historii, 1897 asercji PASS), pełna regresja 79/79 PASS;
    - Odbiór manualny użytkownika 6/6 PASS: A (7 wpisów archiwalnych poprawnie wyświetlonych po starcie), B (otwarcie OperationWindow z panelu z 7 zgodnymi kartami), C (aktualizacja na żywo podczas transferów, plakietka, liczniki, skopiowanie 2 plików), D (przywrócenie 9 rekordów po pełnym restarcie), E (czysty profil z pustą historią poprawnie pokazuje brak operacji), F (spójność obydwu widoków i integralność bazy z 9 rekordami).
  - **Etap 2.5 — Ustawienia historii, retencja, synchronizacja wieloinstancyjna i odbiór końcowy:** ✅ CLOSED / ACCEPTED / BACKED UP / COMMITTED / PUSHED (8/8 PASS)
    - Integracja ustawień historii w `TransferQueueSettingsWidget` (przełącznik „Zapisuj historię operacji”, limity retencji 50/100/200/500, akcja „Wyczyść historię…”);
    - Bezpieczne okno dialogowe z potwierdzeniem (`promptClearHistoryConfirmation`) informujące o usunięciu wyłącznie wpisów dziennika bez naruszania plików na dysku;
    - Ścisła ochrona przed zmartwychwstaniem (resurrection): usunięte zadania z pamięci i dysku nie mogą zostać ponownie zapisane (mechanizm epok `historyEpoch`);
    - Sztywny limit rozmiaru pliku (2 MiB) z priorytetem usuwania najstarszych wpisów przed limitem liczbowym;
    - Bezpieczna synchronizacja współbieżna wielu instancji za pośrednictwem `QLockFile` i mechanizmu epok (Security Gate A);
    - Synchronizacja międzyprocesowa w czasie rzeczywistym za pomocą `QFileSystemWatcher` z debouncingiem (150 ms) oraz asynchronicznym odczytem w tle (`QtConcurrent::run`), odporna na atomową podmianę inodów przez `QSaveFile`;
    - Zabezpieczenie spójności liczników i czyszczenia: `TransferQueueController` usuwa zakończone zadania sesyjne z minionej epoki po odebraniu `historyCleared()`, eliminując zadania-widma, a `OperationPopup` nigdy nie wyświetla mylącego komunikatu „Zapisana historia: 1”, gdy trwały magazyn ma zero rekordów;
    - Poprawka kolorystyki paska narzędzi (`applicationstyle.cpp`): usunięcie problemu białego tła przy rozwijaniu ukrytych przycisków w motywach ciemnych/izolowanych (`background: palette(window)`);
    - Audyt i poprawka wieloinstancyjnego odzyskiwania Batch Rename (`batchrenamerecovery.h`): izolacja blokad procesów potomnych w katalogach `instance-<PID>` przy zachowaniu ochrony osieroconych dzienników po awarii;
    - Pełna weryfikacja automatyczna: 13 nowych zestawów testowych historii (67 zestawów historii / 2008 asercji PASS), `transfer_queue` (2335 asercji PASS), `notifications` (13/13 PASS), `operations` (57 asercji PASS), pełna regresja 77 nadrzędnych zestawów PASS;
    - Pełny odbiór manualny użytkownika (8/8 PASS — 6 listopada 2026 r.):
      - Scenariusz A (ustawienia historii, wartości domyślne i trwałość): PASS;
      - Scenariusz B (konfigurowalne limity retencji 50/100/200/500): PASS;
      - Scenariusz C (wyłączenie i ponowne włączenie zapisywania historii): PASS;
      - Scenariusz D (czyszczenie historii, potwierdzenie, anulowanie i integralność danych): PASS;
      - Scenariusz E (spójność OperationPopup, OperationWindow i trwałego magazynu): PASS;
      - Scenariusz F (dwa niezależne procesy, synchronizacja dodawania i czyszczenia, historyEpoch, brak wskrzeszania rekordów): PASS;
      - Scenariusz G (kopiowanie, przenoszenie i powiadomienia KDE Plasma): PASS;
      - Scenariusz H (ponowny start, trwałość trzech rekordów, integralność danych, brak staging leftovers, płynność GUI, brak zawieszeń i crashy): PASS.
    - Wszystkie funkcjonalności Etapu 2 (Trwała historia transferów) zostały w pełni zrealizowane, zaakceptowane manualnie i formalnie zamknięte w repozytorium.
- **Etap 3 — Bezpieczne ponawianie operacji kwalifikujących się do powtórzenia:** ✅ CLOSED / ACCEPTED / BACKED UP / COMMITTED / PUSHED (A–H 8/8 + UX 6/6, commit `752b56d`)
  - Bezpieczne, świadome ponawianie nieudanych operacji Kopiuj i Przenieś bieżącej sesji;
  - Rygorystyczna wstępna walidacja bezpieczeństwa: istnienie źródeł, niezmienność tożsamości pliku źródłowego (device/inode), uprawnienia do zapisu w katalogu docelowym, wykrywanie konfliktów w miejscu docelowym (w tym plików roboczych `.thispc-part`);
  - Karty archiwalne z trwałej historii po restarcie aplikacji mają przycisk „Ponów” bezwzględnie zablokowany z informacyjnym tooltipem;
  - Pokrycie testami automatycznymi: 43 testy kontraktu Etapu 3 (71 asercji), pełna regresja 77/77 zestawów PASS, izolacja środowiskowa XDG Stage 3 9/9 PASS;
  - Odbiór manualny użytkownika: Scenariusze A–H 8/8 PASS; Dopięcie UX UX.1–UX.6 6/6 PASS;
  - **Dynamiczne monitorowanie kwalifikowalności ponowienia na żywo:**
    - Reaktywne monitorowanie oparte o zdarzenia jądra (`QFileSystemWatcher`) z debouncingiem 150 ms dla nieudanych zadań w otwartym oknie `OperationWindow`;
    - Automatyczne odświeżanie dostępności przycisku („Ponów” staje się aktywny/wyszarzony) oraz podpowiedzi (tooltip) natychmiast po zmianach na dysku (przywrócenie uprawnień, odtworzenie pliku, usunięcie konfliktu/part) bez konieczności ponownego otwierania okna;
    - Sztywne limity zasobów (maks. 50 katalogów, 100 plików), zero odpytywania (zero-polling), obserwator aktywny wyłącznie przy widocznym oknie;
    - Testy jednostkowe UX.1–UX.6 PASS (91 asercji zestawu `operations`).
- **Etap 4 — Akcja „Pokaż w folderze”:** ✅ CLOSED / ACCEPTED / BACKED UP / COMMITTED / PUSHED (8/8 PASS)
  - Wewnętrzna akcja „Pokaż w folderze” dla operacji Kopiuj i Przenieś zakończonych statusem `Completed` — dostępna zarówno dla zadań bieżącej sesji, jak i dla rekordów archiwalnych po restarcie aplikacji (bez uruchamiania zewnętrznego Dolphina ani procesów powłoki);
  - Operacje jednopierwiastkowe otwierają katalog docelowy w aktywnym panelu ThisPC (`Primary` lub `Split`) oraz automatycznie zaznaczają i przewijają widok do przesłanego pliku, katalogu lub dowiązania symbolicznego (z uwzględnieniem docelowych ścieżek po zmianie nazwy `targetUrls`);
  - Operacje wieloelementowe otwierają katalog docelowy z wyczyszczonym zaznaczeniem, bez losowego zaznaczania pojedynczego pliku;
  - Ścisła walidacja bezpieczeństwa w trybie fail-closed (`resolveShowInFolder`): odrzucanie operacji `Failed`, `Cancelled` i aktywnych; odmowa przy brakującym pliku lub katalogu docelowym; blokada katalogów docelowych będących dowiązaniami symbolicznymi (`lstat`), ścieżek względnych i segmentów `..`; odmowa dla odłączonych nośników, `mtp:/`, zdalnych protokołów KIO (`smb://`, `sftp://`, `fish://` itp.) oraz schematów wirtualnych (`trash:/`, `admin:/`, `http://`);
  - Ścisła niezależność paneli `Primary` i `Split View`: nawigacja i zaznaczenie wykonywane wyłącznie w aktywnym panelu bez naruszania lokalizacji ani zaznaczenia w drugim panelu;
  - Integracja na żywo z `QFileSystemWatcher` w otwartym oknie `OperationWindow`, aktualizująca stan przycisku i podpowiedź (tooltip) po usunięciu lub przywróceniu pliku lub katalogu docelowego;
  - Weryfikacja automatyczna: Scenariusze A–L PASS (203 asercje `operations`), pełna regresja 77/77 zestawów PASS, izolacja środowiskowa XDG Stage 4 9/9 PASS;
  - Odbiór manualny użytkownika (8/8 PASS — 8 listopada 2026 r.): A (Completed Copy pojedynczego pliku), B (Completed Move pojedynczego pliku), C (historia po restarcie aplikacji), D (kopiowanie wielu plików z czystym zaznaczeniem), E (zniknięcie i przywrócenie pliku docelowego na żywo), F (zniknięcie i przywrócenie katalogu docelowego na żywo), G (niezależność Primary/Split View), H (H.1 Failed bez przycisku „Pokaż w folderze” + H.2 kontrolowany Cancelled 32 MiB bez przycisków „Pokaż w folderze” i „Ponów”, potwierdzona integralność dysku, brak pozostałości `.thispc-part` oraz zapisany rekord `Cancelled` w historii).
- **Etap 5 — Audyt bezpieczeństwa, remediacja, szlif końcowy, pakietyzacja i wydanie 0.45.0:** (W toku)
  - **Etap 5.1 — Kompleksowy audyt bezpieczeństwa (Comprehensive Security Audit):** ✅ UKOŃCZONY (`docs/SECURITY_AUDIT_045.md`) — Niezależny manualny i automatyczny audyt bezpieczeństwa (`GCC 16.2.1 -Wall -Wextra -Wformat=2 -Wundef`, `clang-tidy 23.1.4`, `ASan` + `UBSan`) całej bazy kodowej ThisPC (Obszary A–G) wraz z 9-punktowym modelem zagrożeń:
    - Potwierdzono **0 luk `CRITICAL`**, **0 luk `HIGH`** oraz **całkowity brak złośliwego kodu / backdoorów**;
    - Zidentyfikowano **2 znaleziska `MEDIUM`** (`SEC-045-01`: ryzyko `heap-use-after-free` w `OperationWindow::rebuild()` przy nałożeniu się nieanulowanego timera 120 ms z `scheduleRebuild()` na synchroniczne `rebuild()`; `SEC-045-02`: publikacja w trybie bez nadpisywania w `LocalFileCopyWorker` i `LocalFileMoveJob` używająca `QFile::rename` zamiast `RENAME_NOREPLACE`) oraz **6 usprawnień utwardzających `LOW`** (`SEC-045-03`..`SEC-045-08`);
    - **Werdykt Security Release Gate (Stage 5.1):** **`BLOCKED` (ZABLOKOWANY)** do czasu wdrożenia i weryfikacji poprawek w Etapie 5.2.
  - **Etap 5.2 — Remediacja bezpieczeństwa i utwardzenie (Security Remediation & Hardening):** ✅ ZAMKNIĘTY / AUTOMATED PASS / MANUAL ACCEPTED (4/4) / BACKED UP / COMMITTED / PUSHED — Wszystkie 8 zgłoszonych podatności (`SEC-045-01`..`SEC-045-08`) naprawione i zweryfikowane:
    - Wyeliminowano `heap-use-after-free` w `OperationWindow` / `TransferQueueViewWidget` poprzez zatrzymywanie timera, liczniki generacji, odpinanie widżetów i `deleteLater()` (`SEC-045-01`);
    - Zaimplementowano atomową funkcję `publishLocalFileNoOverwrite()` z `SYS_renameat2(..., RENAME_NOREPLACE)` bez fallbacku do nadpisującego `rename(2)` (`SEC-045-02`);
    - Utwardzono wycofywanie (rollback): fallback link/unlink weryfikuje tożsamość `st_dev` i `st_ino` przed usunięciem celu; przenoszenie katalogów bez wsparcia `RENAME_NOREPLACE` kończy się bezpieczną odmową (`EINVAL`) zamiast nieatomowego nadpisania;
    - Poprawiono błąd use-after-move w `TransferQueueController::enqueue` (`SEC-045-03`);
    - Objęto runtime'owe sprawdzenia `THISPC_TEST_HARNESS` dyrektywą `#ifdef THISPC_TEST_HARNESS` (`SEC-045-04`);
    - Utwardzono wywołania procesów zewnętrznych za pomocą separatora `--` i ścieżek bezwzględnych (`SEC-045-05`);
    - Utwardzono zapis ACL przed symlinkami i dodano bramkę `BatchRenameRecoveryGate` (`SEC-045-06`);
    - Utwardzono `uninstall.sh` o ścisłą białą listę plików, zakaz `..` i wsparcie izolowanego roota testowego (`SEC-045-07`);
    - Dodano null-check w `m_details`, zabezpieczono anulowanie w `DuplicateFinderJob` i wprowadzono wielowątkowy `qt_error_string()` (`SEC-045-08`);
    - Weryfikacja automatyczna: build produkcyjny PASS, ścisły build GCC 16.2.1 PASS, `clang-tidy` PASS (z udokumentowanymi uwagami informacyjnymi `cert-err33-c`), 19/19 zestawów `ASan + UBSan` PASS (0 błędów), 77/77 pełna regresja PASS, test deinstalacji PASS;
    - Odbiór manualny użytkownika (4/4 FULL PASS — 8 listopada 2026 r.): 1. Stabilność `OperationWindow` przy wielokrotnym klikaniu szczegółów PASS; 2. Rozwiązywanie konfliktów nazw, nienaruszona suma SHA-256 i zero pozostałości `.thispc-part` PASS; 3. Funkcja „Pokaż w folderze” po utwardzeniu PASS; 4. Brak omijania potwierdzenia czyszczenia historii przez `THISPC_TEST_HARNESS=1` w buildzie produkcyjnym PASS;
    - Zweryfikowany backup: `/mnt/g/ThisPC backup/ThisPC-0.45.0-Stage5.2-ManualAccepted-2026-11-08/`;
    - **Stan bramki bezpieczeństwa (Security Release Gate):** **`BLOCKED / FINAL RELEASE VERIFICATION PENDING`** (ZABLOKOWANY do końcowej weryfikacji wydania w Etapie 5.3).
  - **Etap 5.3 — Szlif końcowy, pakietyzacja i wydanie 0.45.0:** (Zaplanowany po zamknięciu Etapu 5.2)

### 0.45.0 — Bezpieczeństwo i bramka wydaniowa (Security Release Gate)
- Przed oficjalnym wydaniem wymagany jest kompleksowy audyt bezpieczeństwa obejmujący: zabezpieczenie przed nieautoryzowanym wykonywaniem kodu, brak podatności typu backdoor, kontrolę uprawnień administracyjnych i brak ich nieuprawnionej eskalacji, weryfikację zależności zewnętrznych, integralność instalatora oraz bezpieczeństwo wszystkich operacji plikowych;
- Wystąpienie jakiejkolwiek nierozwiązanej luki krytycznej lub wysokiego ryzyka bezwzględnie blokuje wydanie wersji;
- Zasada obowiązuje bezwarunkowo dla wydania 0.45.0 oraz wszystkich przyszłych wydań.

#### Rejestr zagadnień UX (zakres 0.45.0):
- **Zagadnienie UX A — Domyślny rozmiar okna „Operacje plikowe”:**
  Docelowy kompaktowy rozmiar początkowy (ok. 580×380 px), z pełną widocznością jednej rozwiniętej karty operacji bez nadmiaru pustej przestrzeni, z zachowaniem ręcznego skalowania, przewijania wielu zadań, Wayland oraz skalowania HiDPI.
- **Zagadnienie UX B — Wykres prędkości po natychmiastowym transferze:**
  Zakończone operacje z mniej niż 2 próbkami prędkości (transfery poniżej 500 ms) nie mogą wyświetlać komunikatu „Oczekiwanie na próbki prędkości…”; ukrycie wykresu lub czytelny stan ukończenia.
- **Zagadnienie UX C — Spójność historii operacji:**
  Zapewnienie, że panel podręczny `OperationPopup` nie raportuje „Brak historii”, gdy w oknie szczegółowym `OperationWindow` znajdują się ukończone operacje bieżącej sesji.

### 0.46.0 — Niezależna architektura i audyt bezpieczeństwa (Standalone Architecture & Security Audit)
- Kompleksowy audyt zależności od KDE Frameworks i bibliotek Qt;
- Projekt koncepcyjny i architektoniczny niezależnego rdzenia ThisPC, odseparowanego od specyficznych wymagań konkretnego środowiska graficznego;
- Opcjonalne, luźno powiązane integracje ze środowiskiem KDE Plasma;
- Alternatywne backendy linuksowe dla środowisk wielopulpitowych;
- Analiza kompatybilności ze środowiskami Linux Mint / Cinnamon, Pop!_OS / COSMIC, XFCE itp.;
- Formalny model zagrożeń (threat model);
- Bezpieczny model pakietowania, dystrybucji i weryfikacji aktualizacji;
- Plan etapowej migracji architektonicznej bez utraty wypracowanych dotąd gwarancji bezpieczeństwa i integralności danych.
*(Uwaga: Faza analityczno-projektowa; prace implementacyjne nie zostały jeszcze rozpoczęte.)*

### 0.47.0 — Zaawansowana synchronizacja paneli (Advanced Split Sync)
- Opcjonalne rekurencyjne porównywanie i synchronizacja katalogów w Split View;
- Tryb symulacji (dry-run) i podgląd planu przed wykonaniem;
- Obsługa wzorców ignorowania (ignore patterns);
- Jawne zasady rozwiązywania konfliktów (conflict policies);
- Domyślnie nadal tryb zachowawczy (zero-delete);
- Tryb lustrzany/usuwanie dopiero jako jawny, wyraźnie niebezpieczny tryb z osobnym potwierdzeniem;
- Zachować model bezpieczeństwa, preflight i rewalidację wypracowaną w wersji 0.31.

### 0.48.0 — Drukowanie obrazów (Image Printing / Print Pictures Workflow)
- Zastąpić i rozszerzyć obecną pojedynczą ścieżkę `Print` własnym, przewidywalnym procesem drukowania obrazów;
- Obsłużyć wiele zaznaczonych plików graficznych w jednym zadaniu, z bezpiecznym dopuszczaniem tylko wspieranych formatów i czytelną obsługą zaznaczeń mieszanych;
- Zapewnić ergonomię zbliżoną funkcjonalnie do „Print Pictures” z systemu Windows 11, bez kopiowania wyglądu 1:1: duży podgląd bieżącej strony/obrazu, liczba stron oraz nawigacja po stronach zestawu;
- Dodać wybór układów strony (presetów), np. cała strona, 13×18, 10×15 oraz wiele zdjęć na stronie; opcjonalny arkusz kontaktowy (contact sheet) traktować jako naturalne rozszerzenie;
- Udostępnić wybór drukarki, rozmiaru papieru, liczby kopii i przełącznik dopasowania do ramki / przycięcia (fit-to-frame / crop), a jakość/DPI oraz typ papieru/nośnika pokazywać tylko wtedy, gdy backend lub sterownik faktycznie zgłasza takie możliwości;
- Korzystać z Qt Print Support oraz wykrywania możliwości drukarki przez CUPS/IPP tam, gdzie jest to dostępne;
- Utrzymać silnik podglądu i układu strony niezależny od systemowego modalnego okna drukowania, aby interfejs i wynik rozmieszczenia były przewidywalne;
- Zapewnić identyczne działanie w panelu głównym i Split View oraz testy układu, paginacji, zaznaczeń mieszanych i zadań wieloplikowych.

---

## Backlog bez przypisanej wersji

Wartościowe funkcje do rozważenia, bez przypisanej wersji ani ustalonego terminu realizacji.

### Dalszy rozwój wyszukiwania — bez przypisanej wersji
- Zapisane wyszukiwania (odłożony Etap 4 z pierwotnego planu 0.41): trwały zapis zwalidowanych definicji zapytania, filtrów, zakresu i sortowania z jawnym ponownym uruchamianiem oraz obsługą niedostępnych lokalizacji.
- Opcjonalna integracja Baloo (odłożony Etap 5 z pierwotnego planu 0.41): warunkowy adapter backendu oceniany pod kątem zgodności semantyki, pokrycia indeksu i kompletności wyników, z zachowaniem automatycznego powrotu (fallback) do niezależnego silnika workera.
- Szybki filtr bieżącego folderu w miejscu (in-place): wartościowy osobny mechanizm, bez wyszukiwania rekurencyjnego; wymaga własnej semantyki widoku i filtrowania.
- Wyszukiwanie po zawartości plików: osobna funkcja wymagająca przeglądu formatów/backendów, wydajności i bezpieczeństwa.
- Zaawansowana logika zapytań AND/OR/NOT: osobna od prostego łączenia aktywnych filtrów przez AND oraz alternatyw wewnątrz jednego filtra wydanych w 0.41.0.

### Rozszerzenia i akcje usług (Extensions / Service Actions) — bez przypisanej wersji
- Preferować wykorzystanie KDE Service Actions i prostego kontraktu rozszerzeń zamiast budowania pełnego własnego ekosystemu wtyczek bez konkretnej potrzeby;
- Pozwolić dodawać nowe akcje menu kontekstowego i integracje bez edycji kodu źródłowego głównego okna.

### Interoperacyjność przeciągania i upuszczania Ark → ThisPC — audyt i plan
- Przyjmowanie zewnętrznego przeciągania i upuszczania (Drag & Drop) z widoków archiwów Ark/KDE do zwykłego folderu w ThisPC dla pojedynczych i wielu zaznaczonych plików oraz folderów;
- Zbadanie w pierwszej kolejności rzeczywistego kontraktu MIME, adresów URI oraz tymczasowego wypakowywania w Ark/KIO — bez zakładania z góry lokalnych adresów `file://` ani obietnicy naiwnej implementacji opartej wyłącznie na prostym kopiowaniu;
- Domyślne stosowanie semantyki Kopiuj (Copy); udostępnianie Przenieś (Move) wyłącznie wtedy, gdy źródło i backend rzeczywiście wspierają je w sposób bezpieczny;
- Obsługa konfliktów oraz Cofnij/Ponów (Undo/Redo) tylko tam, gdzie ich gwarancje można realnie dotrzymać dla kontraktu dostarczonego przez źródło;
- Pełna parzystość działania w panelu głównym, Split View oraz we wszystkich trybach widoku;
- Testowanie analizy danych przez syntetyczne obiekty `QMimeData` oraz rzeczywistej interoperacyjności przez testy manualne z aplikacją Ark.

### Po 0.28.0 — odzyskiwanie (recovery) i domknięcie Batch Rename (wersja do ustalenia)
- Połączenie zweryfikowanego izolowanego silnika zamian (swap v2) z produkcyjnymi kontrolerami Execute/Undo/Redo, workerami, statusami i bramką startową;
- Aktywacja generatora i startowej allowlisty zamian po walidacji black-box Execute/Undo/Redo, drugim restarcie, dwóch instancjach i audycie GUI;
- Zaprojektowanie dedykowanego bezpiecznego protokołu odzyskiwania dla cykli 3+ elementów;
- Sformalizowanie i przetestowanie gwarancji trybu awaryjnego KIO oraz zdalnych adresów URL;
- Badanie odporności na utratę zasilania (power-loss) wyłącznie w jednorazowych maszynach wirtualnych i obrazach systemów plików, nigdy na danych użytkownika;
- Kontynuacja ochrony przed procesami niekooperującymi i wyścigami TOCTOU dla `RENAME_EXCHANGE`.

### Mniejsze funkcje — wersje do ustalenia
- `Ctrl+Shift+T` — ponowne otwarcie zamkniętej karty;
- Opcjonalne przypinane karty;
- Akcje „Kopiuj ścieżkę” / „Kopiuj nazwę pliku”;
- Ostatnie pliki (Recent Files) jako rozszerzenie Ostatnich lokalizacji (Recent Locations);
- Rozbudowany Kosz z kolumnami pierwotnej lokalizacji i daty usunięcia;
- Wyłącznie rozszerzenia miniatur wideo: bazowa obsługa KDE/KIO + `ffmpegthumbs`, asynchroniczne zadania, pamięć podręczna i fallback zostały wydane w 0.39; dalsze usprawnienia formatów/backendów lub zdalnego KIO pozostają kandydatami bez przypisanej wersji;
- Szczegóły na pasku stanu (liczba elementów, liczba zaznaczonych elementów, łączny rozmiar zaznaczenia, wolne miejsce na dysku).

---

## Pomysły do rozważenia w przyszłości

Luźne, niewiążące propozycje (**OPTIONAL / UNASSIGNED / NOT COMMITTED**), które mogą nigdy nie zostać zaimplementowane. Punkty w tej sekcji nie stanowią zobowiązania do wdrożenia ani przypisania do konkretnej wersji (nie są przypisane do wydań 0.44–0.48 ani do żadnego nowego wydania). Podczas kolejnych przeglądów planu rozwoju należy ponownie ocenić ich przydatność:

- Kandydat dotyczący wyglądu: regulacja poziomu przyciemnienia widocznych ukrytych elementów (obecny poziom domyślny: 0.40). W razie wdrożenia preferować proste predefiniowane poziomy zamiast suwaka w menu Widok.
- Opcjonalne efekty dźwiękowe akcji i zdarzeń: dyskretne sygnały dźwiękowe dla długotrwałych zadań, błędów lub zakończenia operacji (wyłącznie niezobowiązujący kandydat w przyszłości).
- Kandydat integracji systemowej: możliwość ustawienia ThisPC jako domyślnego menedżera plików dla typu `inode/directory` przy użyciu standardowych mechanizmów XDG/KDE, z wykrywaniem obecnego programu obsługi i bezpiecznym przywróceniem poprzedniego ustawienia.
- Wyłącznie rozszerzenia wizualizacji pamięci masowej: bazowa interaktywna mapa zajętości (Treemap) została wydana w 0.38; dalsze usprawnienia wizualizacji pozostają niewiążącymi kandydatami.
- Własny indeks wyszukiwania wyłącznie wtedy, gdy Baloo i niezależny silnik workera okażą się niewystarczające; brak zobowiązania do implementacji i brak przypisanej wersji.
- **Klasyczny pasek menu i organizacja poleceń (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Opcjonalny klasyczny pasek menu: `Plik | Edycja | Widok | Narzędzia | Pomoc` (`File | Edit | View | Tools | Help`).
  - Przykładowa organizacja:
    - *Plik:* tworzenie, otwieranie, właściwości, zamykanie.
    - *Edycja:* Cofnij/Ponów, Wytnij/Kopiuj/Wklej, zmiana nazwy, usuwanie.
    - *Widok:* tryby wyświetlania, sortowanie, grupowanie, Split View.
    - *Narzędzia:* wyszukiwanie, porównywanie, przyszłe narzędzia.
    - *Pomoc:* informacje o programie, dokumentacja, zgłaszanie błędów.
  - Zachować dostęp do często używanych funkcji na pasku narzędzi; nie zastępować dotychczasowego UI bez wcześniejszego projektu UX i zgody użytkownika.
- **Personalizowany pasek narzędzi (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Opcjonalny wybór widocznych przycisków i zmiana ich kolejności, bez usuwania funkcji z aplikacji.
  - Trwałe zapamiętywanie ustawień i możliwość przywrócenia domyślnego układu.
- **Zwijany panel boczny (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Przycisk zwijania/rozwijania lewego panelu, najlepiej na wysokości przycisków Wstecz/Dalej.
  - Zachować szerokość panelu i stan widoczności między uruchomieniami; opcjonalny skrót klawiaturowy; bez samoczynnego zwijania.
- **Skróty w menu kontekstowym (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Wyświetlanie rzeczywistych przypisanych skrótów klawiaturowych obok poleceń menu kontekstowego.
  - Etykiety muszą odpowiadać faktycznym skrótom, także po ich zmianie w ustawieniach.
  - Opcja pokazywania/ukrywania etykiet skrótów.
- **Centralne ustawienia ThisPC (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Jedno spójne okno ustawień dla funkcji wymagających konfiguracji.
  - Opracować organizację kategorii ustawień w osobnym przyszłym przeglądzie UX; nie dodawać obecnie sztucznych lub niedziałających opcji.
- **Personalizacja menu kontekstowego (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Możliwość ukrywania wybranych pozycji, np. „Otwórz w Dolphinie” ("Open in Dolphin").
  - Ustawienia osobno dla kontekstów: pliki, foldery, dyski, puste tło.
  - Ukrycie pozycji nie może wyłączyć samej funkcji ani jej skrótu klawiaturowego.
  - Zapamiętywanie ustawień i przywracanie domyślnych pozycji.
  - Rozróżnić tę funkcjonalność od planowanego w 0.46 wsparcia KDE Service Actions — dotyczą różnych aspektów menu (rozszerzenia zewnętrznych akcji usług a widoczność pozycji menu kontekstowego).
- **Konfigurowalne skróty klawiaturowe (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Możliwość zmiany i usunięcia skrótu dla danej funkcji.
  - Wymagana obsługa konfliktów, wyszukiwanie poleceń, zapis ustawień, przywracanie pojedynczych lub wszystkich skrótów domyślnych oraz synchronizacja prezentowanych etykiet w całym interfejsie.
- **Liczba ostatnich lokalizacji w panelu bocznym (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Konfigurowalny limit liczby wyświetlanych ostatnich lokalizacji, najlepiej przez numeryczne `QSpinBox`.
  - Minimum: `0`; maksimum: do ustalenia na etapie projektowania.
  - Limit dotyczy wyłącznie widocznych pozycji: nie usuwa historii lokalizacji i nie może powodować duplikatów.
  - Osobna propozycja od już istniejącego w backlogu kandydata Ostatnich plików (`Recent Files`).
- **Split View — dwuklik separatora 50/50 (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Podwójne kliknięcie lewym przyciskiem myszy separatora między panelami natychmiast przywraca proporcje 50/50 (przeniesione z historycznej wzmianki 0.34 do aktywnego backlogu pomysłów przyszłościowych).
  - Bez wymaganego `Ctrl`, `Alt` ani `Shift`; nie dodawać kombinacji modyfikatorów bez konkretnego dodatkowego zastosowania.
  - Zachować obecne przeciąganie separatora i niezależne działanie paneli.
- **Integracja z telefonem — KDE Connect (`OPTIONAL / UNASSIGNED / NOT COMMITTED`):**
  - Opcjonalna przyszła integracja telefonu:
    - Bezprzewodowe przeglądanie pamięci telefonu;
    - Kopiowanie plików telefon ↔ komputer;
    - Osobny interfejs SMS/MMS i powiadomień;
    - Ewentualna integracja Bluetooth.
  - Wymagany wcześniejszy audyt możliwości KDE Connect i dostępnych API; nie obiecywać funkcji nieobsługiwanych przez konkretny telefon lub backend.

---

## Przed wydaniem 1.0 (Before 1.0)

Cykl testów stabilizacyjnych i wzmacniania niezawodności obejmujący:
- Duże pliki i rozbudowane drzewa katalogów;
- Dziesiątki tysięcy wpisów w systemie plików;
- Systemy plików Btrfs, ext4 oraz NTFS;
- Lokalne i zdalne backendy KIO;
- Pełną parzystość widoku podzielonego (Split View);
- Przypadki brzegowe konfliktów Cofnij/Ponów (Undo/Redo);
- Podłączanie i odłączanie urządzeń w locie (hotplug);
- Odzyskiwanie po awarii procesu;
- Długotrwałe testy obciążeniowe sesji i szczelności pamięci (soak tests);
- Końcowy przegląd dostępności (accessibility) i nawigacji klawiaturą;
- Dopracowanie wyświetlania na ekranach HiDPI oraz w konfiguracjach wielomonitorowych;
- Kompleksowe testy regresyjne bezpieczeństwa operacji plikowych.
