#include "ASTDumper.hpp"
#include "DebugUtils.hpp"
#include "ast/DeclAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/StmtAST.hpp"
#include "ast/TypeAST.hpp"
#include <sstream>

namespace LucDebug {

ASTDumper::ASTDumper(int verb) : verbosity(verb), indentLevel(0) {
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

// Helper to print a single line with proper indentation
void ASTDumper::printLine(const std::string& text) {
    indent();
    out += text + "\n";
}

// Helper to print key-value pair
void ASTDumper::printKV(const std::string& key, const std::string& value) {
    indent();
    out += "\t" + key + ": '" + value + "'\n";
}

// Helper for node header (indentation + node info)
void ASTDumper::printNodeHeader(const BaseAST& node, const std::string& nodeName) {
    indent();
    out += "[" + std::to_string(indentLevel) + "] " + nodeName;
    out += " (kind=" + kindToString(node.kind) + ")";
    out += " at line " + std::to_string(node.loc.line) + ", col " + std::to_string(node.loc.column) + "\n";
}

void ASTDumper::visitChild(BaseAST* child, const std::string& label) {
    if (!child || verbosity < 2) return;
    indentLevel++;
    child->accept(*this);
    indentLevel--;
}

// Cache for primitive type strings (avoid repeated lookups)
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
            std::string res = n->name;
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
            std::string res = "(";
            for (size_t i = 0; i < f->params.size(); ++i) {
                if (i > 0) res += ", ";
                res += formatType(f->params[i].get());
            }
            res += ")";
            if (f->returnType) {
                res += " " + formatType(f->returnType.get());
            }
            if (f->isNullable) res += "?";
            return res;
        }
        default:
            return kindToString(type->kind);
    }
}

// ── Root ──────────────────────────────────────────────────────────────────
void ASTDumper::visit(ProgramAST& node) {
    printNodeHeader(node, "ProgramAST");
    if (verbosity >= 3) {
        indent(); out += "\tpackageName: '" + node.packageName + "'\n";
        indent(); out += "\tfilePath: '" + node.filePath + "'\n";
        indent(); out += "\tdecls (count): " + std::to_string(node.decls.size()) + "\n";
        
        // Recurse into children
        indentLevel++;
        for (const auto& decl : node.decls) {
            if (decl) {
                indent();
                decl->accept(*this);
            }
        }
        indentLevel--;
    }
}

// ── Type nodes ────────────────────────────────────────────────────────────
void ASTDumper::visit(PrimitiveTypeAST& node) {
    printNodeHeader(node, "PrimitiveTypeAST " + formatType(&node));
}

void ASTDumper::visit(NamedTypeAST& node) {
    printNodeHeader(node, "NamedTypeAST '" + node.name + "'");
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
    if (node.isNullable) header += " (nullable)";
    printNodeHeader(node, header);
    indentLevel++;
    for (const auto& param : node.params) visitChild(param.get(), "param");
    if (node.returnType) visitChild(node.returnType.get(), "return");
    indentLevel--;
}

// ── Declaration nodes ─────────────────────────────────────────────────────
void ASTDumper::visit(PackageDeclAST& node) {
    printNodeHeader(node, "PackageDeclAST '" + node.name + "'");
}

void ASTDumper::visit(UseDeclAST& node) {
    std::string pathStr;
    for (size_t i = 0; i < node.path.size(); ++i) {
        if (i > 0) pathStr += ".";
        pathStr += node.path[i];
    }
    std::string header = "UseDeclAST '" + pathStr + "'";
    if (node.alias) header += " as " + *node.alias;
    printNodeHeader(node, header);
}

void ASTDumper::visit(VarDeclAST& node) {
    std::string header = "VarDeclAST '" + node.name + "'";
    if (node.type) header += " : " + formatType(node.type.get());
    printNodeHeader(node, header);
    indentLevel++;
    if (node.init) visitChild(node.init.get(), "init");
    for (const auto& attr : node.attributes) visitChild(attr.get());
    indentLevel--;
}

void ASTDumper::visit(FuncDeclAST& node) {
    std::string header = "FuncDeclAST '" + node.name + "'";
    for (const auto& group : node.paramGroups) {
        header += " (";
        for (size_t i = 0; i < group.size(); ++i) {
            if (i > 0) header += ", ";
            if (group[i] && group[i]->type) {
                header += formatType(group[i]->type.get());
            }
        }
        header += ")";
    }
    if (node.returnType) header += " " + formatType(node.returnType.get());
    printNodeHeader(node, header);
    indentLevel++;
    if (node.body) visitChild(node.body.get());
    for (const auto& attr : node.attributes) visitChild(attr.get());
    indentLevel--;
}

void ASTDumper::visit(StructDeclAST& node) {
    printNodeHeader(node, "StructDeclAST '" + node.name + "'");
    indentLevel++;
    for (const auto& field : node.fields) visitChild(field.get());
    for (const auto& attr : node.attributes) visitChild(attr.get());
    indentLevel--;
}

