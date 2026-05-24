/**
 * @file ASTDumper.cpp
 * @brief Implementation of AST visitor that prints the AST in a human-readable format.
 */

#include "ASTDumper.hpp"
#include "DebugUtils.hpp"
#include "ast/DeclAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/StmtAST.hpp"
#include "ast/TypeAST.hpp"
#include <sstream>
#include <string>

namespace LucDebug {

ASTDumper::ASTDumper(int verb, const StringPool& p) 
    : verbosity(verb), indentLevel(0), pool(&p) {
    rebuildIndentCache();
}

void ASTDumper::rebuildIndentCache() {
    for (int i = 0; i < 16; ++i) {
        indentCache[i].clear();
        for (int j = 0; j < i; ++j) {
            indentCache[i] += '\t';
        }
    }
}

void ASTDumper::indent() {
    if (indentLevel < 16) {
        out += indentCache[indentLevel];
    } else {
        for (int i = 0; i < indentLevel; ++i) out += '\t';
    }
}

void ASTDumper::printLine(const std::string& text) {
    indent();
    out += text + "\n";
}

void ASTDumper::printKV(const std::string& key, const std::string& value) {
    indent();
    out += "\t" + key + ": '" + value + "'\n";
}

void ASTDumper::printNodeHeader(const BaseAST& node, const std::string& nodeName) {
    indent();
    out += "[" + std::to_string(indentLevel) + "] " + nodeName;
    out += " (kind=" + kindToString(node.kind) + ")";
    out += " at line " + std::to_string(node.loc.line()) + ", col " + std::to_string(node.loc.column()) + "\n";
}

void ASTDumper::visitChild(BaseAST* child, const std::string& label) {
    if (!child) return;
    indentLevel++;
    child->accept(*this);
    indentLevel--;
}

std::string ASTDumper::formatType(TypeAST* type) {
    if (!type) return "<unresolved>";

    switch (type->kind) {
        case ASTKind::PrimitiveType:
            return primitiveKindToString(static_cast<PrimitiveTypeAST*>(type)->primitiveKind);

        case ASTKind::NamedType: {
            auto* n = static_cast<NamedTypeAST*>(type);
            std::string res = toStr(pool, n->name);
            if (!n->genericArgs.empty()) {
                res += "<";
                for (size_t i = 0; i < n->genericArgs.size(); ++i) {
                    if (i > 0) res += ", ";
                    res += formatType(n->genericArgs[i].get());
                }
                res += ">";
            }
            return res;
        }

        case ASTKind::NullableType:
            return formatType(static_cast<NullableTypeAST*>(type)->inner.get()) + "?";

        case ASTKind::ResultType: {
            auto* r = static_cast<ResultTypeAST*>(type);
            std::string innerStr = formatType(r->inner.get());
            if (r->hasErrorType()) {
                return innerStr + "!" + formatType(r->errorType.get());
            } else {
                return innerStr + "!";
            }
        }

        case ASTKind::RefType:
            return "&" + formatType(static_cast<RefTypeAST*>(type)->inner.get());

        case ASTKind::PtrType:
            return "*" + formatType(static_cast<PtrTypeAST*>(type)->inner.get());

        case ASTKind::FixedArrayType: {
            auto* a = static_cast<FixedArrayTypeAST*>(type);
            return "[" + std::to_string(a->size) + "]" + formatType(a->element.get());
        }

        case ASTKind::SliceType:
            return "[]" + formatType(static_cast<SliceTypeAST*>(type)->element.get());

        case ASTKind::DynamicArrayType:
            return "[*]" + formatType(static_cast<DynamicArrayTypeAST*>(type)->element.get());

        case ASTKind::FuncType: {
            auto* f = static_cast<FuncTypeAST*>(type);
            const auto& sig = f->sig;
            std::string res;

            // Qualifiers
            uint32_t q = sig.qualifiers;
            if (q & QualifierBits::Async) res += "~async ";
            if (q & QualifierBits::Nullable) res += "~nullable ";
            if (q & QualifierBits::Parallel) res += "~parallel ";

            // Parameter groups (using flat representation)
            size_t paramOffset = 0;
            for (size_t g = 0; g < sig.groupSizes.size(); ++g) {
                res += "(";
                size_t groupSize = sig.groupSizes[g];
                for (size_t i = 0; i < groupSize; ++i) {
                    if (i > 0) res += ", ";
                    const auto& param = sig.allParams[paramOffset + i];
                    if (param->type) res += formatType(param->type.get());
                    if (param->isVariadic) res += "...";
                }
                res += ")";
                paramOffset += groupSize;
            }

            // Return types
            if (!sig.returnTypes.empty()) {
                res += " -> ";
                for (size_t i = 0; i < sig.returnTypes.size(); ++i) {
                    if (i > 0) res += ", ";
                    res += formatType(sig.returnTypes[i].get());
                }
            }
            return res;
        }

        default:
            return kindToString(type->kind);
    }
}

std::string ASTDumper::formatParamGroup(const ArenaSpan<ParamPtr>& params) {
    std::string res = "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) res += ", ";
        if (params[i]->type) {
            res += formatType(params[i]->type.get());
        }
        if (params[i]->isVariadic) res += "...";
    }
    res += ")";
    return res;
}

