# Batch Rename 0.28.0 — Etap 4A: izolowany swap, audyt bezpieczeństwa

Status: **4A NIEZALICZONY — podfaza testowa 4A.1B zaliczona, produkcyjne recovery swapu nieaktywne**.

## Podfaza 4A.1 — stan implementacji testowej

Dodano rygorystyczne rozpoznawanie `kind=swap` w parserze v2 oraz izolowany,
wywoływany wyłącznie przez testy silnik polityki B. Silnik używa jednego
zweryfikowanego `dirfd`, zapisuje trwały intent, wykonuje najwyżej jeden
`renameat2(RENAME_EXCHANGE)`, synchronizuje katalog danych, sprawdza pełne
snapshoty obu nazw i dopiero po trwałym celu usuwa journal. Stan `after`
zawsze pomija syscall, co chroni przed ponownym exchange po awarii.

Produkcja nadal automatycznie odzyskuje wyłącznie `kind=linear`; nazwa swapu
jest widoczna dla audytu i zamyka gate. Generator produkcyjny, GUI Execute,
Undo/Redo i startupowa allowlista swapu nie zostały zmienione.

Zweryfikowana dotąd procesowa macierz obejmuje Execute/Undo/Redo oraz SIGKILL
po prepared i przed/po intent, exchange, fsync katalogu danych, postcheck,
verified i goal; po każdym przypadku recovery osiąga zapisany cel, zachowuje
payloady i kolejny restart jest czysty. Focused Batch Rename: **1272 asercje
PASS**.

Podfaza **4A.1B jest ZALICZONA wyłącznie dla izolowanego silnika testowego**.
Macierz obejmuje drugi SIGKILL podczas recovery Execute/Undo/Redo, kolejne
czyste restarty, dwóch równoległych recovery, kolejność i czas życia blokad,
błędy fsync pliku i katalogu journalu (w tym cleanup) oraz katalogu danych,
ENOSYS/EOPNOTSUPP, konflikty inode/braku nazwy/metadanych, podmianę katalogu i
sterowane TOCTOU przed oraz po syscallu. Walidator fail-closed odrzuca niespójne
plans/states/digest, dodatkowe klucze, niepełną mapę, powtórzoną tożsamość i
symlink journalu. Wszystkie przypadki używają jednorazowych katalogów
tymczasowych i prywatnego `XDG_DATA_HOME`.

Wyniki 2026-09-20: `./scripts/build.sh` PASS; focused `batch_rename panes
actions` = **1757 + 287 + 157 = 2201 asercji PASS**; pełne `--all` = **23
zestawy / 5376 asercji PASS**; `git diff --check` PASS.

`RENAME_EXCHANGE` atomowo zamienia dwa istniejące wpisy nazw i nie ma semantyki
`NOREPLACE`. Kontrole inode/metadanych przed wywołaniem i pełny postcheck
wykrywają ingerencję, lecz nie eliminują nieredukowalnego wyścigu z procesem,
który nie respektuje blokady i zmienia wpisy pomiędzy ostatnim sprawdzeniem a
syscallem. Linux nie oferuje warunkowego exchange „tylko jeśli oba inode nadal
są oczekiwane”; dlatego gwarancja jest ograniczona do współpracujących procesów
i wykrywania po fakcie, bez automatycznego rollbacku stanu niejednoznacznego.

Testy TOCTOU nie obiecują niemożliwej gwarancji stałych nazw: potwierdzają
zachowanie obu oryginalnych payloadów i obcego payloadu, zatrzymanie dalszych
checkpointów, zachowanie journalu i zamknięcie gate po wykryciu niepewności.

Zakres tej podfazy jest celowo ograniczony do audytu i zamrożenia kontraktu.
Nie zmieniono produkcyjnego dispatchu, startup recovery ani historii Undo/Redo.
Etap 3 local-linear pozostaje jedyną automatycznie odzyskiwaną ścieżką v2.

## Ustalenia audytu

- Execute pojedynczego swapu nadal wywołuje `renameat2(RENAME_EXCHANGE)` z
  kontynuacji GUI w `FileActions`.
- Swap Undo/Redo nadal wykonuje precheck, syscall i postcheck synchronicznie w
  `UndoController::replayRecordedSwap()` na wątku GUI.
- Istniejący v2 jest manifestem liniowym. Parser formalnie rozpoznaje także
  `kind=cycle`, ale jego `inspectMapping()` / `mappingAt()` opisuje wyłącznie
  kolejne przeniesienia źródło→cel. Nie wolno użyć tego mapowania do swapu.
- Aktualny produkcyjny startup automatycznie uruchamia wyłącznie dokładnie jeden
  poprawny `kind=linear`. v1, corrupt/unknown/multiple/temp oraz cycle pozostają
  fail-closed/manual-only.
- Kolejność blokad nowych współpracujących ścieżek jest poprawna i musi zostać
  zachowana: kernelowy `flock(recovery-audit.lock)` → `cycle.lock`. Silnik swapu
  nie może samodzielnie brać tych blokad w odwrotnej kolejności.
- Obecny v1 cycle journal oraz rollback cykli 3+ nie są podstawą 4A i nie będą
  migrowane ani rozszerzane.

## Zamrożony manifest v2 swapu

