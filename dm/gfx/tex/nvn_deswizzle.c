// Tegra X1 块线性排布（公开描述见 deko3d / Ryujinx BlockLinearLayout）：
//   GOB = 64 字节 × 8 行 = 512 字节；GOB 内 (x 字节, y 行) 的偏移 =
//     (x%64/32)·256 + (y%8/2)·64 + (x%32/16)·32 + (y%2)·16 + x%16
//   块 = 1 个 GOB 宽 × 2^tileH 个 GOB 高 × 2^tileD 个 GOB 深，块内先排 y 再排 z；块按 x → y → z 行优先铺满本级。
// 级 / 层起点与本级实际块高块深取自 nvn_tex.c，这里只做级内寻址与搬运。
// Tegra X1 block-linear layout (public descriptions: deko3d / Ryujinx BlockLinearLayout):
//   GOB = 64 bytes × 8 rows = 512 bytes; offset of (x byte, y row) within a GOB =
//     (x%64/32)·256 + (y%8/2)·64 + (x%32/16)·32 + (y%2)·16 + x%16
//   block = 1 GOB wide × 2^tileH GOBs high × 2^tileD GOBs deep, ordered y first then z within a block; blocks fill the level in x → y → z row-major order.
// Level / layer start offsets and the actual block height and block depth of this level come from nvn_tex.c; here we only do intra-level addressing and copying.
#include "nvn_deswizzle.h"
#include <string.h>

static uint32_t gob_off(uint32_t x, uint32_t y) {
    return (x % 64 / 32) * 256 + (y % 8 / 2) * 64 + (x % 32 / 16) * 32 + (y % 2) * 16 + x % 16;
}

uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_bytes, uint32_t rows, unsigned th, unsigned td) {
    uint32_t bh = 1u << th, bd = 1u << td;
    uint64_t gobs_x = (row_bytes + 63) / 64, blocks_y = (rows + 8 * bh - 1) / (8 * bh);
    uint64_t block = 512ull * bh * bd;                                  // 一个块的字节 | bytes in one block
    return (z / bd) * gobs_x * blocks_y * block                        // 第几层块（深度方向） | which block layer (along depth)
         + (y / (8 * bh)) * gobs_x * block                             // 第几行块 | which block row
         + (x / 64) * block                                            // 第几列块 | which block column
         + ((z % bd) * bh + (y % (8 * bh)) / 8) * 512ull               // 块内第几个 GOB | which GOB within the block
         + gob_off(x, y);
}

static uint32_t mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

// 本级几何：块宽字节数、块行数、切片数，以及本级本层在存储里的起点；不支持的贴图返回 -1
// Geometry of this level: width-in-blocks byte count, block row count, slice count, and where this level and layer starts in storage; returns -1 for unsupported textures
int nvn_deswizzle_geom(const nvn_texdesc *d, unsigned level, unsigned layer,
                       uint32_t *row_bytes, uint32_t *rows, uint32_t *slices, uint64_t *base, unsigned *th, unsigned *td, uint32_t *bpb) {
    if (d->target == 5 || d->target == 6 || d->target == 10 || d->samples > 1) return -1;   // MS / BUFFER
    int bw, bh; *bpb = (uint32_t)nvn_fmt_bpb(d->format, &bw, &bh);
    if (!*bpb || level >= (d->levels ? d->levels : 1)) return -1;
    nvn_texlayout L; nvn_tex_layout(d, &L);
    if (layer >= L.layers) return -1;
    int one_d = d->target == 0 || d->target == 3;
    uint32_t w = mip(d->width, level), h = one_d ? 1 : mip(d->height, level);
    *slices = d->target == 2 ? mip(d->depth ? d->depth : 1, level) : 1;
    *row_bytes = (w + bw - 1) / bw * *bpb; *rows = (h + bh - 1) / bh;
    *base = (uint64_t)layer * L.layer_size + nvn_tex_level_offset(d, level);
    nvn_tex_level_tiles(d, level, th, td);
    return 0;
}

