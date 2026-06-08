/**
 * @file SemanticAnalyzer.hpp
 * @brief Orchestrates the four passes of semantic analysis.
 * 
 * ============================================================================
 * SEMANTIC ANALYSIS PIPELINE
 * ============================================================================
 * 
 * Phase 1: Declaration Collection
 *   - Walks all ASTs and registers declarations in scopes
 *   - Value declarations (VarDeclAST, FuncDeclAST, etc.) go to value namespace
 *   - Type declarations (StructDeclAST, EnumDeclAST, etc.) go to type namespace
 *   - Detects duplicate `use` declarations within the same file
 * 
 * Phase 2: Type Resolution
 *   - Resolves all type annotations (VarDeclAST::type, FuncDeclAST::funcType, etc.)
 *   - Unwraps type alias chains eagerly (caches result on TypeAliasDeclAST)
 *   - Creates self-type references for structs/enums/traits
 *   - Builds trait conformance map (type → traits)
 * 
 * Phase 3: Declaration Checking
 *   - Validates function bodies, variable initializers, etc.
 *   - Checks type compatibility, control flow, and semantics
 *   - Resolves identifiers using the current scope
 * 
 * Phase 4: Annotation
 *   - Marks expressions as const (isConst) where possible
 *   - Marks behavior access expressions (isBehaviorMember)
 * 
 * All phases share a SemanticContext that provides:
 *   - ScopeStack for name resolution
 *   - TypeResolver for type operations
 *   - Error reporting helpers
 * 
 * @see ScopeStack for scope management
 * @see TypeResolver for type resolution
 * @see DeclarationCollector for Phase 1
 */

#pragma once

#include "helpers/SemanticContext.hpp"
#include "collector/DeclarationCollector.hpp"
#include "resolver/TypeResolver.hpp"
#include "scope/ScopeStack.hpp"
#include "ast/BaseAST.hpp"
#include "ast/DeclAST.hpp"

#include <vector>

namespace luc {

enum class CompilationMode { AOT, JIT };

/**
 * @brief Main orchestrator for semantic analysis.
 * 
 * Owns all components in dependency order:
 *   1. ScopeStack – for name resolution
 *   2. SemanticContext – shared state (references ScopeStack)
 *   3. TypeResolver – type resolution (references SemanticContext)
 *   4. DeclarationCollector – Phase 1 collector (references SemanticContext)
 * 
 * The analyze() method runs the complete pipeline and returns success/failure.
 */
class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(StringPool& pool, ASTArena& arena);
    
    /**
     * @brief Runs the complete semantic analysis pipeline.
     * 
     * @param files All parsed ASTs (root file + all imports)
     * @return true if analysis succeeded with no errors
     */
    bool analyze(std::vector<ProgramAST*>& files);
    
    CompilationMode getCompilationMode() const { return compilationMode_; }

private:
    // Owned components (order matters for initialization)
    ScopeStack scope_;              // Must be first – used by ctx_
    SemanticContext ctx_;           // References scope_
    TypeResolver typeResolver_;     // References ctx_
    DeclarationCollector collector_; // References ctx_
    
    CompilationMode compilationMode_ = CompilationMode::AOT;
    
    // Phase methods
    void collectDeclarations(std::vector<ProgramAST*>& files);
    void resolveTypes(std::vector<ProgramAST*>& files);
    void buildTraitConformanceMap();
    void checkDeclarations(std::vector<ProgramAST*>& files);
    void validateEntryPoint();
    void annotate(std::vector<ProgramAST*>& files);
    
    // Helpers
    void validateNoDuplicateDeclarations();
    void collectUseDeclarations(ProgramAST* prog);
};

} // namespace luc