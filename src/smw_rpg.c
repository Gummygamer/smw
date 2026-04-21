#include "smw_rpg.h"
#include "types.h"
#include "common_rtl.h"
#include "variables.h"

uint8  g_player_level   = 0;
uint16 g_player_xp      = 0;
uint8  g_player_max_hp  = 0;
uint8  g_player_hp      = 0;
uint8  g_player_attack  = 0;
uint8  g_player_defense = 0;

// Returns the base XP reward for a sprite ID (0 = not an enemy).
static uint8 SpriteBaseXp(uint8 id) {
    switch (id) {
    // Goombas
    case 0x00: case 0x01: return 2;
    case 0x02: case 0x03: case 0x10: case 0x11: return 3;   // Para-goombas
    // Spinies
    case 0x04: case 0x05: case 0x06: return 4;
    case 0x07: return 5;                                      // Para-spiny
    // Koopa Troopas
    case 0x08: case 0x09: return 3;
    case 0x0A: case 0x0B: return 4;                          // Para-koopas
    case 0x0D: return 3;                                      // Hopping Koopa
    case 0x34: return 3;                                      // Fast Koopa
    case 0x50: case 0x51: return 3;                          // Koopa (face screen)
    case 0x52: case 0x53: case 0x54: case 0x55: return 3;   // Net Koopas
    // Bob-ombs
    case 0x12: return 5;
    case 0x5D: return 6;                                      // Para Bob-omb
    // Cheep-cheeps
    case 0x14: case 0x15: case 0x16: return 2;
    // Piranha Plants
    case 0x1A: case 0x1B: case 0x2A: case 0x2B: return 4;
    case 0x31: return 4;                                      // Running Piranha
    case 0x39: return 4;                                      // Upside-Down Piranha
    case 0x66: return 4;                                      // Jumping Piranha
    case 0x67: return 5;                                      // Fire Piranha
    // Bullet Bills
    case 0x1C: return 3;
    case 0x82: return 7;                                      // Banzai Bill
    case 0x1D: return 3;                                      // Hopping Flame
    // Lakitu / Magikoopa
    case 0x1E: return 6;
    case 0x1F: case 0x20: return 6;
    // Boos
    case 0x21: return 5;
    case 0x25: return 8;                                      // Big Boo
    // Thwomp / Thwimp
    case 0x26: return 6;
    case 0x27: return 4;
    // Koopa Kid bosses (0x29 = generic Koopa Kid; 0x89–0x8F = named)
    case 0x29: case 0x89: case 0x8A: case 0x8B:
    case 0x8C: case 0x8D: case 0x8E: case 0x8F: return 15;
    case 0x87: return 20;                                     // Bowser
    case 0x88: return 12;                                     // Reznor
    // Blargg
    case 0x2F: case 0x30: return 5;
    // Urchin
    case 0x37: case 0x38: return 4;
    // Shy Guy on stilts
    case 0x35: return 4;
    // Buzzy Beetles
    case 0x41: case 0x42: case 0x43: case 0x44: return 3;
    case 0x61: return 3;                                      // Buzzy (walking)
    // Hammer / Fire / Boomerang / Sledge Bro
    case 0x56: case 0x58: case 0x59: return 7;
    case 0x57: return 8;                                      // Sledge Bro
    case 0x40: return 6;                                      // Wrenches (Larry/Morton)
    // Dry Bones / Sumo Bro
    case 0x5A: case 0x5B: return 5;
    case 0x5C: return 7;                                      // Sumo Bro
    // Walking Fireball
    case 0x5E: return 4;
    // Torpedo Ted
    case 0x63: return 5;
    // Monty Mole
    case 0x64: case 0x65: return 4;
    // Wiggler
    case 0x6C: return 6;
    case 0x86: return 8;                                      // Wiggler head
    // Fish Bone
    case 0x6D: return 4;
    // Dino
    case 0x6F: return 5;                                      // Dino-Torch
    case 0x70: return 6;                                      // Dino-Rhino
    // Volcano Lotus
    case 0x71: return 4;
    // Chargin' Chucks (four types)
    case 0x72: case 0x73: case 0x74: case 0x75: return 5;
    // Pokey
    case 0x78: return 5;
    // Rex / Porcu-Puffer
    case 0x7C: return 4;
    case 0x7D: return 5;
    // Eerie
    case 0x7E: return 5;
    // Rip Van Fish
    case 0x80: return 4;
    // Falling Spike
    case 0x69: return 3;
    default:   return 0;                                      // non-enemy
    }
}

// XP required to advance from the given level to the next.
static uint16 RpgXpToNextLevel(uint8 level) {
    return (uint16)(level + 1) * 10;  // LV1→LV2: 20, LV2→LV3: 30, …
}

