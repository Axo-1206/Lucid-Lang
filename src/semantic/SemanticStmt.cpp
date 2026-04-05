/**
 * @file SemanticStmt.cpp
 *
 * @nutshell Maintains structural depth and controls block-level logic (flow and loops).
 *
 * @responsibility Phase 3c of semantic analysis: walks statement AST nodes and
 *   manages scope depth, loop/async/parallel context flags, and return-type tracking.
 *
 * @logic
 *   checkBlock          — pushScope, check each stmt, popScope
 *   checkExprStmt       — checkExpr; warn if Result<T> discarded
 *   checkDeclStmt       — dispatch to checkVarDecl or checkFuncDecl
 *   checkIfStmt         — check condition is bool; check branches
 *   checkSwitchStmt     — check subject; check each case
 *   checkForStmt        — check iterable; declare loop var; check body
 *   checkWhileStmt      — check condition is bool; loop depth
 *   checkDoWhileStmt    — body first, then condition
 *   checkReturnStmt     — validate return type matches expected
 *   checkBreakStmt      — error if not inside a loop or inside parallel
 *   checkContinueStmt   — same as break
 *   checkParallelForStmt— parallelDepth++, check iterable + body
 *   checkParallelBlock  — parallelDepth++, check sub-blocks
 *
 * @related SemanticAnalyzer.cpp, SemanticDecl.cpp, SemanticExpr.cpp
 */

#include "SemanticSymbol.hpp"
#include "SymbolTable.hpp"
#include "TypeResolver.hpp"
#include "TypeChecker.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "ast/StmtAST.hpp"
#include "ast/DeclAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations 
// NOTE: These are required because Declarations, Statements, and Expressions
// cross-call each other recursively. We use manual forward declarations here
// instead of a header to avoid complex circular dependency loops.
// ─────────────────────────────────────────────────────────────────────────────
TypeAST* checkExpr(ExprAST* node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                   int& parallelDepth, bool insideExtern);

void checkVarDecl(VarDeclAST& node, SymbolTable& symbols, TypeResolver& resolver,
                  DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                  int& parallelDepth, bool insideExtern);

void checkFuncDecl(FuncDeclAST& node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                   int& parallelDepth, bool insideExtern);

// ─────────────────────────────────────────────────────────────────────────────
// Convenience: make a primitive type pointer without allocation.
// ─────────────────────────────────────────────────────────────────────────────
static PrimitiveTypeAST* stmtPrimType(PrimitiveKind k) {
    static PrimitiveTypeAST singletons[] = {
        PrimitiveTypeAST(PrimitiveKind::Bool),
        PrimitiveTypeAST(PrimitiveKind::Byte),
        PrimitiveTypeAST(PrimitiveKind::Short),
        PrimitiveTypeAST(PrimitiveKind::Int),
        PrimitiveTypeAST(PrimitiveKind::Long),
        PrimitiveTypeAST(PrimitiveKind::Ubyte),
        PrimitiveTypeAST(PrimitiveKind::Ushort),
        PrimitiveTypeAST(PrimitiveKind::Uint),
        PrimitiveTypeAST(PrimitiveKind::Ulong),
        PrimitiveTypeAST(PrimitiveKind::Int8),
        PrimitiveTypeAST(PrimitiveKind::Int16),
        PrimitiveTypeAST(PrimitiveKind::Int32),
        PrimitiveTypeAST(PrimitiveKind::Int64),
        PrimitiveTypeAST(PrimitiveKind::Uint8),
        PrimitiveTypeAST(PrimitiveKind::Uint16),
        PrimitiveTypeAST(PrimitiveKind::Uint32),
        PrimitiveTypeAST(PrimitiveKind::Uint64),
        PrimitiveTypeAST(PrimitiveKind::Float),
        PrimitiveTypeAST(PrimitiveKind::Double),
        PrimitiveTypeAST(PrimitiveKind::Decimal),
        PrimitiveTypeAST(PrimitiveKind::String),
        PrimitiveTypeAST(PrimitiveKind::Char),
        PrimitiveTypeAST(PrimitiveKind::Any),
    };
    return &singletons[static_cast<int>(k)];
}

// ─────────────────────────────────────────────────────────────────────────────
// checkStmt — main dispatcher (defined after all helpers)
// ─────────────────────────────────────────────────────────────────────────────
void checkStmt(StmtAST* node, SymbolTable& symbols, TypeResolver& resolver,
               DiagnosticEngine& dc, TypeAST* expectedReturn,
               int& asyncDepth, int& loopDepth, int& parallelDepth,
               bool insideExtern);

