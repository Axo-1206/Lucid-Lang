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

#include "SemanticHelpers.hpp"

#include <iostream>
#include <iterator>


// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations 
// NOTE: These are required because Declarations, Statements, and Expressions
// cross-call each other recursively. We use manual forward declarations here
// instead of a header to avoid complex circular dependency loops.
// ─────────────────────────────────────────────────────────────────────────────
TypeAST* checkExpr(ExprAST* node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& loopDepth,
                   int& parallelDepth, bool insideExtern);

void checkVarDecl(VarDeclAST& node, SymbolTable& symbols, TypeResolver& resolver,
                  DiagnosticEngine& dc, int& loopDepth, int& parallelDepth, bool insideExtern);

void checkFuncDecl(FuncDeclAST& node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& loopDepth, int& parallelDepth, bool insideExtern);

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

// Build a function signature for local function declarations in statement scope.
// This mirrors SemanticCollector's curry-chain shape so local calls type-check
// the same way as top-level functions.
static FuncTypeAST buildLocalFuncSignature(FuncDeclAST& node) {
    LUC_LOG_SEMANTIC_VERBOSE("buildLocalFuncSignature: for function '" << node.name 
                           << "', paramGroups=" << node.type.paramGroups.size());
    
    FuncTypeAST result;
    result.loc = node.loc;
    result.rawQualifiers = node.type.rawQualifiers;
    result.qualifiers = node.type.qualifiers;
    result.isNullable = node.type.isNullable;
    
    TypePtr sig = nullptr;
    // Build from the LAST parameter group to the FIRST (creates curry chain)
    for (int i = static_cast<int>(node.type.paramGroups.size()) - 1; i >= 0; --i) {
        auto ft = std::make_unique<FuncTypeAST>();
        ft->loc = node.loc;
        LUC_LOG_SEMANTIC_EXTREME("\tbuilding param group " << i 
                               << " with " << node.type.paramGroups[i].size() << " params");
        
        // Build a ParamGroup for this function level (for curry chain)
        ParamGroup group;
        for (const auto& param : node.type.paramGroups[i]) {
            // ParamInfo needs name, type, isVariadic, loc
            group.emplace_back(param.name, 
                               SemanticHelpers::cloneType(param.type.get()),
                               param.isVariadic,
                               param.loc);
        }
        ft->paramGroups.push_back(std::move(group));

        if (sig) {
            ft->returnType = std::move(sig);
        } else if (node.type.returnType) {
            ft->returnType = SemanticHelpers::cloneType(node.type.returnType.get());
        }
        sig = std::move(ft);
    }
    
    if (sig) {
        // Copy the built signature into result
        if (sig->isa<FuncTypeAST>()) {
            auto* builtSig = sig->as<FuncTypeAST>();
            result.paramGroups = std::move(builtSig->paramGroups);
            result.returnType = SemanticHelpers::cloneType(builtSig->returnType.get());
            result.qualifiers = builtSig->qualifiers;
            result.isNullable = builtSig->isNullable;
        }
    }
    
    LUC_LOG_SEMANTIC_EXTREME("\tsignature built");
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkStmt — main dispatcher (defined after all helpers)
// ─────────────────────────────────────────────────────────────────────────────
void checkStmt(StmtAST* node, SymbolTable& symbols, TypeResolver& resolver,
               DiagnosticEngine& dc, TypeAST* expectedReturn,
               int& loopDepth, int& parallelDepth, bool insideExtern);

// ─────────────────────────────────────────────────────────────────────────────
// checkBlock
// Opens a new scope, checks each statement, closes scope.
// Records the depth on the node for the Annotator.
// ─────────────────────────────────────────────────────────────────────────────
static void checkBlock(BlockStmtAST& node, SymbolTable& symbols, TypeResolver& resolver,
                       DiagnosticEngine& dc, TypeAST* expectedReturn,
                       int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkBlock: entering block, depth=" << symbols.currentDepth());
    
    symbols.pushScope();
    node.scopeDepth = symbols.currentDepth();
    LUC_LOG_SEMANTIC_EXTREME("\tblock depth set to " << node.scopeDepth);

    int stmtCount = 0;
    for (auto& stmt : node.stmts) {
        stmtCount++;
        LUC_LOG_SEMANTIC_EXTREME("\tchecking stmt #" << stmtCount);
        checkStmt(stmt.get(), symbols, resolver, dc, expectedReturn,
                  loopDepth, parallelDepth, insideExtern);
    }

    symbols.popScope();
    LUC_LOG_SEMANTIC_VERBOSE("checkBlock: exited block, checked " << stmtCount << " statements");
}

// ─────────────────────────────────────────────────────────────────────────────
// checkExprStmt
// Checks the expression for type correctness. Discarding an Expect<T> / Result
// without handling it generates a runtime panic per the language spec — we emit
// a note here (a full warning requires knowledge of the error library, deferred
// until the error library is integrated).
// ─────────────────────────────────────────────────────────────────────────────
static void checkExprStmt(ExprStmtAST& node, SymbolTable& symbols, TypeResolver& resolver,
                           DiagnosticEngine& dc, int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_EXTREME("checkExprStmt");
    
    // Optional: Log the expression kind being checked
    if (node.expr) {
        LUC_LOG_SEMANTIC_EXTREME("\tchecking expression kind: " << LucDebug::kindToString(node.expr->kind));
    }
    
    checkExpr(node.expr.get(), symbols, resolver, dc,loopDepth, parallelDepth, insideExtern);
}

// ─────────────────────────────────────────────────────────────────────────────
// checkDeclStmt
// Dispatches to checkVarDecl or checkFuncDecl then declares the symbol locally.
// ─────────────────────────────────────────────────────────────────────────────
static void checkDeclStmt(DeclStmtAST& node, SymbolTable& symbols, TypeResolver& resolver,
                           DiagnosticEngine& dc, int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkDeclStmt: " << (node.isVar() ? "var" : "func"));
    
    if (node.isVar()) {
        VarDeclAST* vd = node.asVar();
        LUC_LOG_SEMANTIC_EXTREME("\tchecking var: " << vd->name);
        checkVarDecl(*vd, symbols, resolver, dc,
                     loopDepth, parallelDepth, insideExtern);

        // Declare the variable in the current (block) scope.
        TypeAST* resolvedTy = vd->resolvedType ? (TypeAST*)vd->resolvedType : resolver.resolveType(vd->type.get());
        Symbol sym;
        sym.name       = vd->name;
        sym.kind       = SymbolKind::Var;
        sym.declKw     = vd->keyword;
        sym.visibility = Visibility::Private;
        sym.type       = resolvedTy;
        sym.decl       = vd;
        sym.loc        = vd->loc;
        
        if (!symbols.declare(sym)) {
            LUC_LOG_SEMANTIC("\tERROR: variable '" << vd->name << "' already declared in this scope");
            dc.error(DiagnosticCategory::Semantic, vd->loc, DiagCode::E3005,
                     "symbol '" + vd->name + "' is already declared in this scope");
        } else {
            LUC_LOG_SEMANTIC_EXTREME("\tvariable declared successfully");
        }
    } else if (node.isFunc()) {
        FuncDeclAST* fd = node.asFunc();
        LUC_LOG_SEMANTIC_EXTREME("\tchecking func: " << fd->name);
        checkFuncDecl(*fd, symbols, resolver, dc, loopDepth, parallelDepth, insideExtern);

        // Ensure local function symbols carry a callable FuncType signature.
        // The signature is now fd->type (unified FuncTypeAST)
        // No need to build a separate signature unless it's missing
        if (fd->type.paramGroups.empty() && !fd->type.returnType) {
            // If the type is empty, build a signature from the declaration
            // This should not happen normally, but as a fallback
            fd->type = buildLocalFuncSignature(*fd);
        }

        Symbol sym;
        sym.name       = fd->name;
        sym.kind       = SymbolKind::Func;
        sym.declKw     = fd->keyword;
        sym.visibility = Visibility::Private;
        sym.type       = &fd->type;  // Point to the unified FuncTypeAST
        sym.decl       = fd;
        sym.loc        = fd->loc;
        
        if (!symbols.declare(sym)) {
            LUC_LOG_SEMANTIC("\tERROR: function '" << fd->name << "' already declared in this scope");
            dc.error(DiagnosticCategory::Semantic, fd->loc, DiagCode::E3005,
                     "symbol '" + fd->name + "' is already declared in this scope");
        } else {
            LUC_LOG_SEMANTIC_EXTREME("\tfunction declared successfully");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkIfStmt
// ─────────────────────────────────────────────────────────────────────────────
static void checkIfStmt(IfStmtAST& node, SymbolTable& symbols, TypeResolver& resolver,
                         DiagnosticEngine& dc, TypeAST* expectedReturn,
                         int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkIfStmt");
    
    TypeAST* condType = checkExpr(node.condition.get(), symbols, resolver, dc, loopDepth, parallelDepth, insideExtern);
    if (condType && !TypeChecker::isBooleanCompatible(condType)) {
        LUC_LOG_SEMANTIC("\tERROR: condition is not bool-compatible");
        dc.error(DiagnosticCategory::Semantic, node.condition->loc, DiagCode::E3002,
                 "if condition must be bool");
    } else {
        LUC_LOG_SEMANTIC_EXTREME("\tcondition type is bool-compatible");
    }

    if (node.thenBranch) {
        // If the condition is 'x is T', narrow 'x' to 'T' inside the then-branch.
        bool narrowed = false;
        if (node.condition->kind == ASTKind::IsExpr) {
            auto* isExpr = static_cast<IsExprAST*>(node.condition.get());
            if (isExpr->expr->kind == ASTKind::IdentifierExpr) {
                auto* ident = static_cast<IdentifierExprAST*>(isExpr->expr.get());
                Symbol* originalSym = symbols.lookup(ident->name);
                if (originalSym) {
                    LUC_LOG_SEMANTIC_EXTREME("\ttype narrowing: " << ident->name);
                    symbols.pushScope();
                    narrowed = true;
                    Symbol narrowedSym = *originalSym;
                    narrowedSym.type = isExpr->checkType.get();
                    symbols.declare(narrowedSym);
                }
            }
        }

        LUC_LOG_SEMANTIC_EXTREME("\tchecking then branch");
        checkStmt(node.thenBranch.get(), symbols, resolver, dc, expectedReturn,
                  loopDepth, parallelDepth, insideExtern);

        if (narrowed) {
            symbols.popScope();
            LUC_LOG_SEMANTIC_EXTREME("\ttype narrow scope popped");
        }
    }
    
    if (node.elseBranch) {
        LUC_LOG_SEMANTIC_EXTREME("\tchecking else branch");
        checkStmt(node.elseBranch.get(), symbols, resolver, dc, expectedReturn,
                  loopDepth, parallelDepth, insideExtern);
    }
    
    LUC_LOG_SEMANTIC_VERBOSE("checkIfStmt: complete");
}

// ─────────────────────────────────────────────────────────────────────────────
// checkSwitchStmt
// The subject type is checked; each case value must be assignable to it.
// ─────────────────────────────────────────────────────────────────────────────
static void checkSwitchStmt(SwitchStmtAST& node, SymbolTable& symbols,
                              TypeResolver& resolver, DiagnosticEngine& dc,
                              TypeAST* expectedReturn, int& loopDepth, 
                              int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkSwitchStmt: " << node.cases.size() << " cases");
    
    TypeAST* subjectType = checkExpr(node.subject.get(), symbols, resolver, dc,
                                     loopDepth, parallelDepth, insideExtern);
    LUC_LOG_SEMANTIC_EXTREME("\tsubject type checked");

    int caseCount = 0;
    for (auto& cas : node.cases) {
        caseCount++;
        LUC_LOG_SEMANTIC_EXTREME("\tchecking case " << caseCount << " with " 
                               << cas->values.size() << " values");
        
        for (auto& val : cas->values) {
            TypeAST* vt = checkExpr(val.get(), symbols, resolver, dc,
                                    loopDepth, parallelDepth, insideExtern);
            if (subjectType && vt && !TypeChecker::isAssignable(vt, subjectType)) {
                LUC_LOG_SEMANTIC("\tERROR: case value type not assignable to subject");
                dc.error(DiagnosticCategory::Semantic, val->loc, DiagCode::E3002,
                         "switch case value type does not match subject type");
            }
        }
        if (cas->body) {
            LUC_LOG_SEMANTIC_EXTREME("\tchecking case body");
            checkBlock(*cas->body, symbols, resolver, dc, expectedReturn,
                       loopDepth, parallelDepth, insideExtern);
        }
    }

    if (node.defaultBody) {
        LUC_LOG_SEMANTIC_EXTREME("\tchecking default body");
        checkBlock(*node.defaultBody, symbols, resolver, dc, expectedReturn,
                   loopDepth, parallelDepth, insideExtern);
    }
    
    LUC_LOG_SEMANTIC_VERBOSE("checkSwitchStmt: complete");
}

// ─────────────────────────────────────────────────────────────────────────────
// checkForStmt
// Validates the iterable (collection or range), declares the loop variable,
// checks the body.
// ─────────────────────────────────────────────────────────────────────────────
static void checkForStmt(ForStmtAST& node, SymbolTable& symbols, TypeResolver& resolver,
                          DiagnosticEngine& dc, TypeAST* expectedReturn,
                          int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkForStmt: varName='" << node.varName 
                           << "', loopDepth=" << loopDepth);
    
    TypeAST* iterType = checkExpr(node.iterable.get(), symbols, resolver, dc,
                                  loopDepth, parallelDepth, insideExtern);
    LUC_LOG_SEMANTIC_EXTREME("\titerable type checked");

    // Infer element type from the iterable.
    TypeAST* elemType = SemanticHelpers::getPrimitiveType(PrimitiveKind::Int); // default for range
    if (iterType) {
        if (iterType->isa<FixedArrayTypeAST>()) {
            elemType = iterType->as<FixedArrayTypeAST>()->element.get();
            LUC_LOG_SEMANTIC_EXTREME("\titerable is FixedArray, element type inferred");
        } else if (iterType->isa<SliceTypeAST>()) {
            elemType = iterType->as<SliceTypeAST>()->element.get();
            LUC_LOG_SEMANTIC_EXTREME("\titerable is Slice, element type inferred");
        } else if (iterType->isa<DynamicArrayTypeAST>()) {
            elemType = iterType->as<DynamicArrayTypeAST>()->element.get();
            LUC_LOG_SEMANTIC_EXTREME("\titerable is DynamicArray, element type inferred");
        } else if (iterType->isa<RangeExprAST>()) {
            LUC_LOG_SEMANTIC_EXTREME("\titerable is Range, element type = int");
        } else {
            LUC_LOG_SEMANTIC_EXTREME("\titerable is other type, using default int");
        }
    }

    // Explicit type annotation overrides inferred element type.
    if (node.varType) {
        TypeAST* explicit_t = resolver.resolveType(node.varType.get());
        if (explicit_t) {
            elemType = explicit_t;
            LUC_LOG_SEMANTIC_EXTREME("\texplicit var type overrides: " 
                                   << LucDebug::kindToString(elemType->kind));
        }
    }

    LUC_LOG_SEMANTIC_EXTREME("\tloop var type: " << (elemType ? LucDebug::kindToString(elemType->kind) : "null"));
    
    loopDepth++;
    LUC_LOG_SEMANTIC_EXTREME("\tloopDepth incremented to " << loopDepth);
    
    symbols.pushScope();

    // Declare the loop variable in the body scope.
    Symbol loopVar;
    loopVar.name       = node.varName;
    loopVar.kind       = SymbolKind::Var;
    loopVar.declKw     = DeclKeyword::Let;
    loopVar.visibility = Visibility::Private;
    loopVar.type       = elemType;
    loopVar.decl       = nullptr;
    loopVar.loc        = node.loc;
    
    if (!symbols.declare(loopVar)) {
        LUC_LOG_SEMANTIC("\tERROR: loop variable '" << node.varName << "' already declared");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3005,
                 "loop variable '" + node.varName + "' already declared in this scope");
    } else {
        LUC_LOG_SEMANTIC_EXTREME("\tloop variable declared: " << node.varName);
    }

    if (node.body) {
        LUC_LOG_SEMANTIC_EXTREME("\tchecking loop body");
        checkStmt(node.body.get(), symbols, resolver, dc, expectedReturn,
                  loopDepth, parallelDepth, insideExtern);
    }

    symbols.popScope();
    loopDepth--;
    LUC_LOG_SEMANTIC_EXTREME("\tloopDepth decremented to " << loopDepth);
    
    LUC_LOG_SEMANTIC_VERBOSE("checkForStmt: complete");
}

// ─────────────────────────────────────────────────────────────────────────────
// checkWhileStmt
// ─────────────────────────────────────────────────────────────────────────────
static void checkWhileStmt(WhileStmtAST& node, SymbolTable& symbols,
                             TypeResolver& resolver, DiagnosticEngine& dc,
                             TypeAST* expectedReturn, int& loopDepth, 
                             int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkWhileStmt: loopDepth=" << loopDepth);
    
    TypeAST* condType = checkExpr(node.condition.get(), symbols, resolver, dc,
                                  loopDepth, parallelDepth, insideExtern);
    if (condType && !TypeChecker::isBooleanCompatible(condType)) {
        LUC_LOG_SEMANTIC("\tERROR: condition is not bool-compatible");
        dc.error(DiagnosticCategory::Semantic, node.condition->loc, DiagCode::E3002,
                 "while condition must be bool");
    } else {
        LUC_LOG_SEMANTIC_EXTREME("\tcondition type is bool-compatible");
    }

    loopDepth++;
    LUC_LOG_SEMANTIC_EXTREME("\tloopDepth incremented to " << loopDepth);
    
    if (node.body) {
        LUC_LOG_SEMANTIC_EXTREME("\tchecking loop body");
        checkStmt(node.body.get(), symbols, resolver, dc, expectedReturn,
                  loopDepth, parallelDepth, insideExtern);
    }
    
    loopDepth--;
    LUC_LOG_SEMANTIC_EXTREME("\tloopDepth decremented to " << loopDepth);
    LUC_LOG_SEMANTIC_VERBOSE("checkWhileStmt: complete");
}

// ─────────────────────────────────────────────────────────────────────────────
// checkDoWhileStmt
// Body executes first, then condition is checked.
// ─────────────────────────────────────────────────────────────────────────────
static void checkDoWhileStmt(DoWhileStmtAST& node, SymbolTable& symbols,
                               TypeResolver& resolver, DiagnosticEngine& dc,
                               TypeAST* expectedReturn, int& loopDepth, 
                               int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkDoWhileStmt: loopDepth=" << loopDepth);
    
    loopDepth++;
    LUC_LOG_SEMANTIC_EXTREME("\tloopDepth incremented to " << loopDepth);
    
    if (node.body) {
        LUC_LOG_SEMANTIC_EXTREME("\tchecking body (executes before condition)");
        checkStmt(node.body.get(), symbols, resolver, dc, expectedReturn,
                  loopDepth, parallelDepth, insideExtern);
    }
    
    loopDepth--;
    LUC_LOG_SEMANTIC_EXTREME("\tloopDepth decremented to " << loopDepth);

    TypeAST* condType = checkExpr(node.condition.get(), symbols, resolver, dc,
                                  loopDepth, parallelDepth, insideExtern);
    if (condType && !TypeChecker::isBooleanCompatible(condType)) {
        LUC_LOG_SEMANTIC("\tERROR: condition is not bool-compatible");
        dc.error(DiagnosticCategory::Semantic, node.condition->loc, DiagCode::E3002,
                 "do-while condition must be bool");
    } else {
        LUC_LOG_SEMANTIC_EXTREME("\tcondition type is bool-compatible");
    }
    
    LUC_LOG_SEMANTIC_VERBOSE("checkDoWhileStmt: complete");
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
                              TypeAST* expectedReturn, int& loopDepth, 
                              int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkReturnStmt: parallelDepth=" << parallelDepth);
    
    if (parallelDepth > 0) {
        LUC_LOG_SEMANTIC("\tERROR: return inside parallel scope");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "'return' is not allowed inside a parallel scope");
        return;
    }

    if (!node.value) {
        // Bare return — valid only when expected return is void (nullptr).
        LUC_LOG_SEMANTIC_EXTREME("\tvoid return");
        if (expectedReturn != nullptr) {
            LUC_LOG_SEMANTIC("\tERROR: non-void function cannot use bare return");
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "non-void function must return a value");
        }
        return;
    }

    LUC_LOG_SEMANTIC_EXTREME("\tchecking return value expression");
    TypeAST* valType = checkExpr(node.value.get(), symbols, resolver, dc,
                                 loopDepth, parallelDepth, insideExtern);

    if (!expectedReturn) {
        LUC_LOG_SEMANTIC("\tERROR: void function returning a value");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "void function cannot return a value");
        return;
    }

    if (valType && !TypeChecker::isAssignable(valType, expectedReturn)) {
        LUC_LOG_SEMANTIC("\tERROR: return type mismatch");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "return type mismatch");
    } else {
        LUC_LOG_SEMANTIC_EXTREME("\treturn type matches expected");
    }
    
    LUC_LOG_SEMANTIC_VERBOSE("checkReturnStmt: complete");
}