void ASTDumper::visit(FieldDeclAST& node) {
    std::string header = "FieldDeclAST '" + node.name + "'";
    if (node.type) header += " : " + formatType(node.type.get());
    printNodeHeader(node, header);
    indentLevel++;
    if (node.defaultVal) {
        indent();
        out += "\tdefault: ";
        indentLevel++; // Increase for the value's own indentation
        node.defaultVal->accept(*this);
        indentLevel--;
    }
    indentLevel--;
}

void ASTDumper::visit(EnumDeclAST& node) {
    printNodeHeader(node, "EnumDeclAST '" + node.name + "'");
    for (const auto& variant : node.variants) visitChild(variant.get());
}

void ASTDumper::visit(EnumVariantAST& node) {
    std::string header = "EnumVariantAST '" + node.name + "'";
    if (node.explicitValue) header += " = " + std::to_string(*node.explicitValue);
    printNodeHeader(node, header);
}

void ASTDumper::visit(TraitMethodAST& node) {
    std::string header = "TraitMethodAST '" + node.name + "'";
    for (const auto& group : node.paramGroups) {
        header += " (";
        for (size_t i = 0; i < group.size(); ++i) {
            if (group[i] && group[i]->type) {
                header += formatType(group[i]->type.get());
                if (i + 1 < group.size()) header += ", ";
            }
        }
        header += ")";
    }
    if (node.returnType) header += " " + formatType(node.returnType.get());
    printNodeHeader(node, header);
}

