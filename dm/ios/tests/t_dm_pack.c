#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../Sources/dm_pack.h"

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
    } \
} while (0)

static void mkdirs(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(tmp, 0755);
        *p = '/';
    }
    mkdir(tmp, 0755);
}

static void write_file(const char *path, const void *data, size_t n) {
    char parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = '\0';
        mkdirs(parent);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "open %s: %s", path, strerror(errno));
    if (fd < 0) return;
    CHECK(write(fd, data, n) == (ssize_t)n, "write %s", path);
    CHECK(close(fd) == 0, "close %s", path);
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void append(int fd, const void *data, size_t n) {
    CHECK(write(fd, data, n) == (ssize_t)n, "append pack");
}

static void make_pack_method(const char *path, const char *relative_path,
                             const uint8_t *original, size_t original_size,
                             const uint8_t *payload, size_t payload_size,
                             uint8_t method, int corrupt_digest, uint32_t flags) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "create %s", path);
    uint8_t header[DM_PACK_HEADER_SIZE] = {0};
    memcpy(header, DM_PACK_MAGIC, 8);
    put32(header + 8, DM_PACK_VERSION);
    put32(header + 12, flags);
    put64(header + 16, 1);
    put64(header + 24, payload_size);
    append(fd, header, sizeof header);

    uint8_t digest[DM_PACK_DIGEST_SIZE];
    CC_SHA256(original, (CC_LONG)original_size, digest);
    if (corrupt_digest) digest[0] ^= 0xff;
    size_t path_len = strlen(relative_path);
    uint8_t entry[DM_PACK_ENTRY_HEADER_SIZE] = {0};
    put16(entry, (uint16_t)path_len);
    entry[2] = method;
    put64(entry + 4, original_size);
    put64(entry + 12, payload_size);
    memcpy(entry + 20, digest, sizeof digest);
    append(fd, entry, sizeof entry);
    append(fd, relative_path, path_len);
    append(fd, payload, payload_size);
    CHECK(close(fd) == 0, "close pack");
}

static void make_pack(const char *path, const char *relative_path,
                      const uint8_t *payload, size_t n, int corrupt_digest) {
    make_pack_method(path, relative_path, payload, n, payload, n,
                     DM_PACK_METHOD_STORE, corrupt_digest, 0);
}

static void make_pack_declared_payload(const char *path,
                                       const char *relative_path,
                                       const uint8_t *payload, size_t n,
                                       uint64_t declared_payload) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "create %s", path);
    uint8_t header[DM_PACK_HEADER_SIZE] = {0};
    memcpy(header, DM_PACK_MAGIC, 8);
    put32(header + 8, DM_PACK_VERSION);
    put64(header + 16, 1);
    put64(header + 24, declared_payload);
    append(fd, header, sizeof header);

    uint8_t digest[DM_PACK_DIGEST_SIZE];
    CC_SHA256(payload, (CC_LONG)n, digest);
    size_t path_len = strlen(relative_path);
    uint8_t entry[DM_PACK_ENTRY_HEADER_SIZE] = {0};
    put16(entry, (uint16_t)path_len);
    put64(entry + 4, n);
    put64(entry + 12, n);
    memcpy(entry + 20, digest, sizeof digest);
    append(fd, entry, sizeof entry);
    append(fd, relative_path, path_len);
    append(fd, payload, n);
    CHECK(close(fd) == 0, "close pack");
}

static void make_pack_with_nul_path(const char *path) {
    static const char relative[] = {'n', 'e', 's', 't', 'e', 'd', '/',
                                    'v', 'a', 'l', 'u', 'e', '\0', 'x'};
    static const uint8_t payload[] = "nul";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "create %s", path);
    uint8_t header[DM_PACK_HEADER_SIZE] = {0};
    memcpy(header, DM_PACK_MAGIC, 8);
    put32(header + 8, DM_PACK_VERSION);
    put64(header + 16, 1);
    put64(header + 24, sizeof payload - 1);
    append(fd, header, sizeof header);

    uint8_t digest[DM_PACK_DIGEST_SIZE];
    CC_SHA256(payload, (CC_LONG)(sizeof payload - 1), digest);
    uint8_t entry[DM_PACK_ENTRY_HEADER_SIZE] = {0};
    put16(entry, (uint16_t)sizeof relative);
    put64(entry + 4, sizeof payload - 1);
    put64(entry + 12, sizeof payload - 1);
    memcpy(entry + 20, digest, sizeof digest);
    append(fd, entry, sizeof entry);
    append(fd, relative, sizeof relative);
    append(fd, payload, sizeof payload - 1);
    CHECK(close(fd) == 0, "close pack");
}

