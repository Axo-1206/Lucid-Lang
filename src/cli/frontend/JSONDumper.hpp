/// @file cli/frontend/JSONDumper.hpp
/// @brief Complete JSON serialization for all AST nodes and diagnostics.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/memory/StringPool.hpp"

#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <unordered_map>

namespace cli {
namespace frontend {

/**
 * @brief Complete JSON serializer for all AST node types.
 * 
 * This class produces a complete, machine-readable JSON representation
 * of the entire AST, including:
 *   - All declaration types (Var, Func, Struct, Enum, Trait, etc.)
 *   - All statement types (Block, If, Switch, For, While, etc.)
 *   - All expression types (Literal, Binary, Call, Pipeline, etc.)
 *   - All type annotations (Primitive, Named, Array, Func, etc.)
 *   - Source locations with file paths
 *   - Diagnostic messages
 * 
 * The output format is designed for:
 *   - LSP integration (complete AST for analysis)
 *   - Tooling (AST inspection, code generation)
 *   - Debugging (human-readable with --json-pretty)
 *   - CI/CD pipelines (structured validation)
 */
class JSONDumper {
public:
    /**
     * @brief Construct a JSON dumper with a string pool.
     * 
     * @param pool The StringPool used to resolve InternedString IDs.
     * @param modules The modules being serialized (for file path lookup).
     * @param pretty Whether to pretty-print with indentation.
     */
    explicit JSONDumper(StringPool& pool, 
                        const std::vector<ModuleAST*>& modules,
                        bool pretty = false);
    ~JSONDumper() = default;

    // ─── Public API ──────────────────────────────────────────────────────

    /**
     * @brief Dump modules and diagnostics to a JSON string.
     * 
     * @param diagnostics The diagnostics to include.
     * @return std::string The JSON representation.
     */
    std::string dump(const DiagnosticEngine& diagnostics);

    /**
     * @brief Dump modules and diagnostics to a file.
     * 
     * @param diagnostics The diagnostics to include.
     * @param filePath The output file path.
     * @return bool True on success, false on failure.
     */
    bool dumpToFile(const DiagnosticEngine& diagnostics,
                    const std::string& filePath);

private:
    // ─── Forward declarations for circular references ───────────────────
    // These are needed because serializeDecl calls serializeType which
    // may call serializeDecl again (for function types with params).

    std::string serializeNode(BaseAST* node);
    std::string serializeDecl(DeclAST* decl);
    std::string serializeStmt(StmtAST* stmt);
    std::string serializeExpr(ExprAST* expr);
    std::string serializeType(TypeAST* type);

    // ─── Module Serialization ──────────────────────────────────────────

    std::string serializeModules();
    std::string serializeModule(ModuleAST* module);

    // ─── Declaration Serializers ──────────────────────────────────────

    std::string serializeImportDecl(ImportDeclAST* decl);
    std::string serializeVarDecl(VarDeclAST* decl);
    std::string serializeParam(ParamAST* param);
    std::string serializeFuncDecl(FuncDeclAST* decl);
    std::string serializeStructDecl(StructDeclAST* decl);
    std::string serializeEnumDecl(EnumDeclAST* decl);
    std::string serializeTraitDecl(TraitDeclAST* decl);
    std::string serializeFieldDecl(FieldDeclAST* field);
    std::string serializeTraitFieldDecl(TraitFieldDeclAST* field);
    std::string serializeEnumVariant(EnumVariantAST* variant);
    std::string serializeGenericParam(GenericParamDeclAST* param);

    // ─── Statement Serializers ────────────────────────────────────────

    std::string serializeBlockStmt(BlockStmtAST* stmt);
    std::string serializeExprStmt(ExprStmtAST* stmt);
    std::string serializeDeclStmt(DeclStmtAST* stmt);
    std::string serializeIfStmt(IfStmtAST* stmt);
    std::string serializeSwitchStmt(SwitchStmtAST* stmt);
    std::string serializeSwitchCase(SwitchCaseAST* case_);
    std::string serializeForStmt(ForStmtAST* stmt);
    std::string serializeWhileStmt(WhileStmtAST* stmt);
    std::string serializeDoWhileStmt(DoWhileStmtAST* stmt);
    std::string serializeReturnStmt(ReturnStmtAST* stmt);
    std::string serializeBreakStmt(BreakStmtAST* stmt);
    std::string serializeContinueStmt(ContinueStmtAST* stmt);
    std::string serializeFuncRefStmt(FuncRefStmtAST* stmt);
    std::string serializeAsyncStmt(AsyncStmtAST* stmt);
    std::string serializeAwaitStmt(AwaitStmtAST* stmt);
    std::string serializeSpawnStmt(SpawnStmtAST* stmt);
    std::string serializeJoinStmt(JoinStmtAST* stmt);

