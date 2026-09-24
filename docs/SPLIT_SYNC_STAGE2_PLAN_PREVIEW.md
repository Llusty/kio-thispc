# SPLIT_SYNC_STAGE2_PLAN_PREVIEW.md — Split View Sync Plan Preview (Stage 2)

## 1. Zakres Stage 2 (Scope)
Stage 2 wersji 0.31.0 realizuje czysto obliczeniowy, deterministyczny i w 100% **odczytowy (READ-ONLY)** mechanizm wyliczania oraz prezentacji planu synchronizacji pomiędzy lewym a prawym panelem w trybie Split View aplikacji `thispc-view`.

W Stage 2:
- Nie są wykonywane żadne modyfikacje ani mutacje plików (`KIO::copy`, `KIO::move`, `KIO::del`, `QFile::copy`, `QFile::remove`, `QFile::rename`, `mkdir`, `rmdir` itp. są bezwzględnie wykluczone).
- Wyliczony plan stanowi wyłącznie podgląd (preview) akcji kwalifikowanych do ewentualnego wykonania w przyszłym etapie Stage 3.
- Interfejs użytkownika nie zawiera przycisków wykonawczych typu „Wykonaj”, „Zastosuj” ani „Usuń”.

---

## 2. Kierunki synchronizacji (Sync Directions)
Plan synchronizacji wyliczany jest deterministycznie dla jednego z dwóch jawnie wybranych kierunków:
1. **Lewy → Prawy** (`LeftToRight`):
   - Źródło (*Source*): Lewy panel
   - Cel (*Destination*): Prawy panel
2. **Prawy → Lewy** (`RightToLeft`):
   - Źródło (*Source*): Prawy panel
   - Cel (*Destination*): Lewy panel

Algorytm planowania jest symetryczny i zunifikowany — logika planowania przyjmuje wynik porównania paneli (Stage 1) oraz parametr kierunku.

---

## 3. Tabela semantyki planowania akcji

Dla wybranego kierunku źródło (`Source`) → cel (`Destination`):

| Stan porównania (Stage 1) | Typ wpisów | Planowana akcja | Etykieta akcji PL / EN | Wykonywalność w Stage 3 | Uzasadnienie (Reason) |
|---|---|---|---|---|---|
| **Same** | Plik po obu stronach | `NoAction` | *Bez zmian* / *No action* | Nie | PL: *"Bez zmian"*<br>EN: *"No action"* |
| **Only on Source** | Zwykły plik | `CopyFile` | *Kopiuj* / *Copy* | Tak | PL: *"Skopiuj do [prawego/lewego] panelu"*<br>EN: *"Copy to [right/left] pane"* |
| **Only on Destination** | Zwykły plik / folder | `NoAction` | *Bez zmian* / *No action* | Nie | PL: *"Pozostaw bez zmian — usuwanie nadmiarowych elementów nie jest częścią Stage 2"*<br>EN: *"Keep unchanged — deleting destination-only items is outside Stage 2"* |
| **Changed** | Zwykły plik po obu stronach | `UpdateFile` | *Zaktualizuj* / *Update* | Tak | PL: *"Zaktualizuj [prawy/lewy] plik wersją z [lewego/prawego] panelu"*<br>EN: *"Update [right/left] file from [left/right] pane"* |
| **Changed** | Niezgodność typu (plik vs folder) | `Conflict` | *Konflikt* / *Conflict* | Nie | PL: *"Konflikt typu: plik / folder"*<br>EN: *"Type conflict: file / folder"* |
| **Same** | Folder po obu stronach | `NoAction` | *Bez zmian* / *No action* | Nie | PL: *"Folder istnieje po obu stronach; zawartość nie została porównana"*<br>EN: *"Folder exists on both sides; contents were not compared"* |
| **Only on Source** | Folder tylko po stronie źródłowej | `Unsupported` | *Nieobsługiwane* / *Unsupported* | Nie | PL: *"Folder tylko po stronie źródłowej — synchronizacja katalogów nie jest częścią Stage 2"*<br>EN: *"Directory exists only on source side — directory synchronization is outside Stage 2"* |

---

## 4. Kluczowe zasady bezpieczeństwa i brak usuwania

1. **Brak polityki Mirror / Brak Delete**:
   Elementy istniejące wyłącznie po stronie docelowej (`Only on Destination`) otrzymują status `NoAction`. Stage 2 nie usuwa ani nie planuje automatycznego usuwania plików nadmiarowych.
2. **Brak rekurencji dla katalogów**:
   Katalogi obecne po obu stronach nie są rekurencyjnie porównywane (`NoAction`). Katalogi obecne tylko po stronie źródłowej oznaczane są jako `Unsupported` (synchronizacja drzew katalogów nie wchodzi w zakres Stage 2).
3. **Brak sum kontrolnych i hashy**:
   Plan opiera się wyłącznie na zaakceptowanym modelu metadanych Stage 1. Nie są odczytywane treści plików ani nie są liczone sumy MD5/SHA-256.
4. **Brak dodatkowych zapytań I/O w fazie planowania**:
   Funkcja `computeSyncPlan` jest czystą, deterministyczną funkcją w pamięci operującej na snapshotcie `CompareEntry`.

---

## 5. Obsługa niepełnych metadanych i duplikatów

- **Niepełne metadane**:
  Jeżeli wpis został zaklasyfikowany w Stage 1 jako `Changed` z powodu asymetrii w dostępności metadanych (np. rozmiar lub czas modyfikacji nieznany po jednej stronie), Stage 2 planuje `UpdateFile` dla zwykłego pliku, zachowując informację o niepełnych metadanych w szczegółach (`differenceDetails`).
- **Zachowanie wielokrotności duplikatów**:
  Dla systemów plików lub workerów KIO zwracających powtórzone nazwy, `computeSyncPlan` zachowuje wielokrotność wpisów w kolejności 1:1 bez spłaszczania mapą kluczy (`std::map[name] = ...`).

---

## 6. Cykl życia snapshotu i odświeżanie (Refresh)

- Wynik podglądu planu (`SyncPlanPreviewDialog`) jest statycznym snapshotem powiązanym z numerem generacji (`m_generation`) porównania paneli.
- Kliknięcie przycisku **Odśwież** w oknie porównania paneli unieważnia stary stan i wymusza ponowny listing asynchroniczny KIO.
- Przycisk podglądu planu jest aktywny wyłącznie po pomyślnym ukończeniu porównania paneli.

---

## 7. Ograniczenia lokalizacji specjalnych

- **`thispc:/` / Ten komputer**: Porównanie oraz planowanie synchronizacji są zablokowane z czytelnym komunikatem informacyjnym.
- **`thispcsearch:/` / Wyniki wyszukiwania**: Porównanie wyników wyszukiwania jest zablokowane.

---

## 8. Co pozostaje do wdrożenia w Stage 3
W kolejnym etapie (Stage 3):
- Wykonanie operacji kopiowania plików zakwalifikowanych jako `CopyFile` oraz aktualizacji `UpdateFile` za pomocą dedykowanych jobów KIO.
- Integracja z oknem postępu transferów i obsługą konfliktów/błędów zapisu.
- Zabezpieczenie przed nadpisaniem bez potwierdzenia.
