# ThisPC 0.45.0 — Stage 5.1: Kompleksowy Audyt Bezpieczeństwa (`SECURITY_AUDIT_045`)

- **Data audytu:** 2026-11-08
- **Audytowany stan repozytorium (HEAD / `main` / `origin/main`):** `be84431295caff83a0582b13d9f8df9d31e08950` (*feat(transfers): complete 0.45.0 stage 4 show in folder action*)
- **Zweryfikowany backup bazowy (Stage 4):** `/mnt/g/ThisPC backup/ThisPC-0.45.0-Stage4-ManualAccepted-2026-11-08/`
- **Wersja zainstalowana w systemie:** `0.44.0` (nienaruszona)
- **Zakres audytu:** Cała baza kodowa projektu `kio-thispc` / `thispc-view` (wszystkie moduły w `src/`, `data/`, `scripts/`, `CMakeLists.txt`, `install.sh`, `uninstall.sh` oraz zestaw testów regresyjnych `tests/`), ze szczególnym uwzględnieniem komponentów wydania `0.45.0` (Etapy 1–4: Powiadomienia KDE Plasma, Trwała historia operacji, Bezpieczne ponawianie `Safe Retry`, akcja `Pokaż w folderze`).
- **Werdykt Security Release Gate (Stage 5.1):** **`BLOCKED` (ZABLOKOWANY do czasu wykonania Stage 5.2 — Security Remediation & Hardening)**
  - **Podatności `CRITICAL`:** `0`
  - **Podatności `HIGH`:** `0`
  - **Złośliwy kod / backdoory / ukryta telemetria:** `0` (brak jakichkolwiek oznak)
  - **Znaleziska `MEDIUM` (wymagające naprawy przed wydaniem 0.45.0 w Stage 5.2):** `2` (`SEC-045-01`, `SEC-045-02`)
  - **Znaleziska `LOW` (zalecane utwardzenie w Stage 5.2):** `6` (`SEC-045-03` .. `SEC-045-08`)

---

## 1. Zakres i metodologia audytu

Audyt przeprowadzono dwutorowo:
1. **Pełny przegląd manualny kodu źródłowego (Obszary A–G):**
   - Przeanalizowano wszystkie pliki `.cpp` i `.h` w katalogu `src/` (łącznie 87 plików źródłowych i nagłówkowych), skrypty budowania i pakowania (`CMakeLists.txt`, `install.sh`, `uninstall.sh`, `scripts/build.sh`, `scripts/build-rc-archives.py`, `scripts/check-version.sh`) oraz pliki integracji pulpitu (`data/org.kde.thispcview.desktop`, `data/org.kde.thispcview.notifyrc`).
2. **Analiza automatyczna (statyczna i dynamiczna):**
   - **Świeża kompilacja produkcyjna (`./scripts/build.sh`):** `PASS` (GCC `16.2.1`, kod wyjścia `0`, `0` ostrzeżeń, `0` błędów).
   - **Rygorystyczna kompilacja statyczna GCC 16.2.1 (`-Wall -Wextra -Wformat=2 -Wundef -O2`):** `PASS` (wykryto 1 ostrzeżenie optymalizatora `-Wstringop-overflow=` w `src/duplicatefinderjob.h:144`, sklasyfikowane jako `SEC-045-08`).
   - **Analiza statyczna LLVM `clang-tidy 23.1.4` (`clang-analyzer-*`, `bugprone-*`, `cert-*`, `concurrency-*`):** wykonana dla wszystkich jednostek translacji `src/*.cpp`. Wykryto:
     - `[bugprone-use-after-move]` w `src/transferqueuecontroller.h:188:84` (`SEC-045-03`),
     - `[clang-analyzer-core.CallAndMessage]` (brak sprawdzenia wskaźnika `m_details` przed wywołaniem metody) w `src/splitbrowserpane.h:1830:9` (`SEC-045-08`),
     - `[concurrency-mt-unsafe]` dla wywołań `std::strerror(errno)` w wątkach roboczych (`src/checksumjob.h:187`, `src/batchrename.h`, `src/batchrenamerecovery.h` — `SEC-045-08`).
   - **`cppcheck`:** `NOT RUN` — narzędzie nie jest zainstalowane w lokalnym środowisku użytkownika (`cppcheck: nie znaleziono polecenia`).
   - ** Analiza dynamiczna AddressSanitizer (`ASan`) + UndefinedBehaviorSanitizer (`UBSan`):**
     - Przebudowano binaria testowe z flagami `-fsanitize=address,undefined -fno-omit-frame-pointer` i uruchomiono zestawy:
       - `history` (67 zestawów / 2008 asercji): **PASS (0 błędów ASan/UBSan)**,
       - `operations` (203 asercje): **PASS (0 błędów ASan/UBSan)**,
       - `admin_retry`, `admin_retry_stage2`, `admin_retry_stage3`, `admin_retry_stage4`, `admin_retry_stage5`, `local_transfer`, `transfer_plan`, `local_move`, `archive`, `archive_jobs`, `archive_creation`, `batch_rename`, `remote_url`, `saved_remote`, `checksums`, `notifications`, `acl`, `acl_editor`, `properties_xattr_edit`, `aggregate_permissions`, `duplicate_finder`, `duplicate_actions`: **PASS (22 zestawy, 0 błędów ASan/UBSan)**,
       - `transfer_queue`: **WYKRYTO RZECZYWISTY BŁĄD `heap-use-after-free` (`SEC-045-01`)** w `OperationWindow::rebuild()` (`src/operationmanager.h:2357`, `2926`) wyzwalany przez nieanulowany timer `OperationWindow::scheduleRebuild()` podczas synchronicznego wywołania `rebuild()`.
   - **Pełna regresja funkcjonalna (`python3 tests/run-pane-actions.py --all`):** **77/77 zestawów PASS**.

---

## 2. Szczegółowy przegląd obszarów A–G

### Obszar A — Potencjalnie złośliwy kod i backdoory (`CLEAN` — brak złośliwego kodu)

1. **Połączenia sieciowe i telemetria:**
   - W całej bazie kodowej nie występują klasy `QNetworkAccessManager`, `QTcpSocket`, `QUdpSocket`, `QSslSocket`, `QHttpMultiPart` ani wywołania `curl`/`wget`.
   - Jedynym użyciem modułu `Qt6::Network` jest `QLocalServer` / `QLocalSocket` w testach izolacji wieloprocesowej (`tests/history-test.cpp`).
   - Adresy `http://` i `https://` pojawiają się wyłącznie na listach zabronionych schematów (np. w `resolveShowInFolder` w `src/operationmanager.h:425` oraz `RemoteUrlHelper` w `src/remoteurlhelper.cpp`) w celu ich odrzucenia (`UnsupportedScheme`).
2. **Wykonywanie zewnętrznych procesów (`QProcess`, `system`, `exec*`, `popen`):**
   - Brak jakichkolwiek wywołań `system()`, `popen()`, `execve()`, `execl*()`, `fork()` (poza kontrolowanym procesem potomnym `fork()` + Landlock ABI 3+ w testach i narzędziu archiwów), `dlopen()` lub `QLibrary`.
   - Wszystkie uruchomienia programów zewnętrznych w kodzie produkcyjnym są jawne, inicjowane wyłącznie przez akcję użytkownika i realizowane bez udziału powłoki (`sh -c` / `bash -c`):
     - `src/thispcview.cpp:298`, `src/appwidgets.cpp:44`: `QProcess::startDetached("dolphin", ...)` — akcja „Otwórz w Dolphinie”.
     - `src/thispcview.cpp:5453, 5508, 5525, 5539`: `KTerminalLauncherJob` / `QProcess::startDetached` dla akcji „Otwórz w terminalu” we wskazanym katalogu lokalnym.
     - `src/selectionmenucontroller.cpp:309`: `QProcess::startDetached("okular", {"--print", localFile})` — drukowanie pliku PDF.
     - `src/selectionmenucontroller.cpp:321`: `QProcess::startDetached("plasma-apply-wallpaperimage", {localFile})` — ustawienie tapety pulpitu.
     - `src/archive-extraction.h:510`: uruchamianie `ark` / `7z` / `unzip` / `tar` przez `QProcess` z twardą separacją argumentów (`--`) oraz izolacją systemu plików **Linux Landlock ABI 3+** (`applyArchiveConfinement` w `src/archive-extraction.h:168-305`) i `PR_SET_NO_NEW_PRIVS`.
     - **Weryfikacja Stage 4 („Pokaż w folderze”):** `OperationWindow::triggerShowInFolder` (`src/operationmanager.h:2905-2918`) oraz `ThisPCView` (`src/thispcview.cpp:3146-3220`) realizują akcję wyłącznie wewnętrznie przez nawigację w aktywnym panelu `ThisPCView` — nigdy nie uruchamiają Dolphina ani procesów zewnętrznych.
3. **Kod zaciemniony, zakodowane ładunki, ukryte ścieżki testowe:**
   - Brak zaciemnionego kodu, dynamicznego konstruowania poleceń i ukrytych payloadów Base64/hex (jedyny konwerter hex w `src/propertiesxattrwriter.h` służy do walidacji wpisów `user.*` xattr w oknie Właściwości).
   - Wykryto dwa miejsca w kodzie produkcyjnym (`src/transferqueueui.h:547` oraz `src/duplicateactioncontroller.h:229`), gdzie `qEnvironmentVariableIsSet("THISPC_TEST_HARNESS")` jest sprawdzane w czasie wykonania bez dyrektywy `#ifdef THISPC_TEST_HARNESS` (opisane jako `SEC-045-04`).