// ── Root ──────────────────────────────────────────────────────────────────
void ASTDumper::visit(ProgramAST& node) {
    printNodeHeader(node, "ProgramAST");
    indent(); out += "\tpackageName: '" + toStr(pool, node.packageName) + "'\n";
    indent(); out += "\tfilePath: '" + std::string(pool->lookup(node.filePath)) + "'\n";
    indent(); out += "\tdecls (count): " + std::to_string(node.decls.size()) + "\n";

    indentLevel++;
    for (const auto& decl : node.decls) {
        if (decl) decl->accept(*this);
    }
    indentLevel--;
}

// ── Type nodes ────────────────────────────────────────────────────────────
void ASTDumper::visit(PrimitiveTypeAST& node) {
    printNodeHeader(node, "PrimitiveTypeAST " + formatType(&node));
}

void ASTDumper::visit(NamedTypeAST& node) {
    printNodeHeader(node, "NamedTypeAST '" + toStr(pool, node.name) + "'");
    indentLevel++;
    for (const auto& arg : node.genericArgs) visitChild(arg.get());
    indentLevel--;
}

void ASTDumper::visit(NullableTypeAST& node) {
    printNodeHeader(node, "NullableTypeAST");
    if (node.inner) visitChild(node.inner.get());
}

void ASTDumper::visit(ResultTypeAST& node) {
    std::string header = "ResultTypeAST ";
    if (node.hasErrorType()) {
        header += formatType(node.inner.get()) + "!" + formatType(node.errorType.get());
    } else {
        header += formatType(node.inner.get()) + "!";
    }
    printNodeHeader(node, header);
    indentLevel++;
    visitChild(node.inner.get(), "inner");
    if (node.errorType) visitChild(node.errorType.get(), "errorType");
    indentLevel--;
}

void ASTDumper::visit(FixedArrayTypeAST& node) {
    printNodeHeader(node, "FixedArrayTypeAST [" + std::to_string(node.size) + "]");
    if (node.element) visitChild(node.element.get());
}

void ASTDumper::visit(SliceTypeAST& node) {
    printNodeHeader(node, "SliceTypeAST");
    if (node.element) visitChild(node.element.get());
}

void ASTDumper::visit(DynamicArrayTypeAST& node) {
    printNodeHeader(node, "DynamicArrayTypeAST");
    if (node.element) visitChild(node.element.get());
}

void ASTDumper::visit(RefTypeAST& node) {
    printNodeHeader(node, "RefTypeAST");
    if (node.inner) visitChild(node.inner.get());
}

void ASTDumper::visit(PtrTypeAST& node) {
    printNodeHeader(node, "PtrTypeAST");
    if (node.inner) visitChild(node.inner.get());
}

void ASTDumper::visit(FuncTypeAST& node) {
    std::string header = "FuncTypeAST";
    uint32_t q = node.sig.qualifiers;
    if (q & QualifierBits::Async) header += " ~async";
    if (q & QualifierBits::Nullable) header += " ~nullable";
    if (q & QualifierBits::Parallel) header += " ~parallel";
    printNodeHeader(node, header);
    
    indentLevel++;
    // Parameter groups (using flat representation)
    size_t paramOffset = 0;
    for (size_t g = 0; g < node.sig.groupSizes.size(); ++g) {
        size_t groupSize = node.sig.groupSizes[g];
        for (size_t i = 0; i < groupSize; ++i) {
            if (node.sig.allParams[paramOffset + i]) {
                visitChild(node.sig.allParams[paramOffset + i].get(), "param");
            }
        }
        paramOffset += groupSize;
    }
    // Return types
    for (const auto& ret : node.sig.returnTypes) {
        if (ret) visitChild(ret.get(), "return");
    }
    indentLevel--;
}

// ── Declaration nodes ─────────────────────────────────────────────────────
void ASTDumper::visit(PackageDeclAST& node) {
    printNodeHeader(node, "PackageDeclAST '" + toStr(pool, node.name) + "'");
}

