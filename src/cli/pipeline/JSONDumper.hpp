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
#include "core/ASTStrings.hpp"
#include "core/JSONFormatter.hpp"

#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <sstream>

namespace cli {
namespace frontend {

// ─── JSON Writer ─────────────────────────────────────────────────────────

/**
 * @brief Simple JSON builder with manual comma control.
 * 
 * This class builds a raw JSON string without formatting.
 * Formatting is applied by JSONFormatter when str() is called.
 * 
 * Usage:
 *   JSONWriter json;
 *   json.beginObject();              // {
 *   json.key("name");                // "name":
 *   json.string("test");             // "test"
 *   json.key("age");                 // "age":
 *   json.number(42);                 // 42
 *   json.endObject();                // }
 *   std::string result = json.str(); // {"name":"test","age":42}
 */
class JSONWriter {
public:
    explicit JSONWriter(bool pretty = false) : m_pretty(pretty) {}

    std::string str() const {
        std::string raw = m_oss.str();
        if (m_pretty) {
            return JSONFormatter::format(raw);
        }
        return raw;
    }

    // ─── Object ──────────────────────────────────────────────────────────

    void beginObject() {
        writeCommaIfNeeded();
        m_oss << "{";
        m_needComma = false;
    }

    void endObject() {
        m_oss << "}";
        m_needComma = true;
    }

    // ─── Array ───────────────────────────────────────────────────────────

    void beginArray() {
        writeCommaIfNeeded();
        m_oss << "[";
        m_needComma = false;
    }

    void endArray() {
        m_oss << "]";
        m_needComma = true;
    }

    // ─── Key ────────────────────────────────────────────────────────────

    void key(const std::string& k) {
        writeCommaIfNeeded();
        m_oss << "\"" << escape(k) << "\": ";
        m_needComma = false;
    }

    // ─── Values ─────────────────────────────────────────────────────────

    void null() {
        writeCommaIfNeeded();
        m_oss << "null";
        m_needComma = true;
    }

    void bool_(bool v) {
        writeCommaIfNeeded();
        m_oss << (v ? "true" : "false");
        m_needComma = true;
    }

    void number(int64_t v) {
        writeCommaIfNeeded();
        m_oss << v;
        m_needComma = true;
    }

    void number(uint64_t v) {
        writeCommaIfNeeded();
        m_oss << v;
        m_needComma = true;
    }

    void number(double v) {
        writeCommaIfNeeded();
        m_oss << std::setprecision(17) << v;
        m_needComma = true;
    }

    void string(const std::string& v) {
        writeCommaIfNeeded();
        m_oss << "\"" << escape(v) << "\"";
        m_needComma = true;
    }

    void string(const char* v) {
        string(std::string(v));
    }

    // ─── Key + Value Convenience ───────────────────────────────────────

    // IMPORTANT: this overload must exist. Without it, calls like
    // json.kv("kind", "FuncDecl") pass a `const char*` for `v`, and that
    // char* is an EXACT match for kv(const std::string&, bool) via the
    // standard pointer-to-bool conversion, but only a USER-DEFINED
    // conversion away from kv(const std::string&, const std::string&).
    // Overload resolution prefers standard conversions over user-defined
    // ones, so every such call silently picked the bool overload and
    // serialized "kind": true instead of the actual AST node name.
    void kv(const std::string& k, const char* v) {
        key(k);
        string(v);
    }

    void kv(const std::string& k, const std::string& v) {
        key(k);
        string(v);
    }

    void kv(const std::string& k, bool v) {
        key(k);
        bool_(v);
    }

    void kv(const std::string& k, int64_t v) {
        key(k);
        number(v);
    }

    void kv(const std::string& k, uint64_t v) {
        key(k);
        number(v);
    }

    void kv(const std::string& k, double v) {
        key(k);
        number(v);
    }

    void kvNull(const std::string& k) {
        key(k);
        null();
    }

    // ─── Array Key Convenience ─────────────────────────────────────────