4. **Autostart, trwałość i modyfikacje profilu użytkownika:**
   - Brak zapisów do `~/.config/autostart`, `~/.bashrc`, `~/.config/fish`, `systemd --user`, `cron` czy usług D-Bus.
   - Zapisy konfiguracyjne i stanowe są ściśle ograniczone do standardowych lokalizacji `QSettings` oraz `QStandardPaths::GenericStateLocation` (`kio-thispc/thispc-view/operations_history.json` i dziennika odzyskiwania `batch-rename-recovery/`).

### Obszar B — Bezpieczeństwo operacji plikowych

1. **Kopiowanie i przenoszenie lokalne (`LocalFileCopyJob`, `LocalFileMoveJob`, `LocalTransferJob`, `KioStagedTransferJob`):**
   - `LocalFileCopyWorker` (`src/localfilecopyjob.h:164-322`) zapisuje dane do unikalnego pliku tymczasowego `.thispc-part.<uuid>` w katalogu docelowym (`O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC`, uprawnienia `0600`), otwiera źródło z `O_RDONLY | O_NOFOLLOW | O_CLOEXEC`, weryfikuje `fstat` (zwykły plik, zgodność `st_dev` + `st_ino` z wynikiem `lstat`) i po zakończeniu wykonuje `fsync(dstFd)` oraz `fsync(parentDirFd)`.
   - W przypadku anulowania lub błędu (np. `ENOSPC`) plik `.thispc-part` jest bezpiecznie usuwany po uprzednim sprawdzeniu `lstat` (`st_dev` + `st_ino`), dzięki czemu nigdy nie zostaje usunięty cudzy plik docelowy (`src/localfilecopyjob.h:119-142`).
   - Przy nadpisywaniu istniejącego pliku (`m_overwrite == true`) `LocalFileOverwrite::replaceDestinationWithBackup` (`src/localfileoverwrite.h`) stosuje bezpieczną zamianę z kopią zapasową (`.thispc-overwrite-backup.<uuid>`) oraz pełnym rollbackiem w razie błędu.
   - **Zidentyfikowana luka TOCTOU (`SEC-045-02`):** W trybie bez nadpisywania (`m_overwrite == false`) `LocalFileCopyWorker::run()` (`src/localfilecopyjob.h:318`) oraz `LocalFileMoveJob::begin()` (`src/localfilemovejob.h:153`) używają `QFile::rename(src, dst)` (czyli `rename(2)` bez `RENAME_NOREPLACE`) po wcześniejszym teście `!QFileInfo::exists(dst)`. Jeżeli w oknie czasowym kopiowania inny proces utworzy plik pod ścieżką docelową, `QFile::rename` nadpisze go zamiast zwrócić `EEXIST`. (Dla porównania `KioStagedDetail::atomicPublishNewFile` w `src/kiostagedtransferjob.h:253` oraz `ArchiveCreationWorker` w `src/archive-creation.h:395` prawidłowo stosują `syscall(SYS_renameat2, ..., RENAME_NOREPLACE)`).
2. **Rezerwacja ścieżek i kolejka transferów (`TransferReservationRegistry`, `TransferQueueController`):**
   - `TransferReservationRegistry` (`src/transferreservationregistry.h`) normalizuje ścieżki leksykalnie (bez podążania za symlinkami na dysku) i blokuje jednoczesne operacje na identycznych lub zagnieżdżonych (rodzic/potomek) ścieżkach docelowych oraz źródłach operacji `Move`.
3. **Stage 3 — Safe Retry (`TransferQueueController::evaluateRetryEligibility`, `retryFailedTask`):**
   - Ponawianie jest dozwolone wyłącznie dla zadań `Copy` i `Move` z bieżącej sesji znajdujących się w stanie `Failed` (`src/transferqueuecontroller.h:762-975`).
   - Karty archiwalne wczytane z `operations_history.json` po restarcie aplikacji są bezwzględnie odrzucane (`ArchivalTaskNotSupported`), co eliminuje ryzyko ponowienia operacji bez pierwotnej migawki tożsamości i-węzłów (`sourceIdentities`).
   - Przed ponowieniem weryfikowane są: brak flagi `isAdminRetry`, istnienie wszystkich źródeł (`lstat`), ścisła zgodność `st_dev` + `st_ino` (ochrona przed podmianą pliku źródłowego lub zamianą na symlink), prawa zapisu w katalogu docelowym (`W_OK | X_OK`) oraz brak jakiegokolwiek konfliktu lub pliku `.thispc-part` w miejscu docelowym.
   - Nowa próba tworzy nowe zadanie z nowym `taskId` i nowym `historyUuid`, zachowując w historii zarówno pierwotny rekord `Failed`, jak i nowy rekord ponowienia.
4. **Stage 4 — „Pokaż w folderze” (`resolveShowInFolder` w `src/operationmanager.h:411-575`):**
   - Funkcja jest w 100% tylko do odczytu (wykonuje jedynie `::lstat` i `::access` bez jakichkolwiek modyfikacji plików).
   - Działa w trybie *fail-closed*: akceptuje wyłącznie operacje `Copy` i `Move` o statusie `Completed`; odrzuca `Failed`, `Cancelled` i stany aktywne; odrzuca schematy zdalne i wirtualne (`mtp:/`, `smb:/`, `sftp:/`, `trash:/`, `admin:/`, `http:/`); odrzuca ścieżki względne i segmenty `..`; odrzuca katalog docelowy będący dowiązaniem symbolicznym (`S_ISLNK(dirSt.st_mode)`).
   - Nawigacja i zaznaczenie są ściśle ograniczone do aktywnego panelu (`Primary` lub `Split View`), bez ingerencji w drugi panel.
5. **Batch Rename (`src/batchrename.h`, `src/batchrenamerecovery.h`, `src/batchrenamejournal.h`):**
   - Operacje liniowe i zamiany używają `SYS_renameat2` z flagami `RENAME_NOREPLACE` / `RENAME_EXCHANGE` oraz weryfikują `st_dev` + `st_ino` każdego elementu przed wykonaniem kroku.
   - Dziennik odzyskiwania (`BatchRenameJournalWriter`) stosuje uprawnienia `0700` dla katalogu i `0600` dla pliku, `O_NOFOLLOW`, sumy kontrolne CRC32/SHA-256 i izolację katalogów instancji `instance-<PID>`.
6. **Skaner pamięci masowej i wyszukiwarka duplikatów (`src/storagescanworker.h`, `src/duplicatefinderjob.h`, `src/duplicateactioncontroller.h`):**
   - `StorageScanWorker` używa `lstat`, nigdy nie podąża za symlinkami (`S_ISLNK`), pilnuje granicy systemu plików (`st_dev == rootDev`) i deduplikuje hardlinki po `(st_dev, st_ino)`.
   - `DuplicateActionController` nigdy nie usuwa plików automatycznie i przed przeniesieniem do Kosza lub katalogu wykonuje ponowną walidację `st_dev`, `st_ino`, `st_size` oraz `mtime`.

### Obszar C — Uprawnienia administracyjne (`admin://` i Polkit)

1. **Kontrakt i centralna allowlista (`src/adminretrycontract.h`, `src/adminretrycontroller.h`, `src/adminretryexecutor.h`):**
   - ThisPC nigdy nie uruchamia procesu GUI jako `root`.
   - `isGuiAdminRetryAllowed()` (`src/adminretrycontract.h:73-93`) dopuszcza wyłącznie cztery operacje jednopierwiastkowe:
     1. `CopyFileToProtected` (`file://` → `admin://`),
     2. `CopyFileFromProtected` (`admin://` → `file://`),
     3. `CreateProtectedFile` (`admin://`),
     4. `CreateProtectedFolder` (`admin://`).
   - Operacje `Rename`, `Move`, `Delete`, `Trash`, `EmptyTrash`, operacje wieloplikowe (batch), symlinki, hardlinki (`st_nlink > 1`) oraz zasoby zdalne są bezwzględnie zablokowane w GUI (`OperationDisallowedInGui`).
   - Wymagany jest jednorazowy, ważny przez 60 sekund token zgody użytkownika (`AdminUserConsent`), powiązany z konkretnym typem operacji i kanoniczną parą ścieżek `(sourcePath, destinationPath)`.
   - Ponowne wywołanie na zadaniu posiadającym znacznik `AlreadyRetriedAsAdmin` jest blokowane, co wyklucza pętle zapytań Polkit.
2. **Bezpieczeństwo artefaktów po nieudanym transferze administracyjnym (`AdminArtifactOwnershipAudit`):**
   - Klient GUI nigdy nie wywołuje `::unlink()` na plikach w katalogu chronionym po błędzie lub anulowaniu zadania `admin://`, lecz przeprowadza 9-punktowy audyt (`src/adminretryexecutor.h:122-245`) i zachowuje ewentualny artefakt w trybie *fail-closed*, rejestrując jego ścieżkę w `ActiveTargetRegistry`.
   - Dla `CopyFileFromProtected` plik docelowy jest tworzony lokalnie z `O_CREAT | O_EXCL | O_NOFOLLOW` i uprawnieniami `0600` (`rw-------`), należącymi do nieuprzywilejowanego użytkownika (`uid == geteuid()`).

