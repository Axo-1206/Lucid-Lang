#pragma once

#include "ast/BaseAST.hpp" 
#include "ast/support/StringPool.hpp"
#include <string>

namespace LucDebug {

class ASTDumper : public ASTVisitor {
    int verbosity;
    int indentLevel;
    std::string out; // Build the string here
    const StringPool* pool;

    void indent();
    void printHeader(const BaseAST& node, const std::string& nodeName);
    void visitChild(BaseAST* child, const std::string& label = "");
    std::string formatType(struct TypeAST* type);

private:
    void printLine(const std::string& text);
    void printKV(const std::string& key, const std::string& value);
    void printNodeHeader(const BaseAST& node, const std::string& nodeName);
    
    // Cache for frequently used strings
    std::string indentCache[16]; // Cache indentation strings up to level 15
    void rebuildIndentCache();
    
public:
    ASTDumper(int verbosity, const StringPool& pool);
    
    // Get the resulting string
    std::string getOutput() const { return out; }

    // ── Type nodes ────────────────────────────────────────────────────────────
    void visit(PrimitiveTypeAST& node) override;
    void visit(NamedTypeAST& node) override;
    void visit(NullableTypeAST& node) override;
    void visit(FixedArrayTypeAST& node) override;
    void visit(SliceTypeAST& node) override;
    void visit(DynamicArrayTypeAST& node) override;
    void visit(RefTypeAST& node) override;
    void visit(PtrTypeAST& node) override;
    void visit(FuncTypeAST& node) override;

    // ── Declaration nodes ─────────────────────────────────────────────────────
    void visit(PackageDeclAST& node) override;
    void visit(UseDeclAST& node) override;
    void visit(VarDeclAST& node) override;
    void visit(FuncDeclAST& node) override;
    void visit(StructDeclAST& node) override;
    void visit(FieldDeclAST& node) override;
    void visit(EnumDeclAST& node) override;
    void visit(EnumVariantAST& node) override;
    void visit(TraitMethodAST& node) override;
    void visit(TraitDeclAST& node) override;
    void visit(ImplDeclAST& node) override;
    void visit(MethodDeclAST& node) override;
    void visit(FromDeclAST& node) override;
    void visit(FromEntryAST& node) override;
    void visit(TypeAliasDeclAST& node) override;
    void visit(GenericParamAST& node) override;
    void visit(ParamAST& node) override;

    // ── Expression nodes ──────────────────────────────────────────────────────
    void visit(LiteralExprAST& node) override;
    void visit(IdentifierExprAST& node) override;
    void visit(ArrayLiteralExprAST& node) override;
    void visit(StructLiteralExprAST& node) override;
    void visit(BinaryExprAST& node) override;
    void visit(UnaryExprAST& node) override;
    void visit(CallExprAST& node) override;
    void visit(IndexExprAST& node) override;
    void visit(FieldAccessExprAST& node) override;
    void visit(BehaviorAccessExprAST& node) override;
    void visit(NullableChainExprAST& node) override;
    void visit(NullCoalesceExprAST& node) override;
    void visit(AssignExprAST& node) override;
    void visit(IsExprAST& node) override;
    void visit(PipelineExprAST& node) override;
    void visit(PipelineStepAST& node) override;
    void visit(ComposeExprAST& node) override;
    void visit(ComposeOperandAST& node) override;
    void visit(AnonFuncExprAST& node) override;
    void visit(AwaitExprAST& node) override;
    void visit(MatchExprAST& node) override;
    void visit(IfExprAST& node) override;
    void visit(RangeExprAST& node) override;
    void visit(TypeConvExprAST& node) override;

    // ── Pattern nodes ─────────────────────────────────────────────────────────
    void visit(BindPatternAST& node) override;
    void visit(WildcardPatternAST& node) override;
    void visit(TypePatternAST& node) override;
    void visit(StructPatternAST& node) override;
    void visit(PatternExprAST& node) override;
    void visit(MatchArmAST& node) override;
    void visit(DefaultArmAST& node) override;

    // ── Statement nodes ───────────────────────────────────────────────────────
    void visit(BlockStmtAST& node) override;
    void visit(ExprStmtAST& node) override;
    void visit(DeclStmtAST& node) override;
    void visit(IfStmtAST& node) override;
    void visit(SwitchStmtAST& node) override;
    void visit(ForStmtAST& node) override;
    void visit(WhileStmtAST& node) override;
    void visit(DoWhileStmtAST& node) override;
    void visit(ReturnStmtAST& node) override;
    void visit(BreakStmtAST& node) override;
    void visit(ContinueStmtAST& node) override;

    // ── Unknown / Recovery nodes ──────────────────────────────────────────────
    void visit(UnknownDeclAST&) override;
    void visit(UnknownExprAST&) override;
    void visit(UnknownStmtAST&) override;
    void visit(UnknownTypeAST&) override;

    // ── Root ──────────────────────────────────────────────────────────────────
    void visit(ProgramAST& node) override;

    // ── Compiler Directive nodes (@) ──────────────────────────────────────────
    void visit(AttributeAST& node) override;
    void visit(IntrinsicCallExprAST& node) override;
};

} // namespace LucDebug
