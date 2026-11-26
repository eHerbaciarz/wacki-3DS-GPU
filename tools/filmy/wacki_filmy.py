#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""wacki_filmy.py — wyciąga filmy z Wacków do obróbki.

Filmy gry to pliki Dane_NN.dta: AVI z obrazem FLIC (Autodesk Animator,
256 kolorów, 640×480) i dźwiękiem PCM. Dla każdego filmu powstaje folder
filmy/Dane_NN/ z:

  klatki/00000.png ...  wszystkie klatki (PNG; --rgb = 24-bitowe zamiast
                        z paletą)
  dzwiek.wav            ścieżka dźwiękowa
  Dane_NN.avi           kopia oryginału z rozszerzeniem .avi
  info.txt              rozmiar, klatki/s, liczba klatek, dźwięk, polecenie
                        ffmpeg do złożenia klatek z powrotem w film
  podglad.mp4           (gdy jest ffmpeg) film do obejrzenia

Uruchomienie:
  dwuklik (Windows: filmy.bat) — szuka plików gry obok siebie / folder wyżej
  python wacki_filmy.py --dane FOLDER_GRY [--wyjscie filmy] [--rgb]
                        [--tylko Dane_10] [--bez-podgladu]

Tylko biblioteka standardowa Pythona (3.8+). Dekoder klatek to przepisany
na Pythona dekoder z silnika (src/flic/decoder.c), więc klatki są dokładnie
takie, jak w grze.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import wave
import zlib

FRAME_W, FRAME_H = 640, 480


def log(msg):
    print(msg, flush=True)


# --------------------------------------------------------------------------
# FLIC (jak src/flic/decoder.c)

CK_COLOR_256, CK_DELTA_FLC, CK_COLOR_64 = 4, 7, 11
CK_BLACK, CK_BRUN, CK_COPY = 13, 15, 16


class Flic:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = bytearray(w * h)
        self.pal = bytearray(768)

    def _color(self, d, shift):
        if len(d) < 2:
            return
        packets = d[0] | d[1] << 8
        p, idx = 2, 0
        while packets and p + 2 <= len(d):
            packets -= 1
            idx += d[p]
            n = d[p + 1] or 256
            p += 2
            for _ in range(n):
                if idx >= 256 or p + 3 > len(d):
                    break
                self.pal[idx * 3:idx * 3 + 3] = bytes(
                    (c << shift) & 0xFF for c in d[p:p + 3])
                idx += 1
                p += 3

    def _brun(self, d):
        w, px, end = self.w, self.px, len(d)
        p = 0
        for y in range(self.h):
            if p >= end:
                return
            p += 1                                   # liczba pakietów — bez znaczenia
            x, row = 0, y * w
            while x < w:
                if p >= end:
                    return
                n = d[p]
                p += 1
                if n < 128:                          # n powtórzeń bajtu
                    if p >= end:
                        return
                    k = min(n, w - x)
                    px[row + x:row + x + k] = bytes((d[p],)) * k
                    x += n
                    p += 1
                else:                                # 256-n bajtów dosłownie
                    # (jak w silniku: bajty za końcem wiersza nie są
                    # zjadane — liczy się tylko to, co weszło do wiersza)
                    cnt = min(256 - n, w - x)
                    lit = d[p:p + cnt]
                    px[row + x:row + x + len(lit)] = lit
                    x += len(lit)
                    p += len(lit)
                    if len(lit) < cnt:
                        return

    def _delta(self, d):
        w, h, px, end = self.w, self.h, self.px, len(d)
        if end < 2:
            return
        lines = d[0] | d[1] << 8
        p, y = 2, 0
        while lines > 0:
            if p + 2 > end:
                return
            op = d[p] | d[p + 1] << 8
            p += 2
            if op & 0xC000 == 0xC000:                # pomiń wiersze
                y += 0x10000 - op
                continue
            ok = 0 <= y < h
            if op & 0xC000 == 0x8000:                # ostatni bajt wiersza
                if ok:
                    px[y * w + w - 1] = op & 0xFF
                y += 1
                lines -= 1
                continue
            row, x = y * w, 0
            for _ in range(op):
                if p + 2 > end:
                    return
                x += d[p]
                n = d[p + 1]
                p += 2
                if n < 128:                          # n par dosłownie
                    seg = d[p:p + 2 * n]
                    full = len(seg) // 2 * 2
                    k = min(full, w - x)
                    if ok and k > 0:
                        px[row + x:row + x + k] = seg[:k]
                    x += full
                    p += full
                    if full < 2 * n:
                        return
                else:                                # para powtórzona 256-n razy
                    cnt = 256 - n
                    if p + 2 > end:
                        return
                    pair = d[p:p + 2]
                    p += 2
                    k = min(2 * cnt, w - x)
                    if ok and k > 0:
                        px[row + x:row + x + k] = (pair * cnt)[:k]
                    x += 2 * cnt
            y += 1
            lines -= 1

    def frame(self, f):
        if len(f) < 16 or (f[4] | f[5] << 8) != 0xF1FA:
            return
        chunks = f[6] | f[7] << 8
        p, end = 16, len(f)
        for _ in range(chunks):
            if p + 6 > end:
                break
            sz, typ = struct.unpack_from("<IH", f, p)
            if sz < 6:
                break
            body = f[p + 6:min(p + sz, end)]
            if typ == CK_COLOR_256:
                self._color(body, 0)
            elif typ == CK_COLOR_64:
                self._color(body, 2)
            elif typ == CK_BRUN:
                self._brun(body)
            elif typ == CK_DELTA_FLC:
                self._delta(body)
            elif typ == CK_BLACK:
                self.px[:] = bytes(len(self.px))
            elif typ == CK_COPY:
                n = min(len(body), len(self.px))
                self.px[:n] = body[:n]
            if sz > end - p:
                break
            p += sz


