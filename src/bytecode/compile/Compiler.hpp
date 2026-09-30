/// @file compile/Compiler.hpp
///
/// @responsibility The driver: takes a resolved module set and produces
///                 a Bytecode.
///
/// ─── Design: the compiler trusts Sema ─────────────────────────────────────
/// Every name is resolved, every type is resolved, every constant is
/// folded, every resource kind is classified. The compiler reads those
/// fields and asserts them as it goes. An assertion failure means Sema
/// broke its contract; it is a compiler bug, not a user error.

#pragma once

#include "../Bytecode.hpp"

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/memory/StringPool.hpp"

#include <optional>
#include <unordered_map>
#include <vector>

namespace lucid::bytecode::compile {

class Compiler {
public:
    Compiler(lucid::diag::DiagnosticEngine& diagnostics,
             StringPool& pool);

    Bytecode compile(const std::vector<ModuleAST*>& modules);

    /// The session's string pool. Every call site that needs text from
    /// an InternedString resolves it through this pool.
    StringPool& pool() noexcept { return m_pool; }

    /// The index reserved for a function in pass A of compile().
    /// Returns nullopt if the function is host-bound or was not
    /// registered (which is a compiler bug).
    std::optional<uint32_t> functionIndexOf(const FnDeclAST* fn) const;

private:
    lucid::diag::DiagnosticEngine& m_diag;
    StringPool& m_pool;

    // Populated during compile(). Not a member of the artifact; it
    // is the driver's bookkeeping for resolving forward references.
    std::unordered_map<const FnDeclAST*, uint32_t> m_functionIndex;

    // Baking helpers. Each does one kind of translation from a
    // Sema-resolved declaration into a part of the artifact.
    void bakeTable(const TableDeclAST* table,
                   StaticData& staticData,
                   HostSymbolTable& hostSymbols);
    void bakeTopLevelBinding(const VarDeclAST* var,
                             StaticData& staticData);

    // Placeholders for Phase 2. Both are replaced by real
    // implementations in Phase 3.
    TypeDescriptor makeUnknownTypeDescriptor() const;
    FunctionProto makePlaceholderProto(const FnDeclAST* fn) const;
};

} // namespace lucid::bytecode::compile