#include "dm_pack.h"

#include <CommonCrypto/CommonDigest.h>
#include <compression.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DM_PACK_IO_CHUNK (1u << 20)

static int rm_tree(const char *path);

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static uint16_t pair_u16(uint32_t flags, int high) {
    return high ? (uint16_t)(flags >> 16) : (uint16_t)(flags & 0xffffu);
}

static void set_error(char *why, size_t why_n, const char *message) {
    if (why && why_n) snprintf(why, why_n, "%s", message);
}

static void set_errno_error(char *why, size_t why_n, const char *op,
                            const char *path) {
    if (why && why_n) {
        snprintf(why, why_n, "%s %s: %s", op, path ? path : "", strerror(errno));
    }
}

static int read_exact(int fd, void *buffer, size_t length) {
    uint8_t *p = buffer;
    while (length) {
        ssize_t n = read(fd, p, length);
        if (n > 0) {
            p += n;
            length -= (size_t)n;
        } else if (n == 0) {
            return 0;
        } else if (errno != EINTR) {
            return -1;
        }
    }
    return 1;
}

static int write_exact(int fd, const void *buffer, size_t length) {
    const uint8_t *p = buffer;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n > 0) {
            p += n;
            length -= (size_t)n;
        } else if (n < 0 && errno != EINTR) {
            return -1;
        }
    }
    return 0;
}

static bool valid_path(const char *path) {
    if (!path || !path[0] || path[0] == '/' || path[0] == '\\') return false;
    if (strcmp(path, DM_PACK_MARKER) == 0) return false;
    const char *p = path;
    while (*p) {
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == 0 || (n == 1 && p[0] == '.') ||
            (n == 2 && p[0] == '.' && p[1] == '.')) {
            return false;
        }
        if (!end) break;
        p = end + 1;
    }
    return true;
}

static int make_parent_dirs(const char *root, const char *relative) {
    size_t root_len = strlen(root);
    char full[PATH_MAX];
    if (root_len + strlen(relative) + 2 > sizeof full) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(full, root, root_len);
    full[root_len] = '/';
    strcpy(full + root_len + 1, relative);
    for (char *p = full + root_len + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(full, 0755) && errno != EEXIST) return -1;
        struct stat st;
        if (stat(full, &st) || !S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            return -1;
        }
        *p = '/';
    }
    return 0;
}

static int make_directory_path(const char *path) {
    char full[PATH_MAX];
    size_t n = strlen(path);
    if (!n || n >= sizeof full) {
        errno = n ? ENAMETOOLONG : EINVAL;
        return -1;
    }
    memcpy(full, path, n + 1);
    for (char *p = full + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(full, 0755) && errno != EEXIST) return -1;
        struct stat st;
        if (stat(full, &st) || !S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            return -1;
        }
        *p = '/';
    }
    if (mkdir(full, 0755) && errno != EEXIST) return -1;
    struct stat st;
    if (stat(full, &st) || !S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} path_set;

static void path_set_free(path_set *set) {
    for (size_t i = 0; i < set->count; i++) free(set->items[i]);
    free(set->items);
    memset(set, 0, sizeof *set);
}

static int path_set_add(path_set *set, const char *path) {
    if (set->count == set->capacity) {
        size_t capacity = set->capacity ? set->capacity * 2 : 1024;
        char **items = realloc(set->items, capacity * sizeof *items);
        if (!items) return -1;
        set->items = items;
        set->capacity = capacity;
    }
    char *copy = strdup(path);
    if (!copy) return -1;
    set->items[set->count++] = copy;
    return 0;
}

static int path_compare(const void *left, const void *right) {
    const char *const *a = left;
    const char *const *b = right;
    int result = strcasecmp(*a, *b);
    return result ? result : strcmp(*a, *b);
}

static int path_set_find_duplicates(path_set *set, char *why, size_t why_n) {
    if (set->count < 2) return DM_PACK_OK;
    qsort(set->items, set->count, sizeof *set->items, path_compare);
    for (size_t i = 1; i < set->count; i++) {
        if (strcasecmp(set->items[i - 1], set->items[i]) == 0) {
            if (why && why_n) {
                snprintf(why, why_n, "资源包包含冲突路径：%s / %s",
                         set->items[i - 1], set->items[i]);
            }
            return DM_PACK_ERR_DUPLICATE;
        }
    }
    return DM_PACK_OK;
}