    void arrayKey(const std::string& k) {
        key(k);
        beginArray();
    }

private:
    void writeCommaIfNeeded() {
        if (m_needComma) {
            m_oss << ",";
        }
    }

    static std::string escape(const std::string& str) {
        std::ostringstream oss;
        for (char c : str) {
            switch (c) {
                case '"':  oss << "\\\""; break;
                case '\\': oss << "\\\\"; break;
                case '\b': oss << "\\b"; break;
                case '\f': oss << "\\f"; break;
                case '\n': oss << "\\n"; break;
                case '\r': oss << "\\r"; break;
                case '\t': oss << "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                            << static_cast<int>(c);
                    } else {
                        oss << c;
                    }
                    break;
            }
        }
        return oss.str();
    }

    std::ostringstream m_oss;
    bool m_pretty = false;
    bool m_needComma = false;
};

// ─── JSONDumper ─────────────────────────────────────────────────────────

class JSONDumper {
public:
    explicit JSONDumper(StringPool& pool, 
                        const std::vector<ModuleAST*>& modules,
                        bool pretty = false);
    ~JSONDumper() = default;

    std::string dump(const DiagnosticEngine& diagnostics);
    bool dumpToFile(const DiagnosticEngine& diagnostics,
                    const std::string& filePath);

private:
    // ─── Serialization Methods ────────────────────────────────────────

    void serializeModules(JSONWriter& json);
    void serializeModule(JSONWriter& json, ModuleAST* module);
    void serializeDecl(JSONWriter& json, DeclAST* decl);
    void serializeStmt(JSONWriter& json, StmtAST* stmt);
    void serializeExpr(JSONWriter& json, ExprAST* expr);
    void serializeType(JSONWriter& json, TypeAST* type);
    void serializeDiagnostics(JSONWriter& json, const DiagnosticEngine& diagnostics);
    void serializeLocation(JSONWriter& json, const SourceLocation& loc, ModuleAST* module = nullptr);

    // ─── Declaration Serializers ──────────────────────────────────────

    void serializeImportDecl(JSONWriter& json, ImportDeclAST* decl);
    void serializeVarDecl(JSONWriter& json, VarDeclAST* decl);
    void serializeParam(JSONWriter& json, ParamAST* param);
    void serializeFuncDecl(JSONWriter& json, FuncDeclAST* decl);
    void serializeStructDecl(JSONWriter& json, StructDeclAST* decl);
    void serializeEnumDecl(JSONWriter& json, EnumDeclAST* decl);
    void serializeTraitDecl(JSONWriter& json, TraitDeclAST* decl);
    void serializeFieldDecl(JSONWriter& json, FieldDeclAST* field);
    void serializeTraitFieldDecl(JSONWriter& json, TraitFieldDeclAST* field);
    void serializeEnumVariant(JSONWriter& json, EnumVariantAST* variant);
    void serializeGenericParam(JSONWriter& json, GenericParamDeclAST* param);

    // ─── Statement Serializers ────────────────────────────────────────

    void serializeBlockStmt(JSONWriter& json, BlockStmtAST* stmt);
    void serializeExprStmt(JSONWriter& json, ExprStmtAST* stmt);
    void serializeDeclStmt(JSONWriter& json, DeclStmtAST* stmt);
    void serializeIfStmt(JSONWriter& json, IfStmtAST* stmt);
    void serializeSwitchStmt(JSONWriter& json, SwitchStmtAST* stmt);
    void serializeSwitchCase(JSONWriter& json, SwitchCaseAST* case_);
    void serializeForStmt(JSONWriter& json, ForStmtAST* stmt);
    void serializeWhileStmt(JSONWriter& json, WhileStmtAST* stmt);
    void serializeDoWhileStmt(JSONWriter& json, DoWhileStmtAST* stmt);
    void serializeReturnStmt(JSONWriter& json, ReturnStmtAST* stmt);
    void serializeBreakStmt(JSONWriter& json, BreakStmtAST* stmt);
    void serializeContinueStmt(JSONWriter& json, ContinueStmtAST* stmt);
    void serializeFuncRefStmt(JSONWriter& json, FuncRefStmtAST* stmt);
    void serializeAsyncStmt(JSONWriter& json, AsyncStmtAST* stmt);
    void serializeAwaitStmt(JSONWriter& json, AwaitStmtAST* stmt);
    void serializeSpawnStmt(JSONWriter& json, SpawnStmtAST* stmt);
    void serializeJoinStmt(JSONWriter& json, JoinStmtAST* stmt);

