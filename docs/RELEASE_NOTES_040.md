# ThisPC 0.40.0 — Advanced Properties Follow-up

Source/runtime version: 0.40.0. Publication status is verified separately on GitHub.

## English

- Properties now detects backend capabilities and retains a no-follow identity snapshot, rejecting writes after an item disappears, moves, is replaced, or loses its mount.
- Extended Attributes shows bounded text/binary previews and supports immediate local user.* add/edit/remove through verified file descriptors. These operations are outside Apply/Cancel/Undo. security.*, trusted.* and system.* remain protected.
- Numeric POSIX permissions (0000–0777) synchronize with permission checkboxes, preserve existing special bits, and refresh ACL state using normal kernel mask semantics.
- Hidden presentation distinguishes KIO visibility, Unix dot-name semantics, and unverified native flags. Safe Unix dot-name changes use the shared rename path; NTFS native Hidden is not emulated or written.
- Primary, Split and Search share Properties behavior, refresh, target-loss guards and safe close sequencing. ACL remains unavailable for links after rename, including broken links.
- Manual Stage 1–6 acceptance is prior user-confirmed evidence. Live ext4 and remote KIO remain NOT TESTED; deterministic policies are covered automatically. Final release verification is recorded in PROPERTIES_RELEASE_040.md; the pre-release audit remains historical.

Excluded: SELinux labels, chattr/immutable, Linux file capabilities, larger ACL redesign, admin:// retry.

## Polski

- Właściwości rozpoznają capabilities backendu i zachowują tożsamość elementu bez podążania za symlinkiem. Zniknięcie, przeniesienie, podmiana lub utrata mountu blokują zapis ze starego dialogu.
- Atrybuty rozszerzone mają ograniczone podglądy tekstu i danych binarnych. Lokalne user.* można dodawać, edytować i usuwać przez zweryfikowany deskryptor. Zapis jest natychmiastowy, poza Zastosuj/Anuluj/Cofnij; security.*, trusted.* i system.* są chronione.
- Tryb POSIX 0000–0777 jest zsynchronizowany z checkboxami, zachowuje istniejące special bits i odświeża ACL zgodnie z semantyką maski kernela.
- Hidden rozróżnia widoczność KIO, kropkę w nazwie Unix i niepotwierdzone flagi natywne. Zmiana kropki korzysta ze wspólnego rename; natywne Hidden NTFS nie jest emulowane ani zapisywane.
- Primary, Split i Search współdzielą dialog, odświeżanie, blokady utraty elementu i bezpieczne zamykanie. ACL działających i zerwanych symlinków pozostaje niedostępne również po rename.
- FULL MANUAL PASS Stage 1–6 pochodzi z wcześniejszego odbioru potwierdzonego przez użytkownika. Realny ext4 i zdalny KIO: NOT TESTED. Końcowa weryfikacja wydania: PROPERTIES_RELEASE_040.md; audyt przed wydaniem pozostaje historyczny.

Poza zakresem: etykiety SELinux, chattr/immutable, Linux file capabilities, większy redesign ACL, admin:// retry.

Known limitation / Znane ograniczenie: path-based KIO/libacl operations cannot guarantee atomic protection against hostile replacement during an operation. / Operacje KIO/libacl oparte na ścieżce nie gwarantują atomowej ochrony przed wrogą podmianą podczas operacji.