static int inspect_fd(int fd, dm_pack_info *info, char *why, size_t why_n) {
    uint8_t header[DM_PACK_HEADER_SIZE];
    off_t end = lseek(fd, 0, SEEK_END);
    if (end < 0 || lseek(fd, 0, SEEK_SET) < 0) {
        set_errno_error(why, why_n, "无法读取资源包", "");
        return DM_PACK_ERR_IO;
    }
    uint32_t flags;
    ssize_t header_read = read_exact(fd, header, sizeof header);
    if (header_read != 1 ||
        memcmp(header, DM_PACK_MAGIC, 8) ||
        le32(header + 8) != DM_PACK_VERSION) {
        set_error(why, why_n, "资源包格式不正确");
        return DM_PACK_ERR_FORMAT;
    }
    if (lseek(fd, DM_PACK_HEADER_SIZE, SEEK_SET) < 0) {
        set_errno_error(why, why_n, "无法读取资源包", "");
        return DM_PACK_ERR_IO;
    }
    flags = le32(header + 12);
    uint16_t pair_index = pair_u16(flags, 0);
    uint16_t pair_count = pair_u16(flags, 1);
    if ((pair_index == 0) != (pair_count == 0) ||
        (pair_count != 0 && pair_index > pair_count)) {
        set_error(why, why_n, "资源包批次字段不正确");
        return DM_PACK_ERR_FORMAT;
    }
    uint64_t count = le64(header + 16);
    uint64_t payload = le64(header + 24);
    if (!count || count > 10000000ULL || payload > (1ULL << 52)) {
        set_error(why, why_n, "资源包头字段越界");
        return DM_PACK_ERR_FORMAT;
    }
    uint64_t total = 0;
    uint64_t payload_total = 0;
    uint64_t current = DM_PACK_HEADER_SIZE;
    for (uint64_t i = 0; i < count; i++) {
        uint8_t entry[DM_PACK_ENTRY_HEADER_SIZE];
        if (read_exact(fd, entry, sizeof entry) != 1) {
            set_error(why, why_n, "资源包条目头被截断");
            return DM_PACK_ERR_FORMAT;
        }
        uint16_t path_len = le16(entry);
        uint8_t method = entry[2];
        uint64_t original = le64(entry + 4);
        uint64_t stored = le64(entry + 12);
        if (!path_len || path_len > DM_PACK_MAX_PATH ||
            (method != DM_PACK_METHOD_STORE && method != DM_PACK_METHOD_DEFLATE) ||
            (method == DM_PACK_METHOD_STORE && stored != original) ||
            stored > original || original > DM_PACK_MAX_ENTRY_BYTES ||
            current + DM_PACK_ENTRY_HEADER_SIZE + path_len + stored >
                (uint64_t)end) {
            set_error(why, why_n, "资源包条目字段越界");
            return DM_PACK_ERR_FORMAT;
        }
        char path[DM_PACK_MAX_PATH + 1];
        if (read_exact(fd, path, path_len) != 1) {
            set_error(why, why_n, "资源包路径被截断");
            return DM_PACK_ERR_FORMAT;
        }
        path[path_len] = '\0';
        if (memchr(path, '\0', path_len) || !valid_path(path)) {
            set_error(why, why_n, "资源包路径不安全");
            return DM_PACK_ERR_PATH;
        }
        if (lseek(fd, (off_t)stored, SEEK_CUR) < 0) {
            set_error(why, why_n, "资源包条目被截断");
            return DM_PACK_ERR_FORMAT;
        }
        total += original;
        payload_total += stored;
        current += DM_PACK_ENTRY_HEADER_SIZE + path_len + stored;
    }
    if (current != (uint64_t)end || payload_total != payload) {
        set_error(why, why_n, "资源包长度与头声明不一致");
        return DM_PACK_ERR_FORMAT;
    }
    if (info) {
        info->pack_size = (uint64_t)end;
        info->entry_count = count;
        info->original_bytes = total;
        info->stored_bytes = payload_total;
        info->pair_index = pair_index;
        info->pair_count = pair_count;
    }
    return DM_PACK_OK;
}

