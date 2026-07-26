/// @file ContextKind.hpp
/// @brief Semantic context kinds for validation rules.
/// 
/// Defines the kinds of semantic constructs that can appear on the context stack.
/// Each kind determines what statements are legal (e.g., `return` is only legal
/// inside FuncBody, `break` is only legal inside LoopBody or SwitchBody).
#pragma once

namespace sema {

/// @brief The kind of semantic construct currently being analyzed.
/// 
/// Each frame on the context stack has one of these kinds. The kind determines
/// what statements are legal (e.g., `return` is only legal inside FuncBody,
/// `break` is only legal inside LoopBody or SwitchBody).
enum class ContextKind {
    TopLevel,       ///< Module-level declarations (no function context)
    FuncBody,       ///< Inside a function body (return allowed)
    LoopBody,       ///< Inside a loop body (break/continue allowed)
    SwitchBody,     ///< Inside a switch body (case/default allowed)
    AsyncBody,      ///< Inside an async function (await allowed)
    GeneratorBody,  ///< Inside a generator function (yield allowed)
    ParallelBody,   ///< Inside a parallel/spawn block
    IfStmt,         ///< Inside an if statement (for type narrowing)
    Block,          ///< Inside a block statement (for pending inverse narrowing)
};

/// @brief Human-readable name for a ContextKind.
/// Used for diagnostic messages.
inline const char* contextKindName(ContextKind kind) {
    switch (kind) {
        case ContextKind::TopLevel:      return "top level";
        case ContextKind::FuncBody:      return "function body";
        case ContextKind::LoopBody:      return "loop body";
        case ContextKind::SwitchBody:    return "switch body";
        case ContextKind::AsyncBody:     return "async body";
        case ContextKind::GeneratorBody: return "generator body";
        case ContextKind::ParallelBody:  return "parallel body";
        case ContextKind::IfStmt:        return "if statement";
        case ContextKind::Block:         return "block";
    }
    return "unknown context";
}

} // namespace sema