### Obszar D — Zasoby zdalne i urządzenia (`RemoteUrlHelper`, `SavedRemoteLocation`, `MTP`, `UDisks2`)

1. **Zasoby zdalne (`src/remoteurlhelper.cpp`, `src/savedremotelocation.cpp`):**
   - `RemoteUrlHelper::validateConnectionInput` akceptuje wyłącznie protokoły `smb`, `sftp`, `ftp`, `webdav` i `webdavs`, odrzuca znaki sterujące i nieprawidłowe porty (`1..65535`).
   - `SavedRemoteLocation::normalizeRemoteUrl` wymusza `url.setPassword(QString())` — ThisPC **nigdy** nie zapisuje haseł w `QSettings` ani w plikach stanu; poświadczenia są obsługiwane wyłącznie przez natywny `KIO` / `KWallet`.
2. **Urządzenia MTP (`mtp:/`):**
   - Obowiązuje ścisła blokada bezpieczeństwa `MtpMoveVerdict::RefusedUnsupported` (`src/transferqueuecontroller.h`) dla wszystkich kierunków operacji `Move` obejmujących `mtp:/`, co chroni przed utratą plików źródłowych na skutek braku atomowej weryfikacji zapisu po stronie protokołu MTP.
   - Trwałe usuwanie na MTP (`src/thispcview.cpp`) wymaga znormalizowanej głębokości ścieżki `>= 3`, co chroni korzeń urządzenia i główne magazyny pamięci przed przypadkowym skasowaniem.
3. **Montowanie i odmontowywanie (`src/devicemountcontroller.cpp`, `src/deviceremovalcontroller.cpp`):**
   - Operacje `Unmount`, `PowerOff` i `Eject` korzystają z natywnych wywołań `QtDBus` do `org.freedesktop.UDisks2` z jawną flagą `auth.no_user_interaction = false` (respektującą systemowe reguły Polkit) i bezpiecznie przekierowują panele `Primary`/`Split` z odmontowanego woluminu do `thispc:/`.

### Obszar E — Pamięć, współbieżność i cykl życia obiektów Qt

1. **Wykryty błąd bezpieczeństwa pamięci (`SEC-045-01` — `MEDIUM`):**
   - Analiza dynamiczna `ASan` wykryła błąd `heap-use-after-free` w `OperationWindow::rebuild()` (`src/operationmanager.h:2356-2359, 2920-2937`).
   - `OperationWindow::scheduleRebuild()` używa anonimowego `QTimer::singleShot(120, this, ...)` chronionego jedynie flagą `m_rebuildScheduled`. Synchroniczne wywołanie `OperationWindow::rebuild()` (np. przy przełączeniu karty `showOperationsTab()`, `showQueueTab()`, w `showEvent()` lub w teście) **nie anuluje** wcześniej uzbrojonego timera 120 ms, a samo `rebuild()` niszczy widżety kart natychmiastowo przez `delete child->widget();` zamiast `deleteLater()`. Gdy timer 120 ms wyzwala się podczas kolejnego `processEvents()`, niszczy nowo utworzone widżety kart pod wskaźnikami trzymanymi na stosie wywołującego.
2. **Obserwator zmian na dysku (`QFileSystemWatcher` w `OperationWindow` — Stage 3 UX & Stage 4):**
   - `refreshRetryEligibilityWatchPaths()` (`src/operationmanager.h:2956-3065`) posiada twarde limity zasobów (`kMaxWatchedDirs = 50`, `kMaxWatchedFiles = 100`), 150 ms debouncing (`m_retryWatchDebounceTimer`), zero odpytywania (zero-polling), działa wyłącznie gdy okno `OperationWindow` jest widoczne (`isVisible()`) i natychmiast czyści ścieżki w `hideEvent()`.
   - Przed dodaniem ścieżki do `QFileSystemWatcher` wykonywana jest szybka walidacja `isSafeLocalWatchCandidate()` (wykluczająca ścieżki sieciowe/wirtualne i symlinki).

### Obszar F — Trwała historia i konfiguracja (`TransferHistoryManager`)

1. **Bezpieczeństwo magazynu `operations_history.json` (`src/transferhistorymanager.h`):**
   - Ścieżka pliku jest wyznaczana przez `QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation)` z pełnym poszanowaniem izolowanych zmiennych środowiskowych `XDG_STATE_HOME` i `XDG_CONFIG_HOME`.
   - Przed każdym odczytem i zapisem funkcja `isSymlinkPath()` (`::lstat` + `S_ISLNK`) sprawdza zarówno sam plik `operations_history.json`, jak i jego katalog nadrzędny — w razie wykrycia dowiązania symbolicznego operacja jest natychmiast przerywana (`SymlinkRejected`).
   - Zapisy odbywają się atomowo przez `QSaveFile` pod ochroną międzyprocesowej blokady `QLockFile` (`operations_history.json.lock`, limit czasu 500 ms, `staleLockTime = 30000 ms`), a po zatwierdzeniu wymuszane są prywatne uprawnienia `0600` (`::chmod(path, 0600)`).
   - Rozmiar pliku przy odczycie jest ograniczony do twardego limitu `kMaxFileSizeBytes = 2 MiB` (`2 * 1024 * 1024`), a przy zapisie najstarsze rekordy są przycinane tak, aby wynikowy JSON nigdy nie przekroczył 2 MiB ani limitu retencji (`50..500`, domyślnie `200`).
   - Uszkodzone lub obcięte pliki JSON nie powodują awarii — są zachowywane pod nazwą `.corrupted.<timestamp>.<counter>` z uprawnieniami `0600`, a stan w pamięci jest inicjowany czystym magazynem.
   - Mechanizm epok (`historyEpoch`) oraz zbiory `m_clearedEpochs` i `m_deletedUuids` gwarantują, że wyczyszczenie historii w jednej instancji nie zostanie nadpisane („wskrzeszone”) przez spóźniony zapis z drugiej instancji (Security Gate A).

### Obszar G — Instalator, skrypty budowania i zależności zewnętrzne

1. **Skrypty instalacyjne (`install.sh`, `uninstall.sh`):**
   - `install.sh` używa `set -e`, buduje projekt jako zwykły użytkownik i wywołuje `sudo` wyłącznie dla `cmake --install build` oraz opcjonalnego czyszczenia starych artefaktów w `/usr/local`.
   - **Zidentyfikowane ryzyko w `uninstall.sh` (`SEC-045-07` — `LOW`):** `uninstall.sh:20-22` odczytuje linie z pliku `build/install_manifest.txt` (znajdującego się w katalogu roboczym zwykłego użytkownika) i dla każdej linii wykonuje `sudo rm -f -- "$file"` bez walidacji prefiksu ścieżki. Ponadto `install.sh` i `uninstall.sh` używają `pkill -f '/kf6/kio/thispc'` oraz `pkill -f 'kio_thispc'`, co dopasowuje dowolną linię poleceń zawierającą ten podciąg.
2. **Zależności zewnętrzne w środowisku docelowym:**
   - Zweryfikowano wersje bibliotek systemowych linkowanych przez `thispc-view`:
     - `Qt6` (`Core`, `Gui`, `Widgets`, `Concurrent`, `DBus`, `Network`, `PrintSupport`, `Pdf`, `Multimedia`, `MultimediaWidgets`),
     - `KDE Frameworks 6` (`KF6 6.31.0`: `KIO`, `I18n`, `CoreAddons`, `Solid`, `FileMetaData`, `Notifications`),
     - `libarchive 3.8.11` (używana do bezpiecznego podglądu spisu archiwów bez wypakowywania oraz tworzenia archiwów z `O_NOFOLLOW`),
     - `zlib 1.3.1`, `libacl 2.4.0`, `TagLib`, `Exiv2`, `CUPS 2.4.20`.

---

## 3. Model zagrożeń (Threat Model)

