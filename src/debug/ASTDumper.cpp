/**
 * @file ASTDumper.cpp
 * @brief Implementation of AST dumping as free functions.
 * 
 * This file provides human-readable dumping of AST nodes for debugging.
 * It uses the current AST definitions from BaseAST.hpp, DeclAST.hpp,
 * ExprAST.hpp, StmtAST.hpp, and TypeAST.hpp.
 */

#include "ASTDumper.hpp"
#include "debug/DebugUtils.hpp"

namespace debug {

namespace {

// =============================================================================
// Forward declaration – the main dispatch function
// =============================================================================

void dumpNode(std::string& out, const BaseAST* node, const StringPool* pool, int indentLevel);

// =============================================================================
// Indentation helpers
// =============================================================================

static std::string getIndent(int level) {
    std::string result;
    for (int i = 0; i < level; ++i) {
        result += "  ";
    }
    return result;
}

static void printLine(std::string& out, int indentLevel, const std::string& text) {
    out += getIndent(indentLevel) + text + "\n";
}

static void printNodeHeader(std::string& out, int indentLevel, const BaseAST& node, const std::string& nodeName) {
    out += getIndent(indentLevel) + nodeName;
    out += " (kind=" + kindToString(node.kind) + ")";
    if (node.loc.isKnown()) {
        out += " at " + std::to_string(node.loc.line()) + ":" + std::to_string(node.loc.column());
    }
    out += "\n";
}

// =============================================================================
// Type formatting helpers
// =============================================================================

std::string formatType(const TypeAST* type, const StringPool* pool) {
    if (!type) return "<unresolved>";

    switch (type->kind) {
        case ASTKind::PrimitiveType: {
            auto* p = static_cast<const PrimitiveTypeAST*>(type);
            return primitiveKindToString(p->primitiveKind);
        }

        case ASTKind::NamedType: {
            auto* n = static_cast<const NamedTypeAST*>(type);
            std::string res = std::string(pool->lookup(n->name));
            if (!n->genericArgs.empty()) {
                res += "<";
                for (size_t i = 0; i < n->genericArgs.size(); ++i) {
                    if (i > 0) res += ", ";
                    res += formatType(n->genericArgs[i], pool);
                }
                res += ">";
            }
            return res;
        }

        case ASTKind::NullableType: {
            auto* n = static_cast<const NullableTypeAST*>(type);
            return formatType(n->inner, pool) + "?";
        }

        case ASTKind::FallibleType: {
            auto* f = static_cast<const FallibleTypeAST*>(type);
            return formatType(f->inner, pool) + "!";
        }

        case ASTKind::CombinedType: {
            auto* c = static_cast<const CombinedTypeAST*>(type);
            return formatType(c->inner, pool) + "?!";
        }

        case ASTKind::RefType: {
            auto* r = static_cast<const RefTypeAST*>(type);
            return "&" + formatType(r->inner, pool);
        }

        case ASTKind::PtrType: {
            auto* p = static_cast<const PtrTypeAST*>(type);
            return "*" + formatType(p->inner, pool);
        }

        case ASTKind::ArrayType: {
            auto* a = static_cast<const ArrayTypeAST*>(type);
            std::string res = "[";
            if (a->isFixed()) {
                res += std::to_string(a->size);
            } else if (a->isSlice()) {
                res += "_";
            } else {
                res += "*";
            }
            res += "]";
            res += formatType(a->element, pool);
            return res;
        }

        case ASTKind::FuncType: {
            const auto* ft = static_cast<const FuncTypeAST*>(type);
            std::string res = "(";
            for (size_t i = 0; i < ft->params.size(); ++i) {
                if (i > 0) res += ", ";
                const auto* param = ft->params[i];
                if (param) {
                    res += std::string(pool->lookup(param->name)) + " ";
                    if (param->type) res += formatType(param->type, pool);
                    if (param->isVariadic) res += "...";
                } else {
                    res += "<unknown>";
                }
            }
            res += ")";
            
            if (ft->hasArrow && ft->returnType) {
                res += " -> ";
                res += formatType(ft->returnType, pool);
            }
            
            return res;
        }

        default:
            return kindToString(type->kind);
    }
}

std::string arrayKindToString(ArrayKind kind) {
    switch (kind) {
        case ArrayKind::Slice:   return "slice";
        case ArrayKind::Dynamic: return "dynamic";
        case ArrayKind::Fixed:   return "fixed";
    }
    return "unknown";
}

std::string declKeywordToString(DeclKeyword keyword) {
    switch (keyword) {
        case DeclKeyword::Let:   return "let";
        case DeclKeyword::Const: return "const";
    }
    return "unknown";
}

// =============================================================================
// Individual node dumpers
// =============================================================================

// ─── ModuleAST ──────────────────────────────────────────────────────────────

void dumpModule(std::string& out, const ModuleAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "ModuleAST");
    printLine(out, indentLevel + 1, "filePath: '" + std::string(pool->lookup(node->filePath)) + "'");
    printLine(out, indentLevel + 1, "hasErrors: " + std::string(node->hasErrors ? "true" : "false"));
    printLine(out, indentLevel + 1, "decls (count): " + std::to_string(node->decls.size()));

