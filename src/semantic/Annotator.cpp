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
 *     • every VarDeclAST / FuncDeclAST declared with the 'val' keyword
 *     • every IdentifierExprAST whose symbol was declared with 'val'
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

#include "SemanticSymbol.hpp"
#include "SymbolTable.hpp"
#include "ast/BaseAST.hpp"
#include "ast/DeclAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/StmtAST.hpp"
#include "ast/TypeAST.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Annotator  — Phase 4 ASTVisitor
//
// Performs a recursive, post-order tree walk so that when we reach a parent
// node, all its children already have their isConst flags set, enabling
// bottom-up const propagation in a single pass without separate fixup phases.
// ─────────────────────────────────────────────────────────────────────────────

class Annotator : public ASTVisitor {
public:
    explicit Annotator(SymbolTable& symbols) : symbols_(symbols) {}

    // Entry point — annotate every top-level declaration in one program file.
    void annotateProgram(ProgramAST& prog) {
        for (auto& decl : prog.decls) {
            if (decl) decl->accept(*this);
        }
    }

private:
    SymbolTable& symbols_;

    // Convenience: dispatch accept() on any non-null BaseAST node.
    void walk(BaseAST* node) {
        if (node) node->accept(*this);
    }

    // ── Declaration nodes ─────────────────────────────────────────────────────

    void visit(PackageDeclAST& /*node*/) override {
        // No semantic annotations needed on package declarations.
    }

    void visit(UseDeclAST& /*node*/) override {
        // No semantic annotations needed on use declarations.
    }

    void visit(ModuleDeclAST& /*node*/) override {
        // No semantic annotations needed on module declarations.
    }

    void visit(VarDeclAST& node) override {
        // Walk the initialiser first (post-order).
        if (node.init) walk(node.init.get());

        // 'val' variables are compile-time constants.
        //   val MAX int  = 65536   → MAX.isConst = true
        //   imt PI float = 3.14    → PI.isConst = false  (immut, not compile-time)
        //   let x  int   = 0       → x.isConst  = false
        node.isConst = (node.keyword == DeclKeyword::Val);
    }

    void visit(ParamAST& node) override {
        // Parameters are always let-like — they accept any value passed in.
        node.isConst = false;
    }

    void visit(GenericParamAST& /*node*/) override {
        // Generic params are compile-time, but they are not runtime values.
    }

    void visit(FuncDeclAST& node) override {
        // Walk parameter groups and body first.
        for (auto& group : node.paramGroups)
            for (auto& param : group)
                walk(param.get());
        walk(node.body.get());

        // 'val' functions are compile-time-bound (not reassignable at call sites).
        node.isConst = (node.keyword == DeclKeyword::Val);
    }

    void visit(StructDeclAST& node) override {
        for (auto& field : node.fields)
            walk(field.get());
        // Struct type declarations themselves are not runtime values.
    }

    void visit(FieldDeclAST& node) override {
        // Annotate the default value expression if present.
        if (node.defaultVal) walk(node.defaultVal.get());
    }

    void visit(EnumDeclAST& node) override {
        for (auto& variant : node.variants)
            walk(variant.get());
        // Enum declarations represent a closed set of compile-time integers.
        node.isConst = true;
    }

    void visit(EnumVariantAST& node) override {
        // Every enum variant is a named integer constant.
        node.isConst = true;
    }

    void visit(TraitMethodAST& /*node*/) override {
        // Trait method signatures have no body — nothing to annotate.
    }

    void visit(TraitDeclAST& /*node*/) override {
        // Trait declarations are structural contracts, not runtime values.
    }

    void visit(ImplDeclAST& node) override {
        for (auto& method : node.methods)
            walk(method.get());
    }

    void visit(MethodDeclAST& node) override {
        for (auto& group : node.paramGroups)
            for (auto& param : group)
                walk(param.get());
        walk(node.body.get());
        // Methods are never compile-time constants; they dispatch at runtime.
        node.isConst = false;
    }

    void visit(FromDeclAST& node) override {
        for (auto& entry : node.entries)
            walk(entry.get());
        // Conversion blocks themselves are not constants.
        node.isConst = false;
    }

    void visit(FromEntryAST& node) override {
        for (auto& group : node.paramGroups)
            for (auto& param : group)
                walk(param.get());
        walk(node.body.get());
        // Individual conversion entries are not constants.
        node.isConst = false;
    }

    void visit(TypeAliasDeclAST& /*node*/) override {
        // Type alias declarations are purely compile-time name mappings.
    }

