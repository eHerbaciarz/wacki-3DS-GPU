# Panorama z wysuwanym panelem

Dawny tryb „stretch” rozciągał całą klatkę 4:3 na ekran 16:9. Teraz,
**w czasie gry**, obraz jest przycięty (scena jest bez zniekształceń),
a dolny panel z przedmiotami jest schowany i wysuwa się, gdy kursor
dojedzie do dolnej krawędzi.

Klatka gry ma 640×480. Scena to wiersze 0–399 (16:10), a panel to wiersze
400–479.

## Tryby (jeden przycisk, po kolei)

| Klik | `aspect_mode` | Co widać |
|------|---------------|----------|
| 1 | `wide` | **Panorama przycięta.** Scena wypełnia całą szerokość ekranu. Kilka wierszy się nie mieści (16:9: 360 z 400, Vita: ~363, 3DS: 384), więc okno przesuwa się za kursorem. |
| 2 | `wide-bars` | **Panorama z paskami.** Widać całą scenę 640×400, po bokach są wąskie czarne paski (16:9: po 5%, 3DS: po 8 px). |
| 3 | `4:3` | Oryginał 4:3 z pasami. |

Przyciski: PC `F7`, Switch fizyczny `X`, Vita `Y`, 3DS `Y`. Wybór zapisuje
się w `wacki.cfg`. Stara wartość `stretch` działa jak `wide`.

## Panel

- **Wysuwanie:** kursor w wierszu ≥ 396, czyli przy dolnej krawędzi
  ekranu. Panel wjeżdża w ~0,2 s i wypycha scenę w górę, więc nic go nie
  zasłania.
- **Chowanie:** kursor wraca wyżej, do wiersza < 330.
- **Rozmowy:** w trakcie wypowiadanej kwestii dialogu panel zostaje
  schowany.
- **Okno:** obraz to pionowe okno w klatce. Gdy panel jest schowany, okno
  sięga do wiersza 400. Gdy panel jest wysunięty, sięga do wiersza 480.
  - Górny brzeg okna liczy się proporcjonalnie: `y0 = y_kursora·(L − okno)/L`.
  - Dzięki temu pozycja kursora na ekranie jest proporcjonalna do jego
    wiersza w grze.
  - Przeliczanie myszy i dotyku z powrotem na grę nie zależy więc od
    przesunięcia, a kursor nie „ucieka”.
  - Na PC nieruchoma mysz jest przeliczana na nowo co klatkę, gdy panel
    się przesuwa.

## W 4:3 zostaje

- filmy;
- menu i ekrany bez panelu (np. pauza, Grafika);
- ekran nie szerszy niż ~7:5, np. okno 640×480 na PC;
- stary backend procesorowy 3DS (`VIDEO_3DS=fb`): tam `wide` to dawne
  rozciągnięcie.

## Napisy

Napisy dialogów i dźwięków trzymają się widocznego pasa sceny, więc nie
wypadają poza przycięty obraz.

## Kod

| Plik | Co robi |
|------|---------|
| `src/util/wide_layout.c` | Stan, animacja, geometria i przeliczanie ekran → gra (wspólne dla wszystkich platform). |
| `src/scene/hud_paint.c` | `WideLayoutMarkGameplay()`: ta klatka to gra z panelem. |
| `src/graphics.c` | `WideLayoutFrame()` raz na klatkę, przed napisami. |
| `src/text/dialog.c` | `WideLayoutSetTalking()` w trakcie kwestii. |
| `src/platform/sdl/video_sdl.c` | Backend PC i Switcha. |
| `src/platform/vita/video_vita.c` | Backend Vity. |
| `src/platform/3ds/video_3ds_c3d.c` | Backend 3DS. Każdy backend rysuje jeden prostokąt według `WideLayoutGet()`. |

Testy: `tests/test_filters.c` (`wide_layout_*`).
