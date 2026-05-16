// /**
//  * @file SemanticHelpers.hpp
//  *
//  * @nutshell Shared utility functions for semantic analysis passes (Phases 1-3).
//  *
//  * @reason Eliminates code duplication across SemanticDecl.cpp, SemanticStmt.cpp,
//  *   and SemanticExpr.cpp by providing a single source of truth for primitive
//  *   type singletons, type cloning, and function signature building.
//  *
//  * @responsibility Centralized helper functions that don't logically belong to
//  *   TypeResolver but are needed across multiple semantic files.
//  *
//  * @usage Include this header in SemanticDecl.cpp, SemanticStmt.cpp, and
//  *   SemanticExpr.cpp where shared functionality is needed.
//  *
//  * @design_principles
//  *   1. All functions are inline to avoid linking issues across TU boundaries
//  *   2. No dependencies on TypeResolver or other components that would create cycles
//  *   3. Primitive type singletons are static locals for thread-safe lazy initialization
//  *
//  * @related SemanticDecl.cpp, SemanticStmt.cpp, SemanticExpr.cpp, TypeResolver.cpp
//  */

// #pragma once

// #include "SymbolTable.hpp"
// #include "ast/TypeAST.hpp"
// #include "ast/StmtAST.hpp"
// #include "ast/DeclAST.hpp"
// #include "ast/ExprAST.hpp"
// #include "ast/support/ASTArena.hpp"
// #include "ast/support/StringPool.hpp"
// #include "debug/DebugMacros.hpp"
// #include "debug/DebugUtils.hpp"
// #include "diagnostics/DiagnosticEngine.hpp"
// #include "diagnostics/DiagnosticCodes.hpp"

// #include <string_view>


// namespace SemanticHelpers {

// // ─────────────────────────────────────────────────────────────────────────────
// // FunctionContext - Tracks the current function being analyzed for async validation
// // ─────────────────────────────────────────────────────────────────────────────

// class FunctionContext {
// public:
//     static FunctionContext& instance() {
//         static FunctionContext ctx;
//         return ctx;
//     }
    
//     void push(const std::string& name, Symbol* sym) {
//         LUC_LOG_SEMANTIC_VERBOSE("FunctionContext::push: " << name);
//         stack_.push_back(sym);
//         current_ = sym;
//     }
    
//     void pop() {
//         if (!stack_.empty()) {
//             stack_.pop_back();
//             current_ = stack_.empty() ? nullptr : stack_.back();
//             LUC_LOG_SEMANTIC_VERBOSE("FunctionContext::pop");
//         } else {
//             LUC_LOG_SEMANTIC("FunctionContext::pop: WARNING - stack empty");
//             current_ = nullptr;
//         }
//     }
    
//     Symbol* current() const { return current_; }
    
//     bool isInsideAsync() const {
//         if (!current_ || !current_->type) {
//             LUC_LOG_SEMANTIC_EXTREME("isInsideAsync: no current function -> false");
//             return false;
//         }
        
//         if (!current_->type->isa<FuncTypeAST>()) {
//             LUC_LOG_SEMANTIC_EXTREME("isInsideAsync: not a FuncTypeAST -> false");
//             return false;
//         }
        
//         bool isAsync = current_->type->as<FuncTypeAST>()->sig.isAsync();
//         LUC_LOG_SEMANTIC_EXTREME("isInsideAsync: " << current_->name.id 
//                                  << " -> " << (isAsync ? "true" : "false"));
//         return isAsync;
//     }
    
//     void clear() {
//         stack_.clear();
//         current_ = nullptr;
//     }
    
// private:
//     FunctionContext() = default;
//     Symbol* current_ = nullptr;
//     std::vector<Symbol*> stack_;
// };

// inline void pushFunction(const std::string& name, Symbol* sym) {
//     FunctionContext::instance().push(name, sym);
// }

// inline void popFunction() {
//     FunctionContext::instance().pop();
// }

// inline bool isInsideAsyncFunction() {
//     return FunctionContext::instance().isInsideAsync();
// }