// ─────────────────────────────────────────────────────────────────────────────
// checkBreakStmt
// ─────────────────────────────────────────────────────────────────────────────
static void checkBreakStmt(BreakStmtAST& node, DiagnosticEngine& dc,
                             int loopDepth, int parallelDepth) {
    LUC_LOG_SEMANTIC_VERBOSE("checkBreakStmt: loopDepth=" << loopDepth 
                           << ", parallelDepth=" << parallelDepth);
    
    if (parallelDepth > 0) {
        LUC_LOG_SEMANTIC("\tERROR: break inside parallel scope");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "'break' is not allowed inside a parallel scope");
    } else if (loopDepth <= 0) {
        LUC_LOG_SEMANTIC("\tERROR: break outside loop");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "'break' must be inside a loop");
    } else {
        LUC_LOG_SEMANTIC_EXTREME("\tbreak is valid");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkContinueStmt
// ─────────────────────────────────────────────────────────────────────────────
static void checkContinueStmt(ContinueStmtAST& node, DiagnosticEngine& dc,
                                int loopDepth, int parallelDepth) {
    LUC_LOG_SEMANTIC_VERBOSE("checkContinueStmt: loopDepth=" << loopDepth 
                           << ", parallelDepth=" << parallelDepth);
    
    if (parallelDepth > 0) {
        LUC_LOG_SEMANTIC("\tERROR: continue inside parallel scope");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "'continue' is not allowed inside a parallel scope");
    } else if (loopDepth <= 0) {
        LUC_LOG_SEMANTIC("\tERROR: continue outside loop");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "'continue' must be inside a loop");
    } else {
        LUC_LOG_SEMANTIC_EXTREME("\tcontinue is valid");
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
                                   TypeAST* expectedReturn, int& loopDepth, 
                                   int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkParallelForStmt: varName='" << node.varName 
                           << "', parallelDepth=" << parallelDepth);
    
    TypeAST* iterType = checkExpr(node.iterable.get(), symbols, resolver, dc,
                                  loopDepth, parallelDepth, insideExtern);
    LUC_LOG_SEMANTIC_EXTREME("\titerable type checked");

    TypeAST* elemType = SemanticHelpers::getPrimitiveType(PrimitiveKind::Int);
    if (iterType) {
        if (iterType->isa<FixedArrayTypeAST>()) {
            elemType = iterType->as<FixedArrayTypeAST>()->element.get();
            LUC_LOG_SEMANTIC_EXTREME("\titerable is FixedArray");
        } else if (iterType->isa<SliceTypeAST>()) {
            elemType = iterType->as<SliceTypeAST>()->element.get();
            LUC_LOG_SEMANTIC_EXTREME("\titerable is Slice");
        } else if (iterType->isa<DynamicArrayTypeAST>()) {
            elemType = iterType->as<DynamicArrayTypeAST>()->element.get();
            LUC_LOG_SEMANTIC_EXTREME("\titerable is DynamicArray");
        } else {
            LUC_LOG_SEMANTIC_EXTREME("\titerable is other type");
        }
    }

    if (node.varType) {
        TypeAST* explicit_t = resolver.resolveType(node.varType.get());
        if (explicit_t) {
            elemType = explicit_t;
            LUC_LOG_SEMANTIC_EXTREME("\texplicit var type overrides");
        }
    }

    LUC_LOG_SEMANTIC_EXTREME("\tparallelDepth incremented to " << (parallelDepth + 1));
    parallelDepth++;
    symbols.pushScope();

    Symbol loopVar;
    loopVar.name = node.varName;
    loopVar.kind = SymbolKind::Var;
    loopVar.declKw = DeclKeyword::Let;
    loopVar.visibility = Visibility::Private;
    loopVar.type = elemType;
    loopVar.decl = nullptr;
    loopVar.loc = node.loc;
    
    if (!symbols.declare(loopVar)) {
        LUC_LOG_SEMANTIC("\tERROR: parallel loop variable already declared");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3005,
                 "parallel loop variable '" + node.varName + "' already declared");
    } else {
        LUC_LOG_SEMANTIC_EXTREME("\tparallel loop variable declared: " << node.varName);
    }

    if (node.body) {
        LUC_LOG_SEMANTIC_EXTREME("\tchecking parallel body");
        checkStmt(node.body.get(), symbols, resolver, dc, expectedReturn,
                  loopDepth, parallelDepth, insideExtern);
    }

    symbols.popScope();
    parallelDepth--;
    LUC_LOG_SEMANTIC_EXTREME("\tparallelDepth decremented to " << parallelDepth);
    LUC_LOG_SEMANTIC_VERBOSE("checkParallelForStmt: complete");
}

