#include "StringPool.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace lucid {

StringPool::StringPool() {
    // Reserve ID 0 for the empty / invalid handle. Every real string
    // starts at ID 1.
    strings_.push_back(std::string_view{});
}

std::string_view StringPool::allocateString(std::string_view s) {
    if (s.empty()) return std::string_view{};

    if (!currentBlock_ || currentOffset_ + s.size() > kBlockSize) {
        const size_t allocSize = std::max(kBlockSize, s.size());
        blocks_.push_back(std::make_unique<char[]>(allocSize));
        currentBlock_  = blocks_.back().get();
        currentOffset_ = 0;
    }

    char* dest = currentBlock_ + currentOffset_;
    std::memcpy(dest, s.data(), s.size());
    currentOffset_ += s.size();

    return std::string_view(dest, s.size());
}

InternedString StringPool::intern(std::string_view s) {
    if (s.empty()) return InternedString{0};

    if (auto it = internMap_.find(s); it != internMap_.end()) {
        return InternedString{it->second};
    }

    // Copy the bytes into a pool block first. The view we store in the
    // map must point into a block, never into the caller's buffer.
    const std::string_view stored = allocateString(s);

    // Belt-and-suspenders for the block invariant. Fires only if a future
    // change to allocateString stops copying.
    assert(!blocks_.empty() &&
           "StringPool: allocateString must have created a block");

    const uint32_t id = static_cast<uint32_t>(strings_.size());
    strings_.push_back(stored);
    internMap_.emplace(stored, id);

    return InternedString{id};
}

std::string StringPool::lookup(InternedString s) const {
    if (!contains(s) || s.id == 0) return {};
    return std::string(strings_[s.id]);
}

std::string_view StringPool::lookupView(InternedString s) const {
    if (!contains(s) || s.id == 0) return {};
    return strings_[s.id];
}

} // namespace lucid