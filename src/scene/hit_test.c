/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Mateusz Szuła
 *
 * src/scene/hit_test.c — scene click resolver + verb_id reverse lookup.
 *
 * Two public helpers:
 *
 *   - ClickHitTest(mx, my, *out_verb): walk the click list, run a
 *     per-kind hit-test (8bpp pixel, 1bpp packed, or bbox-only) against
 *     the mouse, pick the winning verb_id by foot_y depth or short-
 *     circuit to a kind=2 first-match. Returns 1 on hit + verb in
 *     `*out_verb`; out_verb defaults to NEUTRAL_VERB on miss.
 *
 *   - FindEntityByVerbId(target_verb): reverse lookup — given a verb id,
 *     find the OWNER entity (the sprite or mask whose click payload
 *     carries that verb). Used by opcodes 0x15/0x26/0x28 etc.
 *
 * Both walk g_click_list_head via EntityListAt/Count. Sprite payloads
 * (kind=1) carry a verb table indexed by current frame; mask payloads
 * (kind=2) pre-bake the verb at registration time.
 */

#include "wacki.h"

#include <stddef.h>
#include <stdint.h>

extern Entity *g_click_list_head;
extern int     EntityListCount(int click_list);
extern Entity *EntityListAt   (int click_list, int idx);

/* ---- module constants --------------------------------------------- */

#define NEUTRAL_VERB                  0x26

/* Iterator selector for EntityListAt/Count. */
#define CLICK_LIST_INDEX              1

/* Click descriptor kinds at +0x08. */
#define CLICK_KIND_SPRITE             1
#define CLICK_KIND_MASK               2

/* Mask flag bits at owner+0x14. The low byte (after XOR with HITTABLE)
 * selects the per-pixel test mode. */
#define MASK_FLAG_ALWAYS_HIT          0x0010
#define MASK_FLAG_HITTABLE            0x8000
#define MASK_TEST_8BPP                1
#define MASK_TEST_1BPP_PACKED         2
#define MASK_TEST_BBOX                4
#define MASK_TEST_TERMINATE           8

/* 1bpp mask: MSB packing (each byte stores 8 pixels left-to-right). */
#define BPP1_HIGH_BIT                 0x80
#define BPP1_BITS_PER_BYTE            8
#define BPP1_BIT_INDEX_MASK           0x07

/* ---- helpers ------------------------------------------------------- */

extern int AnimFrameRleSrcLen(const AnimAsset *a, const uint8_t *px);

/* Pixel number `index` of an RLE frame (same stream as DepackRleFrame),
 * found by walking the runs instead of unpacking the whole frame.
 * Returns the fill colour (transparent) if the stream ends first. */
static uint8_t rle_pixel_at(const uint8_t *src, int src_len, size_t index)
{
    if (!src || src_len < 3) return 0;
    const uint8_t *end = src + src_len;
    uint8_t fill = src[0], mark_a = src[1], mark_b = src[2];
    const uint8_t *p = src + 3;
    size_t pos = 0;
    while (p < end) {
        uint8_t b = *p++;
        size_t  count;
        uint8_t v;
        if (b == mark_a) {
            if (p >= end) break;
            count = (size_t)*p++ + 1;
            v = fill;
        } else if (b == mark_b) {
            if (p + 1 >= end) break;
            count = (size_t)*p++ + 1;
            v = *p++;
        } else {
            count = 1;
            v = b;
        }
        if (index < pos + count) return v;
        pos += count;
    }
    return fill;
}

/* Pixel test for a kind=1 click descriptor: 8bpp pixel at (mx, my) on
 * the owner's atlas frame. Caller has already done the bbox check.
 * Also returns the current frame so the verb-table lookup can use it. */
static int pixel_hit_sprite(Entity *owner, int16_t mouse_x, int16_t mouse_y,
                            int16_t ox, int16_t oy, uint16_t ow, uint16_t oh,
                            uint16_t *out_frame)
{
    *out_frame = 0;
    AnimAsset *atlas =
        (AnimAsset *)ent_ptr_resolve(EOFF(owner, ENT_OFF_ATLAS_SLOT, uint32_t));
    if (!atlas || !atlas->frame_count) return 0;

    uint16_t cur_frame = EOFF(owner, ENT_OFF_FRAME, uint16_t);
    if (cur_frame >= atlas->frame_count) cur_frame = 0;
    *out_frame = cur_frame;

    const uint8_t *px = atlas->pixel_ptrs ? atlas->pixel_ptrs[cur_frame] : NULL;
    if (!px) return 0;

    int lx = mouse_x - ox;
    int ly = mouse_y - oy;

    /* The entity's width/height were baked by the last VM step (and
     * doubled for 2x sprites), but the frame may have changed since or
     * be drawn scaled. Map into the frame's own pixel grid and refuse
     * anything outside it — reading past the frame (seen on the flower
     * in front of the Africa mine) walks off the end of the asset. */
    int fw = atlas->off_widths  ? atlas->off_widths [cur_frame] : (int)ow;
    int fh = atlas->off_heights ? atlas->off_heights[cur_frame] : (int)oh;
    if (fw <= 0 || fh <= 0 || ow == 0 || oh == 0) return 0;
    if (fw != (int)ow) lx = lx * fw / (int)ow;
    if (fh != (int)oh) ly = ly * fh / (int)oh;
    if (lx < 0 || ly < 0 || lx >= fw || ly >= fh) return 0;

    size_t at = (size_t)ly * (size_t)fw + (size_t)lx;

    /* kind=3 frames are RLE streams (see DepackRleFrame), not a pixel
     * grid: indexing them directly tested a random compressed byte —
     * and, for a small packed frame like the flower in front of the
     * Africa mine (45x47 px in 811 bytes), read past the asset. */
    if (atlas->kind == 3)
        return rle_pixel_at(px, AnimFrameRleSrcLen(atlas, px), at) != 0;

    if (atlas->raw_buffer && atlas->raw_size) {
        const uint8_t *lo = (const uint8_t *)atlas->raw_buffer;
        const uint8_t *hi = lo + atlas->raw_size;
        if (px >= lo && px < hi && at >= (size_t)(hi - px)) return 0;
    }
    return px[at] != 0;
}

