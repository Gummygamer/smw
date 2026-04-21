#include "smw_softbody.h"
#include "common_rtl.h"
#include "smw_rtl.h"
#include "variables.h"
#include "snes/ppu.h"

// 5x5 control-point lattice over a 16x16 source sprite (4x4 quadtree cells).
// Each frame we integrate a spring-mass network, forward-map the deformed
// source pixels into a canvas, and re-encode that canvas back into sprite
// VRAM tiles.  Two rendering modes:
//
//   mode 1 (non-shell): 32x32 canvas centred on the original 16x16, drawn
//          via four 16x16 OAM entries pointing at scratch VRAM tiles — so
//          deformation can spill far outside the original silhouette.
//
//   mode 2 (shell, or mode-1 fallback when the scratch pool is full):
//          16x16 canvas, outer ring of control points edge-locked so the
//          deformation stays inside the sprite's original tile footprint.

#define SB_N       5
#define SB_CELLS   (SB_N - 1)
#define SB_PX      16
#define SB_CELL_PX (SB_PX / SB_CELLS)
#define SB_SUB     4
#define SB_SUB_PX  (SB_PX * SB_SUB)
#define SB_FP      8

#define SB_CANVAS        32
#define SB_CANVAS_MARGIN 8   // (SB_CANVAS - SB_PX) / 2

// Scratch charnum pools in sprite bank 2 (flags.bank = 1).  Each pool owns
// four quadrant base charnums whose 2x2 tile blocks are non-overlapping, so
// two pools together tile bank 2's rows 0xE/0xF exactly.
#define SB_POOL_COUNT 2
static const uint8 kPoolQuadCharnum[SB_POOL_COUNT][4] = {
  // Quadrant order: TL, TR, BL, BR.
  { 0xE0, 0xE2, 0xE8, 0xEA },
  { 0xE4, 0xE6, 0xEC, 0xEE },
};
static uint8 g_pool_used[SB_POOL_COUNT];

typedef struct SbSlot {
  int16 ox[SB_N * SB_N];       // offset from rest (SB_FP units)
  int16 oy[SB_N * SB_N];
  int16 vx[SB_N * SB_N];       // velocity (SB_FP per frame)
  int16 vy[SB_N * SB_N];
  uint16 prev_spr_x;
  uint16 prev_spr_y;
  uint8  src_pix[SB_PX * SB_PX];    // decoded source image (pal idx 0..15)

  uint8  mode;        // 0 inactive, 1 = 32x32 expanded, 2 = 16x16 edge-lock
  int8   pool_id;     // 0..SB_POOL_COUNT-1 in mode 1, -1 otherwise
  uint8  oamindex;    // snapshot of spr_oamindex[k] at activation

  // mode 1: scratch tile vaddrs + their snapshot (so we can restore clean
  //         bank-2 tiles when the carry ends).
  uint16 scratch_vaddr[16];
  uint16 scratch_orig[16][16];

  // mode 2: the held sprite's own 4 tile vaddrs + snapshot (we rewrite
  //         these each frame and restore them on drop).
  uint16 orig_tile_vaddr[4];
  uint16 orig_tile_word[4][16];
} SbSlot;

static SbSlot g_sb[12];

// ─── SNES 4bpp planar tile <-> palette-index pixel array ─────────────────
// Per 8x8 tile: 16 words.  Row r (0..7):
//   vram[vaddr+r]   = BP0(r) | BP1(r) << 8
//   vram[vaddr+r+8] = BP2(r) | BP3(r) << 8
// Bit 7 is the leftmost pixel.

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

