/// @file cli/frontend/JSONDumper.cpp
/// @brief Implementation of complete JSON serialization.

#include "JSONDumper.hpp"

#include <sstream>
#include <iomanip>

namespace cli {
namespace frontend {

// ─── Constructor ─────────────────────────────────────────────────────────

JSONDumper::JSONDumper(StringPool& pool, 
                       const std::vector<ModuleAST*>& modules,
                       bool pretty)
    : pool(pool), modules(modules), pretty(pretty) {
    // Build module map for file path lookups
    for (auto* module : modules) {
        if (module) {
            moduleMap[module->filePath] = module;
        }
    }
}

// ─── Public API ──────────────────────────────────────────────────────────

std::string JSONDumper::dump(const DiagnosticEngine& diagnostics) {
    std::ostringstream oss;
    
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 1;
    
    // Modules
    oss << indent(1) << quote("modules") << ": " << serializeModules();
    oss << ",";
    if (pretty) oss << "\n";
    
    // Diagnostics
    oss << indent(1) << quote("diagnostics") << ": " << serializeDiagnostics(diagnostics);
    oss << "\n";
    
    indentLevel = 0;
    oss << "}";
    
    return oss.str();
}

bool JSONDumper::dumpToFile(const DiagnosticEngine& diagnostics,
                             const std::string& filePath) {
    std::ofstream file(filePath);
    if (!file.is_open()) {
        return false;
    }
    
    file << dump(diagnostics);
    return true;
}

// ─── Helper: Kind to String ────────────────────────────────────────────

std::string JSONDumper::kindToString(ASTKind kind) const {
    switch (kind) {
        // Declarations
        case ASTKind::ImportDecl:    return "ImportDecl";
        case ASTKind::VarDecl:       return "VarDecl";
        case ASTKind::Param:         return "Param";
        case ASTKind::GenericParamDecl: return "GenericParamDecl";
        case ASTKind::FuncDecl:      return "FuncDecl";
        case ASTKind::FieldDecl:     return "FieldDecl";
        case ASTKind::StructDecl:    return "StructDecl";
        case ASTKind::EnumVariant:   return "EnumVariant";
        case ASTKind::EnumDecl:      return "EnumDecl";
        case ASTKind::TraitFieldDecl: return "TraitFieldDecl";
        case ASTKind::TraitDecl:     return "TraitDecl";
        
        // Statements
        case ASTKind::BlockStmt:     return "BlockStmt";
        case ASTKind::ExprStmt:      return "ExprStmt";
        case ASTKind::DeclStmt:      return "DeclStmt";
        case ASTKind::IfStmt:        return "IfStmt";
        case ASTKind::SwitchStmt:    return "SwitchStmt";
        case ASTKind::SwitchCase:    return "SwitchCase";
        case ASTKind::ForStmt:       return "ForStmt";
        case ASTKind::WhileStmt:     return "WhileStmt";
        case ASTKind::DoWhileStmt:   return "DoWhileStmt";
        case ASTKind::ReturnStmt:    return "ReturnStmt";
        case ASTKind::BreakStmt:     return "BreakStmt";
        case ASTKind::ContinueStmt:  return "ContinueStmt";
        case ASTKind::FuncRefStmt:   return "FuncRefStmt";
        case ASTKind::AsyncStmt:     return "AsyncStmt";
        case ASTKind::AwaitStmt:     return "AwaitStmt";
        case ASTKind::SpawnStmt:     return "SpawnStmt";
        case ASTKind::JoinStmt:      return "JoinStmt";
        
        // Expressions
        case ASTKind::LiteralExpr:      return "LiteralExpr";
        case ASTKind::IdentifierExpr:   return "IdentifierExpr";
        case ASTKind::ArrayLiteralExpr: return "ArrayLiteralExpr";
        case ASTKind::StructLiteralExpr: return "StructLiteralExpr";
        case ASTKind::FieldInit:        return "FieldInit";
        case ASTKind::BinaryExpr:       return "BinaryExpr";
        case ASTKind::UnaryExpr:        return "UnaryExpr";
        case ASTKind::CallExpr:         return "CallExpr";
        case ASTKind::IntrinsicCallExpr: return "IntrinsicCallExpr";
        case ASTKind::IndexExpr:        return "IndexExpr";
        case ASTKind::SliceExpr:        return "SliceExpr";
        case ASTKind::FieldAccessExpr:  return "FieldAccessExpr";
        case ASTKind::ModuleAccessExpr: return "ModuleAccessExpr";
        case ASTKind::AssignExpr:       return "AssignExpr";
        case ASTKind::NullCoalesceExpr: return "NullCoalesceExpr";
        case ASTKind::PipelineExpr:     return "PipelineExpr";
        case ASTKind::PipelineStep:     return "PipelineStep";
        case ASTKind::ComposeExpr:      return "ComposeExpr";
        case ASTKind::ComposeOperand:   return "ComposeOperand";
        case ASTKind::AnonFuncExpr:     return "AnonFuncExpr";
        case ASTKind::IfExpr:           return "IfExpr";
        case ASTKind::RangeExpr:        return "RangeExpr";
        
        // Types
        case ASTKind::PrimitiveType:    return "PrimitiveType";
        case ASTKind::NamedType:        return "NamedType";
        case ASTKind::ModuleTypeAccess: return "ModuleTypeAccess";
        case ASTKind::ArrayType:        return "ArrayType";
        case ASTKind::NullableType:     return "NullableType";
        case ASTKind::FallibleType:     return "FallibleType";
        case ASTKind::CombinedType:     return "CombinedType";
        case ASTKind::RefType:          return "RefType";
        case ASTKind::PtrType:          return "PtrType";
        case ASTKind::FuncType:         return "FuncType";
        case ASTKind::FutureType:       return "FutureType";
        case ASTKind::ThreadType:       return "ThreadType";
        
        default: return "Unknown";
    }
}

// ─── Helper: LiteralKind to String ────────────────────────────────────

std::string JSONDumper::literalKindToString(LiteralKind kind) const {
    switch (kind) {
        case LiteralKind::Int:      return "Int";
        case LiteralKind::Float:    return "Float";
        case LiteralKind::String:   return "String";
        case LiteralKind::RawString:return "RawString";
        case LiteralKind::Char:     return "Char";
        case LiteralKind::Hex:      return "Hex";
        case LiteralKind::Binary:   return "Binary";
        case LiteralKind::True:     return "True";
        case LiteralKind::False:    return "False";
        case LiteralKind::Nil:      return "Nil";
        case LiteralKind::Err:      return "Err";
        default: return "Unknown";
    }
}

// ─── Helper: BinaryOp to String ──────────────────────────────────────

std::string JSONDumper::binaryOpToString(BinaryOp op) const {
    switch (op) {
        case BinaryOp::Add:   return "+";
        case BinaryOp::Sub:   return "-";
        case BinaryOp::Mul:   return "*";
        case BinaryOp::Div:   return "/";
        case BinaryOp::Pow:   return "**";
        case BinaryOp::Mod:   return "%";
        case BinaryOp::Eq:    return "==";
        case BinaryOp::Ne:    return "!=";
        case BinaryOp::Lt:    return "<";
        case BinaryOp::Gt:    return ">";
        case BinaryOp::Le:    return "<=";
        case BinaryOp::Ge:    return ">=";
        case BinaryOp::And:   return "and";
        case BinaryOp::Or:    return "or";
        case BinaryOp::BitAnd:return "&";
        case BinaryOp::BitOr: return "|";
        case BinaryOp::BitXor:return "^";
        case BinaryOp::Shl:   return "<<";
        case BinaryOp::Shr:   return ">>";
        default: return "Unknown";
    }
}

// ─── Helper: UnaryOp to String ──────────────────────────────────────

std::string JSONDumper::unaryOpToString(UnaryOp op) const {
    switch (op) {
        case UnaryOp::Neg:    return "-";
        case UnaryOp::Not:    return "not";
        case UnaryOp::BitNot: return "~";
        default: return "Unknown";
    }
}

// ─── Helper: AssignOp to String ─────────────────────────────────────

std::string JSONDumper::assignOpToString(AssignOp op) const {
    switch (op) {
        case AssignOp::Assign:      return "=";
        case AssignOp::AddAssign:   return "+=";
        case AssignOp::SubAssign:   return "-=";
        case AssignOp::MulAssign:   return "*=";
        case AssignOp::DivAssign:   return "/=";
        case AssignOp::PowAssign:   return "**=";
        case AssignOp::ModAssign:   return "%=";
        case AssignOp::BitAndAssign:return "&=";
        case AssignOp::BitOrAssign: return "|=";
        case AssignOp::BitXorAssign:return "^=";
        case AssignOp::ShlAssign:   return "<<=";
        case AssignOp::ShrAssign:   return ">>=";
        default: return "Unknown";
    }
}

// ─── Helper: PrimitiveKind to String ──────────────────────────────

std::string JSONDumper::primitiveKindToString(PrimitiveKind kind) const {
    switch (kind) {
        case PrimitiveKind::Bool:   return "bool";
        case PrimitiveKind::Byte:   return "byte";
        case PrimitiveKind::Short:  return "short";
        case PrimitiveKind::Int:    return "int";
        case PrimitiveKind::Long:   return "long";
        case PrimitiveKind::Ubyte:  return "ubyte";
        case PrimitiveKind::Ushort: return "ushort";
        case PrimitiveKind::Uint:   return "uint";
        case PrimitiveKind::Ulong:  return "ulong";
        case PrimitiveKind::Int8:   return "int8";
        case PrimitiveKind::Int16:  return "int16";
        case PrimitiveKind::Int32:  return "int32";
        case PrimitiveKind::Int64:  return "int64";
        case PrimitiveKind::Uint8:  return "uint8";
        case PrimitiveKind::Uint16: return "uint16";
        case PrimitiveKind::Uint32: return "uint32";
        case PrimitiveKind::Uint64: return "uint64";
        case PrimitiveKind::Float:  return "float";
        case PrimitiveKind::Double: return "double";
        case PrimitiveKind::Decimal:return "decimal";
        case PrimitiveKind::String: return "string";
        case PrimitiveKind::Char:   return "char";
        default: return "Unknown";
    }
}

// ─── Helper: ArrayKind to String ──────────────────────────────────

std::string JSONDumper::arrayKindToString(ArrayKind kind) const {
    switch (kind) {
        case ArrayKind::Slice:   return "Slice";
        case ArrayKind::Dynamic: return "Dynamic";
        case ArrayKind::Fixed:   return "Fixed";
        default: return "Unknown";
    }
}

// ─── Helper: DeclKeyword to String ──────────────────────────────

std::string JSONDumper::declKeywordToString(DeclKeyword keyword) const {
    switch (keyword) {
        case DeclKeyword::Let:   return "let";
        case DeclKeyword::Const: return "const";
        default: return "Unknown";
    }
}

// ─── Helper: ValueState to String ──────────────────────────────────

std::string JSONDumper::valueStateToString(ValueState state) const {
    switch (state) {
        case ValueState::None:    return "None";
        case ValueState::Definite:return "Definite";
        case ValueState::Nil:     return "Nil";
        case ValueState::Err:     return "Err";
        case ValueState::Unknown: return "Unknown";
        default: return "Unknown";
    }
}

// ─── Helper: Get Module Path ──────────────────────────────────────

std::string JSONDumper::getModulePath(InternedString filePath) const {
    return str(filePath);
}

// ─── Helper: Location Serialization ──────────────────────────────────

std::string JSONDumper::serializeLocation(const SourceLocation& loc, ModuleAST* module) {
    std::ostringstream oss;
    oss << "{";
    oss << quote("line") << ": " << loc.line() << ", ";
    oss << quote("column") << ": " << loc.column();
    if (module && module->filePath.isValid()) {
        oss << ", " << quote("file") << ": " << quote(getModulePath(module->filePath));
    }
    oss << "}";
    return oss.str();
}

// ─── Helper: JSON Values ──────────────────────────────────────────────

std::string JSONDumper::str(InternedString s) const {
    return pool.lookup(s);
}

std::string JSONDumper::escapeString(const std::string& str) const {
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

std::string JSONDumper::indent(int level) const {
    if (!pretty) return "";
    return std::string(level * 2, ' ');
}

std::string JSONDumper::quote(const std::string& str) const {
    return "\"" + escapeString(str) + "\"";
}

std::string JSONDumper::jsonBool(bool value) const {
    return value ? "true" : "false";
}

std::string JSONDumper::jsonNull() const {
    return "null";
}

std::string JSONDumper::jsonNumber(int64_t value) const {
    return std::to_string(value);
}

std::string JSONDumper::jsonNumber(uint64_t value) const {
    return std::to_string(value);
}

std::string JSONDumper::jsonNumber(double value) const {
    std::ostringstream oss;
    oss << std::setprecision(17) << value;
    return oss.str();
}

// ─── Module Serialization ──────────────────────────────────────────────

std::string JSONDumper::serializeModules() {
    std::ostringstream oss;
    oss << "[";
    if (pretty) oss << "\n";
    indentLevel = 2;
    
    for (size_t i = 0; i < modules.size(); ++i) {
        if (i > 0) {
            oss << ",";
            if (pretty) oss << "\n";
        }
        oss << serializeModule(modules[i]);
    }
    
    indentLevel = 1;
    if (pretty) oss << "\n" << indent(1);
    oss << "]";
    
    return oss.str();
}

std::string JSONDumper::serializeModule(ModuleAST* module) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 3;
    
    // File path
    oss << indent(3) << quote("kind") << ": " << quote("Module");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(3) << quote("filePath") << ": " << quote(getModulePath(module->filePath));
    oss << ",";
    if (pretty) oss << "\n";
    
    // Imports
    oss << indent(3) << quote("imports") << ": [";
    for (size_t i = 0; i < module->imports.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << quote(getModulePath(module->imports[i]));
    }
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    // Has errors
    oss << indent(3) << quote("hasErrors") << ": " << jsonBool(module->hasErrors);
    oss << ",";
    if (pretty) oss << "\n";
    
    // Declarations
    oss << indent(3) << quote("declarations") << ": [";
    if (pretty) oss << "\n";
    indentLevel = 4;
    for (size_t i = 0; i < module->decls.size(); ++i) {
        if (i > 0) {
            oss << ",";
            if (pretty) oss << "\n";
        }
        oss << serializeDecl(module->decls[i]);
    }
    indentLevel = 3;
    if (pretty) oss << "\n" << indent(3);
    oss << "]";
    oss << "\n";
    
    indentLevel = 2;
    oss << indent(2) << "}";
    
    return oss.str();
}

// ─── Declaration Serializers ──────────────────────────────────────────

std::string JSONDumper::serializeDecl(DeclAST* decl) {
    if (!decl) return jsonNull();
    
    switch (decl->kind) {
        case ASTKind::ImportDecl:    return serializeImportDecl(static_cast<ImportDeclAST*>(decl));
        case ASTKind::VarDecl:       return serializeVarDecl(static_cast<VarDeclAST*>(decl));
        case ASTKind::Param:         return serializeParam(static_cast<ParamAST*>(decl));
        case ASTKind::FuncDecl:      return serializeFuncDecl(static_cast<FuncDeclAST*>(decl));
        case ASTKind::StructDecl:    return serializeStructDecl(static_cast<StructDeclAST*>(decl));
        case ASTKind::EnumDecl:      return serializeEnumDecl(static_cast<EnumDeclAST*>(decl));
        case ASTKind::TraitDecl:     return serializeTraitDecl(static_cast<TraitDeclAST*>(decl));
        case ASTKind::FieldDecl:     return serializeFieldDecl(static_cast<FieldDeclAST*>(decl));
        case ASTKind::TraitFieldDecl: return serializeTraitFieldDecl(static_cast<TraitFieldDeclAST*>(decl));
        case ASTKind::EnumVariant:   return serializeEnumVariant(static_cast<EnumVariantAST*>(decl));
        case ASTKind::GenericParamDecl: return serializeGenericParam(static_cast<GenericParamDeclAST*>(decl));
        default:
            // Fallback for unknown declarations
            std::ostringstream oss;
            oss << "{";
            oss << quote("kind") << ": " << quote(kindToString(decl->kind)) << ", ";
            oss << quote("name") << ": " << quote(str(decl->name));
            oss << "}";
            return oss.str();
    }
}

// ─── Import Decl ─────────────────────────────────────────────────────

std::string JSONDumper::serializeImportDecl(ImportDeclAST* decl) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("ImportDecl");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("path") << ": " << quote(str(decl->path));
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("alias") << ": " << quote(str(decl->alias));
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Var Decl ────────────────────────────────────────────────────────

std::string JSONDumper::serializeVarDecl(VarDeclAST* decl) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("VarDecl");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("name") << ": " << quote(str(decl->name));
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("keyword") << ": " << quote(declKeywordToString(decl->keyword));
    oss << ",";
    if (pretty) oss << "\n";
    