# --------------------------------------------------------------------------
# PNG (zlib z biblioteki standardowej)

def _chunk(typ, data):
    c = typ + data
    return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)


def png_bytes(w, h, px, pal):
    """PNG z paletą (8 bitów, typ 3)."""
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0)
    raw = b"".join(b"\x00" + bytes(px[y * w:(y + 1) * w]) for y in range(h))
    return (b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr) + _chunk(b"PLTE", bytes(pal)) +
            _chunk(b"IDAT", zlib.compress(raw, 6)) + _chunk(b"IEND", b""))


def png_rgb_bytes(w, h, px, pal):
    lut_r = bytes(pal[i * 3] for i in range(256))
    lut_g = bytes(pal[i * 3 + 1] for i in range(256))
    lut_b = bytes(pal[i * 3 + 2] for i in range(256))
    rows = []
    for y in range(h):
        row = bytes(px[y * w:(y + 1) * w])
        r, g, b = row.translate(lut_r), row.translate(lut_g), row.translate(lut_b)
        out = bytearray(3 * w)
        out[0::3], out[1::3], out[2::3] = r, g, b
        rows.append(b"\x00" + bytes(out))
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr) +
            _chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + _chunk(b"IEND", b""))


# --------------------------------------------------------------------------
# AVI

def czy_avi(path):
    try:
        with open(path, "rb") as f:
            h = f.read(12)
        return h[:4] == b"RIFF" and h[8:12] == b"AVI "
    except OSError:
        return False