    for (const auto& decl : node->decls) {
        dumpNode(out, decl, pool, indentLevel + 1);
    }
}

// ─── Type nodes ─────────────────────────────────────────────────────────────

void dumpPrimitiveType(std::string& out, const PrimitiveTypeAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "PrimitiveTypeAST " + formatType(node, pool));
}

void dumpNamedType(std::string& out, const NamedTypeAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "NamedTypeAST '" + std::string(pool->lookup(node->name)) + "'";
    if (!node->genericArgs.empty()) {
        header += " <";
        for (size_t i = 0; i < node->genericArgs.size(); ++i) {
            if (i > 0) header += ", ";
            header += formatType(node->genericArgs[i], pool);
        }
        header += ">";
    }
    printNodeHeader(out, indentLevel, *node, header);
}

void dumpNullableType(std::string& out, const NullableTypeAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "NullableTypeAST");
    if (node->inner) dumpNode(out, node->inner, pool, indentLevel + 1);
}

void dumpFallibleType(std::string& out, const FallibleTypeAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "FallibleTypeAST");
    if (node->inner) dumpNode(out, node->inner, pool, indentLevel + 1);
}

void dumpCombinedType(std::string& out, const CombinedTypeAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "CombinedTypeAST T?!");
    if (node->inner) dumpNode(out, node->inner, pool, indentLevel + 1);
}

void dumpArrayType(std::string& out, const ArrayTypeAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "ArrayTypeAST (" + arrayKindToString(node->arrayKind) + ")";
    if (node->isFixed()) {
        header += " size=" + std::to_string(node->size);
    }
    printNodeHeader(out, indentLevel, *node, header);
    if (node->element) dumpNode(out, node->element, pool, indentLevel + 1);
}

void dumpRefType(std::string& out, const RefTypeAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "RefTypeAST");
    if (node->inner) dumpNode(out, node->inner, pool, indentLevel + 1);
}

void dumpPtrType(std::string& out, const PtrTypeAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "PtrTypeAST");
    if (node->inner) dumpNode(out, node->inner, pool, indentLevel + 1);
}

void dumpFuncType(std::string& out, const FuncTypeAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "FuncTypeAST";
    printNodeHeader(out, indentLevel, *node, header);
    
    // Dump parameters
    out += getIndent(indentLevel + 1) + "params: (";
    for (size_t i = 0; i < node->params.size(); ++i) {
        if (i > 0) out += ", ";
        const auto* param = node->params[i];
        if (param) {
            out += std::string(pool->lookup(param->name));
            if (param->type) out += " " + formatType(param->type, pool);
            if (param->isVariadic) out += "...";
        } else {
            out += "<unknown>";
        }
    }
    out += ")\n";
    
    if (node->hasArrow && node->returnType) {
        out += getIndent(indentLevel + 1) + "-> " + formatType(node->returnType, pool) + "\n";
    }
}

// ─── Declaration nodes ─────────────────────────────────────────────────────

void dumpImportDecl(std::string& out, const ImportDeclAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "ImportDeclAST path='" + std::string(pool->lookup(node->path)) + "'";
    header += " alias='" + std::string(pool->lookup(node->alias)) + "'";
    printNodeHeader(out, indentLevel, *node, header);
}

void dumpVarDecl(std::string& out, const VarDeclAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "VarDeclAST " + declKeywordToString(node->keyword) + " '" + std::string(pool->lookup(node->name)) + "'";
    if (node->type) header += " : " + formatType(node->type, pool);
    if (node->isConst) header += " [const]";
    printNodeHeader(out, indentLevel, *node, header);
    if (node->init) dumpNode(out, node->init, pool, indentLevel + 1);
    for (const auto& attr : node->attributes) dumpNode(out, attr, pool, indentLevel + 1);
}

void dumpParam(std::string& out, const ParamAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "ParamAST '" + std::string(pool->lookup(node->name)) + "'";
    if (node->type) header += " : " + formatType(node->type, pool);
    if (node->isVariadic) header += "...";
    if (node->isConst) header += " [const]";
    printNodeHeader(out, indentLevel, *node, header);
}