// ─────────────────────────────────────────────────────────────────────────────
// checkParallelBlockStmt
// Each sub-block is an independent task — they all run concurrently.
// ─────────────────────────────────────────────────────────────────────────────
static void checkParallelBlockStmt(ParallelBlockStmtAST& node, SymbolTable& symbols,
                                    TypeResolver& resolver, DiagnosticEngine& dc,
                                    TypeAST* expectedReturn, int& loopDepth, 
                                    int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkParallelBlockStmt: " << node.subBlocks.size() 
                           << " sub-blocks, parallelDepth=" << parallelDepth);
    
    LUC_LOG_SEMANTIC_EXTREME("\tparallelDepth incremented to " << (parallelDepth + 1));
    parallelDepth++;
    
    int subBlockCount = 0;
    for (auto& sub : node.subBlocks) {
        subBlockCount++;
        LUC_LOG_SEMANTIC_EXTREME("\tchecking sub-block " << subBlockCount);
        checkBlock(*sub, symbols, resolver, dc, expectedReturn,
                   loopDepth, parallelDepth, insideExtern);
    }
    
    parallelDepth--;
    LUC_LOG_SEMANTIC_EXTREME("\tparallelDepth decremented to " << parallelDepth);
    LUC_LOG_SEMANTIC_VERBOSE("checkParallelBlockStmt: complete, checked " 
                           << subBlockCount << " sub-blocks");
}

