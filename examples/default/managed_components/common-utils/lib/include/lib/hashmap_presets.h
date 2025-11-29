#pragma once

#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

unsigned int string_hash(const void *key);
bool string_equals(const void *a, const void *b);
unsigned int int_hash(const void *key);
bool int_equals(const void *a, const void *b);
unsigned int mac_hash(const void *key);
bool mac_equals(const void *a, const void *b);
unsigned int ptr_hash(const void *key);
bool ptr_equals(const void *a, const void *b);

#ifdef __cplusplus
}
#endif