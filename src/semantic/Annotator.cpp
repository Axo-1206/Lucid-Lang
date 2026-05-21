/**
 * @file Annotator.cpp
 *
 * @nutshell Attaches final semantic properties to raw AST nodes.
 *
 * @reason Once the compiler has successfully completed all types and symbol resolutions, the AST
 *   itself needs to carry these contextual stamps to safely interface with the machine code
 *   generator (Codegen).
 *
 * @responsibility Phase 4 of semantic analysis: binds resolved state context back to the
 *   abstract syntax tree.
 *
 * @logic
 *   Annotator is an ASTVisitor that does a full post-order walk of every ProgramAST after
 *   Phase 3 has finished. It stamps the following BaseAST fields:
 *
 *   isConst  — true when a node is a compile-time constant. Set on:
 *     • every LiteralExprAST except nil  (42, 3.14, "hi", true, false, …)
 *     • every VarDeclAST / FuncDeclAST declared with the 'const' keyword
 *     • every IdentifierExprAST whose symbol was declared with 'const'
 *     • enum declarations and their variants (integer-backed, fixed at compile time)
 *     • BinaryExprAST / UnaryExprAST / RangeExprAST / FieldAccessExprAST whose
 *       sub-expressions are all const  (constant-folding marker, not folded here)
 *     • TypeConvExprAST when its operand is const and conversion is safe
 *     • ArrayLiteralExprAST / StructLiteralExprAST when all elements/inits are const
 *
 *   isBehaviorMember — reinforced on BehaviorAccessExprAST (already written inline
 *     by checkExpr in Phase 3; re-affirmed here so codegen can rely on it regardless
 *     of visit order).
 *
 *   resolvedType and scopeDepth are NOT touched here — they are written inline
 *   by checkExpr (Phase 3b) and checkBlock (Phase 3c) respectively.
 *
 * @related SemanticExpr.cpp, SemanticStmt.cpp, SemanticAnalyzer.cpp
 */

#include "header/SemanticSymbol.hpp"
#include "header/SymbolTable.hpp"
#include "ast/BaseAST.hpp"
#include "ast/DeclAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/StmtAST.hpp"
#include "ast/TypeAST.hpp"
#include "ast/support/StringPool.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Annotator  — Phase 4 ASTVisitor
//
// Performs a recursive, post-order tree walk so that when we reach a parent
// node, all its children already have their isConst flags set, enabling
// bottom-up const propagation in a single pass without separate fixup phases.
// ─────────────────────────────────────────────────────────────────────────────

class Annotator : public ASTVisitor {
public:
    explicit Annotator(SymbolTable& symbols, StringPool& pool)
        : symbols_(symbols), pool_(pool) {
        LUC_LOG_SEMANTIC_VERBOSE("Annotator constructed");
    }

    // Entry point — annotate every top-level declaration in one program file.
    void annotateProgram(ProgramAST& prog) {
        LUC_LOG_SEMANTIC_EXTREME("annotateProgram: processing file: " << pool_.lookup(prog.filePath));
        for (auto& decl : prog.decls) {
            if (decl) decl->accept(*this);
        }
    }

private:
    SymbolTable& symbols_;
    StringPool& pool_;

    void logConst(const std::string& nodeType, const std::string& name, bool isConst) {
        LUC_LOG_SEMANTIC_EXTREME("\t" << nodeType << " '" << name << "' isConst=" << (isConst ? "true" : "false"));
    }

    // Walk any BaseAST node if non-null.
    void walk(BaseAST* node) {
        if (node) node->accept(*this);
    }

    // ── Declaration nodes ─────────────────────────────────────────────────────

    void visit(PackageDeclAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(PackageDeclAST): name=" << pool_.lookup(node.name));
    }

    void visit(UseDeclAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(UseDeclAST)");
        // No walkable children (path and alias are InternedString)
    }

