#include "smw_softbody.h"
#include "common_rtl.h"
#include "smw_rtl.h"
#include "variables.h"
#include "snes/ppu.h"

// Grid: 5x5 control points → 4x4 mesh cells across a 16x16 pixel sprite.
// Each cell covers 4x4 source pixels; interior points form a "quadtree"
// that can freely stretch, compress, or bend.
#define SB_N       5                     // points per row / column
#define SB_CELLS   (SB_N - 1)            // 4x4 cells
#define SB_PX      16                    // sprite pixel size
#define SB_CELL_PX (SB_PX / SB_CELLS)    // 4 pixels per cell edge
#define SB_SUB     4                     // 4x oversample when warping
#define SB_SUB_PX  (SB_PX * SB_SUB)      // 64 sub-samples per axis

// Fixed-point scale for velocities/offsets. 1 unit = 1/SB_FP pixels.
// Gives us ~0.125 px resolution while still fitting in int16 during math.
#define SB_FP      8

typedef struct SbSlot {
  int16 ox[SB_N * SB_N];       // offset from rest (SB_FP scale)
  int16 oy[SB_N * SB_N];
  int16 vx[SB_N * SB_N];       // velocity (SB_FP per frame)
  int16 vy[SB_N * SB_N];
  uint16 prev_spr_x;
  uint16 prev_spr_y;
  uint8  src_pix[SB_PX * SB_PX];    // decoded source image (palette 0-15)
  uint16 orig_tile_word[4][16];     // original VRAM words for 4 tiles
  uint16 tile_vaddr[4];             // VRAM word-address of each tile
  uint8  active;                    // 1 = body initialised for this slot
} SbSlot;

static SbSlot g_sb[12];

// ─── SNES 4bpp planar tile <-> pixel array ──────────────────────────────
// VRAM layout per 8x8 tile: 16 words.  Row r (0..7):
//   vram[vaddr+r]   = BP0(r) | BP1(r) << 8    // low byte BP0, high byte BP1
//   vram[vaddr+r+8] = BP2(r) | BP3(r) << 8
// Bit 7 of each byte is the leftmost pixel (MSB first).

static void DecodeTile(uint16 vaddr, uint8 out[64]) {
  for (int row = 0; row < 8; ++row) {
    uint16 lo = g_ppu->vram[(vaddr + row) & 0x7fff];
    uint16 hi = g_ppu->vram[(vaddr + row + 8) & 0x7fff];
    for (int col = 0; col < 8; ++col) {
      int s = 7 - col;
      uint8 px = ((lo >> s) & 1)
               | (((lo >> (s + 8)) & 1) << 1)
               | (((hi >> s) & 1) << 2)
               | (((hi >> (s + 8)) & 1) << 3);
      out[row * 8 + col] = px;
    }
  }
}

static void EncodeTile(const uint8 in[64], uint16 vaddr) {
  for (int row = 0; row < 8; ++row) {
    uint16 lo = 0, hi = 0;
    for (int col = 0; col < 8; ++col) {
      uint8 px = in[row * 8 + col];
      int s = 7 - col;
      lo |= (uint16)((px >> 0) & 1) << s;
      lo |= (uint16)((px >> 1) & 1) << (s + 8);
      hi |= (uint16)((px >> 2) & 1) << s;
      hi |= (uint16)((px >> 3) & 1) << (s + 8);
    }
    g_ppu->vram[(vaddr + row) & 0x7fff] = lo;
    g_ppu->vram[(vaddr + row + 8) & 0x7fff] = hi;
  }
}

// SNES sprites wrap within rows of 16 tiles when addressing a 16x16 quad.
// For base charnum `ch` in bank `bank`, compute the 4 tile VRAM addresses.
static void ComputeTileAddrs(uint8 ch, uint8 bank, uint16 addrs[4]) {
  uint16 base = bank ? PPU_objTileAdr2(g_ppu) : PPU_objTileAdr1(g_ppu);
  uint8 tl = ch;
  uint8 tr = (ch & 0xf0) | ((ch + 1) & 0x0f);
  uint8 bl = (ch + 0x10);
  uint8 br = (bl & 0xf0) | ((bl + 1) & 0x0f);
  addrs[0] = (base + tl * 16) & 0x7fff;
  addrs[1] = (base + tr * 16) & 0x7fff;
  addrs[2] = (base + bl * 16) & 0x7fff;
  addrs[3] = (base + br * 16) & 0x7fff;
}

