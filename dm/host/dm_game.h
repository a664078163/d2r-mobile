// 游戏 profile：把通用 Horizon 宿主与具体游戏能力隔开。
// Game profile: keeps the common Horizon host separate from game-specific features.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    DM_GAME_D2R = 0,
    DM_GAME_ACNH = 1,
    DM_GAME_D3 = 2,
} dm_game_kind;

dm_game_kind dm_game_current(void);
int dm_game_is_d2r(void);
int dm_game_is_acnh(void);
int dm_game_is_d3(void);
const char *dm_game_name(void);
const char *dm_game_argv0(void);
