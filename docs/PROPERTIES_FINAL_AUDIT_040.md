# ThisPC 0.40.0 — finalny audyt przed wydaniem (2026-10-08)

Werdykt: **READY FOR RELEASE** po minimalnej naprawie ACL symlinków i ponownej walidacji. Oznacza gotowość do autoryzowanego przygotowania wydania, nie już opublikowane wydanie. Runtime/source nadal 0.39.0. Brak otwartych blokerów produktu w zbadanym zakresie. Bump i końcowe materiały wydania należy wykonać dopiero po osobnej decyzji użytkownika.

## Worktree i pochodzenie

Repo: /home/sebastianh/Pobrane/kio-thispc. Branch main; HEAD, lokalny origin/main i peeled v0.39.0 identyczne: af39da5e1719e7d3fada3b437aab100260b9697f. Ahead/behind 0/0. Tag v0.39.0 annotated. Origin https://github.com/Llusty/kio-thispc.git. Nie pobierano nowych refs; zgodność upstream dotyczy lokalnej referencji, nie nowej weryfikacji serwera. v0.40.0 nie istnieje lokalnie.

Przed edycją: 12 zmodyfikowanych plików tracked, 354 insertions / 68 deletions; 4 nowe dokumenty, 6 modułów, 6 tests oraz outputs/properties-040-stage6 z logami/debug/fixture evidence. Całość przypisana do Stage 1–6, bez obcych lub niewyjaśnionych zmian. CMake rejestruje wszystkie sześć nagłówków w obu targetach; runner rejestruje sześć nowych suites dokładnie raz. Nowe pliki pozostają untracked do przyszłego jawnego stagingu. Surowy diff/inventory i refs zachowano w artifacts. outputs zawiera również wcześniejsze nieudane próby/debug, nie tylko final PASS; nie wolno włączyć go hurtowo do źródłowego release assetu.

## Stage 1–6 i manual acceptance

| Stage | Zakres | Automatycznie obecnie | Odbiór manualny |
|---|---|---|---|
| 1 | capability/locality/identity | PASS | wcześniejszy FULL MANUAL PASS użytkownika |
| 2 | bounded xattr viewer | PASS | wcześniejszy FULL MANUAL PASS użytkownika |
| 3 | lokalne user.* add/edit/remove | PASS | wcześniejszy FULL MANUAL PASS użytkownika |
| 4 | numeric POSIX, special bits, ACL | PASS | wcześniejszy FULL MANUAL PASS użytkownika |
| 5 | backend-aware Hidden | PASS | wcześniejszy FULL MANUAL PASS użytkownika |
| 6 | integration, loss, close, parity | PASS | wcześniejszy FULL MANUAL PASS użytkownika |

Źródło manual status: bezpośrednia wiadomość użytkownika przekazująca finalny audyt, nie cached assistant reference. Potwierdzono wcześniej Primary/Split/Search, symlinki/broken links, Apply/OK/Cancel, delete/move/replacement/parent loss, real Btrfs xattr+POSIX+Hidden i dalszą edycję po rename, NTFS fuseblk /mnt/g oraz ntfs3 /mnt/f-raid z zapisem/readback, dwie poprawki availability/layout, real vfat SanDisk U3 Cruzer Micro z eject/odłączeniem/remount i odmową zapisu ze starego dialogu, lifetime i pusty pgrep po shutdown. Audyt nie powtarzał manualnych testów ani nie modyfikował NTFS/USB/RAID. Ta akceptacja poprzedza nową minimalną poprawkę symlink ACL; jej weryfikacja w audycie jest automatyczna.

RESULTS.md na fixture nadal ma historyczne PENDING. Został zachowany bez zmian zgodnie z poleceniem. Aktualny status i jego pochodzenie zapisano tutaj oraz w nagłówku raportu Stage 6 i roadmapach. Starsze sekcje raportów są historyczne, nie bieżące deklaracje testów.

## Bezpieczeństwo i ryzyko