void ASTDumper::visit(UseDeclAST& node) {
    std::string pathStr;
    for (size_t i = 0; i < node.path.size(); ++i) {
        if (i > 0) pathStr += ".";
        pathStr += toStr(pool, node.path[i]);
    }
    std::string header = "UseDeclAST '" + pathStr + "'";
    if (node.alias) header += " as " + toStr(pool, *node.alias);
    printNodeHeader(node, header);
}

void ASTDumper::visit(VarDeclAST& node) {
    std::string header = "VarDeclAST '" + toStr(pool, node.name) + "'";
    if (node.type) header += " : " + formatType(node.type.get());
    printNodeHeader(node, header);
    indentLevel++;
    if (node.init) visitChild(node.init.get(), "init");
    for (const auto& attr : node.attributes) visitChild(attr.get());
    indentLevel--;
}

void ASTDumper::visit(FuncDeclAST& node) {
    std::string header = "FuncDeclAST '" + toStr(pool, node.name) + "'";
    
    // Qualifiers
    uint32_t q = node.sig.qualifiers;
    if (q & QualifierBits::Async) header += " ~async";
    if (q & QualifierBits::Nullable) header += " ~nullable";
    if (q & QualifierBits::Parallel) header += " ~parallel";
    
    // Parameter groups (using flat representation)
    size_t paramOffset = 0;
    for (size_t g = 0; g < node.sig.groupSizes.size(); ++g) {
        header += " (";
        size_t groupSize = node.sig.groupSizes[g];
        for (size_t i = 0; i < groupSize; ++i) {
            if (i > 0) header += ", ";
            const auto& param = node.sig.allParams[paramOffset + i];
            if (param->type) header += formatType(param->type.get());
        }
        header += ")";
        paramOffset += groupSize;
    }
    
    // Return types
    if (!node.sig.returnTypes.empty()) {
        header += " -> ";
        for (size_t i = 0; i < node.sig.returnTypes.size(); ++i) {
            if (i > 0) header += ", ";
            header += formatType(node.sig.returnTypes[i].get());
        }
    }
    
    printNodeHeader(node, header);
    indentLevel++;
    if (node.body) visitChild(node.body.get());
    for (const auto& attr : node.attributes) visitChild(attr.get());
    indentLevel--;
}

void ASTDumper::visit(StructDeclAST& node) {
    printNodeHeader(node, "StructDeclAST '" + toStr(pool, node.name) + "'");
    indentLevel++;
    for (const auto& field : node.fields) visitChild(field.get());
    for (const auto& attr : node.attributes) visitChild(attr.get());
    indentLevel--;
}

void ASTDumper::visit(FieldDeclAST& node) {
    std::string header = "FieldDeclAST '" + toStr(pool, node.name) + "'";
    if (node.type) header += " : " + formatType(node.type.get());
    printNodeHeader(node, header);
    indentLevel++;
    if (node.defaultVal) {
        indent(); out += "\tdefault: ";
        indentLevel++;
        node.defaultVal->accept(*this);
        indentLevel--;
    }
    indentLevel--;
}

void ASTDumper::visit(EnumDeclAST& node) {
    printNodeHeader(node, "EnumDeclAST '" + toStr(pool, node.name) + "'");
    for (const auto& variant : node.variants) visitChild(variant.get());
}

void ASTDumper::visit(EnumVariantAST& node) {
    std::string header = "EnumVariantAST '" + toStr(pool, node.name) + "'";
    if (node.explicitValue) header += " = " + std::to_string(*node.explicitValue);
    printNodeHeader(node, header);
}

void ASTDumper::visit(TraitMethodAST& node) {
    std::string header = "TraitMethodAST '" + toStr(pool, node.name) + "'";
    
    // Qualifiers
    uint32_t q = node.sig.qualifiers;
    if (q & QualifierBits::Async) header += " ~async";
    if (q & QualifierBits::Nullable) header += " ~nullable";
    if (q & QualifierBits::Parallel) header += " ~parallel";
    
    // Parameter groups (using flat representation)
    size_t paramOffset = 0;
    for (size_t g = 0; g < node.sig.groupSizes.size(); ++g) {
        header += " (";
        size_t groupSize = node.sig.groupSizes[g];
        for (size_t i = 0; i < groupSize; ++i) {
            if (i > 0) header += ", ";
            const auto& param = node.sig.allParams[paramOffset + i];
            if (param->type) header += formatType(param->type.get());
        }
        header += ")";
        paramOffset += groupSize;
    }
    
    // Return types
    if (!node.sig.returnTypes.empty()) {
        header += " -> ";
        for (size_t i = 0; i < node.sig.returnTypes.size(); ++i) {
            if (i > 0) header += ", ";
            header += formatType(node.sig.returnTypes[i].get());
        }
    }
    
    printNodeHeader(node, header);
}

