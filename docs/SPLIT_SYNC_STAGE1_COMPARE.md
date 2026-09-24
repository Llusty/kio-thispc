# SPLIT_SYNC_STAGE1_COMPARE.md — Split View Pane Comparison (Stage 1)

## 1. Zakres Stage 1 (Scope)
Stage 1 wersji 0.31.0 realizuje bezpieczne, wyłącznie **odczytowe (READ-ONLY)** porównanie zawartości bieżącego katalogu lewego (głównego) i prawego (Split View) panelu w aplikacji `thispc-view`.

W Stage 1:
- Nie są wykonywane żadne operacje kopiowania, przenoszenia, usuwania, synchronizacji ani zmiany nazw plików.
- Interfejs użytkownika ma charakter wyłącznie informacyjny i prezentacyjny.
- Nie ma możliwości przypadkowego zmodyfikowania plików z poziomu okna wyników porównania.

---

## 2. Granice porównania (Direct Children Only)
- Porównywane są **wyłącznie bezpośrednie dzieci** aktualnie otwartych katalogów w obu panelach.
- **Brak rekurencji**: Algorytm nie schodzi w głąb poddrzew katalogów.
- Dla podkatalogów obecnych po obu stronach status określa istnienie katalogu o danej nazwie na poziomie bieżącej ścieżki; nie stanowi gwarancji, że ich zawartość jest identyczna.
- Nie jest obliczany rekurencyjny rozmiar katalogów.

---

## 3. Semantyka klasyfikacji wyników

Klasyfikacja wpisów opiera się na dokładnym dopasowaniu nazwy (`case-sensitive`, bez zniekształceń wielkości liter i z pełną obsługą Unicode):

| Status | Etykieta PL | Etykieta EN | Warunki kwalifikacji |
|---|---|---|---|
| **Same** | *Takie same* | *Same* | Pozycja występuje po obu stronach; dla plików metadane (rozmiar, mtime) są zgodne lub brak różnic w dostępnych danych; dla katalogów oznacza to istnienie folderu o tej samej nazwie po obu stronach. |
| **Only left** | *Tylko po lewej* | *Only left* | Pozycja o danej nazwie istnieje wyłącznie w katalogu lewego panelu. |
| **Only right** | *Tylko po prawej* | *Only right* | Pozycja o danej nazwie istnieje wyłącznie w katalogu prawego panelu. |
| **Changed** | *Zmienione* | *Changed* | Pozycja istnieje po obu stronach, ale różni się typem (plik vs katalog), rozmiarem, czasem modyfikacji `mtime`, lub metadana jest znana po jednej stronie, a nieznana po drugiej. |

---

## 4. Porównywane metadane, brakujące pola i brak hashy
- **Porównywane atrybuty**:
  1. Nazwa wpisu (dokładne porównanie znaków UTF-8/Unicode bez wymuszonego lowercase).
  2. Rodzaj wpisu (`isDir` / MIME type).
  3. Rozmiar pliku (`size` w bajtach z `KIO::UDSEntry::UDS_SIZE` / `QFileInfo::size()`).
  4. Czas modyfikacji (`mtime` w sekundach epoki).
- **Obsługa brakujących metadanych**:
  - Jeżeli brak różnic w dostępnych atrybutach, ale przynajmniej jedno pole (rozmiar lub data) jest niedostępne po obu stronach (np. `size == -1` lub `mtime <= 0`), status pozostaje `Same`, lecz `differenceReason` jednoznacznie informuje:
    - PL: *"Brak różnic w dostępnych metadanych; część metadanych niedostępna"*
    - EN: *"No differences in available metadata; some metadata unavailable"*
  - W kolumnach szczegółów metadanych (Lewy / Prawy) brakujące wartości formatowane są symbolem myślnika `"—"`. Jeśli dostępny jest tylko rozmiar lub tylko czas, wyświetlana jest znana część wraz z `"—"` dla brakującej.
  - Jeśli dana metadana jest znana po jednej stronie, a niedostępna po drugiej, wpis jest klasyfikowany jako `Changed` z precyzyjną informacją w szczegółach.
