/**
 * @file ConstraintChecker.hpp
 * @brief Checks trait constraint satisfaction for generic type parameters.
 * 
 * This namespace provides pure functions for validating that a concrete type
 * implements all required traits. All functions are stateless - they read
 * the struct → traits mapping from SemanticContext::structTraits.
 * 
 * Example: In `Box<T : Drawable + Comparable>`, when `T` is instantiated with
 * `Circle`, this checks that `Circle` implements both `Drawable` and `Comparable`.
 * 
 * NOTE: This does NOT validate impl target types (e.g., `impl int`). 
 * That validation belongs in ImplResolver.
 */

#pragma once

#include "ast/TypeAST.hpp"
#include "ast/support/InternedString.hpp"
#include <vector>

struct SemanticContext;

namespace ConstraintChecker {

    // ─────────────────────────────────────────────────────────────────────────
    // Main Constraint Checking
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Check if a type satisfies all required trait constraints.
     * 
     * @param ctx The semantic context (for diagnostics and structTraits map)
     * @param type The concrete type being checked (e.g., Circle when T=Circle)
     * @param requiredTraits List of trait names that the type must implement
     * @return true if all constraints are satisfied, false otherwise
     * 
     * Special cases:
     * - If requiredTraits is empty → returns true
     * - If type is still a generic parameter (not substituted) → returns true
     *   (constraint checking is deferred to instantiation time)
     */
    bool satisfies(SemanticContext& ctx,
                   TypeAST* type, 
                   const std::vector<InternedString>& requiredTraits);
    
    // ─────────────────────────────────────────────────────────────────────────
    // Type Classification Helpers
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Check if a type is a "value type" (can be nullable with `?`).
     */
    bool isValueType(SemanticContext& ctx, TypeAST* type);
    
    /**
     * @brief Check if a type is a struct type.
     */
    bool isStructType(SemanticContext& ctx, TypeAST* type);
    
    /**
     * @brief Check if a type is an enum type.
     */
    bool isEnumType(SemanticContext& ctx, TypeAST* type);
    
    /**
     * @brief Check if a type is a function type.
     */
    bool isFunctionType(SemanticContext& ctx, TypeAST* type);
    
    /**
     * @brief Check if a type is a reference type (&T).
     */
    bool isReferenceType(SemanticContext& ctx, TypeAST* type);
    
    /**
     * @brief Check if a type is an array type.
     */
    bool isArrayType(SemanticContext& ctx, TypeAST* type);
    
    // ─────────────────────────────────────────────────────────────────────────
    // Type Resolution Helpers
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Get the base name of a type (for struct/alias lookup).
     * @return The interned name, or empty InternedString if not applicable
     */
    InternedString getTypeName(SemanticContext& ctx, TypeAST* type);
    
    /**
     * @brief Recursively unwrap type aliases to get the underlying type.
     * @return The underlying type after unwrapping all aliases
     */
    TypeAST* unwrapAliases(SemanticContext& ctx, TypeAST* type);
    
    /**
     * @brief Check if a type can be an impl target (struct, enum, or alias to them).
     * @return true if the type is a valid impl target
     */
    bool isValidImplTarget(SemanticContext& ctx, TypeAST* type);
    
} // namespace ConstraintChecker