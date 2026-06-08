/**
 * @file ScopeStack.hpp
 * @brief Lexical scoping for AST-node-only name resolution.
 * 
 * ============================================================================
 * DESIGN PRINCIPLES
 * ============================================================================
 * 
 * The ScopeStack manages nested scopes (blocks, functions, etc.) and stores
 * AST nodes directly – no separate Symbol struct.
 * 
 * Two separate namespaces:
 *   - Value namespace: VarDeclAST, FuncDeclAST, ParamAST, FieldDeclAST, 
 *                      MethodDeclAST, EnumVariantAST
 *   - Type namespace:  StructDeclAST, EnumDeclAST, TraitDeclAST, TypeAliasDeclAST
 * 
 * Overload sets are stored separately for functions with the same name.
 * 
 * ─── Lookup Rules ─────────────────────────────────────────────────────────
 * 
 *   - Values: searched from innermost scope outward
 *   - Types: searched from innermost scope outward
 *   - Overloads: searched from innermost scope outward (returns all candidates)
 * 
 * ─── Memory Management ─────────────────────────────────────────────────────
 * 
 *   - All pointers are raw – AST nodes are owned by the ASTArena
 *   - The ScopeStack does NOT own the AST nodes
 *   - Scopes are cleared when popped (but nodes remain in arena)
 * 
 * @see ValueDeclAST, TypeDeclAST in DeclAST.hpp
 */

#pragma once

#include "ast/DeclAST.hpp"
#include "ast/support/InternedString.hpp"
#include "ast/support/StringPool.hpp"

#include <unordered_map>
#include <deque>
#include <vector>
#include <string_view>

// ============================================================================
// Scope – Single lexical scope with separate namespaces
// ============================================================================

/**
 * @brief A single lexical scope (global, function, block, etc.).
 * 
 * Each scope contains three maps:
 *   - values:   Value namespace (VarDeclAST, FuncDeclAST, etc.)
 *   - types:    Type namespace (StructDeclAST, EnumDeclAST, etc.)
 *   - overloads: Function overload sets (FuncDeclAST only)
 * 
 * @note ImplDeclAST and FromDeclAST are NOT stored in scopes – they are
 *       collected separately for conformance checking.
 */
struct Scope {
    // Value namespace: variables, functions, parameters, fields, methods, enum variants
    std::unordered_map<uint32_t, ValueDeclAST*> values;
    
    // Type namespace: structs, enums, traits, type aliases
    std::unordered_map<uint32_t, TypeDeclAST*> types;
    
    // Overload sets: function name → list of overloads (only for FuncDeclAST)
    std::unordered_map<uint32_t, std::vector<FuncDeclAST*>> overloads;
    
    /**
     * @brief Clears all maps in this scope.
     */
    void clear() {
        values.clear();
        types.clear();
        overloads.clear();
    }
    
    /**
     * @brief Returns true if all maps are empty.
     */
    bool empty() const {
        return values.empty() && types.empty() && overloads.empty();
    }
    
    /**
     * @brief Returns total number of entries across all maps.
     */
    size_t size() const {
        return values.size() + types.size() + overloads.size();
    }
};

// ============================================================================
// ScopeStack – Manages nested scopes
// ============================================================================

/**
 * @brief Manages a stack of lexical scopes.
 * 
 * Usage:
 *   ScopeStack scopes;
 *   scopes.push();                    // Enter global scope
 *   scopes.declareValue(varDecl);     // Declare a variable
 *   scopes.push();                    // Enter function body
 *   scopes.declareValue(param);       // Declare a parameter
 *   ValueDeclAST* found = scopes.lookupValue(name);  // Searches outward
 *   scopes.pop();                     // Exit function body
 * 
 * ─── Lookup Semantics ─────────────────────────────────────────────────────
 * 
 *   - Searches from innermost (top) scope outward to outermost (bottom)
 *   - Returns the first declaration found
 *   - For overloads, returns the entire overload set from the innermost
 *     scope that contains any overload for that name
 * 
 * ─── Shadowing ────────────────────────────────────────────────────────────
 * 
 *   - Inner scopes can shadow outer declarations (same name)
 *   - Shadowed declarations are not visible in inner scopes
 *   - This is automatically handled by outward search order
 */
class ScopeStack {
public:
    // ─────────────────────────────────────────────────────────────────────────
    // Scope Management
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Pushes a new empty scope onto the stack.
     */
    void push();
    
    /**
     * @brief Pops the top scope from the stack.
     * 
     * The popped scope's contents are discarded (AST nodes remain in arena).
     */
    void pop();
    
