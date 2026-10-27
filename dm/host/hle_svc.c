// Horizon 内核系统调用（装 SDK 后由 SDK 运行时触发）。只实现 P1 启动链要的：内存布局 + 堆 + 查询 + 调试输出 + 计时。
// 寄存器约定：入参 w1.. / 出参 w0=Result、x1=值（GetInfo / SetHeapSize），与公开 Horizon ABI 一致。
// Horizon kernel system calls (triggered by the SDK runtime once the SDK is loaded). Only what the P1 boot chain needs is implemented: memory layout + heap + query + debug output + timing.
// Register convention: inputs w1.. / outputs w0=Result, x1=value (GetInfo / SetHeapSize), consistent with the public Horizon ABI.
#include "dm.h"
#include "dm_game.h"
#include "hle.h"
#include <stdatomic.h>
#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>

// 预留一整块客体地址空间，按 Horizon 分区（地址值直接用宿主 mmap 得到的真地址）。只占地址、不占内存。
// 大小 5 GB：游戏要的是开头 1 GB（别名区、栈）+ 一整块堆（应用内存池 3285 MiB，见下面 APP_POOL），合计约 4.2 GB；
// iOS 上不带「扩展虚拟寻址」权限时一次最多能预留 6 GB（真机实读：6 GB 成、7 GB 不成），所以不能留得更宽——
// 留到 8 GB 的话，签不了那项权限的开发者账号（免费账号）出的包在这一步就退出。
// Reserve one whole block of guest address space, partitioned the Horizon way (address values are the real addresses returned by the host's mmap).
// It only takes address space, not memory.
// Size 5 GB: the game needs the first 1 GB (alias region, stacks) + one whole heap block (application memory pool of 3285 MiB, see APP_POOL below),
// about 4.2 GB in total; on iOS without the "extended virtual addressing" entitlement a single reservation can be at most 6 GB
// (read on a real device: 6 GB succeeds, 7 GB fails), so it cannot be made any wider --
// with an 8 GB reservation, builds from developer accounts that cannot sign that entitlement (free accounts) would exit at this step.
#define RESERVE (5ull << 30)
static uint8_t *g_as;              // ASLR 区基址 = 整块 | ASLR region base = the whole block
static uint8_t *g_heap; static size_t g_heap_cur, g_heap_cap; static int g_heap_rejects;
// 地址段整个落在客体堆当前已开放（可读写）的范围内：这种读不用陷内核核对，直接 memcpy（diag.c tk_rd 等每拍读客体内存的地方用）
// The address range lies entirely within the currently opened (readable and writable) part of the guest heap: such reads need no kernel trap to check, memcpy directly (used where guest memory is read every tick, such as diag.c tk_rd)
int dm_guest_heap_readable(uint64_t a, size_t n) { uint64_t b = (uint64_t)(uintptr_t)g_heap; size_t cur = g_heap_cur; return g_heap && a >= b && n <= cur && a - b <= cur - n; }
static uint8_t *g_alias; static size_t g_alias_cap;
static uint8_t *g_stack; static size_t g_stack_cap;

static void mem_init(void) {
    if (g_as) return;
    g_as = mmap(NULL, RESERVE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_as == MAP_FAILED) die("reserve guest AS");
    // 固定划分（都在预留块内）：alias 512MB、stack 256MB、heap 其余从 2GB 处起
    // Fixed partition (all inside the reserved block): alias 512MB, stack 256MB, heap the rest starting at 2GB
    g_alias = g_as + 0x10000000; g_alias_cap = 0x20000000;
    g_stack = g_as + 0x30000000; g_stack_cap = 0x10000000;
    g_heap  = g_as + 0x40000000; g_heap_cap  = RESERVE - 0x40000000;   // 4 GB，容得下游戏那一整块约 3.2 GB 的堆 | 4 GB, enough to hold the game's single heap block of about 3.2 GB
    LOG("guest AS %p +%#llx  heap %p alias %p stack %p", g_as, (unsigned long long)RESERVE, g_heap, g_alias, g_stack);
}

// 应用内存池 = Switch（4 GB 机型）给应用的 3285 MB。游戏启动（main+0x1b66160）把 QueryMemoryInfo 报的总量整块设成堆、失败就减 2 MB 重试，
// 整块交给自己的 StandardAllocator（main+0x32137a0）——所以堆大小 = 池 − 映像占用，宿主报多少它就只有多少。
// 之前这里报 3 GB 且 SetHeapSize 从不失败：堆比真机少约 150 MB，进局高峰主堆只剩约 18 MB，本机负载高时时序一偏申请 128 KB 返回空、
// 客体不判空直接写（CRASH main+0x1b36138，合入后复核 inv_tap）。
// Application memory pool = the 3285 MB the Switch (4 GB model) gives applications. At startup (main+0x1b66160) the game sets the whole total reported by QueryMemoryInfo as the heap, retrying with 2 MB less on failure,
// and hands the whole block to its own StandardAllocator (main+0x32137a0) -- so heap size = pool − image footprint, it has only as much as the host reports.
// Previously 3 GB was reported here and SetHeapSize never failed: the heap was about 150 MB smaller than on the real device, at the run entry peak the main heap had only about 18 MB left, and under high local load a timing shift made a 128 KB request return null,
// which the guest writes to without a null check (CRASH main+0x1b36138, re-verified after merge with inv_tap).
#define APP_POOL 0xCD500000ull
// 报给游戏的池 = Switch 的 3285 MB + 512 MB（DM_APP_POOL_ADD_MB=<整数> 可调，可为负——往小调用来造「主堆用尽」的现场）。
// 为什么多给：游戏自己固定划走约 3.08 GB，主堆余量进局只有 100–180 MB、换图时地板三四十 MB、最大可分配块二十几 MB；用尽时原版多处不判空直接写
// （公开仓库，另见下面 dm_guest_heap_stat）。实测多给的量一比一变成主堆余量（138 → 649 MB），游戏划走的不跟着变；
// 没碰过的堆页不计入常驻内存，余量真被用掉时常驻才涨。堆区上限是预留里的 4 GB（mem_init），3728 MB 放得下；再加要先动预留。
// 取 +512 而不是更大：给 6 GB 内存的机型留余地，真机上限有读数后再定。
// Pool reported to the game = the Switch's 3285 MB + 512 MB (adjustable with DM_APP_POOL_ADD_MB=<integer>, may be negative -- lowering it is how to stage a "main heap exhausted" scene).
// Why more is given: the game carves out a fixed 3.08 GB or so for itself, leaving the main heap only 100–180 MB of free space on entering a game, a floor of thirty to forty MB when changing maps and a largest allocatable block of twenty-odd MB; when it runs out the original game writes through null without checking in many places
// (see also dm_guest_heap_stat below). Measured: the extra amount turns one-to-one into main heap free space (138 -> 649 MB), while what the game carves out does not change with it;
// heap pages never touched do not count toward resident memory, which grows only when the free space is actually used. The heap region limit is the 4 GB in the reservation (mem_init), which fits 3728 MB; adding more means changing the reservation first.
// +512 rather than more: to leave room for devices with 6 GB of RAM; to be revisited once there are readings of the limit on real devices.
static uint64_t app_pool(void) { static uint64_t v; if (!v) { const char *e = getenv("DM_APP_POOL_ADD_MB"); long add = e ? atol(e) : 512; v = APP_POOL + ((uint64_t)add << 20); if (e) LOG("DM_APP_POOL_ADD_MB：应用内存池报 %llu MB（Switch 的 3285 MB %+ld）", (unsigned long long)(v >> 20), add); } return v; }
static size_t image_bytes(void) { size_t n = 0; for (int i = 0; i < g_nmod; i++) n += g_mod[i].size; return (n + 0x1fffff) & ~(size_t)0x1fffff; }