void dumpGenericParamDecl(std::string& out, const GenericParamDeclAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "GenericParamDeclAST '" + std::string(pool->lookup(node->name)) + "'";
    if (!node->constraints.empty()) {
        header += " : ";
        for (size_t i = 0; i < node->constraints.size(); ++i) {
            if (i > 0) header += " + ";
            header += formatType(node->constraints[i], pool);
        }
    }
    printNodeHeader(out, indentLevel, *node, header);
}

void dumpFuncDecl(std::string& out, const FuncDeclAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "FuncDeclAST " + declKeywordToString(node->keyword) + " '" + std::string(pool->lookup(node->name)) + "'";
    if (!node->genericParams.empty()) {
        header += " <";
        for (size_t i = 0; i < node->genericParams.size(); ++i) {
            if (i > 0) header += ", ";
            if (node->genericParams[i]) {
                header += std::string(pool->lookup(node->genericParams[i]->name));
            }
        }
        header += ">";
    }
    if (node->isConst) header += " [const]";
    printNodeHeader(out, indentLevel, *node, header);
    
    if (node->funcType) {
        dumpNode(out, node->funcType, pool, indentLevel + 1);
    }
    if (node->body) {
        dumpNode(out, node->body, pool, indentLevel + 1);
    }
    for (const auto& attr : node->attributes) {
        dumpNode(out, attr, pool, indentLevel + 1);
    }
}

void dumpFieldDecl(std::string& out, const FieldDeclAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "FieldDeclAST '" + std::string(pool->lookup(node->name)) + "'";
    if (node->type) header += " : " + formatType(node->type, pool);
    if (node->isConst) header += " [const]";
    printNodeHeader(out, indentLevel, *node, header);
    if (node->defaultVal) dumpNode(out, node->defaultVal, pool, indentLevel + 1);
    for (const auto& attr : node->attributes) dumpNode(out, attr, pool, indentLevel + 1);
}

void dumpStructDecl(std::string& out, const StructDeclAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "StructDeclAST '" + std::string(pool->lookup(node->name)) + "'";
    if (!node->genericParams.empty()) {
        header += " <";
        for (size_t i = 0; i < node->genericParams.size(); ++i) {
            if (i > 0) header += ", ";
            if (node->genericParams[i]) {
                header += std::string(pool->lookup(node->genericParams[i]->name));
            }
        }
        header += ">";
    }
    printNodeHeader(out, indentLevel, *node, header);
    
    // Dump trait refs
    if (!node->traitRefs.empty()) {
        out += getIndent(indentLevel + 1) + "traits: ";
        for (size_t i = 0; i < node->traitRefs.size(); ++i) {
            if (i > 0) out += ", ";
            out += formatType(node->traitRefs[i], pool);
        }
        out += "\n";
    }
    
    // Dump fields
    for (const auto& field : node->fields) {
        dumpNode(out, field, pool, indentLevel + 1);
    }
    
    // Dump generic params
    for (const auto& gp : node->genericParams) {
        dumpNode(out, gp, pool, indentLevel + 1);
    }
    
    // Dump attributes
    for (const auto& attr : node->attributes) {
        dumpNode(out, attr, pool, indentLevel + 1);
    }
}

void dumpEnumVariant(std::string& out, const EnumVariantAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "EnumVariantAST '" + std::string(pool->lookup(node->name)) + "'";
    header += " = " + std::to_string(node->value);
    printNodeHeader(out, indentLevel, *node, header);
    for (const auto& attr : node->attributes) dumpNode(out, attr, pool, indentLevel + 1);
}

void dumpEnumDecl(std::string& out, const EnumDeclAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "EnumDeclAST '" + std::string(pool->lookup(node->name)) + "'";
    if (node->backingType) {
        header += " : " + formatType(node->backingType, pool);
    }
    printNodeHeader(out, indentLevel, *node, header);
    for (const auto& variant : node->variants) dumpNode(out, variant, pool, indentLevel + 1);
    for (const auto& attr : node->attributes) dumpNode(out, attr, pool, indentLevel + 1);
}

void dumpTraitFieldDecl(std::string& out, const TraitFieldDeclAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "TraitFieldDeclAST '" + std::string(pool->lookup(node->name)) + "'";
    if (node->type) header += " : " + formatType(node->type, pool);
    if (node->isConst) header += " [const]";
    printNodeHeader(out, indentLevel, *node, header);
    for (const auto& attr : node->attributes) dumpNode(out, attr, pool, indentLevel + 1);
}

void dumpTraitDecl(std::string& out, const TraitDeclAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "TraitDeclAST '" + std::string(pool->lookup(node->name)) + "'";
    if (!node->genericParams.empty()) {
        header += " <";
        for (size_t i = 0; i < node->genericParams.size(); ++i) {
            if (i > 0) header += ", ";
            if (node->genericParams[i]) {
                header += std::string(pool->lookup(node->genericParams[i]->name));
            }
        }
        header += ">";
    }
    printNodeHeader(out, indentLevel, *node, header);
    for (const auto& field : node->fields) dumpNode(out, field, pool, indentLevel + 1);
    for (const auto& gp : node->genericParams) dumpNode(out, gp, pool, indentLevel + 1);
    for (const auto& attr : node->attributes) dumpNode(out, attr, pool, indentLevel + 1);
}