static void make_pack_with_oversized_entry(const char *path) {
    static const char relative[] = "oversized.bin";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "create %s", path);
    uint8_t header[DM_PACK_HEADER_SIZE] = {0};
    memcpy(header, DM_PACK_MAGIC, 8);
    put32(header + 8, DM_PACK_VERSION);
    put64(header + 16, 1);
    put64(header + 24, DM_PACK_MAX_ENTRY_BYTES + 1ULL);
    append(fd, header, sizeof header);

    uint8_t entry[DM_PACK_ENTRY_HEADER_SIZE] = {0};
    put16(entry, (uint16_t)(sizeof relative - 1));
    put64(entry + 4, DM_PACK_MAX_ENTRY_BYTES + 1ULL);
    put64(entry + 12, DM_PACK_MAX_ENTRY_BYTES + 1ULL);
    append(fd, entry, sizeof entry);
    append(fd, relative, sizeof relative - 1);
    CHECK(ftruncate(fd, (off_t)(DM_PACK_HEADER_SIZE + sizeof entry +
                                sizeof relative - 1 +
                                DM_PACK_MAX_ENTRY_BYTES + 1ULL)) == 0,
          "extend oversized pack");
    CHECK(close(fd) == 0, "close oversized pack");
}

static int count_regular(const char *root) {
    DIR *dir = opendir(root);
    if (!dir) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", root, entry->d_name);
        struct stat st;
        if (stat(path, &st)) continue;
        count += S_ISDIR(st.st_mode) ? count_regular(path) : 1;
    }
    closedir(dir);
    return count;
}

typedef struct {
    const char *pack;
    const char *destination;
    int result;
    char why[512];
} small_stack_args;

static int pack_always_cancel(void *user) {
    int *calls = user;
    (*calls)++;
    return 1;
}

static void *extract_on_small_stack(void *opaque) {
    small_stack_args *args = opaque;
    const char *paths[] = {args->pack};
    args->result = dm_pack_extract(paths, 1, args->destination, NULL, NULL, NULL,
                                   args->why, sizeof args->why);
    return NULL;
}