// 游戏主堆读数：主堆 = main+0x32137a0 那个 nn::mem::StandardAllocator（启动时把整块堆交给它）。申请失败返回空，原版多处不判空直接写
// （公开仓库 连战 82 分钟后异步读盘回调 main+0x27ecdc 要 765 KB 连续块得空，memset 写 0 地址；进程常驻内存看不出这件事）。
// 余量 / 最大可分配块调的是分配器自己的查询（sdk+0x2f9d78 / +0x2f9dbc，内部走 CentralHeap::Query、自己加锁），只许在客体线程上调。
// 读到的数留一份在 g_gheap_last 里，分配返回空时的报红直接带上次读数（那一刻不再进分配器）。单位：总量 / 余量 MB，最大块 KB
// Game main heap readout: the main heap = the nn::mem::StandardAllocator at main+0x32137a0 (the whole heap block is handed to it at startup). A failed allocation returns null, and the original game writes through it without a null check in many places
// (after 82 minutes of boss rush, the asynchronous disk-read callback main+0x27ecdc asked for a contiguous 765 KB block, got null, and memset wrote to address 0; the process resident memory does not show this).
// Free space / largest allocatable block call the allocator's own queries (sdk+0x2f9d78 / +0x2f9dbc, which go through CentralHeap::Query and take their own lock) and may only be called on a guest thread.
// The values read are kept in g_gheap_last, and the red report on a null allocation carries the last readout directly (the allocator is not entered again at that moment). Units: total / free in MB, largest block in KB
static module_t *mod_named(const char *n) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, n)) return &g_mod[i]; return NULL; }
static _Atomic unsigned long long g_gheap_last[3];
int dm_guest_heap_stat(unsigned long long *total_mb, unsigned long long *free_mb, unsigned long long *maxblk_kb) {
    module_t *mm = mod_named("main"), *ms = mod_named("sdk"); if (!mm || !ms || !g_heap_cur) return 0;
    uint8_t *al = mm->base + 0x32137a0; if (!al[0]) return 0;   // 首字节 = 已初始化（sdk+0x2f9898 写） | First byte = initialized (written by sdk+0x2f9898)
    uint64_t fr = ((uint64_t (*)(void *))(ms->base + 0x2f9d78))(al), mx = ((uint64_t (*)(void *))(ms->base + 0x2f9dbc))(al);
    *total_mb = g_heap_cur >> 20; *free_mb = fr >> 20; *maxblk_kb = mx >> 10;
    g_gheap_last[0] = *total_mb; g_gheap_last[1] = *free_mb; g_gheap_last[2] = *maxblk_kb; return 1;
}
// 上次读到的主堆读数（总量 MB、余量 MB、最大可分配块 KB；没读过全 0）。任何线程可调——不进分配器（iOS 壳在主线程上记内存告警那一行用）
// The last main-heap readout (total MB, free MB, largest allocatable block KB; all 0 if never read). Callable from any thread - does not enter the allocator (the iOS shell uses it on the main thread for the memory-warning log line)
void dm_guest_heap_last(unsigned long long v[3]) { for (int i = 0; i < 3; i++) v[i] = atomic_load(&g_gheap_last[i]); }
HLE_FORCE("_ZN2nn3mem17StandardAllocator8AllocateEmm", "照原版分配，只在返回空时报红（原版调用方多处不判空）", void *, hle_stdalloc_allocate, void *self, size_t size, size_t align) {
    static void *(*orig)(void *, size_t, size_t); if (!orig) { module_t *ms = mod_named("sdk"); orig = (void *(*)(void *, size_t, size_t))(ms->base + 0x2f9974); }
    void *p = orig(self, size, align);
    if (!p && size) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) LOG("【红】游戏堆分配返回空：申请 %zu 字节（对齐 %zu），分配器 %p；上次读数 主堆 %llu MB、余量 %llu MB、最大可分配块 %llu KB", size, align, self,
        (unsigned long long)g_gheap_last[0], (unsigned long long)g_gheap_last[1], (unsigned long long)g_gheap_last[2]); }
    return p;
}

// 内存账（memstat.m）用：客体地址空间整块范围（未初始化时 base = NULL）
// For memory accounting (memstat.m): the range of the whole guest address space block (base = NULL when not initialized)
void svc_guest_range(uint8_t **base, size_t *size) { *base = g_as; *size = RESERVE; }

