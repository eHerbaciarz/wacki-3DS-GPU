# PS Vita: szybsze rysowanie na GPU (SDL2 GXM)

Gałąź `vita-gpu` = `3ds-gpu` + scalony `port-vita` + zmiany niżej, według
notatek z paczki ReClaw `reclaw-vita-gpu` (Herbaciarz, GPL-3.0).

Na Vicie SDL2 już rysuje przez GPU (GXM), więc nie ma osobnej warstwy jak
na 3DS: skalowanie 640×480 → ekran 960×544 robi GPU. Wolna była część CPU
każdej klatki we wspólnym `video_sdl.c` i domyślne zegary. Vita ma teraz
własny `src/platform/vita/video_vita.c` (desktop/Switch/handheldy bez zmian).

## Co się zmieniło

- **Zegary** (z `vita_speed.h`): CPU 444 MHz, szyna 222, GPU 222, xbar 166
  zamiast startowych 333 MHz. Ustawiane w `vita.c` przed `main()`.
  Tylny panel dotykowy wyłączony (`VITA_DISABLE_TOUCH_BACK`).
- **Tekstura RGB565** zamiast ARGB8888: połowa bajtów na klatkę; paleta jako
  tablica 256 kolorów liczona tylko przy zmianie palety; konwersja 4 piksele
  naraz (wcześniej 3 odczyty palety na piksel, 1,2 MB zapisu na klatkę).
- **Dwie tekstury na zmianę**: `SDL_LockTexture` na GXM daje bezpośrednio
  pamięć tekstury bez czekania, a przy wyłączonym vsync GPU może jeszcze
  czytać poprzednią — pisanie do drugiej usuwa ryzyko śmieci/rwania.
- **Pomijanie identycznych klatek** (obraz, paleta, tryb) — ekran zostaje.
- **Wygładzanie** domyślnie włączone, przełączane pierwszym przyciskiem
  ekranu **Grafika** (`s_opt_gfx1` / `video_mode`), tak jak na 3DS. To
  **„ostre wygładzanie”**: klatka najpierw dokładnie podwojona (najbliższy
  sąsiad, do tekstury 1280×960 w GPU), potem płynnie zmniejszona do ekranu
  — gładkie krawędzie bez rozmycia, jakie daje zwykłe liniowe przy 1,13×.
  Całość w GPU.
- **Panorama / panorama z paskami / 4:3** (przycisk Y): zob.
  [panorama.md](panorama.md); 4:3 = 725×544 na środku.
  **Dotyk** (tryb absolute) liczony przez ten sam prostokąt — wcześniej w
  4:3 cały panel był mapowany na całą grę mimo czarnych pasów.
- **SDL2_mixer** nie jest już linkowany (kod go nie używa).
- **Log** `ux0:/data/wacki/wacki_perf.log` — ten sam format co na 3DS
  (`docs/3ds-gpu.md`), wątek o niskim priorytecie co 0,5 s. Na Vicie
  `wait` = czas `SDL_RenderPresent`, brak osobnego czasu GPU. `sleep`
  przychodzi z `EnginePaceFrame` (nowy hook `g_pace_sleep_hook` w
  `timer.c`). Linie `!` = ostrzeżenia/błędy, w tym nazwa renderera
  (powinno być `VITA gxm (GPU)`).

Vsync zostaje wyłączony jak w `port-vita` — silnik sam trzyma 30 FPS.

## Budowanie

```
tools/build-vita.sh        # dist/wacki.vpk
```

Lokalne VitaSDK (`VITASDK`, domyślnie `/usr/local/vitasdk`) gdy nie ma
Dockera albo przy `WACKI_VITA_LOCAL=1`; inaczej obraz `vitasdk/vitasdk`.
Oficjalne SDL2 (GXM) jest budowane, jeśli go brak albo ciągnie vitaGL.

## Do przetestowania (na Vicie)

- [ ] W logu `renderer: VITA gxm (GPU)`.
- [ ] Obraz poprawny w 4:3 i obu panoramach, bez śmieci przy ruchu i przy wysuwaniu panelu.
- [ ] Dotyk w 4:3 trafia w kliknięte miejsce (także przy krawędziach).
- [ ] Wygładzanie domyślnie włączone; pierwszy przycisk Grafiki przełącza
      od razu; zostaje po restarcie.
- [ ] FPS/`engine`/`draw` w logu wobec poprzedniego buildu `port-vita`.
- [ ] Bateria / temperatura przy 444 MHz w dłuższej grze.
- [ ] Filmy i dźwięk bez zmian.
