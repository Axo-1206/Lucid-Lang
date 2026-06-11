/**
 * @file Annotator.hpp
 * @brief Phase 4 annotation pass – marks const expressions and behavior members.
 * 
 * ============================================================================
 * PHASE 4: ANNOTATION
 * ============================================================================
 * 
 * This pass traverses the AST and annotates nodes with additional information
 * that cannot be determined during earlier phases.
 * 
 * ─── Annotations Applied ───────────────────────────────────────────────────
 * 
 *   ExprAST::isConst:
 *     - Marks expressions that are compile-time constants
 *     - Used for const variable initialization validation
 *     - Used for array size expressions
 *     - Used for enum variant values
 * 
 *   ExprAST::isBehaviorMember:
 *     - Marks BehaviorAccessExprAST nodes that are method references
 *     - Used for code generation to distinguish method calls from field access
 * 
 *   ExprAST::resolvedType:
 *     - Already set during Phase 3 (type checking)
 *     - This pass may refine or propagate constness
 * 
 * ─── Propagation Rules ─────────────────────────────────────────────────────
 * 
 *   Constants propagate through expressions:
 *     - Literal expressions are always const
 *     - Binary operations: const if both operands are const
 *     - Unary operations: const if operand is const
 *     - Function calls: const if function is const and arguments are const
 *     - Identifier: const if the referenced declaration is const
 * 
 * ─── Dependencies ─────────────────────────────────────────────────────────
 * 
 *   - TypeResolver: for type information
 *   - ScopeStack: for declaration lookup
 * 
 * @see ExprAST for field definitions
 * @see SemanticAnalyzer::annotate() for pipeline integration
 */

#pragma once

#include "ast/BaseAST.hpp"
#include "ast/DeclAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/StmtAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/scope/ScopeStack.hpp"
#include "semantic/resolver/TypeResolver.hpp"

/**
 * @brief Phase 4 annotator – marks const expressions and behavior members.
 * 
 * This class traverses the entire AST and sets annotation flags on nodes.
 * It is stateless aside from the context reference.
 */
class Annotator {
public:
    explicit Annotator(SemanticContext& ctx);
    
    /**
     * @brief Annotates all ASTs in the given file list.
     * 
     * @param files List of parsed programs to annotate
     */
    void annotateAll(std::vector<ProgramAST*>& files);
    
    /**
     * @brief Annotates a single program AST.
     * 
     * @param program The program to annotate
     */
    void annotateProgram(ProgramAST* program);
    
private:
    SemanticContext& ctx_;
    
    // ========================================================================
    // Expression Annotation
    // ========================================================================
    
    /**
     * @brief Annotates an expression with constness.
     * 
     * @param expr The expression to annotate
     * @return true if the expression is const
     */
    bool annotateExpr(ExprAST* expr);
    
    bool annotateLiteralExpr(LiteralExprAST* expr);
    bool annotateIdentifierExpr(IdentifierExprAST* expr);
    bool annotateBinaryExpr(BinaryExprAST* expr);
    bool annotateUnaryExpr(UnaryExprAST* expr);
    bool annotateCallExpr(CallExprAST* expr);
    bool annotateArrayLiteralExpr(ArrayLiteralExprAST* expr);
    bool annotateStructLiteralExpr(StructLiteralExprAST* expr);
    bool annotateFieldAccessExpr(FieldAccessExprAST* expr);
    bool annotateBehaviorAccessExpr(BehaviorAccessExprAST* expr);
    bool annotateIndexExpr(IndexExprAST* expr);
    bool annotateSliceExpr(SliceExprAST* expr);
    bool annotateAssignExpr(AssignExprAST* expr);
    bool annotateIsExpr(IsExprAST* expr);
    bool annotateNullCoalesceExpr(NullCoalesceExprAST* expr);
    bool annotateNullableChainExpr(NullableChainExprAST* expr);
    bool annotateAnonFuncExpr(AnonFuncExprAST* expr);
    bool annotateAwaitExpr(AwaitExprAST* expr);
    bool annotateRangeExpr(RangeExprAST* expr);
    
    // ========================================================================
    // Statement Annotation
    // ========================================================================
    
    void annotateStmt(StmtAST* stmt);
    void annotateBlockStmt(BlockStmtAST* block);
    void annotateExprStmt(ExprStmtAST* exprStmt);
    void annotateDeclStmt(DeclStmtAST* declStmt);
    void annotateIfStmt(IfStmtAST* ifStmt);
    void annotateSwitchStmt(SwitchStmtAST* switchStmt);
    void annotateForStmt(ForStmtAST* forStmt);
    void annotateWhileStmt(WhileStmtAST* whileStmt);
    void annotateDoWhileStmt(DoWhileStmtAST* doWhileStmt);
    void annotateReturnStmt(ReturnStmtAST* retStmt);
    void annotateMultiVarDecl(MultiVarDeclAST* multiDecl);
    void annotateMultiAssignStmt(MultiAssignStmtAST* multiAssign);
    
    // ========================================================================
    // Declaration Annotation
    // ========================================================================
    
    void annotateDecl(DeclAST* decl);
    void annotateVarDecl(VarDeclAST* var);
    void annotateFuncDecl(FuncDeclAST* func);
    void annotateStructDecl(StructDeclAST* structDecl);
    void annotateEnumDecl(EnumDeclAST* enumDecl);
    void annotateTraitDecl(TraitDeclAST* trait);
    void annotateImplDecl(ImplDeclAST* impl);
    void annotateFromDecl(FromDeclAST* from);
    void annotateTypeAliasDecl(TypeAliasDeclAST* alias);
    
    // ========================================================================
    // Helpers
    // ========================================================================
    
    /**
     * @brief Checks if a value declaration is const (let vs const).
     * 
     * @param decl The value declaration
     * @return true if the declaration is const (keyword == Const)
     */
    bool isConstDecl(ValueDeclAST* decl);
    
    /**
     * @brief Propagates constness from a declaration to its uses.
     * 
     * @param decl The declaration
     * @return true if the declaration's value is const
     */
    bool getDeclConstness(DeclAST* decl);
};

/**
 * @brief Convenience function to annotate all files.
 * 
 * @param files List of parsed programs
 * @param ctx Semantic context
 */
void annotateAll(std::vector<ProgramAST*>& files, SemanticContext& ctx);
