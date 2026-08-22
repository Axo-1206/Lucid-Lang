/// @file cli/frontend/JSONDumper.cpp
/// @brief Implementation of complete JSON serialization.

#include "JSONDumper.hpp"
#include "core/ast/TypeAST.hpp"

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
        case ASTKind::ImportDecl:       return serializeImportDecl(decl->as<ImportDeclAST>());
        case ASTKind::VarDecl:          return serializeVarDecl(decl->as<VarDeclAST>());
        case ASTKind::Param:            return serializeParam(decl->as<ParamAST>());
        case ASTKind::FuncDecl:         return serializeFuncDecl(decl->as<FuncDeclAST>());
        case ASTKind::StructDecl:       return serializeStructDecl(decl->as<StructDeclAST>());
        case ASTKind::EnumDecl:         return serializeEnumDecl(decl->as<EnumDeclAST>());
        case ASTKind::TraitDecl:        return serializeTraitDecl(decl->as<TraitDeclAST>());
        case ASTKind::FieldDecl:        return serializeFieldDecl(decl->as<FieldDeclAST>());
        case ASTKind::TraitFieldDecl:   return serializeTraitFieldDecl(decl->as<TraitFieldDeclAST>());
        case ASTKind::EnumVariant:      return serializeEnumVariant(decl->as<EnumVariantAST>());
        case ASTKind::GenericParamDecl: return serializeGenericParam(decl->as<GenericParamDeclAST>());
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