void ASTDumper::visit(TraitDeclAST& node) {
    printNodeHeader(node, "TraitDeclAST '" + toStr(pool, node.name) + "'");
    for (const auto& method : node.methods) visitChild(method.get());
}

void ASTDumper::visit(TraitRefAST& node) {
    std::string header = "TraitRefAST '" + toStr(pool, node.name) + "'";
    if (!node.genericArgs.empty()) {
        header += "<";
        for (size_t i = 0; i < node.genericArgs.size(); ++i) {
            if (i > 0) header += ", ";
            header += formatType(node.genericArgs[i].get());
        }
        header += ">";
    }
    printNodeHeader(node, header);
}

void ASTDumper::visit(ImplDeclAST& node) {
    std::string header = "ImplDeclAST";
    
    // Add visibility
    if (node.visibility == Visibility::Package) {
        header = "pub " + header;
    } else if (node.visibility == Visibility::Export) {
        header = "export " + header;
    }
    
    // Add target type
    if (node.targetType) {
        header += " for " + formatType(node.targetType.get());
    } else {
        header += " for <unknown>";
    }
    
    // Add generic parameters if any
    if (!node.genericParams.empty()) {
        header += " <";
        for (size_t i = 0; i < node.genericParams.size(); ++i) {
            if (i > 0) header += ", ";
            if (node.genericParams[i]) {
                header += toStr(pool, node.genericParams[i]->name);
            }
        }
        header += ">";
    }
    
    // Add receiver alias if present
    if (node.receiverAlias.isValid()) {
        header += " as " + toStr(pool, node.receiverAlias);
    }
    
    // Add trait conformance if present
    if (node.traitRef) {
        header += " : " + toStr(pool, node.traitRef->name);
        if (!node.traitRef->genericArgs.empty()) {
            header += "<";
            for (size_t i = 0; i < node.traitRef->genericArgs.size(); ++i) {
                if (i > 0) header += ", ";
                header += formatType(node.traitRef->genericArgs[i].get());
            }
            header += ">";
        }
    }
    
    printNodeHeader(node, header);
    
    indentLevel++;
    for (const auto& gp : node.genericParams) {
        if (gp) visitChild(gp.get());
    }
    for (const auto& method : node.methods) {
        if (method) visitChild(method.get());
    }
    indentLevel--;
}

void ASTDumper::visit(MethodDeclAST& node) {
    std::string header = "MethodDeclAST '" + toStr(pool, node.name) + "'";
    
    // Qualifiers
    uint32_t q = node.sig.qualifiers;
    if (q & QualifierBits::Async) header += " ~async";
    if (q & QualifierBits::Nullable) header += " ~nullable";
    if (q & QualifierBits::Parallel) header += " ~parallel";
    
    // Parameter groups (using flat representation)
    size_t paramOffset = 0;
    for (size_t g = 0; g < node.sig.groupSizes.size(); ++g) {
        header += " (";
        size_t groupSize = node.sig.groupSizes[g];
        for (size_t i = 0; i < groupSize; ++i) {
            if (i > 0) header += ", ";
            const auto& param = node.sig.allParams[paramOffset + i];
            if (param->type) header += formatType(param->type.get());
        }
        header += ")";
        paramOffset += groupSize;
    }
    
    // Return types
    if (!node.sig.returnTypes.empty()) {
        header += " -> ";
        for (size_t i = 0; i < node.sig.returnTypes.size(); ++i) {
            if (i > 0) header += ", ";
            header += formatType(node.sig.returnTypes[i].get());
        }
    }
    
    printNodeHeader(node, header);
    if (node.body) visitChild(node.body.get());
}

void ASTDumper::visit(FromDeclAST& node) {
    std::string header = "FromDeclAST";
    
    // Add visibility
    if (node.visibility == Visibility::Package) {
        header = "pub " + header;
    } else if (node.visibility == Visibility::Export) {
        header = "export " + header;
    }
    
    // Add target type
    if (node.targetType) {
        header += " to " + formatType(node.targetType.get());
    } else {
        header += " to <unknown>";
    }
    
    // Add generic parameters if any
    if (!node.genericParams.empty()) {
        header += " <";
        for (size_t i = 0; i < node.genericParams.size(); ++i) {
            if (i > 0) header += ", ";
            if (node.genericParams[i]) {
                header += toStr(pool, node.genericParams[i]->name);
            }
        }
        header += ">";
    }
    
    printNodeHeader(node, header);
    
    indentLevel++;
    for (const auto& entry : node.entries) {
        if (entry) visitChild(entry.get());
    }
    for (const auto& gp : node.genericParams) {
        if (gp) visitChild(gp.get());
    }
    indentLevel--;
}

