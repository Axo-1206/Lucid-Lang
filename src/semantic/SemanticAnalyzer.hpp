/**
 * @file SemanticAnalyzer.hpp
 * @brief Orchestrates the four passes of semantic analysis.
 * 
 * ============================================================================
 * SEMANTIC ANALYSIS PIPELINE
 * ============================================================================
 * 
 * The semantic analyzer runs a multi-phase pipeline over parsed ASTs:
 * 
 *   Phase 1: Declaration Collection
 *     - Registers all declarations in ScopeStack (values and types)
 *     - Detects duplicate `use` declarations within the same file
 *     - No type resolution yet – just name registration
 * 
 *   Phase 1.5: Duplicate Detection
 *     - Checks for duplicate declarations across files in global scope
 *     - Values and types are checked separately (separate namespaces)
 * 
 *   Phase 2: Type Resolution
 *     - Resolves all type annotations (VarDeclAST::type, FuncDeclAST::funcType, etc.)
 *     - Unwraps type alias chains eagerly (caches on TypeAliasDeclAST)
 *     - Creates self-type references for structs/enums/traits
 * 
 *   Phase 2.5: Trait Conformance Map
 *     - Builds map from type → list of traits it implements
 *     - Used for generic constraint checking
 * 
 *   Phase 3: Declaration Checking
 *     - Validates function bodies, variable initializers, etc.
 *     - Checks type compatibility, control flow, and semantics
 *     - Uses ExprChecker, StmtChecker, and DeclChecker
 * 
 *   Phase 3.5: Entry Point Validation
 *     - Validates 'main' function signature
 *     - Sets compilation mode (AOT/JIT) based on attributes
 * 
 *   Phase 4: Annotation
 *     - Marks expressions as const (isConst) where possible
 *     - Marks behavior access expressions (isBehaviorMember)
 * 
 * ─── Component Ownership ────────────────────────────────────────────────────
 * 
 *   SemanticAnalyzer owns all components in dependency order:
 *     1. ScopeStack        – for name resolution
 *     2. SemanticContext   – shared state (references ScopeStack)
 *     3. TypeResolver      – type resolution (references SemanticContext)
 *     4. DeclarationCollector – Phase 1 collector (references SemanticContext)
 * 
 *   All checkers are stateless functions in the luc::checker namespace.
 * 
 * ─── Usage Example ─────────────────────────────────────────────────────────
 * 
 *   SemanticAnalyzer analyzer(pool, arena);
 *   std::vector<ProgramAST*> files = { program1, program2 };
 *   if (analyzer.analyze(files)) {
 *       // Semantic analysis succeeded
 *       CompilationMode mode = analyzer.getCompilationMode();
 *   }
 * 
 * @see ScopeStack for scope management
 * @see TypeResolver for type resolution
 * @see DeclarationCollector for Phase 1
 * @see luc::checker for statement/expression/declaration checkers
 */

#pragma once

#include "helpers/SemanticContext.hpp"
#include "collector/DeclarationCollector.hpp"
#include "resolver/TypeResolver.hpp"
#include "scope/ScopeStack.hpp"
#include "ast/BaseAST.hpp"
#include "ast/DeclAST.hpp"
#include "ast/BaseAST.hpp"

#include <vector>

/**
 * @brief Compilation mode determined by @aot/@jit attributes on main.
 * 
 *   - AOT (Ahead-Of-Time): Compile to native executable
 *   - JIT (Just-In-Time):  Generate code that can be JIT-compiled at runtime
 */
enum class CompilationMode {
    AOT,    ///< Ahead-Of-Time compilation (default)
    JIT     ///< Just-In-Time compilation
};

/**
 * @brief Main orchestrator for semantic analysis.
 * 
 * This class owns all components needed for semantic analysis and runs
 * the complete pipeline in the correct order. It is stateless aside from
 * the components it owns – all analysis state is in SemanticContext.
 * 
 * ─── Error Handling ────────────────────────────────────────────────────────
 * 
 *   All errors and warnings are reported through the diagnostic system
 *   (global diagnostic module). The analyze() method returns false if any
 *   errors were reported during any phase.
 * 
 * ─── Thread Safety ─────────────────────────────────────────────────────────
 * 
 *   Not thread-safe – designed for single-threaded compilation.
 * 
 * @see SemanticContext for shared analysis state
 * @see ScopeStack for name resolution
 * @see TypeResolver for type resolution
 */
class SemanticAnalyzer {
public:
    /**
     * @brief Constructs a SemanticAnalyzer with required resources.
     * 
     * @param pool String pool for string interning
     * @param arena AST arena for allocating temporary types
     */
    explicit SemanticAnalyzer(StringPool& pool, ASTArena& arena);
    
    /**
     * @brief Runs the complete semantic analysis pipeline.
     * 
     * @param files All parsed ASTs (root file + all imports)
     * @return true if analysis succeeded with no errors
     */
    bool analyze(std::vector<ProgramAST*>& files);
    
    /**
     * @brief Returns the compilation mode determined from @aot/@jit attributes.
     * 
     * Only valid after analyze() has completed successfully.
     * 
     * @return CompilationMode AOT or JIT
     */
    CompilationMode getCompilationMode() const { return compilationMode_; }

private:
    // ========================================================================
    // Owned Components (order matters for initialization)
    // ========================================================================
    
    ScopeStack scope_;              ///< Scope stack for name resolution (must be first)
    SemanticContext ctx_;           ///< Shared analysis state (references scope_)
    TypeResolver typeResolver_;     ///< Type resolution engine (references ctx_)
    DeclarationCollector collector_; ///< Phase 1 declaration collector (references ctx_)
    
    // ========================================================================
    // Analysis Result
    // ========================================================================
    
    CompilationMode compilationMode_ = CompilationMode::AOT;  ///< Determined from @aot/@jit
    
    // ========================================================================
    // Phase Methods
    // ========================================================================
    
    /**
     * @brief Phase 1: Collects all declarations into scopes.
     * 
     * @param files All parsed ASTs
     */
    void collectDeclarations(std::vector<ProgramAST*>& files);
    
    /**
     * @brief Phase 2: Resolves all type annotations.
     * 
     * @param files All parsed ASTs
     */
    void resolveTypes(std::vector<ProgramAST*>& files);
    
    /**
     * @brief Phase 2.5: Builds type → traits conformance map.
     */
    void buildTraitConformanceMap();
    
    /**
     * @brief Phase 3: Checks all declarations semantically.
     * 
     * @param files All parsed ASTs
     */
    void checkDeclarations(std::vector<ProgramAST*>& files);
    
    /**
     * @brief Phase 3.5: Validates the 'main' entry point.
     */
    void validateEntryPoint();
    
    /**
     * @brief Phase 4: Annotates AST with constness and behavior flags.
     * 
     * @param files All parsed ASTs
     */
    void annotate(std::vector<ProgramAST*>& files);
    
    // ========================================================================
    // Helper Methods
    // ========================================================================
    
    /**
     * @brief Validates no duplicate declarations across files in global scope.
     */
    void validateNoDuplicateDeclarations();
    
    /**
     * @brief Collects use declarations for duplicate detection within a file.
     * 
     * @param prog The program AST to scan for use declarations
     */
    void collectUseDeclarations(ProgramAST* prog);
};