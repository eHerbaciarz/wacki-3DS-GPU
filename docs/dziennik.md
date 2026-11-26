# Dziennik (logi do zgłaszania błędów)

Gra zawsze zapisuje plik **`wacki.log`** obok `wacki.cfg`:

| Platforma | Gdzie |
|-----------|-------|
| Windows / Linux / Steam Deck | Folder gry, a dokładniej folder, z którego uruchamiasz program. |
| Switch | `sdmc:/switch/wacki/` |
| 3DS | `sdmc:/3ds/wacki/` |
| Vita | `ux0:/data/wacki/` |

Poprzednie uruchomienie zostaje jako **`wacki.old.log`**. W pliku są
ważne zdarzenia (start, wersja, platforma, dane, ostrzeżenia i błędy),
a każda linia ma czas od startu.

## Diagnostyka (dokładny dziennik)

Diagnostykę włączasz klawiszem **`Shift+F3`**, wpisem `diagnostyka=1`
w `wacki.cfg` albo opcją `--diag`. Gra pamięta wtedy w pamięci ostatnie
2048 zdarzeń: każde kliknięcie (co, gdzie, którą postacią), przejście,
podniesienie przedmiotu, wejście do pokoju i kroki skryptów.

Te wpisy trafiają do `wacki.log`:

| Kiedy | Jak |
|-------|-----|
| Na żądanie | PC: `F3`. |
| Przy awarii | Automatycznie, gdy program się wysypie. |
| Przy wyjściu | Automatycznie przy wyjściu z gry, jeśli diagnostyka jest włączona. |

## Jak zgłosić błąd

1. Włącz diagnostykę (`Shift+F3`).
2. Odtwórz błąd.
3. Gdy gra się zablokuje, naciśnij `F3`.
4. Dołącz `wacki.log` do zgłoszenia.

Kod: `src/log.c` (pierścień + plik), `include/wacki/log.h`.
`LOG_TRACE` i `LOG_DEBUG` są wkompilowane także w wydania, ale formatują
się tylko przy włączonej diagnostyce.