void ASTDumper::visit(FromEntryAST& node) {
    std::string header = "FromEntryAST '" + formatType(node.returnType.get()) + "'";
    
    // Parameter groups (using flat representation)
    size_t paramOffset = 0;
    for (size_t g = 0; g < node.sig.groupSizes.size(); ++g) {
        header += " (";
        size_t groupSize = node.sig.groupSizes[g];
        for (size_t i = 0; i < groupSize; ++i) {
            if (i > 0) header += ", ";
            const auto& param = node.sig.allParams[paramOffset + i];
            if (param->type) header += formatType(param->type.get());
        }
        header += ")";
        paramOffset += groupSize;
    }
    
    printNodeHeader(node, header);
    if (node.body) visitChild(node.body.get());
}

void ASTDumper::visit(TypeAliasDeclAST& node) {
    std::string header = "TypeAliasDeclAST '" + toStr(pool, node.name) + "'";
    if (node.aliasedType) header += " = " + formatType(node.aliasedType.get());
    printNodeHeader(node, header);
}

void ASTDumper::visit(GenericParamAST& node) {
    std::string header = "GenericParamAST '" + toStr(pool, node.name) + "'";
    if (!node.constraints.empty()) {
        header += " : ";
        for (size_t i = 0; i < node.constraints.size(); ++i) {
            header += toStr(pool, node.constraints[i]);
            if (i + 1 < node.constraints.size()) header += " + ";
        }
    }
    printNodeHeader(node, header);
}

void ASTDumper::visit(ParamAST& node) {
    std::string header = "ParamAST '" + toStr(pool, node.name) + "'";
    if (node.type) header += " : " + formatType(node.type.get());
    if (node.isVariadic) header += "...";
    printNodeHeader(node, header);
}

// ── Expression nodes ──────────────────────────────────────────────────────
void ASTDumper::visit(LiteralExprAST& node) {
    printNodeHeader(node, "LiteralExprAST '" + toStr(pool, node.value) + "'");
}

void ASTDumper::visit(IdentifierExprAST& node) {
    printNodeHeader(node, "IdentifierExprAST '" + toStr(pool, node.name) + "'");
}

void ASTDumper::visit(ArrayLiteralExprAST& node) {
    printNodeHeader(node, "ArrayLiteralExprAST");
    for (const auto& elem : node.elements) visitChild(elem.get());
}

void ASTDumper::visit(StructLiteralExprAST& node) {
    printNodeHeader(node, "StructLiteralExprAST '" + toStr(pool, node.typeName) + "'");
    for (const auto& init : node.inits) {
        if (init && init->value) visitChild(init->value.get());
    }
}

void ASTDumper::visit(BinaryExprAST& node) {
    printNodeHeader(node, "BinaryExprAST");
    if (node.left) visitChild(node.left.get(), "left");
    if (node.right) visitChild(node.right.get(), "right");
}

void ASTDumper::visit(UnaryExprAST& node) {
    printNodeHeader(node, "UnaryExprAST");
    if (node.operand) visitChild(node.operand.get(), "operand");
}

void ASTDumper::visit(CallExprAST& node) {
    printNodeHeader(node, "CallExprAST");
    if (node.callee) visitChild(node.callee.get(), "callee");
    for (const auto& arg : node.args) visitChild(arg.get());
}

void ASTDumper::visit(IndexExprAST& node) {
    printNodeHeader(node, "IndexExprAST");
    if (node.target) visitChild(node.target.get(), "target");
    if (node.index) visitChild(node.index.get(), "index");
    if (node.sliceEnd) visitChild(node.sliceEnd.get(), "sliceEnd");
}

void ASTDumper::visit(FieldAccessExprAST& node) {
    printNodeHeader(node, "FieldAccessExprAST ." + toStr(pool, node.field));
    if (node.object) visitChild(node.object.get(), "object");
}

void ASTDumper::visit(BehaviorAccessExprAST& node) {
    printNodeHeader(node, "BehaviorAccessExprAST " + toStr(pool, node.typeName) + ":" + toStr(pool, node.method));
}

void ASTDumper::visit(NullableChainExprAST& node) {
    printNodeHeader(node, "NullableChainExprAST");
    if (node.object) visitChild(node.object.get(), "object");
    if (!node.steps.empty()) {
        indent(); out += "\tsteps: ";
        for (size_t i = 0; i < node.steps.size(); ++i) {
            out += toStr(pool, node.steps[i]);
            if (i + 1 < node.steps.size()) out += ", ";
        }
        out += "\n";
    }
}

void ASTDumper::visit(NullCoalesceExprAST& node) {
    printNodeHeader(node, "NullCoalesceExprAST");
    if (node.value) visitChild(node.value.get(), "value");
    if (node.fallback) visitChild(node.fallback.get(), "fallback");
}

