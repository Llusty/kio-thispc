# Batch Rename 0.28.0 — crash recovery, Etap 3

## Status

**Podfaza 3A: PASS jako test-hooked silnik i schema.**

**Podfaza 3B: PASS jako izolowana macierz bezpieczeństwa testowego silnika.**

**Podfaza 3C.1: PASS jako izolowana integracja UI i black-box testowego
binarium.**

**Podfaza 3C.2: PASS — kontrolowana aktywacja produkcyjna wyłącznie dla
kwalifikującego się local-linear Undo/Redo.**

**Etap 3: PASS w zakresie local-linear i polityki B.**

Produkcja zapisuje kwalifikujące się liniowe Undo/Redo jako v2, a startup
dopuszcza dokładnie jeden poprawny v2 `forward`, `undo` albo `redo` i dokańcza
wyłącznie zapisany kierunek. V1, cykle, corrupt/unknown, multiple, suspicious
temp oraz live/ambiguous lock pozostają manual-only/fail-closed. Nie zmieniono
wersji 0.27.0.

## Zrealizowany zakres 3A

- ścisły parser v2 rozpoznaje `direction=undo` i `direction=redo` bez migracji v1;
- generator testowy zapisuje pełny manifest v2 z metadanymi inode zajmującego
  ścieżkę wejściową danego kierunku;
- wspólny silnik realizuje politykę B bez zmiany kierunku;
- Undo wykonuje kroki w deterministycznej kolejności odwrotnej, Redo w kolejności
  wykonania planu;
- każdy krok zachowuje sekwencję `intent → renameat2(RENAME_NOREPLACE) → fsync
  tego samego dirfd → pełny postcheck → verified`;
- final goal i usunięcie journalu następują dopiero po pełnym trwałym wyniku;
- test-hooked startup dokańcza dokładnie jeden poprawny liniowy v2 Undo/Redo.

## Dowody

Wszystkie nowe przypadki używają `QTemporaryDir`, prywatnego recovery root pod
testowym XDG i osobnych procesów. Nie dotykają rzeczywistych journalów ani danych
użytkownika.

- 12 punktów SIGKILL generatora × 2 kierunki = 24 przypadki procesowe;
- po każdym journalu startup dokańcza zapisany kierunek i drugi restart jest
  idempotentny;
- zwykłe grupowe Ctrl+Z/Ctrl+Y i v1/manual-only pozostają objęte regresją;
- focused Batch Rename: **864/864 PASS**;
- pełne `--all`: **PASS**, w tym Batch Rename **864/864 PASS**;
- produkcyjny build: **PASS**.

## Brakujący gate przed aktywacją produkcyjną

- 3C.2: końcowy audyt wyboru ścieżki w `UndoController` oraz osobna decyzja o
  przełączeniu produkcyjnego generatora v1 na v2 i dopuszczeniu startupowego
  Undo/Redo. 3C.1 nie wykonuje żadnego z tych przełączeń.

Do czasu przejścia tych gate'ów nie wolno włączać produkcyjnego v2 Undo/Redo ani
uznawać Etapu 3 za zakończony. Swapy, cykle, KIO fallback, power-loss i Etap 4
pozostają poza zakresem.

## Dowody 3B

- 14 punktów drugiego SIGKILL podczas samego recovery × Undo/Redo; po każdym
  przypadku dwa kolejne procesy kończą zapisany kierunek i potwierdzają
  idempotentny, trwały cel;
- wymuszone błędy `fsync` journal-file, journal-dir i data-dir oraz
  `renameat2` z `ENOSYS`/`EOPNOTSUPP` dla obu kierunków pozostawiają journal,
  nie nadpisują obcych inode'ów i pozwalają na bezpieczny kolejny restart;
- kierunkowe warianty TOCTOU precheck→syscall, dwóch równoległych procesów oraz
  konfliktów: obcy/brakujący inode źródła, zajęta luka, zmiana metadanych i
  podmieniony katalog;
- wspólne testy parsera/fence nadal rozróżniają v1, corrupt, unknown, multiple,
  suspicious temp oraz live/stale/ambiguous lock; zwykłe Execute→Undo→Redo
  jednym Ctrl+Z/Ctrl+Y pozostaje regresją;
