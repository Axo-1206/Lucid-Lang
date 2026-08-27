/// @file cli/frontend/JSONDumper.cpp
/// @brief Implementation of complete JSON serialization.

#include "JSONDumper.hpp"
#include "core/ast/TypeAST.hpp"

#include <sstream>
#include <iomanip>
#include <iostream>

namespace cli {
namespace frontend {

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
    if (!file.is_open()) {
        return false;
    }
    
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

// ─── Module Serialization ──────────────────────────────────────────────

void JSONDumper::serializeModules(JSONWriter& json) {
    json.beginArray();
    for (auto* module : modules) {
        if (module) {
            serializeModule(json, module);
        }
    }
    json.endArray();
}

void JSONDumper::serializeModule(JSONWriter& json, ModuleAST* module) {
    json.beginObject();
    json.kv("kind", "Module");
    json.kv("filePath", getModulePath(module->filePath));
    
    json.key("imports");
    json.beginArray();
    for (auto imp : module->imports) {
        json.string(getModulePath(imp));
    }
    json.endArray();
    
    json.kv("hasErrors", module->hasErrors);
    
    json.key("declarations");
    json.beginArray();
    for (auto* decl : module->decls) {
        if (decl) {
            serializeDecl(json, decl);
        }
    }
    json.endArray();
    json.endObject();
}

// ─── Declaration Serializers ──────────────────────────────────────────

void JSONDumper::serializeDecl(JSONWriter& json, DeclAST* decl) {
    if (!decl) { json.null(); return; }
    
    switch (decl->kind) {
        case ASTKind::ImportDecl:       serializeImportDecl(json, decl->as<ImportDeclAST>()); break;
        case ASTKind::VarDecl:          serializeVarDecl(json, decl->as<VarDeclAST>()); break;
        case ASTKind::Param:            serializeParam(json, decl->as<ParamAST>()); break;
        case ASTKind::FuncDecl:         serializeFuncDecl(json, decl->as<FuncDeclAST>()); break;
        case ASTKind::StructDecl:       serializeStructDecl(json, decl->as<StructDeclAST>()); break;
        case ASTKind::EnumDecl:         serializeEnumDecl(json, decl->as<EnumDeclAST>()); break;
        case ASTKind::TraitDecl:        serializeTraitDecl(json, decl->as<TraitDeclAST>()); break;
        case ASTKind::FieldDecl:        serializeFieldDecl(json, decl->as<FieldDeclAST>()); break;
        case ASTKind::TraitFieldDecl:   serializeTraitFieldDecl(json, decl->as<TraitFieldDeclAST>()); break;
        case ASTKind::EnumVariant:      serializeEnumVariant(json, decl->as<EnumVariantAST>()); break;
        case ASTKind::GenericParamDecl: serializeGenericParam(json, decl->as<GenericParamDeclAST>()); break;
        default:
            json.beginObject();
            // Explicitly call core::astKindToString and pass the result as a string literal
            json.kv("kind", std::string(astKindToString(decl->kind)));
            json.kv("name", str(decl->name));
            json.endObject();
            break;
    }
}

// ─── Import Decl ─────────────────────────────────────────────────────

void JSONDumper::serializeImportDecl(JSONWriter& json, ImportDeclAST* decl) {
    json.beginObject();
    json.kv("kind", "ImportDecl");
    json.kv("path", str(decl->path));
    json.kv("alias", str(decl->alias));
    json.endObject();
}

// ─── Var Decl ────────────────────────────────────────────────────────

void JSONDumper::serializeVarDecl(JSONWriter& json, VarDeclAST* decl) {
    json.beginObject();
    json.kv("kind", "VarDecl");
    json.kv("name", str(decl->name));
    json.kv("keyword", declKeywordToString(decl->keyword));
    
    if (decl->type) {
        json.key("type");
        serializeType(json, decl->type);
    }
    if (decl->init) {
        json.key("init");
        serializeExpr(json, decl->init);
    }
    
    json.key("location");
    serializeLocation(json, decl->loc);
    json.endObject();
}

void JSONDumper::serializeParam(JSONWriter& json, ParamAST* param) {
    json.beginObject();
    json.kv("kind", "Param");
    json.kv("name", str(param->name));
    json.kv("isVariadic", param->isVariadic);
    json.kv("isConstParam", param->isConstParam);
    
    if (param->type) {
        json.key("type");
        serializeType(json, param->type);
    }
    
    json.key("location");
    serializeLocation(json, param->loc);
    json.endObject();
}

void JSONDumper::serializeFuncDecl(JSONWriter& json, FuncDeclAST* decl) {
    json.beginObject();
    json.kv("kind", "FuncDecl");
    json.kv("name", str(decl->name));
    json.kv("keyword", declKeywordToString(decl->keyword));
    
    json.key("genericParams");
    json.beginArray();
    for (auto* param : decl->genericParams) {
        if (param) serializeGenericParam(json, param);
    }
    json.endArray();
    
    if (decl->funcType) {
        json.key("funcType");
        serializeType(json, decl->funcType);
    }
    if (decl->body) {
        json.key("body");
        serializeStmt(json, decl->body);
    }
    
    json.kv("isForeignFunction", decl->isForeignFunction);
    json.kv("isInline", decl->isInline);
    json.kv("hasClosure", decl->hasClosure);
    
    json.key("location");
    serializeLocation(json, decl->loc);
    json.endObject();
}

void JSONDumper::serializeStructDecl(JSONWriter& json, StructDeclAST* decl) {
    json.beginObject();
    json.kv("kind", "StructDecl");
    json.kv("name", str(decl->name));
    
    json.key("genericParams");
    json.beginArray();
    for (auto* param : decl->genericParams) {
        if (param) serializeGenericParam(json, param);
    }
    json.endArray();
    
    json.key("fields");
    json.beginArray();
    for (auto* field : decl->fields) {
        if (field) serializeFieldDecl(json, field);
    }
    json.endArray();
    
    json.key("traitRefs");
    json.beginArray();
    for (auto* trait : decl->traitRefs) {
        if (trait) serializeType(json, trait);
    }
    json.endArray();
    
    json.kv("isPacked", decl->isPacked);
    json.key("location");
    serializeLocation(json, decl->loc);
    json.endObject();
}

void JSONDumper::serializeEnumDecl(JSONWriter& json, EnumDeclAST* decl) {
    json.beginObject();
    json.kv("kind", "EnumDecl");
    json.kv("name", str(decl->name));
    
    if (decl->backingType) {
        json.key("backingType");
        serializeType(json, decl->backingType);
    }
    
    json.key("variants");
    json.beginArray();
    for (auto* variant : decl->variants) {
        if (variant) serializeEnumVariant(json, variant);
    }
    json.endArray();
    
    json.key("location");
    serializeLocation(json, decl->loc);
    json.endObject();
}

void JSONDumper::serializeTraitDecl(JSONWriter& json, TraitDeclAST* decl) {
    json.beginObject();
    json.kv("kind", "TraitDecl");
    json.kv("name", str(decl->name));
    
    json.key("genericParams");
    json.beginArray();
    for (auto* param : decl->genericParams) {
        if (param) serializeGenericParam(json, param);
    }
    json.endArray();
    
    json.key("fields");
    json.beginArray();
    for (auto* field : decl->fields) {
        if (field) serializeTraitFieldDecl(json, field);
    }
    json.endArray();
    
    json.key("location");
    serializeLocation(json, decl->loc);
    json.endObject();
}

void JSONDumper::serializeFieldDecl(JSONWriter& json, FieldDeclAST* field) {
    json.beginObject();
    json.kv("kind", "FieldDecl");
    json.kv("name", str(field->name));
    json.kv("isConstField", field->isConstField);
    
    if (field->type) {
        json.key("type");
        serializeType(json, field->type);
    }
    if (field->defaultVal) {
        json.key("defaultVal");
        serializeExpr(json, field->defaultVal);
    }
    
    json.key("location");
    serializeLocation(json, field->loc);
    json.endObject();
}

void JSONDumper::serializeTraitFieldDecl(JSONWriter& json, TraitFieldDeclAST* field) {
    json.beginObject();
    json.kv("kind", "TraitFieldDecl");
    json.kv("name", str(field->name));
    json.kv("isConstField", field->isConstField);
    
    if (field->type) {
        json.key("type");
        serializeType(json, field->type);
    }
    
    json.key("location");
    serializeLocation(json, field->loc);
    json.endObject();
}

void JSONDumper::serializeEnumVariant(JSONWriter& json, EnumVariantAST* variant) {
    json.beginObject();
    json.kv("kind", "EnumVariant");
    json.kv("name", str(variant->name));
    json.kv("value", static_cast<int64_t>(variant->value));
    json.key("location");
    serializeLocation(json, variant->loc);
    json.endObject();
}

void JSONDumper::serializeGenericParam(JSONWriter& json, GenericParamDeclAST* param) {
    json.beginObject();
    json.kv("kind", "GenericParamDecl");
    json.kv("name", str(param->name));
    
    json.key("constraints");
    json.beginArray();
    for (auto* constraint : param->constraints) {
        if (constraint) serializeType(json, constraint);
    }
    json.endArray();
    
    json.key("location");
    serializeLocation(json, param->loc);
    json.endObject();
}

// ─── Statement Serializers ──────────────────────────────────────────────

void JSONDumper::serializeStmt(JSONWriter& json, StmtAST* stmt) {
    if (!stmt) { json.null(); return; }
    
    switch (stmt->kind) {
        case ASTKind::BlockStmt:     serializeBlockStmt(json, stmt->as<BlockStmtAST>()); break;
        case ASTKind::ExprStmt:      serializeExprStmt(json, stmt->as<ExprStmtAST>()); break;
        case ASTKind::DeclStmt:      serializeDeclStmt(json, stmt->as<DeclStmtAST>()); break;
        case ASTKind::IfStmt:        serializeIfStmt(json, stmt->as<IfStmtAST>()); break;
        case ASTKind::SwitchStmt:    serializeSwitchStmt(json, stmt->as<SwitchStmtAST>()); break;
        case ASTKind::ForStmt:       serializeForStmt(json, stmt->as<ForStmtAST>()); break;
        case ASTKind::WhileStmt:     serializeWhileStmt(json, stmt->as<WhileStmtAST>()); break;
        case ASTKind::DoWhileStmt:   serializeDoWhileStmt(json, stmt->as<DoWhileStmtAST>()); break;
        case ASTKind::ReturnStmt:    serializeReturnStmt(json, stmt->as<ReturnStmtAST>()); break;
        case ASTKind::BreakStmt:     serializeBreakStmt(json, stmt->as<BreakStmtAST>()); break;
        case ASTKind::ContinueStmt:  serializeContinueStmt(json, stmt->as<ContinueStmtAST>()); break;
        case ASTKind::FuncRefStmt:   serializeFuncRefStmt(json, stmt->as<FuncRefStmtAST>()); break;
        case ASTKind::AsyncStmt:     serializeAsyncStmt(json, stmt->as<AsyncStmtAST>()); break;
        case ASTKind::AwaitStmt:     serializeAwaitStmt(json, stmt->as<AwaitStmtAST>()); break;
        case ASTKind::SpawnStmt:     serializeSpawnStmt(json, stmt->as<SpawnStmtAST>()); break;
        case ASTKind::JoinStmt:      serializeJoinStmt(json, stmt->as<JoinStmtAST>()); break;
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

void JSONDumper::serializeExprStmt(JSONWriter& json, ExprStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "ExprStmt");
    if (stmt->expr) {
        json.key("expr");
        serializeExpr(json, stmt->expr);
    }
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeDeclStmt(JSONWriter& json, DeclStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "DeclStmt");
    if (stmt->decl) {
        json.key("decl");
        serializeDecl(json, stmt->decl);
    }
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeIfStmt(JSONWriter& json, IfStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "IfStmt");
    if (stmt->condition) {
        json.key("condition");
        serializeExpr(json, stmt->condition);
    }
    if (stmt->thenBranch) {
        json.key("thenBranch");
        serializeStmt(json, stmt->thenBranch);
    }
    if (stmt->elseBranch) {
        json.key("elseBranch");
        serializeStmt(json, stmt->elseBranch);
    }
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeSwitchStmt(JSONWriter& json, SwitchStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "SwitchStmt");
    if (stmt->subject) {
        json.key("subject");
        serializeExpr(json, stmt->subject);
    }
    json.key("cases");
    json.beginArray();
    for (auto* case_ : stmt->cases) {
        if (case_) serializeSwitchCase(json, case_);
    }
    json.endArray();
    if (stmt->defaultBody) {
        json.key("defaultBody");
        serializeStmt(json, stmt->defaultBody);
    }
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

void JSONDumper::serializeForStmt(JSONWriter& json, ForStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "ForStmt");
    if (stmt->indexVar) {
        json.key("indexVar");
        serializeParam(json, stmt->indexVar);
    }
    if (stmt->valueVar) {
        json.key("valueVar");
        serializeParam(json, stmt->valueVar);
    }
    if (stmt->iterable) {
        json.key("iterable");
        serializeExpr(json, stmt->iterable);
    }
    if (stmt->step) {
        json.key("step");
        serializeExpr(json, stmt->step);
    }
    if (stmt->body) {
        json.key("body");
        serializeStmt(json, stmt->body);
    }
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeWhileStmt(JSONWriter& json, WhileStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "WhileStmt");
    if (stmt->condition) {
        json.key("condition");
        serializeExpr(json, stmt->condition);
    }
    if (stmt->body) {
        json.key("body");
        serializeStmt(json, stmt->body);
    }
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeDoWhileStmt(JSONWriter& json, DoWhileStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "DoWhileStmt");
    if (stmt->body) {
        json.key("body");
        serializeStmt(json, stmt->body);
    }
    if (stmt->condition) {
        json.key("condition");
        serializeExpr(json, stmt->condition);
    }
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeReturnStmt(JSONWriter& json, ReturnStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "ReturnStmt");
    if (stmt->value) {
        json.key("value");
        serializeExpr(json, stmt->value);
    }
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeBreakStmt(JSONWriter& json, BreakStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "BreakStmt");
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeContinueStmt(JSONWriter& json, ContinueStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "ContinueStmt");
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeFuncRefStmt(JSONWriter& json, FuncRefStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "FuncRefStmt");
    if (stmt->target) {
        json.key("target");
        serializeExpr(json, stmt->target);
    }
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeAsyncStmt(JSONWriter& json, AsyncStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "AsyncStmt");
    if (stmt->binding) {
        json.key("binding");
        serializeVarDecl(json, stmt->binding);
    }
    if (stmt->call) {
        json.key("call");
        serializeExpr(json, stmt->call);
    }
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeAwaitStmt(JSONWriter& json, AwaitStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "AwaitStmt");
    json.key("targets");
    json.beginArray();
    for (auto* target : stmt->targets) {
        if (target) serializeExpr(json, target);
    }
    json.endArray();
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeSpawnStmt(JSONWriter& json, SpawnStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "SpawnStmt");
    if (stmt->binding) {
        json.key("binding");
        serializeVarDecl(json, stmt->binding);
    }
    if (stmt->call) {
        json.key("call");
        serializeExpr(json, stmt->call);
    }
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

void JSONDumper::serializeJoinStmt(JSONWriter& json, JoinStmtAST* stmt) {
    json.beginObject();
    json.kv("kind", "JoinStmt");
    json.key("targets");
    json.beginArray();
    for (auto* target : stmt->targets) {
        if (target) serializeExpr(json, target);
    }
    json.endArray();
    json.key("location");
    serializeLocation(json, stmt->loc);
    json.endObject();
}

// ─── Expression Serializers ─────────────────────────────────────────────

void JSONDumper::serializeExpr(JSONWriter& json, ExprAST* expr) {
    if (!expr) { json.null(); return; }
    
    switch (expr->kind) {
        case ASTKind::LiteralExpr:       serializeLiteralExpr(json, expr->as<LiteralExprAST>()); break;
        case ASTKind::IdentifierExpr:    serializeIdentifierExpr(json, expr->as<IdentifierExprAST>()); break;
        case ASTKind::ArrayLiteralExpr:  serializeArrayLiteralExpr(json, expr->as<ArrayLiteralExprAST>()); break;
        case ASTKind::StructLiteralExpr: serializeStructLiteralExpr(json, expr->as<StructLiteralExprAST>()); break;
        case ASTKind::BinaryExpr:        serializeBinaryExpr(json, expr->as<BinaryExprAST>()); break;
        case ASTKind::UnaryExpr:         serializeUnaryExpr(json, expr->as<UnaryExprAST>()); break;
        case ASTKind::CallExpr:          serializeCallExpr(json, expr->as<CallExprAST>()); break;
        case ASTKind::IntrinsicCallExpr: serializeIntrinsicCallExpr(json, expr->as<IntrinsicCallExprAST>()); break;
        case ASTKind::IndexExpr:         serializeIndexExpr(json, expr->as<IndexExprAST>()); break;
        case ASTKind::SliceExpr:         serializeSliceExpr(json, expr->as<SliceExprAST>()); break;
        case ASTKind::FieldAccessExpr:   serializeFieldAccessExpr(json, expr->as<FieldAccessExprAST>()); break;
        case ASTKind::ModuleAccessExpr:  serializeModuleAccessExpr(json, expr->as<ModuleAccessExprAST>()); break;
        case ASTKind::AssignExpr:        serializeAssignExpr(json, expr->as<AssignExprAST>()); break;
        case ASTKind::NullCoalesceExpr:  serializeNullCoalesceExpr(json, expr->as<NullCoalesceExprAST>()); break;
        case ASTKind::PipelineExpr:      serializePipelineExpr(json, expr->as<PipelineExprAST>()); break;
        case ASTKind::ComposeExpr:       serializeComposeExpr(json, expr->as<ComposeExprAST>()); break;
        case ASTKind::AnonFuncExpr:      serializeAnonFuncExpr(json, expr->as<AnonFuncExprAST>()); break;
        case ASTKind::IfExpr:            serializeIfExpr(json, expr->as<IfExprAST>()); break;
        case ASTKind::RangeExpr:         serializeRangeExpr(json, expr->as<RangeExprAST>()); break;
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
    
    json.key("resolvedType");
    if (expr->hasType()) {
        serializeType(json, expr->resolvedType);
    } else {
        json.null();
    }
    
    json.kv("valueState", valueStateToString(expr->valueState));
    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeIdentifierExpr(JSONWriter& json, IdentifierExprAST* expr) {
    json.beginObject();
    json.kv("kind", "IdentifierExpr");
    json.kv("name", str(expr->name));
    
    json.key("genericArgs");
    json.beginArray();
    for (auto* arg : expr->genericArgs) {
        if (arg) serializeType(json, arg);
    }
    json.endArray();
    
    json.kv("resolved", expr->resolvedDecl != nullptr);
    json.key("resolvedDecl");
    if (expr->resolvedDecl) {
        json.string(str(expr->resolvedDecl->name));
    } else {
        json.null();
    }
    
    json.key("resolvedType");
    if (expr->hasType()) {
        serializeType(json, expr->resolvedType);
    } else {
        json.null();
    }
    
    json.kv("valueState", valueStateToString(expr->valueState));
    json.kv("isConst", expr->isConst);
    json.kv("isLValue", expr->isLValue);
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
    
    json.kv("isConst", expr->isConst);
    json.key("resolvedType");
    if (expr->hasType()) {
        serializeType(json, expr->resolvedType);
    } else {
        json.null();
    }
    json.kv("valueState", valueStateToString(expr->valueState));
    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeStructLiteralExpr(JSONWriter& json, StructLiteralExprAST* expr) {
    json.beginObject();
    json.kv("kind", "StructLiteralExpr");
    json.kv("typeName", str(expr->typeName));
    
    json.key("genericArgs");
    json.beginArray();
    for (auto* arg : expr->genericArgs) {
        if (arg) serializeType(json, arg);
    }
    json.endArray();
    
    json.key("inits");
    json.beginArray();
    for (auto* init : expr->inits) {
        if (init) serializeFieldInit(json, init);
    }
    json.endArray();
    
    json.kv("isConst", expr->isConst);
    json.key("resolvedType");
    if (expr->hasType()) {
        serializeType(json, expr->resolvedType);
    } else {
        json.null();
    }
    json.kv("valueState", valueStateToString(expr->valueState));
    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeFieldInit(JSONWriter& json, FieldInitAST* init) {
    json.beginObject();
    json.kv("kind", "FieldInit");
    json.kv("name", str(init->name));
    if (init->value) {
        json.key("value");
        serializeExpr(json, init->value);
    }
    json.key("location");
    serializeLocation(json, init->loc);
    json.endObject();
}

void JSONDumper::serializeBinaryExpr(JSONWriter& json, BinaryExprAST* expr) {
    json.beginObject();
    json.kv("kind", "BinaryExpr");
    json.kv("op", binaryOpToString(expr->op));
    if (expr->left) {
        json.key("left");
        serializeExpr(json, expr->left);
    }
    if (expr->right) {
        json.key("right");
        serializeExpr(json, expr->right);
    }
    json.kv("isConst", expr->isConst);
    json.key("resolvedType");
    if (expr->hasType()) {
        serializeType(json, expr->resolvedType);
    } else {
        json.null();
    }
    json.kv("valueState", valueStateToString(expr->valueState));
    json.kv("isLValue", expr->isLValue);
    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeUnaryExpr(JSONWriter& json, UnaryExprAST* expr) {
    json.beginObject();
    json.kv("kind", "UnaryExpr");
    json.kv("op", unaryOpToString(expr->op));
    if (expr->operand) {
        json.key("operand");
        serializeExpr(json, expr->operand);
    }
    json.kv("isConst", expr->isConst);
    json.key("resolvedType");
    if (expr->hasType()) {
        serializeType(json, expr->resolvedType);
    } else {
        json.null();
    }
    json.kv("valueState", valueStateToString(expr->valueState));
    json.kv("isLValue", expr->isLValue);
    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeCallExpr(JSONWriter& json, CallExprAST* expr) {
    json.beginObject();
    json.kv("kind", "CallExpr");
    if (expr->callee) {
        json.key("callee");
        serializeExpr(json, expr->callee);
    }
    json.key("genericArgs");
    json.beginArray();
    for (auto* arg : expr->genericArgs) {
        if (arg) serializeType(json, arg);
    }
    json.endArray();
    json.key("args");
    json.beginArray();
    for (auto* arg : expr->args) {
        if (arg) serializeExpr(json, arg);
    }
    json.endArray();
    json.kv("hasArgPack", expr->hasArgPack);
    json.kv("isConst", expr->isConst);
    json.key("resolvedType");
    if (expr->hasType()) {
        serializeType(json, expr->resolvedType);
    } else {
        json.null();
    }
    json.kv("valueState", valueStateToString(expr->valueState));
    json.kv("isLValue", expr->isLValue);
    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeIntrinsicCallExpr(JSONWriter& json, IntrinsicCallExprAST* expr) {
    json.beginObject();
    json.kv("kind", "IntrinsicCallExpr");
    json.kv("intrinsicName", str(expr->intrinsicName));
    json.key("args");
    json.beginArray();
    for (auto* arg : expr->args) {
        if (arg) serializeExpr(json, arg);
    }
    json.endArray();
    json.kv("isConst", expr->isConst);
    json.key("resolvedType");
    if (expr->hasType()) {
        serializeType(json, expr->resolvedType);
    } else {
        json.null();
    }
    json.kv("valueState", valueStateToString(expr->valueState));
    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeIndexExpr(JSONWriter& json, IndexExprAST* expr) {
    json.beginObject();
    json.kv("kind", "IndexExpr");
    if (expr->target) {
        json.key("target");
        serializeExpr(json, expr->target);
    }
    if (expr->index) {
        json.key("index");
        serializeExpr(json, expr->index);
    }
    json.kv("isConst", expr->isConst);
    json.key("resolvedType");
    if (expr->hasType()) {
        serializeType(json, expr->resolvedType);
    } else {
        json.null();
    }
    json.kv("valueState", valueStateToString(expr->valueState));
    json.kv("isLValue", expr->isLValue);
    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeSliceExpr(JSONWriter& json, SliceExprAST* expr) {
    json.beginObject();
    json.kv("kind", "SliceExpr");
    if (expr->target) {
        json.key("target");
        serializeExpr(json, expr->target);
    }
    json.key("start");
    if (expr->start) {
        serializeExpr(json, expr->start);
    } else {
        json.null();
    }
    json.key("end");
    if (expr->end) {
        serializeExpr(json, expr->end);
    } else {
        json.null();
    }
    json.kv("isExclusive", expr->isExclusive);
    json.kv("isConst", expr->isConst);
    json.key("resolvedType");
    if (expr->hasType()) {
        serializeType(json, expr->resolvedType);
    } else {
        json.null();
    }
    json.kv("valueState", valueStateToString(expr->valueState));
    json.kv("isLValue", expr->isLValue);
    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeFieldAccessExpr(JSONWriter& json, FieldAccessExprAST* expr) {
    json.beginObject();
    json.kv("kind", "FieldAccessExpr");
    if (expr->object) {
        json.key("object");
        serializeExpr(json, expr->object);
    }
    json.kv("fieldName", str(expr->fieldName));
    
    json.key("resolvedDecl");
    if (expr->resolvedDecl) {
        json.string(str(expr->resolvedDecl->name));
    } else {
        json.null();
    }
    
    json.key("ownerType");
    if (expr->ownerType) {
        json.string(str(expr->ownerType->name));
    } else {
        json.null();
    }
    
    json.kv("isEnumAccess", expr->isEnumAccess);
    json.key("fieldIndex");
    if (expr->fieldIndex != SIZE_MAX) {
        json.number(static_cast<uint64_t>(expr->fieldIndex));
    } else {
        json.null();
    }
    
    json.kv("isConst", expr->isConst);
    json.key("resolvedType");
    if (expr->hasType()) {
        serializeType(json, expr->resolvedType);
    } else {
        json.null();
    }
    json.kv("valueState", valueStateToString(expr->valueState));
    json.kv("isLValue", expr->isLValue);
    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeModuleAccessExpr(JSONWriter& json, ModuleAccessExprAST* expr) {
    json.beginObject();
    json.kv("kind", "ModuleAccessExpr");
    json.kv("moduleName", str(expr->moduleName));
    json.kv("memberName", str(expr->memberName));
    
    json.key("genericArgs");
    json.beginArray();
    for (auto* arg : expr->genericArgs) {
        if (arg) serializeType(json, arg);
    }
    json.endArray();
    
    json.key("resolvedDecl");
    if (expr->resolvedDecl) {
        json.string(str(expr->resolvedDecl->name));
    } else {
        json.null();
    }
    
    json.kv("resolved", expr->resolvedDecl != nullptr);
    json.kv("isConst", expr->isConst);
    json.key("resolvedType");
    if (expr->hasType()) {
        serializeType(json, expr->resolvedType);
    } else {
        json.null();
    }
    json.kv("valueState", valueStateToString(expr->valueState));
    json.kv("isLValue", expr->isLValue);
    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeAssignExpr(JSONWriter& json, AssignExprAST* expr) {
    json.beginObject();
    json.kv("kind", "AssignExpr");
    json.kv("op", assignOpToString(expr->op));
    if (expr->lhs) {
        json.key("lhs");
        serializeExpr(json, expr->lhs);
    }
    if (expr->rhs) {
        json.key("rhs");
        serializeExpr(json, expr->rhs);
    }
    json.kv("isConst", expr->isConst);
    json.key("resolvedType");
    if (expr->hasType()) {
        serializeType(json, expr->resolvedType);
    } else {
        json.null();
    }
    json.kv("valueState", valueStateToString(expr->valueState));
    json.kv("isLValue", expr->isLValue);
    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeNullCoalesceExpr(JSONWriter& json, NullCoalesceExprAST* expr) {
    json.beginObject();
    json.kv("kind", "NullCoalesceExpr");
    if (expr->value) {
        json.key("value");
        serializeExpr(json, expr->value);
    }
    if (expr->fallback) {
        json.key("fallback");
        serializeExpr(json, expr->fallback);
    }
    json.kv("isConst", expr->isConst);
    json.key("resolvedType");
    if (expr->hasType()) {
        serializeType(json, expr->resolvedType);
    } else {
        json.null();
    }
    json.kv("valueState", valueStateToString(expr->valueState));
    json.kv("isLValue", expr->isLValue);
    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializePipelineExpr(JSONWriter& json, PipelineExprAST* expr) {
    json.beginObject();
    json.kv("kind", "PipelineExpr");
    if (expr->seed) {
        json.key("seed");
        serializeExpr(json, expr->seed);
    }
    json.key("steps");
    json.beginArray();
    for (auto* step : expr->steps) {
        if (step) serializePipelineStep(json, step);
    }
    json.endArray();
    json.kv("isConst", expr->isConst);
    json.key("resolvedType");
    if (expr->hasType()) {
        serializeType(json, expr->resolvedType);
    } else {
        json.null();
    }
    json.kv("valueState", valueStateToString(expr->valueState));
    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializePipelineStep(JSONWriter& json, PipelineStepAST* step) {
    json.beginObject();
    json.kv("kind", "PipelineStep");
    if (step->callable) {
        json.key("callable");
        serializeExpr(json, step->callable);
    }
    json.key("packArgs");
    json.beginArray();
    for (auto* arg : step->packArgs) {
        if (arg) serializeExpr(json, arg);
    }
    json.endArray();
    json.key("location");
    serializeLocation(json, step->loc);
    json.endObject();
}

void JSONDumper::serializeComposeExpr(JSONWriter& json, ComposeExprAST* expr) {
    json.beginObject();
    json.kv("kind", "ComposeExpr");
    if (expr->left) {
        json.key("left");
        serializeExpr(json, expr->left);
    }
    json.key("operands");
    json.beginArray();
    for (auto* operand : expr->operands) {
        if (operand) serializeComposeOperand(json, operand);
    }
    json.endArray();
    json.kv("isConst", expr->isConst);
    json.key("resolvedType");
    if (expr->hasType()) {
        serializeType(json, expr->resolvedType);
    } else {
        json.null();
    }
    json.kv("valueState", valueStateToString(expr->valueState));
    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeComposeOperand(JSONWriter& json, ComposeOperandAST* operand) {
    json.beginObject();
    json.kv("kind", "ComposeOperand");
    if (operand->callable) {
        json.key("callable");
        serializeExpr(json, operand->callable);
    }
    json.key("genericArgs");
    json.beginArray();
    for (auto* arg : operand->genericArgs) {
        if (arg) serializeType(json, arg);
    }
    json.endArray();
    json.key("location");
    serializeLocation(json, operand->loc);
    json.endObject();
}

void JSONDumper::serializeAnonFuncExpr(JSONWriter& json, AnonFuncExprAST* expr) {
    json.beginObject();
    json.kv("kind", "AnonFuncExpr");
    if (expr->funcType) {
        json.key("funcType");
        serializeType(json, expr->funcType);
    }
    if (expr->body) {
        json.key("body");
        serializeStmt(json, expr->body);
    }
    json.kv("hasClosure", expr->hasClosure);
    json.kv("isReturned", expr->isReturned);
    
    json.key("captures");
    json.beginArray();
    for (const auto& cap : expr->captures) {
        json.beginObject();
        json.kv("decl", str(cap.decl->name));
        json.kv("byReference", cap.byReference);
        json.kv("index", static_cast<uint64_t>(cap.index));
        json.endObject();
    }
    json.endArray();
    
    json.kv("isConst", expr->isConst);
    json.key("resolvedType");
    if (expr->hasType()) {
        serializeType(json, expr->resolvedType);
    } else {
        json.null();
    }
    json.kv("valueState", valueStateToString(expr->valueState));
    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeIfExpr(JSONWriter& json, IfExprAST* expr) {
    json.beginObject();
    json.kv("kind", "IfExpr");
    if (expr->condition) {
        json.key("condition");
        serializeExpr(json, expr->condition);
    }
    if (expr->thenBranch) {
        json.key("thenBranch");
        serializeExpr(json, expr->thenBranch);
    }
    if (expr->elseBranch) {
        json.key("elseBranch");
        serializeExpr(json, expr->elseBranch);
    }
    json.kv("isConst", expr->isConst);
    json.key("resolvedType");
    if (expr->hasType()) {
        serializeType(json, expr->resolvedType);
    } else {
        json.null();
    }
    json.kv("valueState", valueStateToString(expr->valueState));
    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

void JSONDumper::serializeRangeExpr(JSONWriter& json, RangeExprAST* expr) {
    json.beginObject();
    json.kv("kind", "RangeExpr");
    if (expr->lo) {
        json.key("lo");
        serializeExpr(json, expr->lo);
    }
    if (expr->hi) {
        json.key("hi");
        serializeExpr(json, expr->hi);
    }
    json.kv("isExclusive", expr->isExclusive);
    json.kv("isConst", expr->isConst);
    json.key("resolvedType");
    if (expr->hasType()) {
        serializeType(json, expr->resolvedType);
    } else {
        json.null();
    }
    json.kv("valueState", valueStateToString(expr->valueState));
    json.key("location");
    serializeLocation(json, expr->loc);
    json.endObject();
}

// ─── Type Serializers ─────────────────────────────────────────────────

void JSONDumper::serializeType(JSONWriter& json, TypeAST* type) {
    if (!type) { json.null(); return; }
    
    switch (type->kind) {
        case ASTKind::PrimitiveType:    serializePrimitiveType(json, type->as<PrimitiveTypeAST>()); break;
        case ASTKind::NamedType:        serializeNamedType(json, type->as<NamedTypeAST>()); break;
        case ASTKind::ModuleTypeAccess: serializeModuleTypeAccess(json, type->as<ModuleTypeAccessAST>()); break;
        case ASTKind::ArrayType:        serializeArrayType(json, type->as<ArrayTypeAST>()); break;
        case ASTKind::NullableType:     serializeNullableType(json, type->as<NullableTypeAST>()); break;
        case ASTKind::FallibleType:     serializeFallibleType(json, type->as<FallibleTypeAST>()); break;
        case ASTKind::CombinedType:     serializeCombinedType(json, type->as<CombinedTypeAST>()); break;
        case ASTKind::RefType:          serializeRefType(json, type->as<RefTypeAST>()); break;
        case ASTKind::PtrType:          serializePtrType(json, type->as<PtrTypeAST>()); break;
        case ASTKind::FuncType:         serializeFuncType(json, type->as<FuncTypeAST>()); break;
        case ASTKind::FutureType:       serializeFutureType(json, type->as<FutureTypeAST>()); break;
        case ASTKind::ThreadType:       serializeThreadType(json, type->as<ThreadTypeAST>()); break;
        default:
            json.beginObject();
            json.kv("kind", astKindToString(type->kind));
            json.endObject();
            break;
    }
}

void JSONDumper::serializePrimitiveType(JSONWriter& json, PrimitiveTypeAST* type) {
    json.beginObject();
    json.kv("kind", "PrimitiveType");
    json.kv("primitiveKind", primitiveKindToString(type->primitiveKind));
    json.key("location");
    serializeLocation(json, type->loc);
    json.endObject();
}

void JSONDumper::serializeNamedType(JSONWriter& json, NamedTypeAST* type) {
    json.beginObject();
    json.kv("kind", "NamedType");
    json.kv("name", str(type->name));
    
    json.key("genericArgs");
    json.beginArray();
    for (auto* arg : type->genericArgs) {
        if (arg) serializeType(json, arg);
    }
    json.endArray();
    
    json.key("resolvedDecl");
    if (type->resolvedDecl) {
        json.string(str(type->resolvedDecl->name));
    } else {
        json.null();
    }
    
    json.key("location");
    serializeLocation(json, type->loc);
    json.endObject();
}

void JSONDumper::serializeModuleTypeAccess(JSONWriter& json, ModuleTypeAccessAST* type) {
    json.beginObject();
    json.kv("kind", "ModuleTypeAccess");
    json.kv("moduleName", str(type->moduleName));
    json.kv("typeName", str(type->typeName));
    
    json.key("genericArgs");
    json.beginArray();
    for (auto* arg : type->genericArgs) {
        if (arg) serializeType(json, arg);
    }
    json.endArray();
    
    json.key("location");
    serializeLocation(json, type->loc);
    json.endObject();
}

void JSONDumper::serializeArrayType(JSONWriter& json, ArrayTypeAST* type) {
    json.beginObject();
    json.kv("kind", "ArrayType");
    json.kv("arrayKind", arrayKindToString(type->arrayKind));
    if (type->isFixed()) {
        json.kv("size", static_cast<uint64_t>(type->size));
    }
    json.key("element");
    if (type->element) {
        serializeType(json, type->element);
    } else {
        json.null();
    }
    json.key("location");
    serializeLocation(json, type->loc);
    json.endObject();
}

void JSONDumper::serializeNullableType(JSONWriter& json, NullableTypeAST* type) {
    json.beginObject();
    json.kv("kind", "NullableType");
    json.key("inner");
    if (type->inner) {
        serializeType(json, type->inner);
    } else {
        json.null();
    }
    json.key("location");
    serializeLocation(json, type->loc);
    json.endObject();
}

void JSONDumper::serializeFallibleType(JSONWriter& json, FallibleTypeAST* type) {
    json.beginObject();
    json.kv("kind", "FallibleType");
    json.key("inner");
    if (type->inner) {
        serializeType(json, type->inner);
    } else {
        json.null();
    }
    json.key("location");
    serializeLocation(json, type->loc);
    json.endObject();
}

void JSONDumper::serializeCombinedType(JSONWriter& json, CombinedTypeAST* type) {
    json.beginObject();
    json.kv("kind", "CombinedType");
    json.key("inner");
    if (type->inner) {
        serializeType(json, type->inner);
    } else {
        json.null();
    }
    json.key("location");
    serializeLocation(json, type->loc);
    json.endObject();
}

void JSONDumper::serializeRefType(JSONWriter& json, RefTypeAST* type) {
    json.beginObject();
    json.kv("kind", "RefType");
    json.key("inner");
    if (type->inner) {
        serializeType(json, type->inner);
    } else {
        json.null();
    }
    json.key("location");
    serializeLocation(json, type->loc);
    json.endObject();
}

void JSONDumper::serializePtrType(JSONWriter& json, PtrTypeAST* type) {
    json.beginObject();
    json.kv("kind", "PtrType");
    json.key("inner");
    if (type->inner) {
        serializeType(json, type->inner);
    } else {
        json.null();
    }
    json.key("location");
    serializeLocation(json, type->loc);
    json.endObject();
}

void JSONDumper::serializeFuncType(JSONWriter& json, FuncTypeAST* type) {
    json.beginObject();
    json.kv("kind", "FuncType");
    json.key("params");
    json.beginArray();
    for (auto* param : type->params) {
        if (param) serializeParam(json, param);
    }
    json.endArray();
    json.key("returnType");
    if (type->returnType) {
        serializeType(json, type->returnType);
    } else {
        json.null();
    }
    json.key("location");
    serializeLocation(json, type->loc);
    json.endObject();
}

void JSONDumper::serializeFutureType(JSONWriter& json, FutureTypeAST* type) {
    json.beginObject();
    json.kv("kind", "FutureType");
    json.key("inner");
    if (type->inner) {
        serializeType(json, type->inner);
    } else {
        json.null();
    }
    json.key("location");
    serializeLocation(json, type->loc);
    json.endObject();
}

void JSONDumper::serializeThreadType(JSONWriter& json, ThreadTypeAST* type) {
    json.beginObject();
    json.kv("kind", "ThreadType");
    json.key("inner");
    if (type->inner) {
        serializeType(json, type->inner);
    } else {
        json.null();
    }
    json.key("location");
    serializeLocation(json, type->loc);
    json.endObject();
}

// ─── Location ─────────────────────────────────────────────────────

void JSONDumper::serializeLocation(JSONWriter& json, const SourceLocation& loc, ModuleAST* module) {
    json.beginObject();
    json.kv("line", static_cast<uint64_t>(loc.line()));
    json.kv("column", static_cast<uint64_t>(loc.column()));
    if (module && module->filePath.isValid()) {
        json.kv("file", getModulePath(module->filePath));
    }
    json.endObject();
}

// ─── Diagnostics ─────────────────────────────────────────────────────

void JSONDumper::serializeDiagnostics(JSONWriter& json, const DiagnosticEngine& diagnostics) {
    json.beginObject();
    json.kv("errorCount", static_cast<uint64_t>(diagnostics.errorCount()));
    json.kv("warningCount", static_cast<uint64_t>(diagnostics.warningCount()));
    json.key("messages");
    json.beginArray();
    for (const auto& d : diagnostics.all()) {
        json.beginObject();
        json.kv("severity", severityName(d.severity));
        json.kv("category", d.category());

        if (d.code != DiagCode(0)) {
            json.kv("code", static_cast<uint64_t>(d.code));
        } else {
            json.kvNull("code");
        }

        json.kv("message", d.message);

        if (d.file.isValid()) {
            json.kv("file", str(d.file));
        } else {
            json.kvNull("file");
        }

        json.key("location");
        serializeLocation(json, d.location);
        json.endObject();
    }
    json.endArray();
    json.endObject();
}

} // namespace frontend
} // namespace cli