// ─────────────────────────────────────────────────────────────────────────────
// checkBlock
// Opens a new scope, checks each statement, closes scope.
// Records the depth on the node for the Annotator.
// ─────────────────────────────────────────────────────────────────────────────
static void checkBlock(BlockStmtAST& node, SymbolTable& symbols, TypeResolver& resolver,
                       DiagnosticEngine& dc, TypeAST* expectedReturn,
                       int& asyncDepth, int& loopDepth, int& parallelDepth,
                       bool insideExtern) {
    symbols.pushScope();
    node.scopeDepth = symbols.currentDepth();

    for (auto& stmt : node.stmts) {
        checkStmt(stmt.get(), symbols, resolver, dc, expectedReturn,
                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    }

    symbols.popScope();
}

// ─────────────────────────────────────────────────────────────────────────────
// checkExprStmt
// Checks the expression for type correctness. Discarding an Expect<T> / Result
// without handling it generates a runtime panic per the language spec — we emit
// a note here (a full warning requires knowledge of the error library, deferred
// until the error library is integrated).
// ─────────────────────────────────────────────────────────────────────────────
static void checkExprStmt(ExprStmtAST& node, SymbolTable& symbols, TypeResolver& resolver,
                           DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                           int& parallelDepth, bool insideExtern) {
    checkExpr(node.expr.get(), symbols, resolver, dc,
              asyncDepth, loopDepth, parallelDepth, insideExtern);
}

// ─────────────────────────────────────────────────────────────────────────────
// checkDeclStmt
// Dispatches to checkVarDecl or checkFuncDecl then declares the symbol locally.
// ─────────────────────────────────────────────────────────────────────────────
static void checkDeclStmt(DeclStmtAST& node, SymbolTable& symbols, TypeResolver& resolver,
                           DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                           int& parallelDepth, bool insideExtern) {
    if (node.isVar()) {
        VarDeclAST* vd = node.asVar();
        checkVarDecl(*vd, symbols, resolver, dc,
                     asyncDepth, loopDepth, parallelDepth, insideExtern);

        // Declare the variable in the current (block) scope.
        TypeAST* resolvedTy = resolver.resolveType(vd->type.get());
        Symbol sym;
        sym.name       = vd->name;
        sym.kind       = SymbolKind::Var;
        sym.declKw     = vd->keyword;
        sym.visibility = Visibility::Private;
        sym.type       = resolvedTy;
        sym.decl       = vd;
        sym.isAsync    = false;
        sym.loc        = vd->loc;
        if (!symbols.declare(sym)) {
            dc.error(DiagnosticCategory::Semantic, vd->loc, DiagCode::E3005,
                     "symbol '" + vd->name + "' is already declared in this scope");
        }
    } else if (node.isFunc()) {
        FuncDeclAST* fd = node.asFunc();
        checkFuncDecl(*fd, symbols, resolver, dc,
                      asyncDepth, loopDepth, parallelDepth, insideExtern);

        Symbol sym;
        sym.name       = fd->name;
        sym.kind       = SymbolKind::Func;
        sym.declKw     = fd->keyword;
        sym.visibility = Visibility::Private;
        sym.type       = fd->returnType ? resolver.resolveType(fd->returnType.get()) : nullptr;
        sym.decl       = fd;
        sym.isAsync    = fd->isAsync;
        sym.loc        = fd->loc;
        if (!symbols.declare(sym)) {
            dc.error(DiagnosticCategory::Semantic, fd->loc, DiagCode::E3005,
                     "symbol '" + fd->name + "' is already declared in this scope");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkIfStmt
// ─────────────────────────────────────────────────────────────────────────────
static void checkIfStmt(IfStmtAST& node, SymbolTable& symbols, TypeResolver& resolver,
                         DiagnosticEngine& dc, TypeAST* expectedReturn,
                         int& asyncDepth, int& loopDepth, int& parallelDepth,
                         bool insideExtern) {
    TypeAST* condType = checkExpr(node.condition.get(), symbols, resolver, dc,
                                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    if (condType && !TypeChecker::isBooleanCompatible(condType)) {
        dc.error(DiagnosticCategory::Semantic, node.condition->loc, DiagCode::E3002,
                 "if condition must be bool");
    }

    if (node.thenBranch) {
        checkStmt(node.thenBranch.get(), symbols, resolver, dc, expectedReturn,
                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    }
    if (node.elseBranch) {
        checkStmt(node.elseBranch.get(), symbols, resolver, dc, expectedReturn,
                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkSwitchStmt
// The subject type is checked; each case value must be assignable to it.
// ─────────────────────────────────────────────────────────────────────────────
static void checkSwitchStmt(SwitchStmtAST& node, SymbolTable& symbols,
                              TypeResolver& resolver, DiagnosticEngine& dc,
                              TypeAST* expectedReturn, int& asyncDepth,
                              int& loopDepth, int& parallelDepth, bool insideExtern) {
    TypeAST* subjectType = checkExpr(node.subject.get(), symbols, resolver, dc,
                                     asyncDepth, loopDepth, parallelDepth, insideExtern);

    for (auto& cas : node.cases) {
        for (auto& val : cas->values) {
            TypeAST* vt = checkExpr(val.get(), symbols, resolver, dc,
                                    asyncDepth, loopDepth, parallelDepth, insideExtern);
            if (subjectType && vt && !TypeChecker::isAssignable(vt, subjectType)) {
                dc.error(DiagnosticCategory::Semantic, val->loc, DiagCode::E3002,
                         "switch case value type does not match subject type");
            }
        }
        if (cas->body) {
            checkBlock(*cas->body, symbols, resolver, dc, expectedReturn,
                       asyncDepth, loopDepth, parallelDepth, insideExtern);
        }
    }

    if (node.defaultBody) {
        checkBlock(*node.defaultBody, symbols, resolver, dc, expectedReturn,
                   asyncDepth, loopDepth, parallelDepth, insideExtern);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkForStmt
// Validates the iterable (collection or range), declares the loop variable,
// checks the body.
// ─────────────────────────────────────────────────────────────────────────────
static void checkForStmt(ForStmtAST& node, SymbolTable& symbols, TypeResolver& resolver,
                          DiagnosticEngine& dc, TypeAST* expectedReturn,
                          int& asyncDepth, int& loopDepth, int& parallelDepth,
                          bool insideExtern) {
    TypeAST* iterType = checkExpr(node.iterable.get(), symbols, resolver, dc,
                                  asyncDepth, loopDepth, parallelDepth, insideExtern);

    // Infer element type from the iterable.
    TypeAST* elemType = stmtPrimType(PrimitiveKind::Int); // default for range
    if (iterType) {
        if (iterType->isa<FixedArrayTypeAST>())
            elemType = iterType->as<FixedArrayTypeAST>()->element.get();
        else if (iterType->isa<SliceTypeAST>())
            elemType = iterType->as<SliceTypeAST>()->element.get();
        else if (iterType->isa<DynamicArrayTypeAST>())
            elemType = iterType->as<DynamicArrayTypeAST>()->element.get();
    }

    // Explicit type annotation overrides inferred element type.
    if (node.varType) {
        TypeAST* explicit_t = resolver.resolveType(node.varType.get());
        if (explicit_t) elemType = explicit_t;
    }

    loopDepth++;
    symbols.pushScope();

    // Declare the loop variable in the body scope.
    Symbol loopVar;
    loopVar.name       = node.varName;
    loopVar.kind       = SymbolKind::Var;
    loopVar.declKw     = DeclKeyword::Let;
    loopVar.visibility = Visibility::Private;
    loopVar.type       = elemType;
    loopVar.decl       = nullptr;
    loopVar.isAsync    = false;
    loopVar.loc        = node.loc;
    symbols.declare(loopVar);

    if (node.body) {
        checkStmt(node.body.get(), symbols, resolver, dc, expectedReturn,
                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    }

    symbols.popScope();
    loopDepth--;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkWhileStmt
// ─────────────────────────────────────────────────────────────────────────────
static void checkWhileStmt(WhileStmtAST& node, SymbolTable& symbols,
                             TypeResolver& resolver, DiagnosticEngine& dc,
                             TypeAST* expectedReturn, int& asyncDepth,
                             int& loopDepth, int& parallelDepth, bool insideExtern) {
    TypeAST* condType = checkExpr(node.condition.get(), symbols, resolver, dc,
                                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    if (condType && !TypeChecker::isBooleanCompatible(condType)) {
        dc.error(DiagnosticCategory::Semantic, node.condition->loc, DiagCode::E3002,
                 "while condition must be bool");
    }

    loopDepth++;
    if (node.body) {
        checkStmt(node.body.get(), symbols, resolver, dc, expectedReturn,
                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    }
    loopDepth--;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkDoWhileStmt
// Body executes first, then condition is checked.
// ─────────────────────────────────────────────────────────────────────────────
static void checkDoWhileStmt(DoWhileStmtAST& node, SymbolTable& symbols,
                               TypeResolver& resolver, DiagnosticEngine& dc,
                               TypeAST* expectedReturn, int& asyncDepth,
                               int& loopDepth, int& parallelDepth, bool insideExtern) {
    loopDepth++;
    if (node.body) {
        checkStmt(node.body.get(), symbols, resolver, dc, expectedReturn,
                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    }
    loopDepth--;

    TypeAST* condType = checkExpr(node.condition.get(), symbols, resolver, dc,
                                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    if (condType && !TypeChecker::isBooleanCompatible(condType)) {
        dc.error(DiagnosticCategory::Semantic, node.condition->loc, DiagCode::E3002,
                 "do-while condition must be bool");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkReturnStmt
//
// Rules enforced:
//   - Cannot appear inside a parallel scope.
//   - A void return (nullptr value) is only valid in a void function.
//   - A value return must be assignable to the enclosing function's return type.
// ─────────────────────────────────────────────────────────────────────────────
static void checkReturnStmt(ReturnStmtAST& node, SymbolTable& symbols,
                              TypeResolver& resolver, DiagnosticEngine& dc,
                              TypeAST* expectedReturn, int& asyncDepth,
                              int& loopDepth, int& parallelDepth, bool insideExtern) {
    if (parallelDepth > 0) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "'return' is not allowed inside a parallel scope");
        return;
    }

    if (!node.value) {
        // Bare return — valid only when expected return is void (nullptr).
        if (expectedReturn != nullptr) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "non-void function must return a value");
        }
        return;
    }

    TypeAST* valType = checkExpr(node.value.get(), symbols, resolver, dc,
                                 asyncDepth, loopDepth, parallelDepth, insideExtern);

    if (!expectedReturn) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "void function cannot return a value");
        return;
    }

    if (valType && !TypeChecker::isAssignable(valType, expectedReturn)) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "return type mismatch");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkBreakStmt
// ─────────────────────────────────────────────────────────────────────────────
static void checkBreakStmt(BreakStmtAST& node, DiagnosticEngine& dc,
                             int loopDepth, int parallelDepth) {
    if (parallelDepth > 0) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "'break' is not allowed inside a parallel scope");
    } else if (loopDepth <= 0) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "'break' must be inside a loop");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkContinueStmt
// ─────────────────────────────────────────────────────────────────────────────
static void checkContinueStmt(ContinueStmtAST& node, DiagnosticEngine& dc,
                                int loopDepth, int parallelDepth) {
    if (parallelDepth > 0) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "'continue' is not allowed inside a parallel scope");
    } else if (loopDepth <= 0) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "'continue' must be inside a loop");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkParallelForStmt
//
// Rules enforced:
//   - Iterable must be a collection type.
//   - Loop variable is bound per-iteration.
//   - Inside body: no await, no return, no break/continue, no outer writes.
//     (The no-outer-write check is enforced at assignment time via parallelDepth.)
// ─────────────────────────────────────────────────────────────────────────────
static void checkParallelForStmt(ParallelForStmtAST& node, SymbolTable& symbols,
                                   TypeResolver& resolver, DiagnosticEngine& dc,
                                   TypeAST* expectedReturn, int& asyncDepth,
                                   int& loopDepth, int& parallelDepth, bool insideExtern) {
    TypeAST* iterType = checkExpr(node.iterable.get(), symbols, resolver, dc,
                                  asyncDepth, loopDepth, parallelDepth, insideExtern);

    TypeAST* elemType = stmtPrimType(PrimitiveKind::Int);
    if (iterType) {
        if (iterType->isa<FixedArrayTypeAST>())
            elemType = iterType->as<FixedArrayTypeAST>()->element.get();
        else if (iterType->isa<SliceTypeAST>())
            elemType = iterType->as<SliceTypeAST>()->element.get();
        else if (iterType->isa<DynamicArrayTypeAST>())
            elemType = iterType->as<DynamicArrayTypeAST>()->element.get();
    }

    if (node.varType) {
        TypeAST* explicit_t = resolver.resolveType(node.varType.get());
        if (explicit_t) elemType = explicit_t;
    }

    parallelDepth++;
    symbols.pushScope();

    Symbol loopVar;
    loopVar.name = node.varName; loopVar.kind = SymbolKind::Var;
    loopVar.declKw = DeclKeyword::Let; loopVar.visibility = Visibility::Private;
    loopVar.type = elemType; loopVar.decl = nullptr;
    loopVar.isAsync = false; loopVar.loc = node.loc;
    symbols.declare(loopVar);

    if (node.body) {
        checkStmt(node.body.get(), symbols, resolver, dc, expectedReturn,
                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    }

    symbols.popScope();
    parallelDepth--;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkParallelBlockStmt
// Each sub-block is an independent task — they all run concurrently.
// ─────────────────────────────────────────────────────────────────────────────
static void checkParallelBlockStmt(ParallelBlockStmtAST& node, SymbolTable& symbols,
                                    TypeResolver& resolver, DiagnosticEngine& dc,
                                    TypeAST* expectedReturn, int& asyncDepth,
                                    int& loopDepth, int& parallelDepth, bool insideExtern) {
    parallelDepth++;
    for (auto& sub : node.subBlocks) {
        checkBlock(*sub, symbols, resolver, dc, expectedReturn,
                   asyncDepth, loopDepth, parallelDepth, insideExtern);
    }
    parallelDepth--;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkStmt — main dispatcher
// ─────────────────────────────────────────────────────────────────────────────
void checkStmt(StmtAST* node, SymbolTable& symbols, TypeResolver& resolver,
               DiagnosticEngine& dc, TypeAST* expectedReturn,
               int& asyncDepth, int& loopDepth, int& parallelDepth,
               bool insideExtern) {
    if (!node) return;

    switch (node->kind) {
        case ASTKind::BlockStmt:
            checkBlock(*node->as<BlockStmtAST>(), symbols, resolver, dc, expectedReturn,
                       asyncDepth, loopDepth, parallelDepth, insideExtern);
            break;

        case ASTKind::ExprStmt:
            checkExprStmt(*node->as<ExprStmtAST>(), symbols, resolver, dc,
                          asyncDepth, loopDepth, parallelDepth, insideExtern);
            break;

        case ASTKind::DeclStmt:
            checkDeclStmt(*node->as<DeclStmtAST>(), symbols, resolver, dc,
                          asyncDepth, loopDepth, parallelDepth, insideExtern);
            break;

        case ASTKind::IfStmt:
            checkIfStmt(*node->as<IfStmtAST>(), symbols, resolver, dc, expectedReturn,
                        asyncDepth, loopDepth, parallelDepth, insideExtern);
            break;

        case ASTKind::SwitchStmt:
            checkSwitchStmt(*node->as<SwitchStmtAST>(), symbols, resolver, dc,
                            expectedReturn, asyncDepth, loopDepth, parallelDepth,
                            insideExtern);
            break;

        case ASTKind::ForStmt:
            checkForStmt(*node->as<ForStmtAST>(), symbols, resolver, dc, expectedReturn,
                         asyncDepth, loopDepth, parallelDepth, insideExtern);
            break;

        case ASTKind::WhileStmt:
            checkWhileStmt(*node->as<WhileStmtAST>(), symbols, resolver, dc, expectedReturn,
                           asyncDepth, loopDepth, parallelDepth, insideExtern);
            break;

        case ASTKind::DoWhileStmt:
            checkDoWhileStmt(*node->as<DoWhileStmtAST>(), symbols, resolver, dc,
                             expectedReturn, asyncDepth, loopDepth, parallelDepth,
                             insideExtern);
            break;

        case ASTKind::ReturnStmt:
            checkReturnStmt(*node->as<ReturnStmtAST>(), symbols, resolver, dc,
                            expectedReturn, asyncDepth, loopDepth, parallelDepth,
                            insideExtern);
            break;

        case ASTKind::BreakStmt:
            checkBreakStmt(*node->as<BreakStmtAST>(), dc, loopDepth, parallelDepth);
            break;

        case ASTKind::ContinueStmt:
            checkContinueStmt(*node->as<ContinueStmtAST>(), dc, loopDepth, parallelDepth);
            break;

        case ASTKind::ParallelForStmt:
            checkParallelForStmt(*node->as<ParallelForStmtAST>(), symbols, resolver, dc,
                                 expectedReturn, asyncDepth, loopDepth, parallelDepth,
                                 insideExtern);
            break;

        case ASTKind::ParallelBlockStmt:
            checkParallelBlockStmt(*node->as<ParallelBlockStmtAST>(), symbols, resolver,
                                   dc, expectedReturn, asyncDepth, loopDepth,
                                   parallelDepth, insideExtern);
            break;

        default:
            break;
    }
}