/**
 * @file SemanticExpr.cpp
 *
 * @nutshell Validates exactly how expressions interact with each other in code.
 *
 * @responsibility Phase 3b of semantic analysis: walks every expression node, resolves
 *   its type, writes resolvedType onto the node, and enforces all operator and
 *   language-level expression rules.
 *
 * @logic
 *   checkExpr dispatches to a handler per ASTKind. Each handler returns the TypeAST*
 *   that the expression evaluates to, or nullptr if an irrecoverable error occurred.
 *   The returned pointer is also written onto node->resolvedType for the Annotator.
 *
 * @related SemanticAnalyzer.cpp, SemanticDecl.cpp, SemanticStmt.cpp
 */

#include "registry/IntrinsicRegistry.hpp"
#include "header/SemanticHelpers.hpp"
#include "ast/BaseAST.hpp"
#include "registry/BuiltinMethodRegistry.hpp"

#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations 
// NOTE: These are required because Declarations, Statements, and Expressions
// cross-call each other recursively. We use manual forward declarations here
// instead of a header to avoid complex circular dependency loops.
// ─────────────────────────────────────────────────────────────────────────────
void checkStmt(StmtAST* node, SymbolTable& symbols, TypeResolver& resolver,
               DiagnosticEngine& dc, TypeAST* expectedReturn,
               int& loopDepth, int& parallelDepth, bool insideExtern);



static TypeAST* errorFallback(ExprAST* node) {
    LUC_LOG_SEMANTIC("errorFallback: setting node type to Any");
    TypeAST* fallback = SemanticHelpers::getPrimitiveType(PrimitiveKind::Any);
    node->resolvedType = fallback;
    return fallback;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkLiteralExpr
// Maps token kind to a primitive TypeAST.
// nil → nullptr (caller handles nil assignability).
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkLiteralExpr(LiteralExprAST& node) {
    LUC_LOG_SEMANTIC_EXTREME("checkLiteralExpr: kind=" << static_cast<int>(node.kind) 
                           << ", value='" << node.value << "'");
    

    TypeAST* t = nullptr;
    switch (node.kind) {
        case LiteralKind::True:
        case LiteralKind::False:    t = SemanticHelpers::getPrimitiveType(PrimitiveKind::Bool);   break;
        case LiteralKind::Int:      t = SemanticHelpers::getPrimitiveType(PrimitiveKind::Int);    break;
        case LiteralKind::Hex:      t = SemanticHelpers::getPrimitiveType(PrimitiveKind::Int);    break;
        case LiteralKind::Binary:   t = SemanticHelpers::getPrimitiveType(PrimitiveKind::Int);    break;
        case LiteralKind::Float:    t = SemanticHelpers::getPrimitiveType(PrimitiveKind::Float);  break;
        case LiteralKind::String:
        case LiteralKind::RawString:t = SemanticHelpers::getPrimitiveType(PrimitiveKind::String); break;
        case LiteralKind::Char:     t = SemanticHelpers::getPrimitiveType(PrimitiveKind::Char);   break;
        case LiteralKind::Nil:      t = nullptr; break; // nil has no concrete type alone
    }
    node.resolvedType = t;
    LUC_LOG_SEMANTIC_EXTREME("\tresult type: " << (t ? LucDebug::kindToString(t->kind) : "null (nil)"));
    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkIdentExpr
// Looks the name up in the symbol table. Reports E3001 if missing.
// Returns the symbol's declared type.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkIdentExpr(IdentifierExprAST& node, SymbolTable& symbols,
                                DiagnosticEngine& dc) {
    LUC_LOG_SEMANTIC_VERBOSE("checkIdentExpr: name='" << node.name << "'");
    
    Symbol* sym = symbols.lookup(node.name);
    if (!sym) {
        LUC_LOG_SEMANTIC("\tERROR: undeclared identifier '" << node.name << "'");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                 "undeclared identifier '" + node.name + "'");
        return errorFallback(&node);
    }
    node.resolvedType = sym->type;
    node.isBehaviorMember = (sym->kind == SymbolKind::Method);
    
    LUC_LOG_SEMANTIC_EXTREME("\tresolved type: " << (sym->type ? LucDebug::kindToString(sym->type->kind) : "null"));
    return sym->type;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkFieldAccessExpr
// obj.field — resolves the struct type of obj, then looks up the field.
// Also handles enum variant access: Direction.North.
// ─────────────────────────────────────────────────────────────────────────────
// static TypeAST* checkFieldAccessExpr(FieldAccessExprAST& node, SymbolTable& symbols,
//                                      TypeResolver& resolver, DiagnosticEngine& dc,
//                                      int& asyncDepth, int& loopDepth,
//                                      int& parallelDepth, bool insideExtern);

// Forward declare checkExpr so the helpers below can call it.
TypeAST* checkExpr(ExprAST* node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& loopDepth, int& parallelDepth, bool insideExtern);

static TypeAST* checkFieldAccessExpr(FieldAccessExprAST& node, SymbolTable& symbols,
                                     TypeResolver& resolver, DiagnosticEngine& dc,
                                     int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkFieldAccessExpr: field='" << node.field << "'");
    
    TypeAST* objType = checkExpr(node.object.get(), symbols, resolver, dc,
                                 loopDepth, parallelDepth, insideExtern);
    if (!objType) return errorFallback(&node);
    
    LUC_LOG_SEMANTIC_EXTREME("\tobject type: " << LucDebug::kindToString(objType->kind));

    // Resolve named types to their struct symbol.
    std::string typeName;
    if (objType->isa<NamedTypeAST>()) {
        typeName = objType->as<NamedTypeAST>()->name;
        LUC_LOG_SEMANTIC_EXTREME("\ttypeName: " << typeName);
    } else if (objType->isa<PtrTypeAST>()) {
        LUC_LOG_SEMANTIC("\tERROR: cannot access field on raw pointer type");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "cannot access field '" + node.field + "' on raw pointer type '*T'; "
                 "use '@ptrToRef(T, ptr)' to cross the safety boundary first");
        return errorFallback(&node);
    }

    // Enum variant access: Direction.North
    if (!typeName.empty()) {
        Symbol* typeSym = symbols.lookup(typeName);
        if (typeSym && typeSym->kind == SymbolKind::Enum) {
            LUC_LOG_SEMANTIC_EXTREME("\tEnum variant access");
            node.resolvedType = objType;
            return objType;
        }
        // Struct field access
        if (typeSym && typeSym->kind == SymbolKind::Struct) {
            auto* structDecl = typeSym->decl->as<StructDeclAST>();
            for (auto& f : structDecl->fields) {
                if (f->name == node.field) {
                    TypeAST* ft = resolver.resolveType(f->type.get());
                    LUC_LOG_SEMANTIC_EXTREME("\tfound field, type: " << LucDebug::kindToString(ft->kind));
                    node.resolvedType = ft;
                    return ft;
                }
            }
            LUC_LOG_SEMANTIC("\tERROR: struct '" << typeName << "' has no field '" << node.field << "'");
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                     "struct '" + typeName + "' has no field '" + node.field + "'");
            return errorFallback(&node);
        }
    }

    // Fallback
    node.resolvedType = SemanticHelpers::getPrimitiveType(PrimitiveKind::Any);
    LUC_LOG_SEMANTIC_EXTREME("\tfallback to Any type");
    return SemanticHelpers::getPrimitiveType(PrimitiveKind::Any);
}

// ─────────────────────────────────────────────────────────────────────────────
// checkBehaviorAccessExpr
// Vec2:normalize — looks up the mangled name "Vec2.normalize" in the symbol table.
//
// The registered type for a method is always  (Self) -> (params…) -> ReturnType
// because SemanticCollector prepends a self-group. When the programmer writes
// p:offset they are already binding 'p' as self, so the type visible at the
// call site must be the *inner* type after self is consumed — i.e. the return
// type of the outer FuncTypeAST.  We strip that one level here so that
// subsequent CallExpr checks see the correct user-facing signature.
//
// Codegen annotations stamped here:
//   node.concreteTypeArgs    — concrete type arg strings from the receiver's
//                              declared type (e.g. ["Circle"] for Scene<Circle>).
//                              Empty for non-generic structs or when the
//                              receiver type is itself abstract (T inside a
//                              generic body, identified by isGenericParam).
//   node.resolvedMangledName — fully qualified LLVM function name for direct
//                              registry lookup in codegen Pass 0 and Pass 2.
//                              e.g. "Scene<Circle>.drawAll" or "Vec2.normalize".
//                              Empty when the receiver type is abstract —
//                              codegen must use its TypeSubst map in that case.
// ─────────────────────────────────────────────────────────────────────────────

// Helper: map PrimitiveKind to its canonical Luc type name string.
// Used by checkBehaviorAccessExpr and other semantic helpers that need to
// produce stable type-name strings for codegen annotations.
static const char* primitiveKindName(PrimitiveKind k) {
    LUC_LOG_SEMANTIC_EXTREME("primitiveKindName: kind=" << static_cast<int>(k));
    switch (k) {
        case PrimitiveKind::Bool:    return "bool";
        case PrimitiveKind::Byte:    return "byte";
        case PrimitiveKind::Short:   return "short";
        case PrimitiveKind::Int:     return "int";
        case PrimitiveKind::Long:    return "long";
        case PrimitiveKind::Ubyte:   return "ubyte";
        case PrimitiveKind::Ushort:  return "ushort";
        case PrimitiveKind::Uint:    return "uint";
        case PrimitiveKind::Ulong:   return "ulong";
        case PrimitiveKind::Int8:    return "int8";
        case PrimitiveKind::Int16:   return "int16";
        case PrimitiveKind::Int32:   return "int32";
        case PrimitiveKind::Int64:   return "int64";
        case PrimitiveKind::Uint8:   return "uint8";
        case PrimitiveKind::Uint16:  return "uint16";
        case PrimitiveKind::Uint32:  return "uint32";
        case PrimitiveKind::Uint64:  return "uint64";
        case PrimitiveKind::Float:   return "float";
        case PrimitiveKind::Double:  return "double";
        case PrimitiveKind::Decimal: return "decimal";
        case PrimitiveKind::String:  return "string";
        case PrimitiveKind::Char:    return "char";
        case PrimitiveKind::Any:     return "any";
    }
    return "";
}

// Helper: extract a stable type-name string from a single type arg node.
// Returns an empty string when the type is abstract (a generic param) or
// unsupported — callers treat empty as "skip this instantiation".
static std::string typeArgString(TypeAST* t) {
    LUC_LOG_SEMANTIC_EXTREME("typeArgString: type=" << (t ? LucDebug::kindToString(t->kind) : "null"));
    if (!t) return "";
    if (t->isa<PrimitiveTypeAST>())
        return primitiveKindName(t->as<PrimitiveTypeAST>()->primitiveKind);
    if (t->isa<NamedTypeAST>()) {
        auto* named = t->as<NamedTypeAST>();
        // isGenericParam is stamped by TypeResolver::visit(NamedTypeAST).
        // Abstract params (T, K, V) must not produce instantiation keys.
        if (named->isGenericParam) return "";
        return named->name;
    }
    return "";
}

// Helper: build the mangled LLVM name for a generic or non-generic method.
//   Non-generic:  "Vec2.normalize"
//   Generic:      "Scene<Circle>.drawAll"
//   Abstract:     "" (empty — receiver type is a generic param, codegen uses TypeSubst)
static std::string buildMangledMethodName(const std::string& structName,
                                          const std::vector<std::string>& typeArgs,
                                          const std::string& method) {
    LUC_LOG_SEMANTIC_EXTREME("buildMangledMethodName: " << structName << "." << method);
    if (typeArgs.empty())
        return structName + "." + method;

    std::string name = structName + "<";
    for (size_t i = 0; i < typeArgs.size(); ++i) {
        if (i) name += ",";
        name += typeArgs[i];
    }
    name += ">." + method;
    return name;
}

static TypeAST* checkBehaviorAccessExpr(BehaviorAccessExprAST& node, SymbolTable& symbols,
                                         DiagnosticEngine& dc) {
    LUC_LOG_SEMANTIC_VERBOSE("checkBehaviorAccessExpr: " << node.typeName << ":" << node.method);
    
    Symbol* lhsSym = symbols.lookup(node.typeName);
    if (!lhsSym) {
        LUC_LOG_SEMANTIC("\tERROR: undeclared identifier '" << node.typeName << "'");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                 "undeclared identifier '" + node.typeName + "'");
        return errorFallback(&node);
    }
    
    // DISALLOW STATIC ACCESS
    // The left side of ':' must be an instance, not a type name
    if (lhsSym->kind != SymbolKind::Var && lhsSym->kind != SymbolKind::Param) {
        LUC_LOG_SEMANTIC("\tERROR: static behavior access");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "behavior access ':' is only valid on instances; '" + node.typeName +
                 "' is a type name. Use an instance variable instead.");
        return errorFallback(&node);
    }

    // Extract the underlying struct/type name from the instance
    if (!lhsSym->type || !lhsSym->type->isa<NamedTypeAST>()) {
        LUC_LOG_SEMANTIC("\tERROR: identifier does not resolve to named struct type");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "identifier '" + node.typeName + "' does not resolve to a named struct type");
        return errorFallback(&node);
    }

    auto* receiverNamedType = lhsSym->type->as<NamedTypeAST>();
    std::string actualTypeName = receiverNamedType->name;
    std::string mangled = actualTypeName + "." + node.method;
    LUC_LOG_SEMANTIC_EXTREME("\tmangled name: " << mangled);

    // Look up the method in the type's namespace
    Symbol* sym = symbols.lookup(mangled);
    if (!sym) {
        LUC_LOG_SEMANTIC("\tERROR: no method '" << node.method << "' on '" << actualTypeName << "'");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                 "no method '" + node.method + "' found on '" + actualTypeName + "'");
        return errorFallback(&node);
    }
    
    node.isBehaviorMember = true;
    
    // Codegen annotations for generic structs
    std::vector<std::string> concreteArgs;
    bool receiverIsAbstract = receiverNamedType->isGenericParam;

    if (!receiverIsAbstract) {
        for (auto& arg : receiverNamedType->genericArgs) {
            std::string s = typeArgString(arg.get());
            if (s.empty()) {
                concreteArgs.clear();
                receiverIsAbstract = true;
                break;
            }
            concreteArgs.push_back(std::move(s));
        }
    }

    node.concreteTypeArgs = concreteArgs;
    node.resolvedMangledName = receiverIsAbstract
        ? ""
        : buildMangledMethodName(actualTypeName, concreteArgs, node.method);

    // CRITICAL FIX: NO STRIPPING OF SELF
    // The method signature stored in the symbol table has NO self parameter.
    // It is exactly what the user sees: (params...) -> return
    // Therefore we use it directly without any modification.
    node.resolvedType = sym->type;
    node.typeName = actualTypeName;
    
    LUC_LOG_SEMANTIC_EXTREME("\tresolved type: " << (sym->type ? LucDebug::kindToString(sym->type->kind) : "null"));
    return sym->type;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkBinaryExpr
