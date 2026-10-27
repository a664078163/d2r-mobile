#include "nvn_acnh_key.h"

nvn_acnh_key_status nvn_acnh_key_parse(const void *control, const uint8_t *code,
                                      size_t control_available, size_t code_available,
                                      nvn_acnh_key *out) {
    if (!control || !code || !out || control_available < 0x708) return NVN_ACNH_KEY_NULL;
    const uint8_t *c = (const uint8_t *)control;
    uint32_t control_length, code_length;
    memcpy(&control_length, c + 0x20, sizeof control_length);
    if (control_length < NVN_ACNH_CONTROL_MIN || control_length > NVN_ACNH_CONTROL_SIZE ||
        control_length > control_available) return NVN_ACNH_KEY_CONTROL_LENGTH;
    memcpy(&code_length, c + 0x704, sizeof code_length);
    if (!code_length || code_length > code_available) return NVN_ACNH_KEY_CODE_LENGTH;
    memset(out, 0, sizeof *out);
    out->control_length = control_length;
    out->code_length = code_length;
    return NVN_ACNH_KEY_OK;
}

void nvn_acnh_key_compute(const nvn_acnh_key *parsed, const void *control, const uint8_t *code,
                          nvn_acnh_sha256_fn sha256, uint8_t out[NVN_ACNH_KEY_SIZE]) {
    uint8_t digest[32];
    sha256(digest, control, parsed->control_length, code, parsed->code_length);
    memcpy(out, digest, NVN_ACNH_KEY_SIZE);   // 摘要字节直接对应 hexdigest() 前 16 位 | Digest bytes are the leading 16 hex digits
}
