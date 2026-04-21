#pragma once
#include "types.h"

// Per-pixel soft-body deformation for sprites Mario is holding.
//
// Each held sprite gets a 5x5 lattice of control points (a 4x4 grid of
// quadtree cells over a 16x16 pixel sprite).  Points obey spring + gravity
// + neighbor-constraint physics.  Each frame we resample the source pixel
// grid through the deformed mesh and write the warped result straight
// back to VRAM — no SNES OAM-tile rigidity.

// Reset all state.  Called on level load / slot clear.
void SoftBodyResetAll(void);

// Run the soft-body pass for the currently-carried sprite in slot k.
// Must be called after ProcessStunnedNormalSprite has written the sprite's
// OAM entry (we read the charnum / flags from there).
void SoftBodyOnCarriedFrame(uint8 k);

// Scan all 12 sprite slots; any slot that had an active body but is no
// longer in carried state has its original VRAM tile bytes restored.
// Call once per sprite-processing frame.
void SoftBodyPollAllSlots(void);