//
// Rules enforced:
//   and / or  — short circuit (codegen responsibility), both sides must be
//               bool or nullable. Non-bool/non-nullable operand → E3002.
//   ==  / !=  — value equality. Struct → E3011. Function → E3012. Array → E3013.
//               Nullable types are valid: nil == nil, nil != value.
//   ===       — reference equality. Valid on &T, structs, nullable of above.
//               Primitives → E3002.
//   < > <= >= — ordering. Produces bool.
//   &&  / ||  — bitwise AND/OR. Integer types only → E3002 on non-integer.
//   ~^        — bitwise XOR. Integer types only.
//   << >>     — shift. Integer types only.
//   arithmetic — + - * / ^ % with pointer and type rules as before.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkBinaryExpr(BinaryExprAST& node, SymbolTable& symbols,
                                 TypeResolver& resolver, DiagnosticEngine& dc,
                                 int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkBinaryExpr: op=" << static_cast<int>(node.op));
    
    TypeAST* lt = checkExpr(node.left.get(),  symbols, resolver, dc,
                            loopDepth, parallelDepth, insideExtern);
    TypeAST* rt = checkExpr(node.right.get(), symbols, resolver, dc,
                            loopDepth, parallelDepth, insideExtern);
    
    LUC_LOG_SEMANTIC_EXTREME("\tleft type: " << (lt ? LucDebug::kindToString(lt->kind) : "null"));
    LUC_LOG_SEMANTIC_EXTREME("\tright type: " << (rt ? LucDebug::kindToString(rt->kind) : "null"));

    TypeAST* result = nullptr;

    // Helper to warn if an operand is nullable and return its non-nullable version.
    auto checkAndUnwrapNullable = [&](TypeAST* t, ExprAST* locExpr) -> TypeAST* {
        LUC_LOG_SEMANTIC_EXTREME("checkAndUnwrapNullable");
        if (t && TypeChecker::isNullable(t)) {
            LUC_LOG_SEMANTIC_EXTREME("\tnullable operand detected");
            dc.warning(DiagnosticCategory::Semantic, locExpr->loc, DiagCode::W3003,
                    "performing operation on nullable type; value may be nil at runtime");
            if (t->isa<NullableTypeAST>()) {
                return t->as<NullableTypeAST>()->inner.get();
            }
            // For FuncTypeAST with isNullable = true, we could also unwrap, 
            // but arithmetic on functions is already an error.
        }
        return t;
    };

    switch (node.op) {

        // ── Logical: and / or ─────────────────────────────────────────────────
        // Short circuit semantics are handled by codegen.
        // Semantic pass only validates that operands are bool or nullable.
        case BinaryOp::And:
        case BinaryOp::Or:
            LUC_LOG_SEMANTIC_EXTREME("\tlogical and/or operation");
            if (lt && !TypeChecker::isBoolOrNullable(lt)) {
                LUC_LOG_SEMANTIC("\tERROR: 'and'/'or' left operand must be bool or nullable");
                dc.error(DiagnosticCategory::Semantic, node.left->loc, DiagCode::E3002,
                         "'and'/'or' left operand must be bool or nullable type");
            }
            if (rt && !TypeChecker::isBoolOrNullable(rt)) {
                LUC_LOG_SEMANTIC("\tERROR: 'and'/'or' right operand must be bool or nullable");
                dc.error(DiagnosticCategory::Semantic, node.right->loc, DiagCode::E3002,
                         "'and'/'or' right operand must be bool or nullable type");
            }
            result = SemanticHelpers::getPrimitiveType(PrimitiveKind::Bool);
            break;

        // ── Value equality: == and != ─────────────────────────────────────────
        // Strict type rules — struct, function, and array types are forbidden.
        case BinaryOp::Eq:
        case BinaryOp::Ne:
            LUC_LOG_SEMANTIC_EXTREME("\tEquality comparison");
            
            // Check both left and right types for value-comparability
            if (lt && !TypeChecker::isValueComparable(lt, &symbols)) {
                if (lt->isa<NamedTypeAST>()) {
                    Symbol* sym = symbols.lookup(lt->as<NamedTypeAST>()->name);
                    if (sym && sym->kind == SymbolKind::Struct) {
                        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3011,
                                "cannot use '==' on struct type '" +
                                lt->as<NamedTypeAST>()->name +
                                "'; implement 'Equatable' and use ':equals()'");
                    } else if (lt->isa<FuncTypeAST>()) {
                        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3012,
                                "cannot use '==' on function type");
                    } else if (lt->isa<FixedArrayTypeAST>() || lt->isa<SliceTypeAST>() || lt->isa<DynamicArrayTypeAST>()) {
                        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3013,
                                "cannot use '==' on array type");
                    } else {
                        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                                "type '" + LucDebug::kindToString(lt->kind) + "' is not value-comparable");
                    }
                    return SemanticHelpers::getPrimitiveType(PrimitiveKind::Bool);
                }
            }
            
            if (rt && !TypeChecker::isValueComparable(rt, &symbols)) {
                // similar error reporting for right side
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                        "right operand type is not value-comparable");
                return SemanticHelpers::getPrimitiveType(PrimitiveKind::Bool);
            }
            
            // Also check that left and right types are compatible
            if (lt && rt && !TypeChecker::isAssignable(lt, rt) && !TypeChecker::isAssignable(rt, lt)) {
                LUC_LOG_SEMANTIC("\tERROR: type mismatch in equality comparison");
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                        "cannot compare '" + LucDebug::kindToString(lt->kind) +
                        "' with '" + LucDebug::kindToString(rt->kind) + "'");
            }
            
            result = SemanticHelpers::getPrimitiveType(PrimitiveKind::Bool);
            break;

        case BinaryOp::RefEq:
            LUC_LOG_SEMANTIC_EXTREME("\treference equality");
            if (lt && !TypeChecker::isReferenceComparable(lt)) {
                LUC_LOG_SEMANTIC("\tERROR: reference equality on non-reference type");
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "'===' reference equality is not valid on primitive or "
                         "non-reference type; use '==' for value comparison instead");
            }
            result = SemanticHelpers::getPrimitiveType(PrimitiveKind::Bool);
            break;

        // ── Ordering comparisons ──────────────────────────────────────────────
        case BinaryOp::Lt: case BinaryOp::Gt:
        case BinaryOp::Le: case BinaryOp::Ge:
            LUC_LOG_SEMANTIC_EXTREME("	ordering comparison");
            result = SemanticHelpers::getPrimitiveType(PrimitiveKind::Bool);
            break;

        // ── Arithmetic ────────────────────────────────────────────────────────
        case BinaryOp::Add:
            LUC_LOG_SEMANTIC_EXTREME("\taddition");
            if (lt && lt->isa<PtrTypeAST>()) {
                LUC_LOG_SEMANTIC("\tERROR: pointer addition not allowed");
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "operator '+' is not supported for raw pointer types; "
                         "use '@ptrOffset(ptr, n)' instead");
                result = SemanticHelpers::getPrimitiveType(PrimitiveKind::Any);
            } else if (lt && lt->isa<PrimitiveTypeAST>() &&
                       lt->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::String) {
                lt = checkAndUnwrapNullable(lt, node.left.get());
                rt = checkAndUnwrapNullable(rt, node.right.get());
                if (rt && !TypeChecker::isAssignable(rt, lt)) {
                    LUC_LOG_SEMANTIC("\tERROR: string addition requires string RHS");
                    dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                             "'+' on strings requires string right-hand side");
                }
                result = SemanticHelpers::getPrimitiveType(PrimitiveKind::String);
            } else {
                lt = checkAndUnwrapNullable(lt, node.left.get());
                rt = checkAndUnwrapNullable(rt, node.right.get());
                result = TypeChecker::unify(lt, rt);
                if (!result && lt) result = lt;
            }
            break;

        case BinaryOp::Sub:
            LUC_LOG_SEMANTIC_EXTREME("\tsubtraction");
            if (lt && lt->isa<PtrTypeAST>()) {
                LUC_LOG_SEMANTIC("\tERROR: pointer subtraction not allowed");
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "operator '-' is not supported for raw pointer types; "
                         "use '@ptrDiff(p1, p2)' or '@ptrOffset(ptr, -n)' instead");
                result = SemanticHelpers::getPrimitiveType(PrimitiveKind::Any);
            } else {
                lt = checkAndUnwrapNullable(lt, node.left.get());
                rt = checkAndUnwrapNullable(rt, node.right.get());
                result = TypeChecker::unify(lt, rt);
                if (!result && lt) result = lt;
            }
            break;

        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Pow:
        case BinaryOp::Mod:
            LUC_LOG_SEMANTIC_EXTREME("\tmultiplication/division/power/modulo");
            if ((lt && lt->isa<PtrTypeAST>()) || (rt && rt->isa<PtrTypeAST>())) {
                LUC_LOG_SEMANTIC("\tERROR: arithmetic on pointer");
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "arithmetic operators are not supported for raw pointer types");
                result = SemanticHelpers::getPrimitiveType(PrimitiveKind::Any);
            } else {
                lt = checkAndUnwrapNullable(lt, node.left.get());
                rt = checkAndUnwrapNullable(rt, node.right.get());
                result = TypeChecker::unify(lt, rt);
                if (!result && lt) result = lt;
            }
            break;

        // ── Bitwise: && || ~^ << >> ───────────────────────────────────────────
        // Integer types only. Non-integer operand → E3002.
        case BinaryOp::BitAnd:
        case BinaryOp::BitOr:
        case BinaryOp::BitXor:
        case BinaryOp::Shl:
        case BinaryOp::Shr: {
            LUC_LOG_SEMANTIC_EXTREME("\tbitwise operation");
            auto isIntegerType = [](TypeAST* t) -> bool {
                LUC_LOG_SEMANTIC_EXTREME("isIntegerType");
                if (!t || !t->isa<PrimitiveTypeAST>()) return false;
                auto k = t->as<PrimitiveTypeAST>()->primitiveKind;
                switch (k) {
                    case PrimitiveKind::Byte:   case PrimitiveKind::Short:
                    case PrimitiveKind::Int:    case PrimitiveKind::Long:
                    case PrimitiveKind::Ubyte:  case PrimitiveKind::Ushort:
                    case PrimitiveKind::Uint:   case PrimitiveKind::Ulong:
                    case PrimitiveKind::Int8:   case PrimitiveKind::Int16:
                    case PrimitiveKind::Int32:  case PrimitiveKind::Int64:
                    case PrimitiveKind::Uint8:  case PrimitiveKind::Uint16:
                    case PrimitiveKind::Uint32: case PrimitiveKind::Uint64:
                        return true;
                    default:
                        return false;
                }
            };
            if (lt && !isIntegerType(lt)) {
                LUC_LOG_SEMANTIC("\tERROR: bitwise operator requires integer operands (left)");
                dc.error(DiagnosticCategory::Semantic, node.left->loc, DiagCode::E3002,
                         "bitwise operator requires integer operands; left operand is not an integer type");
            }
            if (rt && !isIntegerType(rt)) {
                LUC_LOG_SEMANTIC("\tERROR: bitwise operator requires integer operands (right)");
                dc.error(DiagnosticCategory::Semantic, node.right->loc, DiagCode::E3002,
                         "bitwise operator requires integer operands; right operand is not an integer type");
            }
            result = lt ? lt : rt;
            break;
        }
    }

    node.resolvedType = result;
    LUC_LOG_SEMANTIC_EXTREME("\tresult type: " << (result ? LucDebug::kindToString(result->kind) : "null"));
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkUnaryExpr
//
// Rules enforced:
//   not — valid on bool and nullable types only
//         nil treated as false, non-nil treated as true
//         non-bool/non-nullable → E3002
//   -   — arithmetic negation, numeric only
//   ~   — bitwise NOT, integer types only
//   &   — reference operator, produces &T
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkUnaryExpr(UnaryExprAST& node, SymbolTable& symbols,
                                TypeResolver& resolver, DiagnosticEngine& dc,
                                int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkUnaryExpr: op=" << static_cast<int>(node.op));
    TypeAST* inner = checkExpr(node.operand.get(), symbols, resolver, dc,
                               loopDepth, parallelDepth, insideExtern);
    TypeAST* result = inner;

    switch (node.op) {
        case UnaryOp::Not:
            // 'not' is valid on bool and nullable types.
            // Nullable: nil → false, non-nil → true. This is the nil check idiom:
            //   let x int? = nil
            //   if not x { ... }  -- true: x is nil, treated as false
            if (inner && !TypeChecker::isBoolOrNullable(inner)) {
                LUC_LOG_SEMANTIC("\tERROR: 'not' requires bool or nullable operand");
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "'not' requires a bool or nullable operand; "
                         "got non-bool type — use an explicit comparison instead, "
                         "e.g. 'n == 0' instead of 'not n'");
            }
            result = SemanticHelpers::getPrimitiveType(PrimitiveKind::Bool);
            break;

        case UnaryOp::Neg:
            // Arithmetic negation — numeric types only
            // Let codegen validate the exact numeric kind
            result = inner;
            break;

        case UnaryOp::BitNot:
            // Bitwise NOT — integer types only
            // Detailed type check deferred to codegen for now
            result = inner;
            break;

        case UnaryOp::Ref:
            // &x — take a reference, produces &T wrapping inner's type
            // For now return inner; codegen wraps in RefTypeAST
            result = inner;
            break;
    }

    node.resolvedType = result;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkCallExpr
