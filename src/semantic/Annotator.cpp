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
 *     • TypeConvExprAST when its operand is const
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

    // Helper to log const status changes
    void logConst(const std::string& nodeType, const std::string& name, bool isConst) {
        LUC_LOG_SEMANTIC_EXTREME("\t" << nodeType << " '" << name << "' isConst=" << (isConst ? "true" : "false"));
    }

    // Convenience: dispatch accept() on any non-null BaseAST node.
    void walk(BaseAST* node) {
        if (node) node->accept(*this);
    }

    // ── Declaration nodes ─────────────────────────────────────────────────────

    void visit(PackageDeclAST& /*node*/) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(PackageDeclAST)");
    }

    void visit(UseDeclAST& /*node*/) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(UseDeclAST)");
    }

    void visit(VarDeclAST& node) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(VarDeclAST): name=" << pool_.lookup(node.name));

        for (auto& attr : node.attributes) walk(attr.get());
        if (node.init) walk(node.init.get());

        node.isConst = (node.keyword == DeclKeyword::Const);
        logConst("VarDecl", std::string(pool_.lookup(node.name)), node.isConst);
    }

    void visit(GenericParamAST& /*node*/) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(GenericParamAST)");
    }

    void visit(FuncDeclAST& node) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(FuncDeclAST): name=" << pool_.lookup(node.name)
                            << ", keyword=" << (node.keyword == DeclKeyword::Const ? "const" : "let"));

        for (auto& attr : node.attributes) walk(attr.get());

        // Parameters are in node.sig (FuncSignature), not BaseAST, so no walking.
        // The body is a BlockStmtAST (BaseAST) – walk it.
        walk(node.body.get());

        node.isConst = (node.keyword == DeclKeyword::Const);
        logConst("FuncDecl", std::string(pool_.lookup(node.name)), node.isConst);
    }

    void visit(StructDeclAST& node) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(StructDeclAST): name=" << pool_.lookup(node.name));

        for (auto& attr : node.attributes) walk(attr.get());
        for (auto& field : node.fields) walk(field.get());
        // Struct declarations are not runtime values.
    }

    void visit(FieldDeclAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(FieldDeclAST): name=" << pool_.lookup(node.name));
        if (node.defaultVal) walk(node.defaultVal.get());
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

    void visit(TraitMethodAST& /*node*/) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(TraitMethodAST)");
    }

    void visit(TraitDeclAST& /*node*/) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(TraitDeclAST)");
    }

    void visit(ImplDeclAST& node) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(ImplDeclAST): structName=" << pool_.lookup(node.structName));

        for (auto& method : node.methods) walk(method.get());
    }

    void visit(MethodDeclAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(MethodDeclAST): name=" << pool_.lookup(node.name));

        // Parameters are in node.sig, not BaseAST – skip walking.
        walk(node.body.get());
        node.isConst = false;
    }

    void visit(FromDeclAST& node) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(FromDeclAST): targetType=" << pool_.lookup(node.targetTypeName));

        for (auto& entry : node.entries) walk(entry.get());
        node.isConst = false;
    }

    void visit(FromEntryAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(FromEntryAST)");
        walk(node.body.get());
        node.isConst = false;
    }

    void visit(TypeAliasDeclAST& /*node*/) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(TypeAliasDeclAST)");
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

        bool allConst = !node.elements.empty();
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

        bool allConst = !node.inits.empty();
        for (auto& init : node.inits) {
            if (init->value) {
                walk(init->value.get());
                if (!init->value->isConst) allConst = false;
            } else {
                allConst = false;
            }
        }
        node.isConst = allConst;
        LUC_LOG_SEMANTIC_EXTREME("\tinits=" << node.inits.size()
                               << ", allConst=" << (allConst ? "true" : "false"));
    }

    void visit(FieldAccessExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(FieldAccessExprAST)");
        walk(node.object.get());
        node.isConst = node.object && node.object->isConst;
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
        bool lc = node.left  && node.left->isConst;
        bool rc = node.right && node.right->isConst;
        node.isConst = lc && rc;
        LUC_LOG_SEMANTIC_EXTREME("\tleftConst=" << lc << ", rightConst=" << rc
                               << ", result=" << (node.isConst ? "true" : "false"));
    }

    void visit(UnaryExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(UnaryExprAST)");
        walk(node.operand.get());
        node.isConst = node.operand && node.operand->isConst;
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
        // steps are InternedString, not BaseAST – no walking.
        // Note: NullableChainExprAST has no `fallback` member; that belongs to NullCoalesceExprAST.
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

    void visit(ComposeExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(ComposeExprAST)");
        walk(node.left.get());
        for (auto& op : node.operands) walk(op.get());
        node.isConst = false;
    }

    void visit(AnonFuncExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(AnonFuncExprAST)");
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

    void visit(AttributeAST& /*node*/) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(AttributeAST)");
    }

    void visit(AttributeArgAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(AttributeArgAST)");
        // Attribute arguments are literals; no further walking needed.
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

    void visit(BindPatternAST& /*node*/) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(BindPatternAST)");
    }

    void visit(WildcardPatternAST& /*node*/) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(WildcardPatternAST)");
    }

    void visit(TypePatternAST& /*node*/) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(TypePatternAST)");
    }

    void visit(StructPatternAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(StructPatternAST): type=" << pool_.lookup(node.typeName));
        for (auto& fp : node.fields) {
            if (fp && fp->subPattern) walk(fp->subPattern.get());
        }
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
        // Walk the contained declaration directly (it's a BaseAST).
        walk(node.decl.get());
    }

    void visit(IfStmtAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(IfStmtAST)");
        walk(node.condition.get());
        walk(node.thenBranch.get());
        if (node.elseBranch) walk(node.elseBranch.get());
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

    void visit(BreakStmtAST& /*node*/) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(BreakStmtAST)");
    }

    void visit(ContinueStmtAST& /*node*/) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(ContinueStmtAST)");
    }

    void visit(MultiVarDeclAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(MultiVarDeclAST)");
        walk(node.rhs.get());
        // Variables themselves are not walked as BaseAST; they are pairs of (name, type).
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
};

// ─────────────────────────────────────────────────────────────────────────────
// annotateAll  — public entry point called by SemanticAnalyzer::annotate()
//
// Creates one Annotator instance shared across all files in the package
// (so the symbol table lookup in visitIdentifierExprAST resolves correctly
// for cross-file references that were collected in Phase 1).
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