static int move(const nvn_texdesc *d, unsigned level, unsigned layer, uint8_t *bl, uint8_t *lin, size_t pitch, int to_linear) {
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return -1;
    if (pitch < row_bytes) return -1;
    bl += base;
    if (d->flags & 0x10) {
        size_t storage_pitch = nvn_tex_level_stride(d, level), storage_slice = storage_pitch * rows;
        for (uint32_t z = 0; z < slices; z++) for (uint32_t y = 0; y < rows; y++)
            memcpy(lin + ((size_t)z * rows + y) * pitch, bl + (size_t)z * storage_slice + (size_t)y * storage_pitch, row_bytes);
        return 0;
    }
    for (uint32_t z = 0; z < slices; z++)
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t *row = lin + ((size_t)z * rows + y) * pitch;
            for (uint32_t x = 0; x < row_bytes; x += 16) {             // 16 字节一段：GOB 内 16 字节连续，且整除所有块宽 | 16-byte segments: 16 bytes are contiguous within a GOB, and 16 divides all block widths
                uint32_t n = row_bytes - x < 16 ? row_bytes - x : 16;
                uint8_t *p = bl + nvn_bl_offset(x, y, z, row_bytes, rows, th, td);
                if (to_linear) memcpy(row + x, p, n); else memcpy(p, row + x, n);
            }
        }
    return 0;
}

int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, void *dst, size_t dst_pitch) {
    return move(d, level, layer, (uint8_t *)src, (uint8_t *)dst, dst_pitch, 1);
}
int nvn_swizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, size_t src_pitch, void *dst) {
    return move(d, level, layer, (uint8_t *)dst, (uint8_t *)src, src_pitch, 0);
}

int nvn_write_texels(const nvn_texdesc *d, unsigned level, unsigned layer,
                     const NVNcopyRegion *r, const void *src,
                     size_t row_stride, size_t image_stride, void *dst_storage) {
    if (!d || !r || !src || !dst_storage || r->xoffset < 0 || r->yoffset < 0 || r->zoffset < 0 ||
        r->width <= 0 || r->height <= 0 || r->depth <= 0) return -1;
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh); if (!bpb) return -1;
    uint32_t full_rb, full_rows, full_slices, full_bpb; uint64_t unused; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, d->target == 2 ? 0 : layer, &full_rb, &full_rows, &full_slices, &unused, &th, &td, &full_bpb)) return -1;
    if (r->xoffset % bw || r->yoffset % bh || (r->width % bw && r->xoffset + r->width != (int)(full_rb / bpb * bw)) ||
        (r->height % bh && r->yoffset + r->height != (int)(full_rows * bh))) return -1;
    uint32_t rw = ((uint32_t)r->width + bw - 1) / bw, rh = ((uint32_t)r->height + bh - 1) / bh;
    size_t packed = (size_t)rw * bpb, row = row_stride ? row_stride : packed;
    size_t image = image_stride ? image_stride : row * rh;
    if (row < packed || image < row * rh) return -1;
    int is3d = d->target == 2; uint32_t nz = (uint32_t)r->depth;
    nvn_texlayout L; nvn_tex_layout(d, &L);
    if (!is3d && (uint32_t)layer + (uint32_t)r->zoffset + nz > L.layers) return -1;
    for (uint32_t zi = 0; zi < nz; zi++) {
        unsigned dst_layer = is3d ? 0 : layer + (unsigned)r->zoffset + zi;
        uint32_t dst_z = is3d ? (uint32_t)r->zoffset + zi : 0;
        uint64_t base; unsigned lth, ltd; uint32_t rb, rows, slices, lbpb;
        if (nvn_deswizzle_geom(d, level, dst_layer, &rb, &rows, &slices, &base, &lth, &ltd, &lbpb)) return -1;
        uint8_t *dst = (uint8_t *)dst_storage + base;
        const uint8_t *sp = (const uint8_t *)src + (size_t)zi * image;
        for (uint32_t y = 0; y < rh; y++) {
            uint32_t sy = (uint32_t)r->yoffset / bh + y;
            const uint8_t *srow = sp + (size_t)y * row;
            if (d->flags & 0x10) {
                size_t pitch = nvn_tex_level_stride(d, level);
                memcpy(dst + (size_t)sy * pitch + (size_t)(r->xoffset / bw) * bpb, srow, packed);
            } else {
                uint32_t x0 = (uint32_t)r->xoffset / bw * bpb;
                for (uint32_t x = 0; x < packed;) {
                    uint32_t xb = x0 + x; size_t n = 16 - (xb & 15); if (n > packed - x) n = packed - x;
                    memcpy(dst + nvn_bl_offset(xb, sy, dst_z, rb, rows, lth, ltd), srow + x, n); x += (uint32_t)n;
                }
            }
        }
    }
    return 0;
}
