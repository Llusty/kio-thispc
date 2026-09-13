# Aktualizacja do 0.21.0

```bash
cd ~/Pobrane
rm -rf kio-thispc
unzip kio-thispc-0.21.0.zip
cd kio-thispc
chmod +x install.sh
./install.sh
```

## 0.21.0 — refaktor architektury i stabilizacja

- wydzielono główne komponenty z `src/thispcview.cpp` do osobnych modułów;
- ograniczono duplikację pomiędzy panelem głównym i Split View;
- zachowano kompatybilność ustawień i przywracania sesji;
- dodano/rozszerzono testy paneli, Drag & Drop kart, `PropertiesDialog`, wyszukiwania i `FileActions`;
- finalny zestaw automatyczny: 577 zaliczonych asercji;
- pełny test manualny KDE/CachyOS zakończony bez regresji;
- brak zmian wymagających migracji ustawień użytkownika.

## 0.20.0 — Szybki dostęp / Ulubione / Ostatnie

- nowa sekcja `Szybki dostęp` w lewym panelu;
- przypinanie i odpinanie folderów z menu kontekstowego;
- zmiana kolejności przypiętych folderów przez Drag & Drop;
- trwały zapis przypiętych folderów i ich kolejności przez `QSettings`;
- nowa sekcja `Ostatnie` z ostatnio odwiedzanymi lokalizacjami;
- trwały zapis historii ostatnich lokalizacji między uruchomieniami;
- zachowany stabilny układ nazw plików z 0.19.0.4.

## 0.19.0.4 — stabilny układ nazw w widoku ikon

- stała, sztywna geometria siatki w IconMode (`gridSize` + `uniformItemSizes`);
- zaznaczenie elementu nigdy nie zmienia `sizeHint()` ani nie przesuwa rzędów poniżej;
- formatowanie tekstu przez `QTextLayout` z obsługą `WrapAtWordBoundaryOrAnywhere`;
- maksymalnie 2 linie w trybie standardowym (ostatnia linia poprawnie elidowana);
- maksymalnie 4 linie w trybie „Pełne nazwy” z zachowaniem jednolitej wysokości kafelków;
- pełna, nieobcięta nazwa zaznaczonego elementu rysowana jako callout na poziomie viewportu po bazowym `paintEvent`;
- całkowite usunięcie starej nakładki QLabel;
- brak zmian w trybach ListMode oraz DetailsMode.
