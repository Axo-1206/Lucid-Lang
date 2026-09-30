/**
 * @file Serialize.hpp
 *
 * @responsibility Round-trip a Bytecode to and from a byte stream.
 *                 The .lucb format.
 *
 * ─── Design: a versioned, self-describing format ──────────────────────────
 * The header carries a magic number and a format version. Every read
 * validates against the format's structural invariants and reports a
 * DiagCode-tagged error on failure. Serialize's job is format
 * validation, not semantic validation — Sema already ran, and the
 * interpreter will re-validate at load.
 */

#pragma once

#include "Bytecode.hpp"
#include "core/diagnostics/DiagCode.hpp"

#include <istream>
#include <ostream>
#include <optional>
#include <string>

namespace lucid::bytecode {

/// @brief The current .lucb format version.
constexpr uint32_t LUCB_FORMAT_VERSION = 1;

/// @brief The magic number at the start of every .lucb file.
constexpr uint32_t LUCB_MAGIC = 0x4C554342;  // "LUCB" in ASCII

/// @brief A serialization error.
struct SerializeError {
    lucid::diag::DiagCode code;
    std::string           message;
};

/// @brief Write a Bytecode to a stream.
/// @return nullopt on success, an error on failure.
std::optional<SerializeError> serialize(const Bytecode& bc, std::ostream& os);

/// @brief Read a Bytecode from a stream.
/// @return the Bytecode on success, an error on failure.
struct DeserializeResult {
    std::optional<Bytecode>       bytecode;
    std::optional<SerializeError> error;
};
DeserializeResult deserialize(std::istream& is);

} // namespace lucid::bytecode