- Identity: device/inode/type EntryNoFollow, pierwotna tożsamość nie jest zastępowana przy utracie. Apply i rename ponownie sprawdzają ścieżkę; loss latch blokuje stare okno, ACL, POSIX, Hidden i xattr. Ancestor watches/Solid wykrywają path/mount loss. Odtworzenie ścieżki nie przywraca zapisów starego dialogu.
- Rename: odrzuca istniejący destination, także broken symlink; KIO moveAs bez delegate nadpisującego. Revalidation następuje również po wcześniejszym permission job. Symlink rename dotyczy samego linku.
- Naprawiony blocker: refresh ACL po rename przekazywał hardcoded symlink=false. Działający link otrzymywał ACL celu; możliwe było późniejsze zapisanie jego uprawnień mimo polityki no-follow. Test przed poprawką zakończył się FAIL 'renamed link ACL remains unavailable' (runner exit 1, suite process 134). Poprawka wszystkich trzech refresh call sites w src/propertiesdialog.h przekazuje captured entryKind==SymbolicLink. Tests/properties-integration.cpp sprawdza live/broken link rename, SymlinkUnavailable, odmowę write i niezmienione 0644 celu. Suite zwiększona 209 -> 228 checks. Żadnego fallbacku ani osłabienia guardów.
- xattr: wyłącznie lokalne user.*, ścisłe UTF-8/hex/rozmiar; chronione namespaces bez writes. Revalidation + O_NOFOLLOW + fstat + końcowa revalidation, następnie fsetxattr/fremovexattr. XATTR_CREATE/REPLACE chronią add/edit conflicts. Zapis przypięty do zweryfikowanego FD. Immediate write jest poza Apply/Cancel/Undo i UI mówi o tym jawnie.
- POSIX: cztery cyfry 0000–0777; synchronizacja checkboxów; merge zachowuje 07000; readback 07777; ACL named entries pozostają, maska odzwierciedla normalny chmod. Symlinki/broken links bez chmod celu. Numeric nie rozszerza remote ani admin retry.
- Hidden: KIO UDS_HIDDEN nie jest deklarowany jako pewny native NTFS flag. NTFS/fuseblk/ntfs3 i niejednoznaczne backends pozostają read-only dla Hidden; bez dot-name emulacji. Native flag read/write nie jest obiecywany. Unix safe rename ograniczony do znanych filesystemów i odpowiednich capabilities.
- Local/remote: capability resolver, xattr reader/writer i numeric gates odrzucają zdalne URL przed lokalnymi syscallami; deterministic zero-call coverage. Istniejący admin:// flow jawnie mapuje lokalny obiekt do lokalnego identity preflight, nie jest nową emulacją remote.
- Lifetime: QPointer/deferred close osłania KJob::exec; workers przechwytują wartości, generacje odrzucają stale wyniki, ograniczone pule; busy completion ponownie sprawdza loss. Wspólny production dialog i callbacks dla Primary/Split/Search; inspector availability nie znosi rename/chmod recovery checks.

Ograniczenia pozostają jawne: KIO chmod/rename i libacl writes są path-based; preflight nie gwarantuje atomowej ochrony przeciw wrogiej podmianie w środku backend job. FD xattr nie zapisze do replacement, ale po ostatnim check może jeszcze zmienić oryginalny inode, który właśnie przeniesiono/unlinked. In-flight syscall nie jest cofany przy cancel/generation change. Device/inode/type nie zawiera inode generation ani mount ID; nie daje absolutnej ochrony przed ponownym użyciem tożsamości. Viewer lgetxattr jest path-based i końcowy check odrzuca wykryte replacement; nie jest atomowym snapshotem przeciw ABA. To ograniczenia istniejącego kontraktu, nie świeże obietnice bezpieczeństwa.

Przeprowadzono drugi przegląd ścieżek odczytu/zapisu, symlink/rename/ACL i release assets w tym audycie; nie był to osobny niezależny recenzent ani nowy manual pass.

## Bieżące testy i artifacts

Wyniki obliczono z RAW logów ostatniego przebiegu po poprawce, nie z przekazanych historycznych totals. Focused selection: properties, properties_capabilities, properties_xattrs, properties_xattr_edit, properties_posix_mode, properties_hidden, properties_integration, properties_data, properties_lifecycle, drive_properties, acl, acl_editor, action_state, panes, search. Dobór różni się od historycznego focused 4177; obecne 4359 nie jest jego prostą sumą.

| Kontrola finalnych źródeł | RUN SUITE | Unikalne | Checks | Failures | Exit | Duplikaty |
|---|---:|---:|---:|---:|---:|---|
| Focused | 15 | 15 | 4359 | 0 | 0 | brak |
| Full | 64 | 64 | 35064 | 0 | 0 | brak |
| version-cli | — | — | 6 | 0 | 0 | nie jest suite |
| configure/build | — | — | — | 0 | 0 | — |
| git diff --check | — | — | — | 0 | 0 | — |
| DESTDIR staged install | — | — | — | 0 | 0 | — |

Build skonfigurowano i skompilowano bez scripts/build.sh, którego rm -rf build naruszyłoby wymóg zachowania istniejących build fixtures. Użyto równoważnego configure + incremental compile. Final full runner dodatkowo buduje production i test-hook target do batch rename. CLI test weryfikuje --version i -v, stdout oraz stderr; osobny odczyt potwierdza thispc-view 0.39.0.

Durable evidence: outputs/properties-040-final-audit-20261008/. Pliki final-focused.raw.log / final-focused.exit / final-focused.totals.json; final-full.raw.log / final-full.exit / final-full.totals.json; final-build.raw.log/.exit; build-configure.raw.log/.exit; final-version.raw.log/.exit; final-diff-check.raw.log/.exit; install-staging.raw.log/.exit i install-staging-files.txt. parse-current.py odtwarza per-suite counts oraz kontroluje exits, failures, jedno success summary/suite i duplicates. Logi przed poprawką (focused/full) i link-repro FAIL pozostają osobno. Nie doliczono version-cli jako RUN SUITE. Parser/final artifact checks są w artifact-verification.txt.

