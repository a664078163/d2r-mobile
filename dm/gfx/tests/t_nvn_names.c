// NVN 名字表回归：不创建设备、不碰 Metal，验证 ACNH 从原生注册表恢复的接口都能按名字取到跳板。
// NVN name-table regression: does not create a device or touch Metal; verifies every interface
// recovered from ACNH's native registration table resolves to a thunk by name.
#include "../nvn/nvn_protos.h"
#include "../nvn/nvn_count.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

extern const char *nvn_names[NVN_NFUNC];
extern void *nvn_thunk[NVN_NFUNC];
struct { _Atomic uint64_t cnt; void *fn; } nvn_slot[NVN_NFUNC];

static const char *const k_acnh_added[] = {
    "nvnTextureBuilderGetViewOffset",
    "nvnProgramGetSubtileSize",
    "nvnCommandBufferSetControlMemoryCallbackEnabled",
    "nvnCommandBufferBindUniformBuffersDeferred",
    "nvnCommandBufferBindStorageBuffersDeferred",
    "nvnCommandBufferBindTexturesDeferred",
    "nvnCommandBufferBindImagesDeferred",
    "nvnCommandBufferBindSeparateTexturesDeferred",
    "nvnCommandBufferBindSeparateSamplersDeferred",
    "nvnCommandBufferOverrideSubtileSize",
    "nvnCommandBufferSetZCullZF32CompressionEnable",
    "nvnCommandBufferSetProvokingVertexMode",
    "nvnDeviceSetAutomaticCheckpointsMask",
};

static int failures, checks;

#define EXPECT(name, want, got) do { \
    unsigned long long w_ = (unsigned long long)(want), g_ = (unsigned long long)(got); \
    checks++; \
    printf("%-52s 期望 %-5llu -> 真结果 %-5llu %s\n", (name), w_, g_, w_ == g_ ? "✓" : "✗"); \
    failures += w_ != g_; \
} while (0)

static int name_index(const char *name) {
    for (unsigned i = 0; i < NVN_NFUNC; i++)
        if (!strcmp(nvn_names[i], name)) return (int)i;
    return -1;
}

int main(void) {
    EXPECT("名字表大小", 536, NVN_NFUNC);

    int unique = 1;
    for (unsigned i = 0; i < NVN_NFUNC && unique; i++) {
        if (!nvn_names[i] || !nvn_thunk[i] || name_index(nvn_names[i]) != (int)i) unique = 0;
        for (unsigned j = 0; j < i; j++)
            if (!strcmp(nvn_names[i], nvn_names[j])) { unique = 0; break; }
    }
    EXPECT("名字唯一且每个都有跳板", 1, unique);

    unsigned found = 0;
    for (unsigned i = 0; i < sizeof k_acnh_added / sizeof *k_acnh_added; i++) {
        int at = name_index(k_acnh_added[i]);
        printf("  %-54s %s\n", k_acnh_added[i], at >= 0 && nvn_thunk[at] ? "✓" : "✗");
        found += at >= 0 && nvn_thunk[at] != NULL;
    }
    EXPECT("ACNH 新增名字全部可取址", sizeof k_acnh_added / sizeof *k_acnh_added, found);

    printf("== %d 项，失败 %d ==\n", checks, failures);
    return failures != 0;
}