int main(void) {
    char root[] = "/private/tmp/dm_pack_test.XXXXXX";
    CHECK(mkdtemp(root) != NULL, "mkdtemp");
    if (failures) return 1;
    char pack[PATH_MAX], pack2[PATH_MAX], compressed[PATH_MAX], bad[PATH_MAX];
    char destination[PATH_MAX], output[PATH_MAX], zero[PATH_MAX];
    snprintf(pack, sizeof pack, "%s/a.dmp", root);
    snprintf(pack2, sizeof pack2, "%s/b.dmp", root);
    snprintf(compressed, sizeof compressed, "%s/compressed.dmp", root);
    snprintf(bad, sizeof bad, "%s/bad.dmp", root);
    snprintf(destination, sizeof destination, "%s/missing/parents/output", root);
    snprintf(output, sizeof output, "%s/missing/parents/output/nested/value.bin", root);
    snprintf(zero, sizeof zero, "%s/missing/parents/output/nested/zero", root);

    const uint8_t payload[] = "ACNH pack test";
    make_pack(pack, "nested/value.bin", payload, sizeof payload - 1, 0);
    make_pack(pack2, "nested/zero", (const uint8_t *)"", 0, 0);
    // Raw RFC 1951 DEFLATE produced by Python zlib.compressobj(
    // 6, zlib.DEFLATED, -15) for b"ACNH compressed pack" * 64.
    static const uint8_t compressed_payload[] = {
        0x73,0x74,0xf6,0xf3,0x50,0x48,0xce,0xcf,0x2d,0x28,
        0x4a,0x2d,0x2e,0x4e,0x4d,0x51,0x28,0x48,0x4c,0xce,
        0x76,0x1c,0x15,0x1b,0x15,0x1b,0x15,0x1b,0x15,0x1b,
        0x01,0x62,0x00,
    };
    uint8_t compressed_original[1280] = {0};
    for (size_t i = 0; i < 64; i++) {
        memcpy(compressed_original + i * (sizeof "ACNH compressed pack" - 1),
               "ACNH compressed pack", sizeof "ACNH compressed pack" - 1);
    }
    make_pack_method(compressed, "nested/compressed.txt",
                     compressed_original, sizeof compressed_original,
                     compressed_payload, sizeof compressed_payload,
                     DM_PACK_METHOD_DEFLATE, 0, 0);
    make_pack(bad, "../escape", payload, sizeof payload - 1, 0);

    dm_pack_info info;
    char why[512];
    CHECK(dm_pack_inspect(pack, &info, why, sizeof why) == DM_PACK_OK,
          "inspect: %s", why);
    CHECK(info.entry_count == 1, "entry count");
    CHECK(info.original_bytes == sizeof payload - 1, "original bytes");

    const char *paths[] = {pack, pack2, compressed};
    int progress_calls = 0;
    CHECK(dm_pack_extract(paths, 3, destination, NULL, NULL, NULL,
                          why, sizeof why) == DM_PACK_OK,
          "extract: %s", why);
    int fd = open(output, O_RDONLY);
    char got[64] = {0};
    CHECK(fd >= 0 && read(fd, got, sizeof got - 1) == (ssize_t)(sizeof payload - 1),
          "read extracted");
    if (fd >= 0) close(fd);
    CHECK(!memcmp(got, payload, sizeof payload - 1), "extracted bytes");
    struct stat st;
    CHECK(!stat(zero, &st) && st.st_size == 0, "empty file");
    CHECK(dm_pack_is_complete(destination, why, sizeof why), "complete marker");
    snprintf(output, sizeof output, "%s/nested/compressed.txt", destination);
    fd = open(output, O_RDONLY);
    uint8_t decoded[sizeof compressed_original] = {0};
    CHECK(fd >= 0 && read(fd, decoded, sizeof decoded) ==
          (ssize_t)sizeof decoded, "read compressed entry");
    if (fd >= 0) close(fd);
    CHECK(!memcmp(decoded, compressed_original, sizeof decoded),
          "raw DEFLATE entry");
    CHECK(count_regular(destination) == 4, "regular file count (3 files + marker)");

    char cancelling[PATH_MAX];
    snprintf(cancelling, sizeof cancelling, "%s/cancelling", root);
    CHECK(dm_pack_extract(paths, 3, cancelling, NULL,
                          pack_always_cancel, &progress_calls,
                          why, sizeof why) == DM_PACK_ERR_CANCELLED,
          "cancel must stop extraction");
    CHECK(progress_calls > 0, "cancel callback must be consulted");
    CHECK(access(cancelling, F_OK) != 0,
          "cancelled import must remove staging");

    char pair1[PATH_MAX], pair2[PATH_MAX];
    char pair1_out[PATH_MAX], pair2_out[PATH_MAX];
    snprintf(pair1, sizeof pair1, "%s/pair-001.dmp", root);
    snprintf(pair2, sizeof pair2, "%s/pair-002.dmp", root);
    snprintf(pair1_out, sizeof pair1_out, "%s/pair-out-1", root);
    snprintf(pair2_out, sizeof pair2_out, "%s/pair-out-2", root);
    CHECK(dm_pack_inspect(pack, &info, why, sizeof why) == DM_PACK_OK,
          "inspect standalone: %s", why);
    CHECK(info.pair_index == 0 && info.pair_count == 0,
          "standalone pair metadata");
    make_pack_method(pair1, "first.bin", payload, sizeof payload - 1,
                     payload, sizeof payload - 1, DM_PACK_METHOD_STORE, 0,
                     1u | (2u << 16));
    make_pack_method(pair2, "second.bin", payload, sizeof payload - 1,
                     payload, sizeof payload - 1, DM_PACK_METHOD_STORE, 0,
                     2u | (2u << 16));
    const char *incomplete[] = {pair1};
    CHECK(dm_pack_extract(incomplete, 1, pair1_out, NULL, NULL, NULL,
                          why, sizeof why) == DM_PACK_ERR_FORMAT,
          "incomplete pair must not publish");
    CHECK(access(pair1_out, F_OK) != 0, "incomplete pair staging must be absent");
    CHECK(dm_pack_inspect(pair1, &info, why, sizeof why) == DM_PACK_OK,
          "inspect pair: %s", why);
    CHECK(info.pair_index == 1 && info.pair_count == 2, "pair metadata");
    const char *complete[] = {pair1, pair2};
    CHECK(dm_pack_extract(complete, 2, pair2_out, NULL, NULL, NULL,
                          why, sizeof why) == DM_PACK_OK,
          "complete pair: %s", why);
    snprintf(output, sizeof output, "%s/first.bin", pair2_out);
    CHECK(access(output, R_OK) == 0, "paired first file");
    snprintf(output, sizeof output, "%s/second.bin", pair2_out);
    CHECK(access(output, R_OK) == 0, "paired second file");

    make_pack(pair2, "third.bin", payload, sizeof payload - 1, 0);
    const char *replacement[] = {pair2};
    CHECK(dm_pack_extract(replacement, 1, pair2_out, NULL, NULL, NULL,
                          why, sizeof why) == DM_PACK_OK,
          "replacement staging: %s", why);
    snprintf(output, sizeof output, "%s/first.bin", pair2_out);
    CHECK(access(output, F_OK) != 0,
          "staging extraction must not inherit the live layer");
    snprintf(output, sizeof output, "%s/third.bin", pair2_out);
    CHECK(access(output, R_OK) == 0, "replacement staging has new file");

    const char *bad_paths[] = {bad};
    char unsafe[PATH_MAX], escape[PATH_MAX];
    snprintf(unsafe, sizeof unsafe, "%s/unsafe", root);
    snprintf(escape, sizeof escape, "%s/escape", root);
    CHECK(dm_pack_extract(bad_paths, 1, unsafe, NULL, NULL, NULL,
                          why, sizeof why) == DM_PACK_ERR_PATH,
          "path traversal must fail");
    CHECK(access(escape, F_OK) != 0, "path traversal must not write outside destination");

    char corrupt[PATH_MAX], corrupt_out[PATH_MAX];
    snprintf(corrupt, sizeof corrupt, "%s/corrupt.dmp", root);
    snprintf(corrupt_out, sizeof corrupt_out, "%s/corrupt-out", root);
    make_pack(corrupt, "wrong.bin", payload, sizeof payload - 1, 1);
    const char *corrupt_paths[] = {corrupt};
    CHECK(dm_pack_extract(corrupt_paths, 1, corrupt_out, NULL, NULL, NULL,
                          why, sizeof why) == DM_PACK_ERR_DIGEST,
          "corrupt digest must fail");
    CHECK(access(corrupt_out, F_OK) != 0, "failed import must not publish");

    char duplicate1[PATH_MAX], duplicate2[PATH_MAX], duplicate_out[PATH_MAX];
    snprintf(duplicate1, sizeof duplicate1, "%s/duplicate-1.dmp", root);
    snprintf(duplicate2, sizeof duplicate2, "%s/duplicate-2.dmp", root);
    snprintf(duplicate_out, sizeof duplicate_out, "%s/duplicate-out", root);
    make_pack(duplicate1, "same.bin", payload, sizeof payload - 1, 0);
    make_pack(duplicate2, "same.bin", payload, sizeof payload - 1, 0);
    const char *duplicate_paths[] = {duplicate1, duplicate2};
    CHECK(dm_pack_extract(duplicate_paths, 2, duplicate_out, NULL, NULL, NULL,
                          why, sizeof why) == DM_PACK_ERR_DUPLICATE,
          "duplicate path must fail instead of replacing the first entry");
    CHECK(access(duplicate_out, F_OK) != 0,
          "duplicate path failure must remove staging");

    char case1[PATH_MAX], case2[PATH_MAX], case_out[PATH_MAX];
    snprintf(case1, sizeof case1, "%s/case-1.dmp", root);
    snprintf(case2, sizeof case2, "%s/case-2.dmp", root);
    snprintf(case_out, sizeof case_out, "%s/case-out", root);
    make_pack(case1, "Case.bin", payload, sizeof payload - 1, 0);
    make_pack(case2, "case.bin", payload, sizeof payload - 1, 0);
    const char *case_paths[] = {case1, case2};
    CHECK(dm_pack_extract(case_paths, 2, case_out, NULL, NULL, NULL,
                          why, sizeof why) == DM_PACK_ERR_DUPLICATE,
          "case-insensitive path conflict must fail");
    CHECK(access(case_out, F_OK) != 0,
          "case conflict failure must remove staging");

    char nul_pack[PATH_MAX], nul_out[PATH_MAX];
    snprintf(nul_pack, sizeof nul_pack, "%s/nul.dmp", root);
    snprintf(nul_out, sizeof nul_out, "%s/nul-out", root);
    make_pack_with_nul_path(nul_pack);
    const char *nul_paths[] = {nul_pack};
    CHECK(dm_pack_extract(nul_paths, 1, nul_out, NULL, NULL, NULL,
                          why, sizeof why) == DM_PACK_ERR_PATH,
          "embedded NUL in a pack path must fail");
    CHECK(access(nul_out, F_OK) != 0, "unsafe path failure must remove staging");

    char marker_pack[PATH_MAX], marker_out[PATH_MAX];
    snprintf(marker_pack, sizeof marker_pack, "%s/marker.dmp", root);
    snprintf(marker_out, sizeof marker_out, "%s/marker-out", root);
    make_pack(marker_pack, DM_PACK_MARKER, payload, sizeof payload - 1, 0);
    const char *marker_paths[] = {marker_pack};
    CHECK(dm_pack_extract(marker_paths, 1, marker_out, NULL, NULL, NULL,
                          why, sizeof why) == DM_PACK_ERR_PATH,
          "pack must not supply the completion marker");

    char declared_pack[PATH_MAX];
    snprintf(declared_pack, sizeof declared_pack, "%s/declared.dmp", root);
    make_pack_declared_payload(declared_pack, "declared.bin",
                               payload, sizeof payload - 1, 1);
    CHECK(dm_pack_inspect(declared_pack, &info, why, sizeof why) ==
          DM_PACK_ERR_FORMAT,
          "declared payload must equal the sum of stored entry sizes");

    char too_many_pair[PATH_MAX], too_many_out[PATH_MAX];
    snprintf(too_many_pair, sizeof too_many_pair, "%s/too-many.dmp", root);
    snprintf(too_many_out, sizeof too_many_out, "%s/too-many-out", root);
    make_pack_method(too_many_pair, "too-many.bin", payload, sizeof payload - 1,
                     payload, sizeof payload - 1, DM_PACK_METHOD_STORE, 0,
                     2000u | (2000u << 16));
    const char *too_many_paths[] = {too_many_pair};
    CHECK(dm_pack_extract(too_many_paths, 1, too_many_out, NULL, NULL, NULL,
                          why, sizeof why) == DM_PACK_ERR_FORMAT,
          "pair counts above the importer limit must fail safely");
    CHECK(access(too_many_out, F_OK) != 0,
          "oversized pair metadata must not create staging");

    char oversized_pack[PATH_MAX];
    snprintf(oversized_pack, sizeof oversized_pack, "%s/oversized.dmp", root);
    make_pack_with_oversized_entry(oversized_pack);
    CHECK(dm_pack_inspect(oversized_pack, &info, why, sizeof why) ==
          DM_PACK_ERR_FORMAT,
          "entry larger than the 64 MiB device limit must fail");

    char absent[PATH_MAX], absent_out[PATH_MAX];
    snprintf(absent, sizeof absent, "%s/absent.dmp", root);
    snprintf(absent_out, sizeof absent_out, "%s/absent-out", root);
    mkdirs(absent_out);
    const char *absent_paths[] = {absent};
    CHECK(dm_pack_extract(absent_paths, 1, absent_out, NULL, NULL, NULL,
                          why, sizeof why) == DM_PACK_ERR_IO,
          "missing pack must fail");
    CHECK(access(absent_out, F_OK) != 0,
          "a failure before extraction must still remove staging");

    char staging[PATH_MAX];
    snprintf(staging, sizeof staging, "%s/publish-staging", root);
    mkdirs(staging);
    snprintf(output, sizeof output, "%s/publish-staging/new.bin", root);
    write_file(output, "new", 3);
    char old[PATH_MAX];
    snprintf(old, sizeof old, "%s/publish-old", root);
    mkdirs(old);
    snprintf(output, sizeof output, "%s/publish-old/old.bin", root);
    write_file(output, "old", 3);
    CHECK(dm_pack_publish(staging, old, why, sizeof why) == DM_PACK_OK,
          "publish: %s", why);
    snprintf(output, sizeof output, "%s/new.bin", old);
    CHECK(access(output, R_OK) == 0, "published new file");
    snprintf(output, sizeof output, "%s/old.bin", old);
    CHECK(access(output, F_OK) != 0, "old tree removed");
    snprintf(output, sizeof output, "%s/new.bin", old);

    char bad_marker[PATH_MAX];
    snprintf(bad_marker, sizeof bad_marker, "%s/%s", old, DM_PACK_MARKER);
    write_file(bad_marker, "not-a-marker\n", 13);
    CHECK(!dm_pack_is_complete(old, why, sizeof why),
          "malformed completion marker must not count as complete");
    write_file(bad_marker, "files=1 bytes=3\n", 16);
    CHECK(dm_pack_is_complete(old, why, sizeof why),
          "valid completion marker: %s", why);
    CHECK(unlink(output) == 0, "remove published file for completeness check");
    CHECK(!dm_pack_is_complete(old, why, sizeof why),
          "missing published file must invalidate completion marker");
    write_file(output, "new", 3);
    CHECK(dm_pack_is_complete(old, why, sizeof why),
          "restored published file: %s", why);
    CHECK(truncate(output, 2) == 0, "truncate published file");
    CHECK(!dm_pack_is_complete(old, why, sizeof why),
          "truncated published file must invalidate completion marker");

    char large_pack[PATH_MAX], large_out[PATH_MAX], large_expected[PATH_MAX];
    snprintf(large_pack, sizeof large_pack, "%s/large.dmp", root);
    snprintf(large_out, sizeof large_out, "%s/large-out", root);
    snprintf(large_expected, sizeof large_expected, "%s/large.bin", large_out);
    size_t large_size = DM_PACK_MAX_ENTRY_BYTES;
    uint8_t *large = malloc(large_size);
    CHECK(large != NULL, "allocate 64 MiB fixture");
    if (!large) return 1;
    for (size_t i = 0; i < large_size; i++) large[i] = (uint8_t)(i * 131u + 17u);
    make_pack(large_pack, "large.bin", large, large_size, 0);

    small_stack_args small = {
        .pack = large_pack,
        .destination = large_out,
    };
    pthread_attr_t attr;
    pthread_t thread;
    CHECK(pthread_attr_init(&attr) == 0, "pthread_attr_init");
    CHECK(pthread_attr_setstacksize(&attr, 512u << 10) == 0,
          "pthread_attr_setstacksize");
    CHECK(pthread_create(&thread, &attr, extract_on_small_stack, &small) == 0,
          "pthread_create");
    CHECK(pthread_join(thread, NULL) == 0, "pthread_join");
    pthread_attr_destroy(&attr);
    CHECK(small.result == DM_PACK_OK, "extract on 512 KiB stack: %s", small.why);
    int large_fd = open(large_expected, O_RDONLY);
    CHECK(large_fd >= 0, "open large extracted file");
    ssize_t large_read = large_fd >= 0 ? read(large_fd, large, large_size) : -1;
    if (large_fd >= 0) close(large_fd);
    CHECK(large_read == (ssize_t)large_size, "large extracted size");
    free(large);

    if (!failures) puts("t_dm_pack: PASS");
    return failures != 0;
}
