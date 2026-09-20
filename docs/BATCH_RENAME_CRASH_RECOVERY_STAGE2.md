# Batch Rename 0.28.0 — crash recovery, Etap 2

## Status: 2B.1, 2B.2 i 2C PASS w zakresie awarii procesu; 0.28.0 nadal wstrzymane

Podfaza 2C aktywuje kontrolowane v2 wyłącznie dla kwalifikującego się lokalnego,
liniowego initial Execute (co najmniej dwie aktywne zmiany, jeden katalog, bez
swapów i cykli). Ta sama przetestowana ścieżka wykonuje trwałe
`intent → renameat2(RENAME_NOREPLACE)` względem zweryfikowanego deskryptora
katalogu → `fsync` tego samego deskryptora → pełny postcheck wszystkich
`dev/ino/type/mode/size/mtime` i oczekiwanych pustych ścieżek → trwałe
`verified`. Journal jest usuwany dopiero po pełnym trwałym celu i fsync katalogu
recovery.

Startup worker, trzymając `recovery-audit.lock`, rozpoznaje dokładnie jeden
poprawny forward-linear v2, następnie bierze `cycle.lock` i idempotentnie
dokańcza żądany kierunek. v1, unknown, corrupt, multiple, temporary, live lub
ambiguous lock oraz każda niezgodność mapowania pozostają fail-closed bez
automatycznego usuwania. UI zachowuje zamknięty gate i trwałe komunikaty dla
checking, recovering, success oraz intervention/conflict. Historia Undo/Redo
sprzed restartu nie jest odtwarzana; nowe, ukończone operacje nadal tworzą nową
historię grupową. Undo/Redo nadal używa v1 i pozostaje manual-only po crashu.

Podfaza 2B.2 dodała produkcyjny, nieblokujący startup audit i w tamtym momencie
nie aktywowała jeszcze generatora v2 ani automatycznego recovery. Gate jest fail-closed
od konstrukcji singletonu, przez cały stan `checking`, a następnie dla każdego
`pending`, konfliktu i niejednoznaczności. W zakresie 2B.2 worker trzymał wspólny
procesowy protokół przez singleton, lecz nie przejmował `QLockFile` i nie
wykonywał mutacji. Dostęp z GUI używa nieblokującego
odczytu stanu, więc nawet celowo spowolniony audit nie zatrzymuje event loop.

Trwały niemodalny status rozróżnia `checking`, `requires intervention/conflict`
i zakończone sprawdzenie. Stan czysty mówi jawnie, że historia Undo/Redo sprzed
restartu nie jest odtwarzana. Nie twierdzi, że wykonano recovery. Zamknięcie
okna podczas workera jest bezpieczne: callback GUI znika z oknem, a singleton,
worker i `flock` zachowują prawidłowy czas życia procesu.

Zaimplementowano i uruchomiono testowe rusztowanie ograniczone do lokalnego,
liniowego Execute (co najmniej dwie aktywne zmiany, jeden katalog, bez swapów i
cykli). Format v2 zapisuje pełną kolejność, identity katalogu, `dev/ino`, typ,
tryb, rozmiar i nanosekundowy mtime elementów oraz digest. Prototyp wykonuje
`intent → renameat2(RENAME_NOREPLACE) → fsync(dirfd danych) → pełny postcheck →
verified`, a journal usuwa dopiero po trwałym celu i synchronizacji katalogu
recovery. Undo/Redo nadal zapisuje v1 i pozostaje manual-only.

Izolowany helper był zabijany przez `SIGKILL` przed/po prepared, intent,
syscall, fsync katalogu danych i verified, po ostatnim rename przed celem, po
celu przed unlink oraz po unlink przed fsync katalogu journalu. Focused gate
został rozszerzony w podfazie 2B.1 część 1 również o ponowny `SIGKILL` podczas
samego recovery (14 markerów), kolejne idempotentne restarty, dwa równoległe
procesy recovery i kolejność locków. Deterministyczne test-hooki obejmują błędy
`fsync` pliku journalu, katalogu journalu i data dir, `renameat2` zwracające
`ENOSYS`/`EOPNOTSUPP`, a także race zajmujący cel tuż przed syscall. Focused gate
osiągnął 609 asercji PASS. To sprawdza crash procesu, nie zanik zasilania.

Macierz fail-closed obejmuje ponadto obcy inode źródła, brak źródła, zmianę
metadanych, zajętą dziurę, podmianę inode katalogu danych, v1/corrupt/unknown,
wiele journalów oraz live/stale/ambiguous lock. Testy sprawdzają zachowanie
journalu przy konflikcie, nienadpisanie obcego pliku i jego treści, zachowanie
oryginalnych inode oraz cleanup dopiero po pełnym trwałym celu. Wszystkie nowe
faulty działają tylko w osobnych procesach i jednorazowych katalogach
tymczasowych; nie używają realnego recovery root użytkownika.

Kod produkcyjny używa v2 tylko w powyższym wąskim przypadku. Hooki SIGKILL,
wstrzymania i wymuszonych błędów pozostają wyłącznie w buildzie testowym;
produkcyjny syscall i recovery nie zależą od testowego bypassu.

## Weryfikacja 2C

- `./scripts/build.sh` — PASS;
- focused Batch Rename — 624/624 PASS;
- produkcyjny black-box prawdziwego `build/bin/thispc-view`, bez makra testowego,
  z prywatnym jednorazowym `XDG_DATA_HOME`: recovery oraz drugi idempotentny
  restart — PASS;
- pełne `--all` — PASS, w tym Batch Rename 624/624;
- `git diff --check` — PASS;
- wersja pozostaje 0.27.0.

Historyczna macierz 2B.2 używała wyłącznie odizolowanego `XDG_DATA_HOME`, jednorazowych
katalogów oraz osobnych procesów. Obejmują clean, v1, kwalifikujący się v2,
unknown/corrupt/multiple, live/stale/ambiguous lock, trwałe statusy, brak
dispatchu, responsywność przy opóźnionym workerze i zamknięcie okna w trakcie.
Przed aktywacją 2C testy v2 potwierdzały brak rename, rollbacku, zapisu i
cleanupu na produkcyjnym startupie; w 2C odpowiednie asercje zastąpiono
sprawdzeniem bezpiecznego dokończenia i idempotentnego restartu.

## Granice i kryteria wydania

- Undo/Redo recovery, swapy, cykle i KIO fallback pozostają poza zakresem;
- osobny test power-loss na jednorazowym obrazie filesystemu/VM jest wymagany
  przed deklarowaniem odporności na zanik zasilania.

Etap 2 może być oceniany wyłącznie jako recovery po awarii procesu dla jawnie
opisanego podzbioru. Nie jest to pełne recovery Batch Rename ani deklaracja
odporności na zanik zasilania. Etap 3 nie został rozpoczęty, wersja pozostaje
0.27.0 i wydanie 0.28.0 pozostaje wstrzymane.