| # | Aktor / Wektor zagrożenia | Granica zaufania (Trust Boundary) | Potencjalny wpływ | Istniejące zabezpieczenia w ThisPC | Pozostałe luki / odniesienie do znalezisk |
|---|---|---|---|---|---|
| **TM-1** | **Złośliwe nazwy plików i ścieżki** (znaki specjalne, bardzo długie nazwy, sekwencje `..`, spacje wiodące/końcowe, znaki sterujące, mylące rozszerzenia Unicode) | System plików / nazwy wpisów katalogowych → parsery ścieżek, komendy zewnętrzne i widoki UI | Wstrzyknięcie opcji CLI (argument injection), wyjście poza katalog docelowy (path traversal), uszkodzenie stanu UI | Brak wywołań powłoki (`sh -c`); `LaunchUrlResolver` i `ArchiveExtraction` odrzucają `..`; `ArchiveCreation` waliduje komponenty wpisów; w `ArchiveExtraction` stosowany jest separator `--` oraz Landlock ABI 3+ | `SEC-045-05` (brak `--` w pomocniczych wywołaniach `dolphin`, `okular`, `plasma-apply-wallpaperimage`) |
| **TM-2** | **Złośliwe dowiązania symboliczne i twarde (symlinks / hardlinks)** | Niezaufany katalog na dysku → silniki kopiowania, przenoszenia, właściwości, skanowania, archiwów, historii i „Pokaż w folderze” | Nadpisanie lub odczyt pliku poza katalogiem docelowym, pętla nieskończona skanera, eskalacja przez `admin://`, sfałszowanie pliku historii | `O_NOFOLLOW` w `LocalFileCopyWorker`, `PropertiesXattrWriter`, `AggregatePermissionsWriter`, `BatchRenameJournal`; `lstat` i zakaz symlinków w `AdminRetry`, `StorageScanWorker`, `TransferHistoryManager` oraz `resolveShowInFolder` | `SEC-045-06` (`AclController::write` używa ścieżkowego `acl_set_file` bez własnego strażnika `lstat`) |
| **TM-3** | **Zewnętrzny proces modyfikujący pliki w trakcie operacji (TOCTOU / podmiana i-węzła)** | Równoległy proces lokalny → okno czasowe między preflight a wykonaniem operacji Copy/Move/Retry/Rename/Properties | Nadpisanie nowo utworzonego pliku w miejscu docelowym, skopiowanie lub usunięcie podmienionego pliku źródłowego | Weryfikacja `(st_dev, st_ino)` między `lstat` i `fstat(fd)` w `LocalFileCopyWorker`, `Properties`, `BatchRename` i Stage 3 `Safe Retry`; `RENAME_NOREPLACE` w `KioStagedTransferJob`, `ArchiveCreation` i `BatchRename` | **`SEC-045-02` (`MEDIUM`)**: `LocalFileCopyWorker` i `LocalFileMoveJob` w trybie bez nadpisywania używają `QFile::rename` (`rename(2)`) zamiast `RENAME_NOREPLACE` |
| **TM-4** | **Uszkodzony lub spreparowany plik `operations_history.json`** | Plik w `~/.local/state/kio-thispc/thispc-view/` → parser `TransferHistoryManager` i widoki `OperationWindow` / `OperationPopup` | Odmowa usługi (alokacja ogromnej pamięci / zawieszenie GUI), wstrzyknięcie fałszywych ścieżek do `Safe Retry` lub `Pokaż w folderze` | Limit rozmiaru pliku `2 MiB` przed odczytem; odrzucenie symlinków (`lstat`); zachowanie pliku `.corrupted` przy błędnym JSON; blokada `Safe Retry` dla wszystkich rekordów archiwalnych (`ArchivalTaskNotSupported`); ścisła walidacja `resolveShowInFolder` | Brak otwartych luk; odporność potwierdzona w 67 zestawach testów `history` (w tym pod ASan/UBSan) |
| **TM-5** | **Złośliwy lub uszkodzony serwer SMB / SFTP / FTP / WebDAV** | Zdalny serwer sieciowy → worker KIO → `ThisPCView` / `DirectoryListingCore` / `TransferQueueController` | Zawieszenie wątku GUI, wykonanie zdalnego pliku wykonywalnego, wyciek poświadczeń | Asynchroniczne wywołania KIO z tokenami generacji; `setRunExecutables(false)` dla zdalnych URL; wyłączone automatyczne miniatury i skanowanie rekurencyjne dla zdalnych schematów; brak zapisu haseł w `SavedRemoteLocation` | Brak otwartych luk w kodzie ThisPC |
| **TM-6** | **Odłączany w locie nośnik USB / MTP podczas transferu lub przeglądania historii** | Urządzenie wymienne (`UDisks2` / `kiod_kmtpd`) → aktywne transfery, karty historii, `QFileSystemWatcher` | Błąd I/O, pozostawienie niekompletnych plików, zawieszenie GUI przy renderowaniu kart historii lub blokada odmontowania | Blokada `MTP Move` (`RefusedUnsupported`); brak synchronicznego `stat` podczas renderowania kart archiwalnych w `OperationWindow`; bezpieczne przekierowanie paneli do `thispc:/` po odmontowaniu | Brak otwartych luk |
| **TM-7** | **Użytkownik nieuprzywilejowany próbujący nadużyć ścieżek `admin://`** | Niezaufane środowisko użytkownika → `AdminRetryController` / `AdminRetryExecutor` → worker `kio-admin` (Polkit) | Nieautoryzowane nadpisanie lub usunięcie plików systemowych, eskalacja uprawnień, przejęcie własności pliku `root` | Centralna allowlista 4 operacji jednopierwiastkowych; blokada `Rename`/`Move`/`Delete`/symlinków/hardlinków w GUI; jednorazowy 60-sekundowy token zgody; brak automatycznego `unlink()` po błędzie (`AdminArtifactOwnershipAudit`); `0600` dla plików kopiowanych z `admin://` | Brak otwartych luk (potwierdzone w 5 zestawach `admin_retry*` pod ASan/UBSan) |
| **TM-8** | **Wiele jednoczesnych instancji ThisPC** | Równoległe procesy `thispc-view` tego samego użytkownika → wspólny plik `operations_history.json` oraz dzienniki `batch-rename-recovery` | Wyścig zapisu, utrata rekordów, wskrzeszenie wyczyszczonej historii, kolizja blokad odzyskiwania | `QLockFile` + atomowy `QSaveFile` + `historyEpoch` + CRDT merge + `QFileSystemWatcher` (150 ms debounce) w `TransferHistoryManager`; osobne katalogi `instance-<PID>` w `BatchRenameRecovery` | Brak otwartych luk wieloinstancyjnych |
| **TM-9** | **Skrypty instalacyjne i budujące (`install.sh`, `uninstall.sh`, `scripts/`)** | Katalog roboczy użytkownika (`build/`) → wykonanie `sudo` w `install.sh` / `uninstall.sh` | Usunięcie dowolnych plików systemowych jako `root` w razie podmiany `build/install_manifest.txt`, przypadkowe ubicie procesów przez `pkill -f` | `set -e` w skryptach; kompilacja wykonywana bez `sudo` | `SEC-045-07` (`LOW`): brak walidacji ścieżek z `build/install_manifest.txt` przed `sudo rm -f` w `uninstall.sh` |

---

## 4. Klasyfikacja znalezisk (Findings Classification)

Poniżej zestawiono wszystkie zidentyfikowane problemy zgodnie z 10-punktowym schematem klasyfikacji.

---

### `SEC-045-01` — Ryzyko `heap-use-after-free` w `OperationWindow::rebuild()` wskutek nieanulowanego timera `scheduleRebuild()` i synchronicznego `delete` widżetów

- **ID:** `SEC-045-01`
- **Poziom ryzyka:** **`MEDIUM`** (Bezpieczeństwo pamięci / Cykl życia widżetów Qt)
- **Lokalizacja w kodzie:**
  - `src/operationmanager.h`, klasa `OperationWindow`, metody `rebuild()` (linie `2352–2365`) oraz `scheduleRebuild()` (linie `2920–2937`),
  - `src/transferqueueui.h`, klasa `TransferQueueViewWidget`, metoda `rebuildTasks()` (linia `1913`),
  - `tests/transfer-queue.cpp`, przypadek testowy `S3-GEOM-07` / `S3-GEOM-08` (linie `2838–2855`).
- **Przyczyna techniczna:**
  1. `OperationWindow::scheduleRebuild()` (`src/operationmanager.h:2920-2937`) planuje odświeżenie za pomocą anonimowego wywołania `QTimer::singleShot(120, this, [this]() { m_rebuildScheduled = false; if (isVisible()) rebuild(); });`, używając wyłącznie flagi logicznej `bool m_rebuildScheduled`.
  2. Gdy `OperationWindow::rebuild()` zostaje wywołane bezpośrednio/synchronicznie (np. w `showOperationsTab()` w linii `2317`, `showQueueTab()` w linii `2328`, `showEvent()` w linii `2902` lub w kodzie testowym), wcześniej zaplanowany anonimowy timer 120 ms **nie jest zatrzymywany**, a flaga `m_rebuildScheduled` nie anuluje callbacku będącego już w kolejce timera Qt.
  3. Co więcej, `OperationWindow::rebuild()` (`src/operationmanager.h:2356-2359`) niszczy wszystkie istniejące widżety kart natychmiastowo przez `delete child->widget();` (podobnie `TransferQueueViewWidget::rebuildTasks()` w `src/transferqueueui.h:1913` wywołuje `delete it.value();`) zamiast bezpiecznego odłączenia od układu i `deleteLater()`.
- **Warunki wykorzystania:**
  - Zdarzenie modelu operacji (np. zakończenie zadania `finish()`, zmiana postępu lub przełączenie widoku) uzbraja 120-milisekundowy timer `scheduleRebuild()`, po czym przed upływem 120 ms następuje synchroniczne wywołanie `rebuild()` oraz przetwarzanie pętli zdarzeń (np. kliknięcie przycisku rozwijania szczegółów na karcie lub `QCoreApplication::processEvents()`), w trakcie którego wyzwala się zaległy timer i zwalnia pamięć widżetów kart.
- **Możliwe skutki:**
  - Odczyt ze zwolnionej pamięci sterty (`heap-use-after-free`) i awaria procesu (`SIGSEGV` / `SIGABRT`) podczas interakcji z oknem „Operacje plikowe”.
