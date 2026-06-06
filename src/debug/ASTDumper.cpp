/**
 * @file ASTDumper.cpp
 * @brief Implementation of AST dumping as free functions.
 */

#include "ASTDumper.hpp"
#include "DebugUtils.hpp"
#include "ast/DeclAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/StmtAST.hpp"
#include "ast/TypeAST.hpp"

namespace LucDebug {

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
        result += '\t';
    }
    return result;
}

static void printLine(std::string& out, int indentLevel, const std::string& text) {
    out += getIndent(indentLevel) + text + "\n";
}

static void printNodeHeader(std::string& out, int indentLevel, const BaseAST& node, const std::string& nodeName) {
    out += getIndent(indentLevel) + "[" + std::to_string(indentLevel) + "] " + nodeName;
    out += " (kind=" + kindToString(node.kind) + ")";
    out += " at line " + std::to_string(node.loc.line()) + ", col " + std::to_string(node.loc.column()) + "\n";
}

// =============================================================================
// Type formatting helpers
// =============================================================================

std::string formatType(const TypeAST* type, const StringPool* pool) {
    if (!type) return "<unresolved>";

    switch (type->kind) {
        case ASTKind::PrimitiveType:
            return primitiveKindToString(static_cast<const PrimitiveTypeAST*>(type)->primitiveKind);

        case ASTKind::NamedType: {
            auto* n = static_cast<const NamedTypeAST*>(type);
            std::string res = toStr(pool, n->name);
            if (!n->genericArgs.empty()) {
                res += "<";
                for (size_t i = 0; i < n->genericArgs.size(); ++i) {
                    if (i > 0) res += ", ";
                    res += formatType(n->genericArgs[i].get(), pool);
                }
                res += ">";
            }
            return res;
        }

        case ASTKind::NullableType:
            return formatType(static_cast<const NullableTypeAST*>(type)->inner.get(), pool) + "?";

        case ASTKind::ResultType: {
            auto* r = static_cast<const ResultTypeAST*>(type);
            std::string innerStr = formatType(r->inner.get(), pool);
            if (r->hasErrorType()) {
                return innerStr + "!" + formatType(r->errorType.get(), pool);
            }
            return innerStr + "!";
        }

        case ASTKind::RefType:
            return "&" + formatType(static_cast<const RefTypeAST*>(type)->inner.get(), pool);

        case ASTKind::PtrType:
            return "*" + formatType(static_cast<const PtrTypeAST*>(type)->inner.get(), pool);

        case ASTKind::ArrayType: {
            auto* a = static_cast<const ArrayTypeAST*>(type);
            switch (a->arrayKind) {
                case ArrayKind::Slice:
                    return "[_, " + formatType(a->element.get(), pool) + "]";
                case ArrayKind::Dynamic:
                    return "[*, " + formatType(a->element.get(), pool) + "]";
                case ArrayKind::Fixed:
                    return "[" + std::to_string(a->size) + ", " + formatType(a->element.get(), pool) + "]";
            }
            return "<invalid array>";
        }

        case ASTKind::FuncType: {
            const auto* ft = static_cast<const FuncTypeAST*>(type);
            std::string res;

            uint32_t q = ft->qualifiers;
            if (q & QualifierBits::Async)   res += "~async ";
            if (q & QualifierBits::Nullable) res += "~nullable ";
            if (q & QualifierBits::Parallel) res += "~parallel ";

            const FuncSignature& sig = ft->sig;
            size_t paramOffset = 0;
            for (size_t g = 0; g < sig.groupSizes.size(); ++g) {
                res += "(";
                size_t groupSize = sig.groupSizes[g];
                for (size_t i = 0; i < groupSize; ++i) {
                    if (i > 0) res += ", ";
                    const auto& param = sig.allParams[paramOffset + i];
                    if (param->type) res += formatType(param->type.get(), pool);
                    if (param->isVariadic) res += "...";
                }
                res += ")";
                paramOffset += groupSize;
            }

            if (!sig.returnTypes.empty()) {
                res += " -> ";
                for (size_t i = 0; i < sig.returnTypes.size(); ++i) {
                    if (i > 0) res += ", ";
                    res += formatType(sig.returnTypes[i], pool);
                }
            }
            return res;
        }

        default:
            return kindToString(type->kind);
    }
}

std::string formatVisibility(Visibility vis) {
    switch (vis) {
        case Visibility::Package: return "pub";
        case Visibility::Export:  return "export";
        default:                  return "";
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

// =============================================================================
// Individual node dumpers (all take const BaseAST* and cast internally)
// =============================================================================

void dumpProgram(std::string& out, const ProgramAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "ProgramAST");
    out += getIndent(indentLevel) + "\tpackageName: '" + toStr(pool, node->packageName) + "'\n";
    out += getIndent(indentLevel) + "\tfilePath: '" + std::string(pool->lookup(node->filePath)) + "'\n";
    out += getIndent(indentLevel) + "\tdecls (count): " + std::to_string(node->decls.size()) + "\n";

    for (const auto& decl : node->decls) {
        dumpNode(out, decl.get(), pool, indentLevel + 1);
    }
}

void dumpPrimitiveType(std::string& out, const PrimitiveTypeAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "PrimitiveTypeAST " + formatType(node, pool));
}

void dumpNamedType(std::string& out, const NamedTypeAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "NamedTypeAST '" + toStr(pool, node->name) + "'");
    for (const auto& arg : node->genericArgs) {
        dumpNode(out, arg.get(), pool, indentLevel + 1);
    }
}

void dumpNullableType(std::string& out, const NullableTypeAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "NullableTypeAST");
    if (node->inner) dumpNode(out, node->inner.get(), pool, indentLevel + 1);
}

