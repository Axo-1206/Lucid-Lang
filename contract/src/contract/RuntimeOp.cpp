/// @file contract/RuntimeOp.cpp
/// @brief The runtime operation info table.

#include "contract/RuntimeOp.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG

#include <array>

namespace lucid::contract {

namespace {

constexpr std::array<RuntimeOpInfo, 256> RUNTIME_OP_TABLE = [] {
    std::array<RuntimeOpInfo, 256> t{};
    for (auto& e : t) e = {RuntimeOp::Panic, nullptr, 0, 0};

    t[0x01] = {RuntimeOp::Retain,       "Retain",       0, 0};
    t[0x02] = {RuntimeOp::Release,      "Release",      1, 0};
    t[0x03] = {RuntimeOp::CopyString,   "CopyString",   1, 1};
    t[0x04] = {RuntimeOp::FreeString,   "FreeString",   1, 0};
    t[0x05] = {RuntimeOp::CopyArray,    "CopyArray",    1, 1};
    t[0x06] = {RuntimeOp::FreeArray,    "FreeArray",    1, 0};
    t[0x07] = {RuntimeOp::ConcatString, "ConcatString", 2, 1};
    t[0x08] = {RuntimeOp::Panic,        "Panic",        1, 0};

    return t;
}();

} // namespace

bool isRuntimeOp(uint8_t byte) noexcept {
    if (byte == 0x00) return false;
    return RUNTIME_OP_TABLE[byte].name != nullptr;
}

const RuntimeOpInfo& runtimeOpInfo(RuntimeOp op) noexcept {
    const uint8_t b = static_cast<uint8_t>(op);
    AST_ASSERT_MSG(RUNTIME_OP_TABLE[b].name != nullptr,
        "runtimeOpInfo: no entry for this RuntimeOp — the enum and "
        "the table are out of sync");
    return RUNTIME_OP_TABLE[b];
}

} // namespace lucid::contract