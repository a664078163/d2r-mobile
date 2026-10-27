#ifndef DM_PACK_H
#define DM_PACK_H

#include <stddef.h>
#include <stdint.h>

/*
 * ACNH import pack.
 *
 * This is a sequential, one-way import container, not a RomFS filesystem and
 * not a replacement for the game's existing file service.  All integers are
 * little-endian.  A .dmp file begins with:
 *
 *   u8  magic[8]       "DMACNHPK"
 *   u32 version        1
 *   u32 flags          low 16 bits = pair_index (1-based), high 16 = pair_count
 *   u64 entry_count
 *   u64 payload_bytes  sum of stored payload sizes
 *
 * Each entry then has:
 *
 *   u16 path_len       relative UTF-8 path, '/' separators
 *   u8  method         0 = stored, 1 = raw DEFLATE
 *   u8  reserved
 *   u64 original_size
 *   u64 stored_size
 *   u8  sha256[32]     digest of the original bytes
 *   u8  path[path_len]
 *   u8  payload[stored_size]
 *
 * Pack header flags may carry a pair identity in the low 16 bits (1-based
 * pair_index) and high 16 bits (pair_count).  Both zero means a standalone
 * pack.  One import call must provide exactly every unique index from 1
 * through pair_count; the layer publishes only after all of them verify.
 * Files in the same layer must be one complete set, not a mix of different
 * build generations.  Every entry path must be unique.
 *
 * The device extracts all packs into a staging directory, verifies every path,
 * size and digest, then publishes the complete tree.  A source tree is never
 * partially exposed to the game.
 *
 * Pack size and entry size are different limits.  pack_size is a transfer and
 * retry unit chosen by the builder (currently about 1 GiB).  original_size is
 * capped at DM_PACK_MAX_ENTRY_BYTES (64 MiB) so one malformed entry cannot
 * request an unbounded device allocation.  Paths are unique case-insensitively because
 * Apple filesystems may be case-insensitive.
 */

#define DM_PACK_MAGIC "DMACNHPK"
#define DM_PACK_VERSION 1u
#define DM_PACK_HEADER_SIZE 32u
#define DM_PACK_ENTRY_HEADER_SIZE 52u
#define DM_PACK_METHOD_STORE 0u
#define DM_PACK_METHOD_DEFLATE 1u
#define DM_PACK_MAX_PATH 512u
#define DM_PACK_DIGEST_SIZE 32u
#define DM_PACK_MAX_ENTRY_BYTES (64u << 20)
#define DM_PACK_MARKER ".dm-pack-complete"

enum {
    DM_PACK_OK = 0,
    DM_PACK_ERR_IO = 1,
    DM_PACK_ERR_FORMAT = 2,
    DM_PACK_ERR_DIGEST = 3,
    DM_PACK_ERR_SIZE = 4,
    DM_PACK_ERR_PATH = 5,
    DM_PACK_ERR_MEMORY = 6,
    DM_PACK_ERR_CANCELLED = 7,
    DM_PACK_ERR_DUPLICATE = 8
};

typedef struct {
    uint64_t pack_size;
    uint64_t entry_count;
    uint64_t original_bytes;
    uint64_t stored_bytes;
    uint16_t pair_index;
    uint16_t pair_count;
} dm_pack_info;

typedef int (*dm_pack_progress_fn)(uint64_t done_bytes, uint64_t total_bytes,
                                   uint64_t done_entries, uint64_t total_entries,
                                   const char *current_path, void *user);

typedef int (*dm_pack_cancel_fn)(void *user);

int dm_pack_inspect(const char *path, dm_pack_info *out, char *why, size_t why_n);
int dm_pack_extract(const char *const *paths, size_t path_count,
                    const char *destination,
                    dm_pack_progress_fn progress, dm_pack_cancel_fn cancelled,
                    void *user, char *why, size_t why_n);
int dm_pack_publish(const char *staging, const char *destination,
                    char *why, size_t why_n);
int dm_pack_is_complete(const char *destination, char *why, size_t why_n);

#endif