void dumpResultType(std::string& out, const ResultTypeAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "ResultTypeAST ";
    if (node->hasErrorType()) {
        header += formatType(node->inner.get(), pool) + "!" + formatType(node->errorType.get(), pool);
    } else {
        header += formatType(node->inner.get(), pool) + "!";
    }
    printNodeHeader(out, indentLevel, *node, header);
    dumpNode(out, node->inner.get(), pool, indentLevel + 1);
    if (node->errorType) dumpNode(out, node->errorType.get(), pool, indentLevel + 1);
}

void dumpArrayType(std::string& out, const ArrayTypeAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "ArrayTypeAST (" + arrayKindToString(node->arrayKind) + ")";
    if (node->arrayKind == ArrayKind::Fixed) {
        header += " size=" + std::to_string(node->size);
    }
    printNodeHeader(out, indentLevel, *node, header);
    if (node->element) dumpNode(out, node->element.get(), pool, indentLevel + 1);
}

void dumpGenericArrayType(std::string& out, const GenericArrayTypeAST* node, const StringPool* pool, int indentLevel) {
    std::string kindStr;
    switch (node->arrayKind) {
        case ArrayKind::Slice:   kindStr = "[_, <" + toStr(pool, node->typeParamName) + ">]"; break;
        case ArrayKind::Dynamic: kindStr = "[*, <" + toStr(pool, node->typeParamName) + ">]"; break;
        case ArrayKind::Fixed:   kindStr = "[" + std::to_string(node->size) + ", <" + toStr(pool, node->typeParamName) + ">]"; break;
    }
    printNodeHeader(out, indentLevel, *node, "GenericArrayTypeAST " + kindStr);
}

void dumpRefType(std::string& out, const RefTypeAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "RefTypeAST");
    if (node->inner) dumpNode(out, node->inner.get(), pool, indentLevel + 1);
}

void dumpPtrType(std::string& out, const PtrTypeAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "PtrTypeAST");
    if (node->inner) dumpNode(out, node->inner.get(), pool, indentLevel + 1);
}

void dumpFuncType(std::string& out, const FuncTypeAST* node, const StringPool* pool, int indentLevel) {
    // Build qualifier prefix
    std::string qualStr;
    if (node->qualifiers & QualifierBits::Async)   qualStr += "~async ";
    if (node->qualifiers & QualifierBits::Nullable) qualStr += "~nullable ";
    if (node->qualifiers & QualifierBits::Parallel) qualStr += "~parallel ";

    std::string header = "FuncTypeAST " + qualStr;
    printNodeHeader(out, indentLevel, *node, header);

    const FuncSignature& sig = node->sig;
    size_t paramOffset = 0;
    for (size_t g = 0; g < sig.groupSizes.size(); ++g) {
        out += getIndent(indentLevel + 1) + "Group " + std::to_string(g) + ": (";
        size_t groupSize = sig.groupSizes[g];
        for (size_t i = 0; i < groupSize; ++i) {
            if (i > 0) out += ", ";
            const auto& param = sig.allParams[paramOffset + i];
            if (param) {
                out += toStr(pool, param->name) + " : ";
                if (param->type) out += formatType(param->type.get(), pool);
                if (param->isVariadic) out += "...";
            }
        }
        out += ")\n";
        paramOffset += groupSize;
    }

    if (!sig.returnTypes.empty()) {
        out += getIndent(indentLevel + 1) + "-> ";
        for (size_t i = 0; i < sig.returnTypes.size(); ++i) {
            if (i > 0) out += ", ";
            out += formatType(sig.returnTypes[i], pool);
        }
        out += "\n";
    }

    // Optionally dump raw qualifiers (for debugging)
    if (!node->rawQualifiers.empty()) {
        out += getIndent(indentLevel + 1) + "rawQualifiers: ";
        for (size_t i = 0; i < node->rawQualifiers.size(); ++i) {
            if (i > 0) out += " ";
            out += toStr(pool, node->rawQualifiers[i]);
        }
        out += "\n";
    }
}

void dumpPackageDecl(std::string& out, const PackageDeclAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "PackageDeclAST '" + toStr(pool, node->name) + "'");
}

void dumpUseDecl(std::string& out, const UseDeclAST* node, const StringPool* pool, int indentLevel) {
    std::string pathStr;
    for (size_t i = 0; i < node->path.size(); ++i) {
        if (i > 0) pathStr += ".";
        pathStr += toStr(pool, node->path[i]);
    }
    std::string header = "UseDeclAST '" + pathStr + "'";
    if (node->alias) header += " as " + toStr(pool, *node->alias);
    printNodeHeader(out, indentLevel, *node, header);
}

void dumpVarDecl(std::string& out, const VarDeclAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "VarDeclAST '" + toStr(pool, node->name) + "'";
    if (node->type) header += " : " + formatType(node->type.get(), pool);
    printNodeHeader(out, indentLevel, *node, header);
    if (node->init) dumpNode(out, node->init.get(), pool, indentLevel + 1);
    for (const auto& attr : node->attributes) dumpNode(out, attr.get(), pool, indentLevel + 1);
}

void dumpFuncDecl(std::string& out, const FuncDeclAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "FuncDeclAST '" + toStr(pool, node->name) + "'";
    if (!node->genericParams.empty()) {
        header += " <";
        for (size_t i = 0; i < node->genericParams.size(); ++i) {
            if (i > 0) header += ", ";
            if (node->genericParams[i]) {
                header += toStr(pool, node->genericParams[i]->name);
            }
        }
        header += ">";
    }
    printNodeHeader(out, indentLevel, *node, header);

    // Dump function type (signature + qualifiers)
    if (node->funcType) {
        dumpNode(out, node->funcType.get(), pool, indentLevel + 1);
    }
    // Dump body
    if (node->body) {
        dumpNode(out, node->body.get(), pool, indentLevel + 1);
    }
    // Dump attributes
    for (const auto& attr : node->attributes) {
        dumpNode(out, attr.get(), pool, indentLevel + 1);
    }
}

