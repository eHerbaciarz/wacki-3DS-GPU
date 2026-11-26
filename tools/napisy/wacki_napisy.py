#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Mateusz Szuła
"""
wacki_napisy.py — napisy do Wacków: dialogi (głos → tekst Whisperem),
opisy dźwięków dla niesłyszących, napisy do filmów, tłumaczenia.

Gra wyświetla je z pliku <dane gry>/napisy/<język>.txt
(src/text/subtitles.c; format opisany na górze tamtego pliku).

Polecenia (szczegóły: docs/napisy.md, albo `wacki_napisy.py POLECENIE -h`):

  wszystko    rozpakuj DTA, przepisz dialogi i filmy, zapisz plik napisów
  rozpakuj    DANE_*.DTA → katalog roboczy (narzędzie dta-extract z repo)
  przepisz    nagrania dialogów (i filmów) → tekst Whisperem (z pamięcią
              podręczną: drugi raz nie liczy tego samego)
  plik        złóż/uzupełnij napisy/<język>.txt (Twoje poprawki zostają)
  sprawdz     sprawdź plik napisów (taby, puste wpisy, za długie linie)
  tlumacz     szkic pliku w innym języku (opcjonalnie Argos Translate)

Tylko biblioteka standardowa Pythona (3.8+). Do przepisywania mowy jeden z:
  pip install faster-whisper          (zalecane, szybkie na CPU)
  pip install openai-whisper          (wymaga PyTorch)
  whisper.cpp: --whisper-cpp ŚCIEŻKA/whisper-cli --model-plik ggml-*.bin

Pliki gry (DTA, WAV, AVI) zostają u Ciebie: narzędzie zapisuje tylko
tekst. Plik napisów nie zawiera niczego z oryginału poza tym, co
przepisano ze słuchu — trzymaj go osobno od danych gry.
"""

import argparse
import array
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

WERSJA = 1
MOWCY_DOMYSLNIE = {"eb": "Ebek", "fj": "Fjej"}
PODPOWIEDZ_DOMYSLNA = "Wacki, Ebek, Fjej. Rozmowa z gry przygodowej."

# Typowe „halucynacje” Whispera na ciszy / szumie (szczególnie po polsku).
HALUCYNACJE = [
    "amara.org", "napisy stworzone", "napisy wykonane", "napisy przygotow",
    "dziękuję za uwagę", "dzięki za obejrzenie", "subskrybuj", "zasubskrybuj",
    "www.", "http", "tvn", "polsat", "transkrypcj",
]


# --------------------------------------------------------------------------
# wspólne


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def znajdz_pliki(katalog, rozszerzenia):
    """Wszystkie pliki o danych rozszerzeniach (bez względu na wielkość liter)."""
    wynik = []
    roz = tuple(r.lower() for r in rozszerzenia)
    for root, _dirs, files in os.walk(katalog):
        for f in files:
            if f.lower().endswith(roz):
                wynik.append(os.path.join(root, f))
    return sorted(wynik)


def mapa_plikow(katalogi, rozszerzenia):
    """nazwa_małymi → ścieżka (pierwsza znaleziona)."""
    m = {}
    for k in katalogi:
        if k and os.path.isdir(k):
            for p in znajdz_pliki(k, rozszerzenia):
                m.setdefault(os.path.basename(p).lower(), p)
    return m


# --------------------------------------------------------------------------
# Gadki.scr — ten sam podział co src/text/dialog.c


def czytaj_scr(sciezka):
    with open(sciezka, "rb") as f:
        surowe = f.read()
    for kod in ("cp1250", "latin-1"):
        try:
            return surowe.decode(kod)
        except UnicodeDecodeError:
            continue
    return surowe.decode("latin-1", "replace")


def rozmowy_z_gadek(tekst):
    """[(sekcja, wav, tag_mówcy, tekst_w_skrypcie)] w kolejności z pliku.

    Sekcja: `[rozmowa] nazwa`, do następnego [rozmowa] / [animacja].
    Linia: `[sampl] plik.wav`, potem opcjonalnie [eb]/[fj] tekst albo [nic].
    """
    wynik = []
    znaczniki = [m.start() for m in re.finditer(r"\[rozmowa\]", tekst)]
    for i, pocz in enumerate(znaczniki):
        reszta = tekst[pocz + len("[rozmowa]"):]
        m = re.match(r"\s*(\S+)", reszta)
        if not m:
            continue
        nazwa = m.group(1)
        cialo = reszta[m.end():]
        koniec = len(cialo)
        for tag in ("[rozmowa]", "[animacja]"):
            k = cialo.find(tag)
            if k >= 0:
                koniec = min(koniec, k)
        cialo = cialo[:koniec]
        bloki = cialo.split("[sampl]")[1:]
        for blok in bloki:
            mw = re.match(r"[ \t]*([^\s\[]+)", blok)
            if not mw:
                continue
            wav = mw.group(1).lower()
            tag, napis = "", ""
            # jak dialog_extract_line: [eb]/[fj] = mówca (+ tekst), [nic] = cisza,
            # inne znaczniki pomijane
            for mt in re.finditer(r"\[(\w+)\]([^\[\r\n]*)", blok[mw.end():]):
                t = mt.group(1).lower()
                if t == "nic":
                    break
                if t in ("eb", "fj"):
                    tag, napis = t, mt.group(2).strip()
                    break
            wynik.append((nazwa, wav, tag, napis))
    return wynik


