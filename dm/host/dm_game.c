// 游戏 profile：把通用 Horizon 宿主与具体游戏能力隔开。
// Game profile: keeps the common Horizon host separate from game-specific features.
#include "dm_game.h"
#include "dm.h"
#include <string.h>

static dm_game_kind g_game = (dm_game_kind)-1;

dm_game_kind dm_game_current(void) {
    if (g_game != (dm_game_kind)-1) return g_game;
    const char *v = getenv("DM_GAME_PROFILE");
    if (!v || !*v || !strcmp(v, "d2r") || !strcmp(v, "diablo2")) {
        g_game = DM_GAME_D2R;
    } else if (!strcmp(v, "acnh") || !strcmp(v, "animal-crossing") || !strcmp(v, "animal_crossing")) {
        g_game = DM_GAME_ACNH;
    } else if (!strcmp(v, "d3") || !strcmp(v, "diablo3") || !strcmp(v, "diablo-3") || !strcmp(v, "diablo_3")) {
        g_game = DM_GAME_D3;
    } else {
        die("未知游戏 profile「%s」；可用 d2r、acnh 或 d3", v);
    }
    LOG("游戏 profile：%s", g_game == DM_GAME_ACNH ? "acnh（动物森友会）" :
        g_game == DM_GAME_D3 ? "d3（暗黑破坏神 III：永恒之战）" : "d2r（暗黑破坏神 II：狱火重生）");
    return g_game;
}

int dm_game_is_d2r(void) { return dm_game_current() == DM_GAME_D2R; }
int dm_game_is_acnh(void) { return dm_game_current() == DM_GAME_ACNH; }
int dm_game_is_d3(void) { return dm_game_current() == DM_GAME_D3; }
const char *dm_game_name(void) { return dm_game_is_acnh() ? "Animal Crossing: New Horizons" : dm_game_is_d3() ? "Diablo III: Eternal Collection" : "Diablo II: Resurrected"; }
const char *dm_game_argv0(void) { return dm_game_is_acnh() ? "AnimalCrossing" : dm_game_is_d3() ? "DiabloIII" : "D2R"; }