Manifest pozostaje w schema `2`, lecz ma osobny `kind: "swap"`. Dokładny zestaw
kluczy głównych pozostaje zgodny z rygorystycznym parserem v2:

```json
{
  "schema": 2,
  "scope": "recovery-audit-v2",
  "kind": "swap",
  "directory": "/bezwzgledny/katalog",
  "direction": "forward|undo|redo",
  "phase": "prepared|exchange-intent|exchange-verified|verified-complete",
  "completedSteps": 0,
  "directoryDevice": "decimal-u64",
  "directoryInode": "decimal-u64",
  "items": [
    {
      "row": 0,
      "source": "/bezwzgledny/katalog/lewa",
      "destination": "/bezwzgledny/katalog/prawa",
      "device": "decimal-u64",
      "inode": "decimal-u64",
      "mode": "decimal-u64",
      "size": "decimal-i64",
      "mtimeNs": "decimal-i64",
      "type": "file|directory|symlink"
    },
    {
      "row": 1,
      "source": "/bezwzgledny/katalog/prawa",
      "destination": "/bezwzgledny/katalog/lewa",
      "device": "decimal-u64",
      "inode": "decimal-u64",
      "mode": "decimal-u64",
      "size": "decimal-i64",
      "mtimeNs": "decimal-i64",
      "type": "file|directory|symlink"
    }
  ],
  "digest": "sha256-kanonicznego-json-bez-digest"
}
```

`items` zawsze ma dokładnie dwa elementy, różne `(dev,ino)`, wspólny katalog i
pełną mapę wzajemną: `item[0].destination == item[1].source` oraz odwrotnie.
`completedSteps` ma wyłącznie wartości 0 albo 1. Dla każdego kierunku manifest
zapisuje oczekiwane mapy:

- `before`: `item[0]` pod `source[0]`, `item[1]` pod `source[1]`;
- `after`: `item[1]` pod `source[0]`, `item[0]` pod `source[1]`.

Dla Undo plan wejściowy jest budowany z ukończonego swapu, dlatego jego
`before` to stan po Execute, a `after` to stan pierwotny. Redo ma odwrotnie.
Kierunku nie wolno inferować z fazy ani z aktualnych nazw.

Rozpoznanie stanu jest niezależne od `phase` i porównuje oba lstat-y z pełnym
snapshotem `(dev, ino, type, mode, size, mtimeNs)`:

- dokładnie `before` → exchange jeszcze nie zaszedł;
- dokładnie `after` → exchange już zaszedł i nie wolno go powtarzać;
- każdy inny wynik, brak wpisu, obcy/reused inode, zmiana metadanych lub błąd
  stat → konflikt; zero syscalli i journal pozostaje.

## Protokół polityki B

Po uzyskaniu blokad w stałej kolejności i po walidacji katalogu przez trwały
dirfd silnik wykonuje dokładnie:

1. pełny precheck `before` lub deterministyczne stwierdzenie `after`;
2. trwały `exchange-intent` (`QSaveFile` commit, fsync pliku i katalogu journalu);
3. jeżeli stan to `before`: pojedynczy `renameat2(dirfd, left, dirfd, right,
   RENAME_EXCHANGE)`; bez fallbacku;
4. `fsync()` tego samego otwartego data-dirfd;
5. pełny postcheck `after` obu nazw i obu pełnych snapshotów;
6. trwały `exchange-verified`, następnie trwały cel `verified-complete`;
7. unlink journala i fsync katalogu journalu.

Jeżeli recovery zastaje `after`, pomija syscall i przechodzi przez data-dir
fsync, pełny postcheck, verified i goal. To daje idempotencję po SIGKILL między
syscallem a checkpointem. ENOSYS, EOPNOTSUPP, każdy błąd fsync/renameat2,
niebezpieczny lock lub katalog oraz stan niejednoznaczny zachowują journal i
zamykają globalny gate. Nie ma rollbacku ani zgadywania.

## Warunki aktywacji

Produkcja pozostaje wyłączona do osobnego zaliczenia kolejno:

1. [zaliczone w 4A.1B] test-hooked parser/silnik i macierz SIGKILL/fault w
   osobnych procesach, wyłącznie `QTemporaryDir` i prywatne `XDG_DATA_HOME`;
2. [zaliczone w 4A.1B] kill przed/po intent, syscall, data-dir fsync,
   postcheck, verified, goal i unlink; kill recovery, drugi restart, dwa
   procesy, TOCTOU i obcy inode;
3. Execute, Undo i Redo przez prawdziwe produkcyjne kontrolery oraz kontrola
   payload/inode;
4. przeniesienie Execute i Undo/Redo do workerów z trwałymi kierunkowymi
   statusami, zachowaniem pojedynczego Ctrl+Z/Ctrl+Y i startup gate;
5. izolowane black-box `thispc-view`, responsywność GUI, focused i pełna regresja;
6. dopiero wtedy kontrolowana zmiana allowlisty startupu na `kind=swap`.

Cykle 3+, KIO fallback i power-loss pozostają poza zakresem. SIGKILL nie jest
dowodem odporności na zanik zasilania; takie testy wymagają przyszłej disposable
VM/FS-image i nie będą uruchamiane na hoście.