// 4 tile VRAM addresses for a single 16x16 OAM entry at base charnum `ch`.
// SNES sprites wrap within rows of 16 tiles when stepping right/down.
static void ComputeQuadTileAddrs(uint8 ch, uint8 bank, uint16 addrs[4]) {
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

static void SnapshotTiles(const uint16 *addrs, int n, uint16 (*dst)[16]) {
  for (int t = 0; t < n; ++t)
    for (int w = 0; w < 16; ++w)
      dst[t][w] = g_ppu->vram[(addrs[t] + w) & 0x7fff];
}

static void RestoreTiles(const uint16 *addrs, int n, const uint16 (*src)[16]) {
  for (int t = 0; t < n; ++t)
    for (int w = 0; w < 16; ++w)
      g_ppu->vram[(addrs[t] + w) & 0x7fff] = src[t][w];
}

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

static void EncodeSprite16(const uint8 pix[SB_PX * SB_PX], const uint16 addrs[4]) {
  uint8 tile[64];
  for (int t = 0; t < 4; ++t) {
    int tx0 = (t & 1) * 8;
    int ty0 = (t & 2) * 4;
    for (int r = 0; r < 8; ++r)
      for (int c = 0; c < 8; ++c)
        tile[r * 8 + c] = pix[(ty0 + r) * SB_PX + (tx0 + c)];
    EncodeTile(tile, addrs[t]);
  }
}

// Encode a 32x32 canvas into 16 scratch tiles (4 quadrants × 4 tiles each).
static void EncodeCanvas32(const uint8 canvas[SB_CANVAS * SB_CANVAS],
                           const uint16 scratch_vaddr[16]) {
  uint8 tile[64];
  for (int q = 0; q < 4; ++q) {
    int qx0 = (q & 1) * 16;
    int qy0 = (q & 2) * 8;   // 0 or 16
    for (int t = 0; t < 4; ++t) {
      int tx0 = qx0 + (t & 1) * 8;
      int ty0 = qy0 + (t & 2) * 4;
      for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c)
          tile[r * 8 + c] = canvas[(ty0 + r) * SB_CANVAS + (tx0 + c)];
      EncodeTile(tile, scratch_vaddr[q * 4 + t]);
    }
  }
}

// ─── Physics ─────────────────────────────────────────────────────────────

static void StepPhysics(SbSlot *s, int16 hand_dx, int16 hand_dy, int edge_lock) {
  // Anchor (grip) at bottom-centre.  Remains at rest every step; the mesh
  // flows around it.
  const int anchor = 4 * SB_N + 2;

  int16 ax[SB_N * SB_N] = { 0 };
  int16 ay[SB_N * SB_N] = { 0 };

  // 1. Gravity.  Nodes above the anchor droop more than those near it.
  for (int gy = 0; gy < SB_N; ++gy)
    for (int gx = 0; gx < SB_N; ++gx) {
      int i = gy * SB_N + gx;
      int dist_from_anchor = (4 - gy);
      if (dist_from_anchor < 0) dist_from_anchor = 0;
      ay[i] += (int16)(2 + dist_from_anchor);
    }

  // 2. Anchor-to-rest spring (~k = 1/16): deformations decay back.
  for (int i = 0; i < SB_N * SB_N; ++i) {
    ax[i] -= s->ox[i] >> 4;
    ay[i] -= s->oy[i] >> 4;
  }

  // 3. Structural (3/8) + shear (1/8) neighbour springs.
  #define SB_LINK(a_idx, b_idx, knum, kden)                              \
      do {                                                               \
        int16 ex = s->ox[b_idx] - s->ox[a_idx];                          \
        int16 ey = s->oy[b_idx] - s->oy[a_idx];                          \
        int16 fx = (ex * (knum)) / (kden);                               \
        int16 fy = (ey * (knum)) / (kden);                               \
        ax[a_idx] += fx; ay[a_idx] += fy;                                \
        ax[b_idx] -= fx; ay[b_idx] -= fy;                                \
      } while (0)
  for (int gy = 0; gy < SB_N; ++gy) {
    for (int gx = 0; gx < SB_N; ++gx) {
      int i = gy * SB_N + gx;
      if (gx + 1 < SB_N) SB_LINK(i, i + 1,     3, 8);
      if (gy + 1 < SB_N) SB_LINK(i, i + SB_N,  3, 8);
      if (gx + 1 < SB_N && gy + 1 < SB_N) SB_LINK(i, i + SB_N + 1, 1, 8);
      if (gx > 0       && gy + 1 < SB_N) SB_LINK(i, i + SB_N - 1, 1, 8);
    }
  }
  #undef SB_LINK

  // 4. Hand-motion inertia: each non-anchor node fights hand motion by
  //    picking up an opposing velocity kick, scaled by distance from grip.
  for (int gy = 0; gy < SB_N; ++gy) {
    for (int gx = 0; gx < SB_N; ++gx) {
      int i = gy * SB_N + gx;
      if (i == anchor) continue;
      int strength = (4 - gy) + 1;
      s->vx[i] -= (int16)(hand_dx * SB_FP * strength / 4);
      s->vy[i] -= (int16)(hand_dy * SB_FP * strength / 4);
    }
  }

  // 5. Integrate with damping.  Clamp offsets — edge-lock mode keeps them
  //    tight so warped pixels never leave the 16x16 footprint; expanded
  //    mode lets them fill most of the 32x32 canvas.
  int16 lim = edge_lock ? (int16)(3 * SB_FP) : (int16)(14 * SB_FP);
  for (int i = 0; i < SB_N * SB_N; ++i) {
    s->vx[i] += ax[i];
    s->vy[i] += ay[i];
    s->vx[i] = (int16)((s->vx[i] * 13) / 16);
    s->vy[i] = (int16)((s->vy[i] * 13) / 16);
    s->ox[i] += s->vx[i];
    s->oy[i] += s->vy[i];
    if (s->ox[i] >  lim) s->ox[i] =  lim;
    if (s->ox[i] < -lim) s->ox[i] = -lim;
    if (s->oy[i] >  lim) s->oy[i] =  lim;
    if (s->oy[i] < -lim) s->oy[i] = -lim;
  }

  // 6. Edge-lock: force outer ring of points to rest so the silhouette
  //    stays crisp inside the 16x16 tile window.
  if (edge_lock) {
    for (int gy = 0; gy < SB_N; ++gy) {
      for (int gx = 0; gx < SB_N; ++gx) {
        if (gx == 0 || gx == SB_N - 1 || gy == 0 || gy == SB_N - 1) {
          int i = gy * SB_N + gx;
          s->ox[i] = 0; s->oy[i] = 0;
          s->vx[i] = 0; s->vy[i] = 0;
        }
      }
    }
  }

  // Anchor pinned.
  s->ox[anchor] = 0; s->oy[anchor] = 0;
  s->vx[anchor] = 0; s->vy[anchor] = 0;
}