enum { I_CoreMask=0, I_PriorityMask=1, I_AliasAddr=2, I_AliasSize=3, I_HeapAddr=4, I_HeapSize=5,
       I_TotalMem=6, I_UsedMem=7, I_DebuggerAttached=8, I_ResourceLimit=9, I_IdleTick=10, I_RandomEntropy=11,
       I_AslrAddr=12, I_AslrSize=13, I_StackAddr=14, I_StackSize=15, I_SysResTotal=16, I_SysResUsed=17,
       I_ProgramId=18, I_InitProcIdRange=19, I_UserExcAddr=20, I_TotalNonSys=21, I_UsedNonSys=22, I_IsApplication=23 };

static uint8_t g_seen[256];

/*
 * ACNH's first service-manager exchange uses TIPC rather than CMIF. The
 * kernel normally copies the response back into the same thread-local IPC
 * buffer, so SendSyncRequest must do that work before returning success.
 * Keep these handles opaque: the guest only needs stable session identities
 * until the next IPC request, while the actual service implementation is
 * added profile by profile.
 */
typedef struct {
    uint32_t handle;
    char name[9];
} acnh_ipc_session;

static acnh_ipc_session g_acnh_sessions[64];
static unsigned g_acnh_sessions_n;
static pthread_mutex_t g_acnh_ipc_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_acnh_audren_event_read;
static uint32_t g_acnh_audren_event_write;

uint32_t dm_acnh_audren_event_handle(void) {
    if (!g_acnh_audren_event_read)
        g_acnh_audren_event_read = dm_service_event_create(0, &g_acnh_audren_event_write);
    return g_acnh_audren_event_read;
}

void dm_acnh_audren_event_signal(void) {
    if (g_acnh_audren_event_write)
        dm_service_event_signal(g_acnh_audren_event_write);
}

static const char *acnh_session_name(uint32_t handle) {
    pthread_mutex_lock(&g_acnh_ipc_lock);
    const char *name = handle == 0x7100u ? "sm:" : "unknown";
    for (unsigned i = 0; i < g_acnh_sessions_n; i++)
        if (g_acnh_sessions[i].handle == handle) {
            name = g_acnh_sessions[i].name;
            break;
        }
    pthread_mutex_unlock(&g_acnh_ipc_lock);
    return name;
}

static uint32_t acnh_session_for(const char *name) {
    pthread_mutex_lock(&g_acnh_ipc_lock);
    for (unsigned i = 0; i < g_acnh_sessions_n; i++)
        if (!strncmp(g_acnh_sessions[i].name, name, sizeof g_acnh_sessions[i].name)) {
            uint32_t h = g_acnh_sessions[i].handle;
            pthread_mutex_unlock(&g_acnh_ipc_lock);
            return h;
        }
    if (g_acnh_sessions_n >= sizeof g_acnh_sessions / sizeof g_acnh_sessions[0])
        die("ACNH IPC 会话表满");
    acnh_ipc_session *s = &g_acnh_sessions[g_acnh_sessions_n++];
    s->handle = 0x7200u + g_acnh_sessions_n;
    strncpy(s->name, name, sizeof s->name - 1);
    s->name[sizeof s->name - 1] = 0;
    pthread_mutex_unlock(&g_acnh_ipc_lock);
    return s->handle;
}

int dm_acnh_ipc_is_session(uint32_t handle) {
    if (!dm_game_is_acnh()) return 0;
    pthread_mutex_lock(&g_acnh_ipc_lock);
    int found = handle == 0x7100u;
    for (unsigned i = 0; !found && i < g_acnh_sessions_n; i++)
        found = g_acnh_sessions[i].handle == handle;
    pthread_mutex_unlock(&g_acnh_ipc_lock);
    return found;
}

static void acnh_tipc_reply(uint32_t *p, uint32_t move_handle, int with_move_handle) {
    /* TLR[0x1f8] is the SDK's current-thread object pointer; do not erase it
       while reusing the preceding IPC-buffer area. */
    memset(p, 0, 0x1f0);
    /* HIPC response: one raw result word, optionally preceded by a move handle. */
    p[0] = 0;
    p[1] = 1u | (with_move_handle ? 0x80000000u : 0u);
    unsigned i = 2;
    if (with_move_handle) {
        p[i++] = 1u << 5; /* special header: one move handle */
        p[i++] = move_handle;
    }
    p[i] = 0; /* Result = success */
}

static int acnh_tipc_request(uint32_t *p) {
    uint32_t h0 = p[0], h1 = p[1];
    uint32_t type = h0 & 0xffffu;
    if (type < 15 || type > 31) return 0;

    size_t off = 2;
    if (h1 & 0x80000000u) {
        uint32_t sp = p[off++];
        if (sp & 1u) off += 2; /* send-pid descriptor */
        off += (sp >> 1) & 0xfu; /* copy handles */
        off += (sp >> 5) & 0xfu; /* move handles */
    }
    uint32_t raw_words = h1 & 0x3ffu;
    if (type == 16) { /* RegisterClient */
        if (raw_words != 0) return 0;
        acnh_tipc_reply(p, 0, 0);
        LOG("ACNH sm:TIPC RegisterClient → 成功");
        return 1;
    }
    if (type == 17) { /* GetServiceHandle */
        if (raw_words < 2) return 0;
        char name[9] = {0};
        memcpy(name, p + off, 8);
        uint32_t h = acnh_session_for(name);
        acnh_tipc_reply(p, h, 1);
        LOG("ACNH sm:GetServiceHandle %s → %#x", name, h);
        return 1;
    }
    if (type == 15) { /* TIPC close */
        acnh_tipc_reply(p, 0, 0);
        return 1;
    }
    return 0;
}

