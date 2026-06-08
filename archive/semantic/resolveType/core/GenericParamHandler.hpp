/**
 * @file GenericParamHandler.hpp
 * @brief Manages generic parameter stacks and substitution maps during type resolution.
 * 
 * This component maintains two LIFO stacks:
 * 1. Generic parameter stack - tracks in-scope generic parameters (e.g., T in Box<T>)
 * 2. Substitution map stack - tracks concrete type substitutions (e.g., T -> int)
 * 
 * Both stacks support nested generic contexts like generic functions inside generic structs.
 * 
 * Example usage:
 * @code
 * GenericParamHandler handler;
 * handler.pushParams(&structGenericParams);
 * handler.pushSubstMap(&substitutionMap);
 * // ... resolve types ...
 * handler.popSubstMap();
 * handler.popParams();
 * @endcode
 */

#pragma once

#include "ast/BaseAST.hpp"
#include "ast/DeclAST.hpp"
#include "ast/TypeAST.hpp"
#include <unordered_map>
#include <vector>

class GenericParamHandler {
public:
    GenericParamHandler() = default;
    
    // ─────────────────────────────────────────────────────────────────────────
    // Generic Parameter Stack
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Push a new parameter list onto the stack.
     * @param params The parameter list to push (must outlive the push)
     */
    void pushParams(const ArenaSpan<GenericParamPtr>* params);
    
    /**
     * @brief Pop the top parameter list from the stack.
     */
    void popParams();
    
    /**
     * @brief Check if a name is a generic parameter in the current scope.
     * @param name The name to check (e.g., "T", "K", "V")
     * @return true if the name is a generic parameter in any active scope
     */
    bool isParam(InternedString name) const;
    
    /**
     * @brief Get the current (innermost) parameter list.
     * @return Pointer to the current parameter list, or nullptr if stack is empty
     */
    const ArenaSpan<GenericParamPtr>* currentParams() const;
    
    /**
     * @brief Get the depth of the generic parameter stack.
     */
    size_t paramDepth() const { return paramsStack_.size(); }
    
    // ─────────────────────────────────────────────────────────────────────────
    // Substitution Map Stack
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Push a substitution map onto the stack.
     * @param map The map from generic parameter names to concrete types
     */
    void pushSubstMap(const std::unordered_map<InternedString, TypeAST*>* map);
    
    /**
     * @brief Pop the top substitution map from the stack.
     */
    void popSubstMap();
    
    /**
     * @brief Look up a substitution for a generic parameter name.
     * @param name The generic parameter name (e.g., "T")
     * @return The substituted concrete type, or nullptr if no substitution exists
     */
    TypeAST* lookupSubst(InternedString name) const;
    
    /**
     * @brief Get the depth of the substitution map stack.
     */
    size_t substDepth() const { return substStack_.size(); }
    
    // ─────────────────────────────────────────────────────────────────────────
    // Utility
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Clear all stacks (for error recovery).
     */
    void clear();
    
private:
    // Stack of parameter lists (innermost last)
    std::vector<const ArenaSpan<GenericParamPtr>*> paramsStack_;
    
    // Stack of substitution maps (innermost last)
    std::vector<const std::unordered_map<InternedString, TypeAST*>*> substStack_;
};