# 0.25.0 Stage 2 — audyt i zasady ekstrakcji

Stan bazowy: main, b023c600f9a556d68ff89923bc78c88fa6e4394f.
Zachowano i rozwinięto zastane niezacommitowane zmiany Stage 2. Bez wydania,
zmiany numerów wersji, commita i taga. Stage 1A/1B/1C oraz Send To → ZIP pozostają
poza zakresem zmian zachowania.

## Ustalenia audytu Ark

Zweryfikowana wersja lokalna: **Ark 26.08.1**.

- --batch --destination katalog -- archiwum uruchamia osobny proces batch.
  Ark używa KDBusService::Multiple | NoExitOnFailure; nie przekazuje tej pracy
  istniejącemu oknu Ark.
- --dialog w trybie batch pokazuje wybór opcji przed uruchomieniem zadania.
  Anulowanie tego dialogu kończy proces kodem **0**.
- Wynik zadania batch jest podłączony do QCoreApplication::quit(), bez
  przeniesienia błędu KJob do exit code. **Kod 0 nie dowodzi udanej ekstrakcji**.
  Normalne zakończenie jest punktem rozpoczęcia weryfikacji wyniku.
- --autosubfolder według CLI rozpoznaje jeden plik/jeden katalog oraz wiele
  wpisów najwyższego poziomu. Nazwa katalogu i docelowe ścieżki zależą od danych.
  Nie nadaje się samodzielnie do ochrony istniejącego celu.
- Dialog Ark może zmienić docelowy katalog i opcje bez zwrócenia ich wywołującemu.
  Dlatego aplikacja wybiera cel własnym dialogiem katalogu i uruchamia Ark bez
  --dialog oraz bez --autosubfolder.

