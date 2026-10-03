/// @file cli/frontend/JSONDumper.cpp
/// @brief Implementation of complete JSON serialization for the redesigned AST.

#include "JSONDumper.hpp"
#include "core/ast/TypeAST.hpp"

#include <sstream>
#include <iomanip>
#include <iostream>

namespace lucid::cli::pipeline {

// ─── Constructor ─────────────────────────────────────────────────────────

JSONDumper::JSONDumper(StringPool& pool,
                       const std::vector<ModuleAST*>& modules,
                       bool pretty)
    : pool(pool), modules(modules), pretty(pretty) {
    for (auto* module : modules) {
        if (module) {
            moduleMap[module->filePath] = module;
        }
    }
}

// ─── Public API ──────────────────────────────────────────────────────────

std::string JSONDumper::dump(const DiagnosticEngine& diagnostics) {
    JSONWriter json(pretty);
    json.beginObject();
    json.key("modules");
    serializeModules(json);
    json.key("diagnostics");
    serializeDiagnostics(json, diagnostics);
    json.endObject();
    return json.str();
}

bool JSONDumper::dumpToFile(const DiagnosticEngine& diagnostics,
                            const std::string& filePath) {
    std::ofstream file(filePath);
    if (!file.is_open()) return false;
    file << dump(diagnostics);
    return true;
}

// ─── Helpers ─────────────────────────────────────────────────────────────

std::string JSONDumper::str(InternedString s) const {
    return pool.lookup(s);
}

std::string JSONDumper::getModulePath(InternedString filePath) const {
    return str(filePath);
}

// ─── serializeDeclRef ────────────────────────────────────────────────────
//
// Emits a small JSON object that uniquely identifies a declaration:
//
//   { "name": "Person", "kind": "TableDecl" }
//
// `mangledName` is on every DeclAST (set by Sema), so we can emit it
// uniformly — no per-concrete-kind branching is required anymore.

void JSONDumper::serializeDeclRef(JSONWriter& json, DeclAST* decl) {
    if (!decl) { json.null(); return; }
    json.beginObject();
    json.kv("name", str(decl->name));
    json.kv("kind", astKindToString(decl->kind));
    json.kv("mangledName", str(decl->mangledName));
    json.endObject();
}

// ─── DocComment ──────────────────────────────────────────────────────────

void JSONDumper::serializeDocComment(JSONWriter& json, const DocComment& doc) {
    json.beginObject();
    json.kv("text", str(doc.text));
    json.endObject();
}

// ─── Module Serialization ────────────────────────────────────────────────

void JSONDumper::serializeModules(JSONWriter& json) {
    json.beginArray();
    for (auto* module : modules) {
        if (module) serializeModule(json, module);
    }
    json.endArray();
}

void JSONDumper::serializeModule(JSONWriter& json, ModuleAST* module) {
    json.beginObject();
    json.kv("kind", "Module");
    json.kv("filePath", getModulePath(module->filePath));
    json.kv("hasErrors", module->hasErrors);

    // ─── Resolved imports ───────────────────────────────────────────────
    // The AST keeps `resolvedImports` as a map alias -> ModuleAST*.
    // We emit it as an object so the reader can see both the alias and
    // the target's file path.
    json.key("resolvedImports");
    json.beginObject();
    for (const auto& [alias, target] : module->resolvedImports) {
        json.key(str(alias));
        if (target) {
            json.string(getModulePath(target->filePath));
        } else {
            json.null();
        }
    }
    json.endObject();

    json.key("declarations");
    json.beginArray();
    for (auto* decl : module->decls) {
        if (decl) serializeDecl(json, decl);
    }
    json.endArray();

    json.endObject();
}

// ─── Declaration Serializers ─────────────────────────────────────────────

void JSONDumper::serializeDecl(JSONWriter& json, DeclAST* decl) {
    if (!decl) { json.null(); return; }

    switch (decl->kind) {
        case ASTKind::ImportDecl:  serializeImportDecl(json, decl->as<ImportDeclAST>()); break;
        case ASTKind::TableDecl:   serializeTableDecl(json,  decl->as<TableDeclAST>());  break;
        case ASTKind::ColumnDecl:  serializeColumnDecl(json, decl->as<ColumnDeclAST>()); break;
        case ASTKind::FnDecl:      serializeFnDecl(json,     decl->as<FnDeclAST>());     break;
        case ASTKind::VarDecl:     serializeVarDecl(json,    decl->as<VarDeclAST>());    break;
        case ASTKind::Param:       serializeParam(json,      decl->as<ParamAST>());      break;
        default:
            json.beginObject();
            json.kv("kind", astKindToString(decl->kind));
            json.kv("name", str(decl->name));
            json.endObject();
            break;
    }
}

// ─── Import Decl ─────────────────────────────────────────────────────────

void JSONDumper::serializeImportDecl(JSONWriter& json, ImportDeclAST* decl) {
    json.beginObject();
    json.kv("kind", "ImportDecl");
    json.kv("path",  str(decl->path));
    json.kv("alias", str(decl->alias));
    json.endObject();
}

// ─── Column Decl ─────────────────────────────────────────────────────────

void JSONDumper::serializeColumnDecl(JSONWriter& json, ColumnDeclAST* decl) {
    json.beginObject();
    json.kv("kind", "ColumnDecl");
    json.kv("name", str(decl->name));
    json.kv("columnIndex", static_cast<uint64_t>(decl->columnIndex));
    json.kv("isUnique",   decl->isUnique);
    json.kv("isPrimary",  decl->isPrimary);
    json.kv("isReadonly", decl->isReadonly);

    if (decl->type) {
        json.key("type");
        serializeType(json, decl->type);
    }

    json.key("attributes");
    json.beginArray();
    for (auto* attr : decl->attributes) {
        if (attr) serializeDecl(json, attr);
    }
    json.endArray();

    if (decl->hasDoc()) {
        json.key("doc");
        serializeDocComment(json, *decl->doc);
    }

    json.key("location");
    serializeLocation(json, decl->loc);
    json.endObject();
}

// ─── Row ────────────────────────────────────────────────────────────────
//
// `RowAST` is not a `BaseAST` — it has no `kind` and no `loc`. We emit a
// small object with just the cells.

void JSONDumper::serializeRow(JSONWriter& json, RowAST* row) {
    json.beginObject();
    json.kv("kind", "Row");
    json.key("cells");
    json.beginArray();
    for (auto* cell : row->cells) {
        if (cell) serializeExpr(json, cell);
    }
    json.endArray();
    json.endObject();
}

// ─── Table Decl ─────────────────────────────────────────────────────────

void JSONDumper::serializeTableDecl(JSONWriter& json, TableDeclAST* decl) {
    json.beginObject();
    json.kv("kind", "TableDecl");
    json.kv("name", str(decl->name));
    json.kv("isExported", decl->isExported);
    json.kv("mangledName", str(decl->mangledName));

    // ─── Parser / declaration-shape fields ─────────────────────────────
    json.kv("isHostBacked", decl->isHostBacked);
    if (decl->isHostBacked) {
        json.kv("hostName", str(decl->hostName));
    }

    // ─── Semantic attribute flags ──────────────────────────────────────
    json.kv("isFixed",     decl->isFixed);
    json.kv("isReadonly",  decl->isReadonly);
    json.kv("isPacked",    decl->isPacked);
    json.kv("isReserved",  decl->isReserved);
    json.kv("reservedCount", decl->reservedCount);
    json.kv("isColumnar",  decl->isColumnar);
    json.kv("isRequest",   decl->isRequest);

    // ─── Derived queries (handy for readers) ───────────────────────────
    json.kv("hasFixedRowSet", decl->hasFixedRowSet());
    json.kv("hasFrozenCells", decl->hasFrozenCells());

    // ─── Columns ────────────────────────────────────────────────────────
    json.key("columns");
    json.beginArray();
    for (auto* col : decl->columns) {
        if (col) serializeColumnDecl(json, col);
    }
    json.endArray();

    // ─── Inline rows ────────────────────────────────────────────────────
    json.key("rows");
    json.beginArray();
    for (auto* row : decl->rows) {
        if (row) serializeRow(json, row);
    }
    json.endArray();

    // ─── Attributes / doc ──────────────────────────────────────────────
    json.key("attributes");
    json.beginArray();
    for (auto* attr : decl->attributes) {
        if (attr) serializeDecl(json, attr);
    }
    json.endArray();

    if (decl->hasDoc()) {
        json.key("doc");
        serializeDocComment(json, *decl->doc);
    }

    json.key("location");
    serializeLocation(json, decl->loc);
    json.endObject();
}

// ─── Fn Decl ────────────────────────────────────────────────────────────

void JSONDumper::serializeFnDecl(JSONWriter& json, FnDeclAST* decl) {
    json.beginObject();
    json.kv("kind", "FnDecl");
    json.kv("name", str(decl->name));
    json.kv("isExported", decl->isExported);
    json.kv("mangledName", str(decl->mangledName));

    json.kv("isHostBound", decl->isHostBound);
    if (decl->isHostBound) {
        json.kv("hostName", str(decl->hostName));
    }
    json.kv("isSequence", decl->isSequence);

    if (decl->deprecationMessage.isValid()) {
        json.kv("deprecationMessage", str(decl->deprecationMessage));
    }

    // ─── Parameters ────────────────────────────────────────────────────
    json.key("params");
    json.beginArray();
    for (auto* p : decl->params) {
        if (p) serializeParam(json, p);
    }
    json.endArray();

    // ─── Return type ───────────────────────────────────────────────────
    json.key("returnType");
    if (decl->returnType) {
        serializeType(json, decl->returnType);
    } else {
        json.null();
    }

    // ─── Body ──────────────────────────────────────────────────────────
    json.key("body");
    if (decl->body) {
        serializeStmt(json, decl->body);
    } else {
        json.null();
    }

    // ─── Attributes / doc ──────────────────────────────────────────────
    json.key("attributes");
    json.beginArray();
    for (auto* attr : decl->attributes) {
        if (attr) serializeDecl(json, attr);
    }
    json.endArray();

    if (decl->hasDoc()) {
        json.key("doc");
        serializeDocComment(json, *decl->doc);
    }

    json.key("location");
    serializeLocation(json, decl->loc);
    json.endObject();
}

// ─── Var Decl ───────────────────────────────────────────────────────────

void JSONDumper::serializeVarDecl(JSONWriter& json, VarDeclAST* decl) {
    json.beginObject();
    json.kv("kind", "VarDecl");
    json.kv("name", str(decl->name));
    json.kv("isExported", decl->isExported);
    json.kv("mangledName", str(decl->mangledName));
    json.kv("isConst", decl->isConst);

    if (decl->type) {
        json.key("type");
        serializeType(json, decl->type);
    }

    json.key("init");
    if (decl->init) {
        serializeExpr(json, decl->init);
    } else {
        json.null();
    }

    json.key("attributes");
    json.beginArray();
    for (auto* attr : decl->attributes) {
        if (attr) serializeDecl(json, attr);
    }
    json.endArray();

    if (decl->hasDoc()) {
        json.key("doc");
        serializeDocComment(json, *decl->doc);
    }

    json.key("location");
    serializeLocation(json, decl->loc);
    json.endObject();
}

// ─── Param ──────────────────────────────────────────────────────────────

void JSONDumper::serializeParam(JSONWriter& json, ParamAST* param) {
    json.beginObject();
    json.kv("kind", "Param");
    json.kv("name", str(param->name));
    json.kv("isVariadic", param->isVariadic);
    json.kv("isConst", param->isConst);

    if (param->type) {
        json.key("type");
        serializeType(json, param->type);
    }

    json.key("location");
    serializeLocation(json, param->loc);
    json.endObject();
}

// ─── Statement Serializers ──────────────────────────────────────────────

void JSONDumper::serializeStmt(JSONWriter& json, StmtAST* stmt) {
    if (!stmt) { json.null(); return; }

    switch (stmt->kind) {
        case ASTKind::BlockStmt:          serializeBlockStmt(json,          stmt->as<BlockStmtAST>());          break;
        case ASTKind::VarDeclStmt:        serializeVarDeclStmt(json,        stmt->as<VarDeclStmtAST>());        break;
        case ASTKind::AssignStmt:         serializeAssignStmt(json,         stmt->as<AssignStmtAST>());         break;
        case ASTKind::ExprStmt:           serializeExprStmt(json,           stmt->as<ExprStmtAST>());           break;
        case ASTKind::ReturnStmt:         serializeReturnStmt(json,         stmt->as<ReturnStmtAST>());         break;
        case ASTKind::BreakStmt:          serializeBreakStmt(json,          stmt->as<BreakStmtAST>());          break;
        case ASTKind::ContinueStmt:       serializeContinueStmt(json,       stmt->as<ContinueStmtAST>());       break;
        case ASTKind::IfStmt:             serializeIfStmt(json,             stmt->as<IfStmtAST>());             break;
        case ASTKind::SwitchStmt:         serializeSwitchStmt(json,         stmt->as<SwitchStmtAST>());         break;
        case ASTKind::WhileStmt:          serializeWhileStmt(json,          stmt->as<WhileStmtAST>());          break;
        case ASTKind::ForStmt:            serializeForStmt(json,            stmt->as<ForStmtAST>());            break;
        case ASTKind::WaitStmt:           serializeWaitStmt(json,           stmt->as<WaitStmtAST>());           break;
        case ASTKind::WaitFramesStmt:     serializeWaitFramesStmt(json,     stmt->as<WaitFramesStmtAST>());     break;
        case ASTKind::WaitUntilStmt:      serializeWaitUntilStmt(json,      stmt->as<WaitUntilStmtAST>());      break;
        case ASTKind::WaitForEventStmt:   serializeWaitForEventStmt(json,   stmt->as<WaitForEventStmtAST>());   break;
        case ASTKind::WaitForRequestStmt: serializeWaitForRequestStmt(json, stmt->as<WaitForRequestStmtAST>()); break;
        default:
            json.beginObject();
            json.kv("kind", astKindToString(stmt->kind));
            json.endObject();
            break;
    }
}

void JSONDumper::serializeBlockStmt(JSONWriter& json, BlockStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "BlockStmt");
    json.key("statements");
    json.beginArray();
    for (auto* s : stmt->stmts) {
        if (s) serializeStmt(json, s);
    }
    json.endArray();
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeVarDeclStmt(JSONWriter& json, VarDeclStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "VarDeclStmt");
    if (stmt->decl) {
        json.key("decl");
        serializeVarDecl(json, stmt->decl);
    }
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeAssignStmt(JSONWriter& json, AssignStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "AssignStmt");
    json.kv("op", assignOpToString(stmt->op));
    if (stmt->lhs) { json.key("lhs"); serializeExpr(json, stmt->lhs); }
    if (stmt->rhs) { json.key("rhs"); serializeExpr(json, stmt->rhs); }
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeExprStmt(JSONWriter& json, ExprStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "ExprStmt");
    if (stmt->expr) { json.key("expr"); serializeExpr(json, stmt->expr); }
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeReturnStmt(JSONWriter& json, ReturnStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "ReturnStmt");
    json.key("value");
    if (stmt->value) serializeExpr(json, stmt->value);
    else             json.null();
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeBreakStmt(JSONWriter& json, BreakStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "BreakStmt");
    if (stmt->label.isValid()) json.kv("label", str(stmt->label));
    else                       json.kvNull("label");
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeContinueStmt(JSONWriter& json, ContinueStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "ContinueStmt");
    if (stmt->label.isValid()) json.kv("label", str(stmt->label));
    else                       json.kvNull("label");
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeIfStmt(JSONWriter& json, IfStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "IfStmt");
    if (stmt->condition)  { json.key("condition");  serializeExpr(json, stmt->condition); }
    if (stmt->thenBranch) { json.key("thenBranch"); serializeStmt(json, stmt->thenBranch); }
    if (stmt->elseBranch) { json.key("elseBranch"); serializeStmt(json, stmt->elseBranch); }
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeSwitchCase(JSONWriter& json, SwitchCaseAST* case_) {
    json.beginObject();
    json.kv("kind", "SwitchCase");
    json.key("values");
    json.beginArray();
    for (auto* value : case_->values) {
        if (value) serializeExpr(json, value);
    }
    json.endArray();
    if (case_->body) {
        json.key("body");
        serializeStmt(json, case_->body);
    }
    json.key("location");
    serializeLocation(json, case_->loc);
    json.endObject();
}

void JSONDumper::serializeSwitchStmt(JSONWriter& json, SwitchStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "SwitchStmt");
    if (stmt->subject) { json.key("subject"); serializeExpr(json, stmt->subject); }