void RpgInit(void) {
    g_player_level   = 1;
    g_player_xp      = 0;
    g_player_max_hp  = 5;
    g_player_hp      = 5;
    g_player_attack  = 1;
    g_player_defense = 0;
}

void RpgRestoreHp(void) {
    g_player_hp = g_player_max_hp;
}

static void RpgDoLevelUp(void) {
    ++g_player_level;

    // Max HP: +2 per level up to LV10, +1 per level after that.
    uint8 hp_gain = (g_player_level <= 10) ? 2 : 1;
    uint8 new_max = g_player_max_hp + hp_gain;
    g_player_max_hp = (new_max > 99) ? 99 : new_max;

    ++g_player_attack;
    if (g_player_level % 3 == 0 && g_player_defense < 99)
        ++g_player_defense;

    // Full heal on level up.
    g_player_hp = g_player_max_hp;
}

uint8 RpgAwardXp(uint8 sprite_id) {
    uint8 base_xp = SpriteBaseXp(sprite_id);
    if (base_xp == 0) return 0;

    // Attack stat gives a 10% XP bonus per point (integer arithmetic).
    uint16 bonus = (uint16)base_xp * g_player_attack / 10;
    uint16 xp_gained = base_xp + (uint8)bonus;

    uint8 leveled_up = 0;
    g_player_xp += xp_gained;
    while (g_player_level < 99) {
        uint16 needed = RpgXpToNextLevel(g_player_level);
        if (g_player_xp < needed) break;
        g_player_xp -= needed;
        RpgDoLevelUp();
        leveled_up = 1;
    }
    return leveled_up;
}

uint8 RpgTakeDamage(void) {
    if (g_player_hp > 0) --g_player_hp;

    // Invincibility duration: base 0x2F frames, +4 frames per defense point.
    uint16 dur = 0x2Fu + (uint16)g_player_defense * 4u;
    return (dur > 0xFF) ? 0xFF : (uint8)dur;
}

// Base HP for each sprite, independent of world scaling.
static uint8 SpriteBaseHp(uint8 id) {
    switch (id) {
    // Bosses — high base HP
    case 0x87: return 8;   // Bowser
    case 0x29: case 0x89: case 0x8A: case 0x8B:
    case 0x8C: case 0x8D: case 0x8E: case 0x8F: return 5;  // Koopa Kids
    case 0x88: return 4;   // Reznor
    case 0x86: return 3;   // Wiggler head
    // Mini-bosses / tough enemies
    case 0x57: return 3;   // Sledge Bro
    case 0x25: return 3;   // Big Boo
    case 0x26: return 2;   // Thwomp
    case 0x5C: return 2;   // Sumo Bro
    case 0x1E: return 2;   // Lakitu
    case 0x1F: case 0x20: return 2;  // Magikoopa
    // Standard enemies — 1 HP (die in one hit, matches classic SMW)
    default:   return (SpriteBaseXp(id) > 0) ? 1 : 0;
    }
}

// Derive world bracket (0–7) from ow_level_number_lo.
// Used to scale enemy HP for later worlds.
static uint8 CurrentWorldBracket(void) {
    uint8 lvl = ow_level_number_lo;
    if (lvl < 5)  return 0;   // World 1
    if (lvl < 12) return 1;   // World 2
    if (lvl < 20) return 2;   // World 3
    if (lvl < 28) return 3;   // World 4
    if (lvl < 38) return 4;   // World 5
    if (lvl < 45) return 5;   // World 6
    if (lvl < 52) return 6;   // World 7
    return 7;                  // Special / Star world
}

void RpgInitSpriteHp(uint8 k) {
    uint8 base = SpriteBaseHp(spr_spriteid[k]);
    if (base == 0) {
        spr_table1504[k] = 0;
        return;
    }
    uint8 world = CurrentWorldBracket();
    // Bosses scale more aggressively: +1 HP per world bracket past W1.
    // Regular enemies get +1 HP every 2 world brackets (worlds 3, 5, 7).
    uint8 hp;
    uint8 id = spr_spriteid[k];
    uint8 is_boss = (id == 0x87 || id == 0x88 ||
                     (id >= 0x29 && id <= 0x29) ||
                     (id >= 0x89 && id <= 0x8F) ||
                     id == 0x86);
    if (is_boss) {
        hp = base + world;
    } else {
        hp = base + (world / 2);
    }
    spr_table1504[k] = (hp > 99) ? 99 : hp;
}

uint8 RpgHitEnemy(uint8 k) {
    if (spr_table1504[k] == 0) return 1;  // non-enemy, treat as instant kill
    if (spr_table1504[k] > 0) --spr_table1504[k];
    return spr_table1504[k] == 0;
}