// Copy 16 VRAM words per tile into `dst`, capturing pristine contents.
static void SnapshotTiles(const uint16 addrs[4], uint16 dst[4][16]) {
  for (int t = 0; t < 4; ++t)
    for (int w = 0; w < 16; ++w)
      dst[t][w] = g_ppu->vram[(addrs[t] + w) & 0x7fff];
}

// Restore snapshotted tiles to VRAM.
static void RestoreTiles(const uint16 addrs[4], const uint16 src[4][16]) {
  for (int t = 0; t < 4; ++t)
    for (int w = 0; w < 16; ++w)
      g_ppu->vram[(addrs[t] + w) & 0x7fff] = src[t][w];
}

// Pack a 16x16 palette-index image back into 4 SNES tiles at `addrs`.
static void EncodeSprite16(const uint8 pix[SB_PX * SB_PX], const uint16 addrs[4]) {
  uint8 tile[64];
  for (int t = 0; t < 4; ++t) {
    int tx0 = (t & 1) * 8;
    int ty0 = (t & 2) * 4;  // 0 or 8
    for (int r = 0; r < 8; ++r)
      for (int c = 0; c < 8; ++c)
        tile[r * 8 + c] = pix[(ty0 + r) * SB_PX + (tx0 + c)];
    EncodeTile(tile, addrs[t]);
  }
}

// Decode 4 tiles into a 16x16 palette-index image.
static void DecodeSprite16(const uint16 addrs[4], uint8 pix[SB_PX * SB_PX]) {
  uint8 tile[64];
  for (int t = 0; t < 4; ++t) {
    DecodeTile(addrs[t], tile);
    int tx0 = (t & 1) * 8;
    int ty0 = (t & 2) * 4;
    for (int r = 0; r < 8; ++r)
      for (int c = 0; c < 8; ++c)
        pix[(ty0 + r) * SB_PX + (tx0 + c)] = tile[r * 8 + c];
  }
}

// ─── Physics ────────────────────────────────────────────────────────────

static void StepPhysics(SbSlot *s, int16 hand_dx, int16 hand_dy) {
  // Anchor: bottom-centre point (where Mario grips).  It is clamped to
  // (0,0) each step and imparts no inertia — the rest of the mesh reacts
  // around it.
  const int anchor = 4 * SB_N + 2;  // (2, 4)

  // Scratch accumulators for forces, so integration uses a consistent pass.
  int16 ax[SB_N * SB_N] = { 0 };
  int16 ay[SB_N * SB_N] = { 0 };

  // 1. Gravity (constant downward pull, proportional to distance from anchor
  //    so the top droops more than the bottom — mimics weight distribution).
  for (int gy = 0; gy < SB_N; ++gy)
    for (int gx = 0; gx < SB_N; ++gx) {
      int i = gy * SB_N + gx;
      int dist_from_anchor = (4 - gy);  // rows above anchor
      if (dist_from_anchor < 0) dist_from_anchor = 0;
      ay[i] += (int16)(2 + dist_from_anchor);  // SB_FP units
    }

  // 2. Anchor-to-rest spring: gentle pull on every node toward its rest
  //    position so deformations eventually decay.  k ≈ 1/16.
  for (int i = 0; i < SB_N * SB_N; ++i) {
    ax[i] -= s->ox[i] >> 4;
    ay[i] -= s->oy[i] >> 4;
  }

  // 3. Neighbour springs (structural + shear).  Each link pulls the two
  //    endpoints toward having matching offsets (rest-length preserved
  //    because the rest positions already encode spacing).
  //    Structural: horizontal & vertical 4-neighbours, k ≈ 3/8.
  //    Shear:      diagonal 8-neighbours,              k ≈ 1/8.
  #define SB_LINK(a_idx, b_idx, knum, kden)                           \
      do {                                                            \
        int16 ex = s->ox[b_idx] - s->ox[a_idx];                       \
        int16 ey = s->oy[b_idx] - s->oy[a_idx];                       \
        int16 fx = (ex * (knum)) / (kden);                            \
        int16 fy = (ey * (knum)) / (kden);                            \
        ax[a_idx] += fx; ay[a_idx] += fy;                             \
        ax[b_idx] -= fx; ay[b_idx] -= fy;                             \
      } while (0)

  for (int gy = 0; gy < SB_N; ++gy) {
    for (int gx = 0; gx < SB_N; ++gx) {
      int i = gy * SB_N + gx;
      if (gx + 1 < SB_N) SB_LINK(i, i + 1,     3, 8);
      if (gy + 1 < SB_N) SB_LINK(i, i + SB_N, 3, 8);
      if (gx + 1 < SB_N && gy + 1 < SB_N) SB_LINK(i, i + SB_N + 1, 1, 8);
      if (gx > 0       && gy + 1 < SB_N) SB_LINK(i, i + SB_N - 1, 1, 8);
    }
  }
  #undef SB_LINK

  // 4. Hand-motion inertia: when Mario moves by (hand_dx, hand_dy) this
  //    frame, every non-anchor node receives an opposing velocity kick so
  //    it "stays behind" in world-space for a moment.  Intensity scales
  //    with distance from the anchor — far nodes flop more.
  for (int gy = 0; gy < SB_N; ++gy) {
    for (int gx = 0; gx < SB_N; ++gx) {
      int i = gy * SB_N + gx;
      if (i == anchor) continue;
      int strength = (4 - gy) + 1;  // 1..5, larger for upper rows
      s->vx[i] -= (int16)(hand_dx * SB_FP * strength / 4);
      s->vy[i] -= (int16)(hand_dy * SB_FP * strength / 4);
    }
  }

  // 5. Integrate with damping.
  for (int i = 0; i < SB_N * SB_N; ++i) {
    s->vx[i] += ax[i];
    s->vy[i] += ay[i];
    s->vx[i] = (int16)((s->vx[i] * 13) / 16);  // ~18% energy loss / frame
    s->vy[i] = (int16)((s->vy[i] * 13) / 16);
    s->ox[i] += s->vx[i];
    s->oy[i] += s->vy[i];
    // Clamp offsets so the mesh can't invert or run off-tile.
    int16 lim = 7 * SB_FP;
    if (s->ox[i] >  lim) s->ox[i] =  lim;
    if (s->ox[i] < -lim) s->ox[i] = -lim;
    if (s->oy[i] >  lim) s->oy[i] =  lim;
    if (s->oy[i] < -lim) s->oy[i] = -lim;
  }

  // Anchor is immovable.
  s->ox[anchor] = 0;
  s->oy[anchor] = 0;
  s->vx[anchor] = 0;
  s->vy[anchor] = 0;
}

