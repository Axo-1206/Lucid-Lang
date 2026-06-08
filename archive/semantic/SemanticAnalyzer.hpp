/**
 * @file SemanticAnalyzer.hpp
 * @brief Orchestrates the four passes of semantic analysis.
 */

#pragma once

#include "helpers/SemanticContext.hpp"
#include "collectors/SemanticCollector.hpp"
#include "resolveType/TypeDispatcher.hpp"
#include "SymbolTable.hpp"
#include "ast/BaseAST.hpp"

#include <memory>
#include <vector>

enum class CompilationMode { AOT, JIT };

class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(StringPool& pool, ASTArena& arena);

    bool analyze(std::vector<ProgramAST*>& files);
    CompilationMode getCompilationMode() const { return compilationMode_; }

private:
    // Owned components (order matters: ctx_ must be initialized before resolver_)
    SymbolTable symbols_;
    SemanticContext ctx_;        // ctx_ must come BEFORE resolver_
    TypeDispatcher dispatcher_;
    SemanticCollector collector_;

    // Result of analysis
    CompilationMode compilationMode_ = CompilationMode::AOT;

    // Phase methods
    void resolveImports(std::vector<ProgramAST*>& files);
    void collectSymbols(std::vector<ProgramAST*>& files);
    void resolveTypes(std::vector<ProgramAST*>& files);
    void checkDecls(std::vector<ProgramAST*>& files);
    void annotate(std::vector<ProgramAST*>& files);
    
    void validateNoDuplicateSymbols();
    void validateEntryPoint();
    void buildTraitConformanceMap();
};