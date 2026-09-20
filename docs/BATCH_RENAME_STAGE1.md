# 0.28.0 Batch Rename — Stage 1

## Zakres

Stage 1 dodaje bezpieczną zbiorczą zmianę nazw dla co najmniej dwóch lokalnych
elementów zaznaczonych w aktywnym panelu. Działa w widoku ikon, listy i
szczegółów oraz w obu stronach Split View. Dialog pokazuje pełną tabelę
`stara nazwa → nowa nazwa` przed zatwierdzeniem i obsługuje prefiks, sufiks oraz
opcjonalną numerację z numerem początkowym i szerokością pola.

Plan jest liczony ze snapshotu zaznaczenia w deterministycznej kolejności.
Odrzuca puste/niedozwolone i identyczne nazwy, duplikaty wyników, kolizje z
istniejącymi elementami oraz cykle z innymi zaznaczonymi elementami. Porównanie
wyników bez uwzględniania wielkości liter jest celowo konserwatywne dla
filesystemów nieczułych na wielkość liter. Pliki, katalogi i symlinki mogą być
zaznaczone razem; link nie jest dereferencjonowany. Jedna partia jest ograniczona
do 1000 elementów, aby Stage 1 nie blokował GUI ogromną tabelą.

Bezpośrednio przed każdą operacją aplikacja ponownie sprawdza źródło i cel.
Zmiana w filesystemie zatrzymuje pozostałą część. Nazwy są zmieniane kolejno
przez `KIO::moveAs`, bez delegata oferującego nadpisanie. Błąd lub anulowanie
zatrzymuje kolejne pozycje, a już ukończone operacje pozostają jawnie policzone.
Każda udana zmiana jest osobnym wpisem `KIO::FileUndoManager`; atomowe, wspólne
Undo całej partii nie jest jeszcze deklarowane.

## Poza Stage 1

- replace text, case/extension i regex;
- cykle/zamiany nazw wymagające bezpiecznych nazw tymczasowych;
- pojedyncze atomowe Undo/Redo całej partii;
- zdalne URL-e KIO i bardzo duże zestawy wymagające stronicowania/wirtualizacji.

Stage 2 (replace text, case, rozszerzenia, regex i diagnostyka wierszy preview)
jest opisany w `docs/BATCH_RENAME_STAGE2.md`. Ten dokument pozostaje zapisem
zakresu i zaakceptowanych testów Stage 1.

## Ręczna checklista KDE/CachyOS

Testować wyłącznie w nowym katalogu z jednorazowymi danymi.

- [ ] Zaznaczyć 3 pliki w ikonach, ustawić prefiks/sufiks i numerację; preview i wynik są identyczne.
- [ ] Powtórzyć w liście i szczegółach.
- [ ] W Split View zaznaczyć różne elementy po obu stronach; akcja używa tylko aktywnego panelu.
- [ ] Zmienić zaznaczenie po otwarciu dialogu; preview nadal opisuje pierwotny snapshot.
- [ ] Sprawdzić mieszane pliki, katalog i symlink; cel symlinka pozostaje bez zmian.
- [ ] Utworzyć wcześniej kolidującą nazwę; przycisk zatwierdzenia pozostaje wyłączony.
- [ ] Utworzyć kolizję po pokazaniu preview, ale przed wykonaniem; nic nie jest nadpisane.
- [ ] Anulować w trakcie większej partii; kolejne elementy nie są uruchamiane, komunikat podaje liczbę ukończonych.
- [ ] Po częściowym błędzie sprawdzić Ctrl+Z dla każdego ukończonego wpisu osobno.
- [ ] Potwierdzić brak regresji zwykłego Rename, Trash, Split View, Space Quick Look i Alt+P.

Focused test w fish:

```fish
cd ~/Pobrane/kio-thispc
env TMPDIR=/tmp python3 tests/run-pane-actions.py --suites batch_rename panes actions
```

Pełna regresja i build:

```fish
cd ~/Pobrane/kio-thispc
./scripts/build.sh
env TMPDIR=/tmp python3 tests/run-pane-actions.py --all
git diff --check
```
