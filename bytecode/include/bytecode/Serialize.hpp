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
 *
 * Section order (format version 1):
 *  1. Header:        magic u32, formatVersion u32, producerVersion string
 *  2. Manifest
 *  3. HostSymbolTable
 *  4. ConstantPool
 *  5. StaticData
 *  6. FunctionProto[N]: u32 count, then N protos
 *  7. (optional) CRC32 over sections 1-6
 *  
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

/// The producer version string written into the header for
/// diagnostics. Not used by the loader. Bump on every release.
constexpr const char* LUCB_PRODUCER_VERSION = "lucid-0.1.0";

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