/**
 * @file TypeCloner.hpp
 * @brief Deep cloning of type AST nodes with optional generic parameter substitution.
 * 
 * This component creates deep copies of type nodes in the arena. During cloning,
 * it can optionally apply the current substitution map to replace generic parameters
 * with concrete types.
 */

#pragma once

#include "ast/TypeAST.hpp"
#include "ast/BaseAST.hpp"

class GenericParamHandler;

class TypeCloner {
public:
    /**
     * @brief Construct a TypeCloner.
     * @param arena The arena to allocate cloned nodes in
     * @param paramHandler The generic parameter handler (for substitution lookups)
     */
    TypeCloner(ASTArena& arena, GenericParamHandler& paramHandler);
    
    // ─────────────────────────────────────────────────────────────────────────
    // Main Cloning Entry Points
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Create a deep copy of a type node.
     * @param type The type node to clone
     * @return A newly allocated copy of the type node (or nullptr on error)
     */
    TypeAST* clone(const TypeAST* type);
    
    /**
     * @brief Create a deep copy of a function type node.
     * @param src The function type to clone
     * @param loc The source location for the new node
     * @return A newly allocated copy of the function type
     */
    FuncTypeAST* cloneFunc(const FuncTypeAST* src, const SourceLocation& loc);
    
    /**
     * @brief Clone a type while applying the current substitution map.
     * 
     * This is the same as clone(), but generic parameters (e.g., NamedTypeAST
     * with isGenericParam=true) are replaced by their substituted types.
     * 
     * @param type The type node to clone with substitution
     * @return A newly allocated copy with substitutions applied
     */
    TypeAST* cloneWithSubstitution(const TypeAST* type);
    
private:
    // ─────────────────────────────────────────────────────────────────────────
    // Type-Specific Clone Helpers
    // ─────────────────────────────────────────────────────────────────────────
    
    TypeAST* clonePrimitive(const PrimitiveTypeAST* src);
    TypeAST* cloneNamed(const NamedTypeAST* src);
    TypeAST* cloneNamedWithSubstitution(const NamedTypeAST* src);
    TypeAST* cloneNullable(const NullableTypeAST* src);
    TypeAST* cloneResult(const ResultTypeAST* src);
    TypeAST* cloneArray(const ArrayTypeAST* src);
    TypeAST* cloneRef(const RefTypeAST* src);
    TypeAST* clonePtr(const PtrTypeAST* src);
    
    /**
     * @brief Internal helper for cloning function types.
     * @param dst The destination node (already allocated)
     * @param src The source node
     * @param loc The source location
     * @return The cloned function type (same as dst)
     */
    FuncTypeAST* cloneFuncInternal(FuncTypeAST* dst, const FuncTypeAST* src, const SourceLocation& loc);
    
    /**
     * @brief Internal helper for cloning function types with substitution.
     */
    FuncTypeAST* cloneFuncWithSubstitutionInternal(FuncTypeAST* dst, const FuncTypeAST* src, const SourceLocation& loc);
    
    // ─────────────────────────────────────────────────────────────────────────
    // Members
    // ─────────────────────────────────────────────────────────────────────────
    
    ASTArena& arena_;
    GenericParamHandler& paramHandler_;
};