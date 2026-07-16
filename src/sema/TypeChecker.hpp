#pragma once
#include "Sema.hpp"
#include "SemaContext.hpp"

class TypeChecker {
public:
    static bool check(SemaContext& ctx);
    
private:
    static void checkDecl(DeclAST* decl, SemaContext& ctx);
    static void checkStmt(StmtAST* stmt, SemaContext& ctx);
    static TypeAST* checkExpr(ExprAST* expr, SemaContext& ctx);
    static void checkType(TypeAST* type, SemaContext& ctx);
};