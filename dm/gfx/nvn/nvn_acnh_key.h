// ACNH 运行时着色器键：sha256(完整控制段 + 完整代码区) 的前 8 字节。
// 离线提取器 dm/tools/acnh_shader.py 与宿主共用这一契约；D2R 仍只按控制段 0x880 字节计算键。
// ACNH runtime shader key: first 8 bytes of sha256(full control section + full code).
// The offline extractor dm/tools/acnh_shader.py and the host share this contract; D2R still hashes only the 0x880-byte control section.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NVN_ACNH_CONTROL_SIZE 0x2000   // ACNH 实际控制段可超过公开 NVN 0x880；运行时观测上限约 0xf68 | ACNH control data exceeds public NVN 0x880; observed maximum is about 0xf68
#define NVN_ACNH_CONTROL_MIN  0x7CC
#define NVN_ACNH_KEY_SIZE     8

typedef enum {
    NVN_ACNH_KEY_OK = 0,
    NVN_ACNH_KEY_NULL,
    NVN_ACNH_KEY_CONTROL_LENGTH,
    NVN_ACNH_KEY_CODE_LENGTH,
} nvn_acnh_key_status;

typedef struct {
    uint8_t bytes[NVN_ACNH_KEY_SIZE];
    uint32_t control_length;
    uint32_t code_length;
} nvn_acnh_key;

// 只解析键契约需要的两个长度字段。调用者负责保证控制段至少可读到 0x708 字节，
// 与 NVN 的 0x880 字节控制段一致；非 ACNH 布局由长度状态明确拒绝。
// Parse only the two length fields required by the key contract. The caller guarantees the control
// section is readable through 0x708, matching NVN's 0x880-byte control section; other layouts are
// rejected explicitly by the status.
nvn_acnh_key_status nvn_acnh_key_parse(const void *control, const uint8_t *code,
                                      size_t control_available, size_t code_available,
                                      nvn_acnh_key *out);

// 调用者提供 SHA-256 实现，避免把 CommonCrypto 依赖带进纯 C 单测。
// The caller supplies SHA-256 to keep CommonCrypto out of the pure C unit tests.
typedef void (*nvn_acnh_sha256_fn)(uint8_t out[32], const void *control, size_t control_length,
                                  const uint8_t *code, size_t code_length);

void nvn_acnh_key_compute(const nvn_acnh_key *parsed, const void *control, const uint8_t *code,
                          nvn_acnh_sha256_fn sha256, uint8_t out[NVN_ACNH_KEY_SIZE]);
