#include <stdio.h>

#include "../Sources/dm_pack.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s pack\n", argv[0]);
        return 2;
    }
    dm_pack_info info;
    char why[512];
    int result = dm_pack_inspect(argv[1], &info, why, sizeof why);
    if (result != DM_PACK_OK) {
        fprintf(stderr, "inspect failed: %s\n", why);
        return 1;
    }
    if (info.entry_count != 3 || info.pair_index != 1 ||
        info.pair_count != 1) {
        fprintf(stderr, "unexpected metadata: entries=%llu pair=%u/%u\n",
                (unsigned long long)info.entry_count,
                info.pair_index, info.pair_count);
        return 1;
    }
    puts("acnh_pack_selftest: PASS");
    return 0;
}