/* Pixel test for a kind=2 mask click descriptor. The mask flag at
 * owner+0x14 selects the test mode; HITTABLE bit (or ALWAYS_HIT override)
 * must be set or the mask doesn't fire at all. */
static int pixel_hit_mask(Entity *owner, int16_t mouse_x, int16_t mouse_y,
                          int16_t ox, int16_t oy, uint16_t ow)
{
    uint16_t mask_flag  = EOFF(owner, ENT_OFF_PIXELS_SLOT, uint16_t);
    int      always_hit = (mask_flag & MASK_FLAG_ALWAYS_HIT) != 0;

    /* HIDE'd masks (HITTABLE cleared) don't fire unless ALWAYS_HIT
     * forces them through. */
    if (!always_hit && (mask_flag & MASK_FLAG_HITTABLE) == 0) return 0;
    if (always_hit) return 1;

    const uint8_t *px =
        (const uint8_t *)ent_ptr_resolve(EOFF(owner, ENT_OFF_PIXEL_SLOT_ALT, uint32_t));
    int lx = mouse_x - ox;
    int ly = mouse_y - oy;

    switch (mask_flag ^ MASK_FLAG_HITTABLE) {
    case MASK_TEST_8BPP:
        return px && px[ly * (int)ow + lx] != 0;

    case MASK_TEST_1BPP_PACKED: {
        if (!px) return 0;
        int     stride = (ow + 7) / BPP1_BITS_PER_BYTE;
        uint8_t byte   = px[ly * stride + lx / BPP1_BITS_PER_BYTE];
        return (byte & (BPP1_HIGH_BIT >> (lx & BPP1_BIT_INDEX_MASK))) != 0;
    }

    case MASK_TEST_BBOX:
        return 1;

    case MASK_TEST_TERMINATE:
    default:
        /* TERMINATE explicitly says "this mask never hits" — original
         * also cleared a global continue flag, but our sequential
         * walk handles that naturally by simply skipping the entry. */
        return 0;
    }
}

/* Resolve the verb id for a click descriptor that just matched the
 * pixel test. kind=1 uses the verb table indexed by current frame;
 * kind=2 carries the verb pre-baked at +0x12.
 *
 * Mutates the descriptor's cache slot at +0x12 so subsequent
 * FindEntityByVerbId calls can use the cached value. */
static uint16_t resolve_hit_verb(Entity *desc, uint16_t kind, uint16_t cur_frame)
{
    uint8_t        *cb = (uint8_t *)desc;
    uint16_t        verb = EOFF(desc, CLICK_OFF_CACHED_VERB, uint16_t);
    const uint16_t *vt = (const uint16_t *)ent_ptr_resolve(
        EOFF(desc, CLICK_OFF_VERB_TABLE_SLOT, uint32_t));

    if (vt && kind == CLICK_KIND_SPRITE) {
        uint16_t count = vt[0];
        if (count) {
            uint16_t idx = (cur_frame < count) ? cur_frame
                                               : (uint16_t)(count - 1);
            verb = vt[idx + 1];
            *(uint16_t *)(cb + CLICK_OFF_CACHED_VERB) = verb;
        }
    }
    return verb;
}

/* ---- FindEntityByVerbId ------------------------------------------- *
 *
 * Reverse lookup: walk the click list and return the OWNER entity
 * whose payload would resolve to `target_verb` at the current frame.
 * Only kind=1 (sprite) payloads return an owner — kind=2 mask payloads
 * match against the verb but return NULL since the "owner" is a mask
 * desc rather than something callers like op 0x28 SET_ENTITY_XY can
 * use. */
