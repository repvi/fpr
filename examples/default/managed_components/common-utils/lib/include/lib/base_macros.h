#ifndef BASE_MACROS_H
#define BASE_MACROS_H

#include <assert.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define CLAMP(x, min, max) (MIN(MAX(x, min), max))

#define LESS_THAN(a, b) MIN((a), (b))
#define GREATER_THAN(a, b) MAX((a), (b))

#define SWAP(a, b) do { typeof(a) temp = (a); (a) = (b); (b) = temp; } while (0)

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define UNUSED(x) (void)(x)
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define CONCAT(a, b) a##b

#define FOR_LOOP(i, start, end) for (int i = (start); i < (end); ++i)
#define FOR_LOOP_REVERSE(i, start, end) for (int i = (start)-1; i >= (end); --i)

#define FOR_LOOP_PTR(ptr, start, end) for (typeof(start) ptr = (start); ptr != (end); ++ptr)
#define FOR_LOOP_REVERSE_PTR(ptr, start, end) for (typeof(start) ptr = (end); ptr != (start); --ptr)

#ifdef __GNUC__
#define ALIGNED(x) __attribute__((aligned(x)))
#define ALIGNED2 ALIGNED(2)
#define ALIGNED4 ALIGNED(4)
#define ALIGNED8 ALIGNED(8)
#define ALIGNED16 ALIGNED(16)
#define ALIGNED32 ALIGNED(32)
#define ALIGNED64 ALIGNED(64)

// Ensure structure is packed without padding, useful for network protocols or file formats, but not for general use
#define PACKED __attribute__((packed))

#define DEPRECATED __attribute__((deprecated))
#define NORETURN __attribute__((noreturn))
// #define UNUSED __attribute__((unused))
#define WEAK __attribute__((weak))
#define FORCED_INLINE inline __attribute__((always_inline))
#define FORCED_NOINLINE __attribute__((noinline))
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define PURE __attribute__((pure))
#define CONST __attribute__((const))
#define HOT __attribute__((hot))
#define COLD __attribute__((cold))
#define SECTION(name) __attribute__((section(name)))
#define CONSTRUCTOR __attribute__((constructor))
#define CONSTRUCTOR_PRIORITY(prio) __attribute__((constructor(prio)))
#define DESTRUCTOR __attribute__((destructor))
#define DESTRUCTOR_PRIORITY(prio) __attribute__((destructor(prio)))
#define WARN_UNUSED_RESULT __attribute__((warn_unused_result))
#define MALLOC_LIKE __attribute__((malloc))
#define RETURNS_NONNULL __attribute__((returns_nonnull))
#define NONULL(...) __attribute__((nonnull(__VA_ARGS__)))
#define NONULL_ALL __attribute__((nonnull))

#define FORMAT_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#define FORMAT_SCANF(fmt_idx, arg_idx) __attribute__((format(scanf, fmt_idx, arg_idx)))

/* Optimize function by inlining and flattening calls */
#define FLATTERN __attribute__((flatten))

/* Disable optimizations for a function */
#define OPTIMIZE(level) __attribute__((optimize(level)))
#define NO_OPTIMIZE OPTIMIZE("O0")

#define ALIAS(name) __attribute__((alias(#name)))
#define WEAK_ALIAS(name) __attribute__((weak, alias(#name)))

// recommended to not use
#define CLEANUP(func) __attribute__((cleanup(func)))

#define LEADING_ZEROES_32(x) __builtin_clz(x)
#define TRAILING_ZEROES_32(x) __builtin_ctz(x)
#define POPCOUNT_32(x) __builtin_popcount(x)

#define BITSWAP_16(x) __builtin_bswap16(x)
#define BITSWAP_32(x) __builtin_bswap32(x)

#define ASSERT_EQUAL(var1, var2) _Static_assert((var1) == (var2), "Values must be equal")
#define ASSERT_NOT_EQUAL(var1, var2) _Static_assert((var1) != (var2), "Values must not be equal")
#define ASSERT_LESS_THAN(var1, var2) _Static_assert((var1) < (var2), "var1 must be less than var2")
#define ASSERT_GREATER_THAN(var1, var2) _Static_assert((var1) > (var2), "var1 must be greater than var2")
#define ASSERT_LESS_EQUAL(var1, var2) _Static_assert((var1) <= (var2), "var1 must be less than or equal to var2")
#define ASSERT_GREATER_EQUAL(var1, var2) _Static_assert((var1) >= (var2), "var1 must be greater than or equal to var2")

#define ASSERT_TYPE(var1, var2) _Static_assert(__builtin_types_compatible_p(typeof(var1), typeof(var2)), "Types must match")
#define ASSERT_TYPE_NOT_ALLOW(var1, type) _Static_assert(!__builtin_types_compatible_p(typeof(var1), (type)), STRINGIFY(typeof(var1)##" must not be " STRINGIFY(type)))
#define ASSERT_SIZE(var, size) _Static_assert(sizeof(var) == (size), "Size must match")
#define ASSERT_SIZE_AT_LEAST(var, size) _Static_assert(sizeof(var) >= (size), "Size must be at least " STRINGIFY(size))
#define ASSERT_SIZE_AT_MOST(var, size) _Static_assert(sizeof(var) <= (size), "Size must be at most " STRINGIFY(size))
#define ASSERT_IS_POINTER(var) _Static_assert(__builtin_types_compatible_p(typeof(*var), (typeof(var))), "Must be a pointer type")
#define ASSERT_IS_NOT_POINTER(var) _Static_assert(!__builtin_types_compatible_p(typeof(*var), (typeof(var))), "Must not be a pointer type")
#define ASSERT_CONST(expr) _Static_assert(__builtin_constant_p(expr), "Expression must be constant")
#define ASSERT_ARRAY_TYPE(arr, type) _Static_assert(__builtin_types_compatible_p(typeof((arr)[0]), type), "Array element type mismatch")

#define ASSERT_EXPECTED(expr, expected) assert((expr) == (expected))

// Alloca is not safe in C++ with non-trivial types, use with caution, uses stack memory
#define ALLOCA(size) __builtin_alloca(size)

#define NO_OP() __asm__ volatile("nop")

#define RETURN_ON_FALSE_VOID(cond) \
    do { \
        if (__builtin_expect(!!(cond), 0)) { \
            return; \
        } \
    } while (0)

#define RETURN_ON_FALSE_TYPE(cond, ec) \
    do { \
        if (__builtin_expect(!!(cond), 0)) { \
            return (ec); \
        } \
    } while (0)

#else
#error "GCC compatible compiler required"
#endif

#endif // BASE_MACROS_H