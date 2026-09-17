# Split View — szerokość paneli

Punkt wyjścia: `main`, `b023c600f9a556d68ff89923bc78c88fa6e4394f` oraz zastane
niezacommitowane zmiany Stage 2. Bez zmiany wersji, instalacji, commita i taga.

## Przyczyna i zakres poprawki

Lewy QHBoxLayout sumował minimalne szerokości przycisków breadcrumbs. Ich
minimum przechodziło przez QStackedWidget i układy nagłówka do QSplitter.
Regresja na rzeczywistym oknie odtworzyła zmianę szerokości z 642/517 na
3955/330 pikseli po wejściu w głęboki katalog. Rosło również całe okno.

Audyt objął także prawy panel: cały adres był pojedynczym przyciskiem z
zależnym od tekstu sizeHint/minimumSizeHint. Jego jawne minimum 330 px nie
rozwiązywało przepełnienia wewnętrznego nagłówka. Nazwy katalogów i statusy
QLabel dodatkowo wpływały na podpowiedzi szerokości stron QStackedWidget,
również tych aktualnie ukrytych.

- Lewy pasek ma przewijaną zawartość i przyciski przewijania pokazywane przy
  przepełnieniu. Segmenty zachowują pełną szerokość i dotychczasowe akcje.
  Preferowana/minimalna szerokość obszaru przewijania nie zależy od ścieżki;
  wysokość nadal wynika z zawartości. Ograniczenie SetMinAndMaxSize dotyczy
  zawartości, więc powstaje zakres przewijania zamiast szerszego panelu.
- Oba stosy adresu ignorują poziome sizeHint. Prawy przycisk maluje tekst
  ze środkowym wielokropkiem. Nagłówki i statusy obu paneli mają wielokropek
  końcowy i nie wymuszają szerokości. Pełny tekst pozostaje w kontrolkach,
  podpowiedziach i edytorze adresu (Ctrl+L).
- QSplitter nadal sam obsługuje przeciąganie i zmianę rozmiaru. Nie ma
  przywracania stałych szerokości po każdej nawigacji. Zachowano istniejące
  minimum prawego panelu i zakaz zwijania paneli.
- Zapis stanu przed ukryciem prawego panelu usuwa dodatkowy reset podziału
  do 50/50. Przy ponownym pokazaniu odtwarzany jest podział użytkownika;
  zamknięcie okna z ukrytym panelem nie zapisuje zerowej szerokości.

Podstawa zachowania Qt: [QSplitter](https://doc.qt.io/qt-6/qsplitter.html),
[układy](https://doc.qt.io/qt-6/layout.html),
[QScrollArea](https://doc.qt.io/qt-6/qscrollarea.html).

## Testy

`tests/split-layout.cpp` działa na rzeczywistym, pokazanym oknie Qt w trybie
offscreen. Sprawdza krótkie i głębokie ścieżki oraz pojedynczy długi segment
po obu stronach, fokus/edytor/Escape, Ten komputer i ukryte strony stosów,
zamianę paneli, wyłączenie/włączenie Split, rzeczywiste zdarzenia myszy na
separatorze, proporcje podczas resize, minimum szerokości i odtworzenie
ustawień w nowym oknie. Sprawdza również przewijanie, pełne adresy i piksele
przycisku z wielokropkiem. Opcjonalna zmienna THISPC_LAYOUT_SNAPSHOT zapisuje
obraz okna do diagnostyki. Zestaw `split_layout` jest częścią `--all`.

Focused: `TMPDIR=/tmp python3 tests/run-pane-actions.py --suites split_layout panes search archive archive_jobs archive_menu`.

Pełna regresja: `TMPDIR=/tmp python3 tests/run-pane-actions.py --all`.
Build: `./scripts/build.sh`. Kontrola patcha: `git diff --check`.

## Oddzielnie: Stage 2

Kod produkcyjny ekstrakcji pozostał bez zmian względem stanu zastanego.
W istniejącym teście anulowania proces kontrolowany i jego pomocnik zgłaszają
gotowość i czekają na sygnał, zamiast kończyć się po zwykłym opóźnieniu.
Test sprawdza anulowanie, pojedynczy wynik, usunięcie procesu i danych
roboczych oraz ponowną ekstrakcję tego samego archiwum do tego samego celu
przez rzeczywisty Ark. Alarm stanowi wyłącznie zabezpieczenie testu.

Manual KDE pozostaje osobnym krokiem użytkownika; checklista w TEST_CHECKLIST.md.