// ─── Warp ───────────────────────────────────────────────────────────────

// Build warped 16x16 image by forward-mapping each source pixel through
// the deformed lattice, with 4x oversampling to minimise gaps.
static void WarpImage(const SbSlot *s, uint8 dst[SB_PX * SB_PX]) {
  // Fill with transparent (palette 0).
  for (int i = 0; i < SB_PX * SB_PX; ++i) dst[i] = 0;

  // Oversampled scan: (ssx, ssy) in [0..SB_SUB_PX).  Source pixel coords
  // in integer pixel units are (ssx / SB_SUB, ssy / SB_SUB).
  const int denom = SB_SUB_PX / SB_CELLS;  // sub-samples per cell edge = 16

  for (int ssy = 0; ssy < SB_SUB_PX; ++ssy) {
    for (int ssx = 0; ssx < SB_SUB_PX; ++ssx) {
      int sx = ssx >> 2;   // source px X (0..15)
      int sy = ssy >> 2;   // source px Y (0..15)
      uint8 px = s->src_pix[sy * SB_PX + sx];
      if (px == 0) continue;   // transparent stays transparent

      // Which mesh cell are we in? (0..SB_CELLS-1)
      int cx = ssx / denom;
      int cy = ssy / denom;
      if (cx >= SB_CELLS) cx = SB_CELLS - 1;
      if (cy >= SB_CELLS) cy = SB_CELLS - 1;
      int ux = ssx - cx * denom;   // 0..denom-1
      int uy = ssy - cy * denom;

      // Bilinear interpolate offsets of the 4 cell corners.
      int i00 = cy * SB_N + cx;
      int i10 = i00 + 1;
      int i01 = i00 + SB_N;
      int i11 = i01 + 1;
      int w00 = (denom - ux) * (denom - uy);
      int w10 = ux * (denom - uy);
      int w01 = (denom - ux) * uy;
      int w11 = ux * uy;
      int total = denom * denom;
      int dox = (w00 * s->ox[i00] + w10 * s->ox[i10]
               + w01 * s->ox[i01] + w11 * s->ox[i11]) / total;
      int doy = (w00 * s->oy[i00] + w10 * s->oy[i10]
               + w01 * s->oy[i01] + w11 * s->oy[i11]) / total;
      // dox/doy are in SB_FP units.  Convert to sub-pixel.
      int dx_sub = ssx + (dox * SB_SUB) / SB_FP;
      int dy_sub = ssy + (doy * SB_SUB) / SB_FP;
      int dx = dx_sub >> 2;
      int dy = dy_sub >> 2;
      if ((unsigned)dx >= SB_PX || (unsigned)dy >= SB_PX) continue;
      dst[dy * SB_PX + dx] = px;
    }
  }
}