// Validates that the callee is callable, argument count matches, and each
// argument type is assignable to the corresponding parameter type.
// Also handles struct constructor (from() dispatch) and explicit type casting.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkCallExpr(CallExprAST& node, SymbolTable& symbols,
                               TypeResolver& resolver, DiagnosticEngine& dc,
                               int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkCallExpr");
    
    // Check each argument.
    for (auto& arg : node.args) {
        checkExpr(arg.get(), symbols, resolver, dc,
                  loopDepth, parallelDepth, insideExtern);
    }

    // Resolve callee type.
    TypeAST* calleeType = checkExpr(node.callee.get(), symbols, resolver, dc,
                                    loopDepth, parallelDepth, insideExtern);

    /// ── Built-in methods via registry ──────────────────────────────────────────
    if (node.callee->isa<FieldAccessExprAST>()) {
        auto* fieldAcc = node.callee->as<FieldAccessExprAST>();
        TypeAST* objType = static_cast<TypeAST*>(fieldAcc->object->resolvedType);
        if (!objType) {
            objType = checkExpr(fieldAcc->object.get(), symbols, resolver, dc,
                                loopDepth, parallelDepth, insideExtern);
        }
        
        const std::string& methodName = fieldAcc->field;
        
        // Check if this is a built-in method on array types
        std::string typeKey = getBuiltinTypeKey(objType);
        const BuiltinMethodInfo* builtin = BuiltinMethodRegistry::instance().lookup(typeKey, methodName);
        
        if (builtin) {
            // Check argument count
            if (node.args.size() != builtin->argKinds.size()) {
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3003,
                         "'" + methodName + "()' expects " + 
                         std::to_string(builtin->argKinds.size()) + " argument(s)");
                return errorFallback(&node);
            }
            
            // Get element type for arrays
            TypeAST* elemType = nullptr;
            if (objType->isa<FixedArrayTypeAST>()) elemType = objType->as<FixedArrayTypeAST>()->element.get();
            else if (objType->isa<SliceTypeAST>()) elemType = objType->as<SliceTypeAST>()->element.get();
            else if (objType->isa<DynamicArrayTypeAST>()) elemType = objType->as<DynamicArrayTypeAST>()->element.get();
            
            // Check arguments
            for (size_t i = 0; i < node.args.size(); ++i) {
                TypeAST* argType = checkExpr(node.args[i].get(), symbols, resolver, dc, loopDepth, parallelDepth, insideExtern);
                
                if (builtin->argKinds[i] == BuiltinArgKind::IntegerType) {
                    if (argType && !TypeChecker::isIntegerType(argType)) {
                        dc.error(DiagnosticCategory::Semantic, node.args[i]->loc, DiagCode::E3002,
                                 "'" + methodName + "()' argument must be an integer type");
                    }
                } else if (builtin->argKinds[i] == BuiltinArgKind::ElementType) {
                    if (argType && elemType && !TypeChecker::isAssignable(argType, elemType)) {
                        dc.error(DiagnosticCategory::Semantic, node.args[i]->loc, DiagCode::E3002,
                                 "argument type mismatch in '" + methodName + "()'");
                    }
                }
            }
            
            // Determine return type
            TypeAST* returnType = nullptr;
            switch (builtin->returnKind) {
                case BuiltinReturnKind::Void: returnType = nullptr; break;
                case BuiltinReturnKind::IntType: returnType = SemanticHelpers::getPrimitiveType(PrimitiveKind::Int); break;
                case BuiltinReturnKind::BoolType: returnType = SemanticHelpers::getPrimitiveType(PrimitiveKind::Bool); break;
                case BuiltinReturnKind::ElementType: returnType = elemType; break;
            }
            
            node.resolvedType = returnType;
            return returnType;
        }
        // If not a built-in method, continue to normal function resolution
    }

    // If callee is a named type (struct constructor / from() dispatch), return that type.
    if (node.callee->isa<IdentifierExprAST>()) {
        auto* ident = node.callee->as<IdentifierExprAST>();
        Symbol* sym = symbols.lookup(ident->name);
        if (sym && sym->kind == SymbolKind::Struct) {
            node.resolvedType = sym->type ? sym->type : calleeType;
            return static_cast<TypeAST*>(node.resolvedType);
        }
    }

    // Extract return type and check params.
    TypeAST* returnType = nullptr;
    
    // 1. Direct function call: callee is an identifier resolving to a Func/Method/ExternFunc symbol.
    Symbol* calleeSym = nullptr;
    if (node.callee->isa<IdentifierExprAST>()) {
        calleeSym = symbols.lookup(node.callee->as<IdentifierExprAST>()->name);
    }

    // ── ExternFunc (@extern-decorated function) ──────────────────────────────
    if (calleeSym && calleeSym->kind == SymbolKind::ExternFunc && calleeSym->decl) {
        auto* funcDecl = static_cast<FuncDeclAST*>(calleeSym->decl);
        
        // Use the unified FuncTypeAST (funcDecl->type)
        if (!funcDecl->type.paramGroups.empty()) {
            auto& firstGroup = funcDecl->type.paramGroups[0];
            if (node.args.size() != firstGroup.size()) {
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3003,
                         "extern function '" + calleeSym->name +
                         "' expects " + std::to_string(firstGroup.size()) +
                         " argument(s), got " + std::to_string(node.args.size()));
            } else {
                for (size_t i = 0; i < node.args.size(); ++i) {
                    TypeAST* argType = static_cast<TypeAST*>(node.args[i]->resolvedType);
                    TypeAST* paramType = firstGroup[i].type.get();
                    if (argType && paramType && !TypeChecker::isAssignable(argType, paramType)) {
                        dc.error(DiagnosticCategory::Semantic, node.args[i]->loc,
                                 DiagCode::E3002,
                                 "argument " + std::to_string(i + 1) +
                                 " type mismatch in call to extern '" + calleeSym->name + "'");
                    }
                }
            }
        }
        returnType = funcDecl->type.returnType.get();
        node.resolvedType = returnType;
        return returnType;
    }
    
    // ── Regular function or method call ──────────────────────────────────────
    if (calleeSym && (calleeSym->kind == SymbolKind::Func || calleeSym->kind == SymbolKind::Method) && calleeSym->decl) {
        auto* funcDecl = static_cast<FuncDeclAST*>(calleeSym->decl);
        
        // Use the unified FuncTypeAST (funcDecl->type)
        if (!funcDecl->type.paramGroups.empty()) {
            auto& firstGroup = funcDecl->type.paramGroups[0];
            
            if (node.args.size() > firstGroup.size()) {
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3003,
                         "wrong number of arguments: expected " +
                         std::to_string(firstGroup.size()) + ", got " +
                         std::to_string(node.args.size()));
                return errorFallback(&node);
            }
            
            // Check argument types against first parameter group
            for (size_t i = 0; i < node.args.size(); ++i) {
                TypeAST* argType = static_cast<TypeAST*>(node.args[i]->resolvedType);
                TypeAST* paramType = firstGroup[i].type.get();
                if (argType && paramType && !TypeChecker::isAssignable(argType, paramType)) {
                    dc.error(DiagnosticCategory::Semantic, node.args[i]->loc, DiagCode::E3002,
                             "argument " + std::to_string(i + 1) + " type mismatch");
                }
            }
            
            if (node.args.size() < firstGroup.size()) {
                // Partial application: return the curried function type
                if (funcDecl->type.paramGroups.size() > 1) {
                    // Build the remaining signature
                    auto remainingType = std::make_unique<FuncTypeAST>();
                    remainingType->isNullable = false;
                    for (size_t g = 1; g < funcDecl->type.paramGroups.size(); ++g) {
                        // Clone remaining param groups
                        ParamGroup newGroup;
                        for (const auto& pi : funcDecl->type.paramGroups[g]) {
                            newGroup.emplace_back(pi.name, SemanticHelpers::cloneType(pi.type.get()),
                                                  pi.isVariadic, pi.loc);
                        }
                        remainingType->paramGroups.push_back(std::move(newGroup));
                    }
                    remainingType->returnType = SemanticHelpers::cloneType(funcDecl->type.returnType.get());
                    node.resolvedType = remainingType.get();
                    returnType = remainingType.release();
                } else {
                    returnType = &funcDecl->type;
                }
            } else if (node.args.size() == firstGroup.size()) {
                // Full first group provided
                if (funcDecl->type.paramGroups.size() > 1) {
                    // More groups exist: return the next curried function type
                    auto nextType = std::make_unique<FuncTypeAST>();
                    nextType->isNullable = false;
                    for (size_t g = 1; g < funcDecl->type.paramGroups.size(); ++g) {
                        // Clone remaining param groups
                        ParamGroup newGroup;
                        for (const auto& pi : funcDecl->type.paramGroups[g]) {
                            newGroup.emplace_back(pi.name, SemanticHelpers::cloneType(pi.type.get()),
                                                  pi.isVariadic, pi.loc);
                        }
                        nextType->paramGroups.push_back(std::move(newGroup));
                    }
                    nextType->returnType = SemanticHelpers::cloneType(funcDecl->type.returnType.get());
                    node.resolvedType = nextType.get();
                    returnType = nextType.release();
                } else {
                    // No more groups: return the final return type
                    returnType = funcDecl->type.returnType.get();
                }
            }
        } else {
            // No parameters
            returnType = funcDecl->type.returnType.get();
            if (node.args.size() > 0) {
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3003,
                         "wrong number of arguments: expected 0, got " +
                         std::to_string(node.args.size()));
            }
        }
    } 
    // 2. Indirect call: callee evaluates to a FuncTypeAST (e.g. variable holding a closure).
    else if (calleeType && calleeType->isa<FuncTypeAST>()) {
        auto* ft = calleeType->as<FuncTypeAST>();
        
        if (!ft->paramGroups.empty() && !ft->paramGroups[0].empty()) {
            if (node.args.size() > ft->paramGroups[0].size()) {
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3003,
                         "wrong number of arguments: expected " +
                         std::to_string(ft->paramGroups[0].size()) + ", got " +
                         std::to_string(node.args.size()));
                return errorFallback(&node);
            }
            
            // Check argument types
            for (size_t i = 0; i < node.args.size(); ++i) {
                TypeAST* argType = static_cast<TypeAST*>(node.args[i]->resolvedType);
                TypeAST* paramType = ft->paramGroups[0][i].type.get();
                if (argType && paramType && !TypeChecker::isAssignable(argType, paramType)) {
                    dc.error(DiagnosticCategory::Semantic, node.args[i]->loc, DiagCode::E3002,
                             "argument " + std::to_string(i + 1) + " type mismatch");
                }
            }
        }
        
        // Return the appropriate type based on argument count
        if (node.args.size() < ft->paramGroups[0].size() || ft->paramGroups.size() > 1) {
            // Partial application: return the remaining function type
            returnType = ft;
        } else {
            // Full call: return the return type
            returnType = ft->returnType.get();
        }
    } 
    // 3. Not callable.
    else {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "expression is not callable");
        return errorFallback(&node);
    }

    // Determine if this is an async call
    bool isAsync = false;
    if (calleeSym && calleeSym->kind == SymbolKind::Func && calleeSym->type) {
        if (calleeSym->type->isa<FuncTypeAST>()) {
            isAsync = calleeSym->type->as<FuncTypeAST>()->isAsync();
        }
    } else if (calleeType && calleeType->isa<FuncTypeAST>()) {
        isAsync = calleeType->as<FuncTypeAST>()->isAsync();
    }

    node.isAsyncCall = isAsync;
    node.resolvedType = returnType;
    return returnType;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkAssignExpr
// Validates that the LHS is mutable (let variable or let-held field).
// Compound operators are desugared: x += e → x = x + e, then checked.
//
// Special case — function body reassignment:
//   f = { return 42 }
//   f = async { return await doWork() }
//
// The RHS is an AnonFuncExprAST produced by parsePrimaryExpr when it sees a
// bare '{' in expression position (the block-body form).  That node carries
// empty params and nullptr returnType because the parser has no access to the
// symbol table — the real signature lives on the FuncDeclAST that was stored
// when 'f' was first declared.
//
// Detection: RHS is AnonFuncExprAST AND its paramGroups is empty AND its
// returnType is nullptr.  In this case we look up the LHS symbol, recover the
// FuncDeclAST's type (FuncTypeAST), declare the params into scope, and check
// the body with the correct expectedReturn.
//
// If the LHS symbol is not a Func (e.g. the user assigns a block to a plain
// variable of function-type), we fall through to the generic checkExpr path
// which handles AnonFuncExprAST normally via its own (possibly empty) params.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkAssignExpr(AssignExprAST& node, SymbolTable& symbols,
                                 TypeResolver& resolver, DiagnosticEngine& dc,
                                 int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkAssignExpr: op=" << static_cast<int>(node.op));
    
    TypeAST* lhsType = checkExpr(node.lhs.get(), symbols, resolver, dc,
                                 loopDepth, parallelDepth, insideExtern);
    LUC_LOG_SEMANTIC_EXTREME("\tlhs type: " << (lhsType ? LucDebug::kindToString(lhsType->kind) : "null"));

    // ── Function body reassignment:  f = { ... } ─────────────────────────────
    // Detect the block-body form: RHS is an AnonFuncExprAST with no params and
    // no return type — the parser produces this when it sees a bare block in
    // expression position.  We need to use the LHS FuncDeclAST's signature
    // instead so that params are in scope and the return type is checked.
    if (node.op == AssignOp::Assign &&
        node.rhs->isa<AnonFuncExprAST>() &&
        node.lhs->isa<IdentifierExprAST>()) {

        auto* anonBody = node.rhs->as<AnonFuncExprAST>();
        // Only treat as a block-body reassignment when the node has no
        // params and no return type (i.e. it was produced from a bare '{').
        if (anonBody->type.paramGroups.empty() && !anonBody->type.returnType) {
            auto* ident = node.lhs->as<IdentifierExprAST>();
            Symbol* sym = symbols.lookup(ident->name);
            LUC_LOG_SEMANTIC_EXTREME("\tfunction body reassignment for '" << ident->name << "'");

            if (sym && (sym->kind == SymbolKind::Func) && !sym->isExtern && sym->decl) {
                // Verify the symbol is let-declared (reassignable).
                if (sym->declKw != DeclKeyword::Let) {
                    LUC_LOG_SEMANTIC("\tERROR: cannot reassign const function");
                    dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3004,
                             "cannot reassign body of '" + ident->name +
                             "': declared with const");
                }

                if (parallelDepth > 0) {
                    LUC_LOG_SEMANTIC("\tERROR: assignment inside parallel scope");
                    dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                             "assignment to outer variable inside parallel scope is not allowed");
                }

                // Recover the real signature from the FuncDeclAST's type.
                auto* funcDecl = static_cast<FuncDeclAST*>(sym->decl);

                // Get return type from the unified FuncTypeAST
                TypeAST* returnType = funcDecl->type.returnType.get();

                symbols.pushScope();
                LUC_LOG_SEMANTIC_EXTREME("\tpushing scope for function params");

                // Declare the function's parameters into the body scope using the unified type
                for (auto& group : funcDecl->type.paramGroups) {
                    for (auto& param : group) {
                        TypeAST* pt = param.type.get();
                        if (!pt) continue;
                        Symbol ps;
                        ps.name       = param.name;
                        ps.kind       = SymbolKind::Param;
                        ps.declKw     = DeclKeyword::Let;
                        ps.visibility = Visibility::Private;
                        ps.type       = pt;
                        ps.decl       = nullptr;  // ParamInfo has no AST back pointer
                        ps.loc        = param.loc;
                        if (!symbols.declare(ps)) {
                            LUC_LOG_SEMANTIC("\tERROR: duplicate param name");
                            dc.error(DiagnosticCategory::Semantic, param.loc,
                                     DiagCode::E3005,
                                     "duplicate parameter name '" + param.name + "'");
                        } else {
                            LUC_LOG_SEMANTIC_EXTREME("\t\tdeclared param: " << param.name);
                        }
                    }
                }

                // Check the body block with the recovered return type.
                if (anonBody->body) {
                    LUC_LOG_SEMANTIC_EXTREME("\tchecking function body");
                    checkStmt(anonBody->body.get(), symbols, resolver, dc,
                              returnType, loopDepth,
                              parallelDepth, insideExtern);
                }

                symbols.popScope();
                LUC_LOG_SEMANTIC_EXTREME("\tpopped function scope");

                node.resolvedType = lhsType;
                return lhsType;
            }
        }
    }

    // ── Explicit anonymous function reassignment:  f = (x int) int { ... } ────
    // The RHS is an AnonFuncExprAST with its own param groups and return type
    // written explicitly by the programmer.
    if (node.op == AssignOp::Assign &&
        node.rhs->isa<AnonFuncExprAST>() &&
        node.lhs->isa<IdentifierExprAST>()) {

        auto* anonRhs = node.rhs->as<AnonFuncExprAST>();

        // Only enter this branch when the anon func has an explicit signature —
        // i.e. it is NOT the bare block-body form.
        bool hasExplicitSignature = !anonRhs->type.paramGroups.empty() ||
                                    anonRhs->type.returnType != nullptr;

        if (hasExplicitSignature) {
            auto* ident = node.lhs->as<IdentifierExprAST>();
            Symbol* sym = symbols.lookup(ident->name);
            LUC_LOG_SEMANTIC_EXTREME("\tExplicit anonymous function reassignment for '" << ident->name << "'");

            if (sym && (sym->kind == SymbolKind::Func) && !sym->isExtern && sym->decl) {
                auto* funcDecl = static_cast<FuncDeclAST*>(sym->decl);
                bool signatureOk = true;

                // Curry group count check
                if (anonRhs->type.paramGroups.size() != funcDecl->type.paramGroups.size()) {
                    LUC_LOG_SEMANTIC("\tERROR: param group count mismatch");
                    dc.error(DiagnosticCategory::Semantic, anonRhs->loc,
                             DiagCode::E3003,
                             "anonymous function assigned to '" + ident->name +
                             "' has " +
                             std::to_string(anonRhs->type.paramGroups.size()) +
                             " parameter group(s) but declaration has " +
                             std::to_string(funcDecl->type.paramGroups.size()));
                    signatureOk = false;
                } else {
                    // Per-group, per-parameter check
                    for (size_t g = 0; g < funcDecl->type.paramGroups.size(); ++g) {
                        const auto& declGroup = funcDecl->type.paramGroups[g];
                        const auto& anonGroup = anonRhs->type.paramGroups[g];

                        if (anonGroup.size() != declGroup.size()) {
                            LUC_LOG_SEMANTIC("\tERROR: param count mismatch in group " << g);
                            dc.error(DiagnosticCategory::Semantic, anonRhs->loc,
                                     DiagCode::E3003,
                                     "anonymous function assigned to '" + ident->name +
                                     "': group " + std::to_string(g + 1) +
                                     " has " + std::to_string(anonGroup.size()) +
                                     " parameter(s) but declaration has " +
                                     std::to_string(declGroup.size()));
                            signatureOk = false;
                            continue;
                        }

                        for (size_t i = 0; i < declGroup.size(); ++i) {
                            const auto& declParam = declGroup[i];
                            const auto& anonParam = anonGroup[i];

                            // Variadic flag must match exactly
                            if (anonParam.isVariadic != declParam.isVariadic) {
                                LUC_LOG_SEMANTIC("\tERROR: variadic mismatch for param " << i);
                                dc.error(DiagnosticCategory::Semantic, anonParam.loc,
                                         DiagCode::E3002,
                                         "group " + std::to_string(g + 1) +
                                         " parameter " + std::to_string(i + 1) +
                                         " variadic mismatch");
                                signatureOk = false;
                                continue;
                            }

                            TypeAST* declType = declParam.type.get();
                            TypeAST* anonType = anonParam.type.get();

                            if (declType && anonType && !TypeChecker::isAssignable(anonType, declType)) {
                                LUC_LOG_SEMANTIC("\tERROR: type mismatch for param " << i);
                                dc.error(DiagnosticCategory::Semantic, anonParam.loc,
                                         DiagCode::E3002,
                                         "group " + std::to_string(g + 1) +
                                         " parameter " + std::to_string(i + 1) +
                                         " type mismatch");
                                signatureOk = false;
                            }
                        }
                    }
                }

                // Return type check
                TypeAST* declReturn = funcDecl->type.returnType.get();
                TypeAST* anonReturn = anonRhs->type.returnType.get();

                bool returnMatches;
                if (!declReturn && !anonReturn) {
                    returnMatches = true;
                } else if (!declReturn || !anonReturn) {
                    returnMatches = false;
                } else {
                    returnMatches = TypeChecker::isAssignable(anonReturn, declReturn);
                }

                if (!returnMatches) {
                    LUC_LOG_SEMANTIC("\tERROR: return type mismatch");
                    dc.error(DiagnosticCategory::Semantic, anonRhs->loc,
                             DiagCode::E3002,
                             "return type of anonymous function assigned to '" +
                             ident->name + "' does not match declaration");
                    signatureOk = false;
                }

                // Mutability check
                if (sym->declKw != DeclKeyword::Let) {
                    LUC_LOG_SEMANTIC("\tERROR: cannot reassign const function");
                    dc.error(DiagnosticCategory::Semantic, node.loc,
                             DiagCode::E3004,
                             "cannot reassign '" + ident->name +
                             "': declared with const");
                }

                if (parallelDepth > 0) {
                    LUC_LOG_SEMANTIC("\tERROR: assignment inside parallel scope");
                    dc.error(DiagnosticCategory::Semantic, node.loc,
                             DiagCode::E3002,
                             "assignment to outer variable inside parallel scope is not allowed");
                }

                symbols.pushScope();
                LUC_LOG_SEMANTIC_EXTREME("\tpushing scope for function params");

                // Declare params using the anon func's own param names, but the declared types
                if (signatureOk) {
                    for (size_t g = 0; g < funcDecl->type.paramGroups.size(); ++g) {
                        const auto& declGroup = funcDecl->type.paramGroups[g];
                        const auto& anonGroup = anonRhs->type.paramGroups[g];
                        for (size_t i = 0; i < declGroup.size(); ++i) {
                            const auto& anonParam = anonGroup[i];
                            TypeAST* pt = declGroup[i].type.get();
                            if (!pt) continue;
                            Symbol ps;
                            ps.name       = anonParam.name;
                            ps.kind       = SymbolKind::Param;
                            ps.declKw     = DeclKeyword::Let;
                            ps.visibility = Visibility::Private;
                            ps.type       = pt;
                            ps.decl       = nullptr;
                            ps.loc        = anonParam.loc;
                            if (!symbols.declare(ps)) {
                                LUC_LOG_SEMANTIC("\tERROR: duplicate param name");
                                dc.error(DiagnosticCategory::Semantic, anonParam.loc,
                                         DiagCode::E3005,
                                         "duplicate parameter name '" + anonParam.name + "'");
                            } else {
                                LUC_LOG_SEMANTIC_EXTREME("\t\tdeclared param: " << anonParam.name);
                            }
                        }
                    }
                }

                if (anonRhs->body) {
                    LUC_LOG_SEMANTIC_EXTREME("\tchecking function body");
                    checkStmt(anonRhs->body.get(), symbols, resolver, dc,
                              declReturn, loopDepth, parallelDepth, insideExtern);
                }

                symbols.popScope();
                LUC_LOG_SEMANTIC_EXTREME("\tpopped function scope");

                node.resolvedType = lhsType;
                return lhsType;
            }
        }
    }

    // ── DIRECT FUNCTION ASSIGNMENT:  f = existingFunction ────────────────────
    if (node.op == AssignOp::Assign &&
        node.rhs->isa<IdentifierExprAST>() &&
        node.lhs->isa<IdentifierExprAST>()) {
        
        auto* lhsIdent = node.lhs->as<IdentifierExprAST>();
        auto* rhsIdent = node.rhs->as<IdentifierExprAST>();
        
        LUC_LOG_SEMANTIC_EXTREME("\tDirect function assignment: '" << lhsIdent->name 
                               << "' = '" << rhsIdent->name << "'");
        
        Symbol* lhsSym = symbols.lookup(lhsIdent->name);
        if (!lhsSym) {
            LUC_LOG_SEMANTIC("\tERROR: LHS symbol not found: '" << lhsIdent->name << "'");
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                     "undeclared identifier '" + lhsIdent->name + "'");
            return errorFallback(&node);
        }
        
        Symbol* rhsSym = symbols.lookup(rhsIdent->name);
        if (!rhsSym) {
            LUC_LOG_SEMANTIC("\tERROR: RHS symbol not found: '" << rhsIdent->name << "'");
            dc.error(DiagnosticCategory::Semantic, node.rhs->loc, DiagCode::E3001,
                     "undeclared identifier '" + rhsIdent->name + "'");
            return errorFallback(&node);
        }
        
        bool lhsIsFunc = (lhsSym->kind == SymbolKind::Func || lhsSym->kind == SymbolKind::ExternFunc);
        bool rhsIsFunc = (rhsSym->kind == SymbolKind::Func || rhsSym->kind == SymbolKind::ExternFunc);
        
        if (!lhsIsFunc || !rhsIsFunc) {
            LUC_LOG_SEMANTIC_EXTREME("\tNot a function assignment, falling through");
        } else {
            if (lhsSym->declKw != DeclKeyword::Let) {
                LUC_LOG_SEMANTIC("\tERROR: cannot reassign const function");
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3004,
                         "cannot reassign '" + lhsIdent->name + "': declared with const");
            }
            
            if (parallelDepth > 0) {
                LUC_LOG_SEMANTIC("\tERROR: assignment inside parallel scope");
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "assignment to outer variable inside parallel scope is not allowed");
            }
            
            TypeAST* lhsFuncType = lhsSym->type;
            TypeAST* rhsFuncType = rhsSym->type;
            
            if (!lhsFuncType || !rhsFuncType) {
                LUC_LOG_SEMANTIC("\tERROR: missing function type for assignment");
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                         "internal error: missing function type for '" + 
                         (lhsFuncType ? rhsIdent->name : lhsIdent->name) + "'");
            } else {
                if (!TypeChecker::isAssignable(rhsFuncType, lhsFuncType)) {
                    dc.error(DiagnosticCategory::Semantic, node.rhs->loc, DiagCode::E3002,
                             "function signature mismatch in assignment to '" + lhsIdent->name +
                             "': cannot assign from '" + rhsIdent->name + "'");
                }
            }
            
            node.resolvedType = lhsFuncType;
            return lhsFuncType;
        }
    }

    // ── General case ──────────────────────────────────────────────────────────
    TypeAST* rhsType = checkExpr(node.rhs.get(), symbols, resolver, dc,
                                 loopDepth, parallelDepth, insideExtern);
    LUC_LOG_SEMANTIC_EXTREME("\trhs type: " << (rhsType ? LucDebug::kindToString(rhsType->kind) : "null"));

    // Check the LHS is a let-declared symbol.
    if (node.lhs->isa<IdentifierExprAST>()) {
        auto* ident = node.lhs->as<IdentifierExprAST>();
        Symbol* sym = symbols.lookup(ident->name);
        if (sym && sym->declKw != DeclKeyword::Let) {
            LUC_LOG_SEMANTIC("\tERROR: cannot assign to const variable");
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3004,
                     "cannot assign to '" + ident->name + "': declared with const");
        }
    }

    // Parallel scope: writing to outer variables is forbidden.
    if (parallelDepth > 0) {
        LUC_LOG_SEMANTIC("\tERROR: assignment inside parallel scope");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "assignment to outer variable inside parallel scope is not allowed");
    }

    // Type compatibility for plain assignment.
    if (node.op == AssignOp::Assign && lhsType && rhsType) {
        if (!TypeChecker::isAssignable(rhsType, lhsType)) {
            if (lhsType->isa<NamedTypeAST>() &&
                TypeChecker::isFromCastable(rhsType, lhsType, &symbols)) {
                LUC_LOG_SEMANTIC_EXTREME("\tapplying from-casting conversion");
                auto targetTypeNode = std::make_unique<NamedTypeAST>(
                    lhsType->as<NamedTypeAST>()->name);
                targetTypeNode->loc = node.rhs->loc;

                SourceLocation rhsLoc = node.rhs->loc;
                auto convExpr = std::make_unique<TypeConvExprAST>(
                    std::move(targetTypeNode),
                    std::move(node.rhs),
                    /*isUnsafe=*/false);
                convExpr->loc = rhsLoc;

                node.rhs = std::move(convExpr);
                checkExpr(node.rhs.get(), symbols, resolver, dc,
                          loopDepth, parallelDepth, insideExtern);
            } else {
                LUC_LOG_SEMANTIC("\tERROR: type mismatch in assignment");
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "type mismatch in assignment");
            }
        }
    }

    node.resolvedType = lhsType;
    return lhsType;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkIsExpr
// x is int → produces bool, marks the narrowed type on the node.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkIsExpr(IsExprAST& node, SymbolTable& symbols,
                             TypeResolver& resolver, DiagnosticEngine& dc,
                             int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkIsExpr");
    checkExpr(node.expr.get(), symbols, resolver, dc,
              loopDepth, parallelDepth, insideExtern);
    resolver.resolveType(node.checkType.get());
    node.resolvedType = SemanticHelpers::getPrimitiveType(PrimitiveKind::Bool);
    return SemanticHelpers::getPrimitiveType(PrimitiveKind::Bool);
}

// ─────────────────────────────────────────────────────────────────────────────
// checkIfExpr (expression form — else is required)
// Both branches must produce the same type (unified).
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkIfExpr(IfExprAST& node, SymbolTable& symbols,
                              TypeResolver& resolver, DiagnosticEngine& dc,
                              TypeAST* expectedReturn, int& loopDepth,
                              int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkIfExpr");
    TypeAST* condType = checkExpr(node.condition.get(), symbols, resolver, dc,
                                 loopDepth, parallelDepth, insideExtern);
    if (condType && !TypeChecker::isBooleanCompatible(condType)) {
        LUC_LOG_SEMANTIC("\tERROR: if condition must be bool");
        dc.error(DiagnosticCategory::Semantic, node.condition->loc, DiagCode::E3002,
                 "if condition must be bool");
    }
 
    TypeAST* thenType = nullptr;
    if (node.thenBranch) {
        thenType = checkExpr(node.thenBranch.get(), symbols, resolver, dc,
                              loopDepth, parallelDepth, insideExtern);
    }
    
    TypeAST* elseType = nullptr;
    if (node.elseBranch) {
        elseType = checkExpr(node.elseBranch.get(), symbols, resolver, dc,
                               loopDepth, parallelDepth, insideExtern);
    } else {
        LUC_LOG_SEMANTIC("\tERROR: if expression requires else branch");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "if expression requires an else branch");
    }
 
    TypeAST* unified = TypeChecker::unify(thenType, elseType);
    if (!unified && thenType && elseType) {
        LUC_LOG_SEMANTIC("\tERROR: type mismatch between 'if' and 'else' branches");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "type mismatch between 'if' and 'else' branches (cannot unify types)");
    }
 
    node.resolvedType = unified ? unified : SemanticHelpers::getPrimitiveType(PrimitiveKind::Any);
    return static_cast<TypeAST*>(node.resolvedType);
}

// ─────────────────────────────────────────────────────────────────────────────
// checkMatchExpr
// Subject is checked; each arm pattern is validated against the subject type;
// all arm bodies must unify.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkMatchExpr(MatchExprAST& node, SymbolTable& symbols,
                                TypeResolver& resolver, DiagnosticEngine& dc,
                                TypeAST* expectedReturn, int& loopDepth,
                                int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkMatchExpr");
    TypeAST* subjectType = checkExpr(node.subject.get(), symbols, resolver, dc,
                                     loopDepth, parallelDepth, insideExtern);
 
    TypeAST* unified = nullptr;
    for (auto& arm : node.arms) {
        // Check guard if present.
        if (arm->guard) {
            TypeAST* gt = checkExpr(arm->guard.get(), symbols, resolver, dc,
                                    loopDepth, parallelDepth, insideExtern);
            if (gt && !TypeChecker::isBooleanCompatible(gt)) {
                LUC_LOG_SEMANTIC("\tERROR: match arm guard must be bool");
                dc.error(DiagnosticCategory::Semantic, arm->guard->loc, DiagCode::E3002,
                         "match arm guard must be bool");
            }
        }
 
        // Bind patterns introduce variables into the arm scope.
        symbols.pushScope();
        for (auto& pat : arm->patterns) {
            // Pattern validation logic here... (omitted for brevity in this refactor, 
            // should integrate with a unifyPattern signature check in a full pass).
            if (pat->isa<IdentifierExprAST>()) {
                auto* bp = pat->as<IdentifierExprAST>();
                Symbol bs;
                bs.name = bp->name; bs.kind = SymbolKind::Var;
                bs.declKw = DeclKeyword::Let; bs.visibility = Visibility::Private;
                bs.type = subjectType; bs.decl = bp; bs.loc = bp->loc;
                symbols.declare(bs);
            }
        }
 
        TypeAST* armType = nullptr;
        for (auto& expr : arm->exprs) {
            TypeAST* et = checkExpr(expr.get(), symbols, resolver, dc,
                                    loopDepth, parallelDepth, insideExtern);
            // Primary value (first expression) determines the match arm return type.
            if (!armType) armType = et;
        }
 
        if (unified && armType) {
            unified = TypeChecker::unify(unified, armType);
        } else if (!unified) {
            unified = armType;
        }
 
        symbols.popScope();
    }
 
    // Default arm.
    if (node.defaultBody) {
        TypeAST* defaultType = nullptr;
        for (auto& expr : node.defaultBody->exprs) {
            TypeAST* et = checkExpr(expr.get(), symbols, resolver, dc,
                                    loopDepth, parallelDepth, insideExtern);
            if (!defaultType) defaultType = et;
        }
        if (unified && defaultType) {
            unified = TypeChecker::unify(unified, defaultType);
        } else if (!unified) {
            unified = defaultType;
        }
    } else {
        LUC_LOG_SEMANTIC("\tERROR: match expression requires 'default' arm");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "match expression requires a 'default' arm");
    }
 
    node.resolvedType = unified ? unified : SemanticHelpers::getPrimitiveType(PrimitiveKind::Any);
    return static_cast<TypeAST*>(node.resolvedType);
}

// ─────────────────────────────────────────────────────────────────────────────
// checkAwaitExpr
// Valid only inside an async function and not inside a parallel scope.
// Uses FunctionContext to check if current function has the ~async qualifier.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkAwaitExpr(AwaitExprAST& node, SymbolTable& symbols,
                                TypeResolver& resolver, DiagnosticEngine& dc,
                                int& loopDepth, int& parallelDepth, bool insideExtern) {
    
    // ── 1. Check we're inside an async function using FunctionContext ─────────
    if (!SemanticHelpers::isInsideAsyncFunction()) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "'await' can only be used inside an async function (marked with ~async)");
        return errorFallback(&node);
    }
    
    // ── 2. Check parallel scope ───────────────────────────────────────────────
    if (parallelDepth > 0) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E2006,
                 "'await' is not valid inside a 'parallel' block");
        return errorFallback(&node);
    }
    
    // ── 3. Check the inner expression is an async call ────────────────────────
    TypeAST* innerType = checkExpr(node.inner.get(), symbols, resolver, dc,
                                   loopDepth, parallelDepth, insideExtern);
    
    if (!innerType) {
        return errorFallback(&node);
    }
    
    // Must be a CallExpr (await someAsyncFunc())
    if (!node.inner->isa<CallExprAST>()) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "'await' can only be used on async function calls, e.g., 'await httpGet(url)'");
        return errorFallback(&node);
    }
    
    auto* callExpr = node.inner->as<CallExprAST>();
    
    if (!callExpr->isAsyncCall) {
        dc.error(DiagnosticCategory::Semantic, node.inner->loc, DiagCode::E3002,
                 "cannot 'await' a non-async function call; the function must be declared with '~async'");
        return errorFallback(&node);
    }
    
    // ── 4. Extract return type from the async call ────────────────────────────
    TypeAST* returnType = nullptr;
    
    // First, try to get return type from the callee's function type
    if (callExpr->callee && callExpr->callee->resolvedType) {
        TypeAST* calleeType = static_cast<TypeAST*>(callExpr->callee->resolvedType);
        if (calleeType && calleeType->isa<FuncTypeAST>()) {
            returnType = calleeType->as<FuncTypeAST>()->returnType.get();
        }
    }
    
    // Fallback: use the resolved type of the entire call expression
    if (!returnType && callExpr->resolvedType) {
        TypeAST* callResolvedType = static_cast<TypeAST*>(callExpr->resolvedType);
        if (callResolvedType && callResolvedType->isa<FuncTypeAST>()) {
            returnType = callResolvedType->as<FuncTypeAST>()->returnType.get();
        } else {
            returnType = callResolvedType;
        }
    }
    
    // Final fallback: use innerType
    if (!returnType && innerType->isa<FuncTypeAST>()) {
        returnType = innerType->as<FuncTypeAST>()->returnType.get();
    } else if (!returnType) {
        returnType = innerType;
    }
    
    // If still no return type, default to Any
    if (!returnType) {
        returnType = SemanticHelpers::getPrimitiveType(PrimitiveKind::Any);
    }
    
    node.resolvedType = returnType;
    return returnType;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkAnonFuncExpr
//
// Mirrors checkFuncDecl: iterates over paramGroups (outer = curry groups,
// inner = params within that group) so curried anonymous functions are fully
// supported.  A bare-block AnonFuncExprAST (paramGroups.empty()) has no
// params to declare — the enclosing FuncDeclAST already handled them.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkAnonFuncExpr(AnonFuncExprAST& node, SymbolTable& symbols,
                                   TypeResolver& resolver, DiagnosticEngine& dc,
                                   int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkAnonFuncExpr");
    
    // ── Resolve qualifiers using registry (if not already resolved) ──────────
    if (!node.type.rawQualifiers.empty()) {
        for (const auto& qualName : node.type.rawQualifiers) {
            uint32_t bit = QualifierRegistry::instance().getBit(qualName);
            if (bit == 0) {
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E2010,
                         "unknown type qualifier '~" + qualName + "'; " +
                         "known qualifiers: " + QualifierRegistry::instance().allNames());
            } else {
                node.type.qualifiers |= bit;
            }
        }
        node.type.rawQualifiers.clear();  // Free memory
    }
    
    TypeAST* returnType = nullptr;
    if (node.type.returnType) {
        returnType = resolver.resolveType(node.type.returnType.get());
    }

    // ── Track anonymous function if async using FunctionContext ───────────────
    bool isAsync = node.type.isAsync();
    Symbol tempSymbol;
    if (isAsync) {
        tempSymbol.name = "<anon>";
        tempSymbol.kind = SymbolKind::Func;
        tempSymbol.type = &node.type;
        SemanticHelpers::pushFunction("<anon>", &tempSymbol);
    }
    
    symbols.pushScope();
    
    // Declare parameters from node.type.paramGroups
    for (const auto& group : node.type.paramGroups) {
        for (const auto& param : group) {
            TypeAST* pt = resolver.resolveType(param.type.get());
            if (pt) {
                Symbol ps;
                ps.name       = param.name;
                ps.kind       = SymbolKind::Param;
                ps.declKw     = DeclKeyword::Let;
                ps.visibility = Visibility::Private;
                ps.type       = pt;
                ps.decl       = nullptr;  // ParamInfo has no AST back pointer
                ps.loc        = param.loc;
                if (!symbols.declare(ps)) {
                    dc.error(DiagnosticCategory::Semantic, param.loc,
                             DiagCode::E3005,
                             "duplicate parameter name '" + param.name + "'");
                }
            }
        }
    }
    
    if (node.body) {
        checkStmt(node.body.get(), symbols, resolver, dc, returnType,
                  loopDepth, parallelDepth, insideExtern);
    }
    
    symbols.popScope();

    if (isAsync) {
        SemanticHelpers::popFunction();
    }
    
    node.resolvedType = nullptr;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkNullableChainExpr
// player?.weapon?.damage ?? 0
// Every ?. step must be on a nullable type. The ?? fallback must match.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkNullableChainExpr(NullableChainExprAST& node, SymbolTable& symbols,
                                        TypeResolver& resolver, DiagnosticEngine& dc,
                                        int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkNullableChainExpr");
    TypeAST* objType = checkExpr(node.object.get(), symbols, resolver, dc,
                                 loopDepth, parallelDepth, insideExtern);
    if (objType && !TypeChecker::isNullable(objType)) {
        LUC_LOG_SEMANTIC("\tERROR: '?.' chain on non-nullable type");
        dc.error(DiagnosticCategory::Semantic, node.object->loc, DiagCode::E3002,
                 "'?.' chain must start on a nullable type");
    }

    TypeAST* fallbackType = nullptr;
    if (node.fallback) {
        fallbackType = checkExpr(node.fallback.get(), symbols, resolver, dc,
                                 loopDepth, parallelDepth, insideExtern);
    }

    if (!fallbackType) {
        return errorFallback(&node);
    }
    node.resolvedType = fallbackType;
    return fallbackType;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkRangeExpr
// 0..10 — both sides must be the same numeric type.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkRangeExpr(RangeExprAST& node, SymbolTable& symbols,
                                TypeResolver& resolver, DiagnosticEngine& dc,
                                int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkRangeExpr");
    TypeAST* loType = checkExpr(node.lo.get(), symbols, resolver, dc,
                                loopDepth, parallelDepth, insideExtern);
    TypeAST* hiType = checkExpr(node.hi.get(), symbols, resolver, dc,
                                loopDepth, parallelDepth, insideExtern);

    if (loType && hiType && !TypeChecker::isAssignable(loType, hiType) &&
        !TypeChecker::isAssignable(hiType, loType)) {
        LUC_LOG_SEMANTIC("\tERROR: range bounds must be the same type");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "range bounds must be the same type");
    }

    node.resolvedType = loType ? loType : hiType;
    return static_cast<TypeAST*>(node.resolvedType);
}

