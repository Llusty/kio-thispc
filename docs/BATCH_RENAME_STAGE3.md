# 0.28.0 Batch Rename — Stage 3: łańcuchy i cykle nazw

**Aktualny stan przed 0.28.0:** Stage 3A, 3B.1 i 3B.2 przeszły kompilację.
Stage 3B.2: pełna regresja `STAGE3B2_EXIT=0`, Batch Rename 146 PASS.
Stage 3C.1 po naprawie wykrywania nierozwiązanego dziennika przeszedł pełną regresję
(`FULL_REGRESSION_EXIT=0`, Batch Rename 163 PASS) i ręczny cykl 3 plików
bez resztkowego `cycle-*.json`. Stage 3C.2A: 175 asercji Batch Rename i pełna
regresja PASS. Nowsza ręczna sesja potwierdziła swap i cykl 3 z Undo/Redo, dwa
niezależne łańcuchy ze wspólnym Undo/Redo, odmowę Redo po podmianie inode z
komunikatem widocznym ok. 12 s, aktywny lewy panel Split View, dwa race oraz
cancel przed startem. Stan audytowany 2026-09-19. Ostatnia zapisana pełna regresja — 23 zestawy / 3865
asercji PASS — pochodzi sprzed tych nowych testów ręcznych; nie uruchomiono jej
ponownie podczas audytu dokumentacji.

Starsze podrozdziały zachowano jako **historyczny zapis** ograniczeń właściwych
dla danego etapu. Nie należy z nich wyprowadzać bieżącego statusu wydania.
Replay łańcuchów i cykli jest wieloetapowy i nieatomowy; journal umożliwia
ręczną kontrolę, ale nie automatyczne recovery po awarii procesu.

## Wdrożone w Stage 3A

- Planer rozpoznaje **acykliczne łańcuchy** w jednym katalogu, w obrębie dokładnego
  snapshotu zaznaczenia: `1 → 41`, `41 → 441` wykonuje najpierw `41 → 441`.
  Podgląd pozostaje uporządkowany według źródła; `executionOrder` jest oddzielną,
  deterministyczną kolejnością wykonania. Dla trzyelementowego łańcucha
  `1 → 11 → 111 → 1111` wykonanie zaczyna się od ostatniego elementu.
- Każdy wynik jest nadal sprawdzany pod kątem powtarzających się nazw (również
  case-folded) oraz rzeczywistych kolizji z niezaznaczonymi plikami. Kolizje
  różniące się pisownią wielkich/małych liter nie są uznawane za bezpieczne
  zależności. Dwukrotne wskazanie identycznego URL jest odrzucane.
- Po zatwierdzeniu planu sprawdzany jest **cały snapshot przed pierwszą operacją**;
  nadal następuje preflight przed każdym `KIO::moveAs` bez `Overwrite` i bez
  interaktywnego delegata. W Linux `lstat` porównuje urządzenie i inode źródła,
  obok wcześniejszych danych, co wykrywa m.in. podmianę pliku o tej samej
  ścieżce po wyświetleniu podglądu. Linki symboliczne są sprawdzane bez
  dereferencji. To ogranicza, lecz nie usuwa wyścigów między kontrolą a ruchem.
- Anulowanie zatrzymuje **następne** pozycje. Aktualnego zadania KIO nie zabijamy
  w połowie, bo błąd po `kill()` nie dowodzi, że rename nie zdążył już nastąpić.
  Już wykonane zmiany pozostają oddzielnymi wpisami Undo.

## Nadal zablokowane — istotne

**Cykle `A → B, B → A` oraz cykle trzech i więcej nazw są nadal blokowane.**
Wykonanie wymaga unikalnych nazw tymczasowych, journalingu każdej fazy, poprawnej
rekonstrukcji stanu po niejednoznacznym wyniku KIO, bezpiecznego rollbacku bez
nadpisywania, raportowania pozostałych plików tymczasowych i ochrony przed
awarią procesu. Wprowadzenie samego `A → temp` bez tego mechanizmu pozostawiłoby
użytkownikowi nieoczekiwany plik w razie błędu lub anulowania. Stage 3A nie
uruchamia takich przeniesień.