def przedmioty_z_item_scr(tekst):
    """[(numer, wav)] z Item.scr: bloki `[N] plik.wav` (jak src/hud/items.c)."""
    wynik = []
    for m in re.finditer(r"\[(\d+)\][ \t\r\n]*([^\s\[]+)", tekst):
        wynik.append((int(m.group(1)), m.group(2).lower()))
    return wynik


def znajdz_item_scr(a):
    return mapa_plikow(katalogi_danych(a), (".scr",)).get("item.scr")


# --------------------------------------------------------------------------
# rozpakowywanie DTA


def znajdz_dta_extract(podany):
    kandydaci = []
    if podany:
        kandydaci.append(podany)
    tu = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.abspath(os.path.join(tu, "..", ".."))
    for nazwa in ("dta-extract", "dta-extract.exe"):
        kandydaci.append(os.path.join(tu, nazwa))            # obok skryptu
        kandydaci.append(os.path.join(repo, "dist", nazwa))
        w = shutil.which(nazwa)
        if w:
            kandydaci.append(w)
    for k in kandydaci:
        if k and os.path.isfile(k) and os.access(k, os.X_OK):
            return k
    return None


def cmd_rozpakuj(a):
    narzedzie = znajdz_dta_extract(a.dta_extract)
    if not narzedzie:
        sys.exit("Nie ma dta-extract. Zbuduj go w repo: `make` (powstaje dist/dta-extract)\n"
                 "albo podaj --dta-extract ŚCIEŻKA.")
    archiwa = znajdz_pliki(a.dane, (".dta",))
    if not archiwa:
        sys.exit("W %s nie ma plików *.dta" % a.dane)
    cel = os.path.join(a.praca, "dta")
    for arch in archiwa:
        podkat = os.path.join(cel, os.path.splitext(os.path.basename(arch))[0].lower())
        if os.path.isdir(podkat) and os.listdir(podkat) and not a.od_nowa:
            log("  %s: już rozpakowane" % os.path.basename(arch))
            continue
        os.makedirs(podkat, exist_ok=True)
        log("  rozpakowuję %s" % os.path.basename(arch))
        r = subprocess.run([narzedzie, arch, podkat], stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE)
        if r.returncode != 0:
            log("    błąd: %s" % r.stderr.decode(errors="replace").strip())
    log("Rozpakowane do %s" % cel)


# --------------------------------------------------------------------------
# audio: WAV → 16 kHz mono 16-bit (dla whisper.cpp); dźwięk z AVI


def wav_na_16k(wej, wyj):
    with wave.open(wej, "rb") as w:
        kan, szer, czest, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        dane = w.readframes(n)
    probki = pcm_na_float(dane, szer, kan)
    zapisz_16k(probki, czest, wyj)


