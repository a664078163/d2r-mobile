// 服务层事件的最小内核回归：读端可等待、写端可从宿主线程触发、ClearEvent 后再次等待超时。
// Minimal kernel regression for a service event: the read end waits, the host can signal the write end, and ClearEvent makes the next wait time out.
#include "../dm.h"
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

int svc_thread(int num, uint64_t *regs);
uint32_t svc_thread_main_init(void);

static uint32_t g_write;
static uint32_t g_read;

static void *signal_thread(void *arg) {
    (void)arg;
    usleep(20000);
    dm_service_event_signal(g_write);
    return NULL;
}

int main(void) {
    svc_thread_main_init();
    g_read = dm_service_event_create(0, &g_write);
    pthread_t th;
    pthread_create(&th, NULL, signal_thread, NULL);
    uint32_t handles[] = {g_read};
    uint64_t wait_regs[8] = {(uint64_t)-1, (uint64_t)handles, 1, (uint64_t)-1};
    svc_thread(0x18, wait_regs);
    pthread_join(th, NULL);
    int ok = wait_regs[0] == 0 && wait_regs[1] == 0;

    uint64_t clear_regs[8] = {g_read};
    svc_thread(0x12, clear_regs);
    uint64_t timeout_regs[8] = {(uint64_t)-1, (uint64_t)handles, 1, 0};
    svc_thread(0x18, timeout_regs);
    ok = ok && clear_regs[0] == 0 && timeout_regs[0] == 0xEA01u;
    printf("%s 服务事件：等待 / 宿主触发 / ClearEvent 后超时（1/1）\n", ok ? "✓" : "✗");
    return ok ? 0 : 1;
}
