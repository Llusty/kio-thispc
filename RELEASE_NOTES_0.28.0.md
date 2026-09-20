# kio-thispc 0.28.0 — Batch Rename

## Zakres wydania

Wydanie dodaje zbiorczą zmianę nazw z bezpiecznym podglądem, prefix/suffix,
numerowaniem, zamianą tekstu, zmianą wielkości liter i rozszerzeń oraz regex.
Ponowny preflight wykrywa kolizje i zmianę tożsamości plików. Funkcja działa z
aktywnym panelem Split View.

Kwalifikująca się operacja local-linear oznacza co najmniej dwie aktywne zmiany
w jednym lokalnym katalogu, bez swapu/cyklu, ze snapshotem tożsamości i planem,
który może zostać wykonany sekwencyjnie bez KIO fallback. Tylko ta klasa ma w
0.28.0 produkcyjny journal v2 i automatyczne recovery polityki B: po awarii
procesu startup dokańcza zapisany kierunek Execute, Undo albo Redo. Historia
Undo/Redo sprzed restartu nie jest odtwarzana.

## Jawne ograniczenia

- Swap v2: parser i izolowany silnik testowy są zweryfikowane, lecz produkcyjny
  generator, dispatch i startupowa allowlista pozostają wyłączone.
- Cykle 3+: wykonanie i sesyjne Undo/Redo są obsługiwane, ale nie mają
  produkcyjnego automatycznego recovery v2. Wieloetapowy cykl nie jest atomowy.
- KIO fallback, w tym zdalne URL-e i niekwalifikujące się plany, nie ma tych
  samych gwarancji co local-linear. Anulowanie w trakcie tej ścieżki nie zostało
  ręcznie odebrane jako warunek wydania.
- Testy SIGKILL potwierdzają zachowanie po awarii procesu, nie po zaniku
  zasilania. Nie deklarujemy trwałości power-loss bez disposable VM/FS-image.
- Globalna blokada koordynuje współpracujące ścieżki thispc-view. Zewnętrzny
  proces, który jej nie respektuje, może nadal zmieniać nazwy równolegle.
- Precheck/postcheck wykrywają wiele ingerencji, lecz nie eliminują wszystkich
  nieredukowalnych okien TOCTOU. Stan niejednoznaczny zatrzymuje dalsze zmiany,
  zachowuje journal i wymaga ręcznej kontroli.

## Weryfikacja kandydata — 2026-09-20

- build: PASS;
- focused: Batch Rename 1790 + panes 287 + actions 157 = 2234 PASS;
- pełna regresja: 23 zestawy / 5409 asercji PASS;
- `git diff --check`: PASS;
- procesowe recovery, fault injection i black-box aplikacji korzystają z
  katalogów tymczasowych i prywatnego `XDG_DATA_HOME`; nie dotykają prawdziwego
  recovery root ani danych użytkownika.

Końcowy interaktywny ręczny odbiór GUI po zmianie numeru wersji pozostaje
**UNVERIFIED**. Automatyczne i black-box testy nie są przedstawiane jako ręczna
akceptacja człowieka.

## Publikacja

Kandydat nie jest automatycznie instalowany, commitowany ani tagowany. Archiwum
źródłowe jest przygotowywane dopiero po testach; publikacja i instalacja wymagają
osobnej końcowej zgody użytkownika.