- **Dowód / scenariusz reprodukcji (potwierdzony przez AddressSanitizer):**
  - Uruchomienie zestawu `transfer_queue` zbudowanego z `-fsanitize=address,undefined`:
    ```text
    ==112770==ERROR: AddressSanitizer: heap-use-after-free on address 0x7bf8b2c90a10 at pc 0x558efeb45b25 bp 0x7ffde6ab3bd0 sp 0x7ffde6ab3bc8
    READ of size 8 at 0x7bf8b2c90a10 thread T0
        #0 0x558efeb45b24 in main /home/sebastianh/Pobrane/kio-thispc/tests/transfer-queue.cpp:2854
    0x7bf8b2c90a10 is located 0 bytes inside of 40-byte region [0x7bf8b2c90a10,0x7bf8b2c90a38)
    freed by thread T0 here:
        #0 0x7ff8b8f53a6d in operator delete(void*, unsigned long)
        #1 0x7ff8b801ae0d in QObjectPrivate::deleteChildren()
        #2 0x7ff8b91ba197 in QWidget::~QWidget()
        #3 0x558efec1a375 in DetailedOperationCard::~DetailedOperationCard() src/operationmanager.h:3954
        #4 0x558efec092ca in OperationWindow::rebuild() src/operationmanager.h:2357
        #5 0x558efec12db6 in OperationWindow::scheduleRebuild()::{lambda()#1}::operator()() const src/operationmanager.h:2926
    previously allocated by thread T0 here:
        #0 0x7ff8b8f528fd in operator new(unsigned long)
        #1 0x558efec20fc7 in DetailedOperationCard::DetailedOperationCard(...) src/operationmanager.h:4106
        #2 0x558efec09fa6 in OperationWindow::rebuild() src/operationmanager.h:2408
        #3 0x558efeb4577d in main /home/sebastianh/Pobrane/kio-thispc/tests/transfer-queue.cpp:2841
    ```
- **Proponowana poprawka (dla Stage 5.2):**
  1. Zastąpić anonimowy `QTimer::singleShot(120, ...)` w `OperationWindow` dedykowanym obiektem członkowskim `QTimer *m_rebuildTimer` (`setSingleShot(true)`).
  2. Na samym początku `OperationWindow::rebuild()` bezwarunkowo zatrzymywać timer (`if (m_rebuildTimer) m_rebuildTimer->stop(); m_rebuildScheduled = false;`), dzięki czemu każde synchroniczne wywołanie `rebuild()` unieważnia oczekujące odświeżenie z `scheduleRebuild()`.
  3. W `OperationWindow::rebuild()` (`src/operationmanager.h:2356-2359`) oraz `TransferQueueViewWidget::rebuildTasks()` (`src/transferqueueui.h:1913`) zastąpić bezpośrednie `delete widget;` przez `widget->hide(); widget->setParent(nullptr); widget->deleteLater();`, aby widżety nie były niszczone w trakcie trwania wywołania sygnału/zdarzenia na tym samym stosie.
- **Test regresji (dla Stage 5.2):**
  - Test weryfikujący pod kontrolą `ASan` + `UBSan`, że sekwencja `mgr.finish(...)` (uzbrajająca `scheduleRebuild()`) → synchroniczne `opWin.rebuild()` → odczekanie `> 150 ms` z `processEvents()` → kliknięcie `toggleBtn->click()` na karcie nie wyzwala drugiego, nadmiarowego `rebuild()` i nie powoduje `heap-use-after-free`.
- **Status:** `OPEN` (bloker dla Stage 5.2 przed wydaniem 0.45.0).

---

### `SEC-045-02` — Publikacja pliku bez flagi nadpisywania w `LocalFileCopyWorker` i `LocalFileMoveJob` używa `QFile::rename` (`rename(2)`) zamiast atomowego `RENAME_NOREPLACE`

- **ID:** `SEC-045-02`
- **Poziom ryzyka:** **`MEDIUM`** (Bezpieczeństwo systemu plików / okno wyścigu TOCTOU)
- **Lokalizacja w kodzie:**
  - `src/localfilecopyjob.h`, klasa `LocalFileCopyWorker`, metoda `run()` (sprawdzenie preflight w linii `152`, publikacja `QFile::rename(m_tempPath, m_destinationPath)` w linii `318`),
  - `src/localfilemovejob.h`, klasa `LocalFileMoveJob`, metoda `begin()` (sprawdzenie preflight w linii `140`, wykonanie `QFile::rename(m_sourcePath, m_destinationPath)` w linii `153`).
- **Przyczyna techniczna:**
  - Zarówno `LocalFileCopyWorker::run()`, jak i `LocalFileMoveJob::begin()` sprawdzają na początku operacji `!QFileInfo::exists(m_destinationPath)`, gdy `m_overwrite == false`. Jednak właściwa publikacja pliku tymczasowego `.thispc-part` do docelowej nazwy (`src/localfilecopyjob.h:318`) oraz szybkie przeniesienie w obrębie tego samego systemu plików (`src/localfilemovejob.h:153`) używają `QFile::rename(src, dst)`.
  - Na systemie Linux `QFile::rename` wywołuje standardowe wywołanie systemowe POSIX `rename(2)`, które **atomowo nadpisuje** istniejący plik docelowy lub dowiązanie symboliczne, jeśli pojawiły się one pod ścieżką `m_destinationPath` po teście początkowym.
  - W innych modułach ThisPC (`KioStagedDetail::atomicPublishNewFile` w `src/kiostagedtransferjob.h:253`, `LocalTransferJob::transferLink` w `src/localtransferjob.h:314`, `ArchiveCreationWorker` w `src/archive-creation.h:395` oraz `BatchRename` w `src/batchrenamerecovery.h:516`) zastosowano już właściwe wywołanie `syscall(SYS_renameat2, AT_FDCWD, src, AT_FDCWD, dst, RENAME_NOREPLACE)`.
- **Warunki wykorzystania:**
  - Podczas kopiowania dużego pliku z `m_overwrite == false` (np. wielogigabajtowego obrazu lub po wznowieniu z pauzy) inny proces lub użytkownik tworzy plik pod docelową ścieżką `m_destinationPath` zanim kopiowanie do `.thispc-part` dobiegnie końca.
- **Możliwe skutki:**
  - Ciche nadpisanie pliku utworzonego w międzyczasie pod ścieżką docelową pomimo ustawionej flagi `m_overwrite == false`.
- **Dowód / scenariusz reprodukcji:**
  1. Rozpocząć kopiowanie pliku `A` do `B` (`overwrite = false`) i wstrzymać transfer (`pause()`), gdy plik `.thispc-part` istnieje na dysku.
  2. W trakcie pauzy utworzyć na dysku plik `B` z inną zawartością.
  3. Wznowić transfer (`resume()`). Po dokończeniu kopiowania `QFile::rename(m_tempPath, m_destinationPath)` w `src/localfilecopyjob.h:318` nadpisze plik `B` bez zgłoszenia kolizji `EEXIST`.
- **Proponowana poprawka (dla Stage 5.2):**
  - W `LocalFileCopyWorker::run()` (`src/localfilecopyjob.h:318`) oraz `LocalFileMoveJob::begin()` (`src/localfilemovejob.h:153`), gdy `m_overwrite == false`, użyć `syscall(SYS_renameat2, AT_FDCWD, srcBytes.constData(), AT_FDCWD, dstBytes.constData(), RENAME_NOREPLACE)` (z fallbackiem do `::lstat` wyłącznie dla jąder/systemów plików zwracających `EINVAL`/`ENOSYS` dla `RENAME_NOREPLACE`, analogicznie do `KioStagedDetail::atomicPublishNewFile` w `src/kiostagedtransferjob.h:253-270`). W przypadku `EEXIST` usunąć plik `.thispc-part` (dla Copy) lub zachować źródło (dla Move) i zakończyć zadanie błędem kolizji.
- **Test regresji (dla Stage 5.2):**
  - Test jednostkowy w `local_transfer` / `local_move`: wstrzymanie kopiowania `LocalFileCopyJob` (`overwrite = false`), utworzenie pliku docelowego w trakcie pauzy, wznowienie kopiowania i weryfikacja, że istniejący plik docelowy pozostaje nienaruszony, a zadanie zgłasza błąd kolizji i sprząta `.thispc-part`.
- **Status:** `OPEN` (bloker dla Stage 5.2 przed wydaniem 0.45.0).

---

### `SEC-045-03` — Użycie pola `descriptor.priority` po przeniesieniu obiektu (`use-after-move`) w `TransferQueueController::enqueue()`

- **ID:** `SEC-045-03`
- **Poziom ryzyka:** **`LOW`** (Jakość kodu / bezpieczeństwo semantyki przenoszenia C++)
- **Lokalizacja w kodzie:**
  - `src/transferqueuecontroller.h`, klasa `TransferQueueController`, metoda `enqueue(TransferTaskDescriptor descriptor)` (linie `176` oraz `188`).
- **Przyczyna techniczna:**
  - W linii `176` wykonywane jest przeniesienie `task->desc = std::move(descriptor);`, natomiast w linii `188` log diagnostyczny odczytuje `static_cast<int>(descriptor.priority)` z obiektu po przeniesieniu zamiast `static_cast<int>(task->desc.priority)`.