    void visit(VarDeclAST& node) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(VarDeclAST): name=" << pool_.lookup(node.name));
        for (auto& attr : node.attributes) walk(attr.get());
        walk(node.init.get());
        node.isConst = (node.keyword == DeclKeyword::Const);
        logConst("VarDecl", std::string(pool_.lookup(node.name)), node.isConst);
    }

    void visit(GenericParamAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(GenericParamAST): name=" << pool_.lookup(node.name));
        // Generic parameters are not runtime values.
    }

    void visit(ParamAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(ParamAST): name=" << pool_.lookup(node.name));
        // Parameters are never const (they are runtime values).
        node.isConst = false;
    }

    void visit(FuncDeclAST& node) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(FuncDeclAST): name=" << pool_.lookup(node.name)
                            << ", keyword=" << (node.keyword == DeclKeyword::Const ? "const" : "let"));
        for (auto& attr : node.attributes) walk(attr.get());
        for (auto& gp : node.genericParams) walk(gp.get());
        // Parameters are in node.sig.paramGroups – they are ParamAST (BaseAST).
        for (const auto& group : node.sig.paramGroups) {
            for (const auto& param : group) {
                walk(param.get());
            }
        }
        walk(node.body.get());
        node.isConst = (node.keyword == DeclKeyword::Const);
        logConst("FuncDecl", std::string(pool_.lookup(node.name)), node.isConst);
    }

    void visit(StructDeclAST& node) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(StructDeclAST): name=" << pool_.lookup(node.name));
        for (auto& attr : node.attributes) walk(attr.get());
        for (auto& gp : node.genericParams) walk(gp.get());
        for (auto& field : node.fields) walk(field.get());
        // Struct declarations are not runtime values.
    }

    void visit(FieldDeclAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(FieldDeclAST): name=" << pool_.lookup(node.name));
        if (node.defaultVal) walk(node.defaultVal.get());
        // Fields are not runtime values by themselves; default values may be const.
    }

    void visit(EnumDeclAST& node) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(EnumDeclAST): name=" << pool_.lookup(node.name));
        for (auto& variant : node.variants) walk(variant.get());
        node.isConst = true;
        logConst("EnumDecl", std::string(pool_.lookup(node.name)), node.isConst);
    }

    void visit(EnumVariantAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(EnumVariantAST): name=" << pool_.lookup(node.name));
        node.isConst = true;
    }

    void visit(TraitMethodAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(TraitMethodAST): name=" << pool_.lookup(node.name));
        // Trait methods have no bodies; parameters are in node.sig.
        for (const auto& group : node.sig.paramGroups) {
            for (const auto& param : group) walk(param.get());
        }
        // No const for trait methods (they are just signatures)
    }

    void visit(TraitDeclAST& node) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(TraitDeclAST): name=" << pool_.lookup(node.name));
        for (auto& gp : node.genericParams) walk(gp.get());
        for (auto& method : node.methods) walk(method.get());
    }

    void visit(ImplDeclAST& node) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(ImplDeclAST): targetType="
            << (node.targetType ? LucDebug::kindToString(node.targetType->kind) : "<null>"));
        for (auto& attr : node.attributes) walk(attr.get());
        for (auto& gp : node.genericParams) walk(gp.get());
        if (node.targetType) walk(node.targetType.get()); // targetType is a TypeAST
        if (node.traitRef) walk(node.traitRef.get());
        for (auto& method : node.methods) walk(method.get());
    }

    void visit(MethodDeclAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(MethodDeclAST): name=" << pool_.lookup(node.name));
        for (const auto& group : node.sig.paramGroups) {
            for (const auto& param : group) walk(param.get());
        }
        walk(node.body.get());
        node.isConst = false; // Methods are never compile-time constants.
    }

    void visit(FromDeclAST& node) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(FromDeclAST): targetType="
            << (node.targetType ? LucDebug::kindToString(node.targetType->kind) : "<null>"));
        for (auto& attr : node.attributes) walk(attr.get());
        for (auto& gp : node.genericParams) walk(gp.get());
        if (node.targetType) walk(node.targetType.get());
        for (auto& entry : node.entries) walk(entry.get());
        node.isConst = false;
    }

    void visit(FromEntryAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(FromEntryAST)");
        for (const auto& group : node.sig.paramGroups) {
            for (const auto& param : group) walk(param.get());
        }
        if (node.returnType) walk(node.returnType.get());
        walk(node.body.get());
        node.isConst = false;
    }

    void visit(TypeAliasDeclAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(TypeAliasDeclAST): name=" << pool_.lookup(node.name));
        for (auto& gp : node.genericParams) walk(gp.get());
        if (node.aliasedType) walk(node.aliasedType.get());
        // Type aliases are not runtime values.
    }

    // ── Expression nodes ──────────────────────────────────────────────────────

    void visit(LiteralExprAST& node) override {
        node.isConst = (node.kind != LiteralKind::Nil);
        LUC_LOG_SEMANTIC_EXTREME("visit(LiteralExprAST): value='" << pool_.lookup(node.value)
                               << "', isConst=" << (node.isConst ? "true" : "false"));
    }

    void visit(IdentifierExprAST& node) override {
        Symbol* sym = symbols_.lookup(node.name);
        if (sym) {
            node.isConst = (sym->declKw == DeclKeyword::Const);
            LUC_LOG_SEMANTIC_EXTREME("visit(IdentifierExprAST): name=" << pool_.lookup(node.name)
                                   << ", isConst=" << (node.isConst ? "true" : "false"));
        } else {
            LUC_LOG_SEMANTIC_EXTREME("visit(IdentifierExprAST): name=" << pool_.lookup(node.name) << " (symbol not found)");
            node.isConst = false;
        }
    }

    void visit(ArrayLiteralExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(ArrayLiteralExprAST)");
        bool allConst = true;
        for (auto& elem : node.elements) {
            walk(elem.get());
            if (!elem || !elem->isConst) allConst = false;
        }
        node.isConst = allConst;
        LUC_LOG_SEMANTIC_EXTREME("\telements=" << node.elements.size()
                               << ", allConst=" << (allConst ? "true" : "false"));
    }

    void visit(StructLiteralExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(StructLiteralExprAST): type=" << pool_.lookup(node.typeName));
        bool allConst = true;
        for (auto& init : node.inits) {
            walk(init.get());
            if (init && init->value && !init->value->isConst) allConst = false;
            if (!init || !init->value) allConst = false;
        }
        node.isConst = allConst;
        LUC_LOG_SEMANTIC_EXTREME("\tinits=" << node.inits.size()
                               << ", allConst=" << (allConst ? "true" : "false"));
    }

    void visit(FieldInitAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(FieldInitAST): field=" << pool_.lookup(node.name));
        walk(node.value.get());
    }

    void visit(FieldAccessExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(FieldAccessExprAST)");
        walk(node.object.get());
        node.isConst = (node.object && node.object->isConst);
    }

    void visit(BehaviorAccessExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(BehaviorAccessExprAST): " << pool_.lookup(node.typeName) << ":" << pool_.lookup(node.method));
        node.isBehaviorMember = true;
        node.isConst = false;
    }

    void visit(BinaryExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(BinaryExprAST)");
        walk(node.left.get());
        walk(node.right.get());
        bool lc = node.left && node.left->isConst;
        bool rc = node.right && node.right->isConst;
        node.isConst = lc && rc;
        LUC_LOG_SEMANTIC_EXTREME("\tleftConst=" << lc << ", rightConst=" << rc
                               << ", result=" << (node.isConst ? "true" : "false"));
    }

    void visit(UnaryExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(UnaryExprAST)");
        walk(node.operand.get());
        node.isConst = (node.operand && node.operand->isConst);
    }

    void visit(AssignExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(AssignExprAST)");
        walk(node.lhs.get());
        walk(node.rhs.get());
        node.isConst = false;
    }

    void visit(CallExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(CallExprAST)");
        walk(node.callee.get());
        for (auto& arg : node.args) walk(arg.get());
        node.isConst = false;
    }

    void visit(IndexExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(IndexExprAST)");
        walk(node.target.get());
        walk(node.index.get());
        if (node.sliceEnd) walk(node.sliceEnd.get());
        node.isConst = false;
    }

    void visit(IsExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(IsExprAST)");
        walk(node.expr.get());
        node.isConst = false;
    }

    void visit(NullableChainExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(NullableChainExprAST)");
        walk(node.object.get());
        node.isConst = false;
    }

    void visit(NullCoalesceExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(NullCoalesceExprAST)");
        walk(node.value.get());
        walk(node.fallback.get());
        node.isConst = false;
    }

    void visit(PipelineExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(PipelineExprAST)");
        walk(node.seed.get());
        for (auto& step : node.steps) walk(step.get());
        node.isConst = false;
    }

    void visit(PipelineStepAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(PipelineStepAST)");
        for (auto& arg : node.packArgs) walk(arg.get());
        if (node.index) walk(node.index.get());
        if (node.anonFunc) walk(node.anonFunc.get());
        // Steps themselves are not const.
    }

    void visit(ComposeExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(ComposeExprAST)");
        walk(node.left.get());
        for (auto& op : node.operands) walk(op.get());
        node.isConst = false;
    }

    void visit(ComposeOperandAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(ComposeOperandAST)");
        // No walkable children (ident, typeName, method, field are InternedString)
    }

    void visit(AnonFuncExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(AnonFuncExprAST)");
        for (const auto& group : node.sig.paramGroups) {
            for (const auto& param : group) walk(param.get());
        }
        walk(node.body.get());
        node.isConst = false;
    }

    void visit(AwaitExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(AwaitExprAST)");
        walk(node.inner.get());
        node.isConst = false;
    }

    void visit(MatchExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(MatchExprAST)");
        walk(node.subject.get());
        for (auto& arm : node.arms) walk(arm.get());
        if (node.defaultBody) walk(node.defaultBody.get());
        node.isConst = false;
    }

    void visit(IfExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(IfExprAST)");
        walk(node.condition.get());
        if (node.thenBranch) walk(node.thenBranch.get());
        if (node.elseBranch) walk(node.elseBranch.get());
        node.isConst = false;
    }

    void visit(RangeExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(RangeExprAST)");
        walk(node.lo.get());
        walk(node.hi.get());
        bool lc = node.lo && node.lo->isConst;
        bool hc = node.hi && node.hi->isConst;
        node.isConst = lc && hc;
    }

    void visit(TypeConvExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(TypeConvExprAST): isUnsafe=" << (node.isUnsafe ? "true" : "false"));
        walk(node.expr.get());
        node.isConst = !node.isUnsafe && node.expr && node.expr->isConst;
    }

    void visit(IntrinsicCallExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(IntrinsicCallExprAST): name=" << pool_.lookup(node.intrinsicName));
        for (auto& arg : node.args) walk(arg.get());
        bool isCompileTimeType = (pool_.lookup(node.intrinsicName) == "sizeof" ||
                                  pool_.lookup(node.intrinsicName) == "alignof");
        node.isConst = isCompileTimeType;
        LUC_LOG_SEMANTIC_EXTREME("\tisConst=" << (node.isConst ? "true" : "false"));
    }

    // ── Pattern nodes ─────────────────────────────────────────────────────────

    void visit(BindPatternAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(BindPatternAST): name=" << pool_.lookup(node.name));
        // Patterns are not runtime values; no const flag needed.
    }

    void visit(WildcardPatternAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(WildcardPatternAST)");
    }

    void visit(TypePatternAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(TypePatternAST)");
        if (node.checkType) walk(node.checkType.get());
    }

    void visit(StructPatternAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(StructPatternAST): type=" << pool_.lookup(node.typeName));
        for (auto& fp : node.fields) walk(fp.get());
    }

    void visit(FieldPatternAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(FieldPatternAST): field=" << pool_.lookup(node.field));
        if (node.subPattern) walk(node.subPattern.get());
    }

    void visit(PatternExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(PatternExprAST)");
        walk(node.inner.get());
    }

    void visit(MatchArmAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(MatchArmAST)");
        for (auto& pat : node.patterns) walk(pat.get());
        if (node.guard) walk(node.guard.get());
        for (auto& expr : node.exprs) walk(expr.get());
    }

    void visit(DefaultArmAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(DefaultArmAST)");
        for (auto& expr : node.exprs) walk(expr.get());
    }

    // ── Statement nodes ───────────────────────────────────────────────────────

    void visit(BlockStmtAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(BlockStmtAST): " << node.stmts.size() << " statements");
        for (auto& stmt : node.stmts) walk(stmt.get());
    }

    void visit(ExprStmtAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(ExprStmtAST)");
        walk(node.expr.get());
    }

    void visit(DeclStmtAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(DeclStmtAST)");
        walk(node.decl.get());
    }

    void visit(IfStmtAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(IfStmtAST)");
        walk(node.condition.get());
        walk(node.thenBranch.get());
        if (node.elseBranch) walk(node.elseBranch.get());
    }

    void visit(SwitchCaseAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(SwitchCaseAST)");
        for (auto& val : node.values) walk(val.get());
        if (node.body) walk(node.body.get());
    }

    void visit(SwitchStmtAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(SwitchStmtAST): " << node.cases.size() << " cases");
        walk(node.subject.get());
        for (auto& cas : node.cases) {
            for (auto& val : cas->values) walk(val.get());
            if (cas->body) walk(cas->body.get());
        }
        if (node.defaultBody) walk(node.defaultBody.get());
    }

    void visit(ForStmtAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(ForStmtAST): var=" << (node.iterVar ? pool_.lookup(node.iterVar->name) : "<unnamed>"));
        if (node.iterVar) walk(node.iterVar.get());
        walk(node.iterable.get());
        if (node.step) walk(node.step.get());
        walk(node.body.get());
    }

    void visit(WhileStmtAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(WhileStmtAST)");
        walk(node.condition.get());
        walk(node.body.get());
    }

    void visit(DoWhileStmtAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(DoWhileStmtAST)");
        walk(node.body.get());
        walk(node.condition.get());
    }

    void visit(ReturnStmtAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(ReturnStmtAST)");
        for (auto& val : node.values) walk(val.get());
    }

    void visit(BreakStmtAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(BreakStmtAST)");
    }

    void visit(ContinueStmtAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(ContinueStmtAST)");
    }

    void visit(MultiVarDeclAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(MultiVarDeclAST)");
        walk(node.rhs.get());
        // Variables themselves are not BaseAST; no walking.
        node.isConst = (node.keyword == DeclKeyword::Const);
    }

    void visit(MultiAssignStmtAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(MultiAssignStmtAST)");
        for (auto& lhs : node.lhs) walk(lhs.get());
        walk(node.rhs.get());
    }

    // ── Root ──────────────────────────────────────────────────────────────────

    void visit(ProgramAST& node) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(ProgramAST): file=" << pool_.lookup(node.filePath));
        for (auto& decl : node.decls) walk(decl.get());
    }

    // ── Type nodes (these are visited only when they appear as part of declarations) ──
    // We don't set isConst on type nodes; they are not runtime values.
    void visit(PrimitiveTypeAST&) override {}
    void visit(NamedTypeAST&) override {}
    void visit(NullableTypeAST& node) override { if (node.inner) walk(node.inner.get()); }
    void visit(FixedArrayTypeAST& node) override { if (node.element) walk(node.element.get()); }
    void visit(SliceTypeAST& node) override { if (node.element) walk(node.element.get()); }
    void visit(DynamicArrayTypeAST& node) override { if (node.element) walk(node.element.get()); }
    void visit(RefTypeAST& node) override { if (node.inner) walk(node.inner.get()); }
    void visit(PtrTypeAST& node) override { if (node.inner) walk(node.inner.get()); }
    void visit(FuncTypeAST& node) override {
        for (const auto& group : node.sig.paramGroups) {
            for (const auto& param : group) walk(param.get());
        }
        for (auto& ret : node.sig.returnTypes) {
            if (ret) walk(ret.get());
        }
    }

    // ── Attribute nodes ───────────────────────────────────────────────────────
    void visit(AttributeAST& node) override {
        for (auto& arg : node.args) walk(arg.get());
    }
    void visit(AttributeArgAST&) override {}

    // ── Unknown nodes (ignore) ────────────────────────────────────────────────
    void visit(UnknownDeclAST&) override {}
    void visit(UnknownExprAST&) override {}
    void visit(UnknownStmtAST&) override {}
    void visit(UnknownTypeAST&) override {}
};

// ─────────────────────────────────────────────────────────────────────────────
// annotateAll  — public entry point called by SemanticAnalyzer::annotate()
// ─────────────────────────────────────────────────────────────────────────────
void annotateAll(std::vector<ProgramAST*>& files, SymbolTable& symbols, StringPool& pool) {
    LUC_LOG_SEMANTIC_VERBOSE("annotateAll: starting annotation pass on " << files.size() << " files");

    Annotator annotator(symbols, pool);
    for (auto* prog : files) {
        if (prog) {
            LUC_LOG_SEMANTIC_EXTREME("\tannotating file: " << pool.lookup(prog->filePath));
            annotator.annotateProgram(*prog);
        }
    }

    LUC_LOG_SEMANTIC_VERBOSE("annotateAll: annotation pass complete");
}