int dm_pack_inspect(const char *path, dm_pack_info *out, char *why, size_t why_n) {
    if (out) memset(out, 0, sizeof *out);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_errno_error(why, why_n, "无法打开资源包", path);
        return DM_PACK_ERR_IO;
    }
    if (why && why_n) why[0] = '\0';
    int result = inspect_fd(fd, out, why, why_n);
    close(fd);
    return result;
}

typedef struct {
    dm_pack_progress_fn progress;
    dm_pack_cancel_fn cancelled;
    void *user;
    uint64_t total_bytes;
    uint64_t total_entries;
    uint64_t done_bytes;
    uint64_t done_entries;
    uint64_t last_report;
} extract_ctx;

static int report(extract_ctx *ctx, const char *path) {
    if (ctx->progress &&
        (ctx->done_entries == ctx->total_entries || ctx->done_entries == 1 ||
         ctx->done_entries % 32 == 0 ||
         ctx->done_bytes - ctx->last_report >= (8u << 20))) {
        ctx->last_report = ctx->done_bytes;
        if (ctx->progress(ctx->done_bytes, ctx->total_bytes, ctx->done_entries,
                          ctx->total_entries, path, ctx->user) != 0) {
            return DM_PACK_ERR_CANCELLED;
        }
    }
    if (ctx->cancelled && ctx->cancelled(ctx->user)) return DM_PACK_ERR_CANCELLED;
    return DM_PACK_OK;
}

static int copy_stored(int in_fd, int out_fd, uint64_t stored,
                       CC_SHA256_CTX *sha, char *why, size_t why_n) {
    uint8_t *buffer = malloc(DM_PACK_IO_CHUNK);
    if (!buffer) {
        set_error(why, why_n, "资源包复制缓冲区分配失败");
        return DM_PACK_ERR_MEMORY;
    }
    int result = DM_PACK_OK;
    while (stored) {
        size_t n = stored < DM_PACK_IO_CHUNK ? (size_t)stored : DM_PACK_IO_CHUNK;
        if (read_exact(in_fd, buffer, n) != 1) {
            set_error(why, why_n, "资源包条目被截断");
            result = DM_PACK_ERR_FORMAT;
            break;
        }
        CC_SHA256_Update(sha, buffer, (CC_LONG)n);
        if (write_exact(out_fd, buffer, n)) {
            set_errno_error(why, why_n, "写入", "");
            result = DM_PACK_ERR_IO;
            break;
        }
        stored -= n;
    }
    free(buffer);
    return result;
}

static int deflate_entry(int in_fd, int out_fd, uint64_t original,
                         uint64_t stored, CC_SHA256_CTX *sha,
                         char *why, size_t why_n) {
    uint8_t *input = malloc(stored ? (size_t)stored : 1);
    uint8_t *output = malloc(original ? (size_t)original : 1);
    if (!input || !output) {
        free(input);
        free(output);
        set_error(why, why_n, "资源包解压缓冲区过大");
        return DM_PACK_ERR_MEMORY;
    }
    int result = DM_PACK_OK;
    if (read_exact(in_fd, input, (size_t)stored) != 1) {
        set_error(why, why_n, "资源包条目被截断");
        result = DM_PACK_ERR_FORMAT;
        goto done;
    }
    size_t actual = compression_decode_buffer(
        output, (size_t)original, input, (size_t)stored, NULL,
        COMPRESSION_ZLIB);
    if (actual != (size_t)original) {
        set_error(why, why_n, "资源包解压失败");
        result = DM_PACK_ERR_FORMAT;
        goto done;
    }
    CC_SHA256_Update(sha, output, (CC_LONG)actual);
    if (write_exact(out_fd, output, actual)) {
        set_errno_error(why, why_n, "写入", "");
        result = DM_PACK_ERR_IO;
    }
done:
    free(input);
    free(output);
    return result;
}

