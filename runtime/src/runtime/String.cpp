/**
 * @file runtime/String.cpp
 *
 * @responsibility The refcounted string implementation.
 *
 * ─── Design: strings share a static empty buffer ──────────────────────────
 * The empty string is common (it is the default value of a string
 * cell before it is written, and it is a natural result of many
 * operations). A shared zero-length buffer avoids allocating a
 * StringObject for it. The buffer is never freed; the StringObject's
 * `data` points at it.
 *
 * ─── Design: the byte buffer is separate from the header ──────────────────
 * A StringObject is small (16 bytes). The bytes live in a separate
 * allocation. This keeps a StringObject cheap to copy in the runtime's
 * bookkeeping (an array of them, a pool of them) and lets the bytes'
 * lifetime be independent of the header's. The release path frees
 * both.
 *
 * ─── Design: refcount is not atomic ───────────────────────────────────────
 * The runtime is single-threaded (grammar §9.2.6). A host that runs
 * multiple interpreters on multiple threads must not share strings
 * between them. If that ever changes, this file is where atomic
 * refcounting would go.
 */

#include "runtime/String.hpp"

#include <cstdlib>
#include <cstring>
#include <new>

namespace lucid::runtime {

namespace {

// The shared empty-string buffer. Never freed.
const char kEmptyBuffer[1] = { '\0' };

// ─── Allocation helpers ──────────────────────────────────────────────────

/// Allocate a StringObject and a buffer of `length + 1` bytes
/// (the +1 is for a trailing null for host convenience). The buffer
/// is uninitialized; the caller fills it.
///
/// Returns nullptr on allocation failure. The caller checks.
StringObject* allocateStringObject(uint32_t length) {
    // Allocate the header.
    StringObject* s = static_cast<StringObject*>(
        std::malloc(sizeof(StringObject)));
    if (!s) return nullptr;

    // The empty string shares the static buffer.
    if (length == 0) {
        s->refcount = 1;
        s->length   = 0;
        s->data     = kEmptyBuffer;
        return s;
    }

    // Allocate the byte buffer. +1 for the trailing null.
    char* buffer = static_cast<char*>(std::malloc(length + 1));
    if (!buffer) {
        std::free(s);
        return nullptr;
    }
    buffer[length] = '\0';

    s->refcount = 1;
    s->length   = length;
    s->data     = buffer;
    return s;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────

StringObject* allocString(std::string_view bytes) {
    const uint32_t length = static_cast<uint32_t>(bytes.size());
    StringObject* s = allocateStringObject(length);
    if (!s) return nullptr;

    if (length > 0) {
        std::memcpy(const_cast<char*>(s->data), bytes.data(), length);
    }
    return s;
}

StringObject* concatStrings(const StringObject* a, const StringObject* b) {
    const uint32_t total = a->length + b->length;

    StringObject* s = allocateStringObject(total);
    if (!s) return nullptr;

    if (total > 0) {
        char* out = const_cast<char*>(s->data);
        if (a->length > 0) std::memcpy(out, a->data, a->length);
        if (b->length > 0) std::memcpy(out + a->length, b->data, b->length);
    }
    return s;
}

// ─────────────────────────────────────────────────────────────────────────
// Refcounting
// ─────────────────────────────────────────────────────────────────────────

void retainString(StringObject* s) noexcept {
    if (s) ++s->refcount;
}

void releaseString(StringObject* s) noexcept {
    if (!s) return;
    if (--s->refcount > 0) return;

    // Free the byte buffer if it is not the shared empty buffer.
    if (s->length > 0 && s->data != kEmptyBuffer) {
        std::free(const_cast<char*>(s->data));
    }
    std::free(s);
}

StringObject* copyString(const StringObject* s) {
    if (!s) return nullptr;
    return allocString(stringView(s));
}

// ─────────────────────────────────────────────────────────────────────────
// Comparison
// ─────────────────────────────────────────────────────────────────────────

bool stringEquals(const StringObject* a, const StringObject* b) noexcept {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->length != b->length) return false;
    if (a->length == 0) return true;
    return std::memcmp(a->data, b->data, a->length) == 0;
}

int stringCompare(const StringObject* a, const StringObject* b) noexcept {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;

    const uint32_t minLen = a->length < b->length ? a->length : b->length;
    if (minLen > 0) {
        const int c = std::memcmp(a->data, b->data, minLen);
        if (c != 0) return c;
    }
    // Equal up to minLen; shorter string sorts first.
    if (a->length < b->length) return -1;
    if (a->length > b->length) return 1;
    return 0;
}

} // namespace lucid::runtime