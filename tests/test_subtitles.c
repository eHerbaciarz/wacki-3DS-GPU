/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * tests/test_subtitles.c — napisy (src/text/subtitles.c): file parser,
 * UTF-8 → game-font bytes, dialogue/sound/film lookups and the caption
 * drawn on a copy of the frame. */

#include "test.h"
#include "wacki.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t     g_subtitles_on;
extern int         g_cutscene_playing;
extern FontHandle *g_default_font;
extern int         g_subtitles_speakers;
extern int         g_subtitles_sounds;

static const char k_file[] =
    "\xEF\xBB\xBF# test\n"
    "@jezyk=pl\r\n"
    "@mowcy=1\n"
    "[dialogi]\n"
    "FJGUT04A.WAV\tFjej\tZa\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 g\xC4\x99\xC5\x9Bl\xC4\x85 ja\xC5\xBA\xC5\x84\n"
    "fjgut04b.wav\t\tBez m\xC3\xB3wcy\n"
    "ebgut01a.wav\tEbek\t\n"                    /* empty text → ignored */
    "zly wiersz bez tabow\n"                    /* not understood */
    "[dzwieki]\n"
    "wybuch.wav\t[wybuch]\n"
    "[film intro.avi]\n"
    "00:01.200\t00:03.500\t\xE2\x80\x9EPierwszy\xE2\x80\x9D\n"
    "4\t5,5\tDrugi\n"
    "9\t8\tz\xC5\x82y czas\n";                  /* end before start */

TEST(parse_counts_entries)
{
    int n = SubtitlesLoadBuffer(k_file, sizeof k_file - 1);
    ASSERT_EQ(n, 5);            /* 2 dialogue + 1 sound + 2 film cues */
}

TEST(utf8_to_font_polish_and_fallbacks)
{
    char out[64];
    /* ą ż Ł → Futura slots; ž č ä → plain letters; „” → "; … → ... */
    const char in[] = "\xC4\x85\xC5\xBC\xC5\x81 \xC5\xBE\xC4\x8D\xC3\xA4 \xE2\x80\x9E\xE2\x80\x9D \xE2\x80\xA6";
    SubtitlesUtf8ToFont(in, sizeof in - 1, out, sizeof out);
    const char want[] = "\xE2\xFB\xCE zca \"\" ...";
    ASSERT_STREQ(out, want);
}

TEST(dialogue_lookup_is_case_insensitive)
{
    SubtitlesLoadBuffer(k_file, sizeof k_file - 1);
    g_subtitles_on = 1;
    uint32_t ms = SubtitlesDialogLine("fjgut04a.wav", 'f');
    ASSERT_TRUE(ms >= 900);                     /* reading time */
    ms = SubtitlesDialogLine("data/FJGUT04B.WAV", 0);
    ASSERT_TRUE(ms > 0);
    ASSERT_EQ(SubtitlesDialogLine("ebgut01a.wav", 'e'), 0u);   /* empty */
    ASSERT_EQ(SubtitlesDialogLine("nie_ma.wav", 0), 0u);
    g_subtitles_on = 0;
    ASSERT_EQ(SubtitlesDialogLine("fjgut04a.wav", 'f'), 0u);  /* option off */
    SubtitlesDialogEnd();
    g_subtitles_on = 1;
}

/* A tiny 1-bpp font: every glyph is a solid 6×10 block, advance 8. */
struct test_font {
    uint16_t advance, baseline, glyph_stride, cell_width;
    uint8_t  first_char, last_char;
    uint8_t *plane[8];
    uint8_t *width_tab, *advance_tab, *kern_tab;
    uint16_t plane_count;
};
static uint8_t s_plane[10 * 256];
static uint8_t s_width[224 * 4];
static uint8_t s_adv[224 * 2];