void ASTDumper::visit(AssignExprAST& node) {
    printNodeHeader(node, "AssignExprAST");
    if (node.lhs) visitChild(node.lhs.get(), "lhs");
    if (node.rhs) visitChild(node.rhs.get(), "rhs");
}

void ASTDumper::visit(IsExprAST& node) {
    printNodeHeader(node, "IsExprAST");
    if (node.expr) visitChild(node.expr.get(), "expr");
}

void ASTDumper::visit(PipelineExprAST& node) {
    printNodeHeader(node, "PipelineExprAST");
    if (node.seed) visitChild(node.seed.get(), "seed");
    for (const auto& step : node.steps) {
        if (step) visitChild(step.get());
    }
}

void ASTDumper::visit(PipelineStepAST& node) {
    printNodeHeader(node, "PipelineStepAST (kind=" + std::to_string(static_cast<int>(node.kind)) + ")");
    indentLevel++;
    if (node.ident.isValid()) { indent(); out += "ident: " + toStr(pool, node.ident) + "\n"; }
    if (node.typeName.isValid()) { indent(); out += "typeName: " + toStr(pool, node.typeName) + "\n"; }
    if (node.method.isValid()) { indent(); out += "method: " + toStr(pool, node.method) + "\n"; }
    if (node.field.isValid()) { indent(); out += "field: " + toStr(pool, node.field) + "\n"; }
    for (const auto& arg : node.packArgs) visitChild(arg.get(), "packArg");
    if (node.anonFunc) visitChild(node.anonFunc.get(), "anonFunc");
    indentLevel--;
}

void ASTDumper::visit(ComposeExprAST& node) {
    printNodeHeader(node, "ComposeExprAST");
    for (const auto& op : node.operands) {
        if (op) visitChild(op.get());
    }
}

void ASTDumper::visit(ComposeOperandAST& node) {
    printNodeHeader(node, "ComposeOperandAST (kind=" + std::to_string(static_cast<int>(node.kind)) + ")");
    indentLevel++;
    if (node.ident.isValid()) { indent(); out += "ident: " + toStr(pool, node.ident) + "\n"; }
    if (node.typeName.isValid()) { indent(); out += "typeName: " + toStr(pool, node.typeName) + "\n"; }
    if (node.method.isValid()) { indent(); out += "method: " + toStr(pool, node.method) + "\n"; }
    if (node.field.isValid()) { indent(); out += "field: " + toStr(pool, node.field) + "\n"; }
    indentLevel--;
}

void ASTDumper::visit(AnonFuncExprAST& node) {
    std::string header = "AnonFuncExprAST";
    
    // Parameter groups (using flat representation)
    size_t paramOffset = 0;
    for (size_t g = 0; g < node.sig.groupSizes.size(); ++g) {
        header += " (";
        size_t groupSize = node.sig.groupSizes[g];
        for (size_t i = 0; i < groupSize; ++i) {
            if (i > 0) header += ", ";
            const auto& param = node.sig.allParams[paramOffset + i];
            if (param->type) header += formatType(param->type.get());
            if (param->isVariadic) header += "...";
        }
        header += ")";
        paramOffset += groupSize;
    }
    
    // Return types
    if (!node.sig.returnTypes.empty()) {
        header += " -> ";
        for (size_t i = 0; i < node.sig.returnTypes.size(); ++i) {
            if (i > 0) header += ", ";
            header += formatType(node.sig.returnTypes[i].get());
        }
    }
    
    printNodeHeader(node, header);
    if (node.body) visitChild(node.body.get());
}

void ASTDumper::visit(AwaitExprAST& node) {
    printNodeHeader(node, "AwaitExprAST");
    if (node.inner) visitChild(node.inner.get());
}

void ASTDumper::visit(MatchExprAST& node) {
    printNodeHeader(node, "MatchExprAST");
    if (node.subject) visitChild(node.subject.get(), "subject");
    for (const auto& arm : node.arms) visitChild(arm.get());
    if (node.defaultBody) visitChild(node.defaultBody.get(), "default");
}

void ASTDumper::visit(IfExprAST& node) {
    printNodeHeader(node, "IfExprAST");
    if (node.condition) visitChild(node.condition.get(), "cond");
    if (node.thenBranch) visitChild(node.thenBranch.get(), "then");
    if (node.elseBranch) visitChild(node.elseBranch.get(), "else");
}

void ASTDumper::visit(RangeExprAST& node) {
    printNodeHeader(node, "RangeExprAST");
    if (node.lo) visitChild(node.lo.get(), "lo");
    if (node.hi) visitChild(node.hi.get(), "hi");
}

