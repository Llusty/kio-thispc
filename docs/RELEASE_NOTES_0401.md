# ThisPC 0.40.1 — Multi-Selection Properties & Bulk Permissions

Source/runtime version: 0.40.1.

## English

- **Multi-Selection Properties dialog:** Opening Properties (`Alt+Enter` or context menu) with multiple files/folders selected opens an aggregate, modeless Properties window across Primary View, Split View, and Search.
- **Aggregate General tab:** Non-blocking background worker computes cumulative file, folder, and symbolic link counts, total logical size, and filesystem disk allocation (`st_blocks`) with hardlink deduplication. Includes progressive updates, in-flight cancellation, and scan limit guards.
- **Bulk POSIX Permissions:** Dedicated permissions grid allows selective modification of individual rwx bits across all directly selected entries without recursion. Untouched rwx bits and special bits (`setuid`, `setgid`, `sticky`) are preserved. Group permission adjustments properly update the kernel POSIX ACL mask.
- **Strict safety policy:** Supported only on local regular files and directories directly owned by the user on supported Linux filesystems (ext2/3/4, btrfs, xfs, tmpfs). Mixed selections containing symbolic links (live or broken), remote KIO URLs, or unsupported filesystems are strictly read-only.
- **File & mount descriptor validation:** Pinning with `O_PATH | O_NOFOLLOW` prevents symlink redirection, file substitution, moves, or mount loss races both before and after mutation.
- **Explicit confirmation:** Mutations require explicit interactive confirmation defaulting to "No". Cooperative worker cancellation prevents stale writes upon window closure.

## Polski

- **Zbiorcze okno Właściwości:** Wywołanie Właściwości (`Alt+Enter` lub menu kontekstowe) przy zaznaczeniu wielu plików/folderów otwiera zbiorcze, niemodalne okno Właściwości w panelu głównym, Split View i wynikach wyszukiwania.
- **Zakładka Ogólne:** Asynchroniczny worker w tle oblicza sumaryczną liczbę plików, folderów i dowiązań symbolicznych, łączny rozmiar logiczny oraz fizyczną alokację na dysku (`st_blocks`) z deduplikacją hardlinków. Zawiera progresywny licznik, bezpieczne anulowanie i ochronę przed przepełnieniem skanowania.
- **Zbiorcze uprawnienia POSIX:** Dedykowana siatka uprawnień pozwala na selektywną zmianę poszczególnych bitów rwx dla bezpośrednio zaznaczonych pozycji bez rekurencji. Niezmieniane bity rwx oraz bity specjalne (`setuid`, `setgid`, `sticky`) są zachowywane. Zmiana uprawnień grupy prawidłowo aktualizuje maskę ACL kernela.
- **Restrykcyjna polityka bezpieczeństwa:** Obsługa ograniczona do bezpośrednich plików zwykłych i folderów należących do użytkownika na wspieranych systemach plików (ext2/3/4, btrfs, xfs, tmpfs). Mieszane zaznaczenia zawierające symlinki (działające lub zerwane), zdalne URL KIO lub niewspierane systemy plików są w całości tylko do odczytu.
- **Weryfikacja deskryptorowa:** Zabezpieczenie `O_PATH | O_NOFOLLOW` zapobiega przekierowaniom przez symlinki, podmianom plików, przeniesieniu lub utracie montowania przed i po zapisie.
- **Jawne potwierdzenie:** Zmiany wymagają jawnego potwierdzenia z domyślnym wyborem „Nie”. Kooperacyjne anulowanie zadań zapobiega nieaktualnym zapisom po zamknięciu okna.