Entity *FindEntityByVerbId(uint16_t target_verb)
{
    int nclk = EntityListCount(CLICK_LIST_INDEX);
    for (int i = 0; i < nclk; ++i) {
        Entity *c = EntityListAt(CLICK_LIST_INDEX, i);
        if (!c) continue;

        uint8_t  *cb         = (uint8_t *)c;
        uint16_t  click_kind = EOFF(c, CLICK_OFF_KIND, uint16_t);
        const uint16_t *vt   = (const uint16_t *)ent_ptr_resolve(
            EOFF(c, CLICK_OFF_VERB_TABLE_SLOT, uint32_t));

        uint16_t verb;
        if (!vt) {
            verb = EOFF(c, CLICK_OFF_CACHED_VERB, uint16_t);  /* kind=2 cached path */
        } else {
            uint16_t count = vt[0];
            uint16_t idx   = 0;
            if (click_kind == CLICK_KIND_SPRITE) {
                Entity *owner = (Entity *)ent_ptr_resolve(
                    EOFF(c, CLICK_OFF_OWNER_SLOT, uint32_t));
                if (owner) {
                    uint16_t frame = EOFF(owner, ENT_OFF_FRAME, uint16_t);
                    if (count && frame >= count) {
                        verb = vt[count];           /* last entry */
                        goto matched;
                    }
                    idx = frame;
                }
            }
            verb = count ? vt[idx + 1] : 0;
        }
    matched:
        if (verb == target_verb) {
            /* Only sprite payloads return an owner. */
            if (click_kind == CLICK_KIND_SPRITE) {
                return (Entity *)ent_ptr_resolve(
                    EOFF(c, CLICK_OFF_OWNER_SLOT, uint32_t));
            }
            return NULL;
        }
        (void)cb;
    }
    return NULL;
}

/* ---- ClickHitTest -------------------------------------------------- *
 *
 * The mouse → verb_id pipeline:
 *   1. Walk the click list
 *   2. For each desc, run kind-specific pixel test against (mx, my)
 *   3. Resolve the hit verb (kind=1 frame-indexed, kind=2 cached)
 *   4. Pick winner by foot_y depth, kind=2 short-circuits
 *
 * Returns 1 on hit (verb stored in *out_verb); 0 on miss (out_verb is
 * still written to NEUTRAL_VERB so callers don't see stale values). */
int ClickHitTest(int16_t mouse_x, int16_t mouse_y, uint16_t *out_verb)
{
    uint16_t hit_verb   = 0;
    int16_t  hit_foot_y = 0;
    int      any_hit    = 0;

    int nclk = EntityListCount(CLICK_LIST_INDEX);
    for (int i = 0; i < nclk; ++i) {
        Entity *c = EntityListAt(CLICK_LIST_INDEX, i);
        if (!c) continue;

        uint16_t kind = EOFF(c, CLICK_OFF_KIND, uint16_t);
        if (kind != CLICK_KIND_SPRITE && kind != CLICK_KIND_MASK) continue;

        Entity *owner = (Entity *)ent_ptr_resolve(
            EOFF(c, CLICK_OFF_OWNER_SLOT, uint32_t));
        if (!owner) continue;

        /* Bbox prune against owner's drawn rectangle. */
        int16_t  ox = EOFF(owner, ENT_OFF_DRAWN_X, int16_t);
        int16_t  oy = EOFF(owner, ENT_OFF_DRAWN_Y, int16_t);
        uint16_t ow = EOFF(owner, ENT_OFF_WIDTH,   uint16_t);
        uint16_t oh = EOFF(owner, ENT_OFF_HEIGHT,  uint16_t);
        if (mouse_x < ox || mouse_x >= ox + (int)ow ||
            mouse_y < oy || mouse_y >= oy + (int)oh) continue;

        /* Kind-specific pixel test. */
        int      hit       = 0;
        uint16_t cur_frame = 0;
        if (kind == CLICK_KIND_SPRITE) {
            hit = pixel_hit_sprite(owner, mouse_x, mouse_y, ox, oy, ow, oh,
                                   &cur_frame);
        } else {
            hit = pixel_hit_mask(owner, mouse_x, mouse_y, ox, oy, ow);
        }
        if (!hit) continue;

        /* Resolve the verb and pick depth winner. */
        uint16_t verb   = resolve_hit_verb(c, kind, cur_frame);
        int16_t  foot_y = EOFF(owner, ENT_OFF_CLICK_FOOT_Y, int16_t);
        any_hit = 1;

        if (hit_verb == 0) {
            hit_verb   = verb;
            hit_foot_y = foot_y;
            if (kind == CLICK_KIND_MASK) {
                /* kind=2 first hit short-circuits everything else. */
                *out_verb = (verb == 0) ? NEUTRAL_VERB : verb;
                return any_hit;
            }
        } else if (foot_y > hit_foot_y) {
            hit_verb   = verb;
            hit_foot_y = foot_y;
            if (kind == CLICK_KIND_MASK) {
                *out_verb = verb;
                return any_hit;
            }
        }
    }

    *out_verb = hit_verb ? hit_verb : NEUTRAL_VERB;
    return any_hit;
}