// ─────────────────────────────────────────────────────────────────────────────
// checkStmt — main dispatcher
// ─────────────────────────────────────────────────────────────────────────────
void checkStmt(StmtAST* node, SymbolTable& symbols, TypeResolver& resolver,
               DiagnosticEngine& dc, TypeAST* expectedReturn,
               int& loopDepth, int& parallelDepth, bool insideExtern) {
    if (!node) {
        LUC_LOG_SEMANTIC_EXTREME("checkStmt: null node");
        return;
    }

    LUC_LOG_SEMANTIC_EXTREME("checkStmt: kind=" << LucDebug::kindToString(node->kind));

    switch (node->kind) {
        case ASTKind::BlockStmt:
            LUC_LOG_SEMANTIC_EXTREME("\t-> BlockStmt");
            checkBlock(*node->as<BlockStmtAST>(), symbols, resolver, dc, expectedReturn,
                       loopDepth, parallelDepth, insideExtern);
            break;

        case ASTKind::ExprStmt:
            LUC_LOG_SEMANTIC_EXTREME("\t-> ExprStmt");
            checkExprStmt(*node->as<ExprStmtAST>(), symbols, resolver, dc,
                          loopDepth, parallelDepth, insideExtern);
            break;

        case ASTKind::DeclStmt:
            LUC_LOG_SEMANTIC_EXTREME("\t-> DeclStmt");
            checkDeclStmt(*node->as<DeclStmtAST>(), symbols, resolver, dc,
                          loopDepth, parallelDepth, insideExtern);
            break;

        case ASTKind::IfStmt:
            LUC_LOG_SEMANTIC_EXTREME("\t-> IfStmt");
            checkIfStmt(*node->as<IfStmtAST>(), symbols, resolver, dc, expectedReturn,
                        loopDepth, parallelDepth, insideExtern);
            break;

        case ASTKind::SwitchStmt:
            LUC_LOG_SEMANTIC_EXTREME("\t-> SwitchStmt");
            checkSwitchStmt(*node->as<SwitchStmtAST>(), symbols, resolver, dc,
                            expectedReturn, loopDepth, parallelDepth,
                            insideExtern);
            break;

        case ASTKind::ForStmt:
            LUC_LOG_SEMANTIC_EXTREME("\t-> ForStmt");
            checkForStmt(*node->as<ForStmtAST>(), symbols, resolver, dc, expectedReturn,
                         loopDepth, parallelDepth, insideExtern);
            break;

        case ASTKind::WhileStmt:
            LUC_LOG_SEMANTIC_EXTREME("\t-> WhileStmt");
            checkWhileStmt(*node->as<WhileStmtAST>(), symbols, resolver, dc, expectedReturn,
                           loopDepth, parallelDepth, insideExtern);
            break;

        case ASTKind::DoWhileStmt:
            LUC_LOG_SEMANTIC_EXTREME("\t-> DoWhileStmt");
            checkDoWhileStmt(*node->as<DoWhileStmtAST>(), symbols, resolver, dc,
                             expectedReturn, loopDepth, parallelDepth, insideExtern);
            break;

        case ASTKind::ReturnStmt:
            LUC_LOG_SEMANTIC_EXTREME("\t-> ReturnStmt");
            checkReturnStmt(*node->as<ReturnStmtAST>(), symbols, resolver, dc,
                            expectedReturn, loopDepth, parallelDepth, insideExtern);
            break;

        case ASTKind::BreakStmt:
            LUC_LOG_SEMANTIC_EXTREME("\t-> BreakStmt");
            checkBreakStmt(*node->as<BreakStmtAST>(), dc, loopDepth, parallelDepth);
            break;

        case ASTKind::ContinueStmt:
            LUC_LOG_SEMANTIC_EXTREME("\t-> ContinueStmt");
            checkContinueStmt(*node->as<ContinueStmtAST>(), dc, loopDepth, parallelDepth);
            break;

        case ASTKind::ParallelForStmt:
            LUC_LOG_SEMANTIC_EXTREME("\t-> ParallelForStmt");
            checkParallelForStmt(*node->as<ParallelForStmtAST>(), symbols, resolver, dc,
                                 expectedReturn, loopDepth, parallelDepth,
                                 insideExtern);
            break;

        case ASTKind::ParallelBlockStmt:
            LUC_LOG_SEMANTIC_EXTREME("\t-> ParallelBlockStmt");
            checkParallelBlockStmt(*node->as<ParallelBlockStmtAST>(), symbols, resolver,
                                   dc, expectedReturn, loopDepth,
                                   parallelDepth, insideExtern);
            break;

        default:
            LUC_LOG_SEMANTIC("\tWARNING: Unknown statement kind: " 
                           << static_cast<int>(node->kind));
            break;
    }
}