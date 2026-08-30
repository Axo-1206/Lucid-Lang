/// @file BuiltinTypes.hpp
/// @brief Definitions and helpers for compiler-builtin types.
/// 
/// This module is PURE and CONTEXT-FREE. It depends only on:
///   - AST node types (TypeAST, DeclAST, ExprAST)
///   - InternedString
///   - StringPool (for string operations)
///   - DiagnosticEngine (for error reporting)
/// 
/// It does NOT depend on:
///   - SemaContext
///   - ParseContext
///   - CodeGenContext
///   - Any compilation phase-specific context

#pragma once

#include "core/ast/TypeAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/SourceLocation.hpp"

#include <optional>

namespace builtins {

// ─── Built-in Type Names ──────────────────────────────────────────────────

/// @brief Get the interned string for "Arena".
inline InternedString ArenaName(StringPool& pool) {
    return pool.intern("Arena");
}

/// @brief Get the interned string for "ArenaDescriptor".
inline InternedString ArenaDescriptorName(StringPool& pool) {
    return pool.intern("ArenaDescriptor");
}

// ─── Arena Method Names ──────────────────────────────────────────────────

/// @brief All valid Arena method names.
enum class ArenaMethodKind {
    Create,     // static: Arena::create(size) -> Arena!
    Empty,      // static: Arena::empty() -> Arena
    Alloc,      // instance: arena::alloc<T>(count) -> [_]T
    Reset,      // instance: arena::reset() -> ()
    Descriptor, // instance: arena::descriptor() -> ArenaDescriptor
    Capacity,   // instance: arena::capacity() -> uint64
    Remaining,  // instance: arena::remaining() -> uint64
    IsEmpty,    // instance: arena::isEmpty() -> bool
    Space,      // instance: arena::space<T>() -> uint64
    CanFit,     // instance: arena::canFit<T>(n) -> bool
};

inline InternedString ArenaMethodName(StringPool& pool, ArenaMethodKind kind) {
    switch (kind) {
        case ArenaMethodKind::Create:     return pool.intern("create");
        case ArenaMethodKind::Empty:      return pool.intern("empty");
        case ArenaMethodKind::Alloc:      return pool.intern("alloc");
        case ArenaMethodKind::Reset:      return pool.intern("reset");
        case ArenaMethodKind::Descriptor: return pool.intern("descriptor");
        case ArenaMethodKind::Capacity:   return pool.intern("capacity");
        case ArenaMethodKind::Remaining:  return pool.intern("remaining");
        case ArenaMethodKind::IsEmpty:    return pool.intern("isEmpty");
        case ArenaMethodKind::Space:      return pool.intern("space");
        case ArenaMethodKind::CanFit:     return pool.intern("canFit");
    }
    return InternedString();
}

/// @brief Parse an arena method name from an interned string.
std::optional<ArenaMethodKind> parseArenaMethod(InternedString name, StringPool& pool);

// ─── Type Detection ──────────────────────────────────────────────────────

/// @brief Check if a type is the Arena built-in type.
bool isArenaType(TypeAST* type);

/// @brief Check if a type is the ArenaDescriptor built-in type.
bool isArenaDescriptorType(TypeAST* type);

/// @brief Check if a NamedTypeAST is the Arena built-in type.
bool isArenaNamedType(NamedTypeAST* named);

/// @brief Check if a NamedTypeAST is the ArenaDescriptor built-in type.
bool isArenaDescriptorNamedType(NamedTypeAST* named);

// ─── Type Validation ─────────────────────────────────────────────────────

/// @brief Validate that a type is the Arena built-in type.
/// 
/// Uses the node's own location for error reporting.
/// 
/// @param type The type to check.
/// @param node The AST node containing the type (for location).
/// @param pool The string pool for type names.
/// @param diag The diagnostic engine.
/// @return true if the type is Arena, false otherwise.
bool validateArenaType(TypeAST* type, 
                       BaseAST* node,
                       StringPool& pool,
                       DiagnosticEngine& diag);

/// @brief Validate that a type is the ArenaDescriptor built-in type.
/// 
/// Uses the node's own location for error reporting.
/// 
/// @param type The type to check.
/// @param node The AST node containing the type (for location).
/// @param pool The string pool for type names.
/// @param diag The diagnostic engine.
/// @return true if the type is ArenaDescriptor, false otherwise.
bool validateArenaDescriptorType(TypeAST* type,
                                  BaseAST* node,
                                  StringPool& pool,
                                  DiagnosticEngine& diag);

/// @brief Validate that a type is NOT ArenaDescriptor (for struct literals).
/// 
/// ArenaDescriptor cannot be constructed via struct literal syntax.
/// Uses the node's own location for error reporting.
/// 
/// @param type The type being constructed.
/// @param node The AST node containing the type (for location).
/// @param pool The string pool for type names.
/// @param diag The diagnostic engine.
/// @return true if the type is NOT ArenaDescriptor, false otherwise.
bool validateNotArenaDescriptorLiteral(TypeAST* type,
                                        BaseAST* node,
                                        StringPool& pool,
                                        DiagnosticEngine& diag);

// ─── Arena Declaration Validation ───────────────────────────────────────

/// @brief Validate an Arena variable declaration.
/// 
/// Enforces:
///   1. Binding must be `const`
///   2. Initializer must be `Arena::create(size)` or `Arena::empty()`
/// 
/// Uses the declaration's own location for error reporting.
/// 
/// @param decl The variable declaration to validate.
/// @param pool The string pool for name lookup.
/// @param diag The diagnostic engine.
/// @return true if valid, false if error reported.
bool validateArenaVarDecl(VarDeclAST* decl, 
                          StringPool& pool,
                          DiagnosticEngine& diag);

/// @brief Validate that an Arena binding is const.
/// 
/// Uses the declaration's own location for error reporting.
/// 
/// @param decl The variable declaration to check.
/// @param diag The diagnostic engine.
/// @return true if const, false if error reported.
bool validateArenaBindingConst(VarDeclAST* decl,
                                DiagnosticEngine& diag);

/// @brief Validate an Arena initializer.
/// 
/// Uses the initializer's own location for error reporting.
/// 
/// @param init The initializer expression.
/// @param pool The string pool for name lookup.
/// @param diag The diagnostic engine.
/// @return true if valid, false if error reported.
bool validateArenaInitializer(ExprAST* init,
                               StringPool& pool,
                               DiagnosticEngine& diag);

// ─── Arena Access Validation ────────────────────────────────────────────

/// @brief Validate an Arena access expression.
/// 
/// Validates:
///   1. Method name is valid
///   2. Generic arguments are valid (only alloc<T> can have them)
///   3. Argument count is correct for the method
/// 
/// Uses the expression's own location for error reporting.
/// 
/// @param expr The ArenaAccessExprAST to validate.
/// @param pool The string pool for name lookup.
/// @param diag The diagnostic engine.
/// @return The ArenaMethodKind if valid, std::nullopt on error.
std::optional<ArenaMethodKind> validateArenaAccess(ArenaAccessExprAST* expr,
                                                    StringPool& pool,
                                                    DiagnosticEngine& diag);

/// @brief Validate the argument count for an Arena method.
/// 
/// @param method The method kind.
/// @param argCount The number of arguments provided.
/// @param node The AST node containing the call (for location).
/// @param diag The diagnostic engine.
/// @return true if argument count is valid, false otherwise.
bool validateArenaMethodArgCount(ArenaMethodKind method,
                                  size_t argCount,
                                  BaseAST* node,
                                  DiagnosticEngine& diag);

/// @brief Check if an Arena method requires generic arguments.
/// 
/// @param method The method kind.
/// @return true if the method requires a generic argument.
bool arenaMethodRequiresGenericArg(ArenaMethodKind method);

/// @brief Check if an Arena method is static.
/// 
/// @param method The method kind.
/// @return true if the method is static (Arena::method).
bool isArenaMethodStatic(ArenaMethodKind method);

/// @brief Check if an Arena method is void (returns nothing).
/// 
/// @param method The method kind.
/// @return true if the method returns void.
bool isArenaMethodVoid(ArenaMethodKind method);

/// @brief Get the return type for an Arena method.
/// 
/// Returns a TypeAST representing the method's return type.
/// The caller is responsible for ensuring the type is created
/// in the appropriate context and properly cached.
/// 
/// @param method The method kind.
/// @param genericArg The generic argument (for alloc<T> and space<T>).
/// @param pool The string pool for name lookup.
/// @param arena The AST arena for allocation.
/// @return The return type, or nullptr if void.
TypeAST* getArenaMethodReturnType(ArenaMethodKind method,
                                   TypeAST* genericArg,
                                   StringPool& pool,
                                   ASTArena& arena);

// ─── ArenaDescriptor Type Construction ─────────────────────────────────

/// @brief Create a NamedTypeAST for ArenaDescriptor.
/// 
/// This is a factory function that creates the type node.
/// The caller is responsible for caching it in their own context.
/// 
/// @param pool The string pool for name lookup.
/// @param arena The AST arena for allocation.
/// @return The ArenaDescriptor NamedTypeAST.
NamedTypeAST* createArenaDescriptorType(StringPool& pool, ASTArena& arena);

/// @brief Create a NamedTypeAST for Arena.
/// 
/// This is a factory function that creates the type node.
/// The caller is responsible for caching it in their own context.
/// 
/// @param pool The string pool for name lookup.
/// @param arena The AST arena for allocation.
/// @return The Arena NamedTypeAST.
NamedTypeAST* createArenaType(StringPool& pool, ASTArena& arena);

// ─── ArenaDescriptor Layout Information ────────────────────────────────

/// @brief Get the number of fields in ArenaDescriptor.
inline constexpr size_t ArenaDescriptorFieldCount = 2;

/// @brief Get the name of an ArenaDescriptor field by index.
InternedString getArenaDescriptorFieldName(size_t index, StringPool& pool);

/// @brief Get the type of an ArenaDescriptor field by index.
TypeAST* getArenaDescriptorFieldType(size_t index, StringPool& pool, ASTArena& arena);

} // namespace builtins