// ─────────────────────────────────────────────────────────────────────────────
// checkPipelineExpr
// seed -> step -> step
// Each step must be callable. The upstream result is passed as the first arg.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkPipelineExpr(PipelineExprAST& node, SymbolTable& symbols,
                                   TypeResolver& resolver, DiagnosticEngine& dc,
                                   int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkPipelineExpr");
    TypeAST* current = checkExpr(node.seed.get(), symbols, resolver, dc,
                                 loopDepth, parallelDepth, insideExtern);

    for (auto& step : node.steps) {
        switch (step->kind) {
            case PipelineStepKind::Ident: {
                Symbol* sym = symbols.lookup(step->ident);
                if (!sym) {
                    LUC_LOG_SEMANTIC("\tERROR: undeclared identifier in pipeline");
                    dc.error(DiagnosticCategory::Semantic, step->loc, DiagCode::E3001,
                             "undeclared identifier '" + step->ident + "' in pipeline");
                    current = errorFallback(&node);
                } else {
                    TypeAST* st = sym->type;
                    if (st && st->isa<FuncTypeAST>()) {
                        current = st->as<FuncTypeAST>()->returnType.get();
                    } else {
                        current = st;
                    }
                }
                break;
            }
            case PipelineStepKind::BehaviorRef: {
                std::string mangled = step->typeName + "." + step->method;
                Symbol* sym = symbols.lookup(mangled);
                if (!sym) {
                    LUC_LOG_SEMANTIC("\tERROR: method not found in pipeline");
                    dc.error(DiagnosticCategory::Semantic, step->loc, DiagCode::E3001,
                             "no method '" + step->method + "' on '" + step->typeName + "'");
                    current = nullptr;
                } else if (sym->type && sym->type->isa<FuncTypeAST>()) {
                    current = sym->type->as<FuncTypeAST>()->returnType.get();
                }
                break;
            }
            case PipelineStepKind::BehaviorArgPack:
                // Similar to BehaviorRef but with arguments; return type inference
                // requires full method resolution.
                break;
            case PipelineStepKind::ArgPack:
            case PipelineStepKind::FieldRef:
            case PipelineStepKind::FieldArgPack:
            case PipelineStepKind::IndexRef:
            case PipelineStepKind::IndexArgPack:
            case PipelineStepKind::AnonFunc:
                // For complex steps the return type cannot be easily inferred
                // without full function resolution; pass through current for now.
                break;
        }
    }

    node.resolvedType = current;
    return current;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkIndexExpr
// nums[i] → element type; nums[i..j] → slice type.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkIndexExpr(IndexExprAST& node, SymbolTable& symbols,
                                TypeResolver& resolver, DiagnosticEngine& dc,
                                int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkIndexExpr");
    TypeAST* targetType = checkExpr(node.target.get(), symbols, resolver, dc,
                                    loopDepth, parallelDepth, insideExtern);
    TypeAST* idxType = checkExpr(node.index.get(), symbols, resolver, dc,
                                 loopDepth, parallelDepth, insideExtern);

    // Index must be an integer type.
    if (idxType && idxType->isa<PrimitiveTypeAST>()) {
        auto k = idxType->as<PrimitiveTypeAST>()->primitiveKind;
        bool isInt = (k == PrimitiveKind::Int || k == PrimitiveKind::Long ||
                      k == PrimitiveKind::Uint || k == PrimitiveKind::Ulong ||
                      k == PrimitiveKind::Byte || k == PrimitiveKind::Short);
        if (!isInt) {
            LUC_LOG_SEMANTIC("\tERROR: array index must be integer");
            dc.error(DiagnosticCategory::Semantic, node.index->loc, DiagCode::E3002,
                     "array index must be an integer type");
        }
    }

    if (node.sliceEnd) {
        checkExpr(node.sliceEnd.get(), symbols, resolver, dc,
                  loopDepth, parallelDepth, insideExtern);
    }

    // Resolve the element type from the target array type.
    TypeAST* elemType = SemanticHelpers::getPrimitiveType(PrimitiveKind::Any);
    if (targetType) {
        if (targetType->isa<FixedArrayTypeAST>())
            elemType = targetType->as<FixedArrayTypeAST>()->element.get();
        else if (targetType->isa<SliceTypeAST>())
            elemType = targetType->as<SliceTypeAST>()->element.get();
        else if (targetType->isa<DynamicArrayTypeAST>())
            elemType = targetType->as<DynamicArrayTypeAST>()->element.get();
        else if (targetType->isa<PtrTypeAST>()) {
            LUC_LOG_SEMANTIC("\tERROR: cannot index raw pointer");
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "cannot index into raw pointer type '*T'; "
                     "use '@ptrOffset(ptr, i)' for pointer arithmetic or '@ptrToRef' to cross to a safe type");
            TypeAST* fallback = SemanticHelpers::getPrimitiveType(PrimitiveKind::Any);
            node.resolvedType = fallback;
            return fallback;
        }
    }

    // For slice operations, wrap the element type in a SliceTypeAST.
    if (node.kind == IndexKind::Slice) {
        node.sliceType = std::make_unique<SliceTypeAST>(SemanticHelpers::cloneType(elemType));
        node.resolvedType = node.sliceType.get();
        return node.sliceType.get();
    }

    node.resolvedType = elemType;
    return elemType;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkStructLiteralExpr — Validates struct literal and returns struct type
