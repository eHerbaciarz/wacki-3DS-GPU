# Napisy: dialogi, opisy dźwięków dla niesłyszących, filmy, inne języki

W oryginalnej grze dialogi są **tylko głosowe**: `Gadki.scr` podaje dla
każdej kwestii plik WAV (`[sampl]`) i nie ma w nim tekstu. Tekst dochodzi z
osobnego pliku napisów, który robi narzędzie `tools/napisy/wacki_napisy.py`
(przepisuje nagrania Whisperem) i który potem poprawiasz ręcznie.

Gra czyta `napisy/<język>.txt` z katalogu danych gry (tam, gdzie
`DANE_*.DTA`), albo z `./napisy/`. Działa na wszystkich platformach (napisy
są rysowane we wspólnym kodzie, przed wysłaniem klatki na ekran).

## W grze

- **Włączanie:** opcja „napisy” w menu dźwięku (Solund) — ta sama co w
  oryginale; na PC też **F2** (a **F4** = napisy dla niesłyszących).
- **Napisy dla niesłyszących** (imiona mówców + opisy dźwięków): piąty
  przycisk w menu Solund (w oryginale jego znaczenie jest nieznane, port go
  nie używał). Ma oryginalną ikonę, więc po najechaniu kursorem u góry
  ekranu pojawia się „Napisy dla niesłyszących: włączone/wyłączone” (i
  ostrzeżenie, jeśli brakuje pliku napisów). Zapis w `wacki.cfg`.
- **Dialog:** tekst na dole obrazu (nad panelem w grze), przez całą
  kwestię. Z wyłączonym głosem kwestia zostaje na ekranie tyle, ile trzeba
  na przeczytanie (albo do kliknięcia).
- **Opisy dźwięków** (dla niesłyszących), np. `[wybuch]`, na górze ekranu
  przez 2,5 s — także przy wyłączonym dźwięku.
- **Filmy:** napisy z czasami, na dole.
- **Nazwy przedmiotów:** w oryginale nazwę przedmiotu w ekwipunku mówi głos
  (po ~2 s najechania; plik WAV z `Item.scr`, bez tekstu). Z napisami nazwa
  pojawia się też jako tekst nad panelem — od razu po najechaniu, póki
  kursor jest na przedmiocie. Tłumaczy się jak resztę pliku.
- **Kolory:** jasny tekst z ciemnym obrysem, dobierane z palety bieżącej sceny.
- **`wacki.cfg`:**
  - `subtitles_lang=pl` — który plik (`napisy/pl.txt`, `napisy/en.txt`…);
  - `subtitles_speakers=-1|0|1` — imiona mówców przed kwestią (-1 = jak w pliku);
  - `subtitles_sounds=-1|0|1` — opisy dźwięków (-1 = jak w pliku).

Font gry ma polskie ogonki. Inne litery z akcentem (č, ü, é…) pokażą się
bez akcentu, a pisma niełacińskiego (cyrylica, japoński) font nie ma.

## Format pliku

UTF-8, pola oddzielone **tabulatorem**, `|` = nowa linia (długi tekst i tak
się zawija). Pusty tekst = brak napisu.

```
@jezyk=pl
@mowcy=1          # imiona mówców (dla niesłyszących)
@dzwieki=1        # opisy dźwięków

[dialogi]
fjgut04a.wav	Fjej	Ebek, chodź tutaj szybko!
[dzwieki]
wybuch.wav	[wybuch]
[film intro.avi]
00:00.500	00:02.000	Pierwsza linia|druga linia
[przedmioty]
magnes.wav	magnes
```

## Narzędzie

Python 3.8+, sama biblioteka standardowa. Do przepisywania mowy jeden z:
`pip install faster-whisper` (zalecane), `pip install openai-whisper`, albo
whisper.cpp (`--whisper-cpp …/whisper-cli --model-plik ggml-medium.bin`).
Do rozpakowania DTA służy `dta-extract` z repo (`make` → `dist/dta-extract`).

```
# wszystko naraz: rozpakuj DTA, przepisz dialogi (+ filmy), zapisz plik
python3 tools/napisy/wacki_napisy.py wszystko --dane /ścieżka/do/danych --filmy --sdh

# potem: popraw DANE/napisy/pl.txt ręcznie (linie „# SPRAWDŹ” najpierw)
python3 tools/napisy/wacki_napisy.py sprawdz /ścieżka/do/danych/napisy/pl.txt

# inny język: szkic z oryginałem w komentarzach (albo --argos: maszynowo, offline)
python3 tools/napisy/wacki_napisy.py tlumacz /ścieżka/do/danych/napisy/pl.txt --na en
```

- **Pamięć podręczna:** przepisane nagrania trafiają do
  `napisy_praca/przepisane.json`, więc drugie uruchomienie nie liczy ich od
  nowa.
- **Poprawki są bezpieczne:** `plik` i `wszystko` **zachowują Twoje
  poprawki** — uzupełniają tylko puste wpisy.
- **Tekst ze skryptu:** jeśli kwestia ma tekst w `Gadki.scr`, bierze się go
  stamtąd, bez Whispera.
- **Podpowiedź dla Whispera:** `--podpowiedz` podaje mu imiona postaci
  (domyślnie Wacki, Ebek, Fjej).
- **Halucynacje:** typowe zmyślenia Whispera na ciszy (np. „Napisy stworzone
  przez społeczność Amara.org”) i niepewne rozpoznania są oznaczane
  `# SPRAWDŹ`.
- **Dźwięk filmów:** wyciągany wprost z AVI, gdy jest PCM; inaczej przez
  ffmpeg.
- **Prawa:** plik napisów zawiera tylko tekst. Pliki gry zostają u Ciebie —
  napisów nie trzymaj w repo razem z danymi gry.

## Do przetestowania

- [ ] Whisper na prawdziwych nagraniach (tutaj sprawdzone tylko atrapą:
      modele były niedostępne z tego środowiska) — jakość, imiona postaci.
- [ ] Napisy w dialogu: czytelne na kilku scenach, nad panelem.
- [ ] Głos wyłączony: kwestie czekają na przeczytanie / kliknięcie.
- [ ] Opisy dźwięków u góry; filmy z czasami (czy zgrane z obrazem).
- [ ] 3DS: tekst na górnym i dolnym ekranie (zoom), w 3D wychodzi przed obraz.