static int extract_one(int pack_fd, const char *destination,
                       extract_ctx *ctx, char *why, size_t why_n) {
    uint8_t header[DM_PACK_HEADER_SIZE];
    if (read_exact(pack_fd, header, sizeof header) != 1 ||
        memcmp(header, DM_PACK_MAGIC, 8) ||
        le32(header + 8) != DM_PACK_VERSION) {
        set_error(why, why_n, "资源包格式不正确");
        return DM_PACK_ERR_FORMAT;
    }
    uint64_t count = le64(header + 16);
    for (uint64_t i = 0; i < count; i++) {
        uint8_t entry[DM_PACK_ENTRY_HEADER_SIZE];
        if (read_exact(pack_fd, entry, sizeof entry) != 1) {
            set_error(why, why_n, "资源包条目头被截断");
            return DM_PACK_ERR_FORMAT;
        }
        uint16_t path_len = le16(entry);
        uint8_t method = entry[2];
        uint64_t original = le64(entry + 4);
        uint64_t stored = le64(entry + 12);
        uint8_t expected[DM_PACK_DIGEST_SIZE];
        memcpy(expected, entry + 20, sizeof expected);
        char relative[DM_PACK_MAX_PATH + 1];
        if (!path_len || path_len > DM_PACK_MAX_PATH ||
            read_exact(pack_fd, relative, path_len) != 1) {
            set_error(why, why_n, "资源包路径被截断");
            return DM_PACK_ERR_FORMAT;
        }
        relative[path_len] = '\0';
        if (!valid_path(relative)) {
            set_error(why, why_n, "资源包路径不安全");
            return DM_PACK_ERR_PATH;
        }
        int r = report(ctx, relative);
        if (r != DM_PACK_OK) return r;
        if (make_parent_dirs(destination, relative)) {
            set_errno_error(why, why_n, "无法创建父目录", relative);
            return DM_PACK_ERR_IO;
        }
        char full[PATH_MAX];
        if (snprintf(full, sizeof full, "%s/%s", destination, relative) >=
            (int)sizeof full) {
            set_error(why, why_n, "目标路径过长");
            return DM_PACK_ERR_PATH;
        }
        int out_fd = open(full, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (out_fd < 0 && errno == EEXIST) {
            if (unlink(full)) {
                set_errno_error(why, why_n, "无法替换", full);
                return DM_PACK_ERR_IO;
            }
            out_fd = open(full, O_WRONLY | O_CREAT | O_EXCL, 0644);
        }
        if (out_fd < 0) {
            set_errno_error(why, why_n, "无法创建", full);
            return DM_PACK_ERR_IO;
        }
        CC_SHA256_CTX sha;
        CC_SHA256_Init(&sha);
        r = method == DM_PACK_METHOD_DEFLATE
            ? deflate_entry(pack_fd, out_fd, original, stored, &sha, why, why_n)
            : copy_stored(pack_fd, out_fd, stored, &sha, why, why_n);
        if (r == DM_PACK_OK) {
            uint8_t digest[DM_PACK_DIGEST_SIZE];
            CC_SHA256_Final(digest, &sha);
            if (memcmp(digest, expected, sizeof digest)) {
                set_error(why, why_n, "资源包条目校验失败");
                r = DM_PACK_ERR_DIGEST;
            }
        }
        if (r == DM_PACK_OK && fsync(out_fd)) {
            set_errno_error(why, why_n, "同步", full);
            r = DM_PACK_ERR_IO;
        }
        if (close(out_fd) && r == DM_PACK_OK) {
            set_errno_error(why, why_n, "关闭", full);
            r = DM_PACK_ERR_IO;
        }
        if (r != DM_PACK_OK) return r;
        ctx->done_bytes += original;
        ctx->done_entries++;
    }
    return DM_PACK_OK;
}

int dm_pack_extract(const char *const *paths, size_t path_count,
                    const char *destination,
                    dm_pack_progress_fn progress, dm_pack_cancel_fn cancelled,
                    void *user, char *why, size_t why_n) {
    if (!destination || !destination[0]) {
        set_error(why, why_n, "资源导入目标目录无效");
        return DM_PACK_ERR_PATH;
    }
    if (!path_count || !paths) {
        set_error(why, why_n, "没有可导入的资源包");
        return DM_PACK_ERR_FORMAT;
    }
    extract_ctx ctx = {.progress = progress, .cancelled = cancelled, .user = user};
    uint16_t pair_count = 0;
    bool seen[1001] = {0};
    path_set paths_seen = {0};
    int result = DM_PACK_OK;
    for (size_t i = 0; i < path_count; i++) {
        dm_pack_info info = {0};
        int r = dm_pack_inspect(paths[i], &info, why, why_n);
        if (r != DM_PACK_OK) { result = r; goto done; }
        if (info.pair_count) {
            if (info.pair_count > 1000) {
                set_error(why, why_n, "资源包批次数超过上限");
                result = DM_PACK_ERR_FORMAT;
                goto done;
            }
            if (!pair_count) pair_count = info.pair_count;
            if (pair_count != info.pair_count || seen[info.pair_index]) {
                set_error(why, why_n, "资源包批次不完整或重复");
                result = DM_PACK_ERR_FORMAT;
                goto done;
            }
            seen[info.pair_index] = true;
        } else if (pair_count) {
            set_error(why, why_n, "资源包批次字段不一致");
            result = DM_PACK_ERR_FORMAT;
            goto done;
        }
        ctx.total_bytes += info.original_bytes;
        ctx.total_entries += info.entry_count;
    }
    if (pair_count) {
        for (uint16_t i = 1; i <= pair_count; i++) {
            if (!seen[i]) {
                set_error(why, why_n, "资源包批次尚未齐全");
                result = DM_PACK_ERR_FORMAT;
                goto done;
            }
        }
    }
    for (size_t i = 0; i < path_count; i++) {
        int fd = open(paths[i], O_RDONLY);
        if (fd < 0) {
            set_errno_error(why, why_n, "无法打开资源包", paths[i]);
            result = DM_PACK_ERR_IO;
            goto done;
        }
        uint8_t header[DM_PACK_HEADER_SIZE];
        int r = read_exact(fd, header, sizeof header);
        if (r == 1) {
            uint32_t flags = le32(header + 12);
            if (pair_u16(flags, 0) != 0 && pair_u16(flags, 0) > pair_u16(flags, 1)) {
                set_error(why, why_n, "资源包批次字段不正确");
                r = -1;
            }
        }
        if (r == 1) {
            uint64_t count = le64(header + 16);
            for (uint64_t entry_index = 0; entry_index < count; entry_index++) {
                uint8_t entry[DM_PACK_ENTRY_HEADER_SIZE];
                if (read_exact(fd, entry, sizeof entry) != 1) {
                    set_error(why, why_n, "资源包条目头被截断");
                    r = -1;
                    break;
                }
                uint16_t path_len = le16(entry);
                uint64_t stored = le64(entry + 12);
                char path[DM_PACK_MAX_PATH + 1];
                if (read_exact(fd, path, path_len) != 1) {
                    set_error(why, why_n, "资源包路径被截断");
                    r = -1;
                    break;
                }
                path[path_len] = '\0';
                if (path_set_add(&paths_seen, path)) {
                    set_error(why, why_n, "资源包路径索引分配失败");
                    r = -2;
                    break;
                }
                if (lseek(fd, (off_t)stored, SEEK_CUR) < 0) {
                    set_error(why, why_n, "资源包条目被截断");
                    r = -1;
                    break;
                }
            }
        }
        if (close(fd) && r == 1) {
            set_errno_error(why, why_n, "关闭资源包", paths[i]);
            r = -1;
        }
        if (r != 1) {
            result = r == -2 ? DM_PACK_ERR_MEMORY : DM_PACK_ERR_FORMAT;
            goto done;
        }
    }
    result = path_set_find_duplicates(&paths_seen, why, why_n);
    if (result != DM_PACK_OK) goto done;
    if (rm_tree(destination)) {
        set_errno_error(why, why_n, "无法清理旧 staging", destination);
        result = DM_PACK_ERR_IO;
        goto done;
    }
    // This destination is a private staging directory.  Starting from a
    // clean tree is what makes a multi-pack layer all-or-nothing: the app
    // publishes the finished tree with dm_pack_publish() only after every
    // pack has been verified.  Never merge the live layer into staging here,
    // because an extraction failure would then be able to delete resources
    // that were already published.
    if (make_directory_path(destination)) {
        set_errno_error(why, why_n, "无法创建目录", destination);
        result = DM_PACK_ERR_IO;
        goto done;
    }
    if (report(&ctx, "") != DM_PACK_OK) {
        rm_tree(destination);
        set_error(why, why_n, "资源包导入已取消");
        result = DM_PACK_ERR_CANCELLED;
        goto done;
    }
    for (size_t i = 0; i < path_count; i++) {
        int fd = open(paths[i], O_RDONLY);
        if (fd < 0) {
            set_errno_error(why, why_n, "无法打开资源包", paths[i]);
            rm_tree(destination);
            result = DM_PACK_ERR_IO;
            goto done;
        }
        int r = extract_one(fd, destination, &ctx, why, why_n);
        if (close(fd) && r == DM_PACK_OK) {
            set_errno_error(why, why_n, "关闭资源包", paths[i]);
            r = DM_PACK_ERR_IO;
        }
        if (r != DM_PACK_OK) {
            rm_tree(destination);
            result = r;
            goto done;
        }
    }
    if (report(&ctx, "") != DM_PACK_OK) {
        rm_tree(destination);
        set_error(why, why_n, "资源包导入已取消");
        result = DM_PACK_ERR_CANCELLED;
        goto done;
    }
    char marker[PATH_MAX];
    if (snprintf(marker, sizeof marker, "%s/%s", destination, DM_PACK_MARKER) >=
        (int)sizeof marker) {
        set_error(why, why_n, "资源目录路径过长");
        rm_tree(destination);
        result = DM_PACK_ERR_PATH;
        goto done;
    }
    int marker_fd = open(marker, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (marker_fd < 0) {
        set_errno_error(why, why_n, "无法写入完成标记", marker);
        rm_tree(destination);
        result = DM_PACK_ERR_IO;
        goto done;
    }
    char contents[96];
    int n = snprintf(contents, sizeof contents, "files=%llu bytes=%llu\n",
                     (unsigned long long)ctx.total_entries,
                     (unsigned long long)ctx.total_bytes);
    int r = write_exact(marker_fd, contents, (size_t)n);
    if (!r && fsync(marker_fd)) r = -1;
    if (close(marker_fd) && !r) r = -1;
    if (r) {
        set_errno_error(why, why_n, "无法写入完成标记", marker);
        rm_tree(destination);
        result = DM_PACK_ERR_IO;
        goto done;
    }
done:
    path_set_free(&paths_seen);
    if (result != DM_PACK_OK && rm_tree(destination)) {
        char cleanup[256];
        snprintf(cleanup, sizeof cleanup, "；清理 staging 失败：%s",
                 strerror(errno));
        if (why && why_n) {
            size_t used = strlen(why);
            if (used < why_n) {
                snprintf(why + used, why_n - used, "%s", cleanup);
            }
        }
    }
    return result;
}

static int rm_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st)) return 0;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    DIR *dir = opendir(path);
    if (!dir) return -1;
    struct dirent *entry;
    int result = 0;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[PATH_MAX];
        if (snprintf(child, sizeof child, "%s/%s", path, entry->d_name) >=
                (int)sizeof child) {
            result = -1;
            break;
        }
        if (rm_tree(child)) {
            result = -1;
            break;
        }
    }
    closedir(dir);
    if (!result && rmdir(path)) result = -1;
    return result;
}