## Dokumentacja i release assets

Roadmap EN/PL odzwierciedla oficjalny sześciostopniowy scope i wcześniejszy FULL MANUAL PASS; raport Stage 6 i Hidden mają rozróżnienie historii/current acceptance. Stage 1/2 nie mają osobnych nowych audit docs: evidence jest w roadmapach i raportach Stage 3/5/6. README/CHANGELOG/UPDATE pozostają wersją wydaną 0.39.0; nowe 0.40 materiały są wyłącznie DRAFT.

Packaging: CMake targety, KIO worker, executable, desktop i ikony zarejestrowane; staged installation do /tmp PASS, bez sudo/live install. MIT LICENSE obecna, brak nowej zależności linkowej dla sys/xattr; libacl już wymagane w CMake. Nie tworzono release archive z niezatwierdzonego worktree; git archive przed commitem pominęłoby nowe untracked moduły/tests.

Odziedziczone braki do korekty podczas przygotowania release: README PL/EN nadal pokazują archive 0.37; nie wymieniają wymaganej libacl/pkg-config; uninstall banner mówi 0.4. uninstall.sh wymaga build/install_manifest.txt i odmawia bez manifestu; install.sh używa sudo i restartuje workers. Nie uruchamiano tych skryptów. AGENTS.md ma stary baseline 0.27, ale dyscyplina bezpieczeństwa/release nadal obowiązuje; faktyczne refs mają 0.39. Nie są to blokery Stage 1–6, lecz muszą być uwzględnione w release preparation.

Nowe pliki draft: docs/RELEASE_NOTES_040_DRAFT.md (PL/EN) oraz docs/RELEASE_CHECKLIST_040_DRAFT.md (dokładne pola wersji, testy i sekwencja). Obecne README/CHANGELOG/UPDATE i version expectations nie były jeszcze bumpowane.

## Zmiany wprowadzone wyłącznie podczas tego audytu

1. src/propertiesdialog.h: trzy refresh ACL zachowują symlink policy (minimalny guard fix).
2. tests/properties-integration.cpp: live/broken link rename/ACL regression (+19 checks).
3. ROADMAP.md/pl.md: wcześniejszy Stage 6 manual PASS zgodnie z wiadomością użytkownika; stare totals jawnie historyczne; link do final audit.
4. docs/PROPERTIES_STAGE6_INTEGRATION.md i PROPERTIES_HIDDEN_SEMANTICS.md: current acceptance/provenance nad historycznymi sekcjami.
5. docs/PROPERTIES_FINAL_AUDIT_040.md, RELEASE_NOTES_040_DRAFT.md, RELEASE_CHECKLIST_040_DRAFT.md: nowy raport i drafty.
6. outputs/properties-040-final-audit-20261008/: surowa weryfikacja, exits, parser, inventory, patches. Build wygenerował własne pliki oraz staged install manifest; brak zmian systemowej instalacji.

Żadnych innych zmian produktu, resetu, czyszczenia worktree, zmian sources/ projektu ChatGPT, fixture Testy/RESULTS.md, mountów, NTFS UserMapping, ACL realnych partycji czy opcji montowania.

## NOT TESTED, blokery i STOP

Real ext4: **NOT TESTED** (brak dostępnego mountu). Real remote KIO: **NOT TESTED** (brak endpointu). Deterministyczne testy tych polityk PASS. Nie wykonywano nowego sprzętowego/manualnego odbioru; wcześniej potwierdzone NTFS/vfat/Btrfs pozostają wcześniejszymi wynikami. Nie przetestowano nowego source tarballa 0.40 ani bumpu, ponieważ nie są jeszcze autoryzowane. Absolutne hostile atomic/ABA guarantees nie są zapewniane.

Jedyny potwierdzony nowy blocker — ACL target po rename symlinka — naprawiony i sprawdzony. Otwartych blokerów produktu brak; publikację nadal blokuje brak osobnej autoryzacji oraz niewykonane, celowo odłożone release preparation.

Następne kroki dopiero po decyzji użytkownika: version bump + release docs -> final build/focused/full/version/diff/asset verification -> selektywny commit -> annotated v0.40.0 -> push commit/tag -> GitHub release. Pełny zakres plików i asset check w RELEASE_CHECKLIST_040_DRAFT.md. Zachować Testy, RESULTS.md i fixtures do akceptacji audytu; outputs/debug/build data to wyłącznie kandydaci do późniejszego porządkowania, teraz nie usuwać.

**STOP. Nie wykonano bumpu, commit, tag, push, GitHub release ani publikacji. Oczekiwanie na decyzję użytkownika.**