**Jedno, atomowe Undo/Redo batcha także nie jest gotowe.** Publiczne API
`KIO::FileUndoManager` pozwala rejestrować job (`recordCopyJob`, `recordJob`),
ale nie udostępnia transakcyjnego grupowania kilku niezależnych `moveAs`.
Istnieje `KIO::BatchRenameJob` i `batchRenameWithFunction` (KF6 od 6.16),
ale sam fakt posiadania pojedynczego joba nie gwarantuje bezpiecznych cykli,
rollbacku ani atomowości cofania. Trzeba zweryfikować implementację i zachowanie
na docelowym KF6, szczególnie dla błędów/cancel, zanim zastąpi się działającą
historię osobnych wpisów Undo. Nie używać `FileUndoManager::BatchRename` z
arbitralnym zadaniem bez sprawdzenia formatu danych nagrywanych do Undo.

Źródła API: https://api.kde.org/kio-fileundomanager.html oraz
https://api.kde.org/kio-batchrenamejob.html .

## Testy do wykonania w lokalnym repozytorium (fish)

```fish
cd ~/Pobrane/kio-thispc
./scripts/build.sh
env TMPDIR=/tmp python3 tests/run-pane-actions.py --suites batch_rename panes actions
env TMPDIR=/tmp python3 tests/run-pane-actions.py --all
git diff --check
```

Automatyczny test nowego planera obejmuje m.in. łańcuch 2/3 elementów,
blokadę swapu, kolizję zewnętrzną utworzoną po preview, identyfikację podmienionego
inode oraz testowy rename łańcucha w izolowanym `QTemporaryDir`. To **nie** jest
integracyjny test rzeczywistych jobów KIO ani rzeczywistego Ctrl+Z.

## Manual KDE — tylko kopie w świeżym katalogu testowym

- [ ] `1` i `41`, obydwa zaznaczone, prefiks `4`: preview dopuszcza `1 → 41`,
      `41 → 441`; po zatwierdzeniu oba pliki zachowują niezmienioną zawartość.
- [ ] `1`, `11`, `111`, prefiks `1`: trzy poprawne wyniki bez utraty zawartości.
- [ ] Niezaznaczony `41` blokuje `1 → 41`; niezaznaczony `441` blokuje cały
      wyjściowy plan z zaznaczonymi `1`,`41`.
- [ ] Utworzyć niezaznaczony cel po pokazaniu preview, przed potwierdzeniem:
      żaden plik nie zostaje nadpisany ani przemianowany.
- [ ] Swap `ab ↔ ba` przez regex `^(.)(.)$`, zamiana `\2\1`: wykonanie zablokowane;
      preview pokazuje konflikt cykliczny.
- [ ] Wykonać dłuższy łańcuch i anulować: brak nowych operacji po bieżącym
      zadaniu, ukończone wpisy można osobno cofać (Ctrl+Z); nie obiecywać
      jednym Undo całego batcha.
- [ ] Testować aktywny lewy/prawy panel Split View, ikony/listę/szczegóły oraz
      zwykły Rename, Quick Look i Alt+P.

**Dalszy etap:** pełna transakcja cykli i operacji Undo/Redo z jednoznaczną
obsługą częściowych błędów; Stage 4 zdalne URL-e i skalowanie dopiero później.
Wersja źródeł pozostaje 0.27.0 do przygotowania wydania.


## Stage 3B.1 — tylko dwuelementowy swap (patch do weryfikacji)

**Stan: implementacja przygotowana w źródłach; wymaga kompilacji i testów w
lokalnym środowisku użytkownika. Nie potwierdzono automatycznej ani ręcznej
akceptacji Stage 3B.** Podstawowy test Stage 3A `1 → 41 → 441` przeszedł
ręcznie w KDE z kontrolą zawartości obu plików; focused: 85 + 287 + 157 =
529 asercji PASS. Pełna regresja po Stage 3A nie została potwierdzona.

- Para nazw powiązana wzajemną zależnością (`ab → ba`, `ba → ab`) jest
  planowana jako jedno `atomicSwaps`, obok acyklicznego `executionOrder`.
  Dopuszczamy tylko dokładne ścieżki dwóch zaznaczonych elementów w jednym
  katalogu. Kolizje z niezaznaczonymi, duplikaty, niezgodności case-foldingu,
  zmiana inode po preview i niepoprawne nazwy nadal blokują wykonanie.
