#include "consts.h"
#include "funcs.h"
#include "smw_rtl.h"
#include "variables.h"
#include <math.h>

// Gravitational constant — tuned so two entities ~64px apart attract at ~0.4 px/frame²
#define NG_G          50.0f
// Softening length² (pixels²) — prevents singularity; effective min distance ~20px
#define NG_SOFTENING   400.0f
// Max sprite gravitational speed (pixels/frame)
#define NG_SPR_MAXV      5.0f

// Sprite gravitational velocity accumulators (pixels/frame)
static float ng_spr_vx[12], ng_spr_vy[12];
// Sprite sub-pixel position remainder
static float ng_spr_rx[12], ng_spr_ry[12];
// Player gravitational acceleration remainder (speed units = 1/16 px/frame)
static float ng_player_ax_rem, ng_player_ay_rem;

void NewtonianGravity_Update(void) {
    float px = (float)player_xpos;
    float py = (float)player_ypos;

    float sx[12], sy[12];
    bool active[12];
    for (int i = 0; i < 12; i++) {
        active[i] = spr_current_status[i] >= 8;
        if (active[i]) {
            sx[i] = (float)GetSprXPos((uint8)i);
            sy[i] = (float)GetSprYPos((uint8)i);
        } else {
            ng_spr_vx[i] = ng_spr_vy[i] = 0.0f;
            ng_spr_rx[i] = ng_spr_ry[i] = 0.0f;
        }
    }

    // Player gravitational acceleration from all active sprites
    float pax = 0.0f, pay = 0.0f;
    for (int i = 0; i < 12; i++) {
        if (!active[i]) continue;
        float dx = sx[i] - px;
        float dy = sy[i] - py;
        float r2 = dx * dx + dy * dy + NG_SOFTENING;
        float inv_r = 1.0f / sqrtf(r2);
        float a = NG_G * inv_r * inv_r * inv_r;
        pax += a * dx;
        pay += a * dy;
        // Newton's 3rd law: sprite attracted toward player
        ng_spr_vx[i] -= a * dx;
        ng_spr_vy[i] -= a * dy;
    }

    // Sprite–sprite gravity
    for (int i = 0; i < 12; i++) {
        if (!active[i]) continue;
        for (int j = i + 1; j < 12; j++) {
            if (!active[j]) continue;
            float dx = sx[j] - sx[i];
            float dy = sy[j] - sy[i];
            float r2 = dx * dx + dy * dy + NG_SOFTENING;
            float inv_r = 1.0f / sqrtf(r2);
            float a = NG_G * inv_r * inv_r * inv_r;
            ng_spr_vx[i] += a * dx;
            ng_spr_vy[i] += a * dy;
            ng_spr_vx[j] -= a * dx;
            ng_spr_vy[j] -= a * dy;
        }
    }

    // Apply player gravitational acceleration to player_xspeed / player_yspeed.
    // 1 speed unit = 1/16 px/frame, so multiply px/frame² by 16 to get speed units/frame.
    ng_player_ax_rem += pax * 16.0f;
    ng_player_ay_rem += pay * 16.0f;
    int dvx = (int)ng_player_ax_rem;
    int dvy = (int)ng_player_ay_rem;
    ng_player_ax_rem -= (float)dvx;
    ng_player_ay_rem -= (float)dvy;
    {
        int vx = (int)(int8)player_xspeed + dvx;
        int vy = (int)(int8)player_yspeed + dvy;
        if (vx >  127) vx =  127; else if (vx < -127) vx = -127;
        if (vy >   63) vy =   63; else if (vy < -127) vy = -127;
        player_xspeed = (uint8)(int8)vx;
        player_yspeed = (uint8)(int8)vy;
    }

    // Apply sprite gravitational velocity to positions
    for (int i = 0; i < 12; i++) {
        if (!active[i]) continue;
        if (ng_spr_vx[i] >  NG_SPR_MAXV) ng_spr_vx[i] =  NG_SPR_MAXV;
        if (ng_spr_vx[i] < -NG_SPR_MAXV) ng_spr_vx[i] = -NG_SPR_MAXV;
        if (ng_spr_vy[i] >  NG_SPR_MAXV) ng_spr_vy[i] =  NG_SPR_MAXV;
        if (ng_spr_vy[i] < -NG_SPR_MAXV) ng_spr_vy[i] = -NG_SPR_MAXV;

        ng_spr_rx[i] += ng_spr_vx[i];
        ng_spr_ry[i] += ng_spr_vy[i];
        int move_x = (int)ng_spr_rx[i];
        int move_y = (int)ng_spr_ry[i];
        ng_spr_rx[i] -= (float)move_x;
        ng_spr_ry[i] -= (float)move_y;
        if (move_x || move_y)
            AddSprXYPos((uint8)i, (uint16)(int16)move_x, (uint16)(int16)move_y);
    }
}
