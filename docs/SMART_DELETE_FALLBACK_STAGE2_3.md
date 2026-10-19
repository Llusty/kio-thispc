# ThisPC 0.41.2 — Stage 2.3 & 2.3.1: Smart Delete Fallback & Safety Hardening

## 1. Kontekst i analiza problemu

W wersjach ThisPC 0.41.1 oraz w pierwotnym szkicu 0.41.2 naciśnięcie zwykłego klawisza `Delete` na telefonie Android (`mtp:/...`) wyświetlało martwy komunikat błędu:
> „W tej wersji usuwanie do Kosza jest dostępne tylko dla lokalnych plików i folderów.”

W etapie Stage 2.3 wprowadzono dialog fallbacku (propozycja trwałego usunięcia). Audyt bezpieczeństwa wykazał jednak, że warunek uruchomienia fallbacku:
> *„Co najmniej jeden element nie jest plikiem lokalnym”*

był **zbyt szeroki**:
1. **Zaznaczenia mieszane**: Gdyby użytkownik zaznaczył 1 plik lokalny i 1 plik MTP, oba pliki trafiłyby do dialogu trwałego usuwania, omijając Kosz dla pliku lokalnego!
2. **Inne protokoły zdalne**: Dowolny nielokalny protokół KIO (np. `sftp:`, `smb:`, `webdav:`, protokoły nieznane) automatycznie proponowałby trwałe usunięcie pliku zamiast bezpiecznego zachowania.
3. **Brak ponownej weryfikacji**: Podczas wyświetlania modalnego okna dialogowego stan urządzenia mógł ulec zmianie (np. odłączenie kabla USB).

## 2. Architektura bezpieczeństwa Stage 2.3.1 (Safety Hardening)

W etapie 2.3.1 wprowadzono rygorystyczny mechanizm klasyfikacji zaznaczenia:

### A. Walidacja wstępna (Sanity & Root Check)
Każdy element w zaznaczeniu jest weryfikowany:
- Czy URL jest poprawny i niepusty?
- Czy URL nie wskazuje na lokalizację wirtualną `thispc:/`?
- Czy URL nie jest korzeniem systemu plików `/`?
- Czy URL nie jest korzeniem protokołu `mtp:/`, korzeniem urządzenia ani korzeniem pamięci współdzielonej telefonu (`parts.size() >= 3` jest wymagane dla MTP)?
W przypadku wykrycia chronionego korzenia operacja zostaje natychmiast przerwana z czytelnym ostrzeżeniem.

### B. Ścisła klasyfikacja protokołów (Protocol Classification)
- **100% plików lokalnych (`allLocal == true`)**:
  - Wykonanie standardowego przepływu: pytanie o przeniesienie do Kosza, wywołanie `KIO::trash()`, pełna integracja z `KIO::FileUndoManager`.
- **100% plików/folderów MTP wewnątrz pamięci (`allMtp == true && parts.size() >= 3`)**:
  - Wyświetlenie dialogu Smart Delete Fallback z domyślnym przyciskiem „Anuluj”.
  - **Ponowna weryfikacja (Re-validation)**: Po kliknięciu „Usuń trwale” program sprawdza, czy stan URL-i i urządzenia nie uległ zmianie.
  - Wykonanie `KIO::del()` bezpośrednio bez podwójnego okna dialogowego.
- **Zaznaczenia mieszane (`!allLocal && !allMtp`)**:
  - **ZABLOKOWANE**. Program wyświetla komunikat:
    > *„Zaznaczenie zawiera elementy z różnych protokołów lub lokalizacji o różnych możliwościach obsługi Kosza. Wybierz elementy z jednej spójnej lokalizacji.”*
    Żaden plik lokalny ani zdalny nie zostaje skierowany do trwałego usunięcia.
- **Inne protokoły zdalne (`!allLocal && !allMtp`, np. SFTP, SMB, protokoły nieznane)**:
  - **BRAK automatycznego fallbacku**. Program wyświetla informację:
    > *„Ta lokalizacja nie obsługuje przenoszenia plików do Kosza. Aby trwale usunąć te elementy, użyj skrótu Shift+Delete lub opcji «Usuń trwale…» z menu kontekstowego.”*

## 3. Tabela kontraktów testowych DF-001 – DF-025

| Identyfikator | Zakres testu | Status |
|---|---|:---:|
| **DF-001** | Klawisz Delete na pliku lokalnym zachowuje przenoszenie do Kosza | **GREEN** |
| **DF-002** | Klawisz Delete na pliku MTP aktywuje Smart Delete Fallback | **GREEN** |
| **DF-003** | Dialog Fallback podaje jasną przyczynę (brak obsługi Kosza dla lokalizacji) | **GREEN** |
| **DF-004** | Dialog Fallback ostrzega o nieodwracalności operacji | **GREEN** |
| **DF-005** | Anulowanie dialogu nie uruchamia żadnego zadania KIO | **GREEN** |
| **DF-006** | Bezpieczeństwo przycisków: domyślny przycisk i Escape to „Anuluj” | **GREEN** |
| **DF-007** | Zatwierdzenie uruchamia właściwy `KIO::DeleteJob` | **GREEN** |
| **DF-008** | Brak podwójnego potwierdzenia (jednoznaczne przejście do `deleteUrls`) | **GREEN** |
| **DF-009** | Ścisła integralność zaznaczenia (usuwane tylko zatwierdzone URL-e) | **GREEN** |
| **DF-010** | Blokada usuwania korzeni (MTP root, device root, storage root, system root) | **GREEN** |
| **DF-011** | Niedostępność usuwania przerywa operację bez tworzenia zadania KIO | **GREEN** |
| **DF-012** | Błędy uprawnień plików lokalnych nie wywołują fałszywego fallbacku | **GREEN** |
| **DF-013** | Rozłączone urządzenie MTP nie powoduje nieuprawnionego fallbacku | **GREEN** |
| **DF-014** | Shift+Delete działa bez regresji (bezpośredni dialog Permanent Delete) | **GREEN** |
| **DF-015** | Pozycja menu kontekstowego „Usuń trwale…” działa bez regresji | **GREEN** |
| **DF-016** | Zachowanie izolacji widoków Primary i Split podczas fallbacku | **GREEN** |
| **DF-017** | Automatyczne ciche odświeżenie katalogu źródłowego po usunięciu (bez F5) | **GREEN** |
| **DF-018** | Anulowanie lub błąd nie usuwa optymistycznie elementów z widoku | **GREEN** |
| **DF-019** | Lokalne usuwanie do Kosza zachowuje pełną integrację z Undo | **GREEN** |
| **DF-020** | Trwałe usuwanie przez Fallback nie tworzy fałszywego wpisu w historii Undo | **GREEN** |
| **DF-021** | **Zaznaczenie mieszane (lokalne + MTP / różne protokoły) jest bezwzględnie blokowane** | **GREEN** |
| **DF-022** | **Nieznane i inne protokoły zdalne (np. SFTP) nie uruchamiają automatycznego fallbacku** | **GREEN** |
| **DF-023** | **Ponowna weryfikacja URL-i i stanu urządzenia po zamknięciu dialogu przed zadaniem KIO** | **GREEN** |
| **DF-024** | **Błąd zadania przenoszenia do Kosza nigdy samoczynnie nie konwertuje się w KIO::del** | **GREEN** |
| **DF-025** | **Wyłączność jednorodnego MTP: fallback możliwy wyłącznie dla 100% poprawnych celów MTP** | **GREEN** |