- Na Linux do swapu używamy **jednego** `renameat2(RENAME_EXCHANGE)` zamiast
  `A → temp → B`: oba wpisy katalogu są wymieniane atomowo przez kernel, bez
  tymczasowego pliku i bez możliwości przerwania w połowie pojedynczego
  wywołania. Jeśli kernel lub filesystem nie wspiera wymiany, operacja jest
  odrzucana bez sekwencyjnego fallbacku. Na innych platformach dwucykle
  pozostają zablokowane.
- Przed zamianą ponownie sprawdzamy oba snapshoty (w tym dev/inode przez
  `lstat`). Po zamianie weryfikujemy, czy obydwa oryginalne inode zajmują
  oczekiwane przeciwne ścieżki. Równoczesne modyfikacje przez **inny proces**
  między `lstat` i syscall nadal stanowią wyścig TOCTOU. Przy wykrytej
  niezgodności zatrzymujemy dalsze operacje i zgłaszamy konieczność kontroli
  obu nazw; nie dotykamy nieznanego inode przy ryzykownym rollbacku.
- **Brak Undo dla swapu.** Nie rejestrujemy fikcyjnych operacji KIO;
  przed wykonaniem program pokazuje ostrzeżenie z domyślnym „Nie” i wyraźnie
  informuje, że poprzednia historia Undo zostanie unieważniona. Po potwierdzeniu
  stare wpisy są odcinane przez numer seryjny UndoController, a odświeżenie
  widoków jest wymuszane po zakończeniu partii. Zwykłe, acykliczne batch-e
  bez swapu zachowują osobne wpisy Undo. Jeśli partia łączy acykliczne zmiany
  i swapy, potwierdzenie oznacza brak Undo również dla wcześniejszych zmian
  tej konkretnej partii po wykonaniu swapu.
- Anulowanie między swapami nie uruchamia następnych; rozpoczęty swap jest
  pojedynczym syscall i kończy się atomowo. Przy wielu odrębnych parach
  wykonane pary pozostają zmienione (nie obiecujemy atomowości całego batcha).
- **Nadal niewdrożone:** cykle 3+ (`abc → bca → cab → abc`), wspólne Undo/Redo,
  globalna atomowość wielu par, trwały journal/recovery dla wieloetapowych
  cykli i zdalne URL-e. Wersja pozostaje 0.27.0, bez instalacji ani releasu.

### Testy Stage 3B.1 — wyłącznie jednorazowy katalog na lokalnym FS

```fish
cd ~/Pobrane/kio-thispc
./scripts/build.sh
env TMPDIR=/tmp python3 tests/run-pane-actions.py --suites batch_rename panes actions
env TMPDIR=/tmp python3 tests/run-pane-actions.py --all
git diff --check
```

Ręcznie w **uruchomionej wersji roboczej**:

- [ ] Dwa pliki `ab` oraz `ba` z różną zawartością; zaznacz oba, regex
      `^(.)(.)$`, zamiana `\2\1`; preview ma dwie nazwy docelowe,
      ostrzeżenie Undo, aktywny przycisk. Potwierdź ostrzeżenie i odczytaj
      zawartość obu plików: zamienione nazwy, żadnej utraty treści, brak temp.
- [ ] W tym samym katalogu powtórz z katalogami (w każdym osobny plik) oraz
      osobno symlinkami (potwierdź, że cele linków się nie zmieniły).
- [ ] `abc`, `bca`, `cab`; regex `^(.)(..)$`, zamiana `\2\1`: cykl
      trzech musi nadal blokować wykonanie i nie zmieniać danych.
- [ ] Odmów w ostrzeżeniu (domyślne „Nie”): nie zmienia się nic.
- [ ] Po pokazaniu preview podmień jeden wybrany plik na inną kopię:
      preflight musi zatrzymać cały swap, nie dotykając podmienionego pliku.
- [ ] Sprawdź, że Ctrl+Z **nie** cofa zamiany, a poprzednia historia Undo
      została unieważniona; po swapie nowa zwykła operacja powinna mieć
      standardowe Undo. Testuj wyłącznie na kopiach.
- [ ] Oba panele Split View, wszystkie 3 widoki i starszy łańcuch
      `1 → 41 → 441` bez regresji.


## Stage 3B.2 — cykle 3+ bez plików tymczasowych (patch do weryfikacji)

**Nie instalować i nie używać na danych użytkownika, zanim lokalne testy
kompilacji, automatyczna regresja i ręczna akceptacja na kopiach nie przejdą.**
Wersja `0.27.0` pozostaje bez zmian.