    if (decl->type) {
        oss << indent(5) << quote("type") << ": " << serializeType(decl->type);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (decl->init) {
        oss << indent(5) << quote("init") << ": " << serializeExpr(decl->init);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    // Location
    oss << indent(5) << quote("location") << ": " << serializeLocation(decl->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Param ────────────────────────────────────────────────────────────

std::string JSONDumper::serializeParam(ParamAST* param) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("Param");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("name") << ": " << quote(str(param->name));
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("isVariadic") << ": " << jsonBool(param->isVariadic);
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("isConstParam") << ": " << jsonBool(param->isConstParam);
    oss << ",";
    if (pretty) oss << "\n";
    
    if (param->type) {
        oss << indent(5) << quote("type") << ": " << serializeType(param->type);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(param->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Func Decl ────────────────────────────────────────────────────────

std::string JSONDumper::serializeFuncDecl(FuncDeclAST* decl) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("FuncDecl");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("name") << ": " << quote(str(decl->name));
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("keyword") << ": " << quote(declKeywordToString(decl->keyword));
    oss << ",";
    if (pretty) oss << "\n";
    
    // Generic params
    oss << indent(5) << quote("genericParams") << ": [";
    if (pretty) oss << "\n";
    indentLevel = 6;
    for (size_t i = 0; i < decl->genericParams.size(); ++i) {
        if (i > 0) {
            oss << ",";
            if (pretty) oss << "\n";
        }
        oss << serializeGenericParam(decl->genericParams[i]);
    }
    indentLevel = 5;
    if (pretty) oss << "\n" << indent(5);
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    // Function type
    if (decl->funcType) {
        oss << indent(5) << quote("funcType") << ": " << serializeFuncType(decl->funcType);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    // Body
    if (decl->body) {
        oss << indent(5) << quote("body") << ": " << serializeStmt(decl->body);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    // Semantic fields
    oss << indent(5) << quote("isForeignFunction") << ": " << jsonBool(decl->isForeignFunction);
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("isInline") << ": " << jsonBool(decl->isInline);
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("hasClosure") << ": " << jsonBool(decl->hasClosure);
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(decl->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Struct Decl ─────────────────────────────────────────────────────

std::string JSONDumper::serializeStructDecl(StructDeclAST* decl) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("StructDecl");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("name") << ": " << quote(str(decl->name));
    oss << ",";
    if (pretty) oss << "\n";
    
    // Generic params
    oss << indent(5) << quote("genericParams") << ": [";
    if (pretty) oss << "\n";
    indentLevel = 6;
    for (size_t i = 0; i < decl->genericParams.size(); ++i) {
        if (i > 0) {
            oss << ",";
            if (pretty) oss << "\n";
        }
        oss << serializeGenericParam(decl->genericParams[i]);
    }
    indentLevel = 5;
    if (pretty) oss << "\n" << indent(5);
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    // Fields
    oss << indent(5) << quote("fields") << ": [";
    if (pretty) oss << "\n";
    indentLevel = 6;
    for (size_t i = 0; i < decl->fields.size(); ++i) {
        if (i > 0) {
            oss << ",";
            if (pretty) oss << "\n";
        }
        oss << serializeFieldDecl(decl->fields[i]);
    }
    indentLevel = 5;
    if (pretty) oss << "\n" << indent(5);
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    // Trait refs
    oss << indent(5) << quote("traitRefs") << ": [";
    for (size_t i = 0; i < decl->traitRefs.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << serializeType(decl->traitRefs[i]);
    }
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("isPacked") << ": " << jsonBool(decl->isPacked);
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(decl->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Enum Decl ──────────────────────────────────────────────────────

std::string JSONDumper::serializeEnumDecl(EnumDeclAST* decl) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("EnumDecl");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("name") << ": " << quote(str(decl->name));
    oss << ",";
    if (pretty) oss << "\n";
    
    // Backing type
    if (decl->backingType) {
        oss << indent(5) << quote("backingType") << ": " << serializeType(decl->backingType);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    // Variants
    oss << indent(5) << quote("variants") << ": [";
    if (pretty) oss << "\n";
    indentLevel = 6;
    for (size_t i = 0; i < decl->variants.size(); ++i) {
        if (i > 0) {
            oss << ",";
            if (pretty) oss << "\n";
        }
        oss << serializeEnumVariant(decl->variants[i]);
    }
    indentLevel = 5;
    if (pretty) oss << "\n" << indent(5);
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(decl->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Trait Decl ─────────────────────────────────────────────────────

std::string JSONDumper::serializeTraitDecl(TraitDeclAST* decl) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("TraitDecl");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("name") << ": " << quote(str(decl->name));
    oss << ",";
    if (pretty) oss << "\n";
    
    // Generic params
    oss << indent(5) << quote("genericParams") << ": [";
    if (pretty) oss << "\n";
    indentLevel = 6;
    for (size_t i = 0; i < decl->genericParams.size(); ++i) {
        if (i > 0) {
            oss << ",";
            if (pretty) oss << "\n";
        }
        oss << serializeGenericParam(decl->genericParams[i]);
    }
    indentLevel = 5;
    if (pretty) oss << "\n" << indent(5);
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    // Fields
    oss << indent(5) << quote("fields") << ": [";
    if (pretty) oss << "\n";
    indentLevel = 6;
    for (size_t i = 0; i < decl->fields.size(); ++i) {
        if (i > 0) {
            oss << ",";
            if (pretty) oss << "\n";
        }
        oss << serializeTraitFieldDecl(decl->fields[i]);
    }
    indentLevel = 5;
    if (pretty) oss << "\n" << indent(5);
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(decl->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Field Decl ─────────────────────────────────────────────────────

std::string JSONDumper::serializeFieldDecl(FieldDeclAST* field) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 6;
    
    oss << indent(6) << quote("kind") << ": " << quote("FieldDecl");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(6) << quote("name") << ": " << quote(str(field->name));
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(6) << quote("isConstField") << ": " << jsonBool(field->isConstField);
    oss << ",";
    if (pretty) oss << "\n";
    
    if (field->type) {
        oss << indent(6) << quote("type") << ": " << serializeType(field->type);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (field->defaultVal) {
        oss << indent(6) << quote("defaultVal") << ": " << serializeExpr(field->defaultVal);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(6) << quote("location") << ": " << serializeLocation(field->loc);
    
    oss << "\n";
    indentLevel = 5;
    oss << indent(5) << "}";
    
    return oss.str();
}

// ─── Trait Field Decl ──────────────────────────────────────────────

std::string JSONDumper::serializeTraitFieldDecl(TraitFieldDeclAST* field) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 6;
    
    oss << indent(6) << quote("kind") << ": " << quote("TraitFieldDecl");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(6) << quote("name") << ": " << quote(str(field->name));
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(6) << quote("isConstField") << ": " << jsonBool(field->isConstField);
    oss << ",";
    if (pretty) oss << "\n";
    
    if (field->type) {
        oss << indent(6) << quote("type") << ": " << serializeType(field->type);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(6) << quote("location") << ": " << serializeLocation(field->loc);
    
    oss << "\n";
    indentLevel = 5;
    oss << indent(5) << "}";
    
    return oss.str();
}

// ─── Enum Variant ──────────────────────────────────────────────────

std::string JSONDumper::serializeEnumVariant(EnumVariantAST* variant) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 6;
    
    oss << indent(6) << quote("kind") << ": " << quote("EnumVariant");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(6) << quote("name") << ": " << quote(str(variant->name));
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(6) << quote("value") << ": " << jsonNumber(variant->value);
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(6) << quote("location") << ": " << serializeLocation(variant->loc);
    
    oss << "\n";
    indentLevel = 5;
    oss << indent(5) << "}";
    
    return oss.str();
}

// ─── Generic Param ─────────────────────────────────────────────────

std::string JSONDumper::serializeGenericParam(GenericParamDeclAST* param) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 6;
    
    oss << indent(6) << quote("kind") << ": " << quote("GenericParamDecl");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(6) << quote("name") << ": " << quote(str(param->name));
    oss << ",";
    if (pretty) oss << "\n";
    
    // Constraints
    oss << indent(6) << quote("constraints") << ": [";
    for (size_t i = 0; i < param->constraints.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << serializeType(param->constraints[i]);
    }
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(6) << quote("location") << ": " << serializeLocation(param->loc);
    
    oss << "\n";
    indentLevel = 5;
    oss << indent(5) << "}";
    
    return oss.str();
}

// ─── Statement Serializers ──────────────────────────────────────────────

// ... (I'll show the key ones, the pattern continues)

std::string JSONDumper::serializeStmt(StmtAST* stmt) {
    if (!stmt) return jsonNull();
    
    switch (stmt->kind) {
        case ASTKind::BlockStmt:     return serializeBlockStmt(static_cast<BlockStmtAST*>(stmt));
        case ASTKind::ExprStmt:      return serializeExprStmt(static_cast<ExprStmtAST*>(stmt));
        case ASTKind::DeclStmt:      return serializeDeclStmt(static_cast<DeclStmtAST*>(stmt));
        case ASTKind::IfStmt:        return serializeIfStmt(static_cast<IfStmtAST*>(stmt));
        case ASTKind::SwitchStmt:    return serializeSwitchStmt(static_cast<SwitchStmtAST*>(stmt));
        case ASTKind::ForStmt:       return serializeForStmt(static_cast<ForStmtAST*>(stmt));
        case ASTKind::WhileStmt:     return serializeWhileStmt(static_cast<WhileStmtAST*>(stmt));
        case ASTKind::DoWhileStmt:   return serializeDoWhileStmt(static_cast<DoWhileStmtAST*>(stmt));
        case ASTKind::ReturnStmt:    return serializeReturnStmt(static_cast<ReturnStmtAST*>(stmt));
        case ASTKind::BreakStmt:     return serializeBreakStmt(static_cast<BreakStmtAST*>(stmt));
        case ASTKind::ContinueStmt:  return serializeContinueStmt(static_cast<ContinueStmtAST*>(stmt));
        case ASTKind::FuncRefStmt:   return serializeFuncRefStmt(static_cast<FuncRefStmtAST*>(stmt));
        default:
            return serializeNode(stmt);
    }
}

// ─── Block Stmt ─────────────────────────────────────────────────────

std::string JSONDumper::serializeBlockStmt(BlockStmtAST* stmt) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("BlockStmt");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("statements") << ": [";
    if (pretty) oss << "\n";
    indentLevel = 6;
    for (size_t i = 0; i < stmt->stmts.size(); ++i) {
        if (i > 0) {
            oss << ",";
            if (pretty) oss << "\n";
        }
        oss << serializeStmt(stmt->stmts[i]);
    }
    indentLevel = 5;
    if (pretty) oss << "\n" << indent(5);
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(stmt->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Expression Serializers ─────────────────────────────────────────────

std::string JSONDumper::serializeExpr(ExprAST* expr) {
    if (!expr) return jsonNull();
    
    switch (expr->kind) {
        case ASTKind::LiteralExpr:       return serializeLiteralExpr(static_cast<LiteralExprAST*>(expr));
        case ASTKind::IdentifierExpr:    return serializeIdentifierExpr(static_cast<IdentifierExprAST*>(expr));
        case ASTKind::ArrayLiteralExpr:  return serializeArrayLiteralExpr(static_cast<ArrayLiteralExprAST*>(expr));
        case ASTKind::StructLiteralExpr: return serializeStructLiteralExpr(static_cast<StructLiteralExprAST*>(expr));
        case ASTKind::BinaryExpr:        return serializeBinaryExpr(static_cast<BinaryExprAST*>(expr));
        case ASTKind::UnaryExpr:         return serializeUnaryExpr(static_cast<UnaryExprAST*>(expr));
        case ASTKind::CallExpr:          return serializeCallExpr(static_cast<CallExprAST*>(expr));
        case ASTKind::IntrinsicCallExpr: return serializeIntrinsicCallExpr(static_cast<IntrinsicCallExprAST*>(expr));
        case ASTKind::IndexExpr:         return serializeIndexExpr(static_cast<IndexExprAST*>(expr));
        case ASTKind::SliceExpr:         return serializeSliceExpr(static_cast<SliceExprAST*>(expr));
        case ASTKind::FieldAccessExpr:   return serializeFieldAccessExpr(static_cast<FieldAccessExprAST*>(expr));
        case ASTKind::ModuleAccessExpr:  return serializeModuleAccessExpr(static_cast<ModuleAccessExprAST*>(expr));
        case ASTKind::AssignExpr:        return serializeAssignExpr(static_cast<AssignExprAST*>(expr));
        case ASTKind::NullCoalesceExpr:  return serializeNullCoalesceExpr(static_cast<NullCoalesceExprAST*>(expr));
        case ASTKind::PipelineExpr:      return serializePipelineExpr(static_cast<PipelineExprAST*>(expr));
        case ASTKind::ComposeExpr:       return serializeComposeExpr(static_cast<ComposeExprAST*>(expr));
        case ASTKind::AnonFuncExpr:      return serializeAnonFuncExpr(static_cast<AnonFuncExprAST*>(expr));
        case ASTKind::IfExpr:            return serializeIfExpr(static_cast<IfExprAST*>(expr));
        case ASTKind::RangeExpr:         return serializeRangeExpr(static_cast<RangeExprAST*>(expr));
        default:
            return serializeNode(expr);
    }
}

// ─── Literal Expr ─────────────────────────────────────────────────

std::string JSONDumper::serializeLiteralExpr(LiteralExprAST* expr) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("LiteralExpr");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("literalKind") << ": " << quote(literalKindToString(expr->kind));
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("value") << ": " << quote(str(expr->value));
    oss << ",";
    if (pretty) oss << "\n";
    
    // Semantic fields
    oss << indent(5) << quote("isConst") << ": " << jsonBool(expr->isConst);
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->hasType()) {
        oss << indent(5) << quote("resolvedType") << ": " << serializeType(expr->resolvedType);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("valueState") << ": " << quote(valueStateToString(expr->valueState));
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(expr->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Identifier Expr ──────────────────────────────────────────────

std::string JSONDumper::serializeIdentifierExpr(IdentifierExprAST* expr) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("IdentifierExpr");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("name") << ": " << quote(str(expr->name));
    oss << ",";
    if (pretty) oss << "\n";
    
    // Generic args
    oss << indent(5) << quote("genericArgs") << ": [";
    for (size_t i = 0; i < expr->genericArgs.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << serializeType(expr->genericArgs[i]);
    }
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    // Semantic fields
    oss << indent(5) << quote("resolved") << ": " << jsonBool(expr->resolvedDecl != nullptr);
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->hasType()) {
        oss << indent(5) << quote("resolvedType") << ": " << serializeType(expr->resolvedType);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("valueState") << ": " << quote(valueStateToString(expr->valueState));
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("isConst") << ": " << jsonBool(expr->isConst);
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("isLValue") << ": " << jsonBool(expr->isLValue);
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(expr->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Binary Expr ─────────────────────────────────────────────────

std::string JSONDumper::serializeBinaryExpr(BinaryExprAST* expr) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("BinaryExpr");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("op") << ": " << quote(binaryOpToString(expr->op));
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("left") << ": " << serializeExpr(expr->left);
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("right") << ": " << serializeExpr(expr->right);
    oss << ",";
    if (pretty) oss << "\n";
    
    // Semantic fields
    if (expr->hasType()) {
        oss << indent(5) << quote("resolvedType") << ": " << serializeType(expr->resolvedType);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("valueState") << ": " << quote(valueStateToString(expr->valueState));
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("isConst") << ": " << jsonBool(expr->isConst);
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(expr->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Type Serializers ─────────────────────────────────────────────────

std::string JSONDumper::serializeType(TypeAST* type) {
    if (!type) return jsonNull();
    
    switch (type->kind) {
        case ASTKind::PrimitiveType:    return serializePrimitiveType(static_cast<PrimitiveTypeAST*>(type));
        case ASTKind::NamedType:        return serializeNamedType(static_cast<NamedTypeAST*>(type));
        case ASTKind::ArrayType:        return serializeArrayType(static_cast<ArrayTypeAST*>(type));
        case ASTKind::NullableType:     return serializeNullableType(static_cast<NullableTypeAST*>(type));
        case ASTKind::FallibleType:     return serializeFallibleType(static_cast<FallibleTypeAST*>(type));
        case ASTKind::CombinedType:     return serializeCombinedType(static_cast<CombinedTypeAST*>(type));
        case ASTKind::RefType:          return serializeRefType(static_cast<RefTypeAST*>(type));
        case ASTKind::PtrType:          return serializePtrType(static_cast<PtrTypeAST*>(type));
        case ASTKind::FuncType:         return serializeFuncType(static_cast<FuncTypeAST*>(type));
        case ASTKind::ModuleTypeAccess: return serializeModuleTypeAccess(static_cast<ModuleTypeAccessAST*>(type));
        case ASTKind::FutureType:       return serializeFutureType(static_cast<FutureTypeAST*>(type));
        case ASTKind::ThreadType:       return serializeThreadType(static_cast<ThreadTypeAST*>(type));
        default:
            return serializeNode(type);
    }
}

// ─── Primitive Type ─────────────────────────────────────────────

std::string JSONDumper::serializePrimitiveType(PrimitiveTypeAST* type) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("PrimitiveType");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("primitiveKind") << ": " << quote(primitiveKindToString(type->primitiveKind));
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(type->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Named Type ─────────────────────────────────────────────────

std::string JSONDumper::serializeNamedType(NamedTypeAST* type) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("NamedType");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("name") << ": " << quote(str(type->name));
    oss << ",";
    if (pretty) oss << "\n";
    
    // Generic args
    oss << indent(5) << quote("genericArgs") << ": [";
    for (size_t i = 0; i < type->genericArgs.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << serializeType(type->genericArgs[i]);
    }
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(type->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Array Type ─────────────────────────────────────────────────

std::string JSONDumper::serializeArrayType(ArrayTypeAST* type) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("ArrayType");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("arrayKind") << ": " << quote(arrayKindToString(type->arrayKind));
    oss << ",";
    if (pretty) oss << "\n";
    
    if (type->isFixed()) {
        oss << indent(5) << quote("size") << ": " << jsonNumber(type->size);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("element") << ": " << serializeType(type->element);
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(type->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Nullable Type ──────────────────────────────────────────────

std::string JSONDumper::serializeNullableType(NullableTypeAST* type) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("NullableType");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("inner") << ": " << serializeType(type->inner);
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(type->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Function Type ──────────────────────────────────────────────

std::string JSONDumper::serializeFuncType(FuncTypeAST* type) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("FuncType");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("params") << ": [";
    if (pretty) oss << "\n";
    indentLevel = 6;
    for (size_t i = 0; i < type->params.size(); ++i) {
        if (i > 0) {
            oss << ",";
            if (pretty) oss << "\n";
        }
        oss << serializeParam(type->params[i]);
    }
    indentLevel = 5;
    if (pretty) oss << "\n" << indent(5);
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("hasArrow") << ": " << jsonBool(type->hasArrow);
    oss << ",";
    if (pretty) oss << "\n";
    
    if (type->returnType) {
        oss << indent(5) << quote("returnType") << ": " << serializeType(type->returnType);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(type->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Future Type ──────────────────────────────────────────────

std::string JSONDumper::serializeFutureType(FutureTypeAST* type) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("FutureType");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("inner") << ": " << serializeType(type->inner);
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(type->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Thread Type ──────────────────────────────────────────────

std::string JSONDumper::serializeThreadType(ThreadTypeAST* type) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("ThreadType");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("inner") << ": " << serializeType(type->inner);
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(type->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Diagnostics ──────────────────────────────────────────────

std::string JSONDumper::serializeDiagnostics(const DiagnosticEngine& diagnostics) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 2;
    
    oss << indent(2) << quote("errorCount") << ": " << diagnostics.errorCount();
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(2) << quote("warningCount") << ": " << diagnostics.warningCount();
    oss << ",";
    if (pretty) oss << "\n";
    
    // TODO: Serialize individual diagnostic messages
    oss << indent(2) << quote("messages") << ": []";
    oss << "\n";
    
    indentLevel = 1;
    oss << indent(1) << "}";
    
    return oss.str();
}

// ─── Node Dispatcher ──────────────────────────────────────────

std::string JSONDumper::serializeNode(BaseAST* node) {
    if (!node) return jsonNull();
    
    // Dispatch to the appropriate serializer based on kind
    // The actual implementation would call serializeDecl, serializeStmt,
    // serializeExpr, or serializeType based on the node's kind hierarchy
    
    // This is a fallback - the specific serializers above should handle
    // all concrete node types through the switch statements in serializeDecl,
    // serializeStmt, serializeExpr, and serializeType.
    
    std::ostringstream oss;
    oss << "{";
    oss << quote("kind") << ": " << quote(kindToString(node->kind));
    oss << ", " << quote("location") << ": " << serializeLocation(node->loc);
    oss << "}";
    return oss.str();
}

} // namespace frontend
} // namespace cli