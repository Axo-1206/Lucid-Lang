/**
 * @file contract/Format.hpp
 *
 * @responsibility The .lucb format's magic number, format version, and
 *                 producer version. The numbers every producer and
 *                 consumer of a .lucb must agree on.
 *
 * ─── RULE: bumping the format version ─────────────────────────────────────
 * Any change to a file in contract/include/contract/ is a change to
 * the compiler/interpreter interface. Such a change requires bumping
 * LUCB_FORMAT_VERSION below. The version is checked at load time; a
 * mismatch rejects the .lucb before any code runs.
 *
 * Files whose change requires a bump:
 *   - Opcode.hpp          (the instruction set)
 *   - RuntimeOp.hpp       (the runtime operation set)
 *   - TypeDescriptor.hpp  (the type shape)
 *   - Signature.hpp       (the function signature shape)
 *   - ResourcePlan.hpp    (the resource classification)
 *
 * A CI check hashes the contract headers and compares against a
 * checked-in value; if the hash changed, this file must have changed
 * too.
 *
 * ─── Why this is its own header ───────────────────────────────────────────
 * Serialize.hpp needs the version constants to write them, and the
 * interpreter's load path needs them to validate. Pulling in the full
 * Serialize.hpp just to read a u32 is wasteful; Format.hpp is small
 * enough to include anywhere.
 */

#pragma once

#include <cstdint>

namespace lucid::contract {

/// @brief The current .lucb format version.
///
/// Bump this on any change to contract/include/contract/. The loader
/// rejects a .lucb whose version does not match.
constexpr uint32_t LUCB_FORMAT_VERSION = 2;

/// @brief The magic number at the start of every .lucb file.
constexpr uint32_t LUCB_MAGIC = 0x4C554342;  // "LUCB" in ASCII

/// @brief The producer version string written into the header for
///        diagnostics. Not used by the loader. Bump on every release.
constexpr const char* LUCB_PRODUCER_VERSION = "lucid-0.1.0";

} // namespace lucid::contract