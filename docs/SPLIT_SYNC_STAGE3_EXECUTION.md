# SPLIT_SYNC_STAGE3_EXECUTION.md — Split View Synchronization Execution (Stage 3)

## 1. Zakres Stage 3 (Scope)
Stage 3 wersji 0.31.0 implementuje bezpieczny silnik asynchronicznego wykonania planu synchronizacji wygenerowanego w Stage 2 pomiędzy panelami w trybie Split View aplikacji `thispc-view`.

W Stage 3:
- Automatycznie wykonywane są **wyłącznie**:
  - `CopyFile`: plik istnieje wyłącznie po stronie źródłowej (`source -> destination`). Plik docelowy nie istnieje.
  - `UpdateFile`: plik istnieje po obu stronach i różni się metadanymi (`source -> destination`). Następuje bezpieczne zastąpienie pliku docelowego nową wersją ze źródła.
- **Bezwzględnie wyłączone z wykonania**:
  - Usuwanie jakichkolwiek plików lub katalogów (`No Delete / No Mirror`).
  - Modyfikacja elementów oznaczonych jako `Conflict` (np. niezgodność typu plik vs katalog).
  - Modyfikacja elementów oznaczonych jako `Unsupported` (np. katalogi źródłowe).
  - Synchronizacja rekurencyjna katalogów (katalogi po obu stronach pozostają bez zmian).
  - Usuwanie elementów istniejących wyłącznie po stronie docelowej (`Only on Destination` -> `NoAction`).

---

## 2. Architektura wykonania i bezpieczeństwa

### A. Preflight / Revalidation (`revalidateSyncPlan`)
Bezpośrednio przed przystąpieniem do jakichkolwiek mutacji na dysku, silnik wykonuje weryfikację wstępną bieżącego stanu systemu plików względem snapshotu, na bazie którego utworzono plan:
- Weryfikacja bezpieczeństwa nazw (odrzucenie nazw pustych, `.`, `..`, ze znakami `/` i `\`).
- Weryfikacja istnienia i typu pliku źródłowego (`regular file`).
- Weryfikacja rozmiaru oraz czasu modyfikacji pliku źródłowego (jeśli były znane w snapshocie).
- Dla `CopyFile`: potwierdzenie, że element docelowy nadal **NIE istnieje**.
- Dla `UpdateFile`: potwierdzenie, że element docelowy **istnieje**, jest zwykłym plikiem, a jego rozmiar i czas modyfikacji odpowiadają snapshotowi.

W przypadku wykrycia jakiejkolwiek niezgodności (np. plik źródłowy zmodyfikowany lub usunięty po Compare, pojawienie się pliku docelowego, zmiana typu na katalog):
- Weryfikacja wstępna kończy się błędem (`isValid == false`).
- Wykonanie całego planu zostaje **anulowane przed pierwszą mutacją** (`preflightFailed == true`).
- Użytkownik otrzymuje czytelny raport z listą wykrytych niezgodności.
- Stan paneli zostaje automatycznie odświeżony (re-compare).

### B. Świadome potwierdzenie użytkownika (`SyncConfirmationDialog`)
Przed uruchomieniem mutacji wyświetlane jest modalne okno potwierdzenia zawierające:
- Ścieżki źródła i celu oraz kierunek synchronizacji (`Lewy → Prawy` lub `Prawy → Lewy`).
- Liczbę plików do skopiowania (`CopyFile`) i zaktualizowania (`UpdateFile`).
- Informację o wyłączeniach (brak usuwania, brak rekurencji katalogów, konflikty i elementy nieobsługiwane nietknięte).
- Bezpieczny domyślny przycisk skupienia (**Anuluj**).

### C. Bezpieczne operacje plikowe (`LocalFileCopyJob` / KIO)
- Dla plików lokalnych transfery realizowane są przez dedykowany silnik `LocalFileCopyJob`:
  - Dane kopiowane są do pliku tymczasowego `.thispc-part`.
  - W przypadku `UpdateFile`, po pomyślnym zapisaniu nowej wersji i skopiowaniu uprawnień oraz znaczników czasu następuje atomowa podmiana za pomocą `SYS_renameat2(..., RENAME_EXCHANGE)` z weryfikacją tożsamości `LocalFileIdentity`.
  - W razie błędu podczas zapisu plik tymczasowy `.thispc-part` jest usuwany, a oryginalny plik docelowy pozostaje nienaruszony.
- Dla ścieżek KIO wykorzystywane jest `KIO::copyAs`.

### D. Asynchroniczne UI postępu i Cancel (`SyncExecutionDialog`)
- Operacje wykonywane są asynchronicznie, element po elemencie, nie blokując wątku interfejsu użytkownika.
- Wyświetlany jest pasek postępu, licznik wykonanych operacji oraz opis bieżącego elementu (np. `Kopiowanie: plik.txt`).
- Przycisk **Anuluj** umożliwia natychmiastowe przerwanie bieżącego joba i zatrzymanie kolejki:
  - Ukończone wcześniej operacje pozostają zachowane.
  - Kolejne operacje nie są uruchamiane.
  - Raport końcowy oznacza stan jako anulowany.

### E. Odporność na błędy częściowe i raport końcowy
- W przypadku błędu przy pojedynczym pliku (np. brak uprawnień) błąd jest rejestrowany w raporcie, a executor bezpiecznie kontynuuje przetwarzanie pozostałych niezależnych operacji.
- Po zakończeniu (sukces, błąd częściowy, anulowanie lub błąd preflight) prezentowany jest raport podsumowujący:
  - Skopiowano / Zaktualizowano / Bez zmian / Konflikty / Nieobsługiwane / Błędy / Anulowano.
  - Tabela ze szczegółami błędów (nazwa, operacja, komunikat błędu).

### F. Automatyczny Re-Compare
Po zamknięciu okna wykonania (niezależnie od tego, czy nastąpił pełny sukces, błąd czy anulowanie), następuje automatyczne ponowne porównanie paneli (`startListing()`), dzięki czemu widok odzwierciedla aktualny stan na dysku (wykonane `CopyFile` i `UpdateFile` przechodzą w stan `Takie same / Bez zmian`).