// // ─────────────────────────────────────────────────────────────────────────────
// // Print utilities (require StringPool to resolve InternedString names)
// // ─────────────────────────────────────────────────────────────────────────────

// inline void printTypeAST(const std::string& label, TypeAST* t, const StringPool& pool, int indent = 0) {
//     if (!t) {
//         LUC_LOG_SEMANTIC_EXTREME(std::string(indent, ' ') << label << " = nullptr");
//         return;
//     }
    
//     std::string indentStr(indent, ' ');
    
//     switch (t->kind) {
//         case ASTKind::PrimitiveType: {
//             auto* p = t->as<PrimitiveTypeAST>();
//             std::string typeName;
//             switch (p->primitiveKind) {
//                 case PrimitiveKind::Bool:   typeName = "bool"; break;
//                 case PrimitiveKind::Int:    typeName = "int"; break;
//                 case PrimitiveKind::Float:  typeName = "float"; break;
//                 case PrimitiveKind::Double: typeName = "double"; break;
//                 case PrimitiveKind::String: typeName = "string"; break;
//                 case PrimitiveKind::Uint8:  typeName = "uint8"; break;
//                 case PrimitiveKind::Uint64: typeName = "uint64"; break;
//                 case PrimitiveKind::Any:    typeName = "any"; break;
//                 default: typeName = "other(" + std::to_string(static_cast<int>(p->primitiveKind)) + ")";
//             }
//             LUC_LOG_SEMANTIC_EXTREME(indentStr << label << " = PrimitiveType(" << typeName << ")");
//             break;
//         }
//         case ASTKind::NamedType: {
//             auto* n = t->as<NamedTypeAST>();
//             std::string msg = indentStr + label + " = NamedType(" + std::string(pool.lookup(n->name)) + ")";
//             if (n->isGenericParam) msg += " [generic param]";
//             LUC_LOG_SEMANTIC_EXTREME(msg);
//             break;
//         }
//         case ASTKind::NullableType: {
//             LUC_LOG_SEMANTIC_EXTREME(indentStr << label << " = NullableType");
//             printTypeAST("  inner", t->as<NullableTypeAST>()->inner.get(), pool, indent + 2);
//             break;
//         }
//         case ASTKind::PtrType: {
//             LUC_LOG_SEMANTIC_EXTREME(indentStr << label << " = PtrType");
//             printTypeAST("  inner", t->as<PtrTypeAST>()->inner.get(), pool, indent + 2);
//             break;
//         }
//         case ASTKind::FuncType: {
//             auto* f = t->as<FuncTypeAST>();
//             LUC_LOG_SEMANTIC_EXTREME(indentStr << label << " = FuncType");
//             LUC_LOG_SEMANTIC_EXTREME(indentStr << "  async=" << f->sig.isAsync()
//                                     << ", parallel=" << f->sig.isParallel()
//                                     << ", nullable=" << f->sig.isNullable());
//             break;
//         }
//         default:
//             LUC_LOG_SEMANTIC_EXTREME(indentStr << label << " = " << LucDebug::kindToString(t->kind));
//             break;
//     }
// }

// // ─────────────────────────────────────────────────────────────────────────────
// // Primitive Type Singletons
// // ─────────────────────────────────────────────────────────────────────────────