def pcm_na_float(dane, szer, kan):
    if szer == 1:
        surowe = [(b - 128) / 128.0 for b in dane]
    elif szer == 2:
        a = array.array("h")
        a.frombytes(dane[: len(dane) // 2 * 2])
        if sys.byteorder == "big":
            a.byteswap()
        surowe = [v / 32768.0 for v in a]
    else:
        raise ValueError("nieobsługiwana szerokość próbki: %d B" % szer)
    if kan > 1:
        surowe = [sum(surowe[i:i + kan]) / kan for i in range(0, len(surowe) - kan + 1, kan)]
    return surowe


def zapisz_16k(probki, czest, wyj):
    if not probki:
        probki = [0.0]
    krok = czest / 16000.0
    n_wyj = max(1, int(len(probki) / krok))
    out = array.array("h")
    for i in range(n_wyj):
        x = i * krok
        j = int(x)
        f = x - j
        a = probki[j]
        b = probki[j + 1] if j + 1 < len(probki) else a
        v = a + (b - a) * f
        out.append(max(-32768, min(32767, int(v * 32767))))
    if sys.byteorder == "big":
        out.byteswap()
    with wave.open(wyj, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(out.tobytes())


def avi_audio_na_wav(avi, wyj):
    """Dźwięk PCM z AVI (strumień 'auds', bloki ##wb) → WAV 16 kHz.
    Zwraca False, gdy dźwięk nie jest PCM (wtedy próbujemy ffmpeg)."""
    with open(avi, "rb") as f:
        d = f.read()
    if d[:4] != b"RIFF" or d[8:12] != b"AVI ":
        return False
    fmt, numer, strumien, kawalki = None, None, 0, []

    def chodz(pocz, kon):
        nonlocal fmt, numer, strumien
        p = pocz
        while p + 8 <= kon:
            fourcc = d[p:p + 4]
            rozm = struct.unpack("<I", d[p + 4:p + 8])[0]
            tresc = p + 8
            if fourcc in (b"RIFF", b"LIST"):
                typ = d[tresc:tresc + 4]
                if typ == b"strl":
                    strumien_tu = strumien
                    strumien += 1
                    q, qk = tresc + 4, tresc + rozm
                    typ_strumienia = None
                    while q + 8 <= qk:
                        cc = d[q:q + 4]
                        rr = struct.unpack("<I", d[q + 4:q + 8])[0]
                        if cc == b"strh":
                            typ_strumienia = d[q + 8:q + 12]
                        elif cc == b"strf" and typ_strumienia == b"auds" and fmt is None:
                            fmt = struct.unpack("<HHIIHH", d[q + 8:q + 24])
                            numer = strumien_tu
                        q += 8 + rr + (rr & 1)
                else:
                    chodz(tresc + 4, tresc + rozm)
            elif numer is not None and fourcc[2:] == b"wb" and fourcc[:2] == b"%02d" % numer:
                kawalki.append(d[tresc:tresc + rozm])
            p = tresc + rozm + (rozm & 1)

    chodz(12, len(d))
    if not fmt or fmt[0] != 1 or not kawalki:
        return False
    _tag, kan, czest, _bps, _align, bity = fmt
    probki = pcm_na_float(b"".join(kawalki), bity // 8, kan)
    zapisz_16k(probki, czest, wyj)
    return True


def film_na_wav(avi, wyj):
    try:
        if avi_audio_na_wav(avi, wyj):
            return True
    except (struct.error, ValueError, IndexError) as e:
        log("    (AVI: %s)" % e)
    if shutil.which("ffmpeg"):
        r = subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", avi, "-vn", "-ac", "1",
                            "-ar", "16000", "-acodec", "pcm_s16le", wyj])
        return r.returncode == 0
    log("    %s: dźwięk nie jest PCM, a nie ma ffmpeg — pomijam" % os.path.basename(avi))
    return False


# --------------------------------------------------------------------------
# Whisper


class Silnik:
    """Wspólny interfejs: przepisz(ścieżka_wav) → (tekst, [segmenty], pewność_ok)."""

    def __init__(self, a):
        self.a = a
        self.nazwa = None
        self.model = None
        wybor = a.silnik
        if wybor in ("auto", "faster-whisper"):
            try:
                from faster_whisper import WhisperModel  # noqa
                self.model = WhisperModel(a.model, device="auto", compute_type="auto")
                self.nazwa = "faster-whisper"
            except ImportError:
                if wybor != "auto":
                    sys.exit("Brak faster-whisper: pip install faster-whisper")
        if not self.nazwa and wybor in ("auto", "whisper"):
            try:
                import whisper  # noqa
                self.model = whisper.load_model(a.model)
                self.nazwa = "whisper"
            except ImportError:
                if wybor != "auto":
                    sys.exit("Brak openai-whisper: pip install openai-whisper")
        if not self.nazwa and (wybor in ("auto", "whisper.cpp")) and a.whisper_cpp:
            if not a.model_plik:
                sys.exit("whisper.cpp potrzebuje --model-plik (np. ggml-medium.bin)")
            self.nazwa = "whisper.cpp"
        if not self.nazwa and wybor == "atrapa":
            self.nazwa = "atrapa"      # do testów: tekst = nazwa pliku
        if not self.nazwa:
            sys.exit("Nie ma żadnego Whispera. Zainstaluj jeden:\n"
                     "  pip install faster-whisper\n"
                     "albo podaj --whisper-cpp i --model-plik.")
        log("Silnik: %s, model: %s" % (self.nazwa, a.model_plik or a.model))

    def przepisz(self, wav, podpowiedz):
        a = self.a
        if self.nazwa == "atrapa":
            return os.path.splitext(os.path.basename(wav))[0], [], True
        if self.nazwa == "faster-whisper":
            segs, _info = self.model.transcribe(wav, language=a.jezyk_mowy, beam_size=5,
                                                initial_prompt=podpowiedz,
                                                condition_on_previous_text=False)
            segs = list(segs)
            ok = all(s.no_speech_prob < 0.6 and s.avg_logprob > -1.0 for s in segs)
            lista = [(s.start, s.end, s.text.strip()) for s in segs]
            return " ".join(t for _s, _e, t in lista).strip(), lista, ok
        if self.nazwa == "whisper":
            # openai-whisper czyta pliki przez ffmpeg; podajemy mu gotowe próbki
            import numpy as np
            with tempfile.TemporaryDirectory() as tmp:
                w16 = os.path.join(tmp, "a.wav")
                wav_na_16k(wav, w16)
                with wave.open(w16, "rb") as w:
                    pcm = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
            audio = pcm.astype(np.float32) / 32768.0
            r = self.model.transcribe(audio, language=a.jezyk_mowy, initial_prompt=podpowiedz,
                                      fp16=False, condition_on_previous_text=False)
            segs = r.get("segments", [])
            ok = all(s.get("no_speech_prob", 0) < 0.6 and s.get("avg_logprob", 0) > -1.0
                     for s in segs)
            lista = [(s["start"], s["end"], s["text"].strip()) for s in segs]
            return r.get("text", "").strip(), lista, ok
        # whisper.cpp: wymaga 16 kHz mono
        with tempfile.TemporaryDirectory() as tmp:
            w16 = os.path.join(tmp, "a.wav")
            wav_na_16k(wav, w16)
            baza = os.path.join(tmp, "wynik")
            cmd = [a.whisper_cpp, "-m", a.model_plik, "-l", a.jezyk_mowy, "-f", w16,
                   "-oj", "-of", baza, "-np"]
            if podpowiedz:
                cmd += ["--prompt", podpowiedz]
            r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            if r.returncode != 0 or not os.path.exists(baza + ".json"):
                log("    whisper.cpp: %s" % r.stderr.decode(errors="replace")[-300:])
                return "", [], False
            with open(baza + ".json", encoding="utf-8") as f:
                j = json.load(f)
        lista = []
        for s in j.get("transcription", []):
            off = s.get("offsets", {})
            lista.append((off.get("from", 0) / 1000.0, off.get("to", 0) / 1000.0,
                          s.get("text", "").strip()))
        return " ".join(t for _s, _e, t in lista).strip(), lista, True


def podejrzane(tekst):
    t = tekst.lower()
    return any(h in t for h in HALUCYNACJE)


def wczytaj_cache(praca):
    p = os.path.join(praca, "przepisane.json")
    if os.path.exists(p):
        with open(p, encoding="utf-8") as f:
            return json.load(f)
    return {"dialogi": {}, "filmy": {}}


def zapisz_cache(praca, c):
    p = os.path.join(praca, "przepisane.json")
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(c, f, ensure_ascii=False, indent=1)
    os.replace(tmp, p)


def katalogi_danych(a):
    return [os.path.join(a.praca, "dta"), a.dane, getattr(a, "rozpakowane", None)]


def znajdz_gadki(a):
    m = mapa_plikow(katalogi_danych(a), (".scr",))
    p = m.get("gadki.scr")
    if not p:
        sys.exit("Nie znalazłem Gadki.scr (najpierw `rozpakuj`, albo podaj --rozpakowane).")
    return p


def cmd_przepisz(a):
    os.makedirs(a.praca, exist_ok=True)
    linie = rozmowy_z_gadek(czytaj_scr(znajdz_gadki(a)))
    wavy = mapa_plikow(katalogi_danych(a), (".wav",))
    cache = wczytaj_cache(a.praca)
    silnik = None
    podp = a.podpowiedz
    zrobione, brak = 0, 0
    widziane = set()
    for _sek, wav, _tag, napis_w_skrypcie in linie:
        if wav in widziane:
            continue
        widziane.add(wav)
        if napis_w_skrypcie:
            continue                      # tekst już jest w Gadki.scr
        if wav in cache["dialogi"] and not a.od_nowa:
            continue
        sciezka = wavy.get(wav)
        if not sciezka:
            brak += 1
            continue
        if silnik is None:
            silnik = Silnik(a)
        tekst, _segs, ok = silnik.przepisz(sciezka, podp)
        cache["dialogi"][wav] = {"tekst": tekst, "pewne": bool(ok and not podejrzane(tekst)),
                                 "silnik": silnik.nazwa}
        zrobione += 1
        if zrobione % 10 == 0:
            zapisz_cache(a.praca, cache)
            log("  %d nagrań…" % zrobione)
    zapisz_cache(a.praca, cache)
    log("Dialogi: przepisane %d, brak pliku WAV dla %d, razem w Gadki.scr %d."
        % (zrobione, brak, len(widziane)))

    # nazwy przedmiotów (Item.scr): krótkie nagrania, jedno-dwa słowa
    item = znajdz_item_scr(a)
    cache.setdefault("przedmioty", {})
    if item:
        nowe = 0
        for _nr, wav in przedmioty_z_item_scr(czytaj_scr(item)):
            if wav in cache["przedmioty"] and not a.od_nowa:
                continue
            sciezka = wavy.get(wav)
            if not sciezka:
                continue
            if silnik is None:
                silnik = Silnik(a)
            tekst, _segs, ok = silnik.przepisz(sciezka, "Nazwa przedmiotu w grze.")
            tekst = tekst.strip().rstrip(".!").strip()
            cache["przedmioty"][wav] = {"tekst": tekst,
                                        "pewne": bool(ok and not podejrzane(tekst))}
            nowe += 1
        zapisz_cache(a.praca, cache)
        log("Przedmioty: przepisane %d (razem %d)." % (nowe, len(cache["przedmioty"])))
    else:
        log("Przedmioty: nie ma Item.scr — pomijam.")

    if a.filmy:
        avi = mapa_plikow([a.dane, getattr(a, "rozpakowane", None)], (".avi",))
        for nazwa, sciezka in sorted(avi.items()):
            if nazwa in cache["filmy"] and not a.od_nowa:
                continue
            log("  film %s" % nazwa)
            with tempfile.TemporaryDirectory() as tmp:
                w16 = os.path.join(tmp, "film.16k.wav")
                if not film_na_wav(sciezka, w16):
                    continue
                if silnik is None:
                    silnik = Silnik(a)
                _t, segs, ok = silnik.przepisz(w16, podp)
            cache["filmy"][nazwa] = {
                "segmenty": [[round(s, 2), round(e, 2), t] for s, e, t in segs if t],
                "pewne": bool(ok)}
            zapisz_cache(a.praca, cache)
        log("Filmy: %d w pamięci." % len(cache["filmy"]))


# --------------------------------------------------------------------------
# plik napisów


def czytaj_napisy(sciezka):
    """Słowniki z istniejącego pliku: dialogi {wav: (mówca, tekst)},
    dźwięki {wav: tekst}, filmy {film: [(t0, t1, tekst)]}, ustawienia."""
    dlg, snd, film, ust, zle = {}, {}, {}, {}, []
    itm = {}
    czytaj_napisy.przedmioty = itm
    if not os.path.exists(sciezka):
        return dlg, snd, film, ust, zle
    sekcja, biezacy = None, None
    with open(sciezka, encoding="utf-8-sig") as f:
        for nr, linia in enumerate(f, 1):
            linia = linia.rstrip("\r\n")
            s = linia.strip(" ")
            if not s or s.startswith("#"):
                continue
            if s.startswith("@") and "=" in s:
                k, v = s[1:].split("=", 1)
                ust[k.strip()] = v.strip()
                continue
            if s.startswith("["):
                if s == "[dialogi]":
                    sekcja = "d"
                elif s == "[dzwieki]":
                    sekcja = "s"
                elif s == "[przedmioty]":
                    sekcja = "p"
                elif s.startswith("[film ") and s.endswith("]"):
                    sekcja, biezacy = "f", s[6:-1].strip().lower()
                    film.setdefault(biezacy, [])
                else:
                    sekcja = None
                    zle.append((nr, "nieznana sekcja"))
                continue
            pola = [p.strip(" ") for p in s.split("\t")]
            if sekcja == "d" and len(pola) >= 2:
                if len(pola) == 2:
                    dlg[pola[0].lower()] = ("", pola[1])
                else:
                    dlg[pola[0].lower()] = (pola[1], "\t".join(pola[2:]).strip())
            elif sekcja == "s" and len(pola) >= 2:
                snd[pola[0].lower()] = "\t".join(pola[1:]).strip()
            elif sekcja == "p" and len(pola) >= 2:
                itm[pola[0].lower()] = "\t".join(pola[1:]).strip()
            elif sekcja == "f" and len(pola) >= 3:
                film[biezacy].append((pola[0], pola[1], "\t".join(pola[2:]).strip()))
            else:
                zle.append((nr, "brak tabulatorów między polami?"))
    return dlg, snd, film, ust, zle


def czas(t):
    t = max(0.0, float(t))
    m, s = divmod(t, 60.0)
    return "%02d:%06.3f" % (int(m), s)


def cmd_plik(a):
    linie = rozmowy_z_gadek(czytaj_scr(znajdz_gadki(a)))
    cache = wczytaj_cache(a.praca)
    mowcy = dict(MOWCY_DOMYSLNIE)
    for para in (a.mowcy or "").split(","):
        if "=" in para:
            k, v = para.split("=", 1)
            mowcy[k.strip().lower()] = v.strip()

    wyj = a.wyjscie or os.path.join(a.dane, "napisy", a.jezyk + ".txt")
    stare_d, stare_s, stare_f, stare_u, _ = czytaj_napisy(wyj)

    o = []
    o.append("# Wacki — napisy (%s). Format: src/text/subtitles.c, docs/napisy.md" % a.jezyk)
    o.append("# Pola oddziela TABULATOR. „|” = nowa linia. Pusty tekst = brak napisu.")
    o.append("# Linie „# SPRAWDŹ” oznaczają niepewne rozpoznanie mowy.")
    o.append("@wersja=%d" % WERSJA)
    o.append("@jezyk=%s" % a.jezyk)
    o.append("@mowcy=%s" % stare_u.get("mowcy", "1" if a.sdh else "0"))
    o.append("@dzwieki=%s" % stare_u.get("dzwieki", "1" if a.sdh else "0"))
    o.append("")
    o.append("[dialogi]")
    widziane, sekcja_poprz = set(), None
    licz = {"stare": 0, "nowe": 0, "puste": 0, "sprawdz": 0}
    for sek, wav, tag, napis in linie:
        if wav in widziane:
            continue
        widziane.add(wav)
        if sek != sekcja_poprz:
            o.append("# --- rozmowa %s ---" % sek)
            sekcja_poprz = sek
        mowca = mowcy.get(tag, tag)
        if wav in stare_d and stare_d[wav][1]:
            m, t = stare_d[wav]
            o.append("%s\t%s\t%s" % (wav, m or mowca, t))
            licz["stare"] += 1
            continue
        t, pewne = napis, True
        if not t and wav in cache["dialogi"]:
            t = cache["dialogi"][wav]["tekst"]
            pewne = cache["dialogi"][wav]["pewne"]
        if t and not pewne:
            o.append("# SPRAWDŹ (niepewne rozpoznanie)")
            licz["sprawdz"] += 1
        if not t:
            licz["puste"] += 1
        else:
            licz["nowe"] += 1
        o.append("%s\t%s\t%s" % (wav, mowca, t))

    stare_p = getattr(czytaj_napisy, "przedmioty", {})
    item = znajdz_item_scr(a)
    przedm = przedmioty_z_item_scr(czytaj_scr(item)) if item else []
    o.append("")
    o.append("[przedmioty]")
    o.append("# Nazwy przedmiotów (Item.scr): widać je nad panelem po najechaniu")
    wav_przedm = set()
    for nr, wav in przedm:
        if wav in wav_przedm:
            continue
        wav_przedm.add(wav)
        t = stare_p.get(wav, "")
        if not t:
            c = cache.get("przedmioty", {}).get(wav)
            if c:
                t = c["tekst"]
                if t and not c.get("pewne", True):
                    o.append("# SPRAWDŹ (niepewne rozpoznanie)")
        o.append("%s\t%s" % (wav, t))
    for wav in sorted(set(stare_p) - wav_przedm):
        o.append("%s\t%s" % (wav, stare_p[wav]))

    o.append("")
    o.append("[dzwieki]")
    o.append("# Opisy dźwięków dla niesłyszących, np.: wybuch.wav<TAB>[wybuch]")
    wszystkie = mapa_plikow(katalogi_danych(a), (".wav",))
    for wav in sorted(set(wszystkie) - widziane - wav_przedm):
        o.append("%s\t%s" % (wav, stare_s.get(wav, "")))
    for wav in sorted(set(stare_s) - set(wszystkie)):
        o.append("%s\t%s" % (wav, stare_s[wav]))

    filmy = dict((k, v) for k, v in cache.get("filmy", {}).items())
    for nazwa in sorted(set(filmy) | set(stare_f)):
        o.append("")
        o.append("[film %s]" % nazwa)
        if stare_f.get(nazwa):
            for t0, t1, t in stare_f[nazwa]:
                o.append("%s\t%s\t%s" % (t0, t1, t))
            continue
        if nazwa not in filmy:
            continue
        if not filmy[nazwa].get("pewne", True):
            o.append("# SPRAWDŹ (niepewne rozpoznanie)")
        for s, e, t in filmy[nazwa]["segmenty"]:
            o.append("%s\t%s\t%s" % (czas(s), czas(e), t))

    os.makedirs(os.path.dirname(os.path.abspath(wyj)), exist_ok=True)
    with open(wyj, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(o) + "\n")
    log("Zapisano %s: dialogi %d zachowane, %d nowe, %d bez tekstu, %d do sprawdzenia."
        % (wyj, licz["stare"], licz["nowe"], licz["puste"], licz["sprawdz"]))


# --------------------------------------------------------------------------
# sprawdzanie i tłumaczenie


def cmd_sprawdz(a):
    dlg, snd, film, ust, zle = czytaj_napisy(a.plik)
    problemy = 0
    for nr, opis in zle:
        print("linia %d: %s" % (nr, opis))
        problemy += 1
    puste = [w for w, (_m, t) in dlg.items() if not t]
    dlugie = [(w, t) for w, (_m, t) in dlg.items()
              if max((len(x) for x in t.split("|")), default=0) > 150]
    for w, t in dlugie:
        print("%s: bardzo długi tekst (%d znaków) — na ekran wejdą 3 linie" % (w, len(t)))
    znaki = set()
    dozwolone = set("ąćęłńóśźżĄĆĘŁŃÓŚŹŻ")
    for _w, (_m, t) in dlg.items():
        for c in t:
            if ord(c) > 0x7E and c not in dozwolone and c not in "„”“’‘–—…«» ":
                znaki.add(c)
    if znaki:
        print("Znaki spoza fontu gry (pokażą się bez ogonków albo jako ?): %s"
              % " ".join(sorted(znaki)))
    for nazwa, cues in film.items():
        for t0, t1, _t in cues:
            if not re.match(r"^[\d:.,]+$", t0) or not re.match(r"^[\d:.,]+$", t1):
                print("film %s: zły czas %r–%r" % (nazwa, t0, t1))
                problemy += 1
    itm = getattr(czytaj_napisy, "przedmioty", {})
    print("Dialogi: %d (puste %d), przedmioty z nazwą: %d/%d, dźwięki z opisem: %d, filmy: %d, "
          "mówcy=%s, dźwięki=%s"
          % (len(dlg), len(puste), sum(1 for v in itm.values() if v), len(itm),
             sum(1 for v in snd.values() if v), len(film),
             ust.get("mowcy", "0"), ust.get("dzwieki", "1")))
    sys.exit(1 if problemy else 0)


def cmd_komnaty(a):
    """Wypisz etapy i komnaty z Wacky.scr — do słów „@” w podpowiedzi/*.txt."""
    katalogi = [a.rozpakowane] if a.rozpakowane else []
    if a.dane:
        katalogi += [os.path.join(a.praca, "dta"), a.dane]
    sciezka = None
    for k in katalogi:
        if not k or not os.path.isdir(k):
            continue
        for root, _d, files in os.walk(k):
            for f in files:
                if f.lower() == "wacky.scr":
                    sciezka = os.path.join(root, f)
                    break
            if sciezka:
                break
        if sciezka:
            break
    if not sciezka:
        sys.exit("Nie znalazłem Wacky.scr. Najpierw: rozpakuj --dane KATALOG_GRY")
    tekst = open(sciezka, "rb").read().decode("latin-1")
    etap, komnata, wynik = None, None, []
    for m in re.finditer(r"\[(etap|komnata)\]\s*([^\s\[\]]+)|([A-Za-z0-9_\-]+\.pic)", tekst, re.I):
        if m.group(1) and m.group(1).lower() == "etap":
            etap, komnata = m.group(2), None
        elif m.group(1):
            komnata = m.group(2)
            wynik.append([etap, komnata, []])
        elif m.group(3) and wynik and wynik[-1][1] == komnata and komnata:
            pic = m.group(3)
            if pic.lower() not in [x.lower() for x in wynik[-1][2]]:
                wynik[-1][2].append(pic)
    for etap, komnata, pics in wynik:
        print("etap %s\t%s\t%s" % (etap, komnata, ", ".join(pics[:4])))
    log("%d komnat z %s" % (len(wynik), sciezka))


def cmd_tlumacz(a):
    zrodlo = a.plik
    jez_z = a.z or "pl"
    wyj = a.wyjscie or os.path.join(os.path.dirname(zrodlo), a.na + ".txt")
    tlumacz = None
    if a.argos:
        try:
            import argostranslate.translate as at  # noqa
            tlumacz = lambda t: at.translate(t, jez_z, a.na)  # noqa: E731
        except ImportError:
            sys.exit("Brak Argos Translate: pip install argostranslate (i paczka %s→%s)"
                     % (jez_z, a.na))
    stare = czytaj_napisy(wyj)[0] if os.path.exists(wyj) else {}
    stare_p = dict(getattr(czytaj_napisy, "przedmioty", {})) if os.path.exists(wyj) else {}
    o = []
    with open(zrodlo, encoding="utf-8-sig") as f:
        sekcja = None
        for linia in f:
            linia = linia.rstrip("\r\n")
            s = linia.strip(" ")
            if s.startswith("@jezyk="):
                o.append("@jezyk=%s" % a.na)
                continue
            if not s or s.startswith("#") or s.startswith("@"):
                o.append(linia)
                continue
            if s.startswith("["):
                sekcja = s
                o.append(linia)
                continue
            pola = s.split("\t")
            tekst_idx = {"[dialogi]": 2, "[dzwieki]": 1, "[przedmioty]": 1}.get(sekcja, 2)
            if len(pola) <= tekst_idx or not pola[tekst_idx].strip():
                o.append(linia)
                continue
            oryg = pola[tekst_idx]
            if sekcja == "[dialogi]" and pola[0].lower() in stare and stare[pola[0].lower()][1]:
                pola[tekst_idx] = stare[pola[0].lower()][1]
            elif sekcja == "[przedmioty]" and stare_p.get(pola[0].lower()):
                pola[tekst_idx] = stare_p[pola[0].lower()]
            elif tlumacz:
                pola[tekst_idx] = " | ".join(tlumacz(x.strip()) for x in oryg.split("|"))
            o.append("# %s: %s" % (jez_z, oryg))
            o.append("\t".join(pola))
    with open(wyj, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(o) + "\n")
    log("Zapisano %s (%s)." % (wyj, "przetłumaczone Argosem — do korekty" if tlumacz
                                 else "tekst oryginału do podmiany; linie „# %s:” to oryginał" % jez_z))
    log("Font gry ma tylko polskie ogonki: inne litery z akcentem pokażą się bez akcentu.")


# --------------------------------------------------------------------------


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                formatter_class=argparse.RawDescriptionHelpFormatter,
                                epilog="Szczegóły: docs/napisy.md")
    sub = p.add_subparsers(dest="cmd", required=True)

    def wspolne(sp, whisper=False):
        sp.add_argument("--dane", required=True, help="katalog z DANE_*.DTA (dane gry)")
        sp.add_argument("--praca", default="napisy_praca", help="katalog roboczy")
        sp.add_argument("--rozpakowane", help="katalog z już rozpakowanymi plikami gry")
        if whisper:
            sp.add_argument("--silnik", default="auto",
                            choices=["auto", "faster-whisper", "whisper", "whisper.cpp", "atrapa"])
            sp.add_argument("--model", default="medium",
                            help="model Whispera (small/medium/large-v3); medium = dobry kompromis")
            sp.add_argument("--whisper-cpp", help="ścieżka do whisper-cli (whisper.cpp)")
            sp.add_argument("--model-plik", help="model ggml dla whisper.cpp")
            sp.add_argument("--jezyk-mowy", default="pl")
            sp.add_argument("--podpowiedz", default=PODPOWIEDZ_DOMYSLNA,
                            help="słowa, które Whisper ma znać (imiona postaci)")
            sp.add_argument("--filmy", action="store_true", help="przepisz też filmy *.avi")
            sp.add_argument("--od-nowa", action="store_true", help="nie używaj pamięci podręcznej")

    def plikowe(sp):
        sp.add_argument("--jezyk", default="pl")
        sp.add_argument("--wyjscie", help="plik napisów (domyślnie DANE/napisy/JEZYK.txt)")
        sp.add_argument("--sdh", action="store_true",
                        help="dla niesłyszących: imiona mówców i opisy dźwięków włączone")
        sp.add_argument("--mowcy", help="tagi mówców, np. eb=Ebek,fj=Fjej")

    sp = sub.add_parser("rozpakuj", help="DANE_*.DTA → katalog roboczy")
    wspolne(sp)
    sp.add_argument("--dta-extract")
    sp.add_argument("--od-nowa", action="store_true")
    sp.set_defaults(fn=cmd_rozpakuj)

    sp = sub.add_parser("przepisz", help="nagrania → tekst (Whisper)")
    wspolne(sp, whisper=True)
    sp.set_defaults(fn=cmd_przepisz)

    sp = sub.add_parser("plik", help="złóż/uzupełnij plik napisów")
    wspolne(sp)
    plikowe(sp)
    sp.set_defaults(fn=cmd_plik)

    sp = sub.add_parser("wszystko", help="rozpakuj + przepisz + plik")
    wspolne(sp, whisper=True)
    plikowe(sp)
    sp.add_argument("--dta-extract")
    sp.set_defaults(fn=lambda a: (cmd_rozpakuj(a), cmd_przepisz(a), cmd_plik(a)))

    sp = sub.add_parser("sprawdz", help="sprawdź plik napisów")
    sp.add_argument("plik")
    sp.set_defaults(fn=cmd_sprawdz)

    sp = sub.add_parser("tlumacz", help="szkic napisów w innym języku")
    sp.add_argument("plik")
    sp.add_argument("--na", required=True, help="kod języka docelowego, np. en, de, cs")
    sp.add_argument("--z", default="pl")
    sp.add_argument("--wyjscie")
    sp.add_argument("--argos", action="store_true", help="przetłumacz maszynowo (offline)")
    sp.set_defaults(fn=cmd_tlumacz)

    sp = sub.add_parser("komnaty", help="lista etapów i komnat (do podpowiedzi)")
    sp.add_argument("--dane", help="katalog z DANE_*.DTA (po „rozpakuj”)")
    sp.add_argument("--praca", default="napisy_praca")
    sp.add_argument("--rozpakowane", help="katalog z już rozpakowanymi plikami gry")
    sp.set_defaults(fn=cmd_komnaty)

    a = p.parse_args(argv)
    a.fn(a)


def tryb_dwuklik():
    """Uruchomiony bez argumentów (dwuklik w Windowsie): sam znajdzie dane
    gry obok skryptu (albo folder wyżej), rozpakuje je i zapisze listę
    komnat do komnaty.txt — z pauzą, żeby okno nie znikło."""
    tu = os.path.dirname(os.path.abspath(__file__))
    try:
        dane = None
        for kand in (tu, os.path.dirname(tu), os.getcwd()):
            if znajdz_pliki_plytko(kand, (".dta",)):
                dane = kand
                break
        if not dane:
            print("Nie widzę plików Dane_*.dta obok skryptu ani folder wyżej.")
            wpis = input("Podaj folder gry (albo Enter, żeby wyjść): ").strip().strip('"')
            if not wpis:
                return
            dane = wpis
        print("Dane gry: %s" % dane)
        praca = os.path.join(tu, "napisy_praca")
        cmd_rozpakuj(argparse.Namespace(dane=dane, praca=praca, dta_extract=None,
                                        od_nowa=False))
        wyj = os.path.join(tu, "komnaty.txt")
        import io
        bufor = io.StringIO()
        stary, sys.stdout = sys.stdout, bufor
        try:
            cmd_komnaty(argparse.Namespace(dane=dane, praca=praca, rozpakowane=None))
        finally:
            sys.stdout = stary
        with open(wyj, "w", encoding="utf-8") as f:
            f.write(bufor.getvalue())
        print(bufor.getvalue())
        print("Zapisane do: %s  — ten plik odeślij." % wyj)
    except SystemExit as e:
        if e.code not in (None, 0):
            print("Błąd: %s" % e.code)
    except Exception as e:                       # pokaż, nie zamykaj okna
        print("Błąd: %r" % (e,))
    finally:
        try:
            input("\nNaciśnij Enter, żeby zamknąć...")
        except EOFError:
            pass


def znajdz_pliki_plytko(katalog, rozszerzenia):
    try:
        return [f for f in os.listdir(katalog) if f.lower().endswith(rozszerzenia)]
    except OSError:
        return []


if __name__ == "__main__":
    if len(sys.argv) == 1:
        tryb_dwuklik()
    else:
        main()