int dm_pack_publish(const char *staging, const char *destination,
                    char *why, size_t why_n) {
    char old[PATH_MAX];
    if (snprintf(old, sizeof old, "%s.dm-pack-old", destination) >=
        (int)sizeof old) {
        set_error(why, why_n, "资源目录路径过长");
        return DM_PACK_ERR_PATH;
    }
    if (rm_tree(old)) {
        set_errno_error(why, why_n, "无法清理旧目录", old);
        return DM_PACK_ERR_IO;
    }
    if (access(destination, F_OK) == 0 && rename(destination, old)) {
        set_errno_error(why, why_n, "无法移开旧目录", destination);
        return DM_PACK_ERR_IO;
    }
    if (rename(staging, destination)) {
        if (access(old, F_OK) == 0) rename(old, destination);
        set_errno_error(why, why_n, "无法发布资源目录", destination);
        return DM_PACK_ERR_IO;
    }
    if (rm_tree(old)) {
        // The new layer is already live.  Reporting failure here would make
        // the app retransmit and reimport the whole layer even though the
        // resource tree is valid.  The next publish retries this cleanup.
        set_error(why, why_n, "资源已发布，但旧目录未清理");
    }
    return DM_PACK_OK;
}

static int scan_complete_tree(const char *path, bool root,
                              uint64_t *file_count, uint64_t *byte_count,
                              char *why, size_t why_n) {
    struct stat st;
    if (lstat(path, &st)) {
        set_errno_error(why, why_n, "无法统计资源目录", path);
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        set_error(why, why_n, "资源目录不是目录");
        return 0;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        set_errno_error(why, why_n, "无法打开资源目录", path);
        return 0;
    }
    int result = 1;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno) {
                set_errno_error(why, why_n, "无法读取资源目录", path);
                result = 0;
            }
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }
        if (root && !strcmp(entry->d_name, DM_PACK_MARKER)) continue;

        char child[PATH_MAX];
        if (snprintf(child, sizeof child, "%s/%s", path, entry->d_name) >=
            (int)sizeof child) {
            set_error(why, why_n, "资源目录路径过长");
            result = 0;
            break;
        }
        if (lstat(child, &st)) {
            set_errno_error(why, why_n, "无法统计资源文件", child);
            result = 0;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!scan_complete_tree(child, false, file_count, byte_count,
                                    why, why_n)) {
                result = 0;
                break;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode) || st.st_size < 0) {
            set_error(why, why_n, "资源目录包含非普通文件");
            result = 0;
            break;
        }
        uint64_t size = (uint64_t)st.st_size;
        if (*file_count == UINT64_MAX || *byte_count > UINT64_MAX - size) {
            set_error(why, why_n, "资源目录统计溢出");
            result = 0;
            break;
        }
        (*file_count)++;
        *byte_count += size;
    }
    if (closedir(dir) && result) {
        set_errno_error(why, why_n, "无法关闭资源目录", path);
        result = 0;
    }
    return result;
}

