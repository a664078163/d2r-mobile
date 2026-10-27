// ACNH 新增的纹理构造器查询：不创建 NVN 设备、不碰 Metal，验证默认视图偏移与其调用点约束。
// ACNH 生产调用点：先给对齐值补上第 0 级偏移，再按对齐值向上取整。
// ACNH's added texture-builder query: no NVN device or Metal is created; verifies the default-view offset
// and its call-site constraint. ACNH adds the level-0 offset to the alignment first, then rounds up.
#include "../nvn/nvn_tex.h"
#include "../nvn/include/nvn.h"
#include <stdio.h>
#include <string.h>

static int failures, checks;

#define EXPECT(name, want, got) do { \
    long long w_ = (long long)(want), g_ = (long long)(got); checks++; \
    printf("%-52s 期望 %-10lld -> 真结果 %-10lld %s\n", (name), w_, g_, w_ == g_ ? "✓" : "✗"); \
    failures += w_ != g_; \
} while (0)

static nvn_texdesc desc2d(int w, int h, int levels, int format, int flags) {
    nvn_texdesc d; memset(&d, 0, sizeof d);
    d.target = NVN_TEXTURE_TARGET_2D; d.width = (uint32_t)w; d.height = (uint32_t)h; d.depth = 1;
    d.levels = (uint32_t)levels; d.format = format; d.flags = flags;
    return d;
}

static void check_plain(const char *name, nvn_texdesc d) {
    nvn_texlayout L; nvn_tex_layout(&d, &L);
    uint64_t view = nvn_tex_level_offset(&d, 0);
    uint64_t rounded = (view + L.align - 1) / L.align;
    char label[128];
    snprintf(label, sizeof label, "%s：第 0 级视图偏移", name);
    EXPECT(label, 0, view);
    snprintf(label, sizeof label, "%s：对齐值可用于向上取整", name);
    EXPECT(label, 1, L.align > 0);
    snprintf(label, sizeof label, "%s：ACNH 对齐取整", name);
    EXPECT(label, 0, rounded);
}

int main(void) {
    check_plain("2D RGBA8 256x256", desc2d(256, 256, 1, NVN_FORMAT_RGBA8, 0));
    check_plain("2D RGBA8 100x100", desc2d(100, 100, 1, NVN_FORMAT_RGBA8, 0));
    check_plain("2D RGBA8 256x256 mip 9", desc2d(256, 256, 9, NVN_FORMAT_RGBA8, 0));
    check_plain("2D DXT1 128x128", desc2d(128, 128, 1, NVN_FORMAT_RGBA_DXT1, 0));
    check_plain("LINEAR RGBA8 100x100", desc2d(100, 100, 1, NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_LINEAR));

    nvn_texdesc p = desc2d(256, 256, 1, NVN_FORMAT_RGBA8, 0);
    p.pkg_block_h = 2;   // SetPackagedTextureLayout 第 0 字 = 1，存储值再 +1 = 2 | layout byte 0 = 1, plus the stored +1 = 2
    nvn_texlayout a, b; nvn_tex_layout(&p, &a);
    p.pkg_block_h = 0; nvn_tex_layout(&p, &b);
    EXPECT("预排布布局参与尺寸计算（与自动算法可区分）", 1, a.size != b.size || a.tileH != b.tileH);
    EXPECT("预排布布局：第 0 级视图偏移", 0, nvn_tex_level_offset(&p, 0));

    printf("== %d 项，失败 %d ==\n", checks, failures);
    return failures != 0;
}