def czytaj_avi(data):
    """→ (szer, wys, us_na_klatke, format_dzwieku, [klatki], bajty_dzwieku)"""
    info = {"w": 0, "h": 0, "us": 0, "audio": None}
    video, audio = [], bytearray()
    typy = []                                         # strh: 'vids'/'auds' w kolejności

    def walk(p, end, w_movi):
        while p + 8 <= end:
            tag, sz = data[p:p + 4], struct.unpack_from("<I", data, p + 4)[0]
            body, nxt = p + 8, p + 8 + sz + (sz & 1)
            if nxt > len(data) or nxt <= p:
                nxt = end
            if tag == b"LIST" or tag == b"RIFF":
                lt = data[body:body + 4]
                walk(body + 4, min(body + sz, end), w_movi or lt == b"movi")
            elif tag == b"avih" and sz >= 40:
                info["us"] = struct.unpack_from("<I", data, body)[0]
                info["w"], info["h"] = struct.unpack_from("<II", data, body + 32)
            elif tag == b"strh" and sz >= 4:
                typy.append(data[body:body + 4])
            elif tag == b"strf" and typy and typy[-1] == b"auds" and sz >= 16:
                fmt, ch, rate, _bps, _al, bits = struct.unpack_from("<HHIIHH", data, body)
                info["audio"] = (fmt, ch, rate, bits)
            elif w_movi and len(tag) == 4 and tag[:2].isdigit():
                strumien = int(tag[:2])
                rodzaj = typy[strumien] if strumien < len(typy) else b""
                kawalek = data[body:body + sz]
                if rodzaj == b"vids" or (not rodzaj and tag[2:] in (b"dc", b"db")):
                    video.append(kawalek)
                elif rodzaj == b"auds" or tag[2:] == b"wb":
                    audio.extend(kawalek)
            p = nxt

    walk(12, len(data), False)
    return info, video, bytes(audio)


# --------------------------------------------------------------------------