// ─── Expression nodes ──────────────────────────────────────────────────────

void dumpLiteralExpr(std::string& out, const LiteralExprAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "LiteralExprAST " + literalKindToString(node->kind) + " '" + std::string(pool->lookup(node->value)) + "'";
    printNodeHeader(out, indentLevel, *node, header);
}

void dumpIdentifierExpr(std::string& out, const IdentifierExprAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "IdentifierExprAST '" + std::string(pool->lookup(node->name)) + "'";
    if (!node->genericArgs.empty()) {
        header += " <";
        for (size_t i = 0; i < node->genericArgs.size(); ++i) {
            if (i > 0) header += ", ";
            header += formatType(node->genericArgs[i], pool);
        }
        header += ">";
    }
    printNodeHeader(out, indentLevel, *node, header);
}

void dumpArrayLiteralExpr(std::string& out, const ArrayLiteralExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "ArrayLiteralExprAST");
    for (const auto& elem : node->elements) dumpNode(out, elem, pool, indentLevel + 1);
}

void dumpFieldInit(std::string& out, const FieldInitAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "FieldInitAST '" + std::string(pool->lookup(node->name)) + "'";
    printNodeHeader(out, indentLevel, *node, header);
    if (node->value) dumpNode(out, node->value, pool, indentLevel + 1);
}

void dumpStructLiteralExpr(std::string& out, const StructLiteralExprAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "StructLiteralExprAST '" + std::string(pool->lookup(node->typeName)) + "'";
    if (!node->genericArgs.empty()) {
        header += " <";
        for (size_t i = 0; i < node->genericArgs.size(); ++i) {
            if (i > 0) header += ", ";
            header += formatType(node->genericArgs[i], pool);
        }
        header += ">";
    }
    printNodeHeader(out, indentLevel, *node, header);
    for (const auto& init : node->inits) dumpNode(out, init, pool, indentLevel + 1);
}

void dumpBinaryExpr(std::string& out, const BinaryExprAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "BinaryExprAST " + binaryOpToString(node->op);
    printNodeHeader(out, indentLevel, *node, header);
    if (node->left) dumpNode(out, node->left, pool, indentLevel + 1);
    if (node->right) dumpNode(out, node->right, pool, indentLevel + 1);
}

void dumpUnaryExpr(std::string& out, const UnaryExprAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "UnaryExprAST " + unaryOpToString(node->op);
    printNodeHeader(out, indentLevel, *node, header);
    if (node->operand) dumpNode(out, node->operand, pool, indentLevel + 1);
}

void dumpCallExpr(std::string& out, const CallExprAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "CallExprAST";
    if (node->hasArgPack) header += " [argpack]";
    printNodeHeader(out, indentLevel, *node, header);
    
    if (node->callee) dumpNode(out, node->callee, pool, indentLevel + 1);
    
    if (!node->genericArgs.empty()) {
        out += getIndent(indentLevel + 1) + "genericArgs: ";
        for (size_t i = 0; i < node->genericArgs.size(); ++i) {
            if (i > 0) out += ", ";
            out += formatType(node->genericArgs[i], pool);
        }
        out += "\n";
    }
    
    for (const auto& arg : node->args) {
        dumpNode(out, arg, pool, indentLevel + 1);
    }
}

void dumpIndexExpr(std::string& out, const IndexExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "IndexExprAST");
    if (node->target) dumpNode(out, node->target, pool, indentLevel + 1);
    if (node->index) dumpNode(out, node->index, pool, indentLevel + 1);
}

void dumpSliceExpr(std::string& out, const SliceExprAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "SliceExprAST";
    if (node->isExclusive) header += " (exclusive)";
    printNodeHeader(out, indentLevel, *node, header);
    if (node->target) dumpNode(out, node->target, pool, indentLevel + 1);
    if (node->start) dumpNode(out, node->start, pool, indentLevel + 1);
    if (node->end) dumpNode(out, node->end, pool, indentLevel + 1);
}

void dumpFieldAccessExpr(std::string& out, const FieldAccessExprAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "FieldAccessExprAST ." + std::string(pool->lookup(node->fieldName));
    if (!node->genericArgs.empty()) {
        header += " <";
        for (size_t i = 0; i < node->genericArgs.size(); ++i) {
            if (i > 0) header += ", ";
            header += formatType(node->genericArgs[i], pool);
        }
        header += ">";
    }
    printNodeHeader(out, indentLevel, *node, header);
    if (node->object) dumpNode(out, node->object, pool, indentLevel + 1);
}

