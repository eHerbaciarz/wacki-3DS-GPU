# Podpowiedzi

Podpowiedzi są **domyślnie wyłączone**. Włącza je **żarówka** na głównym
ekranie Opcji, pod przyciskami gry.

- Zgaszona, szara i przekreślona: podpowiedzi są wyłączone.
- Zapalona: podpowiedzi są włączone.
- Wybór zapisuje się w `wacki.cfg` jako `hints=0|1`.

## W grze

| Platforma | Podpowiedź |
|-----------|------------|
| PC (klawiatura) | `H` |
| Pad na PC / Steam Deck | View + X |
| Switch | − + Y |
| Vita | SELECT + kwadrat |
| 3DS | SELECT + A |

Przycisk wyboru (View / − / SELECT) naciśnięty samodzielnie robi to, co
wcześniej: kolory na PC i Vicie, tryb dotyku na Switchu, ręczny tryb na
3DS. Działa to w chwili puszczenia przycisku.

- Każde miejsce ma kilka stopni podpowiedzi. Pierwszy to naprowadzenie,
  ostatni to rozwiązanie.
- Pierwszy stopień pokazuje się od razu.
- Każdy następny jest dostępny dopiero **3 minuty** po poprzednim
  (`hint_wait=180` w `wacki.cfg`, w sekundach). Wcześniej przycisk
  pokazuje ostatni stopień z dopiskiem „kolejna za m:ss”.
- Podpowiedź widać u góry ekranu przez 10 s.
- Postęp jest pamiętany osobno dla każdego tematu do końca sesji.
- Gdy podpowiedzi są wyłączone, przycisk tylko przypomina o żarówce.

## Plik

Plik `podpowiedzi/<język>.txt` leży obok danych gry, tak jak `napisy/`.
Język jest ten sam co napisów; gdy brakuje pliku w danym języku, gra
bierze `pl`. Format opisuje nagłówek pliku `podpowiedzi/pl.txt`.

| Wpis | Znaczenie |
|------|-----------|
| `[etap N]` | Etap gry. |
| `= Tytuł` | Temat podpowiedzi. |
| `@ słowa` | Miejsca, w których temat działa: nazwa komnaty albo tła zawiera któreś z tych słów. |
| `@ *` | Temat ogólny dla całego etapu. |
| `1<TAB>tekst` … | Kolejne stopnie podpowiedzi. |

Gra wybiera temat według etapu i miejsca. Jeśli żaden nie pasuje, bierze
temat ogólny etapu.

## Nazwy miejsc

Prawdziwe nazwy komnat są w plikach gry. Wypisze je polecenie:

    python3 tools/napisy/wacki_napisy.py rozpakuj --dane KATALOG_GRY
    python3 tools/napisy/wacki_napisy.py komnaty  --dane KATALOG_GRY

Gra zapisuje też w logu przy każdej podpowiedzi etap, komnatę i tło, np.
`etap 1, komnata 'X', tlo 'Y.pic'`.

Obecny `pl.txt` to **szkic do sprawdzenia**: jest ułożony własnymi słowami
na podstawie poradników, a słowa po `@` są zgadywane.

Kod: `src/text/hints.c`, ikona w `src/menu/subs_icons.c`, testy w
`tests/test_subtitles.c` (`hints_*`).