// inline PrimitiveTypeAST* getPrimitiveType(PrimitiveKind k) {
//     static PrimitiveTypeAST singletons[] = {
//         PrimitiveTypeAST(PrimitiveKind::Bool),
//         PrimitiveTypeAST(PrimitiveKind::Byte),
//         PrimitiveTypeAST(PrimitiveKind::Short),
//         PrimitiveTypeAST(PrimitiveKind::Int),
//         PrimitiveTypeAST(PrimitiveKind::Long),
//         PrimitiveTypeAST(PrimitiveKind::Ubyte),
//         PrimitiveTypeAST(PrimitiveKind::Ushort),
//         PrimitiveTypeAST(PrimitiveKind::Uint),
//         PrimitiveTypeAST(PrimitiveKind::Ulong),
//         PrimitiveTypeAST(PrimitiveKind::Int8),
//         PrimitiveTypeAST(PrimitiveKind::Int16),
//         PrimitiveTypeAST(PrimitiveKind::Int32),
//         PrimitiveTypeAST(PrimitiveKind::Int64),
//         PrimitiveTypeAST(PrimitiveKind::Uint8),
//         PrimitiveTypeAST(PrimitiveKind::Uint16),
//         PrimitiveTypeAST(PrimitiveKind::Uint32),
//         PrimitiveTypeAST(PrimitiveKind::Uint64),
//         PrimitiveTypeAST(PrimitiveKind::Float),
//         PrimitiveTypeAST(PrimitiveKind::Double),
//         PrimitiveTypeAST(PrimitiveKind::Decimal),
//         PrimitiveTypeAST(PrimitiveKind::String),
//         PrimitiveTypeAST(PrimitiveKind::Char),
//         PrimitiveTypeAST(PrimitiveKind::Any),
//     };
//     return &singletons[static_cast<int>(k)];
// }

// // ─────────────────────────────────────────────────────────────────────────────
// // Type Cloning with Arena Support
// // ─────────────────────────────────────────────────────────────────────────────

// // Clone a type using the arena (returns ASTPtr for arena ownership)
// template<typename T>
// inline ASTPtr<T> cloneType(const T* type, ASTArena& arena) {
//     if (!type) return nullptr;
    
//     LUC_LOG_SEMANTIC_EXTREME("cloneType (arena): kind=" << LucDebug::kindToString(type->kind));
    
//     switch (type->kind) {
//         case ASTKind::PrimitiveType: {
//             auto* p = static_cast<const PrimitiveTypeAST*>(type);
//             return arena.make<PrimitiveTypeAST>(p->primitiveKind);
//         }
        
//         case ASTKind::NamedType: {
//             auto* n = static_cast<const NamedTypeAST*>(type);
//             auto clone = arena.make<NamedTypeAST>(n->name);
//             clone->isGenericParam = n->isGenericParam;
//             for (auto& arg : n->genericArgs) {
//                 clone->genericArgs.push_back(cloneType(arg.get(), arena));
//             }
//             return clone;
//         }
        
//         case ASTKind::NullableType: {
//             auto* nl = static_cast<const NullableTypeAST*>(type);
//             return arena.make<NullableTypeAST>(cloneType(nl->inner.get(), arena));
//         }
        
//         case ASTKind::RefType: {
//             auto* r = static_cast<const RefTypeAST*>(type);
//             return arena.make<RefTypeAST>(cloneType(r->inner.get(), arena));
//         }
        
//         case ASTKind::PtrType: {
//             auto* p = static_cast<const PtrTypeAST*>(type);
//             return arena.make<PtrTypeAST>(cloneType(p->inner.get(), arena));
//         }
        
//         case ASTKind::FixedArrayType: {
//             auto* a = static_cast<const FixedArrayTypeAST*>(type);
//             return arena.make<FixedArrayTypeAST>(a->size, cloneType(a->element.get(), arena));
//         }
        
//         case ASTKind::SliceType: {
//             auto* s = static_cast<const SliceTypeAST*>(type);
//             return arena.make<SliceTypeAST>(cloneType(s->element.get(), arena));
//         }
        
//         case ASTKind::DynamicArrayType: {
//             auto* d = static_cast<const DynamicArrayTypeAST*>(type);
//             return arena.make<DynamicArrayTypeAST>(cloneType(d->element.get(), arena));
//         }
        
//         case ASTKind::FuncType: {
//             auto* f = static_cast<const FuncTypeAST*>(type);
//             auto clone = arena.make<FuncTypeAST>();
            
//             // Copy the signature
//             clone->sig = f->sig;
            
//             // Deep copy the param groups using arena
//             clone->sig.paramGroups.clear();
//             for (const auto& group : f->sig.paramGroups) {
//                 std::vector<ASTPtr<ParamAST>> newGroup;
//                 for (const auto& param : group) {
//                     if (param) {
//                         auto newParam = arena.make<ParamAST>();
//                         newParam->name = param->name;
//                         newParam->type = cloneType(param->type.get(), arena);
//                         newParam->isVariadic = param->isVariadic;
//                         newParam->loc = param->loc;
//                         newGroup.push_back(std::move(newParam));
//                     }
//                 }
//                 clone->sig.paramGroups.push_back(std::move(newGroup));
//             }
            