int dm_pack_is_complete(const char *destination, char *why, size_t why_n) {
    char marker[PATH_MAX];
    if (snprintf(marker, sizeof marker, "%s/%s", destination, DM_PACK_MARKER) >=
        (int)sizeof marker) {
        set_error(why, why_n, "资源目录路径过长");
        return 0;
    }
    struct stat st;
    if (lstat(marker, &st) || !S_ISREG(st.st_mode) ||
        st.st_size <= 0 || st.st_size > 128) {
        set_error(why, why_n, "资源包尚未完成");
        return 0;
    }
    int fd = open(marker, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        set_errno_error(why, why_n, "无法读取完成标记", marker);
        return 0;
    }
    char contents[129];
    int n = (int)read(fd, contents, sizeof contents - 1);
    int read_error = n < 0 ? errno : 0;
    close(fd);
    if (n < 0) {
        errno = read_error;
        set_errno_error(why, why_n, "无法读取完成标记", marker);
        return 0;
    }
    contents[n] = '\0';
    unsigned long long files = 0;
    unsigned long long bytes = 0;
    int consumed = 0;
    if (sscanf(contents, "files=%llu bytes=%llu\n%n",
               &files, &bytes, &consumed) != 2 ||
        consumed != n || !files) {
        set_error(why, why_n, "资源包完成标记无效");
        return 0;
    }
    uint64_t actual_files = 0;
    uint64_t actual_bytes = 0;
    if (!scan_complete_tree(destination, true, &actual_files, &actual_bytes,
                             why, why_n)) {
        return 0;
    }
    if (actual_files != (uint64_t)files || actual_bytes != (uint64_t)bytes) {
        set_error(why, why_n, "资源目录与完成标记不一致");
        return 0;
    }
    set_error(why, why_n, "资源包已导入");
    return 1;
}
