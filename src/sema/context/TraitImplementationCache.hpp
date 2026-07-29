/// @file TraitImplementationCache.hpp
/// @brief Tracks which structs implement which traits for fast lookup.
/// 
/// This cache is populated during struct declaration analysis and queried
/// during generic constraint validation. It provides O(1) lookup for
/// "does struct S implement trait T?"
/// 
/// @architectural_note Raw pointers are safe because AST nodes are arena-allocated
///   and outlive the semantic analysis phase. We never delete or modify AST nodes
///   after creation, so pointers remain stable.

#pragma once

#include "core/ast/DeclAST.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sema {

/// @brief Cache for trait implementations.
/// 
/// Provides fast O(1) lookup to answer "does this struct implement this trait?"
/// and "what traits does this struct implement?"
/// 
/// @invariant All struct and trait pointers are non-null and point to valid
///            AST nodes that outlive this cache.
class TraitImplementationCache {
public:
    // ─── Registration ──────────────────────────────────────────────────────

    /// @brief Register that a struct implements a trait.
    /// 
    /// Called during struct declaration analysis after validating that the
    /// struct correctly implements all required fields.
    /// 
    /// @param structDecl The struct that implements the trait.
    /// @param traitDecl The trait being implemented.
    /// @param ctx The semantic context (for error reporting).
    /// @return true if registration succeeded, false if already registered.
    bool addImplementation(const StructDeclAST* structDecl,
                           const TraitDeclAST* traitDecl);

    // ─── Queries ────────────────────────────────────────────────────────────

    /// @brief Check if a struct implements a trait.
    /// 
    /// O(1) lookup using the implementation map.
    /// 
    /// @param structDecl The struct to check.
    /// @param traitDecl The trait to check for.
    /// @return true if the struct implements the trait.
    bool implements(const StructDeclAST* structDecl,
                    const TraitDeclAST* traitDecl) const;

    /// @brief Check if a type implements a trait.
    /// 
    /// Convenience overload that resolves the type to a StructDeclAST if possible.
    /// 
    /// @param typeDecl The type to check (must be a struct type).
    /// @param traitDecl The trait to check for.
    /// @return true if the type implements the trait.
    bool implements(const TypeDeclAST* typeDecl,
                    const TraitDeclAST* traitDecl) const;

    /// @brief Get all traits implemented by a struct.
    /// 
    /// @param structDecl The struct to query.
    /// @return Vector of trait pointers, or empty vector if none.
    const std::vector<const TraitDeclAST*>& getTraitsForStruct(
        const StructDeclAST* structDecl) const;

    /// @brief Get all structs that implement a trait.
    /// 
    /// @param traitDecl The trait to query.
    /// @return Vector of struct pointers, or empty vector if none.
    std::vector<const StructDeclAST*> getStructsForTrait(
        const TraitDeclAST* traitDecl) const;

    // ─── Debug / Inspection ──────────────────────────────────────────────

    /// @brief Clear all cached data (for testing).
    void clear();

    /// @brief Check if a struct has any implementations registered.
    bool hasImplementations(const StructDeclAST* structDecl) const;

    /// @brief Get the total number of registered implementations.
    size_t implementationCount() const;

private:
    // ─── Data Structures ──────────────────────────────────────────────────

    /// Map from struct → list of traits it implements
    std::unordered_map<const StructDeclAST*, std::vector<const TraitDeclAST*>> m_structToTraits;

    /// Map from trait → set of structs that implement it
    /// Using unordered_set for O(1) lookup
    std::unordered_map<const TraitDeclAST*, std::unordered_set<const StructDeclAST*>> m_traitToStructs;

    // ─── Helper Functions ──────────────────────────────────────────────────

    /// @brief Validate that a struct implements a trait's fields.
    /// 
    /// Checks:
    ///   1. All trait fields exist in the struct
    ///   2. Field types match exactly
    ///   3. Const-ness matches (trait const → struct const required)
    /// 
    /// @param structDecl The struct to validate.
    /// @param traitDecl The trait to validate against.
    /// @param ctx The semantic context (for error reporting).
    /// @return true if the struct correctly implements the trait.
    bool validateTraitImplementation(const StructDeclAST* structDecl,
                                      const TraitDeclAST* traitDecl) const;
};

} // namespace sema