- Planer rozpoznaje izolowane cykle długości 3–1000 po ścisłych, dokładnych
  ścieżkach źródło→cel. Pozostałe zabezpieczenia Stage 1–3B.1 (duplikaty,
  unselected target, symlinki, snapshot `lstat`, kontrola całej partii) pozostają.
  Cykle uruchamiane są po acyklicznych zmianach i po niezależnych swapach;
  **cała mieszana partia nie jest transakcją**.
- Cykl `A→B, B→C, C→A` to `exchange(A,B)` i `exchange(A,C)`; ogólnie
  potrzebuje `N-1` wywołań `renameat2(RENAME_EXCHANGE)`. Nie tworzymy
  tymczasowych nazw i żadna pojedyncza wymiana nie usuwa istniejącego wpisu.
  Na innych systemach i gdy dany filesystem nie obsługuje `RENAME_EXCHANGE`
  odrzucamy wymianę; nie ma sekwencyjnego fallbacku przez overwrite.
- Przed każdym wywołaniem weryfikujemy wszystkie nazwy cyklu względem
  oczekiwanych par `dev/inode` (`lstat`, bez dereferencji symlinków). Po każdym
  wywołaniu weryfikujemy całą nową permutację. Jeśli niepewna jest tożsamość
  pliku, zatrzymujemy się i **nie cofamy na ślepo** operacji dotyczącej obcego
  inode. Wyścig między kontrolą a samym syscall nadal jest możliwy.
- Anulowanie lub zwykły błąd między etapami uruchamia rollback: zweryfikowane
  wymiany wykonywane są w odwrotnej kolejności, również jako `RENAME_EXCHANGE`.
  Gdy rollback się nie powiedzie lub wynik jest niepewny, aplikacja pokazuje
  ostrzeżenie o konieczności ręcznej kontroli całego cyklu i kończy partię.
  Poprzednie **ukończone inne cykle/swapy/zmiany nie są wycofywane**.
- Dialog preview i ostrzeżenie z domyślną odpowiedzią „Nie” tłumaczą, że
  wieloetapowy cykl **nie jest globalnie atomowy**. Awaria procesu/systemu
  pomiędzy syscallami może zostawić poprawne pliki pod częściowo zmienionymi
  nazwami. **Nie ma trwałego journalu, automatycznego recovery po restarcie ani
  Ctrl+Z/Redo dla cykli.** Przed pierwszą wymianą historia Undo jest
  unieważniana tak samo jak dla Stage 3B.1. Używać wyłącznie na kopiach do
  momentu zaprojektowania trwałego recovery i integracji Undo.

### Weryfikacja na KDE/CachyOS — testy tylko na kopiach

```fish
cd ~/Pobrane/kio-thispc
git apply --check "$HOME/Pobrane/kio-thispc-0.28-stage3b2.patch"
and git apply "$HOME/Pobrane/kio-thispc-0.28-stage3b2.patch"
and ./scripts/build.sh
and env TMPDIR=/tmp python3 tests/run-pane-actions.py --suites batch_rename panes actions
and env TMPDIR=/tmp python3 tests/run-pane-actions.py --all
and git diff --check
```

- [ ] Trzy pliki `abc`, `bca`, `cab` z inną zawartością; zaznaczyć wszystkie,
      regex `^(.)(..)$`, zamiana `\2\1`: preview pokazuje dokładnie
      `abc→bca`, `bca→cab`, `cab→abc`, ostrzeżenie, przycisk aktywny.
      Potwierdzić na **kopiach**, sprawdzić zawartość każdej nowej ścieżki
      i brak nazw tymczasowych.
- [ ] Cztery pliki `abcd`, `bcda`, `cdab`, `dabc`; regex `^(.)(...)$`,
      zamiana `\2\1`: oczekiwane cztery cele i zachowanie wszystkich danych.
- [ ] Odmówić w ostrzeżeniu: nie zmienia się nic, również Undo.
- [ ] Wstrzymać łańcuch po preview, podmieniając jeden plik w kopii katalogu:
      preflight odrzuca bez wymian. Nie przeprowadzać takich prób na danych
      osobistych.
- [ ] Przetestować osobno cykl katalogów i symlinków (cele linków niezmienne),
      kolizje spoza zaznaczenia, Split View lewy/prawy, wszystkie widoki.
- [ ] Sprawdzić, że po wymianie brak Ctrl+Z i że nowa zwykła operacja może
      ponownie zostać zarejestrowana w Undo. Nie testować wymuszonego crasha
      ani przerwania zasilania na danych użytkownika.