void ASTDumper::visit(TypeConvExprAST& node) {
    std::string header = "TypeConvExprAST";
    if (node.isUnsafe) header += " (unsafe)";
    if (node.targetType) header += " -> " + formatType(node.targetType.get());
    printNodeHeader(node, header);
    if (node.expr) visitChild(node.expr.get());
}

void ASTDumper::visit(ResolveExprAST& node) {
    printNodeHeader(node, "ResolveExprAST");
    indentLevel++;
    if (node.subject) visitChild(node.subject.get(), "subject");
    if (node.okArm) visitChild(node.okArm.get(), "okArm");
    if (node.errArm) visitChild(node.errArm.get(), "errArm");
    indentLevel--;
}

void ASTDumper::visit(OkArmAST& node) {
    std::string header = "OkArmAST '" + toStr(pool, node.bindName) + "'";
    if (node.bindType) header += " : " + formatType(node.bindType.get());
    printNodeHeader(node, header);
    if (node.body) visitChild(node.body.get(), "body");
}

void ASTDumper::visit(ErrArmAST& node) {
    std::string header = "ErrArmAST";
    if (!node.bindName.isValid() && !node.bindType) {
        header += " (bare)";
    } else {
        header += " '" + toStr(pool, node.bindName) + "'";
        if (node.bindType) header += " : " + formatType(node.bindType.get());
    }
    printNodeHeader(node, header);
    if (node.body) visitChild(node.body.get(), "body");
}

// ── Pattern nodes ─────────────────────────────────────────────────────────
void ASTDumper::visit(BindPatternAST& node) {
    printNodeHeader(node, "BindPatternAST '" + toStr(pool, node.name) + "'");
}

void ASTDumper::visit(WildcardPatternAST& node) {
    printNodeHeader(node, "WildcardPatternAST");
}

void ASTDumper::visit(TypePatternAST& node) {
    printNodeHeader(node, "TypePatternAST");
    indentLevel++;
    if (node.checkType) visitChild(node.checkType.get(), "checkType");
    indentLevel--;
}

void ASTDumper::visit(StructPatternAST& node) {
    printNodeHeader(node, "StructPatternAST '" + toStr(pool, node.typeName) + "'");
    for (const auto& field : node.fields) {
        if (field) {
            if (field->subPattern) {
                visitChild(field->subPattern.get(), toStr(pool, field->field));
            } else {
                indent(); out += "\tField: " + toStr(pool, field->field) + " (shorthand)\n";
            }
        }
    }
}

void ASTDumper::visit(PatternExprAST& node) {
    printNodeHeader(node, "PatternExprAST");
    if (node.inner) visitChild(node.inner.get());
}

void ASTDumper::visit(MatchArmAST& node) {
    printNodeHeader(node, "MatchArmAST");
    for (const auto& p : node.patterns) visitChild(p.get());
    if (node.guard) visitChild(node.guard.get(), "guard");
    for (const auto& e : node.exprs) visitChild(e.get());
}

void ASTDumper::visit(DefaultArmAST& node) {
    printNodeHeader(node, "DefaultArmAST");
    for (const auto& e : node.exprs) visitChild(e.get());
}

// ── Statement nodes ───────────────────────────────────────────────────────
void ASTDumper::visit(BlockStmtAST& node) {
    printNodeHeader(node, "BlockStmtAST");
    for (const auto& stmt : node.stmts) visitChild(stmt.get());
}

void ASTDumper::visit(ExprStmtAST& node) {
    printNodeHeader(node, "ExprStmtAST");
    if (node.expr) visitChild(node.expr.get());
}

void ASTDumper::visit(DeclStmtAST& node) {
    printNodeHeader(node, "DeclStmtAST");
    if (node.decl) visitChild(node.decl.get());
}

void ASTDumper::visit(IfStmtAST& node) {
    printNodeHeader(node, "IfStmtAST");
    if (node.condition) visitChild(node.condition.get(), "condition");
    if (node.thenBranch) visitChild(node.thenBranch.get(), "then");
    if (node.elseBranch) visitChild(node.elseBranch.get(), "else");
}

void ASTDumper::visit(SwitchStmtAST& node) {
    printNodeHeader(node, "SwitchStmtAST");
    if (node.subject) visitChild(node.subject.get(), "subject");
    for (const auto& c : node.cases) {
        if (c) {
            indent(); out += "\t\tCase:\n";
            indentLevel++;
            for (const auto& val : c->values) visitChild(val.get());
            if (c->body) visitChild(c->body.get());
            indentLevel--;
        }
    }
    if (node.defaultBody) visitChild(node.defaultBody.get(), "default");
}

