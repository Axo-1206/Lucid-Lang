/// @file cli/pipeline/JSONDumper.hpp
/// @brief Complete JSON serialization for the redesigned AST.

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
#include <unordered_map>
#include <sstream>
#include <iomanip>

namespace lucid::cli::pipeline {

// ─── JSONWriter ─────────────────────────────────────────────────────────
// Unchanged from before — kept verbatim so the rest of the file compiles.
// (See the original for the full definition.)

class JSONWriter {
public:
    explicit JSONWriter(bool pretty = false) : m_pretty(pretty) {}

    std::string str() const {
        std::string raw = m_oss.str();
        if (m_pretty) return JSONFormatter::format(raw);
        return raw;
    }

    void beginObject() { writeCommaIfNeeded(); m_oss << "{"; m_needComma = false; }
    void endObject()   { m_oss << "}"; m_needComma = true; }
    void beginArray()  { writeCommaIfNeeded(); m_oss << "["; m_needComma = false; }
    void endArray()    { m_oss << "]"; m_needComma = true; }

    void key(const std::string& k) {
        writeCommaIfNeeded();
        m_oss << "\"" << escape(k) << "\": ";
        m_needComma = false;
    }

    void null()           { writeCommaIfNeeded(); m_oss << "null";  m_needComma = true; }
    void bool_(bool v)    { writeCommaIfNeeded(); m_oss << (v ? "true" : "false"); m_needComma = true; }
    void number(int64_t v){ writeCommaIfNeeded(); m_oss << v;       m_needComma = true; }
    void number(uint64_t v){writeCommaIfNeeded(); m_oss << v;       m_needComma = true; }
    void number(double v) { writeCommaIfNeeded(); m_oss << std::setprecision(17) << v; m_needComma = true; }

    void string(const std::string& v) {
        writeCommaIfNeeded();
        m_oss << "\"" << escape(v) << "\"";
        m_needComma = true;
    }
    void string(const char* v) { string(std::string(v)); }

    // Overload ordering is load-bearing: `const char*` must beat `bool`.
    void kv(const std::string& k, const char* v)        { key(k); string(v); }
    void kv(const std::string& k, const std::string& v) { key(k); string(v); }
    void kv(const std::string& k, bool v)               { key(k); bool_(v); }
    void kv(const std::string& k, int64_t v)            { key(k); number(v); }
    void kv(const std::string& k, uint64_t v)           { key(k); number(v); }
    void kv(const std::string& k, double v)             { key(k); number(v); }
    void kvNull(const std::string& k)                   { key(k); null(); }

    void arrayKey(const std::string& k) { key(k); beginArray(); }

private:
    void writeCommaIfNeeded() { if (m_needComma) m_oss << ","; }