**Następny etap:** trwały journal i bezpieczne recovery po awarii,
jedno wspólne Undo/Redo dla partii, a dopiero potem wydanie 0.28.0.

## Stage 3C.1 — dziennik diagnostyczny i blokada po przerwanym cyklu

**Status: patch do testów, nie gotowe recovery ani Undo/Redo.** Użytkownik potwierdził
jednorazowy test cyklu `abc → bca → cab → abc` z zachowaniem trzech zawartości,
a Stage 3B.2 zakończył się `STAGE3B2_EXIT=0` (Batch Rename 146 PASS). Pełna
akceptacja pozostałych przypadków Stage 2/3 pozostaje otwarta.

- Przed pierwszą wymianą cyklu 3+ tworzony jest prywatny dziennik JSON w
  `~/.local/share/thispc-view/batch-rename-recovery/` (lub odpowiednim
  `XDG_DATA_HOME`). Zawiera pełne nazwy źródłowe i docelowe, pierwotne
  `lstat` device/inode jako **ciągi znaków**, bieżącą fazę oraz oczekiwane
  pozycje oryginalnych elementów. Nie zapisuje zawartości plików.
- Przed **każdym** syscall `RENAME_EXCHANGE` zapisywany i synchronizowany jest
  zamiar, a po potwierdzeniu tożsamości wszystkich inode zapisywany jest stan
  zakończonego etapu. Analogicznie journal obejmuje poszczególne wymiany
  rollbacku. `QSaveFile` + synchronizacja pliku i katalogu na Linux; błąd
  przed syscall zatrzymuje cykl, błąd zapisu po syscall zatrzymuje dalsze
  operacje i pozostawia journal do ręcznej kontroli.
- Po zweryfikowanym zakończeniu całego cyklu albo pełnym rollbacku dziennik
  jest zamykany i usuwany. Plik pozostaje po awarii procesu, niepewnym stanie
  lub błędzie zapisu/usuwania. Każda kolejna próba Batch Rename wykrywa
  nierozwiązany dziennik i **odmawia wykonania**. Blokada instancji
  uniemożliwia równoległe rozpoczęcie dwóch cykli wykorzystujących dziennik.
- **To nie jest automatyczne odzyskiwanie.** Nie ma automatycznego przesuwania
  plików po restarcie, nie ma journalu całego batcha obejmującego zwykłe
  `KIO::moveAs` ani ochrony przed działaniem innych aplikacji na plikach.
  Między kontrolą `lstat` a syscall nadal istnieje TOCTOU. Pełna transakcja
  wielu cykli nie jest atomowa, a Ctrl+Z/Redo dla cykli i całej partii
  pozostają **niewdrożone**. Nie testować crasha ani zasilania na danych użytkownika.

### Jeśli aplikacja zgłosi nierozwiązany journal

1. Zatrzymaj inne operacje w katalogu i **nie usuwaj dziennika od razu**.
2. Otwórz wskazany plik JSON i porównaj `originals[].source`,
   `originals[].destination`, `device` i `inode` z rzeczywistymi nazwami
   i wynikami `stat -c '%d:%i %n' -- /pełna/ścieżka` (bez `-L` dla symlinków).
   Faza `exchange-intent` może oznaczać zarówno stan *przed*, jak i *po*
   pojedynczym syscall: rozstrzyga **mapowanie inode**, nie sam licznik.
3. Gdy cokolwiek jest niejasne, zachowaj dziennik i kopie plików; nie
   podejmuj masowego `mv`, `rm` ani automatycznego rollbacku. Dopiero po
   świadomej weryfikacji danych i odtworzeniu oczekiwanego stanu przenieś
   dziennik do bezpiecznego archiwum poza katalogiem recovery (nie usuwaj
   bez kopii). Nowy Batch Rename zostanie wtedy odblokowany.

### Testy Stage 3C.1 (tylko kopie)

- [ ] `./scripts/build.sh` i focused `batch_rename panes actions` oraz `--all`.
- [ ] Test JSON: dokładne stringi inode/device, zapis zamiaru i prawidłowe
      wyczyszczenie po zakończeniu/rollbacku.
- [ ] Symulacja przerwania **samego obiektu dziennika w teście jednostkowym**:
      plik przetrwa, a kolejne rozpoczęcie zostanie odrzucone. Nie zabijaj
      prawdziwej aplikacji podczas wymiany na danych osobistych.