// ─── Forward-map warp ────────────────────────────────────────────────────

// Map every source pixel through the deformed lattice into an
// `canvas_size x canvas_size` canvas.  The original 16x16 rest position is
// placed at `(margin, margin)`, so expanded mode (margin=8) gives 8 pixels
// of slack on every side for deformation to extend into.
static void WarpImage(const SbSlot *s, uint8 *dst, int canvas_size, int margin) {
  int total_px = canvas_size * canvas_size;
  for (int i = 0; i < total_px; ++i) dst[i] = 0;

  const int denom = SB_SUB_PX / SB_CELLS;   // sub-samples per cell edge

  for (int ssy = 0; ssy < SB_SUB_PX; ++ssy) {
    for (int ssx = 0; ssx < SB_SUB_PX; ++ssx) {
      int sx = ssx >> 2;
      int sy = ssy >> 2;
      uint8 px = s->src_pix[sy * SB_PX + sx];
      if (px == 0) continue;

      int cx = ssx / denom;
      int cy = ssy / denom;
      if (cx >= SB_CELLS) cx = SB_CELLS - 1;
      if (cy >= SB_CELLS) cy = SB_CELLS - 1;
      int ux = ssx - cx * denom;
      int uy = ssy - cy * denom;

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

      int dx_sub = ssx + (dox * SB_SUB) / SB_FP;
      int dy_sub = ssy + (doy * SB_SUB) / SB_FP;
      int dx = (dx_sub >> 2) + margin;
      int dy = (dy_sub >> 2) + margin;
      if ((unsigned)dx >= (unsigned)canvas_size ||
          (unsigned)dy >= (unsigned)canvas_size) continue;
      dst[dy * canvas_size + dx] = px;
    }
  }
}

// ─── Scratch-pool allocator ──────────────────────────────────────────────

static int8 AllocPool(void) {
  for (int p = 0; p < SB_POOL_COUNT; ++p) {
    if (!g_pool_used[p]) { g_pool_used[p] = 1; return (int8)p; }
  }
  return -1;
}
static void FreePool(int8 p) {
  if (p >= 0 && p < SB_POOL_COUNT) g_pool_used[p] = 0;
}

// ─── Activation / deactivation ───────────────────────────────────────────

static void ResetMesh(SbSlot *s) {
  for (int i = 0; i < SB_N * SB_N; ++i) {
    s->ox[i] = 0; s->oy[i] = 0;
    s->vx[i] = 0; s->vy[i] = 0;
  }
}

