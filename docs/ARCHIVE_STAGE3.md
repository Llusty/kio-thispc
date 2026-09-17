# 0.25.0 Stage 3 — tworzenie archiwów (wersja do testów)

## Zakres

Menu „Wyślij do” zachowuje historyczną pozycję „Skompresowany plik ZIP…”;
dodano pozycje 7z i tar.gz. Wybór dotyczy lokalnych plików i katalogów
mających jeden katalog nadrzędny, także przy zaznaczeniu w wynikach Search.
Użytkownik wybiera lokalną nazwę i katalog docelowy. Rozszerzenie odpowiada
formatowi.

`ArchiveCreationJob` wykorzystuje libarchive w wątku roboczym i istniejący
menedżer operacji (postęp liczony w bajtach wejściowych, zakończenie, błąd,
anulowanie). ZIP nie jest już uruchamiany przez odłączony proces `zip`.
Zakończenie odświeża katalog docelowy w obecnie widocznych panelach.
Nie zmieniono mechanizmu wypakowywania ze Stage 2.

## Ochrona danych

- Pliki wejściowe otwierane są przez deskryptor katalogu, bez podążania za
  dowiązaniami symbolicznymi. Katalog jest skanowany bez rekurencji przez
  symlinki; przy zmianie pliku w trakcie zadania operacja zgłasza błąd.
- Odrzucane są symlinki, pliki specjalne, niepoprawne nazwy, wybór z wielu
  katalogów, a także docelowe archiwum wewnątrz wybranego drzewa katalogów.
- Wynik jest budowany w prywatnym podkatalogu *w katalogu docelowym*.
  Publikacja używa Linux `renameat2(RENAME_NOREPLACE)` — nawet wyścig z innym
  procesem nie może nadpisać pliku, katalogu ani symlinka o tej nazwie.
- Po błędzie i anulowaniu prywatny katalog roboczy jest sprzątany.
  Program nie ma mechanizmu automatycznej naprawy pozostałości po awarii
  całego procesu albo zaniku zasilania.

## Ograniczenia

Wymagany Linux, `/proc`, `renameat2(RENAME_NOREPLACE)`, lokalizacja `C.UTF-8` i libarchive
z obsługą zapisu wskazanych formatów. Limit: 64 GiB danych wejściowych,
100 000 wpisów, długość ścieżki do 4096 znaków. Bez hasła, szyfrowania,
symlinków, plików specjalnych, aktualizacji istniejącego archiwum, scalania,
nadpisywania i Undo. Metadane ograniczają się do czasu modyfikacji i
bezpiecznych uprawnień plików / katalogów; ACL i xattr nie są zachowywane.

Brak pauzy; przy dużych plikach anulowanie może poczekać na trwającą operację
wejścia/wyjścia. Szacowany procent oznacza przeczytane bajty wejściowe,
a nie rozmiar skompresowanego wyniku. Format 7z może wymagać więcej pamięci
i czasu niż ZIP. Dane użytkownika mogą zmienić się po ostatnim sprawdzeniu;
nie jest to migawka systemu plików.

## Testy i akceptacja

Automatycznie: `TMPDIR=/tmp python3 tests/run-pane-actions.py --suites archive_creation`.
Pełna regresja: `TMPDIR=/tmp python3 tests/run-pane-actions.py --all`.
Test obejmuje prawdziwe tworzenie każdego formatu, zawartość, Unicode,
katalogi, kolizje, symlinki, cel wewnątrz źródła i anulowanie.

Przed akceptacją w KDE, na danych testowych:

1. Zaznacz kilka plików i katalog w lewym panelu; utwórz kolejno ZIP, 7z,
   tar.gz. Sprawdź je przez ponowne otwarcie / wypakowanie.
2. Powtórz w prawym panelu i z wyniku Search. Zmień aktywny panel w trakcie
   kompresji; potwierdź, że wynik trafia do wybranego katalogu.
3. Wybierz istniejącą nazwę: aplikacja ma odmówić bez zmian pliku.
   Spróbuj też nazwy ze spacją i polskimi znakami.
4. Spróbuj anulować dłuższą kompresję i potwierdź brak gotowego archiwum oraz
   brak katalogu `.thispc-create-*` w miejscu docelowym.
5. Sprawdź długie ścieżki w Split View i działanie ekstrakcji ze Stage 2.

Nie instalować ani nie commitować bez automatycznych testów w środowisku KDE
i ręcznej akceptacji użytkownika.