- testy używają wyłącznie osobnych procesów, `QTemporaryDir` i izolowanego XDG;
  nie testowano ani nie deklaruje się gwarancji power-loss hostowego systemu plików;
- produkcyjny build: **PASS**;
- focused Batch Rename: **1007/1007 PASS**;
- pełne `--all`: **PASS**, w tym Batch Rename **1007/1007 PASS**;
- `git diff --check`: **PASS**.

## Dowody 3C.1

- niemodalna, trwała etykieta statusu rozróżnia checking, recovering, success
  i conflict dla Undo oraz Redo; sukces jawnie informuje, że historia Undo/Redo
  sprzed restartu nie jest odtwarzana;
- osobny, nieinstalowany cel `thispc-view-stage3c1-test` buduje dokładnie kod
  aplikacji z istniejącym makrem testowym; tylko on przyjmuje izolowany recovery
  root, kontrolowane opóźnienia, ślad prezentacji i automatyczne wyjście testu;
- prawdziwe procesy GUI w prywatnym `XDG_DATA_HOME`, `XDG_CONFIG_HOME`,
  `XDG_CACHE_HOME`, `XDG_RUNTIME_DIR` i `QTemporaryDir` przechodzą startup →
  rozpoznanie kierunku → Undo/Redo recovery → trwały cel oraz drugi restart;
- testy sprawdzają nazwy, treści i pary dev+ino wszystkich czterech plików;
  konflikt zachowuje journal, blokadę, obcy inode i jego treść bez zmian;
- heartbeat głównego event loop podczas kontrolowanie spowolnionego workera
  potwierdza responsywność UI; czysty startup oraz istniejąca regresja zwykłego
  Execute→Undo→Redo v1 pozostają bez awarii;
- osobny proces produkcyjnego `thispc-view` potwierdza, że startup nadal nie
  wykonuje automatycznie v2 Redo; produkcyjny gate pozostaje forward-only;
- focused Batch Rename: **1032/1032 PASS**;
- pełne `--all`: **PASS**, w tym Batch Rename **1032/1032 PASS**;
- produkcyjny build i izolowany build testowy: **PASS**;
- `git diff --check`: **PASS**.

Na zamknięciu 3C.1 cały Etap 3 pozostawał **NIEZALICZONY** do 3C.2:
generator v2 Undo/Redo i automatyczny startup Undo/Redo nie były jeszcze
włączone produkcyjnie.

## Dowody 3C.2

- audyt potwierdził lock-order `recovery-audit.lock` (`flock`) → `cycle.lock`,
  pełny manifest dev/ino/mode/type/size/mtime i luk oraz sekwencję
  `intent → renameat2(RENAME_NOREPLACE) → fsync(data-dirfd) → pełny postcheck
  → verified`;
- `UndoController` używa v2 wyłącznie dla istniejącego gate'u local-linear;
  swapy, cykle i KIO fallback nie zostały przełączone;
- procesowa macierz 12 punktów SIGKILL × Undo/Redo przechodzi przez prawdziwe
  `UndoController::undo()`/`redo()`, prywatny `XDG_DATA_HOME` i osobne procesy;
  startup dokańcza zapisany kierunek, a drugi restart jest idempotentny;
- rzeczywisty produkcyjny `thispc-view` dokończył v2 Redo w izolowanym XDG;
  testy UI obu kierunków, konfliktu, obcego inode, responsywności i utraty starej
  pamięci historii po restarcie pozostały PASS;
- `./scripts/build.sh`: PASS;
- focused `batch_rename panes actions`: **1032 + 287 + 157 = 1476 PASS**;
- pełne `--all`: **23 zestawy / 4651 asercji PASS**;
- `git diff --check`: PASS.

Podfaza 3C.2 i Etap 3 są zaliczone wyłącznie w opisanym zakresie local-linear.
Nie deklaruje się gwarancji power-loss bez disposable VM/FS-image. Swapy,
cykle, KIO fallback i Etap 4 pozostają poza zakresem. Wersja pozostaje 0.27.0;
release 0.28.0 nadal jest wstrzymany.