static void ActivateEdgeLock(uint8 k, uint8 ch, uint8 bank, uint8 idx) {
  SbSlot *s = &g_sb[k];
  s->pool_id = -1;
  s->oamindex = idx;
  ComputeQuadTileAddrs(ch, bank, s->orig_tile_vaddr);
  SnapshotTiles(s->orig_tile_vaddr, 4, s->orig_tile_word);
  DecodeSprite16(s->orig_tile_vaddr, s->src_pix);
  ResetMesh(s);
  s->prev_spr_x = GetSprXPos(k);
  s->prev_spr_y = GetSprYPos(k);
  s->mode = 2;
}

static void ActivateExpanded(uint8 k, uint8 ch, uint8 bank, uint8 idx) {
  int8 pool = AllocPool();
  if (pool < 0) {
    // Pool full — degrade to the 16x16 edge-locked path so the sprite is
    // still animated, just not expanded.
    ActivateEdgeLock(k, ch, bank, idx);
    return;
  }
  SbSlot *s = &g_sb[k];
  s->pool_id = pool;
  s->oamindex = idx;

  // Source pixels come from the sprite's own 4 tiles; we never modify them.
  uint16 src_addrs[4];
  ComputeQuadTileAddrs(ch, bank, src_addrs);
  DecodeSprite16(src_addrs, s->src_pix);

  // Resolve all 16 scratch tile VRAM addresses (4 quadrants × 4 tiles).
  for (int q = 0; q < 4; ++q) {
    uint16 quad_addrs[4];
    ComputeQuadTileAddrs(kPoolQuadCharnum[pool][q], 1, quad_addrs);
    for (int t = 0; t < 4; ++t) s->scratch_vaddr[q * 4 + t] = quad_addrs[t];
  }
  SnapshotTiles(s->scratch_vaddr, 16, s->scratch_orig);

  ResetMesh(s);
  s->prev_spr_x = GetSprXPos(k);
  s->prev_spr_y = GetSprYPos(k);
  s->mode = 1;
}

static void SoftBodyDeactivate(uint8 k) {
  SbSlot *s = &g_sb[k];
  if (!s->mode) return;

  if (s->mode == 1) {
    RestoreTiles(s->scratch_vaddr, 16, s->scratch_orig);
    // Hide the 4 extra OAM entries we pushed each frame.  The sprite's
    // own oam[64] is being re-written by whatever status the sprite is
    // transitioning into, so we leave it alone.
    OamEnt *oam = get_OamEnt(oam_buf, s->oamindex);
    oam[65].ypos = 0xF0;
    oam[66].ypos = 0xF0;
    oam[67].ypos = 0xF0;
    oam[68].ypos = 0xF0;
    uint8 slot = s->oamindex >> 2;
    sprites_oamtile_size_buffer[slot + 65] = 0;
    sprites_oamtile_size_buffer[slot + 66] = 0;
    sprites_oamtile_size_buffer[slot + 67] = 0;
    sprites_oamtile_size_buffer[slot + 68] = 0;
  } else if (s->mode == 2) {
    RestoreTiles(s->orig_tile_vaddr, 4, s->orig_tile_word);
  }

  FreePool(s->pool_id);
  s->pool_id = -1;
  s->mode = 0;
}

// ─── Public entry points ─────────────────────────────────────────────────

void SoftBodyResetAll(void) {
  for (int k = 0; k < 12; ++k) {
    g_sb[k].mode = 0;
    g_sb[k].pool_id = -1;
  }
  for (int p = 0; p < SB_POOL_COUNT; ++p) g_pool_used[p] = 0;
}