static FontHandle *make_font(void)
{
    static struct test_font f;
    memset(&f, 0, sizeof f);
    memset(s_plane, 0xFF, sizeof s_plane);
    for (int i = 0; i < 224; ++i) {
        uint16_t bit = (uint16_t)(i * 8);
        s_width[i*4+0] = (uint8_t)(bit >> 8); s_width[i*4+1] = (uint8_t)bit;
        s_width[i*4+2] = 0;                   s_width[i*4+3] = 6;
        s_adv[i*2] = 0; s_adv[i*2+1] = 8;
    }
    f.advance = 8; f.baseline = 10; f.glyph_stride = 256; f.cell_width = 8;
    f.first_char = 0x20; f.last_char = 0xFF;
    f.plane[0] = s_plane; f.plane_count = 1;
    f.width_tab = s_width; f.advance_tab = s_adv; f.kern_tab = NULL;
    return (FontHandle *)&f;
}

TEST(compose_draws_on_a_copy_above_hud)
{
    SubtitlesLoadBuffer(k_file, sizeof k_file - 1);
    FontHandle *keep = g_default_font;
    g_default_font = make_font();
    g_subtitles_on = 1;
    g_cutscene_playing = 0;

    static uint8_t frame[640 * 480];
    static uint8_t pal[768];
    memset(frame, 7, sizeof frame);
    memset(pal, 128, sizeof pal);
    pal[3*3+0] = pal[3*3+1] = pal[3*3+2] = 0;      /* index 3: darkest  */
    pal[9*3+0] = pal[9*3+1] = pal[9*3+2] = 255;    /* index 9: brightest */

    /* nothing active → the frame itself */
    ASSERT_TRUE(SubtitlesComposeFrame(frame, pal, 640, 480) == frame);

    SubtitlesDialogLine("fjgut04a.wav", 'f');
    const uint8_t *out = SubtitlesComposeFrame(frame, pal, 640, 480);
    ASSERT_TRUE(out != frame);
    for (size_t i = 0; i < sizeof frame; ++i) ASSERT_EQ(frame[i], 7);  /* untouched */

    int bright = 0, dark = 0, below_hud = 0, top = 0;
    for (int y = 0; y < 480; ++y)
        for (int x = 0; x < 640; ++x) {
            uint8_t v = out[y * 640 + x];
            if (v == 9) ++bright;
            if (v == 3) ++dark;
            if (v != 7 && y >= 400) ++below_hud;
            if (v != 7 && y < 300) ++top;
        }
    ASSERT_TRUE(bright > 100);
    ASSERT_TRUE(dark > 50);
    ASSERT_EQ(below_hud, 0);        /* gameplay: text stays above the HUD */
    ASSERT_EQ(top, 0);

    /* cutscene: text goes to the very bottom */
    g_cutscene_playing = 1;
    out = SubtitlesComposeFrame(frame, pal, 640, 480);
    int low = 0;
    for (int y = 400; y < 480; ++y)
        for (int x = 0; x < 640; ++x) if (out[y * 640 + x] == 9) ++low;
    ASSERT_TRUE(low > 100);
    g_cutscene_playing = 0;

    SubtitlesDialogEnd();
    ASSERT_TRUE(SubtitlesComposeFrame(frame, pal, 640, 480) == frame);

    /* option off → untouched even with an active line */
    SubtitlesDialogLine("fjgut04a.wav", 'f');
    g_subtitles_on = 0;
    ASSERT_TRUE(SubtitlesComposeFrame(frame, pal, 640, 480) == frame);
    g_subtitles_on = 1;
    SubtitlesDialogEnd();

    /* sound caption at the top */
    g_subtitles_sounds = -1;
    SubtitlesSound("WYBUCH.WAV");
    out = SubtitlesComposeFrame(frame, pal, 640, 480);
    int top_px = 0;
    for (int y = 0; y < 60; ++y)
        for (int x = 0; x < 640; ++x) if (out[y * 640 + x] == 9) ++top_px;
    ASSERT_TRUE(top_px > 20);
    g_subtitles_sounds = 0;                     /* forced off in wacki.cfg */
    SubtitlesSound("nie_ma.wav");

    g_default_font = keep;
    SubtitlesUnload();
    g_subtitles_sounds = -1;
}