void dumpStructDecl(std::string& out, const StructDeclAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "StructDeclAST '" + toStr(pool, node->name) + "'");
    for (const auto& field : node->fields) dumpNode(out, field.get(), pool, indentLevel + 1);
    for (const auto& attr : node->attributes) dumpNode(out, attr.get(), pool, indentLevel + 1);
}

void dumpFieldDecl(std::string& out, const FieldDeclAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "FieldDeclAST '" + toStr(pool, node->name) + "'";
    if (node->type) header += " : " + formatType(node->type.get(), pool);
    printNodeHeader(out, indentLevel, *node, header);
    if (node->defaultVal) dumpNode(out, node->defaultVal.get(), pool, indentLevel + 1);
}

void dumpEnumDecl(std::string& out, const EnumDeclAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "EnumDeclAST '" + toStr(pool, node->name) + "'");
    for (const auto& variant : node->variants) dumpNode(out, variant.get(), pool, indentLevel + 1);
}

void dumpEnumVariant(std::string& out, const EnumVariantAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "EnumVariantAST '" + toStr(pool, node->name) + "'";
    if (node->explicitValue) header += " = " + std::to_string(*node->explicitValue);
    printNodeHeader(out, indentLevel, *node, header);
}

void dumpTraitMethod(std::string& out, const TraitMethodAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "TraitMethodAST '" + toStr(pool, node->name) + "'");

    // Dump function type (signature + qualifiers)
    if (node->funcType) {
        dumpNode(out, node->funcType.get(), pool, indentLevel + 1);
    }
}

void dumpTraitDecl(std::string& out, const TraitDeclAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "TraitDeclAST '" + toStr(pool, node->name) + "'");
    for (const auto& method : node->methods) dumpNode(out, method.get(), pool, indentLevel + 1);
}

void dumpTraitRef(std::string& out, const TraitRefAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "TraitRefAST '" + toStr(pool, node->name) + "'";
    if (!node->genericArgs.empty()) {
        header += "<";
        for (size_t i = 0; i < node->genericArgs.size(); ++i) {
            if (i > 0) header += ", ";
            header += formatType(node->genericArgs[i].get(), pool);
        }
        header += ">";
    }
    printNodeHeader(out, indentLevel, *node, header);
}

void dumpImplDecl(std::string& out, const ImplDeclAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "ImplDeclAST";
    if (!formatVisibility(node->visibility).empty()) {
        header = formatVisibility(node->visibility) + " " + header;
    }
    if (node->targetType) {
        header += " for " + formatType(node->targetType.get(), pool);
    }
    if (!node->genericParams.empty()) {
        header += " <";
        for (size_t i = 0; i < node->genericParams.size(); ++i) {
            if (i > 0) header += ", ";
            if (node->genericParams[i]) {
                header += toStr(pool, node->genericParams[i]->name);
            }
        }
        header += ">";
    }
    if (node->receiverAlias.isValid()) {
        header += " as " + toStr(pool, node->receiverAlias);
    }
    if (node->traitRef) {
        header += " : " + toStr(pool, node->traitRef->name);
        if (!node->traitRef->genericArgs.empty()) {
            header += "<";
            for (size_t i = 0; i < node->traitRef->genericArgs.size(); ++i) {
                if (i > 0) header += ", ";
                header += formatType(node->traitRef->genericArgs[i].get(), pool);
            }
            header += ">";
        }
    }
    printNodeHeader(out, indentLevel, *node, header);

    for (const auto& gp : node->genericParams) {
        if (gp) dumpNode(out, gp.get(), pool, indentLevel + 1);
    }
    for (const auto& method : node->methods) {
        if (method) dumpNode(out, method.get(), pool, indentLevel + 1);
    }
}

void dumpMethodDecl(std::string& out, const MethodDeclAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "MethodDeclAST '" + toStr(pool, node->name) + "'";
    if (node->isInlineBody()) {
        header += " (inline body)";
    } else if (node->isPlainAssignment()) {
        header += " (plain assignment)";
    } else if (node->isInjectionAssignment()) {
        header += " (injection assignment)";
        if (node->receiverArg.isValid()) {
            header += " receiver: " + toStr(pool, node->receiverArg);
        }
    }
    printNodeHeader(out, indentLevel, *node, header);
    
    if (node->funcType) {
        dumpNode(out, node->funcType.get(), pool, indentLevel + 1);
    }
    if (node->assignmentRef) {
        dumpNode(out, node->assignmentRef.get(), pool, indentLevel + 1);
    }
    if (node->body) {
        dumpNode(out, node->body.get(), pool, indentLevel + 1);
    }
}

void dumpFromDecl(std::string& out, const FromDeclAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "FromDeclAST";
    if (!formatVisibility(node->visibility).empty()) {
        header = formatVisibility(node->visibility) + " " + header;
    }
    if (node->targetType) {
        header += " to " + formatType(node->targetType.get(), pool);
    }
    if (!node->genericParams.empty()) {
        header += " <";
        for (size_t i = 0; i < node->genericParams.size(); ++i) {
            if (i > 0) header += ", ";
            if (node->genericParams[i]) {
                header += toStr(pool, node->genericParams[i]->name);
            }
        }
        header += ">";
    }
    printNodeHeader(out, indentLevel, *node, header);

    for (const auto& entry : node->entries) {
        if (entry) dumpNode(out, entry.get(), pool, indentLevel + 1);
    }
    for (const auto& gp : node->genericParams) {
        if (gp) dumpNode(out, gp.get(), pool, indentLevel + 1);
    }
}

