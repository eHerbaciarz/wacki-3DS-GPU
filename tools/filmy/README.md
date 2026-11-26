# Filmy z gry

`wacki_filmy.py` wyciąga filmy z Wacków, żeby je obrobić. Filmy gry to
pliki `Dane_NN.dta` (np. `Dane_10.dta` to intro). W środku jest AVI
z obrazem FLIC (640×480, 256 kolorów) i dźwiękiem PCM. Zwykłe
odtwarzacze i ffmpeg ich nie otworzą.

Klatki dekoduje przepisany na Pythona dekoder z silnika
(`src/flic/decoder.c`), więc wychodzą dokładnie takie jak w grze.
Zgodność z dekoderem silnika jest sprawdzona na losowych danych.
Wystarczy biblioteka standardowa Pythona 3.8+. ffmpeg jest potrzebny
tylko do `podglad.mp4`.

## Użycie

Najprościej: połóż `wacki_filmy.py` i `filmy.bat` obok plików gry
i kliknij dwa razy `filmy.bat`.

Albo z wiersza poleceń:

    python wacki_filmy.py --dane FOLDER_GRY [--wyjscie filmy] [--rgb]
                          [--tylko Dane_10 Dane_14] [--bez-podgladu]

| Opcja | Znaczenie |
|-------|-----------|
| `--dane` | Folder z plikami `Dane_*.dta`. |
| `--wyjscie` | Dokąd zapisać (domyślnie `filmy/` w folderze gry). |
| `--rgb` | Klatki jako PNG 24-bitowe zamiast z paletą. |
| `--tylko` | Tylko wybrane filmy, np. `Dane_10 Dane_14`. |
| `--bez-podgladu` | Bez `podglad.mp4`. |

## Co powstaje

Dla każdego filmu folder `filmy/Dane_NN/`:

| Plik | Co to |
|------|-------|
| `klatki/00000.png` … | Wszystkie klatki. |
| `dzwiek.wav` | Ścieżka dźwiękowa. |
| `Dane_NN.avi` | Kopia oryginału z rozszerzeniem `.avi`. |
| `info.txt` | Rozmiar, klatki na sekundę, liczba klatek, dźwięk i gotowe polecenie ffmpeg, które składa klatki z powrotem w film. |
| `podglad.mp4` | Film do obejrzenia (gdy jest ffmpeg). |

Gra odtwarza tylko oryginalny format (FLIC w AVI). Żeby wstawić do niej
poprawione filmy (np. powiększone albo w pełnych kolorach), trzeba dopisać
w silniku odtwarzanie innego formatu.
