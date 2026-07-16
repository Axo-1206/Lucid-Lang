#pragma once
#include "Sema.hpp"
#include "SemaContext.hpp"

class NameResolver {
public:
    static bool resolve(SemaContext& ctx);
    
private:
    static void resolveDecl(DeclAST* decl, SemaContext& ctx);
    static void resolveStmt(StmtAST* stmt, SemaContext& ctx);
    static void resolveExpr(ExprAST* expr, SemaContext& ctx);
    static void resolveType(TypeAST* type, SemaContext& ctx);
};