void dumpFromEntry(std::string& out, const FromEntryAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "FromEntryAST '" + formatType(node->returnType.get(), pool) + "'";
    printNodeHeader(out, indentLevel, *node, header);
    if (node->body) dumpNode(out, node->body.get(), pool, indentLevel + 1);
}

void dumpTypeAliasDecl(std::string& out, const TypeAliasDeclAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "TypeAliasDeclAST '" + toStr(pool, node->name) + "'";
    if (node->aliasedType) header += " = " + formatType(node->aliasedType.get(), pool);
    printNodeHeader(out, indentLevel, *node, header);
}

void dumpGenericParam(std::string& out, const GenericParamAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "GenericParamAST '" + toStr(pool, node->name) + "'";
    if (!node->constraints.empty()) {
        header += " : ";
        for (size_t i = 0; i < node->constraints.size(); ++i) {
            header += toStr(pool, node->constraints[i]);
            if (i + 1 < node->constraints.size()) header += " + ";
        }
    }
    printNodeHeader(out, indentLevel, *node, header);
}

void dumpParam(std::string& out, const ParamAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "ParamAST '" + toStr(pool, node->name) + "'";
    if (node->type) header += " : " + formatType(node->type.get(), pool);
    if (node->isVariadic) header += "...";
    printNodeHeader(out, indentLevel, *node, header);
}

void dumpLiteralExpr(std::string& out, const LiteralExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "LiteralExprAST '" + toStr(pool, node->value) + "'");
}

void dumpIdentifierExpr(std::string& out, const IdentifierExprAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "IdentifierExprAST '" + toStr(pool, node->name) + "'";
    if (!node->genericArgs.empty()) {
        header += "<";
        for (size_t i = 0; i < node->genericArgs.size(); ++i) {
            if (i > 0) header += ", ";
            header += formatType(node->genericArgs[i].get(), pool);
        }
        header += ">";
    }
    printNodeHeader(out, indentLevel, *node, header);
}

void dumpArrayLiteralExpr(std::string& out, const ArrayLiteralExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "ArrayLiteralExprAST");
    for (const auto& elem : node->elements) dumpNode(out, elem.get(), pool, indentLevel + 1);
}

void dumpStructLiteralExpr(std::string& out, const StructLiteralExprAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "StructLiteralExprAST '" + toStr(pool, node->typeName) + "'";
    if (!node->genericArgs.empty()) {
        header += "<";
        for (size_t i = 0; i < node->genericArgs.size(); ++i) {
            if (i > 0) header += ", ";
            header += formatType(node->genericArgs[i].get(), pool);
        }
        header += ">";
    }
    printNodeHeader(out, indentLevel, *node, header);
    for (const auto& init : node->inits) {
        if (init && init->value) dumpNode(out, init->value.get(), pool, indentLevel + 1);
    }
}

void dumpFieldInit(std::string& out, const FieldInitAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "FieldInitAST '" + toStr(pool, node->name) + "'");
    if (node->value) dumpNode(out, node->value.get(), pool, indentLevel + 1);
}

void dumpBinaryExpr(std::string& out, const BinaryExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "BinaryExprAST");
    if (node->left) dumpNode(out, node->left.get(), pool, indentLevel + 1);
    if (node->right) dumpNode(out, node->right.get(), pool, indentLevel + 1);
}

void dumpUnaryExpr(std::string& out, const UnaryExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "UnaryExprAST");
    if (node->operand) dumpNode(out, node->operand.get(), pool, indentLevel + 1);
}

void dumpCallExpr(std::string& out, const CallExprAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "CallExprAST";
    if (node->isAsyncCall) header += " (async)";
    printNodeHeader(out, indentLevel, *node, header);
    if (node->callee) dumpNode(out, node->callee.get(), pool, indentLevel + 1);
    for (const auto& arg : node->args) dumpNode(out, arg.get(), pool, indentLevel + 1);
}

void dumpIndexExpr(std::string& out, const IndexExprAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "IndexExprAST";
    header += (node->kind == IndexKind::Slice) ? " (slice)" : " (element)";
    if (node->isExclusive) header += " exclusive";
    printNodeHeader(out, indentLevel, *node, header);
    if (node->target) dumpNode(out, node->target.get(), pool, indentLevel + 1);
    if (node->index) dumpNode(out, node->index.get(), pool, indentLevel + 1);
    if (node->sliceEnd) dumpNode(out, node->sliceEnd.get(), pool, indentLevel + 1);
}

void dumpFieldAccessExpr(std::string& out, const FieldAccessExprAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "FieldAccessExprAST ." + toStr(pool, node->field);
    if (!node->genericArgs.empty()) {
        header += "<";
        for (size_t i = 0; i < node->genericArgs.size(); ++i) {
            if (i > 0) header += ", ";
            header += formatType(node->genericArgs[i].get(), pool);
        }
        header += ">";
    }
    printNodeHeader(out, indentLevel, *node, header);
    if (node->object) dumpNode(out, node->object.get(), pool, indentLevel + 1);
}

void dumpBehaviorAccessExpr(std::string& out, const BehaviorAccessExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "BehaviorAccessExprAST " + toStr(pool, node->typeName) + ":" + toStr(pool, node->method));
}

void dumpNullableChainExpr(std::string& out, const NullableChainExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "NullableChainExprAST");
    if (node->object) dumpNode(out, node->object.get(), pool, indentLevel + 1);
    if (!node->steps.empty()) {
        out += getIndent(indentLevel) + "\tsteps: ";
        for (size_t i = 0; i < node->steps.size(); ++i) {
            out += toStr(pool, node->steps[i]);
            if (i + 1 < node->steps.size()) out += ", ";
        }
        out += "\n";
    }
}