static void acnh_cmif_reply(uint32_t *p, uint32_t move_handle, int with_move_handle, int out_u16) {
    memset(p, 0, 0x1f0);
    unsigned i = 2;
    p[0] = 0;
    p[1] = (out_u16 ? 5u : 4u) | (with_move_handle ? 0x80000000u : 0u);
    if (with_move_handle) {
        p[i++] = 1u << 5; /* special header: one move handle */
        p[i++] = move_handle;
    }
    i = (i + 3u) & ~3u; /* CMIF raw data is 16-byte aligned. */
    p[i + 0] = 0x4f434653u; /* "SFCO" */
    p[i + 1] = 0;           /* version */
    p[i + 2] = 0;           /* Result = success */
    p[i + 3] = 0;           /* token / interface id */
    if (out_u16) p[i + 4] = 0;
}

static void acnh_cmif_reply_copy_handle(uint32_t *p, uint32_t handle) {
    memset(p, 0, 0x1f0);
    p[0] = 0;
    p[1] = 4u | 0x80000000u;
    p[2] = 1u << 1; /* special header: one copy handle */
    p[3] = handle;
    p[4] = 0x4f434653u; /* "SFCO" */
    p[5] = 0;
    p[6] = 0; /* Result = success */
    p[7] = 0;
}

static void acnh_cmif_reply_u64(uint32_t *p, uint64_t value) {
    memset(p, 0, 0x1f0);
    p[0] = 0;
    p[1] = 6u; /* SFCO header (4 words) + u64 result (2 words). */
    p[4] = 0x4f434653u; /* "SFCO" */
    p[5] = 0;
    p[6] = 0; /* Result = success */
    p[7] = 0;
    p[8] = (uint32_t)value;
    p[9] = (uint32_t)(value >> 32);
}