    json.key("cases");
    json.beginArray();
    for (auto* c : stmt->cases) {
        if (c) serializeSwitchCase(json, c);
    }
    json.endArray();

    if (stmt->defaultBody) {
        json.key("defaultBody");
        serializeStmt(json, stmt->defaultBody);
    }
    json.key("defaultLoc");
    serializeLocation(json, stmt->defaultLoc);

    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeWhileStmt(JSONWriter& json, WhileStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "WhileStmt");
    if (stmt->label.isValid()) json.kv("label", str(stmt->label));
    else                       json.kvNull("label");
    if (stmt->condition) { json.key("condition"); serializeExpr(json, stmt->condition); }
    if (stmt->body)      { json.key("body");      serializeStmt(json, stmt->body); }
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeForStmt(JSONWriter& json, ForStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "ForStmt");
    if (stmt->label.isValid()) json.kv("label", str(stmt->label));
    else                       json.kvNull("label");

    json.key("firstVar");
    if (stmt->firstVar) serializeParam(json, stmt->firstVar);
    else                json.null();

    json.key("secondVar");
    if (stmt->secondVar) serializeParam(json, stmt->secondVar);
    else                 json.null();

    if (stmt->iterable) { json.key("iterable"); serializeExpr(json, stmt->iterable); }
    if (stmt->body)     { json.key("body");     serializeStmt(json, stmt->body); }

    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

// ─── Sequence suspend points ────────────────────────────────────────────

void JSONDumper::serializeWaitStmt(JSONWriter& json, WaitStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "WaitStmt");
    if (stmt->seconds) { json.key("seconds"); serializeExpr(json, stmt->seconds); }
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeWaitFramesStmt(JSONWriter& json, WaitFramesStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "WaitFramesStmt");
    if (stmt->frames) { json.key("frames"); serializeExpr(json, stmt->frames); }
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeWaitUntilStmt(JSONWriter& json, WaitUntilStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "WaitUntilStmt");
    if (stmt->predicate) { json.key("predicate"); serializeExpr(json, stmt->predicate); }
    if (stmt->arg)       { json.key("arg");       serializeExpr(json, stmt->arg); }
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeWaitForEventStmt(JSONWriter& json, WaitForEventStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "WaitForEventStmt");
    if (stmt->event) { json.key("event"); serializeExpr(json, stmt->event); }
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeWaitForRequestStmt(JSONWriter& json, WaitForRequestStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "WaitForRequestStmt");
    if (stmt->request) { json.key("request"); serializeExpr(json, stmt->request); }
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

// ─── Expression Serializers ─────────────────────────────────────────────

void JSONDumper::serializeExpr(JSONWriter& json, ExprAST* expr) {
    if (!expr) { json.null(); return; }

    switch (expr->kind) {
        case ASTKind::LiteralExpr:      serializeLiteralExpr(json,      expr->as<LiteralExprAST>());      break;
        case ASTKind::IdentifierExpr:   serializeIdentifierExpr(json,   expr->as<IdentifierExprAST>());   break;
        case ASTKind::ArrayLiteralExpr: serializeArrayLiteralExpr(json, expr->as<ArrayLiteralExprAST>()); break;
        case ASTKind::FieldAccessExpr:  serializeFieldAccessExpr(json,  expr->as<FieldAccessExprAST>());  break;
        case ASTKind::IndexExpr:        serializeIndexExpr(json,        expr->as<IndexExprAST>());        break;
        case ASTKind::CallExpr:         serializeCallExpr(json,         expr->as<CallExprAST>());         break;
        case ASTKind::LambdaExpr:       serializeLambdaExpr(json,       expr->as<LambdaExprAST>());       break;
        case ASTKind::StartExpr:        serializeStartExpr(json,        expr->as<StartExprAST>());        break;
        case ASTKind::UnaryExpr:        serializeUnaryExpr(json,        expr->as<UnaryExprAST>());        break;
        case ASTKind::BinaryExpr:       serializeBinaryExpr(json,       expr->as<BinaryExprAST>());       break;
        case ASTKind::ParenExpr:        serializeParenExpr(json,        expr->as<ParenExprAST>());        break;
        case ASTKind::RangeExpr:        serializeRangeExpr(json,        expr->as<RangeExprAST>());        break;
        default:
            json.beginObject();
            json.kv("kind", astKindToString(expr->kind));
            json.endObject();
            break;
    }
}

void JSONDumper::serializeLiteralExpr(JSONWriter& json, LiteralExprAST* expr) {
    json.beginObject();
    json.kv("kind", "LiteralExpr");
    json.kv("literalKind", literalKindToString(expr->kind));
    json.kv("value", str(expr->value));

    json.kv("isConst", expr->isConst);
    json.kv("isLValue", expr->isLValue);

    json.key("resolvedType");
    if (expr->resolvedType) serializeType(json, expr->resolvedType);
    else                    json.null();

    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeIdentifierExpr(JSONWriter& json, IdentifierExprAST* expr) {
    json.beginObject();
    json.kv("kind", "IdentifierExpr");
    json.kv("name", str(expr->name));

    json.key("resolvedDecl");
    serializeDeclRef(json, expr->resolvedDecl);

    json.kv("isConst",  expr->isConst);
    json.kv("isLValue", expr->isLValue);

    json.key("resolvedType");
    if (expr->resolvedType) serializeType(json, expr->resolvedType);
    else                    json.null();

    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeArrayLiteralExpr(JSONWriter& json, ArrayLiteralExprAST* expr) {
    json.beginObject();
    json.kv("kind", "ArrayLiteralExpr");
    json.key("elements");
    json.beginArray();
    for (auto* elem : expr->elements) {
        if (elem) serializeExpr(json, elem);
    }
    json.endArray();

    json.kv("isConst",  expr->isConst);
    json.kv("isLValue", expr->isLValue);

    json.key("resolvedType");
    if (expr->resolvedType) serializeType(json, expr->resolvedType);
    else                    json.null();

    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeFieldAccessExpr(JSONWriter& json, FieldAccessExprAST* expr) {
    json.beginObject();
    json.kv("kind", "FieldAccessExpr");

    if (expr->object) { json.key("object"); serializeExpr(json, expr->object); }
    json.kv("fieldName", str(expr->fieldName));

    // ─── Resolution flags (set by Sema) ─────────────────────────────────
    json.kv("isModuleAccess",      expr->isModuleAccess);
    json.kv("isTableMethod",       expr->isTableMethod);
    json.kv("isColumnView",        expr->isColumnView);
    json.kv("isFixedRowSugar",     expr->isFixedRowSugar);
    json.kv("isPrimaryLookup",     expr->isPrimaryLookup);
    json.kv("hasResolvedFixedRow", expr->hasResolvedFixedRow);
    if (expr->hasResolvedFixedRow) {
        json.kv("resolvedFixedRowIndex",
                static_cast<uint64_t>(expr->resolvedFixedRowIndex));
    }

    json.key("resolvedColumn");
    serializeDeclRef(json, expr->resolvedColumn);

    json.key("resolvedDecl");
    serializeDeclRef(json, expr->resolvedDecl);

    json.kv("isConst",  expr->isConst);
    json.kv("isLValue", expr->isLValue);

    json.key("resolvedType");
    if (expr->resolvedType) serializeType(json, expr->resolvedType);
    else                    json.null();

    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeIndexExpr(JSONWriter& json, IndexExprAST* expr) {
    json.beginObject();
    json.kv("kind", "IndexExpr");
    if (expr->target) { json.key("target"); serializeExpr(json, expr->target); }
    if (expr->index)  { json.key("index");  serializeExpr(json, expr->index); }

    json.kv("isConst",  expr->isConst);
    json.kv("isLValue", expr->isLValue);

    json.key("resolvedType");
    if (expr->resolvedType) serializeType(json, expr->resolvedType);
    else                    json.null();

    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeCallExpr(JSONWriter& json, CallExprAST* expr) {
    json.beginObject();
    json.kv("kind", "CallExpr");
    if (expr->callee) { json.key("callee"); serializeExpr(json, expr->callee); }

    json.key("args");
    json.beginArray();
    for (auto* arg : expr->args) {
        if (arg) serializeExpr(json, arg);
    }
    json.endArray();

    json.kv("isConst",  expr->isConst);
    json.kv("isLValue", expr->isLValue);

    json.key("resolvedType");
    if (expr->resolvedType) serializeType(json, expr->resolvedType);
    else                    json.null();

    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeLambdaExpr(JSONWriter& json, LambdaExprAST* expr) {
    json.beginObject();
    json.kv("kind", "LambdaExpr");

    json.key("params");
    json.beginArray();
    for (auto* p : expr->params) {
        if (p) serializeParam(json, p);
    }
    json.endArray();

    if (expr->body) { json.key("body"); serializeExpr(json, expr->body); }

    json.kv("isConst",  expr->isConst);
    json.kv("isLValue", expr->isLValue);

    json.key("resolvedType");
    if (expr->resolvedType) serializeType(json, expr->resolvedType);
    else                    json.null();

    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeStartExpr(JSONWriter& json, StartExprAST* expr) {
    json.beginObject();
    json.kv("kind", "StartExpr");
    if (expr->call) { json.key("call"); serializeCallExpr(json, expr->call); }

    json.kv("isConst",  expr->isConst);
    json.kv("isLValue", expr->isLValue);

    json.key("resolvedType");
    if (expr->resolvedType) serializeType(json, expr->resolvedType);
    else                    json.null();

    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeUnaryExpr(JSONWriter& json, UnaryExprAST* expr) {
    json.beginObject();
    json.kv("kind", "UnaryExpr");
    json.kv("op", unaryOpToString(expr->op));
    if (expr->operand) { json.key("operand"); serializeExpr(json, expr->operand); }

    json.kv("isConst",  expr->isConst);
    json.kv("isLValue", expr->isLValue);

    json.key("resolvedType");
    if (expr->resolvedType) serializeType(json, expr->resolvedType);
    else                    json.null();

    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeBinaryExpr(JSONWriter& json, BinaryExprAST* expr) {
    json.beginObject();
    json.kv("kind", "BinaryExpr");
    json.kv("op", binaryOpToString(expr->op));
    if (expr->left)  { json.key("left");  serializeExpr(json, expr->left); }
    if (expr->right) { json.key("right"); serializeExpr(json, expr->right); }

    json.kv("isConst",  expr->isConst);
    json.kv("isLValue", expr->isLValue);

    json.key("resolvedType");
    if (expr->resolvedType) serializeType(json, expr->resolvedType);
    else                    json.null();

    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeParenExpr(JSONWriter& json, ParenExprAST* expr) {
    json.beginObject();
    json.kv("kind", "ParenExpr");
    if (expr->inner) { json.key("inner"); serializeExpr(json, expr->inner); }

    json.kv("isConst",  expr->isConst);
    json.kv("isLValue", expr->isLValue);

    json.key("resolvedType");
    if (expr->resolvedType) serializeType(json, expr->resolvedType);
    else                    json.null();

    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeRangeExpr(JSONWriter& json, RangeExprAST* expr) {
    json.beginObject();
    json.kv("kind", "RangeExpr");
    json.kv("isExclusive", expr->isExclusive);

    if (expr->lo)   { json.key("lo");   serializeExpr(json, expr->lo); }
    if (expr->hi)   { json.key("hi");   serializeExpr(json, expr->hi); }
    json.key("step");
    if (expr->step) { serializeExpr(json, expr->step); }
    else            { json.null(); }

    json.kv("isConst",  expr->isConst);
    json.kv("isLValue", expr->isLValue);

    json.key("resolvedType");
    if (expr->resolvedType) serializeType(json, expr->resolvedType);
    else                    json.null();

    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

// ─── Type Serializers ───────────────────────────────────────────────────

void JSONDumper::serializeType(JSONWriter& json, TypeAST* type) {
    if (!type) { json.null(); return; }

    switch (type->kind) {
        case ASTKind::PrimitiveType: serializePrimitiveType(json, type->as<PrimitiveTypeAST>()); break;
        case ASTKind::NamedType:     serializeNamedType(json,     type->as<NamedTypeAST>());     break;
        case ASTKind::ArrayType:     serializeArrayType(json,     type->as<ArrayTypeAST>());     break;
        case ASTKind::RowRefType:    serializeRowRefType(json,    type->as<RowRefTypeAST>());    break;
        case ASTKind::FunctionType:  serializeFunctionType(json,  type->as<FunctionTypeAST>());  break;
        case ASTKind::NullableType:  serializeNullableType(json,  type->as<NullableTypeAST>());  break;
        default:
            json.beginObject();
            json.kv("kind", astKindToString(type->kind));
            json.key("location");
            serializeLocation(json, type->loc);
            json.endObject();
            break;
    }
}

void JSONDumper::serializePrimitiveType(JSONWriter& json, PrimitiveTypeAST* type) {
    json.beginObject();
    json.kv("kind", "PrimitiveType");
    json.kv("primitiveKind", primitiveKindToString(type->primitiveKind));
    json.kv("bitWidth", static_cast<uint64_t>(primitiveBitWidth(type->primitiveKind)));
    json.key("location");
    serializeLocation(json, type->loc);
    json.endObject();
}

void JSONDumper::serializeNamedType(JSONWriter& json, NamedTypeAST* type) {
    json.beginObject();
    json.kv("kind", "NamedType");
    json.kv("name", str(type->name));

    // qualifier is an InternedString; emit null when it's invalid.
    if (type->isQualified()) {
        json.kv("qualifier", str(type->qualifier));
    } else {
        json.kvNull("qualifier");
    }

    json.key("resolvedDecl");
    serializeDeclRef(json, type->resolvedDecl);

    json.key("location");
    serializeLocation(json, type->loc);
    json.endObject();
}

void JSONDumper::serializeArrayType(JSONWriter& json, ArrayTypeAST* type) {
    json.beginObject();
    json.kv("kind", "ArrayType");
    json.kv("arrayKind", arrayKindToString(type->arrayKind));
    if (type->isFixed()) {
        json.kv("fixedSize", type->fixedSize);
    } else {
        json.kvNull("fixedSize");
    }

    json.key("element");
    if (type->element) serializeType(json, type->element);
    else               json.null();

    json.key("location");
    serializeLocation(json, type->loc);
    json.endObject();
}

void JSONDumper::serializeRowRefType(JSONWriter& json, RowRefTypeAST* type) {
    json.beginObject();
    json.kv("kind", "RowRefType");
    json.key("inner");
    if (type->inner) serializeType(json, type->inner);
    else             json.null();
    json.key("location");
    serializeLocation(json, type->loc);
    json.endObject();
}

void JSONDumper::serializeFunctionType(JSONWriter& json, FunctionTypeAST* type) {
    json.beginObject();
    json.kv("kind", "FunctionType");

    json.key("params");
    json.beginArray();
    for (auto* p : type->params) {
        if (p) serializeType(json, p);
    }
    json.endArray();

    json.key("returnType");
    if (type->returnType) serializeType(json, type->returnType);
    else                  json.null();

    json.key("location");
    serializeLocation(json, type->loc);
    json.endObject();
}

void JSONDumper::serializeNullableType(JSONWriter& json, NullableTypeAST* type) {
    json.beginObject();
    json.kv("kind", "NullableType");
    json.key("inner");
    if (type->inner) serializeType(json, type->inner);
    else             json.null();
    json.key("location");
    serializeLocation(json, type->loc);
    json.endObject();
}

// ─── Location ───────────────────────────────────────────────────────────

void JSONDumper::serializeLocation(JSONWriter& json, const SourceLocation& loc) {
    json.beginObject();
    json.kv("line",   static_cast<uint64_t>(loc.line()));
    json.kv("column", static_cast<uint64_t>(loc.column()));
    json.endObject();
}

// ─── Diagnostics ────────────────────────────────────────────────────────

void JSONDumper::serializeDiagnostics(JSONWriter& json, const DiagnosticEngine& diagnostics) {
    json.beginObject();
    json.kv("errorCount",   static_cast<uint64_t>(diagnostics.errorCount()));
    json.kv("warningCount", static_cast<uint64_t>(diagnostics.warningCount()));

    json.key("messages");
    json.beginArray();
    for (const auto& d : diagnostics.all()) {
        json.beginObject();
        json.kv("severity", severityName(d.severity));
        json.kv("category", d.category());

        if (d.code != DiagCode(0)) json.kv("code", static_cast<uint64_t>(d.code));
        else                       json.kvNull("code");

        json.kv("message", d.message);

        if (d.file.isValid()) json.kv("file", str(d.file));
        else                  json.kvNull("file");

        json.key("location");
        serializeLocation(json, d.location);
        json.endObject();
    }
    json.endArray();
    json.endObject();
}

} // namespace lucid::cli::pipeline