Źródła:
[main.cpp, tag v26.08.1](https://github.com/KDE/ark/blob/v26.08.1/app/main.cpp),
[BatchExtract](https://github.com/KDE/ark/blob/v26.08.1/app/batchextract.cpp),
[Landlock — dokumentacja jądra Linux](https://docs.kernel.org/userspace-api/landlock.html).

W zastanym kodzie brakowało weryfikacji zawartości, kontroli kolizji i własnej
ochrony ścieżek. Zwykły gzip był uznawany za archiwum tar. Proces był dzieckiem
okna, bez integracji z anulowaniem operacji. Blokada powtórzeń opierała się na
tekście ścieżki, a dialog wymuszał odświeżanie obu paneli niezależnie od celu.

## Zachowanie

- Jedno lokalne czytelne archiwum: ZIP, 7z, tar albo tar.gz/tgz. Detekcja menu
  używa zawartości; gzip dodatkowo sprawdza ograniczony nagłówek tar.
  To kwalifikacja do menu, nie pełne poświadczenie poprawności.
- Wypakuj tutaj: katalog pliku klikniętego w panelu lub wyniku Search.
- Wypakuj do: wybór istniejącego lokalnego katalogu w aplikacji. Anulowanie
  wyboru nie tworzy procesu ani katalogu roboczego.
- Jeden wpis najwyższego poziomu trafia do celu pod swoją nazwą. Przy wielu
  wpisach całość trafia do podfolderu nazwanego od archiwum. Dotyczy obu akcji.
- Istniejący plik, katalog, symlink lub dangling symlink oznacza błąd. Nie ma
  scalania, zastępowania ani automatycznego usuwania istniejących danych.
- Praca jest widoczna jako anulowalny KJob w dotychczasowym oknie operacji.
  Pokazuje fazy sprawdzania, ekstrakcji i weryfikacji. Nie zgłasza fikcyjnego
  procentu postępu Ark.
- Tożsamość urządzenie/inode blokuje ponowne uruchomienie tego samego archiwum
  także przez symlink lub hardlink, od dialogu aż do uprzątnięcia procesu.
- Zakończenie odświeża obecnie widoczne lokalne katalogi celu i jego podkatalogi,
  także przez alias symlink. Zmiana aktywnego panelu nie zmienia źródła ani celu.
  Search nie jest uruchamiany ponownie automatycznie.

## Granice ochrony i koszty

1. Poza wątkiem interfejsu powstaje prywatna kopia archiwum. Zmiana źródła podczas
   kopiowania jest wykrywana. Ark czyta tę kopię ze zweryfikowanym rozszerzeniem.
2. libarchive czyta wszystkie wpisy i dane; zapisujemy rozmiary oraz SHA-256.
   Odrzucane są szyfrowanie, symlinki/hardlinki, urządzenia/FIFO, ścieżki absolutne,
   .., ścieżki Windows, sterujące znaki, nieprawidłowe UTF-8, duplikaty oraz
   konflikt plik–katalog. Zwykły gzip jest odrzucany.
3. Ark zapisuje tylko do pustego prywatnego katalogu i własnego katalogu scratch.
   Landlock ogranicza zapis/truncate, tworzenie, usuwanie i rename poza nimi.
   Nie ufamy w tym zakresie samym backendom Ark. Nie tworzymy sesji pulpitu:
   środowisko XDG jest prywatne, adresy D-Bus nie prowadzą do usług użytkownika,
   a Ark działa offscreen. Nowsze ABI dodatkowo ogranicza Unix socket IPC.
4. Proces ma własną grupę. Anulowanie i destrukcja zatrzymują Ark wraz z jego
   pomocnikami, czekają na proces i dopiero wtedy sprzątają staging.
5. Wynik musi dokładnie odpowiadać manifestowi, typom, rozmiarom i hashom.
   Dodatkowe lub brakujące pliki, dowiązania i błąd procesu blokują publikację.
6. Jeden atomowy rename z RENAME_NOREPLACE publikuje wynik. Otwarty deskryptor
   katalogu wiąże ścieżki i sprzątanie z wybranym katalogiem nawet po jego
   przeniesieniu. Zastąpienie starej ścieżki symlinkiem nie przekierowuje zapisu.
   Nie ma fallbacku, który kopiowałby z nadpisywaniem.

Ochrona dotyczy ścieżek i zapisu przy ekstrakcji; nie jest deklaracją pełnej
izolacji dowolnego złośliwego programu ani gwarancją odporności bibliotek na
wszystkie błędy. Landlock nie kontroluje wszystkich operacji na metadanych.
Nie chronimy przed złośliwym procesem tego samego użytkownika manipulującym
pamięcią aplikacji ani przed awarią sprzętu/systemu. Nagłe zabicie całej aplikacji
lub utrata zasilania może pozostawić prywatny .thispc-extract-* do ręcznego
uprzątnięcia; aplikacja nie usuwa automatycznie nieznanych pozostałości.

Limit: 64 GiB kopii wejściowej i rozpakowanej zawartości, 100 000 wpisów, 128
poziomów, ścieżka do 4096 znaków. Puste, szyfrowane i inne formaty są odrzucane.
Detekcja gzip analizuje najwyżej 64 KiB wejścia i 512 bajtów wyniku; nietypowo
długi nagłówek może ukryć menu. Format bez poprawnego UTF-8 może być odrzucony.

Wymagane jest miejsce na kopię archiwum oraz wynik. Pełny odczyt i hashowanie
przed oraz po ekstrakcji zwiększają czas I/O. Tryb offscreen nie udostępnia
interaktywnych pytań Ark: nieoczekiwany dialog backendu może wymagać anulowania
operacji w aplikacji. Anulowanie I/O na niereagującym nośniku może być opóźnione.
Uprawnienia wyniku są prywatne dla właściciela; zachowywane są bity wykonywania
plików, usuwane set-id/sticky. Nie obiecujemy zachowania ACL/xattr/metadanych
archiwum. Ekstrakcja nie dodaje pozycji Undo.

## Weryfikacja

Testy są w archive-detection.cpp, archive-extraction.cpp i archive-menu.cpp.
Runner izoluje każdy zestaw we własnym XDG i używa prywatnego D-Bus.

Focused: TMPDIR=/tmp python3 tests/run-pane-actions.py --suites archive archive_jobs archive_menu

Regresja: TMPDIR=/tmp python3 tests/run-pane-actions.py --all

Testy ekstrakcji sprawdzają rzeczywisty Ark na jednorazowych ZIP/7z/tar.gz/tar,
złe rozszerzenia, uszkodzenia/CRC, szyfrowanie, kolizje i symlinki celu, niebezpieczne
wpisy, normalny exit 0 bez wyniku, błędy startu, crash, cancel/destrukcję, granicę
zapisu Landlock, późną kolizję i zamianę katalogu docelowego na symlink.
Menu jest wykonywane w obu panelach, wszystkich widokach, z Search, wieloma
zaznaczeniami, bez Ark, po zmianie fokusu i lokalizacji. Jeden test integracyjny
prowadzi od okna wyboru celu przez rzeczywisty Ark do odświeżenia właściwego panelu.

## Manual KDE — przed akceptacją i commitem

Wyłącznie jednorazowe archiwa i katalogi:

- [ ] ZIP, 7z, tar.gz: obie akcje w lewym i prawym panelu; ikony/lista/szczegóły.
- [ ] Search: źródło w innym katalogu niż pozostały panel; sprawdzić rzeczywisty cel.
- [ ] Jeden plik, jeden katalog i wiele wpisów; nazwy ze spacjami, #, polskimi literami.
- [ ] W trakcie pracy zmienić fokus i katalog, otworzyć cel w drugim panelu, zamknąć Split.
- [ ] Anulować wybór celu, następnie dłuższą ekstrakcję w oknie operacji; ponowić.
- [ ] Zamknąć aplikację podczas pracy: istniejące pytanie o aktywne operacje i poprawne zatrzymanie.
- [ ] Kolizja istniejącego pliku/katalogu oraz symlinka w jednorazowym celu: błąd bez zmiany danych.
- [ ] Zwykły gzip i tekst nazwany .zip: brak menu. Uszkodzone/szyfrowane archiwum: bezpieczny błąd.
- [ ] Stage 1A/1B/1C oraz Send To → ZIP: szybki smoke test.
- [ ] Akceptacja użytkownika. Dopiero potem osobna decyzja o commicie i wydaniu.