void SoftBodyOnCarriedFrame(uint8 k) {
  // P-balloon uses its own OAM magic; never soft-body it.
  if (spr_spriteid[k] == 125) {
    SoftBodyDeactivate(k);
    return;
  }
  uint8 idx = spr_oamindex[k];
  if (idx == 0) {
    // Sprite is being temporarily hidden (turning around, pipe, face-screen
    // pose).  Let it reset — it'll reactivate on the next visible frame.
    SoftBodyDeactivate(k);
    return;
  }

  // Shell bodies land at oam[66] because StunnedShellGFXRt_01980F bumps the
  // oam index by +8 bytes; everything else writes its 16x16 to oam[64].
  bool is_shell = (spr_property_bits167a[k] & 8) == 0;
  uint8 ent_off = is_shell ? 66 : 64;

  OamEnt *oam = get_OamEnt(oam_buf, idx);
  uint8 slot = idx >> 2;
  if ((sprites_oamtile_size_buffer[slot + ent_off] & 2) == 0) {
    // Sprite wasn't drawn as 16x16 this frame.
    return;
  }
  uint8 ch   = oam[ent_off].charnum;
  uint8 bank = oam[ent_off].flags & 1;

  SbSlot *s = &g_sb[k];
  uint8 desired_mode = is_shell ? 2 : 1;

  // Rebind if anything about the identity changed.
  if (s->mode != desired_mode || s->oamindex != idx) {
    if (s->mode) SoftBodyDeactivate(k);
    if (is_shell) ActivateEdgeLock(k, ch, bank, idx);
    else          ActivateExpanded(k, ch, bank, idx);
  } else if (s->mode == 2) {
    // Animation frame change: our snapshot is stale.
    uint16 cur_addrs[4];
    ComputeQuadTileAddrs(ch, bank, cur_addrs);
    if (cur_addrs[0] != s->orig_tile_vaddr[0]) {
      SoftBodyDeactivate(k);
      ActivateEdgeLock(k, ch, bank, idx);
    }
  }
  if (!s->mode) return;   // activation couldn't happen (shouldn't occur)

  // Mode 1 never touches the source tiles, so refresh the decoded image
  // every frame — cheap, and handles animation transparently.
  if (s->mode == 1) {
    uint16 src_addrs[4];
    ComputeQuadTileAddrs(ch, bank, src_addrs);
    DecodeSprite16(src_addrs, s->src_pix);
  }

  // Frame-to-frame hand motion.
  uint16 cx = GetSprXPos(k), cy = GetSprYPos(k);
  int16 dx = (int16)cx - (int16)s->prev_spr_x;
  int16 dy = (int16)cy - (int16)s->prev_spr_y;
  if (dx >  16) dx =  16;
  if (dx < -16) dx = -16;
  if (dy >  16) dy =  16;
  if (dy < -16) dy = -16;
  s->prev_spr_x = cx;
  s->prev_spr_y = cy;

  // Freeze the mesh during the pickup pose so there's no pop on grab.
  if (timer_display_player_pick_up_pose) {
    ResetMesh(s);
  } else {
    StepPhysics(s, dx, dy, s->mode == 2);
  }

  if (s->mode == 2) {
    // 16x16 edge-locked: rewrite the sprite's own 4 tiles.
    uint8 warped[SB_PX * SB_PX];
    WarpImage(s, warped, SB_PX, 0);
    EncodeSprite16(warped, s->orig_tile_vaddr);
    return;
  }

  // mode == 1: expand to 32x32.  Snapshot the original OAM entry's pose
  // before hiding it, then lay down four quadrants around the anchor.
  uint8 x0 = oam[64].xpos;
  uint8 y0 = oam[64].ypos;
  uint8 flags0 = oam[64].flags;

  uint8 canvas[SB_CANVAS * SB_CANVAS];
  WarpImage(s, canvas, SB_CANVAS, SB_CANVAS_MARGIN);
  EncodeCanvas32(canvas, s->scratch_vaddr);

  // Hide the original 16x16 entry — its tiles are untouched and would
  // otherwise double-render on top of our expanded canvas.
  oam[64].ypos = 0xF0;
  sprites_oamtile_size_buffer[slot + 64] = 0;

  // Our scratch tiles live in bank 2, so force flags.bank = 1.  Flip bits
  // are preserved on each quadrant but quadrant POSITION isn't flipped;
  // this mirrors how a single 16x16 OAM entry handles its tile layout.
  uint8 quad_flags = (flags0 & ~1) | 1;
  uint8 highx = spr_xoffscreen_flag[k] & 1;
  for (int q = 0; q < 4; ++q) {
    int qx = (q & 1) ? 8 : -8;
    int qy = (q & 2) ? 8 : -8;
    oam[65 + q].xpos    = (uint8)(x0 + qx);
    oam[65 + q].ypos    = (uint8)(y0 + qy);
    oam[65 + q].charnum = kPoolQuadCharnum[s->pool_id][q];
    oam[65 + q].flags   = quad_flags;
    sprites_oamtile_size_buffer[slot + 65 + q] = highx | 2;
  }
}

void SoftBodyPollAllSlots(void) {
  for (uint8 k = 0; k < 12; ++k) {
    if (g_sb[k].mode && spr_current_status[k] != 11)
      SoftBodyDeactivate(k);
  }
}
