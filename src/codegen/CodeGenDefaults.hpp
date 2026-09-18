/// @file CodeGenDefaults.hpp
/// @brief Program-wide constants that every codegen caller must agree on.

#pragma once

#include <cstdint>

namespace codegen::defaults {

/// @brief Number of slots in the `__lucid_module_instances` table.
///
/// Every module's IR declares `@__lucid_module_instances = external
/// global [N x ptr]`, and every module's `loadModuleInstance` indexes
/// into it with a per-module ID. For the table to be a single object
/// at runtime, every module in a program must agree on `N`.
///
/// The value is a program-wide constant, not a per-codegen-run
/// decision. The interpreter sizes its session's instance table to
/// this value; the pipeline's `emit-ir` path sizes its IR's array
/// declaration to this value; and both must be the same, so that
/// `emit-ir` output is structurally identical to the IR the
/// interpreter would generate for the same source.
///
/// 256 is the current upper bound on modules per program. See
/// `InterpreterSession` for the runtime-side enforcement (a program
/// whose module count exceeds this is rejected during load).
constexpr uint32_t kModuleCapacity = 256;

} // namespace codegen::defaults