//             // Deep copy return types
//             clone->sig.returnTypes.clear();
//             for (const auto& retType : f->sig.returnTypes) {
//                 clone->sig.returnTypes.push_back(cloneType(retType.get(), arena));
//             }
            
//             // Copy qualifiers
//             clone->sig.qualifiers = f->sig.qualifiers;
//             clone->sig.rawQualifiers = f->sig.rawQualifiers;
            
//             return clone;
//         }
        
//         default:
//             LUC_LOG_SEMANTIC("cloneType (arena): unhandled kind " << static_cast<int>(type->kind));
//             return nullptr;
//     }
// }

// // ─────────────────────────────────────────────────────────────────────────────
// // FUNCTION SIGNATURE HELPERS (Unified for all function-like nodes)
// // ─────────────────────────────────────────────────────────────────────────────

// inline void declareFunctionParameters(FuncTypeAST& type, SymbolTable& symbols,
//                                        DiagnosticEngine& dc, const StringPool& pool) {
//     for (const auto& group : type.sig.paramGroups) {
//         for (const auto& param : group) {
//             if (!param) continue;
            
//             Symbol ps;
//             ps.name = param->name;
//             ps.kind = SymbolKind::Param;
//             ps.declKw = DeclKeyword::Let;
//             ps.visibility = Visibility::Private;
//             ps.type = param->type.get();
//             ps.decl = param.get();
//             ps.loc = param->loc;
            
//             if (!symbols.declare(ps)) {
//                 std::string_view nameStr = pool.lookup(param->name);
//                 LUC_LOG_SEMANTIC("\tERROR: duplicate parameter '" << nameStr << "'");
//                 dc.error(DiagnosticCategory::Semantic, param->loc, DiagCode::E3005,
//                          "duplicate parameter name '" + std::string(nameStr) + "'");
//             }
//         }
//     }
// }

// // ─────────────────────────────────────────────────────────────────────────────
// // Other Helpers
// // ─────────────────────────────────────────────────────────────────────────────

// inline TypeAST* getExprType(const ExprAST* expr) {
//     if (!expr) return nullptr;
//     return static_cast<TypeAST*>(expr->resolvedType);
// }

// inline bool checkAssignable(TypeAST* from, TypeAST* to,
//                             const SourceLocation& loc,
//                             DiagnosticEngine& dc,
//                             bool reportError = true) {
//     LUC_LOG_SEMANTIC_VERBOSE("checkAssignable: from=" << (from ? LucDebug::kindToString(from->kind) : "null")
//                            << ", to=" << (to ? LucDebug::kindToString(to->kind) : "null"));

//     if (!from || !to) {
//         if (reportError) {
//             LUC_LOG_SEMANTIC("checkAssignable: null type");
//             dc.error(DiagnosticCategory::Semantic, loc, DiagCode::E3002,
//                      "type mismatch: cannot assign between incompatible types");
//         }
//         return false;
//     }

//     if (TypeChecker::isAssignable(from, to)) {
//         return true;
//     }

//     if (reportError) {
//         LUC_LOG_SEMANTIC("checkAssignable: type mismatch");
//         dc.error(DiagnosticCategory::Semantic, loc, DiagCode::E3002,
//                  "type mismatch: cannot assign from '" + 
//                  LucDebug::kindToString(from->kind) + "' to '" +
//                  LucDebug::kindToString(to->kind) + "'");
//     }
//     return false;
// }

// // ─────────────────────────────────────────────────────────────────────────────
// // InternedString Helpers (using StringPool)
// // ─────────────────────────────────────────────────────────────────────────────

// inline std::string internedToString(InternedString s, const StringPool& pool) {
//     return std::string(pool.lookup(s));
// }

// inline std::string_view internedToStringView(InternedString s, const StringPool& pool) {
//     return pool.lookup(s);
// }

// } // namespace SemanticHelpers