- **Brak hashy zawartości**:
  W Stage 1 nie są liczone sumy kontrolne (MD5 / SHA-256) ani nie jest odczytywana treść plików, co chroni wydajność i zapobiega blokowaniu I/O. Status `Same` oznacza zgodność na poziomie metadanych.

---

## 5. Obsługa duplikatów nazw (Multiplicity Preservation)
- Jeśli w wyniku specyfiki źródła KIO na liście jednego z paneli pojawi się więcej niż jeden wpis o tej samej nazwie:
  - Wszystkie wpisy są zachowywane (`std::map<QString, std::vector<FileInfo>>`), żaden wpis nie jest nadpisywany.
  - Wpisy o tej samej nazwie są parowane deterministycznie w kolejności wystąpienia do `maxCount = max(countL, countR)`.
  - Nadmiarowe wystąpienia po stronie lewej lub prawej otrzymują odpowiednio status `OnlyLeft` lub `OnlyRight`.

---

## 6. Asynchroniczność KIO i cykl życia okna modalnego
- Listowanie zawartości obu paneli odbywa się asynchronicznie za pomocą `KIO::listDir(..., KIO::HideProgressInfo)`.
- **Okno modalne**: Okno dialogowe `SplitCompareDialog` otwiera się modalnie (`exec()`), zatrzaskując ścieżki lewego i prawego panelu w chwili uruchomienia. Nawigacja w oknie głównym w trakcie otwartego dialogu jest zablokowana.
- **Numery generacji (`m_generation`)**: Każde uruchomienie lub odświeżenie (`startListing()`) inkrementuje numer generacji. Spóźnione lub anulowane odpowiedzi KIO ze starszych zapytań są bezwarunkowo ignorowane.
- **Bezpieczne anulowanie**: Destruktor `~SplitCompareDialog()` oraz ponowne wywołanie `startListing()` natychmiast bezpiecznie przerywają aktywne `KIO::ListJob` (`cancelJobs()`), eliminując wycieki i ryzyko use-after-free.
- **Etykiety ścieżek**: Ścieżki w nagłówkach są zabezpieczone przed wstrzyknięciem znaczników HTML (`.toHtmlEscaped()`).

---

## 7. Dowiązania symboliczne (Symlinks) i typy specjalne
- Zgodnie z architekturą `FileInfo` w `browsercommon.h`, symlinki reprezentowane są według podstawowej flagi typu (`isDir`) oraz dostępnych metadanych KIO.
- W Stage 1 nie jest wykonywany dodatkowy I/O (stat/readlink) dla sprawdzania celów dowiązań symbolicznych, aby zagwarantować natychmiastowy czas odpowiedzi i brak blokad I/O.

---

## 8. Obsługa lokalizacji specjalnych i Search
- **`thispc:/` / Ten komputer**: Porównanie jest blokowane z czytelnym komunikatem — porównywać można jedynie konkretne katalogi.
- **`thispcsearch:/` / Wyniki wyszukiwania**: Porównywanie zbiorów wyników wyszukiwania jako wirtualnych folderów nie ma jednoznacznej semantyki hierarchicznej. W Stage 1 jest bezpiecznie blokowane z czytelnym komunikatem informacyjnym.

---

## 9. Plan dalszych etapów (Future Stages)
Dalsze etapy 0.31.0 (niezaimplementowane w Stage 1):
- Stage 2: Plan wykonawczy i podgląd akcji synchronizacji (Preview execution plan).
- Stage 3: Kopiowanie różnic lewo -> prawo / prawo -> lewo.
- Stage 4: Bezpieczna synchronizacja dwukierunkowa i usuwanie nadmiarowych plików (zabezpieczone opcjami).
- 0.32.0: Porównanie zawartości plików (zewnętrzne narzędzia diff, Meld/KDiff3, sumy SHA-256).