- [ ] W KDE powtórz cykl trzech jednorazowych plików: zawartość poprawna,
      brak nierozwiązanego journalu po powodzeniu, brak regresji Split View.
- [ ] Po symulacji błędu journalu testowego sprawdź blokadę i czytelny komunikat;
      **nie twórz** takiej blokady w realnym `XDG_DATA_HOME` z ważnymi danymi.

**Następny etap:** zaprojektowanie bezpiecznego wznowienia/wycofania na podstawie
rzeczywistego mapowania inode i zintegrowanej historii partii. Nie deklarować
jednego Undo/Redo przed testami częściowego wykonania, kolizji i innych operacji
przeplatanych z historią KIO.


## Stage 3C.2A — pierwszy bezpieczny pionowy wycinek Undo/Redo (patch do weryfikacji)

**Nie jest to jeszcze wspólne Undo/Redo całej partii.** Włączone wyłącznie,
gdy plan zawiera **dokładnie dwa pliki/katalogi/linki i jedną dwustronną wymianę**;
bez łańcucha, kolejnych par ani cyklu 3+. Te inne przypadki zachowują dotychczasowe
ostrożne ograniczenia (łańcuch: osobne wpisy KIO; mieszany batch i cykle: brak
zbiorczego Undo). Wersja aplikacji pozostaje 0.27.0.

- Po pełnym sukcesie dwuelementowego `renameat2(RENAME_EXCHANGE)` kontroler
  rejestruje własny numer seryjny polecenia obok historii KIO, zamiast fałszować
  komendę KIO. Starsza historia jest nadal odcinana przed wymianą, gdyż
  zapamiętane ścieżki mogłyby teraz wskazywać inne inode.
- Pojedyncze Ctrl+Z / Ctrl+Y wykonują po **jednym** atomowym syscall,
  po sprawdzeniu urządzenia/inode, typu i snapshotu mtime/rozmiaru obu wpisów.
  Nie ma fallbacku nadpisującego cel; brak wsparcia kernela lub podmieniony
  inode powoduje odmowę. Istniejący nierozwiązany dziennik blokuje powtórkę.
- Stan po syscall jest ponownie sprawdzany. Niepewna tożsamość odcina historię,
  nie wykonuje ślepego rollbacku. Wyścig między preflight i syscall nadal istnieje.
- Cofanie/ponawianie jest **tylko w bieżącym procesie**; nie przetrwa restartu.
  Nie daje automatycznego recovery po crashu i nie czyni wielu wymian atomowymi.
  Po nowej operacji Redo starej zamiany jest odrzucane.

### Ręczny KDE — wyłącznie na kopiach

- [ ] `uv` i `vu`, różne treści; regex `^(.)(.)$` → `\2\1`:
      po potwierdzeniu ostrzeżenie informuje o Undo dla tej jednej pary,
      Ctrl+Z przywraca obie zawartości, Ctrl+Y ponawia obie.
- [ ] Po Ctrl+Z podmień jeden inode przez bezpieczne przeniesienie do innej
      testowej nazwy i utworzenie nowego pliku; Ctrl+Y ma odmówić i niczego
      nie zamienić. Nie przeprowadzaj tego na plikach użytkownika.
- [ ] `abc`/`bca`/`cab`: ostrzeżenie wciąż wyraźnie mówi o braku Ctrl+Z;
      nie testować prawdziwych plików, tylko jednorazowe kopie.
- [ ] Sprawdź, że zwykły Batch Rename łańcucha, Split View i Quick Look nie
      zmieniły zachowania. Nie deklaruj pełnego Stage 3C jako ukończonego.

Automatyczne testy źródłowe dodają pojedynczy Undo/Redo z kontrolą zawartości,
odmowę Redo po podmianie inode oraz blokadę nagrania łańcucha jako swapu.
Wymagają kompilacji i pełnej regresji w lokalnym KDE/CachyOS.

## Stage 3C.2B.1 — Undo/Redo jednej izolowanej sekwencji cyklicznej (do testów)

