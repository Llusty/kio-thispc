# Batch Rename 0.28.0 — crash recovery, Etap 1

Etap 1 jest wyłącznie inspekcyjny. Nie wykonuje rename, rollbacku ani recovery,
nie migruje v1 i nie modyfikuje istniejących journalów lub odzyskiwanych plików.
Wydanie 0.28.0 pozostaje wstrzymane; polityka B będzie wykonywana dopiero w
osobnym, zatwierdzonym etapie.

## Format v2 (tylko parser i fixtures)

Produkcja nadal zapisuje v1. Parser v2 akceptuje wyłącznie ograniczony obiekt z
polami: `schema=2`, `scope=recovery-audit-v2`, `kind`, `directory`, `direction`,
`phase`, `completedSteps`, `items` i `digest`. Element ma dokładnie `row`,
`source`, `destination`, `device`, `inode`. Digest to SHA-256 zwartego JSON po
usunięciu pola `digest`. Parser ogranicza plik do 1 MiB i 10 000 elementów,
odrzuca symlinki, dodatkowe pola, względne lub obce katalogi, duplikaty oraz
niejednoznaczne identity.

Audytor enumeruje wszystkie `cycle-*` i `linear-*` jako v1, v2, unknown albo
corrupt oraz ukryte/tymczasowe podejrzane artefakty. Dla v2 sprawdza pełną mapę
device/inode i wszystkie oczekiwane luki bez wykonywania operacji na plikach.
Każdy v1, unknown, corrupt, wiele journalów, obcy inode, zajęta luka, stan
częściowy/nieobsługiwany lub niepewny lock zamyka gate.

## Podfazy 1B–1C: lock, bootstrap, dispatch i startup

Oddzielny `recovery-audit.lock` używa kernelowego `flock`. Każda współpracująca
instancja musi zdobyć go przed końcowym audytem i utrzymuje go przez dispatch;
twórca journala musi wejść w ten sam protokół przed `cycle.lock`, utworzeniem i
publikacją journala. Kolejność jest zawsze `recovery-audit.lock → cycle.lock`.
Nie ma ścieżki odwrotnej, więc protokół nie tworzy cyklu deadlocku. Deskryptor
locka ma `CLOEXEC`, zatem podproces nie dziedziczy przypadkowo własności.

Brak katalogu recovery nie oznacza już stanu clean. Bootstrap przechodzi po
absolutnej ścieżce deskryptorami katalogów z `O_NOFOLLOW`, tworzy brakujące
katalogi jako prywatne (`0700`), otwiera przez `openat` wyłącznie prywatny,
regularny `recovery-audit.lock` (`0600`) i dopiero po udanym nieblokującym
`flock` wykonuje audyt. Symlink w dowolnym komponencie, obcy właściciel,
nieprywatny końcowy katalog lub lock, zły typ pliku oraz błąd mkdir/open/flock
zamykają gate. Istniejący `cycle.lock` jest tylko odczytywany i klasyfikowany
jako live/stale/ambiguous; audytor go nie usuwa.

Jest to jedyny wyjątek od „read-only” Etapu 1 wobec danych użytkownika: czysty
audyt może bezpiecznie utworzyć wyłącznie prywatny recovery root i pusty plik
locka. Nie tworzy, nie usuwa, nie migruje ani nie edytuje journala i nie dotyka
plików objętych potencjalnym recovery.

Centralny gate jest sprawdzany na wejściu oraz ponownie po każdym modalnym
dialogu i bezpośrednio przed konstrukcją/startem mutującego joba. Obejmuje to:

- rename pojedynczy i Batch Rename (każda kontynuacja między elementami);
- nowy plik, plik z szablonu i nowy katalog;
- wklejanie, kopiowanie, przenoszenie i DnD, łącznie z natywnym dialogiem
  konfliktu oraz asynchronicznym fallbackiem do KIO;
- Trash i Empty Trash po potwierdzeniu;
- tworzenie i wypakowanie archiwum po dialogu wyboru celu;
- zmiany nazwy i praw w Properties po zatwierdzeniu;
- Undo/Redo KIO, natywne, drzewiaste oraz journalowane replaye.

Menu kontekstowe, skróty klawiaturowe, pasek narzędzi, schowek i DnD zbiegają
się w powyższych kontrolerach; nie mają osobnej ścieżki dispatchu. Samo Copy/Cut
do schowka, przeglądanie, wyszukiwanie, nawigacja, podgląd i odczyt Properties
nie mutują plików i pozostają dostępne. Przy blokadzie startup i okresowy audyt
wyłączają akcje mutujące, a trwała niemodalna etykieta paska stanu pozostaje
widoczna. Czysty audyt ukrywa etykietę i ponownie włącza normalne operacje.

Regresje 1B–1C używają wyłącznie tymczasowego `XDG_DATA_HOME` i osobnego katalogu
recovery. Pokrywają journal pojawiający się w otwartym dialogu rename, nowego
pliku i katalogu (zero dispatchu i zero zmian), journal między asynchronicznymi
elementami Batch Rename (brak następnego dispatchu), startup z pending v1 i
invalid v2 oraz ponowne otwarcie akcji po czystym audycie. Deterministyczne
testy wieloprocesowe startują z nieistniejącym recovery root i barierami:
`A final audit → B journal begin/publish attempt → A dispatch` oraz w kolejności
odwrotnej. Sprawdzają rzeczywisty plik utworzony przez dispatch, brak publikacji
konkurencyjnego journala, zero niedozwolonych efektów w kolejności odwrotnej,
live lock, czysty start, błędy i ataki symlink/path oraz wznowienie po wygaśnięciu
locka. Wszystkie fault injections i artefakty żyją w jednorazowych katalogach.

## Jawne ograniczenia Etapu 1

- v2 nie jest jeszcze emitowane przez produkcyjne Batch Rename.
- `cycle.lock` starszej implementacji nie jest kernelowym lockiem; PID pozwala
  rozpoznać typowe live/stale, ale brak lub niejednoznaczność zawsze zamyka gate.
- `flock` koordynuje wszystkie nowe współpracujące instancje thispc-view i ich
  nowe twórcy journalów, lecz stary writer v1 lub proces zewnętrzny, który nie
  honoruje protokołu, pozostaje poza gwarancją.
- Dla współpracujących instancji odcinek końcowy audit→dispatch oraz
  begin→publish journala jest serializowany tym samym kernelowym lockiem.
  Nie jest to jednak transakcja z dowolnym procesem systemowym: proces
  niekooperujący nadal może zmienić plik lub utworzyć artefakt po rechecku.
- Job KIO uruchomiony przed pojawieniem się journala może nadal wykonać skutki.
  Bariera zatrzymuje nowe i kolejne dispatch'e, ale nie anuluje, nie cofa i nie
  „obejmuje” już działających jobów. Nigdy nie usuwa ani nie modyfikuje obcego
  journala.
- Automatyczne dokończenie kierunku (polityka B), usuwanie journala i jakakolwiek
  mutacja odzyskiwanych danych należą wyłącznie do przyszłego Etapu 2.