- **Warunki wykorzystania:**
  - Każde wywołanie `TransferQueueController::enqueue()` z włączonym logowaniem kategorii `thispc.transferqueue`. Obecnie pole `priority` jest trywialnym typem wyliczeniowym (`enum class TransferPriority`), więc odczyt nie powoduje awarii, ale narusza regułę `bugprone-use-after-move`.
- **Możliwe skutki:**
  - Ryzyko niezdefiniowanego stanu w przypadku rozbudowy struktury `TransferTaskDescriptor` o niestandardowy konstruktor przenoszący.
- **Dowód / scenariusz reprodukcji:**
  - Wykryte przez `clang-tidy 23.1.4`:
    `src/transferqueuecontroller.h:188:84: warning: 'descriptor' used after it was moved [bugprone-use-after-move]`.
- **Proponowana poprawka (dla Stage 5.2):**
  - Zamienić `static_cast<int>(descriptor.priority)` na `static_cast<int>(task->desc.priority)` w `src/transferqueuecontroller.h:188`.
- **Test regresji (dla Stage 5.2):**
  - Weryfikacja `clang-tidy` (`0` ostrzeżeń `bugprone-use-after-move`) oraz istniejące testy `transfer_queue`.
- **Status:** `OPEN`.

---

### `SEC-045-04` — Nieosłonięte dyrektywą `#ifdef THISPC_TEST_HARNESS` sprawdzenia zmiennej środowiskowej w binarium produkcyjnym

- **ID:** `SEC-045-04`
- **Poziom ryzyka:** **`LOW`** (Defense-in-Depth / Izolacja haków testowych od binarium produkcyjnego)
- **Lokalizacja w kodzie:**
  - `src/transferqueueui.h`, funkcja `TransferQueueSettingsWidget::promptClearHistoryConfirmation()` (linia `547`),
  - `src/duplicateactioncontroller.h`, funkcja `DuplicateActionController::showMessage()` (linia `229`).
- **Przyczyna techniczna:**
  - Podczas gdy wszystkie pozostałe punkty wstrzykiwania testowego w projekcie są kompilowane wyłącznie w binariach testowych za pomocą `#ifdef THISPC_TEST_HARNESS` lub `#ifdef THISPC_BATCH_RENAME_TEST_HOOKS`, w dwóch powyższych miejscach wywołanie `qEnvironmentVariableIsSet("THISPC_TEST_HARNESS")` znajduje się w bezwarunkowym kodzie produkcyjnym.
- **Warunki wykorzystania:**
  - Uruchomienie produkcyjnego `thispc-view` w środowisku, w którym ustawiono zmienną `THISPC_TEST_HARNESS=1`, i kliknięcie „Wyczyść historię…” w ustawieniach kolejki transferów (pomija okno potwierdzenia `QMessageBox` i od razu czyści historię).
- **Możliwe skutki:**
  - Pominięcie okna potwierdzenia czyszczenia historii operacji oraz wyciszenie okien komunikatów `DuplicateActionController`, jeśli zmienna środowiskowa `THISPC_TEST_HARNESS` wycieknie do sesji użytkownika.
- **Dowód / scenariusz reprodukcji:**
  - Inspekcja `src/transferqueueui.h:546-550` oraz `src/duplicateactioncontroller.h:228-232`.
- **Proponowana poprawka (dla Stage 5.2):**
  - Osłonić oba bloki `if (qEnvironmentVariableIsSet("THISPC_TEST_HARNESS"))` dyrektywą `#ifdef THISPC_TEST_HARNESS ... #endif`.
- **Test regresji (dla Stage 5.2):**
  - Sprawdzenie statyczne (`grep`), że każde użycie `THISPC_TEST_HARNESS` w `src/` znajduje się wewnątrz bloku preprocesora `#ifdef THISPC_TEST_HARNESS`.
- **Status:** `OPEN`.

---

### `SEC-045-05` — Brak separatora końca opcji `--` w pomocniczych wywołaniach narzędzi zewnętrznych (`dolphin`, `okular`, `plasma-apply-wallpaperimage`)

- **ID:** `SEC-045-05`
- **Poziom ryzyka:** **`LOW`** (Utwardzenie wywołań procesów zewnętrznych)
- **Lokalizacja w kodzie:**
  - `src/thispcview.cpp`, funkcja `openInDolphin(const QUrl &url)` (linia `298`),
  - `src/appwidgets.cpp`, funkcja `openInDolphin(const QUrl &url)` (linia `44`),
  - `src/selectionmenucontroller.cpp`, akcja `Print` (`okular`, linia `309`) oraz `Set as Wallpaper` (`plasma-apply-wallpaperimage`, linia `321`).
- **Przyczyna techniczna:**
  - Wywołania używają bezpiecznego `QProcess::startDetached(program, args)` (bez powłoki), a dla plików lokalnych `url.toLocalFile()` zwraca ścieżkę bezwzględną zaczynającą się od `/`, co chroni przed wstrzyknięciem opcji zaczynającej się od `-`. Jednak dla adresów nielokalnych `openInDolphin` przekazuje `url.toString()` bez separatora `--` i bez walidacji schematu URL.
- **Warunki wykorzystania:**
  - Wywołanie `openInDolphin` z nietypowym adresem URL lub uruchomienie narzędzia pomocniczego na ścieżce niepoprzedzonej separatorem `--`.
- **Możliwe skutki:**
  - Potencjalna interpretacja nietypowego argumentu jako przełącznika CLI przez program zewnętrzny.
- **Dowód / scenariusz reprodukcji:**
  - Inspekcja `src/thispcview.cpp:295-300`, `src/appwidgets.cpp:40-46`, `src/selectionmenucontroller.cpp:309, 321`.
- **Proponowana poprawka (dla Stage 5.2):**
  - W `openInDolphin` przekazywać `"--"` przed adresem docelowym oraz odrzucać puste/nieprawidłowe URL; w `selectionmenucontroller.cpp` upewnić się, że przekazywana ścieżka lokalna jest bezwzględna (`QDir::isAbsolutePath(localFile)`).
- **Test regresji (dla Stage 5.2):**
  - Test jednostkowy weryfikujący walidację argumentów pomocniczych wywołań zewnętrznych.
- **Status:** `OPEN`.

---

### `SEC-045-06` — Brak wewnętrznej weryfikacji `lstat` (`!S_ISLNK`) w `AclController::write()` oraz brak sprawdzenia `BatchRenameRecoveryGate` przed zapisem ACL w `PropertiesWindow`

- **ID:** `SEC-045-06`
- **Poziom ryzyka:** **`LOW`** (Utwardzenie zapisu ACL / Defense-in-Depth)
- **Lokalizacja w kodzie:**
  - `src/aclcontroller.h`, klasa `AclController`, metoda `write(const QString &path)` (linie `33–60`),
  - `src/propertiesdialog.h`, klasa `PropertiesWindow` (zapis ACL w linii `1235` vs. sprawdzenie `BatchRenameRecoveryGate::instance().mutationsBlocked()` dla `chmod` w linii `1274` i `rename` w linii `1369`).
- **Przyczyna techniczna:**
  1. `PropertiesWindow` (`src/propertiesdialog.h:1191-1218`) odświeża `st->capabilities` (`lstat` + weryfikacja `st_dev` i `st_ino`) przed zastosowaniem zmian i wyłącza edytor ACL dla dowiązań symbolicznych. Jednak sama metoda `AclController::write(const QString &path)` (`src/aclcontroller.h:44`) wywołuje ścieżkowe `acl_set_file(encodedPath, ACL_TYPE_ACCESS, accessAcl)` (które w libacl podąża za dowiązaniami symbolicznymi), nie wykonując własnego wewnętrznego sprawdzenia `::lstat` (`!S_ISLNK`) ani `acl_set_fd` na deskryptorze `O_NOFOLLOW`.
  2. Dodatkowo blok zapisu ACL w `src/propertiesdialog.h:1235` (`if (aclEditor->isDirty())`) jest wykonywany przed sprawdzeniem `BatchRenameRecoveryGate::instance().mutationsBlocked()`, podczas gdy zmiana uprawnień POSIX (linia `1274`) i zmiana nazwy (linia `1369`) sprawdzają tę bramkę.
- **Warunki wykorzystania:**
  - Wywołanie `AclController::write()` bezpośrednio z pominięciem GUI lub próba zapisu zmienionego ACL w oknie Właściwości w momencie, gdy aktywna jest blokada `BatchRenameRecoveryGate`.
- **Możliwe skutki:**
  - Modyfikacja ACL mimo aktywnej blokady odzyskiwania Batch Rename lub podążenie za symlinkiem w razie podmiany ścieżki tuż przed `acl_set_file`.
- **Dowód / scenariusz reprodukcji:**
  - Inspekcja `src/aclcontroller.h:33-60` oraz `src/propertiesdialog.h:1235-1280`.
- **Proponowana poprawka (dla Stage 5.2):**
  1. W `src/propertiesdialog.h:1235` dodać sprawdzenie `BatchRenameRecoveryGate::instance().mutationsBlocked()` przed `aclEditor->write()`.
  2. W `AclController::write()` (`src/aclcontroller.h:33-60`) dodać weryfikację `::lstat` odrzucającą dowiązania symboliczne (`S_ISLNK(st.st_mode)`), a dla zwykłych plików bez `default ACL` używać deskryptora otwartego z `O_RDONLY | O_NOFOLLOW | O_CLOEXEC` oraz `acl_set_fd()`.