void dumpNullCoalesceExpr(std::string& out, const NullCoalesceExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "NullCoalesceExprAST");
    if (node->value) dumpNode(out, node->value.get(), pool, indentLevel + 1);
    if (node->fallback) dumpNode(out, node->fallback.get(), pool, indentLevel + 1);
}

void dumpAssignExpr(std::string& out, const AssignExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "AssignExprAST");
    if (node->lhs) dumpNode(out, node->lhs.get(), pool, indentLevel + 1);
    if (node->rhs) dumpNode(out, node->rhs.get(), pool, indentLevel + 1);
}

void dumpIsExpr(std::string& out, const IsExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "IsExprAST");
    if (node->expr) dumpNode(out, node->expr.get(), pool, indentLevel + 1);
    if (node->checkType) dumpNode(out, node->checkType.get(), pool, indentLevel + 1);
}

void dumpPipelineExpr(std::string& out, const PipelineExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "PipelineExprAST");
    if (node->seed) dumpNode(out, node->seed.get(), pool, indentLevel + 1);
    for (const auto& step : node->steps) {
        if (step) dumpNode(out, step.get(), pool, indentLevel + 1);
    }
}

void dumpPipelineStep(std::string& out, const PipelineStepAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "PipelineStepAST");
    if (node->callable) dumpNode(out, node->callable.get(), pool, indentLevel + 1);
    if (!node->packArgs.empty()) {
        out += getIndent(indentLevel + 1) + "packArgs:\n";
        for (const auto& arg : node->packArgs)
            dumpNode(out, arg.get(), pool, indentLevel + 2);
    }
}

void dumpComposeExpr(std::string& out, const ComposeExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "ComposeExprAST");
    for (const auto& op : node->operands) {
        if (op) dumpNode(out, op.get(), pool, indentLevel + 1);
    }
}

void dumpComposeOperand(std::string& out, const ComposeOperandAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "ComposeOperandAST");
    if (node->callable) dumpNode(out, node->callable.get(), pool, indentLevel + 1);
}

void dumpAnonFuncExpr(std::string& out, const AnonFuncExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "AnonFuncExprAST");
    if (node->body) dumpNode(out, node->body.get(), pool, indentLevel + 1);
}

void dumpAwaitExpr(std::string& out, const AwaitExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "AwaitExprAST");
    if (node->inner) dumpNode(out, node->inner.get(), pool, indentLevel + 1);
}

void dumpMatchExpr(std::string& out, const MatchExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "MatchExprAST");
    if (node->subject) dumpNode(out, node->subject.get(), pool, indentLevel + 1);
    for (const auto& arm : node->arms) dumpNode(out, arm.get(), pool, indentLevel + 1);
    if (node->defaultBody) dumpNode(out, node->defaultBody.get(), pool, indentLevel + 1);
}

void dumpIfExpr(std::string& out, const IfExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "IfExprAST");
    if (node->condition) dumpNode(out, node->condition.get(), pool, indentLevel + 1);
    if (node->thenBranch) dumpNode(out, node->thenBranch.get(), pool, indentLevel + 1);
    if (node->elseBranch) dumpNode(out, node->elseBranch.get(), pool, indentLevel + 1);
}

void dumpRangeExpr(std::string& out, const RangeExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "RangeExprAST");
    if (node->lo) dumpNode(out, node->lo.get(), pool, indentLevel + 1);
    if (node->hi) dumpNode(out, node->hi.get(), pool, indentLevel + 1);
}

void dumpResolveExpr(std::string& out, const ResolveExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "ResolveExprAST");
    if (node->subject) dumpNode(out, node->subject.get(), pool, indentLevel + 1);
    if (node->okArm) dumpNode(out, node->okArm.get(), pool, indentLevel + 1);
    if (node->errArm) dumpNode(out, node->errArm.get(), pool, indentLevel + 1);
}

void dumpOkArm(std::string& out, const OkArmAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "OkArmAST '" + toStr(pool, node->bindName) + "'";
    if (node->bindType) header += " : " + formatType(node->bindType.get(), pool);
    printNodeHeader(out, indentLevel, *node, header);
    if (node->body) dumpNode(out, node->body.get(), pool, indentLevel + 1);
}

void dumpErrArm(std::string& out, const ErrArmAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "ErrArmAST";
    if (!node->bindName.isValid() && !node->bindType) {
        header += " (bare)";
    } else {
        header += " '" + toStr(pool, node->bindName) + "'";
        if (node->bindType) header += " : " + formatType(node->bindType.get(), pool);
    }
    printNodeHeader(out, indentLevel, *node, header);
    if (node->body) dumpNode(out, node->body.get(), pool, indentLevel + 1);
}

void dumpBindPattern(std::string& out, const BindPatternAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "BindPatternAST '" + toStr(pool, node->name) + "'");
}

void dumpWildcardPattern(std::string& out, const WildcardPatternAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "WildcardPatternAST");
}

void dumpTypePattern(std::string& out, const TypePatternAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "TypePatternAST");
    if (node->checkType) dumpNode(out, node->checkType.get(), pool, indentLevel + 1);
}

void dumpStructPattern(std::string& out, const StructPatternAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "StructPatternAST '" + toStr(pool, node->typeName) + "'");
    for (const auto& field : node->fields) {
        if (field) {
            if (field->subPattern) {
                dumpNode(out, field->subPattern.get(), pool, indentLevel + 1);
            } else {
                out += getIndent(indentLevel + 1) + "Field: " + toStr(pool, field->field) + " (shorthand)\n";
            }
        }
    }
}