void dumpModuleAccessExpr(std::string& out, const ModuleAccessExprAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "ModuleAccessExprAST " + std::string(pool->lookup(node->moduleName)) + ":" + std::string(pool->lookup(node->memberName));
    if (!node->genericArgs.empty()) {
        header += " <";
        for (size_t i = 0; i < node->genericArgs.size(); ++i) {
            if (i > 0) header += ", ";
            header += formatType(node->genericArgs[i], pool);
        }
        header += ">";
    }
    printNodeHeader(out, indentLevel, *node, header);
}

void dumpNullableChainExpr(std::string& out, const NullableChainExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "NullableChainExprAST");
    if (node->object) dumpNode(out, node->object, pool, indentLevel + 1);
    if (!node->steps.empty()) {
        out += getIndent(indentLevel + 1) + "steps: ";
        for (size_t i = 0; i < node->steps.size(); ++i) {
            if (i > 0) out += ", ";
            out += std::string(pool->lookup(node->steps[i]));
        }
        out += "\n";
    }
}

void dumpNullCoalesceExpr(std::string& out, const NullCoalesceExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "NullCoalesceExprAST ??");
    if (node->value) dumpNode(out, node->value, pool, indentLevel + 1);
    if (node->fallback) dumpNode(out, node->fallback, pool, indentLevel + 1);
}

void dumpAssignExpr(std::string& out, const AssignExprAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "AssignExprAST " + assignOpToString(node->op);
    printNodeHeader(out, indentLevel, *node, header);
    if (node->lhs) dumpNode(out, node->lhs, pool, indentLevel + 1);
    if (node->rhs) dumpNode(out, node->rhs, pool, indentLevel + 1);
}

void dumpPipelineStep(std::string& out, const PipelineStepAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "PipelineStepAST");
    if (node->callable) dumpNode(out, node->callable, pool, indentLevel + 1);
    if (!node->packArgs.empty()) {
        out += getIndent(indentLevel + 1) + "packArgs:\n";
        for (const auto& arg : node->packArgs) {
            dumpNode(out, arg, pool, indentLevel + 2);
        }
    }
}

void dumpPipelineExpr(std::string& out, const PipelineExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "PipelineExprAST |>");
    if (node->seed) dumpNode(out, node->seed, pool, indentLevel + 1);
    for (const auto& step : node->steps) {
        if (step) dumpNode(out, step, pool, indentLevel + 1);
    }
}

void dumpComposeOperand(std::string& out, const ComposeOperandAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "ComposeOperandAST");
    if (node->callable) dumpNode(out, node->callable, pool, indentLevel + 1);
    if (!node->genericArgs.empty()) {
        out += getIndent(indentLevel + 1) + "genericArgs: ";
        for (size_t i = 0; i < node->genericArgs.size(); ++i) {
            if (i > 0) out += ", ";
            out += formatType(node->genericArgs[i], pool);
        }
        out += "\n";
    }
}

void dumpComposeExpr(std::string& out, const ComposeExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "ComposeExprAST +>");
    if (node->left) dumpNode(out, node->left, pool, indentLevel + 1);
    for (const auto& op : node->operands) {
        if (op) dumpNode(out, op, pool, indentLevel + 1);
    }
}

void dumpAnonFuncExpr(std::string& out, const AnonFuncExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "AnonFuncExprAST");
    if (node->funcType) dumpNode(out, node->funcType, pool, indentLevel + 1);
    if (node->body) dumpNode(out, node->body, pool, indentLevel + 1);
}

void dumpIfExpr(std::string& out, const IfExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "IfExprAST");
    if (node->condition) dumpNode(out, node->condition, pool, indentLevel + 1);
    if (node->thenBranch) dumpNode(out, node->thenBranch, pool, indentLevel + 1);
    if (node->elseBranch) dumpNode(out, node->elseBranch, pool, indentLevel + 1);
}

void dumpRangeExpr(std::string& out, const RangeExprAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "RangeExprAST";
    if (node->isExclusive) header += " (exclusive)";
    printNodeHeader(out, indentLevel, *node, header);
    if (node->lo) dumpNode(out, node->lo, pool, indentLevel + 1);
    if (node->hi) dumpNode(out, node->hi, pool, indentLevel + 1);
}

void dumpIntrinsicCallExpr(std::string& out, const IntrinsicCallExprAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "IntrinsicCallExprAST #" + std::string(pool->lookup(node->intrinsicName));
    printNodeHeader(out, indentLevel, *node, header);
    for (const auto& arg : node->args) dumpNode(out, arg, pool, indentLevel + 1);
}