- **Test regresji (dla Stage 5.2):**
  - Test w zestawie `acl`: wywołanie `AclController::write()` na dowiązaniu symbolicznym musi zostać odrzucone bez modyfikacji celu dowiązania.
- **Status:** `OPEN`.

---

### `SEC-045-07` — Brak walidacji ścieżek z `build/install_manifest.txt` przed `sudo rm -f` w `uninstall.sh` oraz szerokie wzorce `pkill -f`

- **ID:** `SEC-045-07`
- **Poziom ryzyka:** **`LOW`** (Utwardzenie skryptów instalacyjnych)
- **Lokalizacja w kodzie:**
  - `uninstall.sh`, linie `9–10` oraz `20–22`,
  - `install.sh`, linie `29–30`.
- **Przyczyna techniczna:**
  1. `uninstall.sh` (linie `20–22`) odczytuje plik `build/install_manifest.txt` z katalogu roboczego użytkownika i wykonuje `sudo rm -f -- "$file"` dla każdego wiersza bez sprawdzenia, czy ścieżka jest bezwzględna, wolna od `..` i położona w dozwolonym prefiksie instalacyjnym (`/usr/`, `/etc/`).
  2. `install.sh` i `uninstall.sh` wywołują `pkill -f '/kf6/kio/thispc'` oraz `pkill -f 'kio_thispc'`, co dopasowuje dowolny proces użytkownika zawierający ten ciąg w argumentach wiersza poleceń (np. edytor otwierający plik o tej nazwie).
- **Warunki wykorzystania:**
  - Zmodyfikowanie pliku `build/install_manifest.txt` przez lokalny nieuprzywilejowany proces przed ręcznym uruchomieniem `./uninstall.sh` przez użytkownika i zatwierdzeniem hasła `sudo`.
- **Możliwe skutki:**
  - Usunięcie plików spoza drzewa instalacyjnego ThisPC z uprawnieniami `root` podczas deinstalacji.
- **Dowód / scenariusz reprodukcji:**
  - Inspekcja `uninstall.sh:17-24`.
- **Proponowana poprawka (dla Stage 5.2):**
  - W `uninstall.sh` przed wywołaniem `sudo rm -f -- "$file"` walidować, że ścieżka nie zawiera `..` i zaczyna się od dozwolonego prefiksu (`/usr/lib/`, `/usr/local/`, `/usr/bin/`, `/usr/share/`, `/etc/xdg/`), a w `install.sh`/`uninstall.sh` zawęzić dopasowanie procesów workera KIO.
- **Test regresji (dla Stage 5.2):**
  - Test skryptowy weryfikujący, że `uninstall.sh` odrzuca niebezpieczne wpisy manifestu (np. `/etc/passwd` lub `/usr/../etc/shadow`).
- **Status:** `OPEN`.

---

### `SEC-045-08` — Higiena analizy statycznej i wielowątkowości: sprawdzenie `m_details` w `SplitBrowserPane::activateDetailsItem()`, buforowanie `QPointer` w `DuplicateFinderJob::cancelInternal()` oraz wątkowo bezpieczne komunikaty `errno`

- **ID:** `SEC-045-08`
- **Poziom ryzyka:** **`LOW`** (Higiena analizy statycznej / współbieżności)
- **Lokalizacja w kodzie:**
  - `src/splitbrowserpane.h`, metoda `SplitBrowserPane::activateDetailsItem()` (linia `1830`),
  - `src/duplicatefinderjob.h`, metoda `DuplicateFinderJob::cancelInternal()` (linie `142–147`),
  - `src/checksumjob.h` (linia `187`), `src/batchrename.h` (linie `516, 575, 666`), `src/batchrenamerecovery.h` (linie `519, 580`).
- **Przyczyna techniczna:**
  1. `SplitBrowserPane::activateDetailsItem()` (`src/splitbrowserpane.h:1830`) wywołuje `m_details->cancelEditingForActivation()` bez sprawdzenia `if (m_details)`, podczas gdy metoda wywołująca `activateCurrentItem()` (linia `765`) sprawdza `m_details ? ... : nullptr`. Wykryte przez analizator statyczny Clang (`[clang-analyzer-core.CallAndMessage]`).
  2. `DuplicateFinderJob::cancelInternal()` (`src/duplicatefinderjob.h:142-147`) trzykrotnie odwołuje się do `QPointer<ChecksumJob> m_activeJob` (`disconnect`, `cancel`, `deleteLater`), co powoduje wygenerowanie przez optymalizator GCC 16.2.1 przy `-O2` ostrzeżenia `-Wstringop-overflow=` na teoretycznej ścieżce wyzerowania wskaźnika między wywołaniami.
  3. Wątki robocze w `checksumjob.h`, `batchrename.h` i `batchrenamerecovery.h` wywołują `std::strerror(errno)` (oznaczone przez `clang-tidy` jako `[concurrency-mt-unsafe]`) zamiast wątkowo bezpiecznego `qt_error_string(errno)`.
- **Warunki wykorzystania:**
  - Wywołanie `activateDetailsItem()` przed inicjalizacją `m_details` lub jednoczesne formatowanie błędów `errno` w wielu wątkach roboczych.
- **Możliwe skutki:**
  - Ostrzeżenia kompilatora/analizatora statycznego oraz potencjalny wyścig na statycznym buforze `strerror` przy nietypowej lokalizacji C.
- **Dowód / scenariusz reprodukcji:**
  - Raport `clang-tidy 23.1.4` oraz kompilacja z `-Wall -Wextra -O2` na GCC 16.2.1.
- **Proponowana poprawka (dla Stage 5.2):**
  1. Dodać `if (!m_details) return;` na początku `SplitBrowserPane::activateDetailsItem()` (`src/splitbrowserpane.h:1830`).
  2. W `DuplicateFinderJob::cancelInternal()` (`src/duplicatefinderjob.h:142-147`) przenieść `ChecksumJob *job = m_activeJob.data(); m_activeJob.clear();` do zmiennej lokalnej przed wywołaniem `disconnect`/`cancel`/`deleteLater`.
  3. Zastąpić `QString::fromLocal8Bit(std::strerror(err))` w wątkach roboczych wywołaniem `qt_error_string(err)`.
- **Test regresji (dla Stage 5.2):**
  - Czysty przebieg kompilacji z `-Wall -Wextra -Wformat=2 -Wundef -O2` (0 ostrzeżeń) oraz `clang-tidy` (0 ostrzeżeń `clang-analyzer-*` i `concurrency-mt-unsafe`).
- **Status:** `OPEN`.

---

## 5. Luki w pokryciu testowym i plan naprawczy Stage 5.2

### Zidentyfikowane luki w testach bezpieczeństwa (do uzupełnienia w Stage 5.2)
1. **Brak testu `ASan` dla przeplotu `scheduleRebuild()` i synchronicznego `rebuild()` z opóźnieniem `> 120 ms`:** Zestaw `transfer_queue` uruchamiany bez `ASan` przechodził pomyślnie, ponieważ zwolniony blok `QToolButton` na stercie nie zdążył zostać nadpisany przed `toggleBtn->click()`. Dopiero uruchomienie `transfer_queue` z `ASan` ujawniło `SEC-045-01`.
2. **Brak testu wyścigu TOCTOU przy publikacji `.thispc-part` w `LocalFileCopyJob` i `LocalFileMoveJob` (`m_overwrite == false`):** Istniejące testy `local_transfer` i `local_move` sprawdzały kolizję istniejącą *przed* startem zadania (`preflight`), ale nie testowały pojawienia się pliku docelowego *w trakcie* trwania kopiowania (`SEC-045-02`).
3. **Brak testu odrzucenia dowiązania symbolicznego bezpośrednio w `AclController::write()` (`SEC-045-06`).**

### Plan działań dla Stage 5.2 (Security Remediation & Hardening)
1. **Krok 1 (`SEC-045-01` — `MEDIUM`):** Naprawić cykl życia przebudowy `OperationWindow::rebuild()` / `scheduleRebuild()` w `src/operationmanager.h` oraz `TransferQueueViewWidget::rebuildTasks()` w `src/transferqueueui.h` (zatrzymywanie dedykowanego `m_rebuildTimer` i użycie `deleteLater()`), a następnie potwierdzić `0` błędów `ASan` + `UBSan` w całym zestawie `transfer_queue`.
2. **Krok 2 (`SEC-045-02` — `MEDIUM`):** Wdrożyć atomową publikację `SYS_renameat2(..., RENAME_NOREPLACE)` dla `m_overwrite == false` w `LocalFileCopyWorker::run()` (`src/localfilecopyjob.h`) oraz `LocalFileMoveJob::begin()` (`src/localfilemovejob.h`) i dodać test regresji TOCTOU w trakcie pauzy transferu.
3. **Krok 3 (`SEC-045-03` .. `SEC-045-08` — `LOW`):** Wdrożyć 6 poprawek utwardzających (`task->desc.priority`, `#ifdef THISPC_TEST_HARNESS`, `--` w wywołaniach pomocniczych, `lstat` + `BatchRenameRecoveryGate` dla ACL, walidacja ścieżek manifestu w `uninstall.sh`, poprawki `m_details` / `QPointer` / `qt_error_string`).
4. **Krok 4 (Weryfikacja Stage 5.2):** Ponownie uruchomić kompilację `-Wall -Wextra -O2`, `clang-tidy`, pełny przebieg `ASan` + `UBSan` (w tym `transfer_queue`, `history`, `operations`, `local_transfer`, `local_move`, `admin_retry*`, `acl`) oraz pełną regresję `77/77`, a następnie zaktualizować status znalezisk w `docs/SECURITY_AUDIT_045.md` na `RESOLVED / VERIFIED`.

