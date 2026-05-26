#include "unity.h"
#include "../internal_include/sprout/internal/helpers.h"
#include <string.h>

void setUp(void) {
}

void tearDown(void) {
}

void test_safe_string_copy_normal(void) {
    char dest[20];
    const char *src = "Hello World";
    _safe_string_copy(dest, src, sizeof(dest));
    TEST_ASSERT_EQUAL_STRING(src, dest);
}

void test_safe_string_copy_truncate(void) {
    char dest[5];
    const char *src = "Hello World";
    _safe_string_copy(dest, src, sizeof(dest));
    TEST_ASSERT_EQUAL_STRING("Hell", dest);
    TEST_ASSERT_EQUAL_UINT8('\0', dest[4]);
}

void test_safe_string_copy_null_dest(void) {
    const char *src = "Hello";
    _safe_string_copy(NULL, src, 10);
    // Should not crash
}

void test_safe_string_copy_null_src(void) {
    char dest[10];
    _safe_string_copy(dest, NULL, sizeof(dest));
    // Should not crash
}

void test_safe_string_copy_zero_size(void) {
    char dest[10];
    const char *src = "Hello";
    _safe_string_copy(dest, src, 0);
    // Should not crash
}

void test_is_broadcast_address_true(void) {
    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_TRUE(is_broadcast_address(broadcast));
}

void test_is_broadcast_address_false(void) {
    uint8_t not_broadcast[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    TEST_ASSERT_FALSE(is_broadcast_address(not_broadcast));
}

void test_is_broadcast_address_partial(void) {
    uint8_t partial[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    TEST_ASSERT_FALSE(is_broadcast_address(partial));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_safe_string_copy_normal);
    RUN_TEST(test_safe_string_copy_truncate);
    RUN_TEST(test_safe_string_copy_null_dest);
    RUN_TEST(test_safe_string_copy_null_src);
    RUN_TEST(test_safe_string_copy_zero_size);
    RUN_TEST(test_is_broadcast_address_true);
    RUN_TEST(test_is_broadcast_address_false);
    RUN_TEST(test_is_broadcast_address_partial);
    return UNITY_END();
}