    static std::string escape(const std::string& str) {
        std::ostringstream oss;
        for (char c : str) {
            switch (c) {
                case '"':  oss << "\\\""; break;
                case '\\': oss << "\\\\"; break;
                case '\b': oss << "\\b";  break;
                case '\f': oss << "\\f";  break;
                case '\n': oss << "\\n";  break;
                case '\r': oss << "\\r";  break;
                case '\t': oss << "\\t";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        oss << "\\u" << std::hex << std::setw(4)
                            << std::setfill('0') << static_cast<int>(c);
                    } else {
                        oss << c;
                    }
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
    // ─── Top level ────────────────────────────────────────────────────
    void serializeModules(JSONWriter& json);
    void serializeModule(JSONWriter& json, ModuleAST* module);
    void serializeDiagnostics(JSONWriter& json, const DiagnosticEngine& diagnostics);
    void serializeLocation(JSONWriter& json, const SourceLocation& loc);

    // ─── Shared ───────────────────────────────────────────────────────
    void serializeDeclRef(JSONWriter& json, DeclAST* decl);
    void serializeDocComment(JSONWriter& json, const DocComment& doc);

    // ─── Declarations ─────────────────────────────────────────────────
    void serializeDecl(JSONWriter& json, DeclAST* decl);
    void serializeImportDecl(JSONWriter& json, ImportDeclAST* decl);
    void serializeTableDecl(JSONWriter& json, TableDeclAST* decl);
    void serializeColumnDecl(JSONWriter& json, ColumnDeclAST* decl);
    void serializeRow(JSONWriter& json, RowAST* row);
    void serializeFnDecl(JSONWriter& json, FnDeclAST* decl);
    void serializeVarDecl(JSONWriter& json, VarDeclAST* decl);
    void serializeParam(JSONWriter& json, ParamAST* param);

    // ─── Statements ───────────────────────────────────────────────────
    void serializeStmt(JSONWriter& json, StmtAST* stmt);
    void serializeBlockStmt(JSONWriter& json, BlockStmtAST* stmt);
    void serializeVarDeclStmt(JSONWriter& json, VarDeclStmtAST* stmt);
    void serializeAssignStmt(JSONWriter& json, AssignStmtAST* stmt);
    void serializeExprStmt(JSONWriter& json, ExprStmtAST* stmt);
    void serializeReturnStmt(JSONWriter& json, ReturnStmtAST* stmt);
    void serializeBreakStmt(JSONWriter& json, BreakStmtAST* stmt);
    void serializeContinueStmt(JSONWriter& json, ContinueStmtAST* stmt);
    void serializeIfStmt(JSONWriter& json, IfStmtAST* stmt);
    void serializeSwitchStmt(JSONWriter& json, SwitchStmtAST* stmt);
    void serializeSwitchCase(JSONWriter& json, SwitchCaseAST* case_);
    void serializeWhileStmt(JSONWriter& json, WhileStmtAST* stmt);
    void serializeForStmt(JSONWriter& json, ForStmtAST* stmt);
    void serializeWaitStmt(JSONWriter& json, WaitStmtAST* stmt);
    void serializeWaitFramesStmt(JSONWriter& json, WaitFramesStmtAST* stmt);
    void serializeWaitUntilStmt(JSONWriter& json, WaitUntilStmtAST* stmt);
    void serializeWaitForEventStmt(JSONWriter& json, WaitForEventStmtAST* stmt);
    void serializeWaitForRequestStmt(JSONWriter& json, WaitForRequestStmtAST* stmt);

    // ─── Expressions ──────────────────────────────────────────────────
    void serializeExpr(JSONWriter& json, ExprAST* expr);
    void serializeLiteralExpr(JSONWriter& json, LiteralExprAST* expr);
    void serializeIdentifierExpr(JSONWriter& json, IdentifierExprAST* expr);
    void serializeArrayLiteralExpr(JSONWriter& json, ArrayLiteralExprAST* expr);
    void serializeFieldAccessExpr(JSONWriter& json, FieldAccessExprAST* expr);
    void serializeIndexExpr(JSONWriter& json, IndexExprAST* expr);
    void serializeCallExpr(JSONWriter& json, CallExprAST* expr);
    void serializeLambdaExpr(JSONWriter& json, LambdaExprAST* expr);
    void serializeStartExpr(JSONWriter& json, StartExprAST* expr);
    void serializeUnaryExpr(JSONWriter& json, UnaryExprAST* expr);
    void serializeBinaryExpr(JSONWriter& json, BinaryExprAST* expr);
    void serializeParenExpr(JSONWriter& json, ParenExprAST* expr);
    void serializeRangeExpr(JSONWriter& json, RangeExprAST* expr);

    // ─── Types ────────────────────────────────────────────────────────
    void serializeType(JSONWriter& json, TypeAST* type);
    void serializePrimitiveType(JSONWriter& json, PrimitiveTypeAST* type);
    void serializeNamedType(JSONWriter& json, NamedTypeAST* type);
    void serializeArrayType(JSONWriter& json, ArrayTypeAST* type);
    void serializeRowRefType(JSONWriter& json, RowRefTypeAST* type);
    void serializeFunctionType(JSONWriter& json, FunctionTypeAST* type);
    void serializeNullableType(JSONWriter& json, NullableTypeAST* type);

    // ─── Helpers ──────────────────────────────────────────────────────
    std::string str(InternedString s) const;
    std::string getModulePath(InternedString filePath) const;

    StringPool& pool;
    const std::vector<ModuleAST*>& modules;
    bool pretty;
    std::unordered_map<InternedString, ModuleAST*> moduleMap;
};

} // namespace lucid::cli::pipeline