---

## 6. Werdykt Security Release Gate (Stage 5.1)

- **Werdykt:** **`BLOCKED` (Wydanie 0.45.0 wstrzymane do zakończenia Stage 5.2)**
- **Uzasadnienie:**
  - Audyt wykluczył obecność złośliwego kodu, backdoorów, ukrytych połączeń sieciowych oraz luk `CRITICAL` i `HIGH`.
  - Architektura nowych funkcji wydania 0.45.0 (Stage 1: Powiadomienia KDE, Stage 2: Trwała historia `operations_history.json`, Stage 3: `Safe Retry`, Stage 4: wewnętrzne „Pokaż w folderze”) spełnia rygorystyczne założenia bezpieczeństwa (*fail-closed*, brak podążania za symlinkami, ochrona epok `historyEpoch`, brak uruchamiania zewnętrznych procesów).
  - Równocześnie analiza dynamiczna `ASan` oraz przegląd wyścigów systemowych wykazały dwa znaleziska o poziomie **`MEDIUM`** (`SEC-045-01`: `heap-use-after-free` w `OperationWindow::rebuild()` przy nałożeniu się timera `scheduleRebuild()` oraz `SEC-045-02`: brak `RENAME_NOREPLACE` w końcowym kroku `LocalFileCopyWorker` i `LocalFileMoveJob` dla `m_overwrite == false`) oraz 6 drobnych usprawnień **`LOW`** (`SEC-045-03`..`SEC-045-08`).
  - Zgodnie z zasadą jakości projektu znaleziska `SEC-045-01`..`SEC-045-08` zostaną naprawione i zweryfikowane testami regresyjnymi w **Stage 5.2** przed przejściem do **Stage 5.3 (Release 0.45.0)**.

---

## 7. Stage 5.2 — Remediation Verification

W ramach **Stage 5.2 (Security Remediation & Hardening)** wszystkie 8 zidentyfikowanych problemów bezpieczeństwa (`SEC-045-01`..`SEC-045-08`) zostało naprawionych w kodzie źródłowym i zweryfikowanych dedykowanymi testami automatycznymi oraz analizatorami dynamicznymi i statycznymi.

### Podsumowanie statusu podatności po wdrożeniu poprawek

| Identyfikator | Poziom | Opis problemu | Wdrożona poprawka (Stage 5.2) | Wynik weryfikacji | Status |
|---|---|---|---|---|---|
| **SEC-045-01** | `MEDIUM` | Heap-use-after-free w `OperationWindow` i `TransferQueueViewWidget` | Dodano dedykowany `m_rebuildTimer`, licznik generacji `m_rebuildGeneration`, bezpieczne odpinanie widżetów (`setParent(nullptr)`, `hide()`, `setObjectName(QString())`) oraz odroczone niszczenie `deleteLater()` z ewidencją w `m_retiredWidgets` / `m_retiredItemWidgets` i zatrzymaniem timera przy `rebuild()`, `hideEvent()`, `closeEvent()`. | **PASS** (`ASan + UBSan`: 0 błędów w `transfer_queue` i `operations`). Dedykowany test przeplotu z timerem 150 ms w `tests/transfer-queue.cpp`. | **RESOLVED / VERIFIED** |
| **SEC-045-02** | `MEDIUM` | TOCTOU i brak atomowej ochrony przed nadpisaniem (`overwrite=false`) | Nowa funkcja `publishLocalFileNoOverwrite()` w `src/localfileoverwrite.h` korzystająca wyłącznie z `SYS_renameat2(..., RENAME_NOREPLACE)` oraz atomowego POSIX `link(2)`/`unlink(2)` (bez fallbacku do nadpisującego `rename(2)`). Wdrożona w `LocalFileCopyWorker`, `LocalFileMoveJob` i `kiostagedtransferjob.h`. | **PASS** (Dedykowane testy wyścigu w locie w `tests/local-file-copy-job.cpp` i `tests/local-file-move-job.cpp`: SHA-256 celu i źródła zachowane w 100%, błąd `DestinationExists` / `EEXIST`, `.thispc-part` usunięty). | **RESOLVED / VERIFIED** |
| **SEC-045-03** | `LOW` | Use-after-move deskryptora w `TransferQueueController::enqueue` | Zastąpiono czytanie z przeniesionego obiektu odwołaniem do `task->desc.priority`. | **PASS** (Test w `tests/transfer-queue.cpp` weryfikujący ścisłe sortowanie `High -> Normal -> Low` przy kolejkowaniu r-value). | **RESOLVED / VERIFIED** |
| **SEC-045-04** | `LOW` | Niezabezpieczone produkcyjne ścieżki `THISPC_TEST_HARNESS` | Wszystkie runtime'owe sprawdzenia zmiennej otoczenia otoczono `#ifdef THISPC_TEST_HARNESS` w `src/transferqueueui.h` i `src/duplicateactioncontroller.h`. W binariach produkcyjnych zmienna nie jest parsowana. | **PASS** (Weryfikacja `strings build/bin/thispc-view`: 0 wystąpień; test `tests/test_safe_uninstall.fish`). | **RESOLVED / VERIFIED** |
| **SEC-045-05** | `LOW` | Obsługa argumentów procesów zewnętrznych | Dodano separator opcji `--` oraz wymuszenie ścieżek bezwzględnych (`/`) dla `dolphin` i `okular` w `src/thispcview.cpp`, `src/appwidgets.cpp`, `src/selectionmenucontroller.cpp`. | **PASS** (Kompilacja i testy `selection_menu` / `panes`). | **RESOLVED / VERIFIED** |
| **SEC-045-06** | `LOW` | Zabezpieczenie zapisu ACL przed symlinkami i blokada w recovery | W `AclController::write()` dodano `lstat` odrzucający symlinki, sprawdzanie typu pliku/katalogu, weryfikację tożsamości deskryptora `O_NOFOLLOW` i użycie `acl_set_fd()`. W `src/propertiesdialog.h` dodano sprawdzenie `BatchRenameRecoveryGate::mutationsBlocked()`. | **PASS** (Dedykowany test odrzucenia symlinku w `tests/acl-controller.cpp`). | **RESOLVED / VERIFIED** |
| **SEC-045-07** | `LOW` | Skrypt deinstalatora i walidacja manifestu | Przepisano `uninstall.sh`: rygorystyczna biała lista ścieżek (`/usr/bin/thispc-view`, `.so`, `.desktop`, `.notifyrc`, ikony `kio-thispc-*`), zakaz ścieżek względnych, `..`, znaków kontrolnych, symlinków, wsparcie izolowanego roota testowego (`THISPC_UNINSTALL_ROOT`). Precyzyjny `pkill` w `install.sh`. | **PASS** (10/10 asercji izolowanego testu `tests/test_safe_uninstall.fish`). | **RESOLVED / VERIFIED** |
| **SEC-045-08** | `LOW` | Null-check w `m_details`, race w `m_activeJob`, wielowątkowe komunikaty błędów | Dodano null-check `if (m_details)` w `src/splitbrowserpane.h:1830`, lokalną kopię wskaźnika w `DuplicateFinderJob::cancelInternal()`, zastąpiono `strerror(errno)` przez bezpieczny wątkowo `qt_error_string(errno)` we wszystkich wątkach roboczych. | **PASS** (Kompilacja ścisła GCC 16.2.1, `clang-tidy`, testy `duplicate_finder`, `checksums`, `batch_rename`). | **RESOLVED / VERIFIED** |

### Podsumowanie wyników automatycznych testów regresji i analizy bezpieczeństwa
- **Kompilacja produkcyjna (`./scripts/build.sh`):** PASS (0 błędów).
- **Kompilacja ze ścisłymi flagami GCC 16.2.1 (`-Wall -Wextra -Wformat=2 -Wundef -O2`):** PASS (0 błędów).
- **Analiza statyczna `clang-tidy`:** PASS (0 błędów w zmodyfikowanym kodzie).
- **Dynamiczna analiza sanitizera (`AddressSanitizer + UndefinedBehaviorSanitizer`):** 19/19 zestawów testowych **PASS** z 0 naruszeniami pamięci / UB.
- **Pełna regresja (`python3 tests/run-pane-actions.py --all`):** **77/77 zestawów PASS**.
- **Izolowany test bezpiecznego odinstalowywania (`tests/test_safe_uninstall.fish`):** 12/12 asercji PASS.
- **Weryfikacja formatowania (`git diff --check`):** PASS (brak trailing whitespace).

### Status Security Release Gate po remediacji Stage 5.2
- **Stan automatyczny:** **PASS (Wszystkie 8 zgłoszonych podatności naprawione i zweryfikowane automatycznie).**
- **Stan bramki wydania:** **`BLOCKED` (Formalnie zablokowana do czasu ukończenia procedury odbioru manualnego w środowisku użytkownika).**