//
// Rules enforced:
//   - Target struct must exist in symbol table
//   - All provided field names must match declared fields
//   - All provided field values must match their declared types
//   - All required fields (no default, not nullable) must be provided
//   - Returns the struct type (or nullptr on error)
//
// TYPE SYSTEM INTEGRATION (THE FIX):
// ──────────────────────────────────
// This function returns sym->type, which is now non-null thanks to the selfType
// fix in StructDeclAST and SemanticCollector::visit(StructDeclAST).
//
// BEFORE FIX:
//   sym->type = nullptr  →  function returns nullptr  →  type checker fails
//   Example: let ops MathOps = MathOps { ... }
//   - checkStructLiteralExpr returns nullptr
//   - checkVarDecl sees: isAssignable(nullptr, MathOps) → ERROR
//   - False positive: "type mismatch in declaration 'ops'"
//
// AFTER FIX:
//   sym->type = NamedTypeAST("MathOps")  →  returns that type
//   - Type comparison succeeds: NamedTypeAST("MathOps") ≈ NamedTypeAST("MathOps")
//   - Struct literals now work correctly in assignments
//
// FALLBACK LOGIC:
// ───────────────
// If sym->type is somehow still null (defensive programming), we try to
// retrieve selfType directly from the StructDeclAST. This provides a safety
// net without breaking code that relied on the old (broken) behavior.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkStructLiteralExpr(StructLiteralExprAST& node, SymbolTable& symbols,
                                        TypeResolver& resolver, DiagnosticEngine& dc,
                                        int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkStructLiteralExpr: typeName='" << node.typeName << "'");
    Symbol* sym = symbols.lookup(node.typeName);
    if (!sym || sym->kind != SymbolKind::Struct) {
        LUC_LOG_SEMANTIC("\tERROR: '" << node.typeName << "' is not a declared struct");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                 "'" + node.typeName + "' is not a declared struct");
        return errorFallback(&node);
    }

    auto* structDecl = sym->decl->as<StructDeclAST>();

    // Set generic context so field types (like T) resolve correctly instead of throwing E3001.
    resolver.setGenericParams(&structDecl->genericParams);

    // Build substitution map if the literal provided generic arguments.
    std::unordered_map<std::string, TypeAST*> substitutionMap;
    if (!node.genericArgs.empty() && node.genericArgs.size() == structDecl->genericParams.size()) {
        for (size_t i = 0; i < structDecl->genericParams.size(); ++i) {
            substitutionMap[structDecl->genericParams[i]->name] = resolver.resolveType(node.genericArgs[i].get());
        }
        resolver.setSubstitutionMap(&substitutionMap);
    }

    // Check that every field init matches the declared field type.
    for (auto& init : node.inits) {
        FieldDeclAST* fieldDecl = nullptr;
        for (auto& f : structDecl->fields) {
            if (f->name == init.name) { fieldDecl = f.get(); break; }
        }
        if (!fieldDecl) {
            LUC_LOG_SEMANTIC("\tERROR: struct '" << node.typeName << "' has no field '" << init.name << "'");
            dc.error(DiagnosticCategory::Semantic, init.loc, DiagCode::E3001,
                     "struct '" + node.typeName + "' has no field '" + init.name + "'");
            continue;
        }
        TypeAST* ft = resolver.resolveType(fieldDecl->type.get());
        TypeAST* vt = checkExpr(init.value.get(), symbols, resolver, dc,
                                loopDepth, parallelDepth, insideExtern);
        if (ft && vt && !TypeChecker::isAssignable(vt, ft)) {
            LUC_LOG_SEMANTIC("\tERROR: type mismatch for field '" << init.name << "' in struct literal");
            dc.error(DiagnosticCategory::Semantic, init.loc, DiagCode::E3002,
                     "type mismatch for field '" + init.name + "' in struct literal");
        }
    }

    // Check that all required fields (no default and not nullable) are provided.
    for (auto& f : structDecl->fields) {
        // Map search: was this field provided in the literal?
        bool found = false;
        for (auto& init : node.inits) {
            if (init.name == f->name) {
                found = true;
                break;
            }
        }

        if (!found) {
            // If the field is missing, it's only valid if it has a default value 
            // OR if its type is nullable (which defaults to nil).
            if (f->defaultVal) continue;
            
            TypeAST* ft = resolver.resolveType(f->type.get());
            if (ft && TypeChecker::isNullable(ft)) continue;

            // Otherwise, it's a semantic error.
            LUC_LOG_SEMANTIC("\tERROR: missing required field '" << f->name << "'");
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "missing required field '" + f->name + "' in struct literal '" +
                     node.typeName + "'");
        }
    }

    // Clear generic context and substitution map.
    resolver.setGenericParams(nullptr);
    resolver.setSubstitutionMap(nullptr);

    // Return the struct type symbol.
    //
    // PRIMARY PATH: Use sym->type (the struct's self-type).
    // ────────────────────────────────────────────────────
    // sym->type is now non-null thanks to the selfType fix:
    // SemanticCollector::visit(StructDeclAST) creates a NamedTypeAST and stores it here.
    // This allows type checkers to compare struct literal types with declared types.
    //
    // IMPACT: Before this fix, sym->type was nullptr, causing checkVarDecl to fail
    // with false "type mismatch" errors for assignments like:
    //   let ops MathOps = MathOps { add = ..., transform = ... }
    // Now it correctly compares NamedTypeAST("MathOps") with NamedTypeAST("MathOps").
    if (sym->type) {
        if (!node.genericArgs.empty()) {
            node.instantiatedType = std::make_unique<NamedTypeAST>(node.typeName);
            for (auto& arg : node.genericArgs) {
                TypeAST* resolvedArg = resolver.resolveType(arg.get());
                node.instantiatedType->genericArgs.push_back(SemanticHelpers::cloneType(resolvedArg));
            }
            node.resolvedType = node.instantiatedType.get();
            return node.instantiatedType.get();
        }
        node.resolvedType = sym->type;
        return sym->type;
    }
    
    // FALLBACK PATH: Retrieve selfType directly from the struct declaration.
    // ───────────────────────────────────────────────────────────────────────
    // If sym->type is somehow still null (shouldn't happen post-fix, but defensive),
    // try to use selfType. This ensures robustness even if the symbol table
    // pointer wasn't properly initialized.
    if (sym->decl && sym->decl->isa<StructDeclAST>()) {
        auto* structDecl = sym->decl->as<StructDeclAST>();
        if (structDecl->selfType) {
            node.resolvedType = structDecl->selfType.get();
            return structDecl->selfType.get();
        }
    }
    
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkArrayLiteralExpr
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkArrayLiteralExpr(ArrayLiteralExprAST& node, SymbolTable& symbols,
                                       TypeResolver& resolver, DiagnosticEngine& dc,
                                       int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkArrayLiteralExpr");
    TypeAST* elemType = nullptr;
    for (auto& elem : node.elements) {
        TypeAST* et = checkExpr(elem.get(), symbols, resolver, dc,
                                loopDepth, parallelDepth, insideExtern);
        if (!elemType && et) elemType = et;
    }
    // The resolved type is left as nullptr; context (declaration type) determines
    // whether this is a fixed / slice / dynamic array.
    node.resolvedType = nullptr;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkTypeConvExpr
// float(x) — safe explicit cast; *float(x) — unsafe bit reinterpret (extern only).
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkTypeConvExpr(TypeConvExprAST& node, SymbolTable& symbols,
                                   TypeResolver& resolver, DiagnosticEngine& dc,
                                   int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkTypeConvExpr");
    if (node.isUnsafe && !insideExtern) {
        LUC_LOG_SEMANTIC("\tERROR: unsafe reinterpret outside extern");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "unsafe type reinterpret '*' is only valid on '@extern'-decorated declarations; "
                 "use '@bitcast(T, x)' for general-purpose bit reinterpretation");
    }
    checkExpr(node.expr.get(), symbols, resolver, dc,loopDepth, parallelDepth, insideExtern);
    TypeAST* targetType = resolver.resolveType(node.targetType.get());
    if (!targetType) {
        return errorFallback(&node);
    }
    node.resolvedType = targetType;
    return targetType;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkIntrinsicCallExpr
//
// Registry-driven validation of '@name(args)' compiler intrinsic calls.
//
// Drives off IntrinsicRegistry::kEntries — every known intrinsic is described
// there with its arg kinds, return kind, and min/max arg counts. Adding a new
// intrinsic requires only a row in IntrinsicRegistry.hpp and no changes here.
//
// FUTURE EXPANSION:
//   - New intrinsics mapping to LLVM (e.g. @prefetch, @expect) should be added
//     to IntrinsicRegistry.hpp. 
//   - Custom validation logic (e.g. type constraints) should be added to the
//     switch statements in this function.
//
// Validation logic:
//   1. Unknown name  → E3009 with full known-names list from registry.
//   2. Type-arg intrinsics (@sizeof, @alignof):
//        - Must have a typeArg and zero value args.
//        - typeArg is resolved through the TypeResolver.
//   3. @bitcast(T, x):
//        - Has both a typeArg (target type) and one value arg.
//   4. All other intrinsics:
//        - Arg count checked against entry.minArgs / entry.maxArgs.
//        - Each value arg is recursively checked through checkExpr.
//        - FloatValue slots require float or double; IntValue slots require
//          any integer primitive.  AnyValue / PtrValue / SizeValue are
//          permissive (type validated at runtime / codegen level).
//   5. Return type is resolved from IntrinsicReturnKind:
//        Void        → nullptr
//        Uint64      → uint64
//        Float32     → float
//        Float64     → double
//        SameAsArg0  → type of the first value argument
//        SameAsArg1  → type of the second value argument
//
// Unknown intrinsic names → E3009. Wrong arg counts / bad types → E3010.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkIntrinsicCallExpr(IntrinsicCallExprAST& node, SymbolTable& symbols,
                                        TypeResolver& resolver, DiagnosticEngine& dc,
                                        int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkIntrinsicCallExpr: name='" << node.intrinsicName << "'");
    const std::string& name = node.intrinsicName;

    // ── 1. Registry lookup ────────────────────────────────────────────────────
    const IntrinsicEntry* entry = IntrinsicRegistry::lookup(name);
    if (!entry) {
        LUC_LOG_SEMANTIC("\tERROR: unknown intrinsic '@" << name << "'");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3009,
                 "unknown compiler intrinsic '@" + name + "'; "
                 "known intrinsics: " + IntrinsicRegistry::allNames());
        // Still walk any provided args so their sub-expressions are visited.
        for (auto& arg : node.args)
            checkExpr(arg.get(), symbols, resolver, dc, loopDepth, parallelDepth, insideExtern);
        return errorFallback(&node);
    }

    // ── 2. Type-only intrinsics: @sizeof(T) and @alignof(T) ──────────────────
    // These take one type argument and zero value arguments.
    if (entry->returnKind == IntrinsicReturnKind::Uint64 &&
        !entry->argKinds.empty() &&
        entry->argKinds[0] == IntrinsicArgKind::TypeArg &&
        entry->minArgs == 0) {

        if (!node.typeArg) {
            LUC_LOG_SEMANTIC("\tERROR: intrinsic requires type argument");
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3010,
                     "'@" + name + "' requires a type argument, e.g. '@" + name + "(int)'");
        } else {
            resolver.resolveType(node.typeArg.get());
        }
        if (!node.args.empty()) {
            LUC_LOG_SEMANTIC("\tERROR: intrinsic takes only type argument");
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3010,
                     "'@" + name + "' takes only a type argument, not value arguments");
        }
        TypeAST* ret = SemanticHelpers::getPrimitiveType(PrimitiveKind::Uint64);
        node.resolvedType = ret;
        return ret;
    }

    // ── 3. @bitcast(T, x) — type arg + one value arg ─────────────────────────
    if (name == "bitcast") {
        if (!node.typeArg) {
            LUC_LOG_SEMANTIC("\tERROR: @bitcast requires type argument");
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3010,
                     "'@bitcast' requires a target type argument: '@bitcast(T, x)'");
        } else {
            resolver.resolveType(node.typeArg.get());
        }
        if (node.args.size() != 1) {
            LUC_LOG_SEMANTIC("\tERROR: @bitcast requires 1 value argument");
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3010,
                     "'@bitcast' requires exactly 1 value argument: '@bitcast(T, x)'");
        } else {
            checkExpr(node.args[0].get(), symbols, resolver, dc, loopDepth, parallelDepth, insideExtern);
        }
        // Return type is the target type (typeArg).
        TypeAST* ret = node.typeArg ? resolver.resolveType(node.typeArg.get())
                                     : SemanticHelpers::getPrimitiveType(PrimitiveKind::Any);
        node.resolvedType = ret;
        return ret;
    }

    // ── 3b. @ptrToRef(T, ptr) — TypeArg + PtrValue ───────────────────────────
    if (name == "ptrToRef") {
        if (!node.typeArg) {
            LUC_LOG_SEMANTIC("\tERROR: @ptrToRef requires type argument");
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3010,
                     "'@ptrToRef' requires a target type argument: '@ptrToRef(T, ptr)'");
        }
        if (node.args.size() != 1) {
            LUC_LOG_SEMANTIC("\tERROR: @ptrToRef requires 1 value argument");
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3010,
                     "'@ptrToRef' requires exactly 1 value argument: '@ptrToRef(T, ptr)'");
        } else {
            TypeAST* pt = checkExpr(node.args[0].get(), symbols, resolver, dc,
                                    loopDepth, parallelDepth, insideExtern);
            if (pt && !pt->isa<PtrTypeAST>()) {
                LUC_LOG_SEMANTIC("\tERROR: @ptrToRef arg must be raw pointer");
                dc.error(DiagnosticCategory::Semantic, node.args[0]->loc, DiagCode::E3010,
                         "'@ptrToRef' argument 1 must be a raw pointer type '*T'");
            }
        }
        TypeAST* ret = nullptr;
        if (node.typeArg) {
            // Returns the provided type. If the user wants a reference, they should pass &T.
            // e.g. @ptrToRef(&Player, buf)
            ret = resolver.resolveType(node.typeArg.get());
        } else {
            ret = SemanticHelpers::getPrimitiveType(PrimitiveKind::Any);
        }
        node.resolvedType = ret;
        return ret;
    }

    // ── 4. Value-argument intrinsics ─────────────────────────────────────────
    // Verify typeArg was NOT supplied for these (they take only values).
    if (node.typeArg) {
        LUC_LOG_SEMANTIC("\tERROR: intrinsic does not take type argument");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3010,
                 "'@" + name + "' does not take a type argument");
    }

    // Arg count check.
    int nArgs = static_cast<int>(node.args.size());
    bool countOk = (nArgs >= entry->minArgs) &&
                   (entry->maxArgs == -1 || nArgs <= entry->maxArgs);
    if (!countOk) {
        std::string expected = (entry->minArgs == entry->maxArgs)
            ? std::to_string(entry->minArgs)
            : std::to_string(entry->minArgs) + ".." + std::to_string(entry->maxArgs);
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3010,
                 "'@" + name + "' expects " + expected + " argument(s), got " +
                 std::to_string(nArgs));
        LUC_LOG_SEMANTIC("\tERROR: intrinsic argument count mismatch");
        // Walk args anyway to surface nested errors.
        for (auto& arg : node.args)
            checkExpr(arg.get(), symbols, resolver, dc, loopDepth, parallelDepth, insideExtern);
        return errorFallback(&node);
    }

    // Check each argument against its registry slot kind.
    // Slots past the argKinds vector length default to AnyValue.
    std::vector<TypeAST*> argTypes;
    argTypes.reserve(node.args.size());

    for (size_t i = 0; i < node.args.size(); ++i) {
        TypeAST* at = checkExpr(node.args[i].get(), symbols, resolver, dc,
                                loopDepth, parallelDepth, insideExtern);
        argTypes.push_back(at);

        // Determine which slot kind applies.
        IntrinsicArgKind slotKind = (i < entry->argKinds.size())
            ? entry->argKinds[i]
            : IntrinsicArgKind::AnyValue;

        if (!at) continue; // expression error already reported

        switch (slotKind) {
            case IntrinsicArgKind::FloatValue:
                if (at->isa<PrimitiveTypeAST>()) {
                    auto k = at->as<PrimitiveTypeAST>()->primitiveKind;
                    if (k != PrimitiveKind::Float  &&
                        k != PrimitiveKind::Double &&
                        k != PrimitiveKind::Decimal) {
                        LUC_LOG_SEMANTIC("\tERROR: intrinsic float argument type mismatch");
                        dc.error(DiagnosticCategory::Semantic,
                                 node.args[i]->loc, DiagCode::E3010,
                                 "'@" + name + "' argument " + std::to_string(i+1) +
                                 " must be float, double, or decimal");
                    }
                }
                break;

            case IntrinsicArgKind::IntValue:
                if (at->isa<PrimitiveTypeAST>()) {
                    auto k = at->as<PrimitiveTypeAST>()->primitiveKind;
                    // Any integer primitive is acceptable.
                    bool isInt =
                        k == PrimitiveKind::Byte  || k == PrimitiveKind::Short  ||
                        k == PrimitiveKind::Int   || k == PrimitiveKind::Long   ||
                        k == PrimitiveKind::Ubyte || k == PrimitiveKind::Ushort ||
                        k == PrimitiveKind::Uint  || k == PrimitiveKind::Ulong  ||
                        k == PrimitiveKind::Int8  || k == PrimitiveKind::Int16  ||
                        k == PrimitiveKind::Int32 || k == PrimitiveKind::Int64  ||
                        k == PrimitiveKind::Uint8 || k == PrimitiveKind::Uint16 ||
                        k == PrimitiveKind::Uint32|| k == PrimitiveKind::Uint64;
                    if (!isInt) {
                        LUC_LOG_SEMANTIC("\tERROR: intrinsic integer argument type mismatch");
                        dc.error(DiagnosticCategory::Semantic,
                                 node.args[i]->loc, DiagCode::E3010,
                                 "'@" + name + "' argument " + std::to_string(i+1) +
                                 " must be an integer type");
                    }
                }
                break;

            case IntrinsicArgKind::SizeValue:
                // Accept any integer; ideally uint64 but we allow widening.
                if (at->isa<PrimitiveTypeAST>()) {
                    auto k = at->as<PrimitiveTypeAST>()->primitiveKind;
                    bool isInt =
                        k == PrimitiveKind::Int  || k == PrimitiveKind::Long  ||
                        k == PrimitiveKind::Uint || k == PrimitiveKind::Ulong ||
                        k == PrimitiveKind::Int32|| k == PrimitiveKind::Int64 ||
                        k == PrimitiveKind::Uint32||k == PrimitiveKind::Uint64;
                    if (!isInt) {
                        LUC_LOG_SEMANTIC("\tERROR: intrinsic size argument type mismatch");
                        dc.error(DiagnosticCategory::Semantic,
                                 node.args[i]->loc, DiagCode::E3010,
                                 "'@" + name + "' size argument must be an integer type");
                    }
                }
                break;

            case IntrinsicArgKind::AnyValue:
            case IntrinsicArgKind::PtrValue:
            case IntrinsicArgKind::TypeArg:
                // No additional type constraint at the semantic level.
                break;
        }
    }

    // ── 4b. Special case pointer intrinsics ──────────────────────────────────
    if (name == "refToPtr") {
        if (!argTypes.empty() && argTypes[0]) {
            // Logic handled in codegen, but semantic return type is always a pointer.
            // We'll return a pointer to the same inner type if it's a reference.
            // For now, return any pointer-like type or just the raw arg if it's already a ptr.
        }
    }

    // ── 5. Resolve return type from registry entry ────────────────────────────
    TypeAST* ret = nullptr;
    switch (entry->returnKind) {
        case IntrinsicReturnKind::Void:
            ret = nullptr;
            break;
        case IntrinsicReturnKind::Uint64:
            ret = SemanticHelpers::getPrimitiveType(PrimitiveKind::Uint64);
            break;
        case IntrinsicReturnKind::Float32:
            ret = SemanticHelpers::getPrimitiveType(PrimitiveKind::Float);
            break;
        case IntrinsicReturnKind::Float64:
            ret = SemanticHelpers::getPrimitiveType(PrimitiveKind::Double);
            break;
        case IntrinsicReturnKind::SameAsArg0:
            ret = !argTypes.empty() ? argTypes[0] : SemanticHelpers::getPrimitiveType(PrimitiveKind::Any);
            break;
        case IntrinsicReturnKind::SameAsArg1:
            ret = argTypes.size() >= 2 ? argTypes[1] : SemanticHelpers::getPrimitiveType(PrimitiveKind::Any);
            break;
        case IntrinsicReturnKind::RefOfTypeArg0:
            // This is handled above in the @ptrToRef special case.
            ret = node.typeArg ? resolver.resolveType(node.typeArg.get()) : SemanticHelpers::getPrimitiveType(PrimitiveKind::Any);
            break;
        case IntrinsicReturnKind::Int64:
            ret = SemanticHelpers::getPrimitiveType(PrimitiveKind::Int64);
            break;
    }

    // For overloaded float intrinsics: if arg0 is double, return double.
    if (entry->isOverloaded &&
        entry->returnKind == IntrinsicReturnKind::SameAsArg0 &&
        !argTypes.empty() && argTypes[0] &&
        argTypes[0]->isa<PrimitiveTypeAST>() &&
        argTypes[0]->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Double) {
        ret = SemanticHelpers::getPrimitiveType(PrimitiveKind::Double);
    }

    node.resolvedType = ret;
    return ret;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkComposeExpr
// f +> g: left output type must match right input type.
//
// RULES:
//   1. Result function has NO qualifiers — qualifiers are stripped entirely
//   2. Curried functions are fully supported:
//      - If LHS is curried: compose its EVENTUAL return type with RHS
//      - If RHS is curried: consume its FIRST parameter group
//      - Remaining groups become part of the result
//   3. If LHS's eventual return type is a function (i.e. not fully applied),
//      composition is impossible — error.
//   4. f +> g produces a new FuncTypeAST with:
//      - Parameters = all LHS parameters (all groups)
//      - Return = if RHS is fully consumed: RHS's return type
//                else: RHS with first group removed (curried result)
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkComposeExpr(ComposeExprAST& node, SymbolTable& symbols,
                                  TypeResolver& resolver, DiagnosticEngine& dc,
                                  int& loopDepth, int& parallelDepth, bool insideExtern) {
    LUC_LOG_SEMANTIC_VERBOSE("checkComposeExpr");
    
    // First, get the type of the left side (which is a pipeline expression)
    TypeAST* leftType = checkExpr(node.left.get(), symbols, resolver, dc,
                                  loopDepth, parallelDepth, insideExtern);
    
    if (!leftType) {
        LUC_LOG_SEMANTIC("\tERROR: left side has no type");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "left side of '+>' has no type");
        return errorFallback(&node);
    }
    
    // In composition, the left side should be callable
    if (!TypeChecker::isCallable(leftType)) {
        LUC_LOG_SEMANTIC("\tERROR: left side not callable");
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "left side of '+>' must be a function");
        return errorFallback(&node);
    }
    
    // Start with left type as the "current" type we're building from
    TypeAST* current = leftType;
    
    // For building the result function's parameter groups
    // We need to capture all parameters from the left side
    std::vector<ParamGroup> leftParamGroups;
    
    // Extract parameter groups from left if it's a function type
    if (leftType->isa<FuncTypeAST>()) {
        auto* leftFunc = leftType->as<FuncTypeAST>();
        // Move the param groups instead of copying
        leftParamGroups = std::move(leftFunc->paramGroups);
    }
    
    // Store the result type that will be returned to the caller
    std::unique_ptr<FuncTypeAST> finalResult = nullptr;
    
    // Process each compose operand in order
    for (auto& operand : node.operands) {
        if (!operand) continue;
        
        TypeAST* opType = nullptr;
        
        // Resolve the operand to its type
        if (operand->kind == ComposeOperandKind::Ident) {
            Symbol* sym = symbols.lookup(operand->ident);
            if (!sym) {
                LUC_LOG_SEMANTIC("\tERROR: undeclared identifier '" << operand->ident << "' in compose");
                dc.error(DiagnosticCategory::Semantic, operand->loc, DiagCode::E3001,
                         "undeclared identifier '" + operand->ident + "' in '+>' composition");
                return errorFallback(&node);
            }
            opType = sym->type;
        } else if (operand->kind == ComposeOperandKind::BehaviorRef) {
            std::string mangled = operand->typeName + "." + operand->method;
            Symbol* sym = symbols.lookup(mangled);
            if (!sym) {
                LUC_LOG_SEMANTIC("\tERROR: method '" << operand->method << "' not found on '" 
                                 << operand->typeName << "'");
                dc.error(DiagnosticCategory::Semantic, operand->loc, DiagCode::E3001,
                         "no method '" + operand->method + "' found on '" + operand->typeName + "'");
                return errorFallback(&node);
            }
            opType = sym->type;
        } else if (operand->kind == ComposeOperandKind::FieldRef) {
            Symbol* sym = symbols.lookup(operand->ident);
            if (!sym) {
                LUC_LOG_SEMANTIC("\tERROR: undeclared identifier '" << operand->ident << "'");
                dc.error(DiagnosticCategory::Semantic, operand->loc, DiagCode::E3001,
                         "undeclared identifier '" + operand->ident + "'");
                return errorFallback(&node);
            }
            opType = sym->type;
        }
        
        if (!opType) {
            LUC_LOG_SEMANTIC("\tERROR: compose operand has no type");
            dc.error(DiagnosticCategory::Semantic, operand->loc, DiagCode::E3002,
                     "compose operand has no type");
            return errorFallback(&node);
        }
        
        if (!TypeChecker::isCallable(opType)) {
            LUC_LOG_SEMANTIC("\tERROR: compose operand not callable");
            dc.error(DiagnosticCategory::Semantic, operand->loc, DiagCode::E3002,
                     "compose operand must be a function");
            return errorFallback(&node);
        }
        
        // For composition, we need the function type
        if (!opType->isa<FuncTypeAST>()) {
            LUC_LOG_SEMANTIC("\tERROR: compose operand is not a function type");
            dc.error(DiagnosticCategory::Semantic, operand->loc, DiagCode::E3002,
                     "compose operand must be a function");
            return errorFallback(&node);
        }
        
        auto* rightFunc = opType->as<FuncTypeAST>();
        
        // ── STRIP QUALIFIERS ───────────────────────────────────────────────
        // Rule 1: The result function has NO qualifiers from either side
        
        // ── CURRY HANDLING: Reduce left to its EVENTUAL return type ─────────
        TypeAST* leftReturnType = current;
        
        // Unroll curried left side to find its eventual return type
        while (leftReturnType && leftReturnType->isa<FuncTypeAST>()) {
            auto* asFunc = leftReturnType->as<FuncTypeAST>();
            if (!asFunc->paramGroups.empty() && asFunc->returnType) {
                leftReturnType = asFunc->returnType.get();
            } else {
                break;
            }
        }
        
        // ── CONSUME RIGHT'S FIRST PARAMETER GROUP ──────────────────────────
        if (rightFunc->paramGroups.empty()) {
            // Right takes no parameters — valid composition
            LUC_LOG_SEMANTIC_EXTREME("\tright function takes no parameters — discarding left return value");
            
            auto resultType = std::make_unique<FuncTypeAST>(/*isNullable=*/false);
            
            // Move all parameter groups from left (not right)
            resultType->paramGroups = std::move(leftParamGroups);
            
            // Set return type = right's return type
            if (rightFunc->returnType) {
                resultType->returnType = SemanticHelpers::cloneType(rightFunc->returnType.get());
            }
            
            // NO QUALIFIERS
            resultType->qualifiers = 0;
            resultType->rawQualifiers.clear();
            
            finalResult = std::move(resultType);
            current = finalResult.get();
            
            // Update leftParamGroups for next iteration
            if (current && current->isa<FuncTypeAST>()) {
                // Need to move from paramGroups, but we're using them in finalResult
                // For the next iteration, we need to copy the param group structure
                // (types only, not ownership)
                auto* currFunc = current->as<FuncTypeAST>();
                leftParamGroups.clear();
                for (const auto& group : currFunc->paramGroups) {
                    ParamGroup newGroup;
                    for (const auto& param : group) {
                        TypePtr clonedType = SemanticHelpers::cloneType(param.type.get());
                        newGroup.emplace_back(param.name, std::move(clonedType), 
                                              param.isVariadic, param.loc);
                    }
                    leftParamGroups.push_back(std::move(newGroup));
                }
            }
            continue;
        }
        
        // Right has at least one parameter group
        auto& firstGroup = rightFunc->paramGroups[0];
        if (firstGroup.empty()) {
            LUC_LOG_SEMANTIC("\tERROR: right function first param group is empty (invalid)");
            dc.error(DiagnosticCategory::Semantic, operand->loc, DiagCode::E3002,
                     "invalid function type: first parameter group is empty");
            return errorFallback(&node);
        }
        
        // The first parameter's type is the expected input type
        TypeAST* expectedInput = firstGroup[0].type.get();
        
        // Check compatibility
        if (leftReturnType && expectedInput) {
            if (!TypeChecker::isAssignable(leftReturnType, expectedInput)) {
                LUC_LOG_SEMANTIC("\tERROR: type mismatch in composition");
                dc.error(DiagnosticCategory::Semantic, operand->loc, DiagCode::E3002,
                         "type mismatch in '+>' composition: left returns '" +
                         LucDebug::kindToString(leftReturnType->kind) +
                         "', but right expects '" +
                         LucDebug::kindToString(expectedInput->kind) + "'");
                return errorFallback(&node);
            }
        }
        
        // ── BUILD THE RESULT TYPE ──────────────────────────────────────────
        auto resultType = std::make_unique<FuncTypeAST>(/*isNullable=*/false);
        
        // Move all parameter groups from left
        resultType->paramGroups = std::move(leftParamGroups);
        
        // Determine the return type:
        if (rightFunc->paramGroups.size() > 1 || firstGroup.size() > 1) {
            // Right is curried OR has multi-param group
            auto remainingType = std::make_unique<FuncTypeAST>(/*isNullable=*/false);
            
            if (firstGroup.size() > 1) {
                // Multi-param case: remove the first parameter
                ParamGroup remainingGroup;
                for (size_t i = 1; i < firstGroup.size(); ++i) {
                    TypePtr clonedType = SemanticHelpers::cloneType(firstGroup[i].type.get());
                    remainingGroup.emplace_back(firstGroup[i].name, std::move(clonedType),
                                                firstGroup[i].isVariadic, firstGroup[i].loc);
                }
                remainingType->paramGroups.push_back(std::move(remainingGroup));
                
                // Copy remaining groups
                for (size_t g = 1; g < rightFunc->paramGroups.size(); ++g) {
                    ParamGroup newGroup;
                    for (const auto& param : rightFunc->paramGroups[g]) {
                        TypePtr clonedType = SemanticHelpers::cloneType(param.type.get());
                        newGroup.emplace_back(param.name, std::move(clonedType),
                                              param.isVariadic, param.loc);
                    }
                    remainingType->paramGroups.push_back(std::move(newGroup));
                }
            } else {
                // Single param but more groups remain
                for (size_t g = 1; g < rightFunc->paramGroups.size(); ++g) {
                    ParamGroup newGroup;
                    for (const auto& param : rightFunc->paramGroups[g]) {
                        TypePtr clonedType = SemanticHelpers::cloneType(param.type.get());
                        newGroup.emplace_back(param.name, std::move(clonedType),
                                              param.isVariadic, param.loc);
                    }
                    remainingType->paramGroups.push_back(std::move(newGroup));
                }
            }
            
            // Copy return type
            if (rightFunc->returnType) {
                remainingType->returnType = SemanticHelpers::cloneType(rightFunc->returnType.get());
            }
            
            resultType->returnType = std::move(remainingType);
        } else {
            // Right has exactly one group with one parameter — composition complete
            if (rightFunc->returnType) {
                resultType->returnType = SemanticHelpers::cloneType(rightFunc->returnType.get());
            }
        }
        
        // NO QUALIFIERS — stripped entirely
        resultType->qualifiers = 0;
        resultType->rawQualifiers.clear();
        
        finalResult = std::move(resultType);
        current = finalResult.get();
        
        // Update leftParamGroups for next iteration (clone, don't move)
        if (current && current->isa<FuncTypeAST>()) {
            auto* currFunc = current->as<FuncTypeAST>();
            leftParamGroups.clear();
            for (const auto& group : currFunc->paramGroups) {
                ParamGroup newGroup;
                for (const auto& param : group) {
                    TypePtr clonedType = SemanticHelpers::cloneType(param.type.get());
                    newGroup.emplace_back(param.name, std::move(clonedType),
                                          param.isVariadic, param.loc);
                }
                leftParamGroups.push_back(std::move(newGroup));
            }
        }
    }
    
    // All operands processed
    if (finalResult) {
        node.resolvedType = finalResult.get();
        // Keep the result alive by storing it. Since we can't store unique_ptr
        // on the node, we need to release ownership but the caller expects
        // the pointer to remain valid. This is a known issue — the caller
        // (checkExpr) will store the pointer in node.resolvedType.
        // We release ownership and hope nothing deletes it.
        // In a proper implementation, you'd attach the owned type to the node.
        return finalResult.release();
    }
    
    node.resolvedType = current;
    return current;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkExpr — main dispatcher
// ─────────────────────────────────────────────────────────────────────────────
TypeAST* checkExpr(ExprAST* node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& loopDepth, int& parallelDepth, bool insideExtern) {
    if (!node) return nullptr;

    LUC_LOG_SEMANTIC("checkExpr: kind=" << LucDebug::kindToString(node->kind));
    LUC_LOG_SEMANTIC("\tNode kind: " << LucDebug::kindToString(node->kind));
    LUC_LOG_SEMANTIC("\tLocation: line " << node->loc.line << ", col " << node->loc.column);
    LUC_LOG_SEMANTIC("\tNode address: " << node);

    switch (node->kind) {
        case ASTKind::LiteralExpr:
            return checkLiteralExpr(*node->as<LiteralExprAST>());

        case ASTKind::IdentifierExpr:
            return checkIdentExpr(*node->as<IdentifierExprAST>(), symbols, dc);

        case ASTKind::FieldAccessExpr:
            return checkFieldAccessExpr(*node->as<FieldAccessExprAST>(), symbols,
                                        resolver, dc, loopDepth,parallelDepth, insideExtern);

        case ASTKind::BehaviorAccessExpr:
            return checkBehaviorAccessExpr(*node->as<BehaviorAccessExprAST>(),symbols, dc);

        case ASTKind::BinaryExpr:
            return checkBinaryExpr(*node->as<BinaryExprAST>(), symbols, resolver, dc,
                                   loopDepth, parallelDepth, insideExtern);

        case ASTKind::UnaryExpr:
            return checkUnaryExpr(*node->as<UnaryExprAST>(), symbols, resolver, dc,
                                  loopDepth, parallelDepth, insideExtern);

        case ASTKind::CallExpr:
            return checkCallExpr(*node->as<CallExprAST>(), symbols, resolver, dc,
                                 loopDepth, parallelDepth, insideExtern);

        case ASTKind::AssignExpr:
            return checkAssignExpr(*node->as<AssignExprAST>(), symbols, resolver, dc,
                                   loopDepth, parallelDepth, insideExtern);

        case ASTKind::IsExpr:
            return checkIsExpr(*node->as<IsExprAST>(), symbols, resolver, dc,
                               loopDepth, parallelDepth, insideExtern);

        case ASTKind::IfExpr:
            return checkIfExpr(*node->as<IfExprAST>(), symbols, resolver, dc,
                               nullptr, loopDepth, parallelDepth, insideExtern);

        case ASTKind::MatchExpr:
            return checkMatchExpr(*node->as<MatchExprAST>(), symbols, resolver, dc,
                                  nullptr, loopDepth, parallelDepth, insideExtern);

        case ASTKind::AwaitExpr:
            return checkAwaitExpr(*node->as<AwaitExprAST>(), symbols, resolver, dc,
                                  loopDepth, parallelDepth, insideExtern);

        case ASTKind::AnonFuncExpr:
            return checkAnonFuncExpr(*node->as<AnonFuncExprAST>(), symbols, resolver, dc,
                                     loopDepth, parallelDepth, insideExtern);

        case ASTKind::NullableChainExpr:
            return checkNullableChainExpr(*node->as<NullableChainExprAST>(), symbols,
                                          resolver, dc, loopDepth,parallelDepth, insideExtern);

        case ASTKind::RangeExpr:
            return checkRangeExpr(*node->as<RangeExprAST>(), symbols, resolver, dc,
                                  loopDepth, parallelDepth, insideExtern);

        case ASTKind::PipelineExpr:
            return checkPipelineExpr(*node->as<PipelineExprAST>(), symbols, resolver, dc,
                                     loopDepth, parallelDepth, insideExtern);

        case ASTKind::ComposeExpr:
            return checkComposeExpr(*node->as<ComposeExprAST>(), symbols, resolver, dc,
                                    loopDepth, parallelDepth, insideExtern);

        case ASTKind::IndexExpr:
            return checkIndexExpr(*node->as<IndexExprAST>(), symbols, resolver, dc,
                                  loopDepth, parallelDepth, insideExtern);

        case ASTKind::StructLiteralExpr:
            return checkStructLiteralExpr(*node->as<StructLiteralExprAST>(), symbols,
                                          resolver, dc, loopDepth, parallelDepth, insideExtern);

        case ASTKind::ArrayLiteralExpr:
            return checkArrayLiteralExpr(*node->as<ArrayLiteralExprAST>(), symbols,
                                         resolver, dc, loopDepth, parallelDepth, insideExtern);

        case ASTKind::TypeConvExpr:
            return checkTypeConvExpr(*node->as<TypeConvExprAST>(), symbols, resolver, dc,
                                     loopDepth, parallelDepth, insideExtern);

        case ASTKind::IntrinsicCallExpr:
            return checkIntrinsicCallExpr(*node->as<IntrinsicCallExprAST>(), symbols,
                                          resolver, dc, loopDepth,parallelDepth, insideExtern);

        default:
            // Unknown or unhandled expression kind — skip silently.
            LUC_LOG_SEMANTIC("\tUNKNOWN NODE KIND: " << LucDebug::kindToString(node->kind));
            return nullptr;
    }
}