TEST(long_text_is_wrapped_not_cut)
{
    static char file[2048];
    char line[1400];
    memset(line, 'a', sizeof line);
    for (int i = 5; i < (int)sizeof line; i += 6) line[i] = ' ';
    line[200] = 0;                              /* ~200 chars → several lines */
    snprintf(file, sizeof file, "[dialogi]\nx.wav\t\t%s\n", line);
    SubtitlesLoadBuffer(file, strlen(file));

    FontHandle *keep = g_default_font;
    g_default_font = make_font();
    static uint8_t frame[640 * 480];
    static uint8_t pal[768];
    memset(frame, 7, sizeof frame);
    memset(pal, 0, sizeof pal);
    pal[5*3] = pal[5*3+1] = pal[5*3+2] = 255;
    SubtitlesDialogLine("x.wav", 0);
    const uint8_t *out = SubtitlesComposeFrame(frame, pal, 640, 480);
    /* rows with text: more than one 10-px line, nothing past the sides */
    int rows = 0;
    for (int y = 0; y < 480; ++y) {
        int has = 0;
        for (int x = 0; x < 640; ++x) if (out[y * 640 + x] == 5) has = 1;
        rows += has;
        ASSERT_TRUE(out[y * 640 + 0] != 5 && out[y * 640 + 639] != 5);
    }
    ASSERT_TRUE(rows >= 20);
    SubtitlesDialogEnd();
    g_default_font = keep;
    SubtitlesUnload();
}

TEST(menu_hint_shows_without_file_or_option)
{
    SubtitlesUnload();
    FontHandle *keep = g_default_font;
    g_default_font = make_font();
    g_subtitles_on = 0;                          /* subtitles off */
    static uint8_t frame[640 * 480];
    static uint8_t pal[768];
    memset(frame, 7, sizeof frame);
    memset(pal, 0, sizeof pal);
    pal[4*3] = pal[4*3+1] = pal[4*3+2] = 250;
    ASSERT_TRUE(SubtitlesComposeFrame(frame, pal, 640, 480) == frame);
    SubtitlesHint("Napisy dla nies\xC5\x82ysz\xC4\x85""cych: w\xC5\x82\xC4\x85""czone", 2000);
    const uint8_t *out = SubtitlesComposeFrame(frame, pal, 640, 480);
    ASSERT_TRUE(out != frame);
    int top = 0;
    for (int y = 0; y < 60; ++y)
        for (int x = 0; x < 640; ++x) if (out[y * 640 + x] == 4) ++top;
    ASSERT_TRUE(top > 50);
    SubtitlesHint(NULL, 0);
    ASSERT_TRUE(SubtitlesComposeFrame(frame, pal, 640, 480) == frame);
    ASSERT_EQ(SubtitlesLoaded(), 0);

    /* SDH = speaker names + sound captions, from wacki.cfg or the file */
    g_subtitles_speakers = 1; g_subtitles_sounds = 1;
    ASSERT_EQ(SubtitlesSdhActive(), 1);
    g_subtitles_sounds = 0;
    ASSERT_EQ(SubtitlesSdhActive(), 0);
    g_subtitles_speakers = -1; g_subtitles_sounds = -1;
    SubtitlesLoadBuffer(k_file, sizeof k_file - 1);   /* @mowcy=1, sounds default 1 */
    ASSERT_EQ(SubtitlesSdhActive(), 1);
    SubtitlesUnload();

    g_subtitles_on = 1;
    g_default_font = keep;
}

TEST(item_name_shown_above_panel_while_hovered)
{
    static const char f[] = "[przedmioty]\nMAGNES.WAV\tmagnes\n";
    ASSERT_EQ(SubtitlesLoadBuffer(f, sizeof f - 1), 1);
    FontHandle *keep = g_default_font;
    g_default_font = make_font();
    g_subtitles_on = 1;
    g_cutscene_playing = 0;
    static uint8_t frame[640 * 480];
    static uint8_t pal[768];
    memset(frame, 7, sizeof frame);
    memset(pal, 0, sizeof pal);
    pal[6*3] = pal[6*3+1] = pal[6*3+2] = 240;
    SubtitlesItemHover("magnes.wav");
    const uint8_t *out = SubtitlesComposeFrame(frame, pal, 640, 480);
    int near_panel = 0, hud = 0;
    for (int y = 340; y < 480; ++y)
        for (int x = 0; x < 640; ++x) {
            if (out[y * 640 + x] == 6 && y < 400) ++near_panel;
            if (out[y * 640 + x] != 7 && y >= 400) ++hud;
        }
    ASSERT_TRUE(near_panel > 20);
    ASSERT_EQ(hud, 0);
    SubtitlesItemHover("nie_ma.wav");          /* unknown: nothing changes */
    g_default_font = keep;
    SubtitlesUnload();
}