    // ─── Expression Serializers ──────────────────────────────────────

    std::string serializeLiteralExpr(LiteralExprAST* expr);
    std::string serializeIdentifierExpr(IdentifierExprAST* expr);
    std::string serializeArrayLiteralExpr(ArrayLiteralExprAST* expr);
    std::string serializeStructLiteralExpr(StructLiteralExprAST* expr);
    std::string serializeFieldInit(FieldInitAST* init);
    std::string serializeBinaryExpr(BinaryExprAST* expr);
    std::string serializeUnaryExpr(UnaryExprAST* expr);
    std::string serializeCallExpr(CallExprAST* expr);
    std::string serializeIntrinsicCallExpr(IntrinsicCallExprAST* expr);
    std::string serializeIndexExpr(IndexExprAST* expr);
    std::string serializeSliceExpr(SliceExprAST* expr);
    std::string serializeFieldAccessExpr(FieldAccessExprAST* expr);
    std::string serializeModuleAccessExpr(ModuleAccessExprAST* expr);
    std::string serializeAssignExpr(AssignExprAST* expr);
    std::string serializeNullCoalesceExpr(NullCoalesceExprAST* expr);
    std::string serializePipelineExpr(PipelineExprAST* expr);
    std::string serializePipelineStep(PipelineStepAST* step);
    std::string serializeComposeExpr(ComposeExprAST* expr);
    std::string serializeComposeOperand(ComposeOperandAST* operand);
    std::string serializeAnonFuncExpr(AnonFuncExprAST* expr);
    std::string serializeIfExpr(IfExprAST* expr);
    std::string serializeRangeExpr(RangeExprAST* expr);

    // ─── Type Serializers ─────────────────────────────────────────────

    std::string serializePrimitiveType(PrimitiveTypeAST* type);
    std::string serializeNamedType(NamedTypeAST* type);
    std::string serializeArrayType(ArrayTypeAST* type);
    std::string serializeNullableType(NullableTypeAST* type);
    std::string serializeFallibleType(FallibleTypeAST* type);
    std::string serializeCombinedType(CombinedTypeAST* type);
    std::string serializeRefType(RefTypeAST* type);
    std::string serializePtrType(PtrTypeAST* type);
    std::string serializeFuncType(FuncTypeAST* type);
    std::string serializeModuleTypeAccess(ModuleTypeAccessAST* type);
    std::string serializeFutureType(FutureTypeAST* type);
    std::string serializeThreadType(ThreadTypeAST* type);

    // ─── Diagnostics Serialization ──────────────────────────────────

    std::string serializeDiagnostics(const DiagnosticEngine& diagnostics);
    std::string serializeLocation(const SourceLocation& loc, ModuleAST* module = nullptr);

    // ─── Helper Methods ──────────────────────────────────────────────

    std::string getModulePath(InternedString filePath) const;
    std::string escapeString(const std::string& str) const;
    std::string str(InternedString s) const;
    std::string indent(int level) const;
    std::string quote(const std::string& str) const;
    std::string jsonBool(bool value) const;
    std::string jsonNull() const;
    std::string jsonNumber(int64_t value) const;
    std::string jsonNumber(uint64_t value) const;
    std::string jsonNumber(double value) const;
    
    // Kind to string helpers
    std::string kindToString(ASTKind kind) const;
    std::string literalKindToString(LiteralKind kind) const;
    std::string binaryOpToString(BinaryOp op) const;
    std::string unaryOpToString(UnaryOp op) const;
    std::string assignOpToString(AssignOp op) const;
    std::string primitiveKindToString(PrimitiveKind kind) const;
    std::string arrayKindToString(ArrayKind kind) const;
    std::string declKeywordToString(DeclKeyword keyword) const;
    std::string valueStateToString(ValueState state) const;
    
    StringPool& pool;
    const std::vector<ModuleAST*>& modules;
    bool pretty;
    int indentLevel = 0;
    
    // Cache for module file path lookups
    std::unordered_map<InternedString, ModuleAST*> moduleMap;
};

} // namespace frontend
} // namespace cli