// ACNH 运行时键契约：完整控制段声明长度与完整代码区共同决定键，越界长度必须被拒绝。
// ACNH runtime key contract: the declared full-control length and full code determine the key;
// out-of-range lengths must be rejected.
#include "../nvn/nvn_acnh_key.h"
#include <stdio.h>

static int failures, checks;

#define EXPECT(name, want, got) do { \
    long long w_ = (long long)(want), g_ = (long long)(got); checks++; \
    printf("%-56s 期望 %-10lld -> 真结果 %-10lld %s\n", (name), w_, g_, w_ == g_ ? "✓" : "✗"); \
    failures += w_ != g_; \
} while (0)

static void fill_header(uint8_t *c, uint32_t control_length, uint32_t code_length) {
    memcpy(c + 0x20, &control_length, sizeof control_length);
    memcpy(c + 0x704, &code_length, sizeof code_length);
}

typedef struct {
    const void *control;
    const uint8_t *code;
    size_t control_length, code_length;
} hash_args_t;
static hash_args_t hash_args;
static const uint8_t sha_stub_value[32] = {
    0x00, 0xec, 0x02, 0xc1, 0xad, 0x95, 0x95, 0x21,
    0x3d, 0x4c, 0xc2, 0x72, 0xa7, 0x5f, 0xe9, 0x0b,
    0x5d, 0x8b, 0x30, 0x29, 0xce, 0x11, 0x42, 0xd9,
    0x12, 0xe4, 0xca, 0x7b, 0xa1, 0xb6, 0xd8, 0x4f,
};
static void sha_stub(uint8_t out[32], const void *control, size_t control_length,
                     const uint8_t *code, size_t code_length) {
    hash_args = (hash_args_t){control, code, control_length, code_length};
    memcpy(out, sha_stub_value, sizeof sha_stub_value);
}

static void test_parse(void) {
    uint8_t control[0x2000] = {0}, code[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    nvn_acnh_key parsed;
    fill_header(control, 0xf68, 4);
    EXPECT("ACNH 扩展控制段 0xf68 + 代码 4 字节：解析成功", NVN_ACNH_KEY_OK,
           nvn_acnh_key_parse(control, code, sizeof control, 4, &parsed));
    EXPECT("扩展控制段长度", 0xf68, parsed.control_length);
    EXPECT("代码长度", 4, parsed.code_length);

    fill_header(control, 0x7CB, 4);
    EXPECT("控制段短于 0x7CC 被拒绝", NVN_ACNH_KEY_CONTROL_LENGTH,
           nvn_acnh_key_parse(control, code, sizeof control, 4, &parsed));
    fill_header(control, 0x2001, 4);
    EXPECT("控制段长于 0x2000 被拒绝", NVN_ACNH_KEY_CONTROL_LENGTH,
           nvn_acnh_key_parse(control, code, sizeof control, 4, &parsed));
    fill_header(control, 0x2000, 9);
    EXPECT("代码长度超出可用区被拒绝", NVN_ACNH_KEY_CODE_LENGTH,
           nvn_acnh_key_parse(control, code, sizeof control, 8, &parsed));
    fill_header(control, 0x880, 0);
    EXPECT("代码长度 0 被拒绝", NVN_ACNH_KEY_CODE_LENGTH,
           nvn_acnh_key_parse(control, code, sizeof control, 8, &parsed));
    fill_header(control, 0x880, 4);
    EXPECT("空控制段被拒绝", NVN_ACNH_KEY_NULL,
           nvn_acnh_key_parse(NULL, code, sizeof control, 4, &parsed));
    EXPECT("可用控制段不足 0x708 被拒绝", NVN_ACNH_KEY_NULL,
           nvn_acnh_key_parse(control, code, 0x707, 4, &parsed));
}

static void test_compute(void) {
    uint8_t control[0x2000] = {0}, code[8] = {1, 2, 3, 4, 5, 6, 7, 8}, key[8];
    nvn_acnh_key parsed;
    fill_header(control, 0xf68, 4);
    EXPECT("键契约：4 字节代码准备", NVN_ACNH_KEY_OK,
           nvn_acnh_key_parse(control, code, sizeof control, 4, &parsed));
    nvn_acnh_key_compute(&parsed, control, code, sha_stub, key);
    EXPECT("哈希输入：控制段长度", 0xf68, hash_args.control_length);
    EXPECT("哈希输入：代码长度", 4, hash_args.code_length);
    EXPECT("哈希输入：控制段指针", (long long)(uintptr_t)control, (long long)(uintptr_t)hash_args.control);
    EXPECT("哈希输入：代码指针", (long long)(uintptr_t)code, (long long)(uintptr_t)hash_args.code);
    EXPECT("键由摘要前 8 字节组成", 1, !memcmp(key, sha_stub_value, sizeof key));
}

int main(void) {
    test_parse();
    test_compute();

    printf("== %d 项，失败 %d ==\n", checks, failures);
    return failures != 0;
}