void dumpPatternExpr(std::string& out, const PatternExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "PatternExprAST");
    if (node->inner) dumpNode(out, node->inner.get(), pool, indentLevel + 1);
}

void dumpMatchArm(std::string& out, const MatchArmAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "MatchArmAST");
    for (const auto& p : node->patterns) dumpNode(out, p.get(), pool, indentLevel + 1);
    if (node->guard) dumpNode(out, node->guard.get(), pool, indentLevel + 1);
    for (const auto& e : node->exprs) dumpNode(out, e.get(), pool, indentLevel + 1);
}

void dumpDefaultArm(std::string& out, const DefaultArmAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "DefaultArmAST");
    for (const auto& e : node->exprs) dumpNode(out, e.get(), pool, indentLevel + 1);
}

void dumpBlockStmt(std::string& out, const BlockStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "BlockStmtAST");
    for (const auto& stmt : node->stmts) dumpNode(out, stmt.get(), pool, indentLevel + 1);
}

void dumpExprStmt(std::string& out, const ExprStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "ExprStmtAST");
    if (node->expr) dumpNode(out, node->expr.get(), pool, indentLevel + 1);
}

void dumpDeclStmt(std::string& out, const DeclStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "DeclStmtAST");
    if (node->decl) dumpNode(out, node->decl.get(), pool, indentLevel + 1);
}

void dumpIfStmt(std::string& out, const IfStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "IfStmtAST");
    if (node->condition) dumpNode(out, node->condition.get(), pool, indentLevel + 1);
    if (node->thenBranch) dumpNode(out, node->thenBranch.get(), pool, indentLevel + 1);
    if (node->elseBranch) dumpNode(out, node->elseBranch.get(), pool, indentLevel + 1);
}

void dumpSwitchStmt(std::string& out, const SwitchStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "SwitchStmtAST");
    if (node->subject) dumpNode(out, node->subject.get(), pool, indentLevel + 1);
    for (const auto& c : node->cases) {
        if (c) {
            out += getIndent(indentLevel + 1) + "Case:\n";
            for (const auto& val : c->values) dumpNode(out, val.get(), pool, indentLevel + 2);
            if (c->body) dumpNode(out, c->body.get(), pool, indentLevel + 2);
        }
    }
    if (node->defaultBody) dumpNode(out, node->defaultBody.get(), pool, indentLevel + 1);
}

void dumpForStmt(std::string& out, const ForStmtAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "ForStmtAST '" + toStr(pool, node->iterVar->name) + "'";
    if (node->iterVar->type) header += " : " + formatType(node->iterVar->type.get(), pool);
    printNodeHeader(out, indentLevel, *node, header);
    if (node->iterable) dumpNode(out, node->iterable.get(), pool, indentLevel + 1);
    if (node->step) dumpNode(out, node->step.get(), pool, indentLevel + 1);
    if (node->body) dumpNode(out, node->body.get(), pool, indentLevel + 1);
}

void dumpWhileStmt(std::string& out, const WhileStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "WhileStmtAST");
    if (node->condition) dumpNode(out, node->condition.get(), pool, indentLevel + 1);
    if (node->body) dumpNode(out, node->body.get(), pool, indentLevel + 1);
}

void dumpDoWhileStmt(std::string& out, const DoWhileStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "DoWhileStmtAST");
    if (node->body) dumpNode(out, node->body.get(), pool, indentLevel + 1);
    if (node->condition) dumpNode(out, node->condition.get(), pool, indentLevel + 1);
}

void dumpReturnStmt(std::string& out, const ReturnStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "ReturnStmtAST");
    for (const auto& val : node->values) dumpNode(out, val.get(), pool, indentLevel + 1);
}

void dumpBreakStmt(std::string& out, const BreakStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "BreakStmtAST");
}

void dumpContinueStmt(std::string& out, const ContinueStmtAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "ContinueStmtAST");
}

void dumpMultiVarDecl(std::string& out, const MultiVarDeclAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "MultiVarDeclAST (";
    header += (node->keyword == DeclKeyword::Let) ? "let" : "const";
    header += ") ";
    for (size_t i = 0; i < node->vars.size(); ++i) {
        if (i > 0) header += ", ";
        header += toStr(pool, node->vars[i].first);
        if (node->vars[i].second) header += " : " + formatType(node->vars[i].second.get(), pool);
    }
    header += " = ...";
    printNodeHeader(out, indentLevel, *node, header);
    if (node->rhs) dumpNode(out, node->rhs.get(), pool, indentLevel + 1);
}

void dumpMultiAssignStmt(std::string& out, const MultiAssignStmtAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "MultiAssignStmtAST (";
    for (size_t i = 0; i < node->lhs.size(); ++i) {
        if (i > 0) header += ", ";
        header += "lhs_" + std::to_string(i);
    }
    header += " = ...";
    printNodeHeader(out, indentLevel, *node, header);
    for (const auto& lhs : node->lhs) dumpNode(out, lhs.get(), pool, indentLevel + 1);
    if (node->rhs) dumpNode(out, node->rhs.get(), pool, indentLevel + 1);
}

void dumpAttribute(std::string& out, const AttributeAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "AttributeAST @" + toStr(pool, node->name);
    if (!node->args.empty()) {
        header += "(";
        for (size_t i = 0; i < node->args.size(); ++i) {
            if (i > 0) header += ", ";
            header += toStr(pool, node->args[i]->value);
        }
        header += ")";
    }
    printNodeHeader(out, indentLevel, *node, header);
}