static uint64_t acnh_align_u64(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static uint64_t acnh_audio_work_size(const uint32_t *raw, unsigned raw_words) {
    /* AudioRendererParameterInternal starts after the four-word SFCI header.
       ACNH uses the extended 0x44-byte form; the first 13 fields retain the
       public audren layout.  Keep the calculation bounded because this is an
       HLE sizing query, not a guest allocation request. */
    if (raw_words < 17) return 0;
    uint64_t sample_count = raw[5];
    uint64_t mix_buffers = raw[6];
    uint64_t sub_mixes = raw[7];
    uint64_t voices = raw[8];
    uint64_t sinks = raw[9];
    uint64_t effects = raw[10];
    uint64_t perf_frames = raw[11];
    uint64_t splitter_count = raw[13];
    uint64_t splitter_destinations = raw[14];
    uint64_t total_mixes = sub_mixes + 1;
    uint64_t size = acnh_align_u64(mix_buffers * 4, 0x40);
    size += sub_mixes * 0x400;
    size += total_mixes * 0x940;
    size += voices * 0x3f0;
    size += acnh_align_u64(total_mixes * 8, 0x10);
    size += acnh_align_u64(voices * 8, 0x10);
    size += acnh_align_u64(((sinks + sub_mixes) * 0x3c0 + sample_count * 4) *
                           (mix_buffers + 6), 0x40);
    size += (sinks + sub_mixes) * 0x2c0;
    size += (effects + voices * 4) * 0x30 + 0x50;
    /* REV3+ splitter state.  These formulas are the public Ryujinx/Yuzu
       implementation and match the ACNH parameter shape. */
    uint64_t n = total_mixes;
    uint64_t node = 4 * n * n + 0x12 * n + 2 * (acnh_align_u64(n, 0x40) / 8);
    uint64_t edge = acnh_align_u64(n * n, 0x40) / 8;
    size += acnh_align_u64(node + edge, 0x10);
    size += splitter_destinations * 0xe0 + splitter_count * 0x20;
    size += acnh_align_u64(splitter_destinations * 4, 0x10);
    size += sinks * 0x170 + (sinks + sub_mixes) * 0x280;
    size += effects * 0x4c0 + voices * 0x100;
    size = acnh_align_u64(size, 0x40) + 0x40;
    if (perf_frames) {
        size += acnh_align_u64((effects + sinks + voices + sub_mixes + 1) * 16 + 0x658,
                               0x40) * (perf_frames + 1);
    }
    /* REV? supports the variadic command buffer.  The old fixed floor is a
       safe lower bound for the startup query and keeps this path deterministic
       until the renderer's update command is implemented. */
    size += effects * 0x840 + sub_mixes * 0x5a38 + sinks * 0x148 +
            splitter_destinations * 0x540 +
            voices * (splitter_count * 0x68 + 0x2e0) +
            ((voices + sub_mixes + effects + sinks + 0x65) << 6) + 0x3f8 + 0x7e;
    return acnh_align_u64(size + 0x7e, 0x1000);
}

static int acnh_cmif_request(uint32_t *p, uint32_t session) {
    uint32_t h0 = p[0], h1 = p[1];
    size_t off = 2;
    if (h1 & 0x80000000u) {
        uint32_t sp = p[off++];
        if (sp & 1u) off += 2; /* send-pid descriptor */
        off += (sp >> 1) & 0xfu; /* copy handles */
        off += (sp >> 5) & 0xfu; /* move handles */
    }
    off += ((h0 >> 16) & 0xfu) * 2; /* send statics */
    off += ((h0 >> 20) & 0xfu) * 2; /* send buffers */
    off += ((h0 >> 24) & 0xfu) * 3; /* receive-buffer descriptors are 3 words on AArch64 */
    off += ((h0 >> 28) & 0xfu) * 2; /* exchange buffers */
    unsigned raw_words = h1 & 0x3ffu;
    if (!raw_words) return 0;
    unsigned raw_off = (unsigned)((off + 3u) & ~3u);
    uint32_t *raw = p + raw_off;
    if (raw[0] != 0x49434653u) return 0; /* "SFCI" */
    uint32_t cmd = raw[2];
    if (cmd == 0 && session == 0x7201u) { /* hid:IApplicationService::CreateAppletResource */
        static uint32_t hid_applet_session;
        if (!hid_applet_session) hid_applet_session = acnh_session_for("hid:applet");
        acnh_cmif_reply(p, hid_applet_session, 1, 0);
        LOG("ACNH hid:CreateAppletResource → %#x", hid_applet_session);
        return 1;
    }
    if (cmd == 0 && session == 0x7202u) { /* IAppletResource::GetSharedMemoryHandle */
        static uint32_t hid_shared_handle = 0x7401u;
        acnh_cmif_reply_copy_handle(p, hid_shared_handle);
        LOG("ACNH hid:applet:GetSharedMemoryHandle → %#x", hid_shared_handle);
        return 1;
    }
    if (session == 0x7201u && (cmd == 1 || cmd == 11 || cmd == 21 || cmd == 31 ||
                                cmd == 100 || cmd == 101 || cmd == 102 || cmd == 103 ||
                                cmd == 104 || cmd == 106 || cmd == 107 || cmd == 108 ||
                                cmd == 109 || cmd == 120)) {
        /* IHidServer activation calls used by ACNH; host input is already active. */
        acnh_cmif_reply(p, 0, 0, 0);
        LOG("ACNH hid command %u → 成功", cmd);
        return 1;
    }
    if (cmd == 3) { /* QueryPointerBufferSize */
        acnh_cmif_reply(p, 0, 0, 1);
        LOG("ACNH %s:CMIF QueryPointerBufferSize → 0", session == 0x7201u ? "nvdrv" : "service");
        return 1;
    }
    if (cmd == 4) { /* CloneCurrentObjectEx */
        acnh_cmif_reply(p, session, 1, 0);
        LOG("ACNH service %#x:CMIF CloneCurrentObjectEx → %#x", session, session);
        return 1;
    }
    if (cmd == 13 && session == 0x7201u && raw_words >= 5 && raw[4] == 1) {
        /* nvdrv #FinishInitialize / #SetGraphicsFirmwareMemoryMarginEnabled.
           Official user processes call it with 1 at the end of nvdrv init;
           it has no output data. */
        acnh_cmif_reply(p, 0, 0, 0);
        LOG("ACNH nvdrv:CMIF FinishInitialize(1) → 成功");
        return 1;
    }
    if (cmd == 0 && session == 0x7201u) {
        LOG("ACNH hid:CMIF command 0 raw_words=%u raw0=%#x raw1=%#x raw2=%#x raw3=%#x raw4=%#x raw5=%#x",
            raw_words, raw[0], raw_words > 1 ? raw[1] : 0, raw_words > 2 ? raw[2] : 0,
            raw_words > 3 ? raw[3] : 0, raw_words > 4 ? raw[4] : 0, raw_words > 5 ? raw[5] : 0);
    }
    if (session == 0x7203u && cmd == 1) {
        const uint64_t work_size = acnh_audio_work_size(raw, raw_words);
        acnh_cmif_reply_u64(p, work_size);
        LOG("ACNH audren:u:GetWorkBufferSize → %#llx", work_size);
        return 1;
    }
    if (session == 0x7203u && cmd == 0) {
        static uint32_t audio_renderer_session;
        if (!audio_renderer_session) audio_renderer_session = acnh_session_for("audren:renderer");
        acnh_cmif_reply(p, audio_renderer_session, 1, 0);
        LOG("ACNH audren:u:OpenAudioRenderer → %#x", audio_renderer_session);
        return 1;
    }
    if (session == 0x7203u && cmd == 6) {
        uint32_t event = dm_acnh_audren_event_handle();
        acnh_cmif_reply_copy_handle(p, event);
        LOG("ACNH audren:u:QuerySystemEvent → %#x", event);
        return 1;
    }
    if (session == 0x7204u && cmd == 7) {
        uint32_t event = dm_acnh_audren_event_handle();
        acnh_cmif_reply_copy_handle(p, event);
        LOG("ACNH audren:renderer:QuerySystemEvent → %#x", event);
        return 1;
    }
    if (session == 0x7204u && (cmd == 3 || cmd == 5 || cmd == 6 || cmd == 8)) {
        acnh_cmif_reply(p, 0, 0, cmd == 3);
        LOG("ACNH audren:renderer command %u → 成功", cmd);
        return 1;
    }
    LOG("ACNH service %#x (%s): 未实现 CMIF command %u（参数 %u 个字）",
        session, acnh_session_name(session), cmd, raw_words - 4);
    return 0;
}

// 已映射区间登记（MapMemory 栈、MapPhysicalMemory alias 页），QueryMemory 据此回报；SDK 靠它找空位
// Registry of mapped ranges (MapMemory stacks, MapPhysicalMemory alias pages), QueryMemory reports from it; the SDK relies on it to find free space
#define MAX_MAP 4096
static struct { uint64_t base, size, src; uint32_t state; } g_map[MAX_MAP];
static int g_nmap;
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;
static int map_find(uint64_t a, uint64_t n) {       // 返回 1 基下标，0 = 无重叠 | returns a 1-based index, 0 = no overlap
    for (int i = 0; i < g_nmap; i++) if (a < g_map[i].base + g_map[i].size && g_map[i].base < a + n) return i + 1;
    return 0;
}
static void map_add(uint64_t b, uint64_t n, uint64_t src, uint32_t st) {
    pthread_mutex_lock(&g_map_lock);
    if (g_nmap >= MAX_MAP) die("映射表满");
    g_map[g_nmap++] = (typeof(g_map[0])){b, n, src, st};
    pthread_mutex_unlock(&g_map_lock);
}
static void map_del(int i) { pthread_mutex_lock(&g_map_lock); g_map[i] = g_map[--g_nmap]; pthread_mutex_unlock(&g_map_lock); }

int svc_thread(int num, uint64_t *regs);   // hle_thread.c：线程与同步类 | hle_thread.c: thread and synchronization kind

uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr) {
    { void hlecount_svc(int); hlecount_svc(num); }   // DM_HLECOUNT：svc 按号计数 | DM_HLECOUNT: svc counts by number
    mem_init();
    if (svc_thread(num, regs)) return regs[0];
    { uint64_t svc_host_call(int, uint64_t *); if (num >= 0x80 && svc_host_call(num, regs)) return regs[0]; }   // 0x80 起：剪口约定的宿主调用（hle_tact.c） | from 0x80: host calls by cut convention (hle_tact.c)
    if (dm_game_is_acnh()) {
        switch (num) {
        case 0x15: {  // CreateTransferMemory：ACNH 只把它作为 IPC 共享区句柄，宿主不需要真实内核对象
            static uint32_t next = 0x7001;
            regs[0] = 0; regs[1] = next++;
            return 0;
        }
        case 0x13: {  // MapSharedMemory(handle, addr, size)：把 HID 共享区映射到客体地址空间
            uint32_t handle = (uint32_t)regs[0];
            uint64_t addr = regs[1], size = regs[2];
            uint64_t end;
            uint64_t lo, hi;
            if (handle != 0x7401u || !size || (addr & 0x3fffu) || (size & 0x3fffu) ||
                __builtin_add_overflow(addr, size, &end) ||
                addr < (uint64_t)g_as || end > (uint64_t)g_as + RESERVE || map_find(addr, size)) {
                LOG("ACNH MapSharedMemory 非法：句柄 %#x 地址 %#llx 大小 %#llx", handle,
                    (unsigned long long)addr, (unsigned long long)size);
                regs[0] = 0xD401; return 0;
            }
            /* macOS uses 16 KiB pages. Open the complete host-page interval even
               though the Horizon caller's range is also expected to be aligned. */
            lo = addr & ~0x3fffull;
            hi = (end + 0x3fffull) & ~0x3fffull;
            if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) {
                LOG("ACNH MapSharedMemory mprotect 失败：%#llx+%#llx errno=%d",
                    (unsigned long long)lo, (unsigned long long)(hi - lo), errno);
                regs[0] = 0xD401; return 0;
            }
            memset((void *)addr, 0, (size_t)size);
            map_add(addr, size, handle, 0x10 /* SharedMemory */);
            LOG("ACNH MapSharedMemory %#x → %#llx+%#llx", handle,
                (unsigned long long)addr, (unsigned long long)size);
            regs[0] = 0; return 0;
        }
        case 0x14: {  // UnmapSharedMemory(handle, addr, size)：撤销 HID 共享区映射
            uint32_t handle = (uint32_t)regs[0];
            uint64_t addr = regs[1], size = regs[2];
            int k = map_find(addr, size);
            if (handle != 0x7401u || !k || g_map[k - 1].base != addr ||
                g_map[k - 1].size != size || g_map[k - 1].src != handle) {
                LOG("ACNH UnmapSharedMemory 非法：句柄 %#x 地址 %#llx 大小 %#llx", handle,
                    (unsigned long long)addr, (unsigned long long)size);
                regs[0] = 0xD401; return 0;
            }
            uint64_t lo = addr & ~0x3fffull;
            uint64_t hi = (addr + size + 0x3fffull) & ~0x3fffull;
            if (mprotect((void *)lo, hi - lo, PROT_NONE)) {
                LOG("ACNH UnmapSharedMemory mprotect 失败：%#llx+%#llx errno=%d",
                    (unsigned long long)lo, (unsigned long long)(hi - lo), errno);
                regs[0] = 0xD401; return 0;
            }
            map_del(k - 1);
            LOG("ACNH UnmapSharedMemory %#x ← %#llx+%#llx", handle,
                (unsigned long long)addr, (unsigned long long)size);
            regs[0] = 0; return 0;
        }
        case 0x1f: {  // ConnectToNamedPort：ACNH 启动期只连接 sm:，由宿主发一个不透明会话句柄
            static uint32_t sm = 0x7100;
            regs[0] = 0; regs[1] = sm;
            return 0;
        }
        case 0x21: {  // SendSyncRequest：消息缓冲在当前线程的 TLR[0]，服务响应由 ACNH profile 处理
            uint32_t *p = (uint32_t *)pthread_getspecific(g_keytlr);
            if (p && regs[0] == 0x7100u && acnh_tipc_request(p)) {
                regs[0] = 0;
                return 0;
            }
            if (p && (uint32_t)regs[0] == 0x7203u &&
                p[0] == 2u && p[1] == 0u && p[2] == 2u && p[3] == dm_acnh_audren_event_handle()) {
                LOG("ACNH audren:u HIPC event-handle request → 成功");
                regs[0] = 0;
                return 0;
            }
            if (p && dm_acnh_ipc_is_session((uint32_t)regs[0])) {
                if (acnh_cmif_request(p, (uint32_t)regs[0])) {
                    regs[0] = 0;
                    return 0;
                }
            }
            LOG("【红】ACNH SendSyncRequest 未实现：会话 %#llx (%s)，消息缓冲 %p",
                regs[0], acnh_session_name((uint32_t)regs[0]), (void *)p);
            regs[0] = 0x2A2u;
            return 0;
        }
        case 0x22: {  // SendSyncRequestWithUserBuffer：消息缓冲由 x0/x1 传入
            LOG("【红】ACNH SendSyncRequestWithUserBuffer 未实现：会话 %#llx，缓冲 %#llx +%#llx",
                regs[0], regs[1], regs[2]);
            regs[0] = 0x2A2u;
            return 0;
        }
        default:
            break;
        }
    }
    switch (num) {
    case 0x01: {  // SetHeapSize(out_addr*, size)  → x1 = heap base
        size_t size = regs[1];
        if (size > g_heap_cap) { LOG("SetHeapSize OOM: req %#zx > cap %#zx", size, g_heap_cap); regs[0] = 0xD401; return 0; }
        if (size + image_bytes() > app_pool()) { g_heap_rejects++; regs[0] = 0x10801; return 0; }   // 超出应用内存池：ResultLimitReached。游戏启动时按 2 MB 递减重试直到放得下（main+0x1b66184） | exceeds the application memory pool: ResultLimitReached. At startup the game retries in 2 MB decrements until it fits (main+0x1b66184)
        int rc = 0;
        if (size > g_heap_cur) rc = mprotect(g_heap + g_heap_cur, size - g_heap_cur, PROT_READ | PROT_WRITE);
        else if (size < g_heap_cur) rc = mprotect(g_heap + size, g_heap_cur - size, PROT_NONE);
        if (g_flag.svc) LOG("SetHeapSize %#zx -> base %p mprotect=%d errno=%d", size, g_heap, rc, errno);
        { static int said; if (!said && size) { said = 1; LOG("SetHeapSize：首次设堆成功 %zu MB（应用内存池 %llu MB − 映像 %zu MB；此前被拒 %d 次）", size >> 20, (unsigned long long)(app_pool() >> 20), image_bytes() >> 20, g_heap_rejects); } }
        g_heap_cur = size;
        regs[0] = 0; regs[1] = (uint64_t)g_heap; return 0;
    }
    case 0x02: regs[0] = 0; return 0;   // SetMemoryPermission
    case 0x03: regs[0] = 0; return 0;   // SetMemoryAttribute
    case 0x04: {  // MapMemory(dst, src, size)：SDK 建线程时把堆上的栈搬进栈区 | MapMemory(dst, src, size): when creating a thread the SDK moves the stack on the heap into the stack region
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        if ((uint8_t *)dst < g_stack || (uint8_t *)dst + size > g_stack + g_stack_cap || map_find(dst, size)) {
            LOG("MapMemory 目标非法或已占用 %#llx+%#llx", dst, size); regs[0] = 0xDC01; return 0; }
        // 苹果页 16K、Horizon 页 4K：可访问区扩到 16K 边界（多放开的是同区未用页，不影响语义）
        // Apple pages are 16K, Horizon pages 4K: the accessible region is extended to a 16K boundary (the extra pages opened are unused pages of the same region, semantics unaffected)
        uint64_t lo = dst & ~0x3FFFull, hi = (dst + size + 0x3FFF) & ~0x3FFFull;
        if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) die("MapMemory mprotect errno=%d", errno);
        memcpy((void *)dst, (void *)src, size);   // 搬移语义：映射期间源区不被访问（Horizon 把它设为不可访问） | move semantics: the source region is not accessed while mapped (Horizon makes it inaccessible)
        map_add(dst, size, src, 0x0B /*MappedMemory*/);
        if (g_flag.svc) LOG("MapMemory %#llx <- %#llx +%#llx", dst, src, size);
        regs[0] = 0; return 0;
    }
    case 0x05: {  // UnmapMemory(dst, src, size)：内容搬回源区；16K 页可能与相邻映射共用，不收回权限 | UnmapMemory(dst, src, size): contents are moved back to the source region; a 16K page may be shared with an adjacent mapping, permissions are not revoked
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        int k = map_find(dst, size);
        if (!k || g_map[k - 1].base != dst || g_map[k - 1].size != size || g_map[k - 1].src != src) {
            LOG("UnmapMemory 与映射不符 %#llx+%#llx", dst, size); regs[0] = 0xD401; return 0; }
        memcpy((void *)src, (void *)dst, size);
        map_del(k - 1);
        regs[0] = 0; return 0;
    }
    case 0x06: {  // QueryMemory(MemoryInfo* out, PageInfo* pout, addr) → x1 = pageinfo
        uint64_t mi = regs[0], addr = regs[2];
        // MemoryInfo: base(0) size(8) state(0x10) attr(0x14) perm(0x18) ...
        uint8_t *a = (uint8_t *)addr;
        uint64_t base, size, state, perm;
        int k;
        if (a >= g_heap && a < g_heap + g_heap_cur) { base=(uint64_t)g_heap; size=g_heap_cur; state=5/*Heap*/; perm=3; }
        else if ((k = map_find(addr, 1))) { base=g_map[k-1].base; size=g_map[k-1].size; state=g_map[k-1].state; perm=3; }
        else if (a >= g_as && a < g_as + RESERVE) {   // 空闲：到相邻映射/堆边界为止 | free: up to the adjacent mapping/heap boundary
            uint64_t lo = (uint64_t)g_as, hi = (uint64_t)g_as + RESERVE;
            if (a >= g_heap) lo = (uint64_t)g_heap + g_heap_cur;
            else if (hi > (uint64_t)g_heap) hi = (uint64_t)g_heap;
            for (int i = 0; i < g_nmap; i++) {
                uint64_t e = g_map[i].base + g_map[i].size;
                if (e <= addr && e > lo) lo = e;
                if (g_map[i].base > addr && g_map[i].base < hi) hi = g_map[i].base;
            }
            base=lo; size=hi-lo; state=0; perm=0; }
        else {
            // 地址落在「当前宿主线程的栈」里：回报整段栈（状态 0xB = 栈）。SDK 初始化主线程记录时拿当时的 sp 查这里，把回报的区间当主线程栈；
            // 之后 nn::os::GetCurrentStackInfo 会断言 sp 仍在那个区间里，不在就中止。原先对客体地址空间之外的地址一律回报「它所在的 4 KiB 页」，
            // 主线程栈于是被记成一页——初始化时的 sp 与后来的 sp 是否同页，取决于宿主栈顶的绝对位置，而它随环境变量 / 启动参数总长移动（换个环境偶发启动即中止）。
            // 暗黑这一版不调 GetCurrentStackInfo（main / subsdk0 没有导入它，SDK 内部也没有调用点；宿主垫 12 档环境变量长度都起得到主菜单），所以没踩到；回报「一页」本身是错的，照改。
            // The address lies in "the current host thread's stack": report the whole stack (state 0xB = stack). When the SDK initializes the main thread record it queries here with the sp at that moment and takes the reported range as the main thread stack;
            // later nn::os::GetCurrentStackInfo asserts that sp is still inside that range and aborts if not. Previously every address outside the guest address space was reported as "the 4 KiB page containing it",
            // so the main thread stack was recorded as one page. Whether the sp at init and a later sp share a page depends on the absolute position of the host stack top, which moves with the total length of environment variables / launch arguments (a different environment occasionally aborts at startup).
            // This Diablo build does not call GetCurrentStackInfo (main / subsdk0 do not import it and the SDK has no internal call site; the host reaches the main menu with 12 different environment padding lengths), so it was never hit; reporting "one page" is wrong in itself, so fix it anyway.
            uint64_t top = (uint64_t)pthread_get_stackaddr_np(pthread_self()), ssz = pthread_get_stacksize_np(pthread_self());
            if (addr < top && addr >= top - ssz) { base = top - ssz; size = ssz; state = 0xB/*Stack*/; perm = 3; }
            else { base=addr & ~0xFFFull; size=0x1000; state=3/*Code*/; perm=5; } }
        if (mi) { uint64_t *m=(uint64_t*)mi; m[0]=base; m[1]=size; ((uint32_t*)mi)[4]=state; ((uint32_t*)mi)[5]=0; ((uint32_t*)mi)[6]=perm; ((uint32_t*)mi)[7]=0; ((uint32_t*)mi)[8]=0; }
        regs[0] = 0; regs[1] = 0; return 0;
    }
    case 0x2C: case 0x2D: {  // MapPhysicalMemory / UnmapPhysicalMemory(addr, size)：只许在 alias 区内（SDK 的 VammManager 用） | MapPhysicalMemory / UnmapPhysicalMemory(addr, size): only allowed inside the alias region (used by the SDK's VammManager)
        uint8_t *a = (uint8_t *)regs[0]; size_t size = regs[1];
        if (a < g_alias || a + size > g_alias + g_alias_cap) { LOG("SVC %#x 越出 alias 区 %p+%#zx", num, a, size); regs[0] = 0xD401; return 0; }
        int rc = num == 0x2C ? mprotect(a, size, PROT_READ | PROT_WRITE)
                             : (madvise(a, size, MADV_FREE), mprotect(a, size, PROT_NONE));
        if (rc) die("SVC %#x mprotect %p+%#zx errno=%d", num, a, size, errno);
        if (g_flag.svc) LOG("%sPhysicalMemory %p+%#zx", num == 0x2C ? "Map" : "Unmap", a, size);
        regs[0] = 0; return 0;
    }
    case 0x26: {  // Break(reason, ptr, size)：SDK 致命中止 | Break(reason, ptr, size): SDK fatal abort
        uint32_t res = (regs[1] && regs[2] >= 4) ? *(uint32_t *)regs[1] : 0;
        module_t *mm = module_of((void *)regs[30]);
        static int nb;
        uint32_t b0 = (regs[1] && regs[2] >= 4) ? *(uint32_t *)regs[1] : 0;
        uint32_t b1 = (regs[1] && regs[2] >= 8) ? *((uint32_t *)regs[1] + 1) : 0;
        LOG("Break#%d reason=%#llx Result=%#x ptr=%#llx size=%#llx data=%#x,%#x svc 位于 %s+%#llx", ++nb, regs[0], res,
            regs[1], regs[2], b0, b1, mm ? mm->name : "?", orig_addr);
        dm_backtrace(regs[29]);
        if (regs[0] & 0x80000000ull)
            return 0;   // 仅通知，可继续
        _exit(42);
    }
    case 0x1E: regs[0] = 0; regs[1] = guest_ns() * 12 / 625; return 0;   // GetSystemTick
    case 0x27: {  // OutputDebugString(str, len)
        if (g_flag.dbg) fprintf(stderr, "[guest] %.*s\n", (int)regs[1], (char *)regs[0]);
        regs[0] = 0; return 0;
    }
    case 0x29: {  // GetInfo(out*, id, handle, subid) → x1 = value
        uint64_t id = regs[1], v = 0;
        switch (id) {
        case I_CoreMask: v = 7; break;
        case I_PriorityMask: v = 0; break;
        case I_AliasAddr: v = (uint64_t)g_alias; break;   case I_AliasSize: v = g_alias_cap; break;
        case I_HeapAddr:  v = (uint64_t)g_heap;  break;   case I_HeapSize:  v = g_heap_cap;  break;
        case I_AslrAddr:  v = (uint64_t)g_as;    break;   case I_AslrSize:  v = RESERVE;     break;
        case I_StackAddr: v = (uint64_t)g_stack; break;   case I_StackSize: v = g_stack_cap; break;
        case I_TotalMem: case I_TotalNonSys: v = app_pool(); break;
        case I_UsedMem:  case I_UsedNonSys:  v = g_heap_cur + image_bytes(); break;   // 已用 = 堆 + 映像，与 SetHeapSize 的核对同一口径（原先写死 64 MB）。sdk 的 QueryMemoryInfo（sdk+0x34f4ec）只拿它算「程序占用」那一格； | Used = heap + image, the same accounting as the check in SetHeapSize (it used to be hard-coded to 64 MB). The sdk's QueryMemoryInfo (sdk+0x34f4ec) only uses it to compute the "program usage" field;
                                                 // 「可用总量」那一格直接是池总量，所以游戏启动设堆仍要从整池起按 2 MB 往下减到 池 − 映像（约 34 次被拒）——这是原版在真机上的走法，不是我们的口径错
                                                 // The "total available" field is simply the pool total, so at startup the game still has to step down from the whole pool by 2 MB at a time until pool - image (about 34 refusals) when setting its heap -- this is how the original behaves on the real device, not a mistake in our accounting
        case I_SysResTotal: v = 0x2000000; break; case I_SysResUsed: v = 0x400000; break;
        case I_DebuggerAttached: v = 0; break;
        case I_RandomEntropy: { uint64_t dm_entropy64(void); v = dm_entropy64(); break; }   // DM_DETCLOCK 开时为确定流（hle_os.c） | a deterministic stream when DM_DETCLOCK is on (hle_os.c)
        case I_ProgramId: v = 0x0100726014352000ull; break;
        case I_IsApplication: v = 1; break;
        case I_UserExcAddr: v = 0; break;
        default: v = 0; break;
        }
        regs[0] = 0; regs[1] = v; return 0;
    }
    default:
        if (!g_seen[num & 0xff]) { g_seen[num & 0xff] = 1; LOG("SVC %#x x0=%#llx x1=%#llx x2=%#llx (占位返回成功)", num, regs[0], regs[1], regs[2]); }
        regs[0] = 0; return 0;
    }
}