/* ---- text/hints.c ------------------------------------------------------ */

static const char HINTS_SAMPLE[] =
    "\xEF\xBB\xBF# test\n"
    "[etap 1]\n"
    "= Og\xC3\xB3lnie\n"
    "@ *\n"
    "1\tCel etapu\n"
    "= Kiosk\n"
    "@ kiosk, ulica\n"
    "1\tNudge\n"
    "2\tKonkret\n"
    "3\tRozwiazanie\n"
    "= Bez poziomow\n"
    "@ pusty\n"
    "[etap 2]\n"
    "= Stacja\n"
    "@ stacja\n"
    "1\tA\n";

TEST(hints_parse_and_find)
{
    ASSERT_EQ(HintsLoadBuffer(HINTS_SAMPLE, sizeof HINTS_SAMPLE - 1), 3);
    ASSERT_EQ(HintsFindTopic(1, "KIOSK2", NULL), 1);       /* key, any case */
    ASSERT_EQ(HintsFindTopic(1, "xx", "ulica_noc.pic"), 1); /* .pic matches  */
    ASSERT_EQ(HintsFindTopic(1, "pusty", NULL), 0);         /* no levels → general */
    ASSERT_EQ(HintsFindTopic(1, NULL, NULL), 0);
    ASSERT_EQ(HintsFindTopic(2, "x", NULL), 2);             /* only topic    */
    ASSERT_EQ(HintsFindTopic(3, "x", NULL), -1);
    HintsUnload();
}

TEST(hints_levels_wait)
{
    extern int g_hint_wait;
    char out[256];
    g_hint_wait = 180;
    HintsLoadBuffer(HINTS_SAMPLE, sizeof HINTS_SAMPLE - 1);
    HintsPress(1, 1000, out, sizeof out);                   /* first: now   */
    ASSERT_TRUE(strstr(out, "1/3") && strstr(out, "Nudge") && strstr(out, "kolejna za 3:00"));
    HintsPress(1, 61000, out, sizeof out);                  /* too early    */
    ASSERT_TRUE(strstr(out, "1/3") && strstr(out, "kolejna za 2:00"));
    HintsPress(1, 181000, out, sizeof out);                 /* 3 min later  */
    ASSERT_TRUE(strstr(out, "2/3") && strstr(out, "Konkret"));
    HintsPress(1, 361000, out, sizeof out);
    ASSERT_TRUE(strstr(out, "3/3") && strstr(out, "Rozwiazanie") && strstr(out, "rozwi"));
    HintsPress(1, 999000, out, sizeof out);                 /* stays at 3/3 */
    ASSERT_TRUE(strstr(out, "3/3") != NULL);
    HintsReset();
    HintsPress(1, 1000000, out, sizeof out);
    ASSERT_TRUE(strstr(out, "1/3") != NULL);
    HintsPress(-1, 0, out, sizeof out);
    ASSERT_TRUE(strstr(out, "Brak") != NULL);
    HintsUnload();
}

SUITE(subtitles)
{
    RUN_TEST(hints_parse_and_find);
    RUN_TEST(hints_levels_wait);
    RUN_TEST(item_name_shown_above_panel_while_hovered);
    RUN_TEST(menu_hint_shows_without_file_or_option);
    RUN_TEST(parse_counts_entries);
    RUN_TEST(utf8_to_font_polish_and_fallbacks);
    RUN_TEST(dialogue_lookup_is_case_insensitive);
    RUN_TEST(compose_draws_on_a_copy_above_hud);
    RUN_TEST(long_text_is_wrapped_not_cut);
}