    /**
     * @brief Returns the current (innermost) scope.
     */
    Scope& current();
    const Scope& current() const;
    
    /**
     * @brief Returns the current nesting depth (0 = global scope).
     */
    size_t depth() const { return scopes_.size(); }
    
    /**
     * @brief Returns true if the stack is empty (no scopes).
     */
    bool empty() const { return scopes_.empty(); }
    
    // ─────────────────────────────────────────────────────────────────────────
    // Declaration (always in current scope)
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Declares a value in the current scope.
     * 
     * @param decl The value declaration (VarDeclAST, FuncDeclAST, etc.)
     * @return true if declared successfully, false if name already exists
     *         in the current scope's value namespace
     */
    bool declareValue(ValueDeclAST* decl);
    
    /**
     * @brief Declares a type in the current scope.
     * 
     * @param decl The type declaration (StructDeclAST, EnumDeclAST, etc.)
     * @return true if declared successfully, false if name already exists
     *         in the current scope's type namespace
     */
    bool declareType(TypeDeclAST* decl);
    
    /**
     * @brief Declares a function overload in the current scope.
     * 
     * This adds the function to the overload set for its name.
     * If this is the first function with this name, a new overload set is created.
     * If a function with the same signature already exists, returns false.
     * 
     * @param func The function declaration
     * @return true if added successfully, false if duplicate signature
     */
    bool declareOverload(FuncDeclAST* func);
    
    // ─────────────────────────────────────────────────────────────────────────
    // Lookup (searches outward from innermost scope)
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Looks up a value by name.
     * 
     * Searches from innermost to outermost scope.
     * Returns the first declaration found.
     * 
     * @param name The interned name to look up
     * @return ValueDeclAST* The found declaration, or nullptr if not found
     */
    ValueDeclAST* lookupValue(InternedString name);
    const ValueDeclAST* lookupValue(InternedString name) const;
    
    /**
     * @brief Looks up a type by name.
     * 
     * Searches from innermost to outermost scope.
     * Returns the first declaration found.
     * 
     * @param name The interned name to look up
     * @return TypeDeclAST* The found declaration, or nullptr if not found
     */
    TypeDeclAST* lookupType(InternedString name);
    const TypeDeclAST* lookupType(InternedString name) const;
    
    /**
     * @brief Looks up an overload set by function name.
     * 
     * Searches from innermost to outermost scope.
     * Returns the overload set from the first scope that contains any
     * overload for this name.
     * 
     * @param name The interned function name
     * @return std::vector<FuncDeclAST*>* Pointer to overload set, or nullptr
     */
    std::vector<FuncDeclAST*>* lookupOverloads(InternedString name);
    const std::vector<FuncDeclAST*>* lookupOverloads(InternedString name) const;
    
    // ─────────────────────────────────────────────────────────────────────────
    // Local Lookup (current scope only, no outward search)
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Looks up a value in the current scope only.
     * 
     * @param name The interned name to look up
     * @return ValueDeclAST* The found declaration, or nullptr
     */
    ValueDeclAST* lookupLocalValue(InternedString name);
    const ValueDeclAST* lookupLocalValue(InternedString name) const;
    
    /**
     * @brief Looks up a type in the current scope only.
     * 
     * @param name The interned name to look up
     * @return TypeDeclAST* The found declaration, or nullptr
     */
    TypeDeclAST* lookupLocalType(InternedString name);
    const TypeDeclAST* lookupLocalType(InternedString name) const;
    
    // ─────────────────────────────────────────────────────────────────────────
    // Debugging
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Dumps the entire scope stack to the log.
     * 
     * @param pool String pool for name demangling
     */
    void dump(const StringPool& pool) const;
    
    /**
     * @brief Returns a string representation of the entire scope stack.
     * 
     * @param pool String pool for name demangling
     * @return std::string Multi-line dump
     */
    std::string toString(const StringPool& pool) const;
    
    /**
     * @brief Returns the global scope (bottom of stack).
     * 
     * @return const Scope& Reference to global scope (empty if stack empty)
     */
    const Scope& getGlobalScope() const;

private:
    std::deque<Scope> scopes_;
    
    // Helper to find overload set in any scope (used by declareOverload)
    std::vector<FuncDeclAST*>* findOverloadSet(InternedString name);
    const std::vector<FuncDeclAST*>* findOverloadSet(InternedString name) const;
    
    // Helper to check if two function signatures conflict
    bool signaturesEqual(FuncDeclAST* a, FuncDeclAST* b) const;
    bool hasConflictingSignature(FuncDeclAST* func, const std::vector<FuncDeclAST*>& overloads) const;
};
