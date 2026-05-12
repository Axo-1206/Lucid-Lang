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
        for(int i=0; i<indentLevel; ++i) out += '\t';
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
    out += " at line " + std::to_string(node.loc.line) + ", col " + std::to_string(node.loc.column) + "\n";
}

void ASTDumper::visitChild(BaseAST* child, const std::string& label) {
    if (!child) return;
    indentLevel++;
    child->accept(*this);
    indentLevel--;
}

// Helper: convert InternedString to display string
static std::string toStr(const StringPool* pool, const InternedString& s) {
    if (!s.isValid()) return "";
    return std::string(pool->lookup(s));
}

static const std::string primitiveKindStrings[] = {
    "bool", "byte", "short", "int", "long", "ubyte", "ushort", "uint", "ulong",
    "float", "double", "string", "char", "any", "primitive"
};

static std::string primitiveKindToString(PrimitiveKind k) {
    int idx = static_cast<int>(k);
    if (idx >= 0 && idx <= static_cast<int>(PrimitiveKind::Any)) {
        return primitiveKindStrings[idx];
    }
    return "primitive";
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
            std::string res;

            // Qualifiers
            uint32_t q = f->sig.qualifiers;
            if (q & QualifierBits::Async) res += "~async ";
            if (q & QualifierBits::Nullable) res += "~nullable ";
            if (q & QualifierBits::Parallel) res += "~parallel ";

            // Parameter groups
            for (const auto& group : f->sig.paramGroups) {
                res += "(";
                for (size_t i = 0; i < group.size(); ++i) {
                    if (i > 0) res += ", ";
                    if (group[i]->type) res += formatType(group[i]->type.get());
                    if (group[i]->isVariadic) res += "...";
                }
                res += ")";
            }

            // Return types (after ->)
            if (!f->sig.returnTypes.empty()) {
                res += " -> ";
                for (size_t i = 0; i < f->sig.returnTypes.size(); ++i) {
                    if (i > 0) res += ", ";
                    res += formatType(f->sig.returnTypes[i].get());
                }
            }
            return res;
        }
        default:
            return kindToString(type->kind);
    }
}

