#pragma once
#include "dm_touch_game_state.h"
enum { DM_TA_UNKNOWN=0, DM_TA_EMPTY=1, DM_TA_GLYPH=2, DM_TA_ATLASES=9 };
typedef struct { const char *asset; int width,height,frame_width,pitch,count; } dm_ta_atlas;
typedef struct { int x,y,width,height; } dm_ta_frame;
typedef struct { int status,atlas,normal_frame,pressed_frame,slot,effective_slot; uint32_t raw_skill_id; const char *reason; } dm_ta_glyph;
extern const dm_ta_atlas dm_ta_atlases[DM_TA_ATLASES];
extern const char dm_ta_manifest_sha[],dm_ta_geometry_sha[],dm_ta_binding_sha[];
dm_ta_glyph dm_ta_skill(uint32_t raw_skill_id);
dm_ta_glyph dm_ta_current(const dm_tg_snapshot *s,uint64_t sdk_button,uint64_t host_now_ns);
/* 同上，skill_set=1 取第二套（alt 槽，原版按住 ZL） | Same as above; skill_set=1 takes the second set (alt slots, hold ZL in the original game) */
dm_ta_glyph dm_ta_current_set(const dm_tg_snapshot *s,uint64_t sdk_button,uint64_t host_now_ns,int skill_set);
/* 画「＋」与「点空位进绑定」共用的一份读数：快照只是旧了（游戏停拍、切后台那一瞬、长卡顿）时
   退回最后一次取到的内容——绑定没变，只是暂时没人刷新。绘制与输入都必须用这一份，否则会出现
   「画着＋」但点下去被当成普通施放的裂缝。 | The one reading shared by drawing the 「＋」 and by
   "tap an empty slot to bind": when the snapshot is merely stale (game ticks stalled, the instant of going
   to background, a long hitch) it falls back to the content captured last -- the binding has not changed,
   nobody is refreshing it for the moment. Both drawing and input must use this one, otherwise a slot can
   show 「＋」 while a tap is treated as an ordinary cast. */
static inline dm_ta_glyph dm_ta_current_set_shown(const dm_tg_snapshot *s,uint64_t sdk_button,uint64_t host_now_ns,int skill_set) {
    dm_ta_glyph g=dm_ta_current_set(s,sdk_button,host_now_ns,skill_set);
    if(g.status==DM_TA_UNKNOWN && s && s->captured_ns && host_now_ns>s->captured_ns && host_now_ns-s->captured_ns>DM_TG_MAX_AGE_NS)
        g=dm_ta_current_set(s,sdk_button,s->captured_ns,skill_set);
    return g;
}
/* 空技能位的唯一判据：绘制那个「＋」和「点一下进绑定」都用这一个。
   manual_empty = 玩家把键拖到空白处留下的本地空位；另一个来源是游戏自己报的空位（槽里 raw id = UINT32_MAX）。
   注意「不知道」（快照还没就绪、旧包没有这张表）不算空位：那时技能钮照常施放，只是画不出图标。
   The one predicate for an empty skill slot, used both to draw the 「＋」 and to decide "a tap enters binding".
   manual_empty = the local empty slot left by dragging a key onto blank space; the other source is the slot the
   game itself reports as empty (raw id = UINT32_MAX in the slot). Note that "unknown" (the snapshot is not ready
   yet, or an old package without this table) does NOT count as empty: the skill button still casts then, it just
   cannot draw an icon. */
static inline int dm_ta_slot_empty(const dm_tg_snapshot *s,uint64_t sdk_button,uint64_t host_now_ns,int skill_set,int manual_empty) {
    if(manual_empty)return 1;
    return dm_ta_current_set_shown(s,sdk_button,host_now_ns,skill_set).status==DM_TA_EMPTY;
}
int dm_ta_frame_at(int atlas,int frame,dm_ta_frame *out);