**To jest kolejny wycinek Stage 3C.2B, nie wspólne Undo dowolnej partii.**
Po potwierdzeniu kompletnego cyklu **dokładnie jednej** grupy 3+ nazw,
bez łańcucha, dodatkowej pary ani innego cyklu, kontroler wystawia jeden
widoczny krok Ctrl+Z/Ctrl+Y. Każdy taki krok wykonuje N−1 wymian
`renameat2(RENAME_EXCHANGE)` w zadaniu QtConcurrent, poza wątkiem GUI.
Indywidualne wymiany są atomowe, **całe Undo/Redo nie jest atomowe**.

- Po zakończeniu oryginalnej operacji porównywana jest cała końcowa permutacja,
  urządzenia/inode oraz typ, rozmiar i mtime wszystkich elementów. Dopiero
  wtedy rejestrowany jest jeden wpis lokalnej historii z numerem seryjnym KIO;
  starsza historia zostaje odcięta zgodnie z polityką wymian Stage 3C.2A.
- Przed Undo/Redo następuje pełny preflight snapshotu. Dla każdego etapu
  `QSaveFile` i `fsync` zapisują zamiar przed syscall i zweryfikowany stan po.
  Poprawnie zakończony cykl usuwa dziennik. Błąd syscall przy znanym stanie
  powoduje próbę odwrócenia *tylko wykonanych* etapów, także z checkpointami.
  Przy nieznanej tożsamości lub błędzie trwałego zapisu operacja zatrzymuje się,
  pozostawia dziennik i unieważnia dalszą niebezpieczną historię.
- Manifest blokuje kolejne Batch Rename i replay Undo/Redo. Zawiera informację
  do **ręcznej kontroli**, lecz nie stanowi automatycznego odzyskiwania po
  awarii. Utrata zasilania w czasie N−1 wymian może zostawić częściową zmianę.
- Historia istnieje **tylko w bieżącym procesie**. Zmiana inode, mtime, rozmiaru,
  typu albo linku powoduje odmowę. Nie ma automatycznego cofania wielu
  niezależnych cykli ani odtwarzania z dziennika po restarcie.

**Historyczne ograniczenia tego wycinka:** jeden Ctrl+Z dla łańcuchów KIO, dwóch lub więcej
niezależnych swapów/cykli, i partii mieszanych; transakcja atomowa całej
partii; automatyczne recovery po crashu; zdalne URL-e. Takie partie nadal
zachowują poprzednią semantykę (Undo per element lub jego brak).

### Ręczna checklista KDE — tylko świeże, jednorazowe pliki

- [ ] `abc`, `bca`, `cab` z trzema odmiennymi zawartościami. Regex
  `^(.)(..)$` → `\2\1`: podgląd i ostrzeżenie mówią o **nieatomowym** Undo,
  wynik po zmianie: `abc=CAB`, `bca=ABC`, `cab=BCA`.
- [ ] Jedno Ctrl+Z przywraca wszystkie trzy pierwotne zawartości; jedno
  Ctrl+Y ponawia pełną permutację. GUI nie powinno blokować się na czas fsync.
- [ ] Po sukcesie żadnego `cycle-*.json` w katalogu dziennika.
- [ ] Po Undo zastąp **jeden** testowy inode innym plikiem i sprawdź, że
  Ctrl+Y nie zamienia niczego (nie dotykaj niepowiązanych plików).
- [ ] Łańcuch i partia mieszana nie reklamują wspólnego Undo. Osobno sprawdź
  pojedynczy swap, oba panele Split View i Quick Look.

Patch wymaga `./scripts/build.sh`, focused `batch_rename panes actions`, pełnej
regresji oraz `git diff --check` na docelowym KDE/CachyOS. Brak wyniku z
lokalnego buildu nie jest PASS. Nie instalować ani nie commitować przed akceptacją.

## Stage 3C.2B.2A — wspólne Undo/Redo dla czystych łańcuchów (patch do testów)

**Bieżący zakres:** po pełnym sukcesie partii zawierającej **wyłącznie lokalne
acykliczne zmiany nazw** (minimum dwa różne inode, jeden katalog, bez swapów
ani cykli) jedno Ctrl+Z cofa wszystkie pozycje, a jedno Ctrl+Y ponawia.
Dotyczy też kilku niezależnych łańcuchów w tym samym planie. Kwalifikujący się
plan nie jest już wykonywany przez serię jobów KIO: cały mapping przechodzi
journalowany replay `RENAME_NOREPLACE`, a jedno wspólne Undo powstaje dopiero
po pełnym sukcesie. W trakcie nie ma Cancel. Odrzucenie ostrzeżenia przed
startem bezpiecznie kończy operację bez zmian.

