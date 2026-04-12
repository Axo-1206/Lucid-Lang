/**
 * @file luc_runtime.c
 *
 * @responsibility Luc JIT runtime — heap management, dynamic arrays, string helpers, io.
 *
 * @usecase Compiled as a plain C translation unit and linked into every JIT module.
 *   All symbols are declared on the Luc side via `extern let` and resolved by the
 *   LLVM ORC JIT's dynamic linker at startup.
 *
 * @abi
 *   LucString  = { i8* ptr, int64 len }         (matches PrimitiveTypeAST(String))
 *   LucArray   = { T* ptr, int64 len, int64 cap } (matches SliceTypeAST / DynamicArrayTypeAST)
 *
 *   Every function that operates on a LucArray receives a void* pointing to the
 *   array struct itself — not to the element buffer. This lets the runtime update
 *   ptr, len, and cap in-place when the buffer is reallocated.
 *
 * @notes
 *   - All allocations go through luc_alloc / luc_free so a future custom allocator
 *     or GC can be dropped in by replacing just those two functions.
 *   - String values are heap-allocated and null-terminated for C interop, even
 *     though the length is stored explicitly.
 *   - This file has no dependencies beyond the C standard library.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// ─────────────────────────────────────────────────────────────────────────────
// ABI types
//
// These structs must match the LLVM struct layouts emitted by CodeGenType.cpp.
// Field order and sizes are load-bearing — do not reorder.
// ─────────────────────────────────────────────────────────────────────────────

typedef struct {
    char*   ptr;   // heap-allocated byte buffer (null-terminated for C interop)
    int64_t len;   // byte length, excluding the null terminator
} LucString;

typedef struct {
    void*   ptr;   // heap-allocated element buffer
    int64_t len;   // number of elements currently stored
    int64_t cap;   // number of elements the buffer can hold before reallocation
} LucArray;


// ─────────────────────────────────────────────────────────────────────────────
// Memory
// ─────────────────────────────────────────────────────────────────────────────

void* luc_alloc(uint64_t bytes) {
    void* p = malloc((size_t)bytes);
    if (!p) {
        fprintf(stderr, "luc runtime: out of memory (requested %llu bytes)\n",
                (unsigned long long)bytes);
        abort();
    }
    return p;
}

void luc_free(void* ptr) {
    free(ptr);
}


// ─────────────────────────────────────────────────────────────────────────────
// Dynamic array operations
//
// All functions take a void* to the LucArray struct (not to the element buffer).
// elemSize is the size of one element in bytes, used for pointer arithmetic.
// ─────────────────────────────────────────────────────────────────────────────

// Grow the internal buffer to at least `min_cap` elements.
static void luc_array_grow(LucArray* arr, int64_t min_cap, uint64_t elem_size) {
    int64_t new_cap = arr->cap == 0 ? 8 : arr->cap * 2;
    if (new_cap < min_cap) new_cap = min_cap;

    void* new_buf = luc_alloc((uint64_t)(new_cap) * elem_size);
    if (arr->ptr && arr->len > 0) {
        memcpy(new_buf, arr->ptr, (size_t)(arr->len) * elem_size);
    }
    luc_free(arr->ptr);
    arr->ptr = new_buf;
    arr->cap = new_cap;
}

// .push(value T) — append one element to the end.
void luc_array_push(void* arr_ptr, const void* elem, uint64_t elem_size) {
    LucArray* arr = (LucArray*)arr_ptr;
    if (arr->len >= arr->cap) {
        luc_array_grow(arr, arr->len + 1, elem_size);
    }
    char* dest = (char*)arr->ptr + (size_t)(arr->len) * elem_size;
    memcpy(dest, elem, elem_size);
    arr->len++;
}

// .pop() — remove the last element and return a pointer to a copy of it.
// The caller owns the returned memory. Returns NULL if the array is empty
// (out-of-bounds access on an empty dynamic array yields nil per the spec).
void* luc_array_pop(void* arr_ptr, uint64_t elem_size) {
    LucArray* arr = (LucArray*)arr_ptr;
    if (arr->len <= 0) return NULL;   // nil — caller checks nullable return
    arr->len--;
    void* copy = luc_alloc(elem_size);
    memcpy(copy, (char*)arr->ptr + (size_t)(arr->len) * elem_size, elem_size);
    return copy;
}

// .insert(i int, v T) — insert one element at index i, shifting elements right.
void luc_array_insert(void* arr_ptr, int64_t idx, const void* elem, uint64_t elem_size) {
    LucArray* arr = (LucArray*)arr_ptr;
    if (arr->len >= arr->cap) {
        luc_array_grow(arr, arr->len + 1, elem_size);
    }
    // Shift elements [idx, len) one position to the right.
    char* base  = (char*)arr->ptr;
    size_t move = (size_t)(arr->len - idx) * elem_size;
    if (move > 0) {
        memmove(base + (size_t)(idx + 1) * elem_size,
                base + (size_t)(idx)     * elem_size,
                move);
    }
    memcpy(base + (size_t)(idx) * elem_size, elem, elem_size);
    arr->len++;
}

// .remove(i int) — remove the element at index i, shifting elements left.
// Returns a pointer to a copy of the removed element. Returns NULL on OOB.
void* luc_array_remove(void* arr_ptr, int64_t idx, uint64_t elem_size) {
    LucArray* arr = (LucArray*)arr_ptr;
    if (idx < 0 || idx >= arr->len) return NULL;
    void* copy   = luc_alloc(elem_size);
    char* base   = (char*)arr->ptr;
    memcpy(copy, base + (size_t)(idx) * elem_size, elem_size);
    size_t move  = (size_t)(arr->len - idx - 1) * elem_size;
    if (move > 0) {
        memmove(base + (size_t)(idx)     * elem_size,
                base + (size_t)(idx + 1) * elem_size,
                move);
    }
    arr->len--;
    return copy;
}

// .clear() — remove all elements, keep the allocated buffer.
void luc_array_clear(void* arr_ptr) {
    LucArray* arr = (LucArray*)arr_ptr;
    arr->len = 0;
    // cap and ptr are intentionally preserved — .reserve() capacity is not lost.
}

// .reserve(n int) — ensure at least n elements can be stored without reallocation.
void luc_array_reserve(void* arr_ptr, int64_t n, uint64_t elem_size) {
    LucArray* arr = (LucArray*)arr_ptr;
    if (n > arr->cap) {
        luc_array_grow(arr, n, elem_size);
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// String helpers
//
// All functions return heap-allocated LucString values.
// The caller is responsible for eventually calling luc_free(result.ptr).
// In practice, the Luc runtime does not yet implement automatic memory management
// for strings — this is noted as a future GC integration point.
// ─────────────────────────────────────────────────────────────────────────────

// Concatenate two LucString values — returns a new heap-allocated LucString.
LucString luc_string_concat(LucString a, LucString b) {
    int64_t new_len = a.len + b.len;
    char*   buf     = (char*)luc_alloc((uint64_t)(new_len) + 1);
    memcpy(buf,           a.ptr, (size_t)a.len);
    memcpy(buf + a.len,   b.ptr, (size_t)b.len);
    buf[new_len] = '\0';
    LucString result = { buf, new_len };
    return result;
}

// Return the byte length of a LucString (mirrors .len() on strings).
int64_t luc_string_len(LucString s) {
    return s.len;
}

// Compare two LucString values — returns 0 if equal, non-zero otherwise.
int32_t luc_string_eq(LucString a, LucString b) {
    if (a.len != b.len) return 0;
    return (memcmp(a.ptr, b.ptr, (size_t)a.len) == 0) ? 1 : 0;
}

// Convert an int64 to a LucString.
LucString luc_int_to_string(int64_t n) {
    // Longest int64 decimal representation is 20 chars + sign + null.
    char buf[22];
    int  written = snprintf(buf, sizeof(buf), "%lld", (long long)n);
    char* heap   = (char*)luc_alloc((uint64_t)(written) + 1);
    memcpy(heap, buf, (size_t)(written) + 1);
    LucString result = { heap, (int64_t)written };
    return result;
}

// Convert a float to a LucString.
LucString luc_float_to_string(float f) {
    char buf[32];
    int  written = snprintf(buf, sizeof(buf), "%g", (double)f);
    char* heap   = (char*)luc_alloc((uint64_t)(written) + 1);
    memcpy(heap, buf, (size_t)(written) + 1);
    LucString result = { heap, (int64_t)written };
    return result;
}

// Convert a double to a LucString.
LucString luc_double_to_string(double d) {
    char buf[32];
    int  written = snprintf(buf, sizeof(buf), "%g", d);
    char* heap   = (char*)luc_alloc((uint64_t)(written) + 1);
    memcpy(heap, buf, (size_t)(written) + 1);
    LucString result = { heap, (int64_t)written };
    return result;
}

// Convert a bool (i1) to a LucString ("true" or "false").
LucString luc_bool_to_string(int8_t b) {
    const char* src = b ? "true" : "false";
    int64_t     len = (int64_t)strlen(src);
    char*       heap = (char*)luc_alloc((uint64_t)(len) + 1);
    memcpy(heap, src, (size_t)(len) + 1);
    LucString result = { heap, len };
    return result;
}

// Convert a char (i32 Unicode codepoint) to a LucString.
LucString luc_char_to_string(int32_t codepoint) {
    // UTF-8 encode the codepoint into up to 4 bytes.
    char buf[5];
    int  len = 0;
    if (codepoint < 0x80) {
        buf[len++] = (char)codepoint;
    } else if (codepoint < 0x800) {
        buf[len++] = (char)(0xC0 | (codepoint >> 6));
        buf[len++] = (char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        buf[len++] = (char)(0xE0 | (codepoint >> 12));
        buf[len++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        buf[len++] = (char)(0x80 | (codepoint & 0x3F));
    } else {
        buf[len++] = (char)(0xF0 | (codepoint >> 18));
        buf[len++] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        buf[len++] = (char)(0x80 | ((codepoint >> 6)  & 0x3F));
        buf[len++] = (char)(0x80 | (codepoint & 0x3F));
    }
    buf[len] = '\0';
    char* heap = (char*)luc_alloc((uint64_t)(len) + 1);
    memcpy(heap, buf, (size_t)(len) + 1);
    LucString result = { heap, (int64_t)len };
    return result;
}

// Wrap a C string literal as a LucString (no heap allocation — ptr points into
// read-only segment). Only safe for string literals baked into the binary.
LucString luc_string_from_literal(const char* cstr, int64_t len) {
    LucString result = { (char*)cstr, len };
    return result;
}


// ─────────────────────────────────────────────────────────────────────────────
// io — basic output
//
// These back the `io.printl` and `io.print` standard library functions.
// A full io module (file I/O, input, event binding) will call into the OS
// directly from Luc-level `extern let` declarations; these helpers cover the
// common print use cases needed to run early programs.
// ─────────────────────────────────────────────────────────────────────────────

// io.printl(s string) — print a LucString followed by a newline.
void luc_printl(LucString s) {
    fwrite(s.ptr, 1, (size_t)s.len, stdout);
    fputc('\n', stdout);
}

// io.print(s string) — print a LucString without a trailing newline.
void luc_print(LucString s) {
    fwrite(s.ptr, 1, (size_t)s.len, stdout);
}

// io.printl_int(n int) — print an integer followed by a newline.
// Convenience used by the codegen for early debugging before string conversion
// is fully wired.
void luc_printl_int(int64_t n) {
    printf("%lld\n", (long long)n);
}

// io.printl_float(f float) — print a float followed by a newline.
void luc_printl_float(float f) {
    printf("%g\n", (double)f);
}


// ─────────────────────────────────────────────────────────────────────────────
// Panic
//
// Called by the JIT when an unhandled Expect<T> result is discarded (per the
// error library spec: "the thread will block immediately with a Panic") or when
// an out-of-bounds access is detected on a fixed/slice array.
// ─────────────────────────────────────────────────────────────────────────────

void luc_panic(LucString message) {
    fprintf(stderr, "luc panic: ");
    fwrite(message.ptr, 1, (size_t)message.len, stderr);
    fputc('\n', stderr);
    abort();
}

void luc_panic_oob(int64_t index, int64_t length) {
    fprintf(stderr, "luc panic: index out of bounds (index %lld, length %lld)\n",
            (long long)index, (long long)length);
    abort();
}