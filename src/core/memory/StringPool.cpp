#include "StringPool.hpp"
#include <cstring>
#include <algorithm>

/**
 * @brief Constructs the pool with ID 0 reserved for the empty string.
 *
 * ID 0 always maps to "" so that InternedString() (id=0) behaves
 * consistently with lookup().
 */
StringPool::StringPool() {
    // Reserve ID 0 for the empty/invalid InternedString
    strings.push_back(std::string_view(""));
}

/**
 * @brief Copies `s` into the current bump-allocated block, allocating a new
 *        block if necessary.
 *
 * Returns a stable `std::string_view` pointing into the block. The caller
 * must ensure the pool outlives the returned view (the pool owns the block).
 */
std::string_view StringPool::allocateString(std::string_view s) {
    if (s.empty()) return std::string_view("");

    if (!currentBlock || currentOffset + s.size() > BLOCK_SIZE) {
        size_t allocSize = std::max(BLOCK_SIZE, s.size());
        blocks.push_back(std::make_unique<char[]>(allocSize));
        currentBlock = blocks.back().get();
        currentOffset = 0;
    }

    char* dest = currentBlock + currentOffset;
    std::memcpy(dest, s.data(), s.size());
    currentOffset += s.size();

    return std::string_view(dest, s.size());
}

/**
 * @brief Returns the InternedString ID for `s`, allocating if new.
 *
 * - Empty string → ID 0
 * - Already interned → existing ID
 * - New string → allocated, given the next sequential ID
 */
InternedString StringPool::intern(std::string_view s) {
    if (s.empty()) return InternedString(0);

    auto it = internMap.find(s);
    if (it != internMap.end()) {
        return InternedString(it->second);
    }

    std::string_view stored = allocateString(s);
    uint32_t id = static_cast<uint32_t>(strings.size());
    strings.push_back(stored);
    internMap.emplace(stored, id);

    return InternedString(id);
}

/**
 * @brief Retrieves the string data for a previously interned ID.
 *
 * Returns "" for ID 0 or any out-of-range ID.
 */
std::string StringPool::lookup(InternedString s) const {
    if (s.id == 0 || s.id >= strings.size()) {
        return std::string("");
    }
    return std::string(strings[s.id]);
}