    // ─── Expression Serializers ──────────────────────────────────────

    void serializeLiteralExpr(JSONWriter& json, LiteralExprAST* expr);
    void serializeIdentifierExpr(JSONWriter& json, IdentifierExprAST* expr);
    void serializeArrayLiteralExpr(JSONWriter& json, ArrayLiteralExprAST* expr);
    void serializeStructLiteralExpr(JSONWriter& json, StructLiteralExprAST* expr);
    void serializeFieldInit(JSONWriter& json, FieldInitAST* init);
    void serializeBinaryExpr(JSONWriter& json, BinaryExprAST* expr);
    void serializeUnaryExpr(JSONWriter& json, UnaryExprAST* expr);
    void serializeCallExpr(JSONWriter& json, CallExprAST* expr);
    void serializeIntrinsicCallExpr(JSONWriter& json, IntrinsicCallExprAST* expr);
    void serializeIndexExpr(JSONWriter& json, IndexExprAST* expr);
    void serializeSliceExpr(JSONWriter& json, SliceExprAST* expr);
    void serializeFieldAccessExpr(JSONWriter& json, FieldAccessExprAST* expr);
    void serializeModuleAccessExpr(JSONWriter& json, ModuleAccessExprAST* expr);
    void serializeAssignExpr(JSONWriter& json, AssignExprAST* expr);
    void serializeNullCoalesceExpr(JSONWriter& json, NullCoalesceExprAST* expr);
    void serializePipelineExpr(JSONWriter& json, PipelineExprAST* expr);
    void serializePipelineStep(JSONWriter& json, PipelineStepAST* step);
    void serializeComposeExpr(JSONWriter& json, ComposeExprAST* expr);
    void serializeComposeOperand(JSONWriter& json, ComposeOperandAST* operand);
    void serializeAnonFuncExpr(JSONWriter& json, AnonFuncExprAST* expr);
    void serializeIfExpr(JSONWriter& json, IfExprAST* expr);
    void serializeRangeExpr(JSONWriter& json, RangeExprAST* expr);

    // ─── Type Serializers ─────────────────────────────────────────────

    void serializePrimitiveType(JSONWriter& json, PrimitiveTypeAST* type);
    void serializeNamedType(JSONWriter& json, NamedTypeAST* type);
    void serializeModuleTypeAccess(JSONWriter& json, ModuleTypeAccessAST* type);
    void serializeArrayType(JSONWriter& json, ArrayTypeAST* type);
    void serializeNullableType(JSONWriter& json, NullableTypeAST* type);
    void serializeFallibleType(JSONWriter& json, FallibleTypeAST* type);
    void serializeCombinedType(JSONWriter& json, CombinedTypeAST* type);
    void serializeRefType(JSONWriter& json, RefTypeAST* type);
    void serializePtrType(JSONWriter& json, PtrTypeAST* type);
    void serializeFuncType(JSONWriter& json, FuncTypeAST* type);
    void serializeFutureType(JSONWriter& json, FutureTypeAST* type);
    void serializeThreadType(JSONWriter& json, ThreadTypeAST* type);

    // ─── Helpers ──────────────────────────────────────────────────────

    std::string str(InternedString s) const;
    std::string getModulePath(InternedString filePath) const;
    
    StringPool& pool;
    const std::vector<ModuleAST*>& modules;
    bool pretty;
    std::unordered_map<InternedString, ModuleAST*> moduleMap;
};

} // namespace frontend
} // namespace cli