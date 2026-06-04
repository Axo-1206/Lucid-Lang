/**
 * @file ConstraintChecker.hpp
 * @brief Checks trait constraint satisfaction for generic type parameters.
 * 
 * This component verifies that a concrete type implements all required traits
 * when used as a generic argument. For example, in `Box<T : Drawable + Comparable>`,
 * when `T` is instantiated with `Circle`, this checks that `Circle` implements
 * both `Drawable` and `Comparable`.
 */

#pragma once

#include "ast/TypeAST.hpp"
#include "ast/support/InternedString.hpp"
#include <unordered_map>
#include <vector>

struct SemanticContext;

class ConstraintChecker {
public:
    /**
     * @brief Construct a ConstraintChecker.
     * @param ctx The semantic context (for diagnostics and symbol lookup)
     */
    explicit ConstraintChecker(SemanticContext& ctx);
    
    // ─────────────────────────────────────────────────────────────────────────
    // Main Constraint Checking
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Check if a type satisfies all required trait constraints.
     * 
     * @param type The concrete type being checked (e.g., Circle when T=Circle)
     * @param requiredTraits List of trait names that the type must implement
     * @return true if all constraints are satisfied, false otherwise
     * 
     * Special cases:
     * - If requiredTraits is empty → returns true
     * - If type is still a generic parameter (not substituted) → returns true
     *   (constraint checking is deferred to instantiation time)
     */
    bool satisfies(TypeAST* type, const std::vector<InternedString>& requiredTraits) const;
    
    // ─────────────────────────────────────────────────────────────────────────
    // Data Management
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Set the struct → traits mapping collected during Phase 1.
     * 
     * This map is built by SemanticCollector from all impl blocks that
     * implement traits for structs.
     * 
     * @param map Map from struct name to list of trait names it implements
     */
    void setStructTraits(const std::unordered_map<InternedString, std::vector<InternedString>>* map);
    
    // ─────────────────────────────────────────────────────────────────────────
    // Type Classification
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Check if a type is a "value type" (not a reference or function type).
     * 
     * Value types are those that can be nullable with `?`:
     * - Primitives (int, float, bool, string, etc.)
     * - Structs
     * - Enums
     * - Arrays
     * - Named aliases to value types
     * 
     * @param type The type to check
     * @return true if the type is a value type
     */
    bool isValueType(TypeAST* type) const;
    
    /**
     * @brief Check if a type is a struct type.
     */
    bool isStructType(TypeAST* type) const;
    
    /**
     * @brief Check if a type is an enum type.
     */
    bool isEnumType(TypeAST* type) const;
    
private:
    // ─────────────────────────────────────────────────────────────────────────
    // Helper Methods
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Get the name of a type for lookup in structTraits_ map.
     */
    InternedString getTypeName(TypeAST* type) const;
    
    /**
     * @brief Recursively unwrap type aliases to get the underlying type.
     */
    TypeAST* unwrapAliases(TypeAST* type) const;
    
    // ─────────────────────────────────────────────────────────────────────────
    // Members
    // ─────────────────────────────────────────────────────────────────────────
    
    SemanticContext& ctx_;
    const std::unordered_map<InternedString, std::vector<InternedString>>* structTraits_ = nullptr;
};