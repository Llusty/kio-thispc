# 0.28.0 Batch Rename — Stage 2

## Zakres i zasady

Stage 2 rozszerza istniejący bezpieczny planer o zamianę tekstu, zmianę
wielkości liter, zmianę/usunięcie rozszerzenia i wyrażenia regularne. Operacje
tekstowe oraz case działają na rdzeniu nazwy; rozszerzenie jest obsługiwane
osobno. Katalogi nie mają rozszerzenia, a nazwa rozpoczynająca się pojedynczą
kropką, np. `.profile`, jest traktowana jako dotfile bez rozszerzenia.

Pusty wzorzec literalny niczego nie wstawia. Pusty lub niepoprawny regex jest
odrzucany. Wzorzec ma limit 1024 znaków, tekst zamiany 4096 znaków, wynik 255
bajtów UTF-8, a partia 1000 elementów. Regex używa składni `QRegularExpression`
i obsługuje odwołania do grup w tekście zamiany.

Cała tabela preview pozostaje widoczna również dla niepoprawnego planu. Kolumna
Stan, zaznaczenie i komunikat wskazują pierwszy problematyczny wiersz. Komunikaty
rozróżniają duplikat wyników, kolizję z innym zaznaczonym źródłem oraz istniejący
element spoza zaznaczenia. Cykle i zamiany nazw nadal są odrzucane; Stage 2 nie
osłabia ochrony przed nadpisaniem.

Wiersze, których wynik jest identyczny z nazwą źródłową, pozostają widoczne ze
stanem `Pominięto (bez zmian)`, ale nie trafiają do kolejności wykonania, postępu
ani historii Undo. Co najmniej jedna rzeczywista zmiana wystarcza do wykonania
partii. Gdy wszystkie wiersze są bez zmian, wykonanie pozostaje zablokowane.

Przed wykonaniem nadal obowiązuje ponowny preflight źródła i celu. Powyższy opis
był prawdziwy dla pierwotnego Stage 2, który wykonywał kolejne `KIO::moveAs`, był
anulowalny między elementami i zapisywał osobne wpisy Undo. **Bieżący stan przed
0.28.0 jest inny:** kwalifikująca się partia lokalna (co najmniej dwie aktywne
zmiany, jeden katalog, różne inode, bez swapów/cykli) jest wykonywana przez
journalowany, nieatomowy replay `renameat2(RENAME_NOREPLACE)`, bez anulowania w
trakcie, a po pełnym sukcesie dostaje jedno wspólne Undo/Redo. Dialog ostrzega o
tym przed startem i można go bezpiecznie odrzucić. Niekwalifikujące się plany
nadal mogą trafić do sekwencyjnej ścieżki KIO z Cancel i osobnym Undo ukończonych
elementów. Zmiana wyłącznie wielkości liter jest dozwolona, gdy cel nie istnieje;
na filesystemie zgłaszającym wariant celu jako istniejący zostaje bezpiecznie
zablokowana zamiast ryzykować nadpisanie.

## Przyczyna zrzutu Stage 1

Dla zaznaczonego elementu `1` prefiks `4` tworzy cel `41`. Kod Stage 1 pokazywał
ten konkretny komunikat tylko wtedy, gdy `41` istniało i jego dokładna ścieżka
nie należała do snapshotu zaznaczenia. Kolizja z elementem zaznaczonym miała
oddzielny komunikat. Sam zrzut nie jest dowodem stanu katalogu, ale nie wskazuje
na false-positive planera. Pusta tabela była wadą UX: planer zwracał natychmiast
przed dopisaniem konfliktowego i dalszych wierszy. Stage 2 zachowuje pełny
preview i wskazuje wiersz `1 → 41`.

## Ręczna checklista KDE/CachyOS — audyt przed 0.28.0

Testować tylko w nowym katalogu z jednorazowymi danymi.

Potwierdzenia użytkownika zapisane podczas ręcznej sesji:

- [x] Kolizja z niezaznaczonym `41`: pełny preview, właściwy wiersz i blokada wykonania.
- [x] Literal, w tym wielokrotne wystąpienie i pusty wzorzec.
- [x] Regex: grupy, błędny i pusty wzorzec.
- [x] Case: lower/upper/Title Case, Unicode i zmiana tylko wielkości liter.
- [x] Rozszerzenia dla `a.tar.gz`, pliku bez rozszerzenia, `.profile` i katalogu.
- [x] Mieszany no-op: jedna aktywna zmiana i trzy pominięte elementy; Undo/Redo PASS; all-no-op pozostaje zablokowany.
- [x] Duplikat wyniku, kolizja z zaznaczonym źródłem i cel spoza zaznaczenia; brak nadpisania.
- [x] Race po preview: ponowny preflight zatrzymuje wykonanie bez nadpisania.
- [ ] Anulowanie **w trakcie** większej, niekwalifikującej się partii KIO i osobne Undo ukończonych elementów — nie wykonano. Nie dotyczy zwykłej kwalifikującej się partii lokalnej, która świadomie nie ma Cancel w trakcie i po sukcesie ma wspólne Undo. To opcjonalna kontrola odmiennej ścieżki (np. zdalne URL-e); ze względu na krótki i niedeterministyczny moment anulowania nie jest minimalnym ręcznym warunkiem lokalnego wydania 0.28.0.
- [x] Split View: preview w obu panelach i wszystkie trzy tryby widoku; ponadto prefiks/sufiks/numeracja, zwykły Rename z Undo/Redo, Trash z Undo, Quick Look i Preview Pane bez regresji.

Osobno potwierdzono anulowanie **przed startem** przez odrzucenie ostrzeżenia:
PASS, zero zmian. Nie jest to dowód anulowania w trakcie.

Focused:

```fish
env TMPDIR=/tmp python3 tests/run-pane-actions.py --suites batch_rename panes actions
```

Full:

```fish
./scripts/build.sh
env TMPDIR=/tmp python3 tests/run-pane-actions.py --all
git diff --check
```

Stage 3A (bezpieczne acykliczne łańcuchy, cykle nadal zablokowane, Undo osobno)
opisuje `docs/BATCH_RENAME_STAGE3.md`.