    void visit(ExternDeclAST& node) override {
        // External C / Vulkan symbols are resolved by the linker — never const.
        node.isConst = false;
    }

    // ── Expression nodes ──────────────────────────────────────────────────────

    void visit(LiteralExprAST& node) override {
        // Every literal except nil is a compile-time scalar constant.
        // nil has no concrete type, so marking it const could mislead codegen
        // into thinking a nullable slot has a permanent non-null value.
        node.isConst = (node.kind != LiteralKind::Nil);
    }

    void visit(IdentifierExprAST& node) override {
        // Propagate isConst from the symbol's declaration keyword.
        // isBehaviorMember was already set by checkExpr in Phase 3.
        Symbol* sym = symbols_.lookup(node.name);
        if (sym) {
            node.isConst = (sym->declKw == DeclKeyword::Val);
        }
    }

    void visit(ArrayLiteralExprAST& node) override {
        bool allConst = !node.elements.empty();
        for (auto& elem : node.elements) {
            walk(elem.get());
            if (!elem || !elem->isConst) allConst = false;
        }
        // An array literal is const only when every element is const.
        // An empty array literal is treated as non-const (runtime-sized).
        node.isConst = allConst;
    }

    void visit(StructLiteralExprAST& node) override {
        bool allConst = !node.inits.empty();
        for (auto& init : node.inits) {
            if (init.value) {
                walk(init.value.get());
                if (!init.value->isConst) allConst = false;
            } else {
                allConst = false;
            }
        }
        // A struct literal is const only when every field initialiser is const.
        node.isConst = allConst;
    }

    void visit(FieldAccessExprAST& node) override {
        // Walk the object first (post-order).
        walk(node.object.get());
        // Field access is const when the object itself is const.
        // This covers the enum variant case: Direction.North — Direction is
        // an enum (isConst=true) so North access is also const.
        node.isConst = node.object && node.object->isConst;
    }

    void visit(BehaviorAccessExprAST& node) override {
        // Reinforce isBehaviorMember (set by Phase 3 checkExpr).
        // Behavior (impl method) references are not runtime constants.
        node.isBehaviorMember = true;
        node.isConst = false;
    }

    void visit(BinaryExprAST& node) override {
        // Walk both operands first (post-order).
        walk(node.left.get());
        walk(node.right.get());
        // A binary expression is a compile-time constant when both operands are.
        bool lc = node.left  && node.left->isConst;
        bool rc = node.right && node.right->isConst;
        node.isConst = lc && rc;
    }

    void visit(UnaryExprAST& node) override {
        walk(node.operand.get());
        // Propagate constness from the single operand.
        node.isConst = node.operand && node.operand->isConst;
    }

    void visit(AssignExprAST& node) override {
        walk(node.lhs.get());
        walk(node.rhs.get());
        // Assignments are side-effecting — never const.
        node.isConst = false;
    }

    void visit(CallExprAST& node) override {
        walk(node.callee.get());
        for (auto& arg : node.args) walk(arg.get());
        // Function calls may have side effects — treat as non-const.
        // (Constant folding of pure built-ins could be added in a future pass.)
        node.isConst = false;
    }

    void visit(IndexExprAST& node) override {
        walk(node.target.get());
        walk(node.index.get());
        if (node.sliceEnd) walk(node.sliceEnd.get());
        // Index accesses involve runtime memory — not compile-time const.
        node.isConst = false;
    }

    void visit(IsExprAST& node) override {
        walk(node.expr.get());
        // 'x is int' is a runtime type check — not compile-time const.
        node.isConst = false;
    }

    void visit(NullableChainExprAST& node) override {
        walk(node.object.get());
        if (node.fallback) walk(node.fallback.get());
        // Nullable chains involve runtime null checks — not const.
        node.isConst = false;
    }

    void visit(PipelineExprAST& node) override {
        walk(node.seed.get());
        // Pipeline steps may call functions — not compile-time const.
        node.isConst = false;
    }

    void visit(ComposeExprAST& node) override {
        walk(node.left.get());
        // Composed functions are higher-order runtime values — not const.
        node.isConst = false;
    }

    void visit(AnonFuncExprAST& node) override {
        for (auto& group : node.paramGroups)
            for (auto& param : group)
                walk(param.get());
        walk(node.body.get());
        // Anonymous function expressions are runtime closures — not const.
        node.isConst = false;
    }

    void visit(AwaitExprAST& node) override {
        walk(node.inner.get());
        // async/await always involves runtime scheduling — never const.
        node.isConst = false;
    }