// ─── Public entry points ────────────────────────────────────────────────

void SoftBodyResetAll(void) {
  for (int k = 0; k < 12; ++k) g_sb[k].active = 0;
}

static void SoftBodyActivate(uint8 k, uint8 ch, uint8 bank) {
  SbSlot *s = &g_sb[k];
  for (int i = 0; i < SB_N * SB_N; ++i) {
    s->ox[i] = 0; s->oy[i] = 0;
    s->vx[i] = 0; s->vy[i] = 0;
  }
  ComputeTileAddrs(ch, bank, s->tile_vaddr);
  SnapshotTiles(s->tile_vaddr, s->orig_tile_word);
  DecodeSprite16(s->tile_vaddr, s->src_pix);
  s->prev_spr_x = GetSprXPos(k);
  s->prev_spr_y = GetSprYPos(k);
  s->active = 1;
}

static void SoftBodyDeactivate(uint8 k) {
  SbSlot *s = &g_sb[k];
  if (!s->active) return;
  RestoreTiles(s->tile_vaddr, s->orig_tile_word);
  s->active = 0;
}

void SoftBodyOnCarriedFrame(uint8 k) {
  if (spr_spriteid[k] == 125) {  // P-balloon: has its own OAM magic
    SoftBodyDeactivate(k);
    return;
  }

  // Most sprites write their main 16x16 entry at oam[64] relative to
  // spr_oamindex[k].  Shells (property_bits167a bit 3 == 0) take a
  // detour through StunnedShellGFXRt which temporarily bumps oamindex
  // by +8 bytes, so their body lands at oam[66] — and the two face
  // tiles end up at oam[64] / oam[65] as 8x8 decorations we leave alone.
  // When oamindex is 0 the +8 shift is skipped and the body is back at
  // oam[64] (that's a very rare case for carried sprites but handle it).
  uint8 idx = spr_oamindex[k];
  bool is_shell = (spr_property_bits167a[k] & 8) == 0;
  uint8 ent_off = (is_shell && idx != 0) ? 66 : 64;

  OamEnt *oam = get_OamEnt(oam_buf, idx);
  uint8 slot = idx >> 2;
  if ((sprites_oamtile_size_buffer[slot + ent_off] & 2) == 0) {
    // The draw routine skipped this sprite or wrote it as 8x8 — bail.
    return;
  }
  uint8 ch   = oam[ent_off].charnum;
  uint8 bank = oam[ent_off].flags & 1;

  SbSlot *s = &g_sb[k];

  // First frame of carry, or charnum / animation frame changed → rebind.
  if (!s->active) {
    SoftBodyActivate(k, ch, bank);
  } else if (s->tile_vaddr[0] != ((uint16)((bank ? PPU_objTileAdr2(g_ppu)
                                                 : PPU_objTileAdr1(g_ppu))
                                           + ch * 16) & 0x7fff)) {
    // Tile changed (e.g. animation): restore old, snapshot new.
    RestoreTiles(s->tile_vaddr, s->orig_tile_word);
    ComputeTileAddrs(ch, bank, s->tile_vaddr);
    SnapshotTiles(s->tile_vaddr, s->orig_tile_word);
    DecodeSprite16(s->tile_vaddr, s->src_pix);
  }

  // Motion since last frame (signed, clamped — prevents huge kicks on
  // screen wraps or teleports).
  uint16 cx = GetSprXPos(k), cy = GetSprYPos(k);
  int16 dx = (int16)cx - (int16)s->prev_spr_x;
  int16 dy = (int16)cy - (int16)s->prev_spr_y;
  if (dx >  16) dx =  16;
  if (dx < -16) dx = -16;
  if (dy >  16) dy =  16;
  if (dy < -16) dy = -16;
  s->prev_spr_x = cx;
  s->prev_spr_y = cy;

  // During the brief "player picked up" pose, hand is still settling —
  // keep the mesh at rest so we don't get a pop.
  if (timer_display_player_pick_up_pose) {
    for (int i = 0; i < SB_N * SB_N; ++i) {
      s->ox[i] = 0; s->oy[i] = 0;
      s->vx[i] = 0; s->vy[i] = 0;
    }
  } else {
    StepPhysics(s, dx, dy);
  }

  // Warp and upload.
  uint8 warped[SB_PX * SB_PX];
  WarpImage(s, warped);
  EncodeSprite16(warped, s->tile_vaddr);
}

void SoftBodyPollAllSlots(void) {
  for (uint8 k = 0; k < 12; ++k) {
    if (g_sb[k].active && spr_current_status[k] != 11)
      SoftBodyDeactivate(k);
  }
}
