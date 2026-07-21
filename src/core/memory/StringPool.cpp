#include "StringPool.hpp"
#include <cstring>
#include <algorithm>

/**
 * @brief Constructs the pool with ID 0 reserved for the empty string.
 */
StringPool::StringPool() {
    strings.push_back(std::string_view(""));
}

/**
 * @brief Clear the pool for testing.
 */
void StringPool::clear() {
    internMap.clear();
    strings.clear();
    blocks.clear();
    currentBlock = nullptr;
    currentOffset = 0;
    // Reserve ID 0 for empty string
    strings.push_back(std::string_view(""));
}

/**
 * @brief Copies `s` into the current bump-allocated block.
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
 */
std::string StringPool::lookup(InternedString s) const {
    if (s.id == 0 || s.id >= strings.size()) {
        return std::string("");
    }
    return std::string(strings[s.id]);
}

/**
 * @brief Retrieves the string data as a view.
 */
std::string_view StringPool::lookupView(InternedString s) const {
    if (s.id == 0 || s.id >= strings.size()) {
        return std::string_view("");
    }
    return strings[s.id];
}