// ─── Statement nodes ──────────────────────────────────────────────────────

void dumpBlockStmt(std::string& out, const BlockStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "BlockStmtAST");
    for (const auto& stmt : node->stmts) dumpNode(out, stmt, pool, indentLevel + 1);
}

void dumpExprStmt(std::string& out, const ExprStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "ExprStmtAST");
    if (node->expr) dumpNode(out, node->expr, pool, indentLevel + 1);
}

void dumpDeclStmt(std::string& out, const DeclStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "DeclStmtAST");
    if (node->decl) dumpNode(out, node->decl, pool, indentLevel + 1);
}

void dumpIfStmt(std::string& out, const IfStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "IfStmtAST");
    if (node->condition) dumpNode(out, node->condition, pool, indentLevel + 1);
    if (node->thenBranch) dumpNode(out, node->thenBranch, pool, indentLevel + 1);
    if (node->elseBranch) dumpNode(out, node->elseBranch, pool, indentLevel + 1);
}

void dumpSwitchCase(std::string& out, const SwitchCaseAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "SwitchCaseAST");
    out += getIndent(indentLevel + 1) + "values: ";
    for (size_t i = 0; i < node->values.size(); ++i) {
        if (i > 0) out += ", ";
        dumpNode(out, node->values[i], pool, indentLevel + 2);
    }
    out += "\n";
    if (node->body) dumpNode(out, node->body, pool, indentLevel + 1);
}

void dumpSwitchStmt(std::string& out, const SwitchStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "SwitchStmtAST");
    if (node->subject) dumpNode(out, node->subject, pool, indentLevel + 1);
    for (const auto& c : node->cases) {
        dumpNode(out, c, pool, indentLevel + 1);
    }
    if (node->defaultBody) {
        out += getIndent(indentLevel + 1) + "default:\n";
        dumpNode(out, node->defaultBody, pool, indentLevel + 2);
    }
}

void dumpForStmt(std::string& out, const ForStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "ForStmtAST");
    if (node->indexVar) {
        out += getIndent(indentLevel + 1) + "index: " + std::string(pool->lookup(node->indexVar->name));
        if (node->indexVar->type) out += " " + formatType(node->indexVar->type, pool);
        out += "\n";
    }
    if (node->valueVar) {
        out += getIndent(indentLevel + 1) + "value: " + std::string(pool->lookup(node->valueVar->name));
        if (node->valueVar->type) out += " " + formatType(node->valueVar->type, pool);
        out += "\n";
    }
    if (node->iterable) dumpNode(out, node->iterable, pool, indentLevel + 1);
    if (node->step) dumpNode(out, node->step, pool, indentLevel + 1);
    if (node->body) dumpNode(out, node->body, pool, indentLevel + 1);
}

void dumpWhileStmt(std::string& out, const WhileStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "WhileStmtAST");
    if (node->condition) dumpNode(out, node->condition, pool, indentLevel + 1);
    if (node->body) dumpNode(out, node->body, pool, indentLevel + 1);
}

void dumpDoWhileStmt(std::string& out, const DoWhileStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "DoWhileStmtAST");
    if (node->body) dumpNode(out, node->body, pool, indentLevel + 1);
    if (node->condition) dumpNode(out, node->condition, pool, indentLevel + 1);
}

void dumpReturnStmt(std::string& out, const ReturnStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "ReturnStmtAST");
    if (node->value) dumpNode(out, node->value, pool, indentLevel + 1);
}

void dumpBreakStmt(std::string& out, const BreakStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "BreakStmtAST");
}

void dumpContinueStmt(std::string& out, const ContinueStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "ContinueStmtAST");
}

void dumpFuncRefStmt(std::string& out, const FuncRefStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "FuncRefStmtAST");
    if (node->target) dumpNode(out, node->target, pool, indentLevel + 1);
}

// ─── Concurrency nodes ─────────────────────────────────────────────────────

void dumpAsyncStmt(std::string& out, const AsyncStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "AsyncStmtAST");
    if (node->target) dumpNode(out, node->target, pool, indentLevel + 1);
    if (node->call) dumpNode(out, node->call, pool, indentLevel + 1);
}

void dumpAwaitStmt(std::string& out, const AwaitStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "AwaitStmtAST");
    for (const auto& target : node->targets) {
        dumpNode(out, target, pool, indentLevel + 1);
    }
}

void dumpSpawnStmt(std::string& out, const SpawnStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "SpawnStmtAST");
    if (node->target) dumpNode(out, node->target, pool, indentLevel + 1);
    if (node->call) dumpNode(out, node->call, pool, indentLevel + 1);
}