void dumpAttributeArg(std::string& out, const AttributeArgAST* node, const StringPool* pool, int indentLevel) {
    std::string header = "AttributeArgAST ";
    switch (node->kind) {
        case AttributeArgKind::StringLit: header += "\"" + toStr(pool, node->value) + "\""; break;
        case AttributeArgKind::IntLit:    header += toStr(pool, node->value); break;
        case AttributeArgKind::BoolLit:   header += toStr(pool, node->value); break;
        case AttributeArgKind::TypeIdent: header += toStr(pool, node->value); break;
    }
    printNodeHeader(out, indentLevel, *node, header);
}

void dumpIntrinsicCallExpr(std::string& out, const IntrinsicCallExprAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "IntrinsicCallExprAST " + toStr(pool, node->intrinsicName));
    if (node->typeArg) dumpNode(out, node->typeArg.get(), pool, indentLevel + 1);
    for (const auto& e : node->args) dumpNode(out, e.get(), pool, indentLevel + 1);
}

void dumpUnknown(std::string& out, const BaseAST* node, const StringPool* pool, int indentLevel) {
    printNodeHeader(out, indentLevel, *node, "Unknown");
}

// =============================================================================
// Main dispatch function – single entry point for all node types
// =============================================================================

void dumpNode(std::string& out, const BaseAST* node, const StringPool* pool, int indentLevel) {
    if (!node) return;

    switch (node->kind) {
        case ASTKind::Program:             dumpProgram(out, static_cast<const ProgramAST*>(node), pool, indentLevel); break;
        case ASTKind::PrimitiveType:       dumpPrimitiveType(out, static_cast<const PrimitiveTypeAST*>(node), pool, indentLevel); break;
        case ASTKind::NamedType:           dumpNamedType(out, static_cast<const NamedTypeAST*>(node), pool, indentLevel); break;
        case ASTKind::NullableType:        dumpNullableType(out, static_cast<const NullableTypeAST*>(node), pool, indentLevel); break;
        case ASTKind::ResultType:          dumpResultType(out, static_cast<const ResultTypeAST*>(node), pool, indentLevel); break;
        case ASTKind::ArrayType:           dumpArrayType(out, static_cast<const ArrayTypeAST*>(node), pool, indentLevel); break;
        case ASTKind::GenericArrayType:    dumpGenericArrayType(out, static_cast<const GenericArrayTypeAST*>(node), pool, indentLevel); break;
        case ASTKind::RefType:             dumpRefType(out, static_cast<const RefTypeAST*>(node), pool, indentLevel); break;
        case ASTKind::PtrType:             dumpPtrType(out, static_cast<const PtrTypeAST*>(node), pool, indentLevel); break;
        case ASTKind::FuncType:            dumpFuncType(out, static_cast<const FuncTypeAST*>(node), pool, indentLevel); break;
        case ASTKind::PackageDecl:         dumpPackageDecl(out, static_cast<const PackageDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::UseDecl:             dumpUseDecl(out, static_cast<const UseDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::VarDecl:             dumpVarDecl(out, static_cast<const VarDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::FuncDecl:            dumpFuncDecl(out, static_cast<const FuncDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::StructDecl:          dumpStructDecl(out, static_cast<const StructDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::FieldDecl:           dumpFieldDecl(out, static_cast<const FieldDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::EnumDecl:            dumpEnumDecl(out, static_cast<const EnumDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::EnumVariant:         dumpEnumVariant(out, static_cast<const EnumVariantAST*>(node), pool, indentLevel); break;
        case ASTKind::TraitMethod:         dumpTraitMethod(out, static_cast<const TraitMethodAST*>(node), pool, indentLevel); break;
        case ASTKind::TraitDecl:           dumpTraitDecl(out, static_cast<const TraitDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::TraitRef:            dumpTraitRef(out, static_cast<const TraitRefAST*>(node), pool, indentLevel); break;
        case ASTKind::ImplDecl:            dumpImplDecl(out, static_cast<const ImplDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::MethodDecl:          dumpMethodDecl(out, static_cast<const MethodDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::FromDecl:            dumpFromDecl(out, static_cast<const FromDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::FromEntry:           dumpFromEntry(out, static_cast<const FromEntryAST*>(node), pool, indentLevel); break;
        case ASTKind::TypeAliasDecl:       dumpTypeAliasDecl(out, static_cast<const TypeAliasDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::GenericParam:        dumpGenericParam(out, static_cast<const GenericParamAST*>(node), pool, indentLevel); break;
        case ASTKind::Param:               dumpParam(out, static_cast<const ParamAST*>(node), pool, indentLevel); break;
        case ASTKind::LiteralExpr:         dumpLiteralExpr(out, static_cast<const LiteralExprAST*>(node), pool, indentLevel); break;
        case ASTKind::IdentifierExpr:      dumpIdentifierExpr(out, static_cast<const IdentifierExprAST*>(node), pool, indentLevel); break;
        case ASTKind::ArrayLiteralExpr:    dumpArrayLiteralExpr(out, static_cast<const ArrayLiteralExprAST*>(node), pool, indentLevel); break;
        case ASTKind::StructLiteralExpr:   dumpStructLiteralExpr(out, static_cast<const StructLiteralExprAST*>(node), pool, indentLevel); break;
        case ASTKind::FieldInit:           dumpFieldInit(out, static_cast<const FieldInitAST*>(node), pool, indentLevel); break;
        case ASTKind::BinaryExpr:          dumpBinaryExpr(out, static_cast<const BinaryExprAST*>(node), pool, indentLevel); break;
        case ASTKind::UnaryExpr:           dumpUnaryExpr(out, static_cast<const UnaryExprAST*>(node), pool, indentLevel); break;
        case ASTKind::CallExpr:            dumpCallExpr(out, static_cast<const CallExprAST*>(node), pool, indentLevel); break;
        case ASTKind::IndexExpr:           dumpIndexExpr(out, static_cast<const IndexExprAST*>(node), pool, indentLevel); break;
        case ASTKind::FieldAccessExpr:     dumpFieldAccessExpr(out, static_cast<const FieldAccessExprAST*>(node), pool, indentLevel); break;
        case ASTKind::BehaviorAccessExpr:  dumpBehaviorAccessExpr(out, static_cast<const BehaviorAccessExprAST*>(node), pool, indentLevel); break;
        case ASTKind::NullableChainExpr:   dumpNullableChainExpr(out, static_cast<const NullableChainExprAST*>(node), pool, indentLevel); break;
        case ASTKind::NullCoalesceExpr:    dumpNullCoalesceExpr(out, static_cast<const NullCoalesceExprAST*>(node), pool, indentLevel); break;
        case ASTKind::AssignExpr:          dumpAssignExpr(out, static_cast<const AssignExprAST*>(node), pool, indentLevel); break;
        case ASTKind::IsExpr:              dumpIsExpr(out, static_cast<const IsExprAST*>(node), pool, indentLevel); break;
        case ASTKind::PipelineExpr:        dumpPipelineExpr(out, static_cast<const PipelineExprAST*>(node), pool, indentLevel); break;
        case ASTKind::PipelineStep:        dumpPipelineStep(out, static_cast<const PipelineStepAST*>(node), pool, indentLevel); break;
        case ASTKind::ComposeExpr:         dumpComposeExpr(out, static_cast<const ComposeExprAST*>(node), pool, indentLevel); break;
        case ASTKind::ComposeOperand:      dumpComposeOperand(out, static_cast<const ComposeOperandAST*>(node), pool, indentLevel); break;
        case ASTKind::AnonFuncExpr:        dumpAnonFuncExpr(out, static_cast<const AnonFuncExprAST*>(node), pool, indentLevel); break;
        case ASTKind::AwaitExpr:           dumpAwaitExpr(out, static_cast<const AwaitExprAST*>(node), pool, indentLevel); break;
        case ASTKind::MatchExpr:           dumpMatchExpr(out, static_cast<const MatchExprAST*>(node), pool, indentLevel); break;
        case ASTKind::IfExpr:              dumpIfExpr(out, static_cast<const IfExprAST*>(node), pool, indentLevel); break;
        case ASTKind::RangeExpr:           dumpRangeExpr(out, static_cast<const RangeExprAST*>(node), pool, indentLevel); break;
        case ASTKind::ResolveExpr:         dumpResolveExpr(out, static_cast<const ResolveExprAST*>(node), pool, indentLevel); break;
        case ASTKind::OkArm:               dumpOkArm(out, static_cast<const OkArmAST*>(node), pool, indentLevel); break;
        case ASTKind::ErrArm:              dumpErrArm(out, static_cast<const ErrArmAST*>(node), pool, indentLevel); break;
        case ASTKind::BindPattern:         dumpBindPattern(out, static_cast<const BindPatternAST*>(node), pool, indentLevel); break;
        case ASTKind::WildcardPattern:     dumpWildcardPattern(out, static_cast<const WildcardPatternAST*>(node), pool, indentLevel); break;
        case ASTKind::TypePattern:         dumpTypePattern(out, static_cast<const TypePatternAST*>(node), pool, indentLevel); break;
        case ASTKind::StructPattern:       dumpStructPattern(out, static_cast<const StructPatternAST*>(node), pool, indentLevel); break;
        case ASTKind::FieldPattern:        // handled by StructPattern
        case ASTKind::PatternExpr:         dumpPatternExpr(out, static_cast<const PatternExprAST*>(node), pool, indentLevel); break;
        case ASTKind::MatchArm:            dumpMatchArm(out, static_cast<const MatchArmAST*>(node), pool, indentLevel); break;
        case ASTKind::DefaultArm:          dumpDefaultArm(out, static_cast<const DefaultArmAST*>(node), pool, indentLevel); break;
        case ASTKind::BlockStmt:           dumpBlockStmt(out, static_cast<const BlockStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::ExprStmt:            dumpExprStmt(out, static_cast<const ExprStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::DeclStmt:            dumpDeclStmt(out, static_cast<const DeclStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::IfStmt:              dumpIfStmt(out, static_cast<const IfStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::SwitchStmt:          dumpSwitchStmt(out, static_cast<const SwitchStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::SwitchCase:          // handled by SwitchStmt
        case ASTKind::ForStmt:             dumpForStmt(out, static_cast<const ForStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::WhileStmt:           dumpWhileStmt(out, static_cast<const WhileStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::DoWhileStmt:         dumpDoWhileStmt(out, static_cast<const DoWhileStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::ReturnStmt:          dumpReturnStmt(out, static_cast<const ReturnStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::BreakStmt:           dumpBreakStmt(out, static_cast<const BreakStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::ContinueStmt:        dumpContinueStmt(out, static_cast<const ContinueStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::MultiVarDecl:        dumpMultiVarDecl(out, static_cast<const MultiVarDeclAST*>(node), pool, indentLevel); break;
        case ASTKind::MultiAssignStmt:     dumpMultiAssignStmt(out, static_cast<const MultiAssignStmtAST*>(node), pool, indentLevel); break;
        case ASTKind::Attribute:           dumpAttribute(out, static_cast<const AttributeAST*>(node), pool, indentLevel); break;
        case ASTKind::AttributeArg:        dumpAttributeArg(out, static_cast<const AttributeArgAST*>(node), pool, indentLevel); break;
        case ASTKind::IntrinsicCallExpr:   dumpIntrinsicCallExpr(out, static_cast<const IntrinsicCallExprAST*>(node), pool, indentLevel); break;
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

} // namespace LucDebug