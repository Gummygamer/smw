#pragma once
#include "types.h"

// Player RPG stats – global C variables (not SNES RAM)
extern uint8 g_player_level;    // 1–99
extern uint16 g_player_xp;      // XP toward next level
extern uint8 g_player_max_hp;   // Maximum HP
extern uint8 g_player_hp;       // Current HP
extern uint8 g_player_attack;   // Attack stat (XP multiplier, +1 per level)
extern uint8 g_player_defense;  // Defense stat (extends invincibility, +1 per 3 levels)

// Initialize stats to level 1 defaults.
void RpgInit(void);

// Restore HP to max (called on level entry after initialization).
void RpgRestoreHp(void);

// Award XP for defeating the sprite with the given sprite_id.
// Returns 1 if the player leveled up, 0 otherwise.
uint8 RpgAwardXp(uint8 sprite_id);

// Apply one hit of damage to the player.
// Returns the invincibility duration to use for the hurt timer.
uint8 RpgTakeDamage(void);
