/**
 * @file DeclarationCollector.hpp
 * @brief Phase 1 collector – registers declarations in scopes.
 * 
 * ============================================================================
 * PHASE 1: DECLARATION COLLECTION
 * ============================================================================
 * 
 * This pass walks all ASTs and registers declarations in the ScopeStack.
 * 
 * ─── Registration Rules ───────────────────────────────────────────────────
 * 
 *   Value namespace (ValueDeclAST):
 *     - VarDeclAST (variables)
 *     - FuncDeclAST (functions)
 *     - ParamAST (function parameters)
 *     - FieldDeclAST (struct fields)
 *     - MethodDeclAST (methods in impl blocks)
 *     - EnumVariantAST (enum variants)
 * 
 *   Type namespace (TypeDeclAST):
 *     - StructDeclAST
 *     - EnumDeclAST
 *     - TraitDeclAST
 *     - TypeAliasDeclAST
 * 
 *   Not registered (handled separately):
 *     - PackageDeclAST (parser-only, not a lookup target)
 *     - UseDeclAST (imports, handled by module loader)
 *     - ImplDeclAST (stored in list for conformance checking)
 *     - FromDeclAST (stored in list for conversion resolution)
 * 
 * ─── Scoping ──────────────────────────────────────────────────────────────
 * 
 *   - Global scope: top-level declarations
 *   - Struct scope: field names (pushed when entering struct, popped when done)
 *   - Function scope: parameter names (pushed when entering function)
 *   - Block scope: local declarations (handled by checkers, not collector)
 * 
 *   The collector only handles declarations that introduce names into scopes.
 *   It does NOT traverse into statement bodies (that's Phase 3).
 * 
 * ─── Duplicate Detection ──────────────────────────────────────────────────
 * 
 *   - Per-scope duplicates are caught by ScopeStack::declareValue/Type
 *   - Cross-file duplicates in global scope are caught by
 *     SemanticAnalyzer::validateNoDuplicateDeclarations()
 *   - UseDecl duplicates are detected here (per-file)
 * 
 * @see ScopeStack for scope management
 * @see SemanticAnalyzer::analyze() for pipeline integration
 */

#pragma once

#include "ast/BaseAST.hpp"
#include "ast/DeclAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "../scope/ScopeStack.hpp"
#include "semantic/helpers/SemanticContext.hpp"

#include <unordered_set>
#include <vector>

namespace luc {

/**
 * @brief Phase 1 collector – registers declarations in scopes.
 * 
 * This collector is called once per ProgramAST during Phase 1.
 * It traverses declarations and registers them in the appropriate
 * namespaces (value or type) in the current scope.
 * 
 * ─── Collection Order ─────────────────────────────────────────────────────
 * 
 *   1. Push global scope (already done by SemanticAnalyzer)
 *   2. For each top-level declaration:
 *      - Register in appropriate namespace
 *      - For structs: push scope, register fields, pop scope
 *      - For functions: push scope, register parameters, pop scope
 *      - For impl/from: store in lists (not registered)
 *   3. Pop global scope (after all files processed)
 * 
 * @note This collector does NOT traverse into statement bodies.
 *       Only declarations that introduce names are processed.
 */
class DeclarationCollector {
public:
    explicit DeclarationCollector(SemanticContext& ctx);
    
    /**
     * @brief Collects all declarations from a single ProgramAST.
     * 
     * @param program The AST to process
     */
    void collectProgram(ProgramAST* program);
    
    /**
     * @brief Returns the list of ImplDeclAST nodes collected.
     * 
     * Used by SemanticAnalyzer to build trait conformance map.
     */
    const std::vector<ImplDeclAST*>& getImpls() const { return impls_; }
    
    /**
     * @brief Returns the list of FromDeclAST nodes collected.
     * 
     * Used by SemanticAnalyzer for conversion resolution.
     */
    const std::vector<FromDeclAST*>& getFroms() const { return froms_; }
    
    /**
     * @brief Clears all collected lists (called before Phase 1).
     */
    void clear() {
        impls_.clear();
        froms_.clear();
        processedFiles_.clear();
    }

private:
    SemanticContext& ctx_;
    
    // Collected lists for non-registered declarations
    std::vector<ImplDeclAST*> impls_;
    std::vector<FromDeclAST*> froms_;
    
    // Track processed files to avoid duplicate processing
    std::unordered_set<std::string> processedFiles_;
    
    // UseDecl duplicate detection (per-file)
    std::unordered_map<InternedString, std::unordered_set<std::string>> fileImports_;
    
    // ─────────────────────────────────────────────────────────────────────────
    // Declaration Handlers
    // ─────────────────────────────────────────────────────────────────────────
    
    void collectUseDecl(UseDeclAST* use);
    void collectVarDecl(VarDeclAST* var);
    void collectFuncDecl(FuncDeclAST* func);
    void collectStructDecl(StructDeclAST* structDecl);
    void collectEnumDecl(EnumDeclAST* enumDecl);
    void collectTraitDecl(TraitDeclAST* trait);
    void collectImplDecl(ImplDeclAST* impl);
    void collectFromDecl(FromDeclAST* from);
    void collectTypeAliasDecl(TypeAliasDeclAST* alias);
    
    // ─────────────────────────────────────────────────────────────────────────
    // Nested Scope Handlers
    // ─────────────────────────────────────────────────────────────────────────
    
    /**
     * @brief Processes struct fields within the struct's own scope.
     * 
     * Pushes a new scope, registers all fields, then pops.
     * This allows field names to be resolved during type checking.
     */
    void collectStructFields(StructDeclAST* structDecl);
    
    /**
     * @brief Processes function parameters within the function's own scope.
     * 
     * Pushes a new scope, registers all parameters, then pops.
     * This allows parameters to be resolved in the function body.
     */
    void collectFunctionParams(FuncDeclAST* func);
    
    /**
     * @brief Processes enum variants within the enum's own scope.
     * 
     * Registers variants in the enum's scope (not in global scope).
     * This allows `EnumName.Variant` access.
     */
    void collectEnumVariants(EnumDeclAST* enumDecl);
    
    // ─────────────────────────────────────────────────────────────────────────
    // Helpers
    // ─────────────────────────────────────────────────────────────────────────
    
    void checkDuplicateUse(InternedString fileName, const std::string& path, const SourceLocation& loc);
    
    /**
     * @brief Creates a self-type reference for a type declaration.
     * 
     * The self-type is a NamedTypeAST that represents the type itself.
     * It's used when the type name appears as a value (e.g., `int("42")`).
     */
    void ensureSelfType(TypeDeclAST* typeDecl);
};

} // namespace luc