def wyciagnij(path, wyjscie, rgb, ffmpeg, podglad):
    nazwa = os.path.splitext(os.path.basename(path))[0]
    cel = os.path.join(wyjscie, nazwa)
    klatki_dir = os.path.join(cel, "klatki")
    os.makedirs(klatki_dir, exist_ok=True)
    with open(path, "rb") as f:
        data = f.read()
    info, video, audio = czytaj_avi(data)
    w = info["w"] or FRAME_W
    h = info["h"] or FRAME_H
    fps = 1e6 / info["us"] if info["us"] else 15.0
    if abs(fps - round(fps)) < 0.01:                # 66666 µs → 15, nie 15.00015
        fps = float(round(fps))
    log("%s: %dx%d, %.3f kl/s, %d klatek%s" % (
        nazwa, w, h, fps, len(video),
        ", dźwięk %d Hz %d-bit %s" % (info["audio"][2], info["audio"][3],
                                     "stereo" if info["audio"][1] == 2 else "mono")
        if info["audio"] else ", bez dźwięku"))

    shutil.copyfile(path, os.path.join(cel, nazwa + ".avi"))
    dec = Flic(w, h)
    for i, fr in enumerate(video):
        dec.frame(fr)
        png = png_rgb_bytes(w, h, dec.px, dec.pal) if rgb else png_bytes(w, h, dec.px, dec.pal)
        with open(os.path.join(klatki_dir, "%05d.png" % i), "wb") as f:
            f.write(png)
        if i % 100 == 99:
            log("  %d/%d" % (i + 1, len(video)))

    wav_path = None
    if audio and info["audio"] and info["audio"][0] == 1:     # PCM
        _fmt, ch, rate, bits = info["audio"]
        wav_path = os.path.join(cel, "dzwiek.wav")
        with wave.open(wav_path, "wb") as wv:
            wv.setnchannels(ch)
            wv.setsampwidth(max(1, bits // 8))
            wv.setframerate(rate)
            wv.writeframes(audio)
    elif audio:
        log("  dźwięk nie jest w PCM (format %d) — zostaje tylko w .avi" % info["audio"][0])

    polecenie = ('ffmpeg -framerate %.6f -i klatki/%%05d.png%s -c:v libx264 -pix_fmt yuv420p '
                 '-crf 16%s film.mp4' % (fps, " -i dzwiek.wav" if wav_path else "",
                                         " -c:a aac -b:a 192k -shortest" if wav_path else ""))
    with open(os.path.join(cel, "info.txt"), "w", encoding="utf-8") as f:
        f.write("plik: %s\nrozmiar: %dx%d\nklatki/s: %.6f\nklatek: %d\n" % (
            os.path.basename(path), w, h, fps, len(video)))
        if info["audio"]:
            f.write("dzwiek: format %d, %d kanal(y), %d Hz, %d bit\n" % info["audio"])
        f.write("\nZlozenie klatek z powrotem w film (w tym folderze):\n  %s\n" % polecenie)

    if ffmpeg and podglad and video:
        r = subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-framerate", "%.6f" % fps,
                            "-i", os.path.join(klatki_dir, "%05d.png")] +
                           (["-i", wav_path] if wav_path else []) +
                           ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18"] +
                           (["-c:a", "aac", "-b:a", "160k", "-shortest"] if wav_path else []) +
                           [os.path.join(cel, "podglad.mp4")],
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if r.returncode != 0:
            log("  ffmpeg: %s" % r.stderr.decode(errors="replace").strip()[:300])
    return len(video)


def znajdz_filmy(dane, tylko):
    wynik = []
    for f in sorted(os.listdir(dane)):
        if not f.lower().endswith(".dta"):
            continue
        if tylko and os.path.splitext(f)[0].lower() not in tylko:
            continue
        p = os.path.join(dane, f)
        if czy_avi(p):
            wynik.append(p)
    return wynik


def ma_dta(k):
    try:
        return any(f.lower().endswith(".dta") for f in os.listdir(k))
    except OSError:
        return False


def main(argv=None):
    ap = argparse.ArgumentParser(description="Wyciąga filmy z Wacków (Dane_NN.dta) do klatek PNG + WAV.")
    ap.add_argument("--dane", help="folder z plikami Dane_*.dta")
    ap.add_argument("--wyjscie", help="dokąd (domyślnie: filmy/ obok plików gry)")
    ap.add_argument("--rgb", action="store_true", help="PNG 24-bitowe zamiast z paletą")
    ap.add_argument("--tylko", nargs="*", help="np. Dane_10 Dane_14")
    ap.add_argument("--bez-podgladu", action="store_true", help="nie rób podglad.mp4")
    a = ap.parse_args(argv)

    dane = a.dane
    if not dane:
        tu = os.path.dirname(os.path.abspath(__file__))
        for k in (tu, os.path.dirname(tu), os.getcwd()):
            if ma_dta(k):
                dane = k
                break
    if not dane:
        print("Nie widzę plików Dane_*.dta obok skryptu ani folder wyżej.")
        dane = input("Podaj folder gry (albo Enter, żeby wyjść): ").strip().strip('"')
        if not dane:
            return 1
    wyjscie = a.wyjscie or os.path.join(dane, "filmy")
    tylko = [t.lower() for t in a.tylko] if a.tylko else None
    filmy = znajdz_filmy(dane, tylko)
    if not filmy:
        print("W %s nie ma filmów (plików Dane_*.dta z AVI w środku)." % dane)
        return 1
    ffmpeg = shutil.which("ffmpeg")
    log("Dane gry: %s\nFilmy: %d → %s%s" % (dane, len(filmy), wyjscie,
        "" if ffmpeg or a.bez_podgladu else "\n(bez ffmpeg: nie będzie podglad.mp4 — klatki i WAV są)"))
    razem = 0
    for p in filmy:
        try:
            razem += wyciagnij(p, wyjscie, a.rgb, ffmpeg, not a.bez_podgladu)
        except Exception as e:                       # jeden zły plik nie psuje reszty
            log("  błąd w %s: %r" % (os.path.basename(p), e))
    log("Gotowe: %d klatek w %s" % (razem, wyjscie))
    return 0


if __name__ == "__main__":
    dwuklik = len(sys.argv) == 1
    try:
        kod = main()
    except KeyboardInterrupt:
        kod = 1
    except Exception as e:
        print("Błąd: %r" % (e,))
        kod = 1
    if dwuklik:
        try:
            input("\nNaciśnij Enter, żeby zamknąć...")
        except EOFError:
            pass
    sys.exit(kod)
