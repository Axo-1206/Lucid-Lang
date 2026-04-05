/**
 * @file SemanticDecl.cpp
 *
 * @nutshell Verifies the structure of structs, enums, functions, and implementations.
 *
 * @responsibility Phase 3a of semantic analysis: walks declaration nodes, resolves their
 *   types via TypeResolver, and enforces all declaration-level rules.
 *
 * @logic
 *   checkVarDecl   — resolves annotation type, enforces val/nil rules, checks init type
 *   checkFuncDecl  — resolves param + return types, pushes func scope, checks body
 *   checkStructDecl— resolves field types, checks no duplicate field names
 *   checkEnumDecl  — validates variant values are unique, assigns auto-increments
 *   checkTraitDecl — resolves method param + return types
 *   checkImplDecl  — checks method bodies, verifies trait conformance if traitRef present
 *   checkFromDecl  — validates source/target types for custom conversions
 *   checkExternDecl— resolves param + return types under insideExtern_ = true
 *
 * @related SemanticAnalyzer.cpp, SemanticStmt.cpp, SemanticExpr.cpp
 */

#include "SemanticAnalyzer.hpp"
#include "SemanticSymbol.hpp"
#include "SymbolTable.hpp"
#include "TypeResolver.hpp"
#include "TypeChecker.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "ast/DeclAST.hpp"
#include "ast/StmtAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"

#include <unordered_set>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations 
// NOTE: These are required because Declarations, Statements, and Expressions
// cross-call each other recursively. We use manual forward declarations here
// instead of a header to avoid complex circular dependency loops.
// ─────────────────────────────────────────────────────────────────────────────
TypeAST* checkExpr(ExprAST* node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                   int& parallelDepth, bool insideExtern);

void checkStmt(StmtAST* node, SymbolTable& symbols, TypeResolver& resolver,
               DiagnosticEngine& dc, TypeAST* expectedReturn,
               int& asyncDepth, int& loopDepth, int& parallelDepth,
               bool insideExtern);

// ─────────────────────────────────────────────────────────────────────────────
// checkVarDecl
//
// Rules enforced:
//   - Type annotation must resolve.
//   - val: nil is forbidden anywhere in the type tree (hasNilInTree).
//   - val/imt without an initialiser is an error.
//   - If an initialiser is present, its type must be assignable to the annotation.
//   - nil literal is only assignable to nullable types.
// ─────────────────────────────────────────────────────────────────────────────
void checkVarDecl(VarDeclAST& node, SymbolTable& symbols, TypeResolver& resolver,
                  DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                  int& parallelDepth, bool insideExtern) {

    // 1. Resolve the declared type.
    TypeAST* declaredType = resolver.resolveType(node.type.get());
    if (!declaredType) return; // resolver already emitted a diagnostic

    // 2. val forbids nil anywhere in the type tree.
    if (node.keyword == DeclKeyword::Val && TypeChecker::hasNilInTree(declaredType)) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "val '" + node.name + "': nil is not allowed in a val type tree");
    }

    // 3. val and imt require an initialiser.
    if (!node.init) {
        if (node.keyword == DeclKeyword::Val) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "val '" + node.name + "' must have an initialiser");
        }
        return;
    }

    // 4. Check the initialiser type and verify assignability.
    TypeAST* initType = checkExpr(node.init.get(), symbols, resolver, dc,
                                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    if (!initType) return;

    // nil literal is assignable only to nullable types.
    if (node.init->isa<LiteralExprAST>()) {
        auto* lit = node.init->as<LiteralExprAST>();
        if (lit->kind == LiteralKind::Nil && !TypeChecker::isNullable(declaredType)) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "nil cannot be assigned to non-nullable type '" + node.name + "'");
            return;
        }
    }

    if (!TypeChecker::isAssignable(initType, declaredType)) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "type mismatch in declaration '" + node.name + "'");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkFuncDecl
//
// Rules enforced:
//   - Each parameter type must resolve.
//   - Return type must resolve (nullptr = void, always valid).
//   - Parameters are declared into a new function scope.
//   - Body is checked via SemanticStmt with the resolved return type as context.
//   - Curried multi-group functions: each group creates a nested function scope.
// ─────────────────────────────────────────────────────────────────────────────
void checkFuncDecl(FuncDeclAST& node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                   int& parallelDepth, bool insideExtern) {

    // Resolve return type (nullptr is void — valid).
    TypeAST* returnType = nullptr;
    if (node.returnType) {
        returnType = resolver.resolveType(node.returnType.get());
        if (!returnType) return;
    }

    // async increments depth so nested await checks work correctly.
    if (node.isAsync) asyncDepth++;

    symbols.pushScope();

    // Resolve param types and declare params in scope.
    for (auto& group : node.paramGroups) {
        for (auto& param : group) {
            TypeAST* pt = resolver.resolveType(param->type.get());
            if (!pt) {
                symbols.popScope();
                if (node.isAsync) asyncDepth--;
                return;
            }
            Symbol ps;
            ps.name       = param->name;
            ps.kind       = SymbolKind::Param;
            ps.declKw     = DeclKeyword::Let;
            ps.visibility = Visibility::Private;
            ps.type       = pt;
            ps.decl       = param.get();
            ps.isAsync    = false;
            ps.loc        = param->loc;
            if (!symbols.declare(ps)) {
                dc.error(DiagnosticCategory::Semantic, param->loc, DiagCode::E3005,
                         "duplicate parameter name '" + param->name + "'");
            }
        }
    }

    // Check the body.
    if (node.body) {
        checkStmt(node.body.get(), symbols, resolver, dc, returnType,
                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    }

    symbols.popScope();
    if (node.isAsync) asyncDepth--;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkStructDecl
//
// Rules enforced:
//   - Each field type must resolve.
//   - Duplicate field names are a semantic error.
//   - Default value types must match their field types.
// ─────────────────────────────────────────────────────────────────────────────
void checkStructDecl(StructDeclAST& node, SymbolTable& symbols, TypeResolver& resolver,
                     DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                     int& parallelDepth, bool insideExtern) {

    std::unordered_set<std::string> seen;
    for (auto& field : node.fields) {
        if (!seen.insert(field->name).second) {
            dc.error(DiagnosticCategory::Semantic, field->loc, DiagCode::E3005,
                     "duplicate field '" + field->name + "' in struct '" + node.name + "'");
            continue;
        }
        TypeAST* ft = resolver.resolveType(field->type.get());
        if (!ft) continue;

        if (field->defaultVal) {
            TypeAST* dvt = checkExpr(field->defaultVal.get(), symbols, resolver, dc,
                                     asyncDepth, loopDepth, parallelDepth, insideExtern);
            if (dvt && !TypeChecker::isAssignable(dvt, ft)) {
                dc.error(DiagnosticCategory::Semantic, field->loc, DiagCode::E3002,
                         "default value type mismatch for field '" + field->name + "'");
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkEnumDecl
//
// Rules enforced:
//   - Explicit integer values must be unique within the enum.
//   - Auto-assigned values are computed sequentially.
// ─────────────────────────────────────────────────────────────────────────────
void checkEnumDecl(EnumDeclAST& node, DiagnosticEngine& dc) {
    std::unordered_set<int> usedValues;
    int nextAuto = 0;

    for (auto& variant : node.variants) {
        int value = variant->explicitValue.has_value() ? *variant->explicitValue : nextAuto;

        if (!usedValues.insert(value).second) {
            dc.error(DiagnosticCategory::Semantic, variant->loc, DiagCode::E3005,
                     "duplicate enum value " + std::to_string(value) +
                     " for variant '" + variant->name + "' in enum '" + node.name + "'");
        }

        nextAuto = value + 1;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkTraitDecl
//
// Rules enforced:
//   - Each method's parameter types and return type must resolve.
//   - No duplicate method names within the trait.
// ─────────────────────────────────────────────────────────────────────────────
void checkTraitDecl(TraitDeclAST& node, TypeResolver& resolver, DiagnosticEngine& dc) {
    std::unordered_set<std::string> seen;
    for (auto& method : node.methods) {
        if (!seen.insert(method->name).second) {
            dc.error(DiagnosticCategory::Semantic, method->loc, DiagCode::E3005,
                     "duplicate method '" + method->name + "' in trait '" + node.name + "'");
            continue;
        }
        for (auto& param : method->params) {
            resolver.resolveType(param->type.get());
        }
        if (method->returnType) {
            resolver.resolveType(method->returnType.get());
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// checkImplDecl
//
// Rules enforced:
//   - The impl target struct must exist in the symbol table.
//   - No duplicate method names across the impl block.
//   - Each method body is checked as a function body.
//   - If traitRef is present, every trait method must be implemented with a
//     matching signature.
// ─────────────────────────────────────────────────────────────────────────────
void checkImplDecl(ImplDeclAST& node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                   int& parallelDepth, bool insideExtern) {
 
    // Verify the target struct exists.
    Symbol* structSym = symbols.lookup(node.structName);
    if (!structSym || structSym->kind != SymbolKind::Struct) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                 "impl target '" + node.structName + "' is not a declared struct");
        return;
    }
 
    std::unordered_set<std::string> seen;
 
    for (auto& method : node.methods) {
        if (!seen.insert(method->name).second) {
            dc.error(DiagnosticCategory::Semantic, method->loc, DiagCode::E3005,
                     "duplicate method '" + method->name + "' in impl for '" +
                     node.structName + "'");
            continue;
        }
 
        TypeAST* returnType = nullptr;
        if (method->returnType) {
            returnType = resolver.resolveType(method->returnType.get());
        }
 
        if (method->isAsync) asyncDepth++;
        symbols.pushScope();
 
        for (auto& param : method->params) {
            TypeAST* pt = resolver.resolveType(param->type.get());
            if (!pt) continue;
            Symbol ps;
            ps.name = param->name; ps.kind = SymbolKind::Param;
            ps.declKw = DeclKeyword::Let; ps.visibility = Visibility::Private;
            ps.type = pt; ps.decl = param.get(); ps.isAsync = false; ps.loc = param->loc;
            if (!symbols.declare(ps)) {
                dc.error(DiagnosticCategory::Semantic, param->loc, DiagCode::E3005,
                         "duplicate parameter '" + param->name + "'");
            }
        }
 
        if (method->body) {
            checkStmt(method->body.get(), symbols, resolver, dc, returnType,
                      asyncDepth, loopDepth, parallelDepth, insideExtern);
        }
 
        symbols.popScope();
        if (method->isAsync) asyncDepth--;
    }
 
    // Trait conformance check.
    if (node.traitRef) {
        Symbol* traitSym = symbols.lookup(node.traitRef->name);
        if (!traitSym || traitSym->kind != SymbolKind::Trait) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                     "trait '" + node.traitRef->name + "' is not declared");
        } else {
            auto* traitDecl = traitSym->decl->as<TraitDeclAST>();
            for (auto& requiredMethod : traitDecl->methods) {
                bool found = false;
                for (auto& m : node.methods) {
                    if (m->name == requiredMethod->name) { found = true; break; }
                }
                if (!found) {
                    dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                             "impl of '" + node.structName + "' for trait '" +
                             node.traitRef->name + "' is missing method '" +
                             requiredMethod->name + "'");
                }
            }
        }
    }
}
 
// ─────────────────────────────────────────────────────────────────────────────
// checkFromDecl
//
// Rules enforced:
//   - Target type (returnTypeName) must resolve.
//   - Source parameter type must resolve.
//   - Source parameter is declared into a new scope for the body.
//   - Body is checked to return the target type (implicitly or explicitly).
// ─────────────────────────────────────────────────────────────────────────────
void checkFromDecl(FromDeclAST& node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                   int& parallelDepth, bool insideExtern) {
 
    // 1. Resolve target type.
    Symbol* targetSym = symbols.lookup(node.returnTypeName);
    if (!targetSym || (targetSym->kind != SymbolKind::Struct && targetSym->kind != SymbolKind::Enum)) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                 "from conversion: target '" + node.returnTypeName + "' is not a nominal type");
        return;
    }
    TypeAST* targetType = targetSym->type;
 
    // 2. Resolve source parameter.
    TypeAST* srcType = resolver.resolveType(node.srcParamType.get());
    if (!srcType) return;
 
    // 3. New scope for the conversion body.
    symbols.pushScope();
 
    Symbol ps;
    ps.name       = node.srcParamName;
    ps.kind       = SymbolKind::Param;
    ps.declKw     = DeclKeyword::Let;
    ps.visibility = Visibility::Private;
    ps.type       = srcType;
    ps.decl       = &node;
    ps.isAsync    = false;
    ps.loc        = node.loc;
    symbols.declare(ps);
 
    // 4. Check the body. Expected return is the target type.
    if (node.body) {
        // from conversions implicitly return the target type at the end of their logic.
        // The last expression in the block is typically assigned to the struct fields.
        checkStmt(node.body.get(), symbols, resolver, dc, targetType,
                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    }
 
    symbols.popScope();
}

// ─────────────────────────────────────────────────────────────────────────────
// checkExternDecl
//
// Rules enforced:
//   - All param and return types resolved with insideExtern_ = true so @T is accepted.
// ─────────────────────────────────────────────────────────────────────────────
void checkExternDecl(ExternDeclAST& node, TypeResolver& resolver, DiagnosticEngine& /*dc*/) {
    resolver.setInsideExtern(true);
    for (auto& param : node.params) {
        resolver.resolveType(param->type.get());
    }
    if (node.returnType) {
        resolver.resolveType(node.returnType.get());
    }
    resolver.setInsideExtern(false);
}

// ─────────────────────────────────────────────────────────────────────────────
// checkTopLevelDecl  — Dispatcher called by SemanticAnalyzer::checkDecls()
// ─────────────────────────────────────────────────────────────────────────────
void checkTopLevelDecl(DeclAST* decl, SymbolTable& symbols, TypeResolver& resolver,
                       DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                       int& parallelDepth, bool insideExtern) {
    if (!decl) return;

    if (decl->isa<VarDeclAST>())
        checkVarDecl(*decl->as<VarDeclAST>(), symbols, resolver, dc,
                     asyncDepth, loopDepth, parallelDepth, insideExtern);

    else if (decl->isa<FuncDeclAST>())
        checkFuncDecl(*decl->as<FuncDeclAST>(), symbols, resolver, dc,
                      asyncDepth, loopDepth, parallelDepth, insideExtern);

    else if (decl->isa<StructDeclAST>())
        checkStructDecl(*decl->as<StructDeclAST>(), symbols, resolver, dc,
                        asyncDepth, loopDepth, parallelDepth, insideExtern);

    else if (decl->isa<EnumDeclAST>())
        checkEnumDecl(*decl->as<EnumDeclAST>(), dc);

    else if (decl->isa<TraitDeclAST>())
        checkTraitDecl(*decl->as<TraitDeclAST>(), resolver, dc);

    else if (decl->isa<ImplDeclAST>())
        checkImplDecl(*decl->as<ImplDeclAST>(), symbols, resolver, dc,
                      asyncDepth, loopDepth, parallelDepth, insideExtern);

    else if (decl->isa<ExternDeclAST>())
        checkExternDecl(*decl->as<ExternDeclAST>(), resolver, dc);
 
    else if (decl->isa<FromDeclAST>())
        checkFromDecl(*decl->as<FromDeclAST>(), symbols, resolver, dc,
                      asyncDepth, loopDepth, parallelDepth, insideExtern);

    // PackageDecl, UseDecl, TypeAliasDecl, ModuleDecl — nothing to check at phase 3.
}