std::string JSONDumper::serializeStmt(StmtAST* stmt) {
    if (!stmt) return jsonNull();
    
    switch (stmt->kind) {
        case ASTKind::BlockStmt:     return serializeBlockStmt(stmt->as<BlockStmtAST>());
        case ASTKind::ExprStmt:      return serializeExprStmt(stmt->as<ExprStmtAST>());
        case ASTKind::DeclStmt:      return serializeDeclStmt(stmt->as<DeclStmtAST>());
        case ASTKind::IfStmt:        return serializeIfStmt(stmt->as<IfStmtAST>());
        case ASTKind::SwitchStmt:    return serializeSwitchStmt(stmt->as<SwitchStmtAST>());
        case ASTKind::ForStmt:       return serializeForStmt(stmt->as<ForStmtAST>());
        case ASTKind::WhileStmt:     return serializeWhileStmt(stmt->as<WhileStmtAST>());
        case ASTKind::DoWhileStmt:   return serializeDoWhileStmt(stmt->as<DoWhileStmtAST>());
        case ASTKind::ReturnStmt:    return serializeReturnStmt(stmt->as<ReturnStmtAST>());
        case ASTKind::BreakStmt:     return serializeBreakStmt(stmt->as<BreakStmtAST>());
        case ASTKind::ContinueStmt:  return serializeContinueStmt(stmt->as<ContinueStmtAST>());
        case ASTKind::FuncRefStmt:   return serializeFuncRefStmt(stmt->as<FuncRefStmtAST>());
        case ASTKind::AsyncStmt:     return serializeAsyncStmt(stmt->as<AsyncStmtAST>());
        case ASTKind::AwaitStmt:     return serializeAwaitStmt(stmt->as<AwaitStmtAST>());
        case ASTKind::SpawnStmt:     return serializeSpawnStmt(stmt->as<SpawnStmtAST>());
        case ASTKind::JoinStmt:      return serializeJoinStmt(stmt->as<JoinStmtAST>());
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

// ─── Expr Stmt ─────────────────────────────────────────────────────

std::string JSONDumper::serializeExprStmt(ExprStmtAST* stmt) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("ExprStmt");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (stmt->expr) {
        oss << indent(5) << quote("expr") << ": " << serializeExpr(stmt->expr);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(stmt->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Decl Stmt ─────────────────────────────────────────────────────

std::string JSONDumper::serializeDeclStmt(DeclStmtAST* stmt) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("DeclStmt");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (stmt->decl) {
        oss << indent(5) << quote("decl") << ": " << serializeDecl(stmt->decl);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(stmt->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── If Stmt ──────────────────────────────────────────────────────

std::string JSONDumper::serializeIfStmt(IfStmtAST* stmt) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("IfStmt");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (stmt->condition) {
        oss << indent(5) << quote("condition") << ": " << serializeExpr(stmt->condition);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (stmt->thenBranch) {
        oss << indent(5) << quote("thenBranch") << ": " << serializeStmt(stmt->thenBranch);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (stmt->elseBranch) {
        oss << indent(5) << quote("elseBranch") << ": " << serializeStmt(stmt->elseBranch);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(stmt->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Switch Stmt ──────────────────────────────────────────────────

std::string JSONDumper::serializeSwitchStmt(SwitchStmtAST* stmt) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("SwitchStmt");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (stmt->subject) {
        oss << indent(5) << quote("subject") << ": " << serializeExpr(stmt->subject);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    // Cases
    oss << indent(5) << quote("cases") << ": [";
    if (pretty) oss << "\n";
    indentLevel = 6;
    for (size_t i = 0; i < stmt->cases.size(); ++i) {
        if (i > 0) {
            oss << ",";
            if (pretty) oss << "\n";
        }
        oss << serializeSwitchCase(stmt->cases[i]);
    }
    indentLevel = 5;
    if (pretty) oss << "\n" << indent(5);
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    if (stmt->defaultBody) {
        oss << indent(5) << quote("defaultBody") << ": " << serializeStmt(stmt->defaultBody);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (stmt->defaultLoc.has_value()) {
        oss << indent(5) << quote("defaultLoc") << ": " << serializeLocation(stmt->defaultLoc.value());
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(stmt->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Switch Case ─────────────────────────────────────────────────

std::string JSONDumper::serializeSwitchCase(SwitchCaseAST* case_) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 7;
    
    oss << indent(7) << quote("kind") << ": " << quote("SwitchCase");
    oss << ",";
    if (pretty) oss << "\n";
    
    // Values
    oss << indent(7) << quote("values") << ": [";
    for (size_t i = 0; i < case_->values.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << serializeExpr(case_->values[i]);
    }
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    if (case_->body) {
        oss << indent(7) << quote("body") << ": " << serializeStmt(case_->body);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(7) << quote("location") << ": " << serializeLocation(case_->loc);
    
    oss << "\n";
    indentLevel = 6;
    oss << indent(6) << "}";
    
    return oss.str();
}

// ─── For Stmt ────────────────────────────────────────────────────

std::string JSONDumper::serializeForStmt(ForStmtAST* stmt) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("ForStmt");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (stmt->indexVar) {
        oss << indent(5) << quote("indexVar") << ": " << serializeParam(stmt->indexVar);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (stmt->valueVar) {
        oss << indent(5) << quote("valueVar") << ": " << serializeParam(stmt->valueVar);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (stmt->iterable) {
        oss << indent(5) << quote("iterable") << ": " << serializeExpr(stmt->iterable);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (stmt->step) {
        oss << indent(5) << quote("step") << ": " << serializeExpr(stmt->step);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (stmt->body) {
        oss << indent(5) << quote("body") << ": " << serializeStmt(stmt->body);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(stmt->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── While Stmt ──────────────────────────────────────────────────

std::string JSONDumper::serializeWhileStmt(WhileStmtAST* stmt) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("WhileStmt");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (stmt->condition) {
        oss << indent(5) << quote("condition") << ": " << serializeExpr(stmt->condition);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (stmt->body) {
        oss << indent(5) << quote("body") << ": " << serializeStmt(stmt->body);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(stmt->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Do While Stmt ──────────────────────────────────────────────

std::string JSONDumper::serializeDoWhileStmt(DoWhileStmtAST* stmt) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("DoWhileStmt");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (stmt->body) {
        oss << indent(5) << quote("body") << ": " << serializeStmt(stmt->body);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (stmt->condition) {
        oss << indent(5) << quote("condition") << ": " << serializeExpr(stmt->condition);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(stmt->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Return Stmt ─────────────────────────────────────────────────

std::string JSONDumper::serializeReturnStmt(ReturnStmtAST* stmt) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("ReturnStmt");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (stmt->value) {
        oss << indent(5) << quote("value") << ": " << serializeExpr(stmt->value);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(stmt->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Break Stmt ──────────────────────────────────────────────────

std::string JSONDumper::serializeBreakStmt(BreakStmtAST* stmt) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("BreakStmt");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(stmt->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Continue Stmt ──────────────────────────────────────────────

std::string JSONDumper::serializeContinueStmt(ContinueStmtAST* stmt) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("ContinueStmt");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(stmt->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Func Ref Stmt ──────────────────────────────────────────────

std::string JSONDumper::serializeFuncRefStmt(FuncRefStmtAST* stmt) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("FuncRefStmt");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (stmt->target) {
        oss << indent(5) << quote("target") << ": " << serializeExpr(stmt->target);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(stmt->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Async Stmt ──────────────────────────────────────────────────

std::string JSONDumper::serializeAsyncStmt(AsyncStmtAST* stmt) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("AsyncStmt");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (stmt->binding) {
        oss << indent(5) << quote("binding") << ": " << serializeVarDecl(stmt->binding);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (stmt->call) {
        oss << indent(5) << quote("call") << ": " << serializeExpr(stmt->call);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(stmt->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Await Stmt ──────────────────────────────────────────────────

std::string JSONDumper::serializeAwaitStmt(AwaitStmtAST* stmt) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("AwaitStmt");
    oss << ",";
    if (pretty) oss << "\n";
    
    // Targets
    oss << indent(5) << quote("targets") << ": [";
    for (size_t i = 0; i < stmt->targets.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << serializeExpr(stmt->targets[i]);
    }
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(stmt->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Spawn Stmt ──────────────────────────────────────────────────

std::string JSONDumper::serializeSpawnStmt(SpawnStmtAST* stmt) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("SpawnStmt");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (stmt->binding) {
        oss << indent(5) << quote("binding") << ": " << serializeVarDecl(stmt->binding);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (stmt->call) {
        oss << indent(5) << quote("call") << ": " << serializeExpr(stmt->call);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(stmt->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Join Stmt ───────────────────────────────────────────────────

std::string JSONDumper::serializeJoinStmt(JoinStmtAST* stmt) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("JoinStmt");
    oss << ",";
    if (pretty) oss << "\n";
    
    // Targets
    oss << indent(5) << quote("targets") << ": [";
    for (size_t i = 0; i < stmt->targets.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << serializeExpr(stmt->targets[i]);
    }
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
        case ASTKind::LiteralExpr:       return serializeLiteralExpr(expr->as<LiteralExprAST>());
        case ASTKind::IdentifierExpr:    return serializeIdentifierExpr(expr->as<IdentifierExprAST>());
        case ASTKind::ArrayLiteralExpr:  return serializeArrayLiteralExpr(expr->as<ArrayLiteralExprAST>());
        case ASTKind::StructLiteralExpr: return serializeStructLiteralExpr(expr->as<StructLiteralExprAST>());
        case ASTKind::BinaryExpr:        return serializeBinaryExpr(expr->as<BinaryExprAST>());
        case ASTKind::UnaryExpr:         return serializeUnaryExpr(expr->as<UnaryExprAST>());
        case ASTKind::CallExpr:          return serializeCallExpr(expr->as<CallExprAST>());
        case ASTKind::IntrinsicCallExpr: return serializeIntrinsicCallExpr(expr->as<IntrinsicCallExprAST>());
        case ASTKind::IndexExpr:         return serializeIndexExpr(expr->as<IndexExprAST>());
        case ASTKind::SliceExpr:         return serializeSliceExpr(expr->as<SliceExprAST>());
        case ASTKind::FieldAccessExpr:   return serializeFieldAccessExpr(expr->as<FieldAccessExprAST>());
        case ASTKind::ModuleAccessExpr:  return serializeModuleAccessExpr(expr->as<ModuleAccessExprAST>());
        case ASTKind::AssignExpr:        return serializeAssignExpr(expr->as<AssignExprAST>());
        case ASTKind::NullCoalesceExpr:  return serializeNullCoalesceExpr(expr->as<NullCoalesceExprAST>());
        case ASTKind::PipelineExpr:      return serializePipelineExpr(expr->as<PipelineExprAST>());
        case ASTKind::ComposeExpr:       return serializeComposeExpr(expr->as<ComposeExprAST>());
        case ASTKind::AnonFuncExpr:      return serializeAnonFuncExpr(expr->as<AnonFuncExprAST>());
        case ASTKind::IfExpr:            return serializeIfExpr(expr->as<IfExprAST>());
        case ASTKind::RangeExpr:         return serializeRangeExpr(expr->as<RangeExprAST>());
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
    } else {
        oss << indent(5) << quote("resolvedType") << ": " << jsonNull();
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
    
    if (expr->resolvedDecl) {
        oss << indent(5) << quote("resolvedDecl") << ": " << quote(str(expr->resolvedDecl->name));
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("resolvedDecl") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (expr->hasType()) {
        oss << indent(5) << quote("resolvedType") << ": " << serializeType(expr->resolvedType);
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("resolvedType") << ": " << jsonNull();
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

// ─── Array Literal Expr ───────────────────────────────────────────

std::string JSONDumper::serializeArrayLiteralExpr(ArrayLiteralExprAST* expr) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("ArrayLiteralExpr");
    oss << ",";
    if (pretty) oss << "\n";
    
    // Elements
    oss << indent(5) << quote("elements") << ": [";
    for (size_t i = 0; i < expr->elements.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << serializeExpr(expr->elements[i]);
    }
    oss << "]";
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
    } else {
        oss << indent(5) << quote("resolvedType") << ": " << jsonNull();
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

// ─── Struct Literal Expr ─────────────────────────────────────────

std::string JSONDumper::serializeStructLiteralExpr(StructLiteralExprAST* expr) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("StructLiteralExpr");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("typeName") << ": " << quote(str(expr->typeName));
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
    
    // Field inits
    oss << indent(5) << quote("inits") << ": [";
    if (pretty) oss << "\n";
    indentLevel = 6;
    for (size_t i = 0; i < expr->inits.size(); ++i) {
        if (i > 0) {
            oss << ",";
            if (pretty) oss << "\n";
        }
        oss << serializeFieldInit(expr->inits[i]);
    }
    indentLevel = 5;
    if (pretty) oss << "\n" << indent(5);
    oss << "]";
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
    } else {
        oss << indent(5) << quote("resolvedType") << ": " << jsonNull();
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

// ─── Field Init ─────────────────────────────────────────────────

std::string JSONDumper::serializeFieldInit(FieldInitAST* init) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 7;
    
    oss << indent(7) << quote("kind") << ": " << quote("FieldInit");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(7) << quote("name") << ": " << quote(str(init->name));
    oss << ",";
    if (pretty) oss << "\n";
    
    if (init->value) {
        oss << indent(7) << quote("value") << ": " << serializeExpr(init->value);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(7) << quote("location") << ": " << serializeLocation(init->loc);
    
    oss << "\n";
    indentLevel = 6;
    oss << indent(6) << "}";
    
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
    
    if (expr->left) {
        oss << indent(5) << quote("left") << ": " << serializeExpr(expr->left);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (expr->right) {
        oss << indent(5) << quote("right") << ": " << serializeExpr(expr->right);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    // Semantic fields
    oss << indent(5) << quote("isConst") << ": " << jsonBool(expr->isConst);
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->hasType()) {
        oss << indent(5) << quote("resolvedType") << ": " << serializeType(expr->resolvedType);
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("resolvedType") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("valueState") << ": " << quote(valueStateToString(expr->valueState));
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

// ─── Unary Expr ─────────────────────────────────────────────────

std::string JSONDumper::serializeUnaryExpr(UnaryExprAST* expr) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("UnaryExpr");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("op") << ": " << quote(unaryOpToString(expr->op));
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->operand) {
        oss << indent(5) << quote("operand") << ": " << serializeExpr(expr->operand);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    // Semantic fields
    oss << indent(5) << quote("isConst") << ": " << jsonBool(expr->isConst);
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->hasType()) {
        oss << indent(5) << quote("resolvedType") << ": " << serializeType(expr->resolvedType);
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("resolvedType") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("valueState") << ": " << quote(valueStateToString(expr->valueState));
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

// ─── Call Expr ─────────────────────────────────────────────────

std::string JSONDumper::serializeCallExpr(CallExprAST* expr) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("CallExpr");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->callee) {
        oss << indent(5) << quote("callee") << ": " << serializeExpr(expr->callee);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    // Generic args
    oss << indent(5) << quote("genericArgs") << ": [";
    for (size_t i = 0; i < expr->genericArgs.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << serializeType(expr->genericArgs[i]);
    }
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    // Arguments
    oss << indent(5) << quote("args") << ": [";
    for (size_t i = 0; i < expr->args.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << serializeExpr(expr->args[i]);
    }
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("hasArgPack") << ": " << jsonBool(expr->hasArgPack);
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
    } else {
        oss << indent(5) << quote("resolvedType") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("valueState") << ": " << quote(valueStateToString(expr->valueState));
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

// ─── Intrinsic Call Expr ──────────────────────────────────────

std::string JSONDumper::serializeIntrinsicCallExpr(IntrinsicCallExprAST* expr) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("IntrinsicCallExpr");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("intrinsicName") << ": " << quote(str(expr->intrinsicName));
    oss << ",";
    if (pretty) oss << "\n";
    
    // Arguments
    oss << indent(5) << quote("args") << ": [";
    for (size_t i = 0; i < expr->args.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << serializeExpr(expr->args[i]);
    }
    oss << "]";
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
    } else {
        oss << indent(5) << quote("resolvedType") << ": " << jsonNull();
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

// ─── Index Expr ────────────────────────────────────────────────

std::string JSONDumper::serializeIndexExpr(IndexExprAST* expr) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("IndexExpr");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->target) {
        oss << indent(5) << quote("target") << ": " << serializeExpr(expr->target);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (expr->index) {
        oss << indent(5) << quote("index") << ": " << serializeExpr(expr->index);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    // Semantic fields
    oss << indent(5) << quote("isConst") << ": " << jsonBool(expr->isConst);
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->hasType()) {
        oss << indent(5) << quote("resolvedType") << ": " << serializeType(expr->resolvedType);
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("resolvedType") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("valueState") << ": " << quote(valueStateToString(expr->valueState));
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

// ─── Slice Expr ────────────────────────────────────────────────

std::string JSONDumper::serializeSliceExpr(SliceExprAST* expr) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("SliceExpr");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->target) {
        oss << indent(5) << quote("target") << ": " << serializeExpr(expr->target);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (expr->start) {
        oss << indent(5) << quote("start") << ": " << serializeExpr(expr->start);
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("start") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (expr->end) {
        oss << indent(5) << quote("end") << ": " << serializeExpr(expr->end);
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("end") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("isExclusive") << ": " << jsonBool(expr->isExclusive);
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
    } else {
        oss << indent(5) << quote("resolvedType") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("valueState") << ": " << quote(valueStateToString(expr->valueState));
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

// ─── Field Access Expr ─────────────────────────────────────────

std::string JSONDumper::serializeFieldAccessExpr(FieldAccessExprAST* expr) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("FieldAccessExpr");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->object) {
        oss << indent(5) << quote("object") << ": " << serializeExpr(expr->object);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("fieldName") << ": " << quote(str(expr->fieldName));
    oss << ",";
    if (pretty) oss << "\n";
    
    // Semantic fields
    if (expr->resolvedDecl) {
        oss << indent(5) << quote("resolvedDecl") << ": " << quote(str(expr->resolvedDecl->name));
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("resolvedDecl") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (expr->ownerType) {
        oss << indent(5) << quote("ownerType") << ": " << quote(str(expr->ownerType->name));
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("ownerType") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("isEnumAccess") << ": " << jsonBool(expr->isEnumAccess);
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->fieldIndex != SIZE_MAX) {
        oss << indent(5) << quote("fieldIndex") << ": " << jsonNumber(static_cast<uint64_t>(expr->fieldIndex));
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("fieldIndex") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("isConst") << ": " << jsonBool(expr->isConst);
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->hasType()) {
        oss << indent(5) << quote("resolvedType") << ": " << serializeType(expr->resolvedType);
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("resolvedType") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("valueState") << ": " << quote(valueStateToString(expr->valueState));
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

// ─── Module Access Expr ─────────────────────────────────────────

std::string JSONDumper::serializeModuleAccessExpr(ModuleAccessExprAST* expr) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("ModuleAccessExpr");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("moduleName") << ": " << quote(str(expr->moduleName));
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("memberName") << ": " << quote(str(expr->memberName));
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
    if (expr->resolvedDecl) {
        oss << indent(5) << quote("resolvedDecl") << ": " << quote(str(expr->resolvedDecl->name));
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("resolvedDecl") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("resolved") << ": " << jsonBool(expr->resolvedDecl != nullptr);
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("isConst") << ": " << jsonBool(expr->isConst);
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->hasType()) {
        oss << indent(5) << quote("resolvedType") << ": " << serializeType(expr->resolvedType);
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("resolvedType") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("valueState") << ": " << quote(valueStateToString(expr->valueState));
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

// ─── Assign Expr ─────────────────────────────────────────────────

std::string JSONDumper::serializeAssignExpr(AssignExprAST* expr) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("AssignExpr");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("op") << ": " << quote(assignOpToString(expr->op));
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->lhs) {
        oss << indent(5) << quote("lhs") << ": " << serializeExpr(expr->lhs);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (expr->rhs) {
        oss << indent(5) << quote("rhs") << ": " << serializeExpr(expr->rhs);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    // Semantic fields
    oss << indent(5) << quote("isConst") << ": " << jsonBool(expr->isConst);
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->hasType()) {
        oss << indent(5) << quote("resolvedType") << ": " << serializeType(expr->resolvedType);
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("resolvedType") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("valueState") << ": " << quote(valueStateToString(expr->valueState));
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

// ─── Null Coalesce Expr ─────────────────────────────────────────

std::string JSONDumper::serializeNullCoalesceExpr(NullCoalesceExprAST* expr) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("NullCoalesceExpr");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->value) {
        oss << indent(5) << quote("value") << ": " << serializeExpr(expr->value);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (expr->fallback) {
        oss << indent(5) << quote("fallback") << ": " << serializeExpr(expr->fallback);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    // Semantic fields
    oss << indent(5) << quote("isConst") << ": " << jsonBool(expr->isConst);
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->hasType()) {
        oss << indent(5) << quote("resolvedType") << ": " << serializeType(expr->resolvedType);
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("resolvedType") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("valueState") << ": " << quote(valueStateToString(expr->valueState));
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

// ─── Pipeline Expr ──────────────────────────────────────────────

std::string JSONDumper::serializePipelineExpr(PipelineExprAST* expr) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("PipelineExpr");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->seed) {
        oss << indent(5) << quote("seed") << ": " << serializeExpr(expr->seed);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    // Steps
    oss << indent(5) << quote("steps") << ": [";
    if (pretty) oss << "\n";
    indentLevel = 6;
    for (size_t i = 0; i < expr->steps.size(); ++i) {
        if (i > 0) {
            oss << ",";
            if (pretty) oss << "\n";
        }
        oss << serializePipelineStep(expr->steps[i]);
    }
    indentLevel = 5;
    if (pretty) oss << "\n" << indent(5);
    oss << "]";
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
    } else {
        oss << indent(5) << quote("resolvedType") << ": " << jsonNull();
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

// ─── Pipeline Step ──────────────────────────────────────────────

std::string JSONDumper::serializePipelineStep(PipelineStepAST* step) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 7;
    
    oss << indent(7) << quote("kind") << ": " << quote("PipelineStep");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (step->callable) {
        oss << indent(7) << quote("callable") << ": " << serializeExpr(step->callable);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    // Pack args
    oss << indent(7) << quote("packArgs") << ": [";
    for (size_t i = 0; i < step->packArgs.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << serializeExpr(step->packArgs[i]);
    }
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(7) << quote("location") << ": " << serializeLocation(step->loc);
    
    oss << "\n";
    indentLevel = 6;
    oss << indent(6) << "}";
    
    return oss.str();
}

// ─── Compose Expr ───────────────────────────────────────────────

std::string JSONDumper::serializeComposeExpr(ComposeExprAST* expr) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("ComposeExpr");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->left) {
        oss << indent(5) << quote("left") << ": " << serializeExpr(expr->left);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    // Operands
    oss << indent(5) << quote("operands") << ": [";
    if (pretty) oss << "\n";
    indentLevel = 6;
    for (size_t i = 0; i < expr->operands.size(); ++i) {
        if (i > 0) {
            oss << ",";
            if (pretty) oss << "\n";
        }
        oss << serializeComposeOperand(expr->operands[i]);
    }
    indentLevel = 5;
    if (pretty) oss << "\n" << indent(5);
    oss << "]";
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
    } else {
        oss << indent(5) << quote("resolvedType") << ": " << jsonNull();
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

// ─── Compose Operand ────────────────────────────────────────────

std::string JSONDumper::serializeComposeOperand(ComposeOperandAST* operand) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 7;
    
    oss << indent(7) << quote("kind") << ": " << quote("ComposeOperand");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (operand->callable) {
        oss << indent(7) << quote("callable") << ": " << serializeExpr(operand->callable);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    // Generic args
    oss << indent(7) << quote("genericArgs") << ": [";
    for (size_t i = 0; i < operand->genericArgs.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << serializeType(operand->genericArgs[i]);
    }
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(7) << quote("location") << ": " << serializeLocation(operand->loc);
    
    oss << "\n";
    indentLevel = 6;
    oss << indent(6) << "}";
    
    return oss.str();
}

// ─── Anon Func Expr ─────────────────────────────────────────────

std::string JSONDumper::serializeAnonFuncExpr(AnonFuncExprAST* expr) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("AnonFuncExpr");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->funcType) {
        oss << indent(5) << quote("funcType") << ": " << serializeFuncType(expr->funcType);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (expr->body) {
        oss << indent(5) << quote("body") << ": " << serializeStmt(expr->body);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    // Semantic fields
    oss << indent(5) << quote("hasClosure") << ": " << jsonBool(expr->hasClosure);
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("isReturned") << ": " << jsonBool(expr->isReturned);
    oss << ",";
    if (pretty) oss << "\n";
    
    // Captures
    oss << indent(5) << quote("captures") << ": [";
    if (pretty) oss << "\n";
    indentLevel = 6;
    for (size_t i = 0; i < expr->captures.size(); ++i) {
        if (i > 0) {
            oss << ",";
            if (pretty) oss << "\n";
        }
        const CapturedVariable& cap = expr->captures[i];
        oss << indent(6) << "{";
        oss << quote("decl") << ": " << quote(str(cap.decl->name)) << ", ";
        oss << quote("byReference") << ": " << jsonBool(cap.byReference) << ", ";
        oss << quote("index") << ": " << jsonNumber(static_cast<uint64_t>(cap.index));
        oss << "}";
    }
    indentLevel = 5;
    if (pretty) oss << "\n" << indent(5);
    oss << "]";
    oss << ",";
    if (pretty) oss << "\n";
    
    // Semantic expression fields
    oss << indent(5) << quote("isConst") << ": " << jsonBool(expr->isConst);
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->hasType()) {
        oss << indent(5) << quote("resolvedType") << ": " << serializeType(expr->resolvedType);
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("resolvedType") << ": " << jsonNull();
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

// ─── If Expr ────────────────────────────────────────────────────

std::string JSONDumper::serializeIfExpr(IfExprAST* expr) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("IfExpr");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->condition) {
        oss << indent(5) << quote("condition") << ": " << serializeExpr(expr->condition);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (expr->thenBranch) {
        oss << indent(5) << quote("thenBranch") << ": " << serializeExpr(expr->thenBranch);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (expr->elseBranch) {
        oss << indent(5) << quote("elseBranch") << ": " << serializeExpr(expr->elseBranch);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    // Semantic fields
    oss << indent(5) << quote("isConst") << ": " << jsonBool(expr->isConst);
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->hasType()) {
        oss << indent(5) << quote("resolvedType") << ": " << serializeType(expr->resolvedType);
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("resolvedType") << ": " << jsonNull();
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

// ─── Range Expr ─────────────────────────────────────────────────

std::string JSONDumper::serializeRangeExpr(RangeExprAST* expr) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("RangeExpr");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (expr->lo) {
        oss << indent(5) << quote("lo") << ": " << serializeExpr(expr->lo);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    if (expr->hi) {
        oss << indent(5) << quote("hi") << ": " << serializeExpr(expr->hi);
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("isExclusive") << ": " << jsonBool(expr->isExclusive);
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
    } else {
        oss << indent(5) << quote("resolvedType") << ": " << jsonNull();
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









// ─── Type Serializers ─────────────────────────────────────────────────

std::string JSONDumper::serializeType(TypeAST* type) {
    if (!type) return jsonNull();
    
    switch (type->kind) {
        case ASTKind::PrimitiveType:    return serializePrimitiveType(type->as<PrimitiveTypeAST>());
        case ASTKind::NamedType:        return serializeNamedType(type->as<NamedTypeAST>());
        case ASTKind::ModuleTypeAccess: return serializeModuleTypeAccess(type->as<ModuleTypeAccessAST>());
        case ASTKind::ArrayType:        return serializeArrayType(type->as<ArrayTypeAST>());
        case ASTKind::NullableType:     return serializeNullableType(type->as<NullableTypeAST>());
        case ASTKind::FallibleType:     return serializeFallibleType(type->as<FallibleTypeAST>());
        case ASTKind::CombinedType:     return serializeCombinedType(type->as<CombinedTypeAST>());
        case ASTKind::RefType:          return serializeRefType(type->as<RefTypeAST>());
        case ASTKind::PtrType:          return serializePtrType(type->as<PtrTypeAST>());
        case ASTKind::FuncType:         return serializeFuncType(type->as<FuncTypeAST>());
        case ASTKind::FutureType:       return serializeFutureType(type->as<FutureTypeAST>());
        case ASTKind::ThreadType:       return serializeThreadType(type->as<ThreadTypeAST>());
        default:return serializeNode(type);
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
    
    // Semantic field: resolved declaration
    if (type->resolvedDecl) {
        oss << indent(5) << quote("resolvedDecl") << ": " << quote(str(type->resolvedDecl->name));
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("resolvedDecl") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(type->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Module Type Access ─────────────────────────────────────────

std::string JSONDumper::serializeModuleTypeAccess(ModuleTypeAccessAST* type) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("ModuleTypeAccess");
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("moduleName") << ": " << quote(str(type->moduleName));
    oss << ",";
    if (pretty) oss << "\n";
    
    oss << indent(5) << quote("typeName") << ": " << quote(str(type->typeName));
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
    
    if (type->element) {
        oss << indent(5) << quote("element") << ": " << serializeType(type->element);
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("element") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
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
    
    if (type->inner) {
        oss << indent(5) << quote("inner") << ": " << serializeType(type->inner);
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("inner") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(type->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Fallible Type ──────────────────────────────────────────────

std::string JSONDumper::serializeFallibleType(FallibleTypeAST* type) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("FallibleType");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (type->inner) {
        oss << indent(5) << quote("inner") << ": " << serializeType(type->inner);
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("inner") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(type->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Combined Type ──────────────────────────────────────────────

std::string JSONDumper::serializeCombinedType(CombinedTypeAST* type) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("CombinedType");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (type->inner) {
        oss << indent(5) << quote("inner") << ": " << serializeType(type->inner);
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("inner") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(type->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Ref Type ────────────────────────────────────────────────────

std::string JSONDumper::serializeRefType(RefTypeAST* type) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("RefType");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (type->inner) {
        oss << indent(5) << quote("inner") << ": " << serializeType(type->inner);
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("inner") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(type->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Ptr Type ────────────────────────────────────────────────────

std::string JSONDumper::serializePtrType(PtrTypeAST* type) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("PtrType");
    oss << ",";
    if (pretty) oss << "\n";
    
    if (type->inner) {
        oss << indent(5) << quote("inner") << ": " << serializeType(type->inner);
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("inner") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
    oss << indent(5) << quote("location") << ": " << serializeLocation(type->loc);
    
    oss << "\n";
    indentLevel = 4;
    oss << indent(4) << "}";
    
    return oss.str();
}

// ─── Func Type ──────────────────────────────────────────────────

std::string JSONDumper::serializeFuncType(FuncTypeAST* type) {
    std::ostringstream oss;
    oss << "{";
    if (pretty) oss << "\n";
    indentLevel = 5;
    
    oss << indent(5) << quote("kind") << ": " << quote("FuncType");
    oss << ",";
    if (pretty) oss << "\n";
    
    // Params
    oss << indent(5) << quote("params") << ": [";
    if (pretty) oss << "\n";
    indentLevel = 6;
    for (size_t i = 0; i < type->params.size(); ++i) {
        if (i > 0) {
            oss << ",";
            if (pretty) oss << "\n";
        }
        // Use serializeParam for each parameter
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
    } else {
        oss << indent(5) << quote("returnType") << ": " << jsonNull();
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
    
    if (type->inner) {
        oss << indent(5) << quote("inner") << ": " << serializeType(type->inner);
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("inner") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
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
    
    if (type->inner) {
        oss << indent(5) << quote("inner") << ": " << serializeType(type->inner);
        oss << ",";
        if (pretty) oss << "\n";
    } else {
        oss << indent(5) << quote("inner") << ": " << jsonNull();
        oss << ",";
        if (pretty) oss << "\n";
    }
    
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