Po pełnym sukcesie kontroler sprawdza całą mapę inode i zapisuje jeden lokalny
wpis historii. Tak jak przy wymianach, **starsze polecenia Undo są odcinane**,
ponieważ ich zapamiętane ścieżki mogą już odnosić się do innych elementów.
Dialog uprzedza o tym przed rozpoczęciem; domyślny wybór to „Nie”. Jeżeli
zapis zbiorczy nie przejdzie kontroli, oryginalne wpisy KIO nie są odcinane.

Każde Undo/Redo działa poza wątkiem GUI przez kolejne
`renameat2(RENAME_NOREPLACE)` w odpowiedniej kolejności. Przed pierwszym ruchem
i każdym kolejnym etapem sprawdzana jest **cała** oczekiwana mapa inode,
metadata i wszystkie puste ścieżki. `RENAME_NOREPLACE` zabrania nadpisania
nawet przy wyścigu między preflight a wywołaniem. Każdy zamiar i potwierdzony
krok trafiają przez `QSaveFile`/`fsync` do `linear-*.json` w katalogu dzienników.
Zwykły błąd próbuje cofnąć tylko sprawdzone kroki w odwrotnej kolejności.
Niespójny inode, niejednoznaczny stan lub nieudany trwały zapis pozostawia
dziennik i wymaga ręcznej kontroli. Dzienniki `linear-*` i `cycle-*` korzystają
z **tej samej blokady** i wspólnego mechanizmu blokującego kolejne operacje.

**Ograniczenia:** jeden krok w UI nie oznacza atomowości plików; awaria procesu
może pozostawić częściowe przemianowanie. Dziennik służy do ręcznej kontroli,
**nie ma automatycznego odtwarzania po awarii**. Historia istnieje tylko w
bieżącym procesie. Zdalne URL-e, operacje z różnymi rodzicami, wybrane wielokrotnie
inode/hardlinki i dowolna partia zawierająca **jednocześnie łańcuchy i
swapy/cykle** nie uzyskują nowego wspólnego Undo; zachowują wcześniejsze
zachowanie, w tym brak wspólnego Undo dla partii mieszanych.

### Testy KDE — wyłącznie nowe, jednorazowe pliki

- [x] Dwa różne łańcuchy w jednym katalogu: pliki `1`,`41`,`2`,`42`, różna
      zawartość, prefiks `4`; wynik `41`,`441`,`42`,`442`, bez nadpisywania.
- [x] Jedno Ctrl+Z przywraca **wszystkie cztery** nazwy i zawartości;
      jedno Ctrl+Y ponownie wykonuje obie sekwencje.
- [ ] Po pełnym sukcesie nie ma `linear-*.json`; w trakcie odtwarzania
      nie powinien blokować się GUI.
- [x] W osobnym, świeżym katalogu: po zakończeniu zastąpić jeden inode
      **wyłącznie na kopiach testowych**; Undo/Redo ma odmówić, nie dotykając
      obcego pliku.
- [ ] Sztucznie zająć wcześniej pustą nazwę `1` po zmianie: całe Undo ma
      odmówić przed pierwszą operacją, a obcy plik ma pozostać nienaruszony.
- [x] Odrzucenie ostrzeżenia przed startem: zero zmian. To nie jest i nie ma być
      anulowanie journalowanego replay w trakcie.
- [ ] Opcjonalnie, jeśli zakres wydania obejmuje plany niekwalifikujące się do
      wspólnej historii (np. zdalne URL-e), przetestować ich odrębną ścieżkę KIO
      z Cancel i indywidualnym Undo. Nie wymuszać tego testu na kwalifikującej
      się lokalnej partii ani nie oznaczać anulowania w trakcie jako PASS.
- [x] Para swap, izolowany cykl 3+, aktywny lewy panel Split View oraz Undo/Redo
      bez regresji. Stage 2 osobno potwierdza preview w obu panelach i Quick Look.

Wymagane lokalne kontrole: `./scripts/build.sh`, focused `batch_rename panes
actions`, pełne `--all`, `git diff --check`. Ostatnia zapisana pełna regresja:
23 zestawy / 3865 asercji PASS, sprzed najnowszej sesji ręcznej; **brak świeżego
rerunu nie jest nowym PASS.**
Bez commita, instalacji, taga, ZIP i zmiany wersji 0.27.0 do akceptacji.