void dumpJoinStmt(std::string& out, const JoinStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "JoinStmtAST");
    for (const auto& target : node->targets) {
        dumpNode(out, target, pool, indentLevel + 1);
    }
}

// ─── Attribute nodes ──────────────────────────────────────────────────────

void dumpAttribute(std::string& out, const AttributeAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "AttributeAST @" + std::string(pool->lookup(node->name));
    if (!node->args.empty()) {
        header += "(";
        for (size_t i = 0; i < node->args.size(); ++i) {
            if (i > 0) header += ", ";
            header += std::string(pool->lookup(node->args[i]->value));
        }
        header += ")";
    }
    printNodeHeader(out, indentLevel, *node, header);
}

// ─── Unknown node ─────────────────────────────────────────────────────────

void dumpUnknown(std::string& out, const BaseAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "Unknown");
}

// =============================================================================
// Main dispatch function
// =============================================================================

void dumpNode(std::string& out, const BaseAST* node, const StringPool* pool, int indentLevel) {
    if (!node) return;

    switch (node->kind) {
        // Root
        case ASTKind::Program:             dumpModule(out, static_cast<const ModuleAST*>(node), pool, indentLevel); break;
        
        // Types
        case ASTKind::PrimitiveType:       dumpPrimitiveType(out, static_cast<const PrimitiveTypeAST*>(node), pool, indentLevel); break;
        case ASTKind::NamedType:           dumpNamedType(out, static_cast<const NamedTypeAST*>(node), pool, indentLevel); break;
        case ASTKind::NullableType:        dumpNullableType(out, static_cast<const NullableTypeAST*>(node), pool, indentLevel); break;
        case ASTKind::FallibleType:        dumpFallibleType(out, static_cast<const FallibleTypeAST*>(node), pool, indentLevel); break;
        case ASTKind::CombinedType:        dumpCombinedType(out, static_cast<const CombinedTypeAST*>(node), pool, indentLevel); break;
        case ASTKind::ArrayType:           dumpArrayType(out, static_cast<const ArrayTypeAST*>(node), pool, indentLevel); break;
        case ASTKind::RefType:             dumpRefType(out, static_cast<const RefTypeAST*>(node), pool, indentLevel); break;
        case ASTKind::PtrType:             dumpPtrType(out, static_cast<const PtrTypeAST*>(node), pool, indentLevel); break;
        case ASTKind::FuncType:            dumpFuncType(out, static_cast<const FuncTypeAST*>(node), pool, indentLevel); break;
        
        // Declarations
        case ASTKind::ImportDecl:          dumpImportDecl(out, static_cast<const ImportDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::VarDecl:             dumpVarDecl(out, static_cast<const VarDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::Param:               dumpParam(out, static_cast<const ParamAST*>(node), pool, indentLevel); break;
        case ASTKind::GenericParamDecl:    dumpGenericParamDecl(out, static_cast<const GenericParamDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::FuncDecl:            dumpFuncDecl(out, static_cast<const FuncDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::FieldDecl:           dumpFieldDecl(out, static_cast<const FieldDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::StructDecl:          dumpStructDecl(out, static_cast<const StructDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::EnumVariant:         dumpEnumVariant(out, static_cast<const EnumVariantAST*>(node), pool, indentLevel); break;
        case ASTKind::EnumDecl:            dumpEnumDecl(out, static_cast<const EnumDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::TraitFieldDecl:      dumpTraitFieldDecl(out, static_cast<const TraitFieldDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::TraitDecl:           dumpTraitDecl(out, static_cast<const TraitDeclAST*>(node), pool, indentLevel); break;
        
        // Expressions
        case ASTKind::LiteralExpr:         dumpLiteralExpr(out, static_cast<const LiteralExprAST*>(node), pool, indentLevel); break;
        case ASTKind::IdentifierExpr:      dumpIdentifierExpr(out, static_cast<const IdentifierExprAST*>(node), pool, indentLevel); break;
        case ASTKind::ArrayLiteralExpr:    dumpArrayLiteralExpr(out, static_cast<const ArrayLiteralExprAST*>(node), pool, indentLevel); break;
        case ASTKind::FieldInit:           dumpFieldInit(out, static_cast<const FieldInitAST*>(node), pool, indentLevel); break;
        case ASTKind::StructLiteralExpr:   dumpStructLiteralExpr(out, static_cast<const StructLiteralExprAST*>(node), pool, indentLevel); break;
        case ASTKind::BinaryExpr:          dumpBinaryExpr(out, static_cast<const BinaryExprAST*>(node), pool, indentLevel); break;
        case ASTKind::UnaryExpr:           dumpUnaryExpr(out, static_cast<const UnaryExprAST*>(node), pool, indentLevel); break;
        case ASTKind::CallExpr:            dumpCallExpr(out, static_cast<const CallExprAST*>(node), pool, indentLevel); break;
        case ASTKind::IndexExpr:           dumpIndexExpr(out, static_cast<const IndexExprAST*>(node), pool, indentLevel); break;
        case ASTKind::SliceExpr:           dumpSliceExpr(out, static_cast<const SliceExprAST*>(node), pool, indentLevel); break;
        case ASTKind::FieldAccessExpr:     dumpFieldAccessExpr(out, static_cast<const FieldAccessExprAST*>(node), pool, indentLevel); break;
        case ASTKind::ModuleAccessExpr:    dumpModuleAccessExpr(out, static_cast<const ModuleAccessExprAST*>(node), pool, indentLevel); break;
        case ASTKind::NullableChainExpr:   dumpNullableChainExpr(out, static_cast<const NullableChainExprAST*>(node), pool, indentLevel); break;
        case ASTKind::NullCoalesceExpr:    dumpNullCoalesceExpr(out, static_cast<const NullCoalesceExprAST*>(node), pool, indentLevel); break;
        case ASTKind::AssignExpr:          dumpAssignExpr(out, static_cast<const AssignExprAST*>(node), pool, indentLevel); break;
        case ASTKind::PipelineStep:        dumpPipelineStep(out, static_cast<const PipelineStepAST*>(node), pool, indentLevel); break;
        case ASTKind::PipelineExpr:        dumpPipelineExpr(out, static_cast<const PipelineExprAST*>(node), pool, indentLevel); break;
        case ASTKind::ComposeOperand:      dumpComposeOperand(out, static_cast<const ComposeOperandAST*>(node), pool, indentLevel); break;
        case ASTKind::ComposeExpr:         dumpComposeExpr(out, static_cast<const ComposeExprAST*>(node), pool, indentLevel); break;
        case ASTKind::AnonFuncExpr:        dumpAnonFuncExpr(out, static_cast<const AnonFuncExprAST*>(node), pool, indentLevel); break;
        case ASTKind::IfExpr:              dumpIfExpr(out, static_cast<const IfExprAST*>(node), pool, indentLevel); break;
        case ASTKind::RangeExpr:           dumpRangeExpr(out, static_cast<const RangeExprAST*>(node), pool, indentLevel); break;
        case ASTKind::IntrinsicCallExpr:   dumpIntrinsicCallExpr(out, static_cast<const IntrinsicCallExprAST*>(node), pool, indentLevel); break;
        
        // Statements
        case ASTKind::BlockStmt:           dumpBlockStmt(out, static_cast<const BlockStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::ExprStmt:            dumpExprStmt(out, static_cast<const ExprStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::DeclStmt:            dumpDeclStmt(out, static_cast<const DeclStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::IfStmt:              dumpIfStmt(out, static_cast<const IfStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::SwitchCase:          dumpSwitchCase(out, static_cast<const SwitchCaseAST*>(node), pool, indentLevel); break;
        case ASTKind::SwitchStmt:          dumpSwitchStmt(out, static_cast<const SwitchStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::ForStmt:             dumpForStmt(out, static_cast<const ForStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::WhileStmt:           dumpWhileStmt(out, static_cast<const WhileStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::DoWhileStmt:         dumpDoWhileStmt(out, static_cast<const DoWhileStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::ReturnStmt:          dumpReturnStmt(out, static_cast<const ReturnStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::BreakStmt:           dumpBreakStmt(out, static_cast<const BreakStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::ContinueStmt:        dumpContinueStmt(out, static_cast<const ContinueStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::FuncRefStmt:         dumpFuncRefStmt(out, static_cast<const FuncRefStmtAST*>(node), pool, indentLevel); break;
        
        // Concurrency
        case ASTKind::AsyncExpr:           dumpAsyncStmt(out, static_cast<const AsyncStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::AwaitExpr:           dumpAwaitStmt(out, static_cast<const AwaitStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::SpawnExpr:           dumpSpawnStmt(out, static_cast<const SpawnStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::JoinExpr:            dumpJoinStmt(out, static_cast<const JoinStmtAST*>(node), pool, indentLevel); break;
        
        // Attributes
        case ASTKind::Attribute:           dumpAttribute(out, static_cast<const AttributeAST*>(node), pool, indentLevel); break;
        
        default:                           dumpUnknown(out, node, pool, indentLevel); break;
    }
}

} // anonymous namespace

// =============================================================================
// Public entry point
// =============================================================================

std::string dumpAST(const BaseAST* node, const StringPool& pool, int verbosity) {
    std::string out;
    if (node) {
        dumpNode(out, node, &pool, 0);
    } else {
        out = "<null>\n";
    }
    return out;
}

} // namespace debug