    void visit(MatchExprAST& node) override {
        walk(node.subject.get());
        for (auto& arm : node.arms)     walk(arm.get());
        if (node.defaultBody)           walk(node.defaultBody.get());
        // match expressions dispatch at runtime — not compile-time const.
        node.isConst = false;
    }

    void visit(IfExprAST& node) override {
        walk(node.condition.get());
        if (node.thenBranch) walk(node.thenBranch.get());
        if (node.elseBranch) walk(node.elseBranch.get());
        // if-expr evaluates a runtime condition — not compile-time const.
        node.isConst = false;
    }

    void visit(RangeExprAST& node) override {
        walk(node.lo.get());
        walk(node.hi.get());
        // A range is const when both bounds are constant literals (e.g. 0..10).
        bool lc = node.lo && node.lo->isConst;
        bool hc = node.hi && node.hi->isConst;
        node.isConst = lc && hc;
    }

    void visit(TypeConvExprAST& node) override {
        walk(node.expr.get());
        // A safe type conversion of a const expression is itself const.
        // Unsafe (*T) conversions reference raw memory — not const.
        node.isConst = !node.isUnsafe && node.expr && node.expr->isConst;
    }

    // ── Pattern nodes ─────────────────────────────────────────────────────────

    void visit(BindPatternAST& /*node*/) override {
        // Bind patterns introduce names — no isConst semantics.
    }

    void visit(WildcardPatternAST& /*node*/) override {
        // Wildcard patterns discard values — no isConst semantics.
    }

    void visit(TypePatternAST& /*node*/) override {
        // Type patterns are compile-time structural checks on union types.
    }

    void visit(StructPatternAST& node) override {
        for (auto& fp : node.fields) {
            if (fp && fp->subPattern) walk(fp->subPattern.get());
        }
    }

    void visit(MatchArmAST& node) override {
        for (auto& pat : node.patterns) walk(pat.get());
        if (node.guard) walk(node.guard.get());
        for (auto& expr : node.exprs)   walk(expr.get());
    }

    void visit(DefaultArmAST& node) override {
        for (auto& expr : node.exprs) walk(expr.get());
    }

    // ── Statement nodes ───────────────────────────────────────────────────────

    void visit(BlockStmtAST& node) override {
        // scopeDepth was already written by checkBlock in SemanticStmt.cpp.
        // We only need to walk child statements so their annotations propagate.
        for (auto& stmt : node.stmts) walk(stmt.get());
    }

    void visit(ExprStmtAST& node) override {
        walk(node.expr.get());
    }

    void visit(DeclStmtAST& node) override {
        // Dispatch to whichever local declaration alternative is held.
        if (node.isVar())  walk(node.asVar());
        else if (node.isFunc()) walk(node.asFunc());
    }

    void visit(IfStmtAST& node) override {
        walk(node.condition.get());
        walk(node.thenBranch.get());
        if (node.elseBranch) walk(node.elseBranch.get());
    }

    void visit(SwitchStmtAST& node) override {
        walk(node.subject.get());
        for (auto& cas : node.cases) {
            for (auto& val : cas->values) walk(val.get());
            if (cas->body) walk(cas->body.get());
        }
        if (node.defaultBody) walk(node.defaultBody.get());
    }

    void visit(ForStmtAST& node) override {
        walk(node.iterable.get());
        if (node.step) walk(node.step.get());
        walk(node.body.get());
    }

    void visit(WhileStmtAST& node) override {
        walk(node.condition.get());
        walk(node.body.get());
    }

    void visit(DoWhileStmtAST& node) override {
        // Body first (loop semantics: body executes before condition is checked).
        walk(node.body.get());
        walk(node.condition.get());
    }

    void visit(ReturnStmtAST& node) override {
        if (node.value) walk(node.value.get());
    }

    void visit(BreakStmtAST& /*node*/) override {
        // No child nodes to walk.
    }

    void visit(ContinueStmtAST& /*node*/) override {
        // No child nodes to walk.
    }

    void visit(ParallelForStmtAST& node) override {
        walk(node.iterable.get());
        if (node.step) walk(node.step.get());
        walk(node.body.get());
    }

    void visit(ParallelBlockStmtAST& node) override {
        for (auto& sub : node.subBlocks) walk(sub.get());
    }

    // ── Root ──────────────────────────────────────────────────────────────────

    void visit(ProgramAST& node) override {
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
void annotateAll(std::vector<ProgramAST*>& files, SymbolTable& symbols) {
    Annotator annotator(symbols);
    for (auto* prog : files) {
        if (prog) annotator.annotateProgram(*prog);
    }
}