// ── Root ──────────────────────────────────────────────────────────────────
void ASTDumper::visit(ProgramAST& node) {
    printNodeHeader(node, "ProgramAST");
    indent(); out += "\tpackageName: '" + toStr(pool, node.packageName) + "'\n";
    indent(); out += "\tfilePath: '" + std::string(pool->lookup(node.filePath)) + "'\n";
    indent(); out += "\tdecls (count): " + std::to_string(node.decls.size()) + "\n";

    indentLevel++;
    for (const auto& decl : node.decls) {
        if (decl) {
            decl->accept(*this);
        }
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
    // Parameter groups
    for (const auto& group : node.sig.paramGroups) {
        for (const auto& param : group) {
            if (param) visitChild(param.get(), "param");
        }
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
    
    // Parameter groups
    for (const auto& group : node.sig.paramGroups) {
        header += " (";
        for (size_t i = 0; i < group.size(); ++i) {
            if (i > 0) header += ", ";
            if (group[i]->type) {
                header += formatType(group[i]->type.get());
            }
        }
        header += ")";
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
    
    // Parameter groups
    for (const auto& group : node.sig.paramGroups) {
        header += " (";
        for (size_t i = 0; i < group.size(); ++i) {
            if (i > 0) header += ", ";
            if (group[i]->type) {
                header += formatType(group[i]->type.get());
            }
        }
        header += ")";
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

void ASTDumper::visit(ImplDeclAST& node) {
    std::string header = "ImplDeclAST '" + toStr(pool, node.structName) + "'";
    if (node.traitRef) {
        header += " : " + toStr(pool, node.traitRef->name);
        if (!node.traitRef->genericArgs.empty()) {
            header += "<";
            for (size_t i = 0; i < node.traitRef->genericArgs.size(); ++i) {
                header += formatType(node.traitRef->genericArgs[i].get());
                if (i + 1 < node.traitRef->genericArgs.size()) header += ", ";
            }
            header += ">";
        }
    }
    printNodeHeader(node, header);
    for (const auto& gp : node.genericParams) visitChild(gp.get());
    for (const auto& method : node.methods) visitChild(method.get());
}

void ASTDumper::visit(MethodDeclAST& node) {
    std::string header = "MethodDeclAST '" + toStr(pool, node.name) + "'";

    // Qualifiers
    uint32_t q = node.sig.qualifiers;
    if (q & QualifierBits::Async) header += " ~async";
    if (q & QualifierBits::Nullable) header += " ~nullable";
    if (q & QualifierBits::Parallel) header += " ~parallel";

    // Parameter groups
    for (const auto& group : node.sig.paramGroups) {
        header += " (";
        for (size_t i = 0; i < group.size(); ++i) {
            if (i > 0) header += ", ";
            if (group[i]->type) {
                header += formatType(group[i]->type.get());
            }
        }
        header += ")";
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
    printNodeHeader(node, "FromDeclAST");
    for (const auto& entry : node.entries) {
        visitChild(entry.get());
    }
}

void ASTDumper::visit(FromEntryAST& node) {
    std::string header = "FromEntryAST '" + formatType(node.returnType.get()) + "'";
    for (const auto& group : node.sig.paramGroups) {
        header += " (";
        for (size_t i = 0; i < group.size(); ++i) {
            if (i > 0) header += ", ";
            if (group[i]->type) {
                header += formatType(group[i]->type.get());
            }
        }
        header += ")";
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
        if (init->value) visitChild(init->value.get());
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
    printNodeHeader(node, "PipelineStepAST (kind=" + std::to_string((int)node.kind) + ")");
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
    printNodeHeader(node, "ComposeOperandAST (kind=" + std::to_string((int)node.kind) + ")");
    indentLevel++;
    if (node.ident.isValid()) { indent(); out += "ident: " + toStr(pool, node.ident) + "\n"; }
    if (node.typeName.isValid()) { indent(); out += "typeName: " + toStr(pool, node.typeName) + "\n"; }
    if (node.method.isValid()) { indent(); out += "method: " + toStr(pool, node.method) + "\n"; }
    if (node.field.isValid()) { indent(); out += "field: " + toStr(pool, node.field) + "\n"; }
    indentLevel--;
}

void ASTDumper::visit(AnonFuncExprAST& node) {
    std::string header = "AnonFuncExprAST";

    // Anonymous functions have no qualifiers
    // Parameter groups
    for (const auto& group : node.sig.paramGroups) {
        header += " (";
        for (size_t i = 0; i < group.size(); ++i) {
            if (i > 0) header += ", ";
            if (group[i]->type) {
                header += formatType(group[i]->type.get());
            }
            if (group[i]->isVariadic) header += "...";
        }
        header += ")";
    }

    // Return types (after ->)
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

// ── Pattern nodes ─────────────────────────────────────────────────────────
void ASTDumper::visit(BindPatternAST& node) {
    printNodeHeader(node, "BindPatternAST '" + toStr(pool, node.name) + "'");
}

void ASTDumper::visit(WildcardPatternAST& node) {
    printNodeHeader(node, "WildcardPatternAST");
}

void ASTDumper::visit(TypePatternAST& node) {
    printNodeHeader(node, "TypePatternAST");
}

void ASTDumper::visit(StructPatternAST& node) {
    printNodeHeader(node, "StructPatternAST '" + toStr(pool, node.typeName) + "'");
    for (const auto& field : node.fields) {
        if (field && field->subPattern) visitChild(field->subPattern.get(), toStr(pool, field->field));
        else if (field) {
            indent(); out += "\t\tField: " + toStr(pool, field->field) + " (shorthand)\n";
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
    std::visit([this](const auto& declPtr) {
        if (declPtr) visitChild(declPtr.get());
    }, node.decl);
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
            for (const auto& val : c->values) visitChild(val.get());
            if (c->body) visitChild(c->body.get());
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
        // Single return value – no label for backward compatibility
        if (node.values[0]) visitChild(node.values[0].get());
    } else {
        // Multiple return values – label each with index
        for (size_t i = 0; i < node.values.size(); ++i) {
            if (node.values[i]) {
                std::string label = "value_" + std::to_string(i);
                visitChild(node.values[i].get(), label);
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

void ASTDumper::visit(AttributeAST& node) {
    std::string header = "AttributeAST @" + toStr(pool, node.name);
    if (!node.args.empty()) {
        header += "(";
        for (size_t i = 0; i < node.args.size(); ++i) {
            header += toStr(pool, node.args[i]->value);
            if (i + 1 < node.args.size()) header += ", ";
        }
        header += ")";
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