void ASTDumper::visit(ForStmtAST& node) {
    printNodeHeader(node, "ForStmtAST '" + toStr(pool, node.iterVar->name) + "'");
    if (node.iterVar->type) {
        indent(); out += "\tvarType: " + formatType(node.iterVar->type.get()) + "\n";
    }
    if (node.iterable) visitChild(node.iterable.get(), "iterable");
    if (node.step) visitChild(node.step.get(), "step");
    if (node.body) visitChild(node.body.get());
}

void ASTDumper::visit(WhileStmtAST& node) {
    printNodeHeader(node, "WhileStmtAST");
    if (node.condition) visitChild(node.condition.get(), "cond");
    if (node.body) visitChild(node.body.get());
}

void ASTDumper::visit(DoWhileStmtAST& node) {
    printNodeHeader(node, "DoWhileStmtAST");
    if (node.body) visitChild(node.body.get());
    if (node.condition) visitChild(node.condition.get(), "cond");
}

void ASTDumper::visit(ReturnStmtAST& node) {
    printNodeHeader(node, "ReturnStmtAST");
    if (node.values.empty()) return;
    
    if (node.values.size() == 1) {
        if (node.values[0]) visitChild(node.values[0].get());
    } else {
        for (size_t i = 0; i < node.values.size(); ++i) {
            if (node.values[i]) {
                visitChild(node.values[i].get(), "value_" + std::to_string(i));
            }
        }
    }
}

void ASTDumper::visit(BreakStmtAST& node) {
    printNodeHeader(node, "BreakStmtAST");
}

void ASTDumper::visit(ContinueStmtAST& node) {
    printNodeHeader(node, "ContinueStmtAST");
}

void ASTDumper::visit(MultiVarDeclAST& node) {
    std::string header = "MultiVarDeclAST (";
    header += (node.keyword == DeclKeyword::Let) ? "let" : "const";
    header += ") ";
    for (size_t i = 0; i < node.vars.size(); ++i) {
        if (i > 0) header += ", ";
        header += toStr(pool, node.vars[i].first);
        if (node.vars[i].second)
            header += " : " + formatType(node.vars[i].second.get());
    }
    header += " = ...";
    printNodeHeader(node, header);
    if (node.rhs) {
        indentLevel++;
        visitChild(node.rhs.get(), "rhs");
        indentLevel--;
    }
}

void ASTDumper::visit(MultiAssignStmtAST& node) {
    std::string header = "MultiAssignStmtAST (";
    for (size_t i = 0; i < node.lhs.size(); ++i) {
        if (i > 0) header += ", ";
        header += "lhs_" + std::to_string(i);
    }
    header += " = ...";
    printNodeHeader(node, header);
    indentLevel++;
    for (size_t i = 0; i < node.lhs.size(); ++i) {
        if (node.lhs[i]) {
            visitChild(node.lhs[i].get(), "lhs_" + std::to_string(i));
        }
    }
    if (node.rhs) visitChild(node.rhs.get(), "rhs");
    indentLevel--;
}

// ── Other nodes ───────────────────────────────────────────────────────────
void ASTDumper::visit(AttributeAST& node) {
    std::string header = "AttributeAST @" + toStr(pool, node.name);
    if (!node.args.empty()) {
        header += "(";
        for (size_t i = 0; i < node.args.size(); ++i) {
            if (i > 0) header += ", ";
            header += toStr(pool, node.args[i]->value);
        }
        header += ")";
    }
    printNodeHeader(node, header);
}

void ASTDumper::visit(AttributeArgAST& node) {
    std::string header = "AttributeArgAST ";
    switch (node.kind) {
        case AttributeArgKind::StringLit: header += "\"" + toStr(pool, node.value) + "\""; break;
        case AttributeArgKind::IntLit:    header += toStr(pool, node.value); break;
        case AttributeArgKind::BoolLit:   header += toStr(pool, node.value); break;
        case AttributeArgKind::TypeIdent: header += toStr(pool, node.value); break;
    }
    printNodeHeader(node, header);
}

void ASTDumper::visit(IntrinsicCallExprAST& node) {
    printNodeHeader(node, "IntrinsicCallExprAST " + toStr(pool, node.intrinsicName));
    if (node.typeArg) visitChild(node.typeArg.get(), "typeArg");
    for (const auto& e : node.args) visitChild(e.get(), "arg");
}

void ASTDumper::visit(UnknownDeclAST& node) {
    printNodeHeader(node, "UnknownDeclAST");
}

void ASTDumper::visit(UnknownExprAST& node) {
    printNodeHeader(node, "UnknownExprAST");
}

void ASTDumper::visit(UnknownStmtAST& node) {
    printNodeHeader(node, "UnknownStmtAST");
}

void ASTDumper::visit(UnknownTypeAST& node) {
    printNodeHeader(node, "UnknownTypeAST");
}

} // namespace LucDebug