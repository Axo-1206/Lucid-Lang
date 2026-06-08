/**
 * @file TypeCloner.hpp
 * @brief Deep cloning of type AST nodes with optional generic parameter substitution.
 * 
 * This namespace provides pure functions for creating deep copies of type nodes.
 * All functions are stateless - dependencies (arena, paramHandler) are passed as parameters.
 * 
 * During cloning with substitution, generic parameters (NamedTypeAST with isGenericParam=true)
 * are replaced by their substituted types from the current GenericParamHandler.
 */

#pragma once

#include "ast/TypeAST.hpp"
#include "ast/support/ASTArena.hpp"
#include <cstddef>

class GenericParamHandler;

namespace TypeCloner {

    // ─────────────────────────────────────────────────────────────────────────
    // Basic Cloning (no substitution)
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Create a deep copy of a type node.
     * @param arena The arena to allocate the cloned node in
     * @param type The type node to clone
     * @return A newly allocated copy of the type node (or nullptr on error)
     * 
     * The source node's location is copied to the destination.
     */
    TypeAST* clone(ASTArena& arena, const TypeAST* type);
    
    /**
     * @brief Create a deep copy of a function type node.
     * @param arena The arena to allocate the cloned node in
     * @param src The function type to clone
     * @return A newly allocated copy of the function type
     */
    FuncTypeAST* cloneFunc(ASTArena& arena, const FuncTypeAST* src);
    
    // ─────────────────────────────────────────────────────────────────────────
    // Cloning with Generic Substitution
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Clone a type while applying the current substitution map.
     * 
     * Generic parameters (NamedTypeAST with isGenericParam=true) are replaced
     * by their substituted types from paramHandler.
     * 
     * @param arena The arena to allocate the cloned node in
     * @param paramHandler The generic parameter handler (for substitution lookups)
     * @param type The type node to clone with substitution
     * @return A newly allocated copy with substitutions applied
     */
    TypeAST* cloneWithSubstitution(ASTArena& arena, 
                                   GenericParamHandler& paramHandler,
                                   const TypeAST* type);
    
    // ─────────────────────────────────────────────────────────────────────────
    // Synthetic Node Creation
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Create a new empty function type node (for type synthesis).
     * @param arena The arena to allocate the node in
     * @param loc The source location (default = unknown)
     * @return A newly allocated empty function type
     */
    FuncTypeAST* createFuncType(ASTArena& arena, const SourceLocation& loc = SourceLocation());
    
} // namespace TypeCloner