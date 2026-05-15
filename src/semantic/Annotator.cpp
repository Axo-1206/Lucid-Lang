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
    explicit Annotator(SymbolTable& symbols) : symbols_(symbols) {
        LUC_LOG_SEMANTIC_VERBOSE("Annotator constructed");
    }

    // Entry point — annotate every top-level declaration in one program file.
    void annotateProgram(ProgramAST& prog) {
        LUC_LOG_SEMANTIC_EXTREME("annotateProgram: processing file: " << prog.filePath);
        for (auto& decl : prog.decls) {
            if (decl) decl->accept(*this);
        }
    }

private:
    SymbolTable& symbols_;
    
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
        // No semantic annotations needed on package declarations.
        LUC_LOG_SEMANTIC_EXTREME("visit(PackageDeclAST)");
    }

    void visit(UseDeclAST& /*node*/) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(UseDeclAST)");
    }

    void visit(VarDeclAST& node) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(VarDeclAST): name=" << node.name);
        
        // Walk attributes and initialiser (post-order).
        for (auto& attr : node.attributes) walk(attr.get());
        if (node.init) walk(node.init.get());

        // 'const' variables are compile-time constants.
        //   const MAX int   = 65536   → MAX.isConst = true
        //   let   x   int   = 0       → x.isConst   = false
        node.isConst = (node.keyword == DeclKeyword::Const);
        logConst("VarDecl", node.name, node.isConst);
    }

    void visit(GenericParamAST& /*node*/) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(GenericParamAST)");
        // Generic params are compile-time, but they are not runtime values.
    }

    void visit(FuncDeclAST& node) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(FuncDeclAST): name=" << node.name 
                            << ", keyword=" << (node.keyword == DeclKeyword::Const ? "const" : "let"));
        
        // Walk attributes, and body (parameters are in node.type)
        for (auto& attr : node.attributes) walk(attr.get());
        
        // Walk parameter groups from the unified FuncTypeAST
        for (auto& group : node.type.paramGroups) {
            for (auto& param : group) {
                // ParamInfo is not a BaseAST, so no walk needed
                // Just log or skip - no AST to traverse
            }
        }
        walk(node.body.get());

        // 'const' functions are permanently bound (not reassignable at call sites).
        node.isConst = (node.keyword == DeclKeyword::Const);
        logConst("FuncDecl", node.name, node.isConst);
    }

    void visit(StructDeclAST& node) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(StructDeclAST): name=" << node.name);
        
        for (auto& attr : node.attributes) walk(attr.get());
        for (auto& field : node.fields)
            walk(field.get());
        // Struct type declarations themselves are not runtime values.
        // No isConst flag needed.
    }

    void visit(FieldDeclAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(FieldDeclAST): name=" << node.name);
        // Annotate the default value expression if present.
        if (node.defaultVal) walk(node.defaultVal.get());
    }

    void visit(EnumDeclAST& node) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(EnumDeclAST): name=" << node.name);
        
        for (auto& variant : node.variants)
            walk(variant.get());
        // Enum declarations represent a closed set of compile-time integers.
        node.isConst = true;
        logConst("EnumDecl", node.name, node.isConst);
    }

    void visit(EnumVariantAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(EnumVariantAST): name=" << node.name);
        // Every enum variant is a named integer constant.
        node.isConst = true;
    }

    void visit(TraitMethodAST& /*node*/) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(TraitMethodAST)");
        // Trait method signatures have no body — nothing to annotate.
    }

    void visit(TraitDeclAST& /*node*/) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(TraitDeclAST)");
        // Trait declarations are structural contracts, not runtime values.
    }

    void visit(ImplDeclAST& node) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(ImplDeclAST): structName=" << node.structName);
        
        for (auto& method : node.methods)
            walk(method.get());
    }

    void visit(MethodDeclAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(MethodDeclAST): name=" << node.name);
        
        // Parameters are in node.type (unified FuncTypeAST)
        // ParamInfo is not a BaseAST, so no walk needed
        
        walk(node.body.get());
        // Methods are never compile-time constants; they dispatch at runtime.
        node.isConst = false;
    }

    void visit(FromDeclAST& node) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(FromDeclAST): targetType=" << node.targetTypeName);
        
        for (auto& entry : node.entries)
            walk(entry.get());
        // Casting blocks themselves are not constants.
        node.isConst = false;
    }

    void visit(FromEntryAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(FromEntryAST)");
        
        // paramGroups now contains ParamInfo (not BaseAST), so no walk needed
        // for the parameters themselves - only walk the body
        walk(node.body.get());
        // Individual casting entries are not constants.
        node.isConst = false;
    }

    void visit(TypeAliasDeclAST& /*node*/) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(TypeAliasDeclAST)");
        // Type alias declarations are purely compile-time name mappings.
    }

    // ── Expression nodes ──────────────────────────────────────────────────────

    void visit(LiteralExprAST& node) override {
        // Every literal except nil is a compile-time scalar constant.
        // nil has no concrete type, so marking it const could mislead codegen
        // into thinking a nullable slot has a permanent non-null value.
        node.isConst = (node.kind != LiteralKind::Nil);
        LUC_LOG_SEMANTIC_EXTREME("visit(LiteralExprAST): value='" << node.value 
                               << "', isConst=" << (node.isConst ? "true" : "false"));
    }

    void visit(IdentifierExprAST& node) override {
        // Propagate isConst from the symbol's declaration keyword.
        // isBehaviorMember was already set by checkExpr in Phase 3.
        Symbol* sym = symbols_.lookup(node.name);
        if (sym) {
            node.isConst = (sym->declKw == DeclKeyword::Const);
            LUC_LOG_SEMANTIC_EXTREME("visit(IdentifierExprAST): name=" << node.name 
                                   << ", isConst=" << (node.isConst ? "true" : "false"));
        } else {
            LUC_LOG_SEMANTIC_EXTREME("visit(IdentifierExprAST): name=" << node.name << " (symbol not found)");
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
        // An array literal is const only when every element is const.
        // An empty array literal is treated as non-const (runtime-sized).
        node.isConst = allConst;
        LUC_LOG_SEMANTIC_EXTREME("\telements=" << node.elements.size() 
                               << ", allConst=" << (allConst ? "true" : "false"));
    }

    void visit(StructLiteralExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(StructLiteralExprAST): type=" << node.typeName);
        
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
        LUC_LOG_SEMANTIC_EXTREME("\tinits=" << node.inits.size() 
                               << ", allConst=" << (allConst ? "true" : "false"));
    }

    void visit(FieldAccessExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(FieldAccessExprAST)");
        // Walk the object first (post-order).
        walk(node.object.get());
        // Field access is const when the object itself is const.
        // This covers the enum variant case: Direction.North — Direction is
        // an enum (isConst=true) so North access is also const.
        node.isConst = node.object && node.object->isConst;
    }

    void visit(BehaviorAccessExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(BehaviorAccessExprAST): " << node.typeName << ":" << node.method);
        // Reinforce isBehaviorMember (set by Phase 3 checkExpr).
        // Behavior (impl method) references are not runtime constants.
        node.isBehaviorMember = true;
        node.isConst = false;
    }

    void visit(BinaryExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(BinaryExprAST)");
        // Walk both operands first (post-order).
        walk(node.left.get());
        walk(node.right.get());
        // A binary expression is a compile-time constant when both operands are.
        bool lc = node.left  && node.left->isConst;
        bool rc = node.right && node.right->isConst;
        node.isConst = lc && rc;
        LUC_LOG_SEMANTIC_EXTREME("\tleftConst=" << lc << ", rightConst=" << rc 
                               << ", result=" << (node.isConst ? "true" : "false"));
    }

    void visit(UnaryExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(UnaryExprAST)");
        walk(node.operand.get());
        // Propagate constness from the single operand.
        node.isConst = node.operand && node.operand->isConst;
    }

    void visit(AssignExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(AssignExprAST)");
        walk(node.lhs.get());
        walk(node.rhs.get());
        // Assignments are side-effecting — never const.
        node.isConst = false;
    }

    void visit(CallExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(CallExprAST)");
        walk(node.callee.get());
        for (auto& arg : node.args) walk(arg.get());
        // Function calls may have side effects — treat as non-const.
        node.isConst = false;
    }

    void visit(IndexExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(IndexExprAST)");
        walk(node.target.get());
        walk(node.index.get());
        if (node.sliceEnd) walk(node.sliceEnd.get());
        // Index accesses involve runtime memory — not compile-time const.
        node.isConst = false;
    }

    void visit(IsExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(IsExprAST)");
        walk(node.expr.get());
        // 'x is int' is a runtime type check — not compile-time const.
        node.isConst = false;
    }

    void visit(NullableChainExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(NullableChainExprAST)");
        walk(node.object.get());
        if (node.fallback) walk(node.fallback.get());
        // Nullable chains involve runtime null checks — not const.
        node.isConst = false;
    }

    void visit(PipelineExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(PipelineExprAST)");
        walk(node.seed.get());
        // Pipeline steps may call functions — not compile-time const.
        node.isConst = false;
    }

    void visit(ComposeExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(ComposeExprAST)");
        walk(node.left.get());
        // Composed functions are higher-order runtime values — not const.
        node.isConst = false;
    }

    void visit(AnonFuncExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(AnonFuncExprAST)");
        
        // Parameters are in node.type.paramGroups (ParamInfo, not BaseAST)
        // No walking needed for ParamInfo - only walk the body
        walk(node.body.get());
        // Anonymous function expressions are runtime closures — not const.
        node.isConst = false;
    }

    void visit(AwaitExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(AwaitExprAST)");
        walk(node.inner.get());
        // async/await always involves runtime scheduling — never const.
        node.isConst = false;
    }

    void visit(MatchExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(MatchExprAST)");
        walk(node.subject.get());
        for (auto& arm : node.arms)     walk(arm.get());
        if (node.defaultBody)           walk(node.defaultBody.get());
        // match expressions dispatch at runtime — not compile-time const.
        node.isConst = false;
    }

    void visit(IfExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(IfExprAST)");
        walk(node.condition.get());
        if (node.thenBranch) walk(node.thenBranch.get());
        if (node.elseBranch) walk(node.elseBranch.get());
        // if-expr evaluates a runtime condition — not compile-time const.
        node.isConst = false;
    }

    void visit(RangeExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(RangeExprAST)");
        walk(node.lo.get());
        walk(node.hi.get());
        // A range is const when both bounds are constant literals (e.g. 0..10).
        bool lc = node.lo && node.lo->isConst;
        bool hc = node.hi && node.hi->isConst;
        node.isConst = lc && hc;
    }

    void visit(TypeConvExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(TypeConvExprAST): isUnsafe=" << (node.isUnsafe ? "true" : "false"));
        walk(node.expr.get());
        // A safe type casting of a const expression is itself const.
        // Unsafe (*T) conversions reference raw memory — not const.
        node.isConst = !node.isUnsafe && node.expr && node.expr->isConst;
    }

    void visit(AttributeAST& /*node*/) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(AttributeAST)");
        // Attributes are compile-time metadata — they carry no runtime value.
        // No isConst propagation needed; args are literals handled at parse time.
    }

    void visit(IntrinsicCallExprAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(IntrinsicCallExprAST): name=" << node.intrinsicName);
        // Walk value arguments (typeArg is a TypeAST, not walked here).
        for (auto& arg : node.args) walk(arg.get());

        // @sizeof and @alignof are compile-time constants (pure type queries).
        // All other intrinsics involve runtime computation — not const.
        bool isCompileTimeType = (node.intrinsicName == "sizeof" ||
                                  node.intrinsicName == "alignof");
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
        LUC_LOG_SEMANTIC_EXTREME("visit(StructPatternAST): type=" << node.typeName);
        for (auto& fp : node.fields) {
            if (fp && fp->subPattern) walk(fp->subPattern.get());
        }
    }

    void visit(MatchArmAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(MatchArmAST)");
        for (auto& pat : node.patterns) walk(pat.get());
        if (node.guard) walk(node.guard.get());
        for (auto& expr : node.exprs)   walk(expr.get());
    }

    void visit(DefaultArmAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(DefaultArmAST)");
        for (auto& expr : node.exprs) walk(expr.get());
    }

    // ── Statement nodes ───────────────────────────────────────────────────────

    void visit(BlockStmtAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(BlockStmtAST): " << node.stmts.size() << " statements");
        // scopeDepth was already written by checkBlock in SemanticStmt.cpp.
        for (auto& stmt : node.stmts) walk(stmt.get());
    }

    void visit(ExprStmtAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(ExprStmtAST)");
        walk(node.expr.get());
    }

    void visit(DeclStmtAST& node) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(DeclStmtAST)");
        if (node.isVar())  walk(node.asVar());
        else if (node.isFunc()) walk(node.asFunc());
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
        LUC_LOG_SEMANTIC_EXTREME("visit(ForStmtAST): var=" << node.varName);
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
        for (auto& val : node.values) {
            walk(val.get());
        }
    }

    void visit(BreakStmtAST& /*node*/) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(BreakStmtAST)");
    }

    void visit(ContinueStmtAST& /*node*/) override {
        LUC_LOG_SEMANTIC_EXTREME("visit(ContinueStmtAST)");
    }

    // ── Root ──────────────────────────────────────────────────────────────────

    void visit(ProgramAST& node) override {
        LUC_LOG_SEMANTIC_VERBOSE("visit(ProgramAST): file=" << node.filePath);
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
    LUC_LOG_SEMANTIC_VERBOSE("annotateAll: starting annotation pass on " << files.size() << " files");
    
    Annotator annotator(symbols);
    for (auto* prog : files) {
        if (prog) {
            LUC_LOG_SEMANTIC_EXTREME("\tannotating file: " << prog->filePath);
            annotator.annotateProgram(*prog);
        }
    }
    
    LUC_LOG_SEMANTIC_VERBOSE("annotateAll: annotation pass complete");
}