void ASTDumper::visit(TraitDeclAST& node) {
    printNodeHeader(node, "TraitDeclAST '" + node.name + "'");
    for (const auto& method : node.methods) visitChild(method.get());
}
void ASTDumper::visit(ImplDeclAST& node) {
    std::string header = "ImplDeclAST '" + node.structName + "'";
    if (node.traitRef) {
        header += " : " + node.traitRef->name;
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
    std::string header = "MethodDeclAST '" + node.name + "'";
    for (const auto& group : node.paramGroups) {
        header += " (";
        for (size_t i = 0; i < group.size(); ++i) {
            if (group[i] && group[i]->type) {
                header += formatType(group[i]->type.get());
                if (i + 1 < group.size()) header += ", ";
            }
        }
        header += ")";
    }
    if (node.returnType) header += " " + formatType(node.returnType.get());
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
    std::string header = "FromEntryAST '" + node.returnTypeName + "'";
    for (const auto& group : node.paramGroups) {
        header += " (";
        for (size_t i = 0; i < group.size(); ++i) {
            if (group[i] && group[i]->type) {
                header += formatType(group[i]->type.get());
                if (i + 1 < group.size()) header += ", ";
            }
        }
        header += ")";
    }
    printNodeHeader(node, header);
    if (node.body) visitChild(node.body.get());
}
void ASTDumper::visit(TypeAliasDeclAST& node) {
    std::string header = "TypeAliasDeclAST '" + node.name + "'";
    if (node.aliasedType) header += " = " + formatType(node.aliasedType.get());
    printNodeHeader(node, header);
}

void ASTDumper::visit(ParamAST& node) {
    std::string header = "ParamAST '" + node.name + "'";
    if (node.type) header += " : " + formatType(node.type.get());
    printNodeHeader(node, header);
}

void ASTDumper::visit(GenericParamAST& node) {
    std::string header = "GenericParamAST '" + node.name + "'";
    if (!node.constraints.empty()) {
        header += " : ";
        for (size_t i = 0; i < node.constraints.size(); ++i) {
            header += node.constraints[i];
            if (i + 1 < node.constraints.size()) header += " + ";
        }
    }
    printNodeHeader(node, header);
}


// ── Expression nodes ──────────────────────────────────────────────────────
void ASTDumper::visit(LiteralExprAST& node) {
    printNodeHeader(node, "LiteralExprAST '" + node.value + "'");
}
void ASTDumper::visit(IdentifierExprAST& node) {
    printNodeHeader(node, "IdentifierExprAST '" + node.name + "'");
}

void ASTDumper::visit(ArrayLiteralExprAST& node) {
    printNodeHeader(node, "ArrayLiteralExprAST");
    for (const auto& elem : node.elements) visitChild(elem.get());
}

void ASTDumper::visit(StructLiteralExprAST& node) {
    printNodeHeader(node, "StructLiteralExprAST '" + node.typeName + "'");
    for (const auto& init : node.inits) {
        if (init.value) visitChild(init.value.get());
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
    printNodeHeader(node, "FieldAccessExprAST ." + node.field);
    if (node.object) visitChild(node.object.get(), "object");
}

void ASTDumper::visit(BehaviorAccessExprAST& node) {
    printNodeHeader(node, "BehaviorAccessExprAST " + node.typeName + ":" + node.method);
}

void ASTDumper::visit(NullableChainExprAST& node) {
    printNodeHeader(node, "NullableChainExprAST");
    if (node.object) visitChild(node.object.get(), "object");
    if (!node.steps.empty()) {
        indent(); out += "\tsteps: ";
        for (size_t i = 0; i < node.steps.size(); ++i) {
            out += node.steps[i];
            if (i + 1 < node.steps.size()) out += ", ";
        }
        out += "\n";
    }
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
        if (!step) continue;
        indent(); out += "\tStep: (kind=" + std::to_string((int)step->kind) + ")\n";
        indentLevel++;
        if (!step->ident.empty()) { indent(); out += "ident: " + step->ident + "\n"; }
        if (!step->typeName.empty()) { indent(); out += "typeName: " + step->typeName + "\n"; }
        if (!step->method.empty()) { indent(); out += "method: " + step->method + "\n"; }
        if (!step->field.empty()) { indent(); out += "field: " + step->field + "\n"; }
        for (const auto& arg : step->packArgs) visitChild(arg.get(), "packArg");
        if (step->anonFunc) visitChild(step->anonFunc.get(), "anonFunc");
        indentLevel--;
    }
}

void ASTDumper::visit(ComposeExprAST& node) {
    printNodeHeader(node, "ComposeExprAST");
    for (const auto& op : node.operands) {
        if (!op) continue;
        indent(); out += "\tOperand: (kind=" + std::to_string((int)op->kind) + ")\n";
        indentLevel++;
        if (!op->ident.empty()) { indent(); out += "ident: " + op->ident + "\n"; }
        if (!op->typeName.empty()) { indent(); out += "typeName: " + op->typeName + "\n"; }
        if (!op->method.empty()) { indent(); out += "method: " + op->method + "\n"; }
        if (!op->field.empty()) { indent(); out += "field: " + op->field + "\n"; }
        indentLevel--;
    }
}

void ASTDumper::visit(AnonFuncExprAST& node) {
    std::string header = "AnonFuncExprAST";
    for (const auto& group : node.paramGroups) {
        header += " (";
        for (size_t i = 0; i < group.size(); ++i) {
            if (group[i] && group[i]->type) {
                header += formatType(group[i]->type.get());
                if (i + 1 < group.size()) header += ", ";
            }
        }
        header += ")";
    }
    if (node.returnType) header += " " + formatType(node.returnType.get());
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
    printNodeHeader(node, "BindPatternAST '" + node.name + "'");
}

void ASTDumper::visit(WildcardPatternAST& node) {
    printNodeHeader(node, "WildcardPatternAST");
}

void ASTDumper::visit(TypePatternAST& node) {
    printNodeHeader(node, "TypePatternAST");
}

void ASTDumper::visit(StructPatternAST& node) {
    printNodeHeader(node, "StructPatternAST '" + node.typeName + "'");
    for (const auto& field : node.fields) {
        if (field && field->subPattern) visitChild(field->subPattern.get(), field->field);
        else if (field) {
            indent(); out += "\t\tField: " + field->field + " (shorthand)\n";
        }
    }
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
    printNodeHeader(node, "ForStmtAST '" + node.varName + "'");
    if (node.varType) {
        indent(); out += "\tvarType: " + formatType(node.varType.get()) + "\n";
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
    if (node.value) visitChild(node.value.get());
}

void ASTDumper::visit(BreakStmtAST& node) {
    printNodeHeader(node, "BreakStmtAST");
}

void ASTDumper::visit(ContinueStmtAST& node) {
    printNodeHeader(node, "ContinueStmtAST");
}

void ASTDumper::visit(ParallelForStmtAST& node) {
    printNodeHeader(node, "ParallelForStmtAST '" + node.varName + "'");
    if (node.varType) {
        indent(); out += "\tvarType: " + formatType(node.varType.get()) + "\n";
    }
    if (node.iterable) visitChild(node.iterable.get(), "iterable");
    if (node.step) visitChild(node.step.get(), "step");
    if (node.body) visitChild(node.body.get());
}

void ASTDumper::visit(ParallelBlockStmtAST& node) {
    printNodeHeader(node, "ParallelBlockStmtAST");
    for (const auto& block : node.subBlocks) visitChild(block.get());
}


void ASTDumper::visit(AttributeAST& node) {
    std::string header = "AttributeAST @" + node.name;
    if (!node.args.empty()) {
        header += "(";
        for (size_t i = 0; i < node.args.size(); ++i) {
            header += node.args[i].value;
            if (i + 1 < node.args.size()) header += ", ";
        }
        header += ")";
    }
    printNodeHeader(node, header);
}

void ASTDumper::visit(IntrinsicCallExprAST& node) {
    printNodeHeader(node, "IntrinsicCallExprAST " + node.intrinsicName);
    if (node.typeArg) visitChild(node.typeArg.get(), "typeArg");
    for (const auto& e : node.args) visitChild(e.get(), "arg");
}

} // namespace LucDebug
