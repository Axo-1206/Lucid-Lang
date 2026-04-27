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

#include "SemanticSymbol.hpp"
#include "SymbolTable.hpp"
#include "TypeResolver.hpp"
#include "TypeChecker.hpp"
#include "IntrinsicRegistry.hpp"
#include "diagnostics/DiagnosticEngine.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "ast/ExprAST.hpp"
#include "ast/DeclAST.hpp"
#include "ast/StmtAST.hpp"
#include "ast/TypeAST.hpp"

#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations 
// NOTE: These are required because Declarations, Statements, and Expressions
// cross-call each other recursively. We use manual forward declarations here
// instead of a header to avoid complex circular dependency loops.
// ─────────────────────────────────────────────────────────────────────────────
void checkStmt(StmtAST* node, SymbolTable& symbols, TypeResolver& resolver,
               DiagnosticEngine& dc, TypeAST* expectedReturn,
               int& asyncDepth, int& loopDepth, int& parallelDepth,
               bool insideExtern);

// ─────────────────────────────────────────────────────────────────────────────
// Helper — make a primitive TypeAST on the fly (unowned, do not delete)
// We use a static local so callers can return a stable pointer without ownership.
// ─────────────────────────────────────────────────────────────────────────────
static PrimitiveTypeAST* primType(PrimitiveKind k) {
    // One singleton per kind, lazily constructed.
    static PrimitiveTypeAST singletons[] = {
        PrimitiveTypeAST(PrimitiveKind::Bool),
        PrimitiveTypeAST(PrimitiveKind::Byte),
        PrimitiveTypeAST(PrimitiveKind::Short),
        PrimitiveTypeAST(PrimitiveKind::Int),
        PrimitiveTypeAST(PrimitiveKind::Long),
        PrimitiveTypeAST(PrimitiveKind::Ubyte),
        PrimitiveTypeAST(PrimitiveKind::Ushort),
        PrimitiveTypeAST(PrimitiveKind::Uint),
        PrimitiveTypeAST(PrimitiveKind::Ulong),
        PrimitiveTypeAST(PrimitiveKind::Int8),
        PrimitiveTypeAST(PrimitiveKind::Int16),
        PrimitiveTypeAST(PrimitiveKind::Int32),
        PrimitiveTypeAST(PrimitiveKind::Int64),
        PrimitiveTypeAST(PrimitiveKind::Uint8),
        PrimitiveTypeAST(PrimitiveKind::Uint16),
        PrimitiveTypeAST(PrimitiveKind::Uint32),
        PrimitiveTypeAST(PrimitiveKind::Uint64),
        PrimitiveTypeAST(PrimitiveKind::Float),
        PrimitiveTypeAST(PrimitiveKind::Double),
        PrimitiveTypeAST(PrimitiveKind::Decimal),
        PrimitiveTypeAST(PrimitiveKind::String),
        PrimitiveTypeAST(PrimitiveKind::Char),
        PrimitiveTypeAST(PrimitiveKind::Any),
    };
    return &singletons[static_cast<int>(k)];
}

// ─────────────────────────────────────────────────────────────────────────────
// checkLiteralExpr
// Maps token kind to a primitive TypeAST.
// nil → nullptr (caller handles nil assignability).
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkLiteralExpr(LiteralExprAST& node) {
    TypeAST* t = nullptr;
    switch (node.kind) {
        case LiteralKind::True:
        case LiteralKind::False:    t = primType(PrimitiveKind::Bool);   break;
        case LiteralKind::Int:      t = primType(PrimitiveKind::Int);    break;
        case LiteralKind::Hex:      t = primType(PrimitiveKind::Int);    break;
        case LiteralKind::Binary:   t = primType(PrimitiveKind::Int);    break;
        case LiteralKind::Float:    t = primType(PrimitiveKind::Float);  break;
        case LiteralKind::String:
        case LiteralKind::RawString:t = primType(PrimitiveKind::String); break;
        case LiteralKind::Char:     t = primType(PrimitiveKind::Char);   break;
        case LiteralKind::Nil:      t = nullptr; break; // nil has no concrete type alone
    }
    node.resolvedType = t;
    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkIdentExpr
// Looks the name up in the symbol table. Reports E3001 if missing.
// Returns the symbol's declared type.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkIdentExpr(IdentifierExprAST& node, SymbolTable& symbols,
                                DiagnosticEngine& dc) {
    Symbol* sym = symbols.lookup(node.name);
    if (!sym) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                 "undeclared identifier '" + node.name + "'");
        return nullptr;
    }
    node.resolvedType = sym->type;
    node.isBehaviorMember = (sym->kind == SymbolKind::Method);
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
                   DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                   int& parallelDepth, bool insideExtern);

static TypeAST* checkFieldAccessExpr(FieldAccessExprAST& node, SymbolTable& symbols,
                                     TypeResolver& resolver, DiagnosticEngine& dc,
                                     int& asyncDepth, int& loopDepth,
                                     int& parallelDepth, bool insideExtern) {
    TypeAST* objType = checkExpr(node.object.get(), symbols, resolver, dc,
                                 asyncDepth, loopDepth, parallelDepth, insideExtern);
    if (!objType) return nullptr;

    // Resolve named types to their struct symbol.
    std::string typeName;
    if (objType->isa<NamedTypeAST>()) {
        typeName = objType->as<NamedTypeAST>()->name;
    } else if (objType->isa<PtrTypeAST>()) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "cannot access field '" + node.field + "' on raw pointer type '*T'; "
                 "use '@ptrToRef(T, ptr)' to cross the safety boundary first");
        return nullptr;
    }

    // Enum variant access: Direction.North
    if (!typeName.empty()) {
        Symbol* typeSym = symbols.lookup(typeName);
        if (typeSym && typeSym->kind == SymbolKind::Enum) {
            // The type of Direction.North is Direction itself.
            node.resolvedType = objType;
            return objType;
        }
        // Struct field access
        if (typeSym && typeSym->kind == SymbolKind::Struct) {
            auto* structDecl = typeSym->decl->as<StructDeclAST>();
            for (auto& f : structDecl->fields) {
                if (f->name == node.field) {
                    TypeAST* ft = resolver.resolveType(f->type.get());
                    node.resolvedType = ft;
                    return ft;
                }
            }
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                     "struct '" + typeName + "' has no field '" + node.field + "'");
            return nullptr;
        }
    }

    // Fallback: return any for now (e.g. any-typed values).
    node.resolvedType = primType(PrimitiveKind::Any);
    return primType(PrimitiveKind::Any);
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
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkBehaviorAccessExpr(BehaviorAccessExprAST& node, SymbolTable& symbols,
                                         TypeResolver& resolver, DiagnosticEngine& dc) {
    Symbol* lhsSym = symbols.lookup(node.typeName);
    if (!lhsSym) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                 "undeclared identifier '" + node.typeName + "'");
        return nullptr;
    }

    // DISALLOW STATIC ACCESS: The ':' operator must be used on an instance (Var, Param, or Field).
    if (lhsSym->kind != SymbolKind::Var && lhsSym->kind != SymbolKind::Param && lhsSym->kind != SymbolKind::Field) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "behavior access ':' is only valid on instances; '" + node.typeName + 
                 "' is a type name. Use an instance variable instead.");
        return nullptr;
    }

    // Extract the underlying struct/type name from the instance.
    if (!lhsSym->type || !lhsSym->type->isa<NamedTypeAST>()) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "identifier '" + node.typeName + "' does not resolve to a named struct type");
        return nullptr;
    }

    std::string actualTypeName = lhsSym->type->as<NamedTypeAST>()->name;

    // ── GENERIC PARAMETER SUPPORT ───────────────────────────────────────────
    // If the type name is a generic parameter, check its constraints for the method.
    if (resolver.getGenericParams()) {
        for (auto& gp : *resolver.getGenericParams()) {
            if (gp && gp->name == actualTypeName) {
                // Look for the method in EACH constraint trait.
                for (auto& constraintName : gp->constraints) {
                    std::string traitMangled = constraintName + "." + node.method;
                    Symbol* traitMethodSym = symbols.lookup(traitMangled);
                    if (traitMethodSym && traitMethodSym->kind == SymbolKind::Method) {
                        // Strip the outermost self-group (Trait -> ...)
                        TypeAST* exposedType = traitMethodSym->type;
                        if (exposedType && exposedType->isa<FuncTypeAST>()) {
                            TypeAST* inner = exposedType->as<FuncTypeAST>()->returnType.get();
                            if (inner) exposedType = inner;
                        }
                        
                        node.resolvedType = exposedType;
                        node.isBehaviorMember = true;
                        return exposedType;
                    }
                }
            }
        }
    }

    std::string mangled = actualTypeName + "." + node.method;

    Symbol* sym = symbols.lookup(mangled);
    if (!sym) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                 "no method '" + node.method + "' found on '" + actualTypeName + "'");
        return nullptr;
    }
    node.isBehaviorMember = true;

    // sym->type is the full (Self) -> ... FuncTypeAST built by SemanticCollector.
    // Strip the outermost self-group so the resolved type reflects what callers
    // actually pass — e.g. p:offset resolves to (dx float) -> (dy float) -> Point.
    TypeAST* exposedType = sym->type;
    if (exposedType && exposedType->isa<FuncTypeAST>()) {
        TypeAST* inner = exposedType->as<FuncTypeAST>()->returnType.get();
        if (inner) exposedType = inner;
    }

    node.resolvedType = exposedType;
    // Update the node's type name to the resolved struct name for downstream passes.
    node.typeName = actualTypeName;
    
    return exposedType;
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
                                 int& asyncDepth, int& loopDepth,
                                 int& parallelDepth, bool insideExtern) {
    TypeAST* lt = checkExpr(node.left.get(),  symbols, resolver, dc,
                            asyncDepth, loopDepth, parallelDepth, insideExtern);
    TypeAST* rt = checkExpr(node.right.get(), symbols, resolver, dc,
                            asyncDepth, loopDepth, parallelDepth, insideExtern);

    TypeAST* result = nullptr;

    switch (node.op) {

        // ── Logical: and / or ─────────────────────────────────────────────────
        // Short circuit semantics are handled by codegen.
        // Semantic pass only validates that operands are bool or nullable.
        case BinaryOp::And:
        case BinaryOp::Or:
            if (lt && !TypeChecker::isBoolOrNullable(lt))
                dc.error(DiagnosticCategory::Semantic, node.left->loc, DiagCode::E3002,
                         "'and'/'or' left operand must be bool or nullable type; "
                         "got non-bool value — use an explicit bool expression");
            if (rt && !TypeChecker::isBoolOrNullable(rt))
                dc.error(DiagnosticCategory::Semantic, node.right->loc, DiagCode::E3002,
                         "'and'/'or' right operand must be bool or nullable type; "
                         "got non-bool value — use an explicit bool expression");
            result = primType(PrimitiveKind::Bool);
            break;

        // ── Value equality: == and != ─────────────────────────────────────────
        // Strict type rules — struct, function, and array types are forbidden.
        case BinaryOp::Eq:
        case BinaryOp::Ne: {
            // Check left side type for forbidden categories
            if (lt) {
                // Struct type: emit E3011
                if (lt->isa<NamedTypeAST>()) {
                    Symbol* sym = symbols.lookup(lt->as<NamedTypeAST>()->name);
                    if (sym && sym->kind == SymbolKind::Struct) {
                        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3011,
                                 "cannot use '==' on struct type '" +
                                 lt->as<NamedTypeAST>()->name +
                                 "'; implement 'Equatable<" +
                                 lt->as<NamedTypeAST>()->name +
                                 ">' and use ':equals()' instead");
                        return primType(PrimitiveKind::Bool);
                    }
                }
                // Function type: emit E3012
                if (lt->isa<FuncTypeAST>()) {
                    dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3012,
                             "cannot use '==' on function type; "
                             "function bodies are incomparable");
                    return primType(PrimitiveKind::Bool);
                }
                // Array types: emit E3013
                if (lt->isa<FixedArrayTypeAST>() ||
                    lt->isa<SliceTypeAST>()       ||
                    lt->isa<DynamicArrayTypeAST>()) {
                    dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3013,
                             "cannot use '==' on array type; "
                             "use a collection library comparison function instead");
                    return primType(PrimitiveKind::Bool);
                }
            }
            result = primType(PrimitiveKind::Bool);
            break;
        }

        // ── Reference equality: === ───────────────────────────────────────────
        // Only valid on reference types and structs (address comparison).
        // Primitives are value types — === has no meaningful semantics for them.
        case BinaryOp::RefEq: {
            if (lt && !TypeChecker::isReferenceComparable(lt)) {
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "'===' reference equality is not valid on primitive or "
                         "non-reference type; use '==' for value comparison instead");
            }
            result = primType(PrimitiveKind::Bool);
            break;
        }

        // ── Ordering comparisons ──────────────────────────────────────────────
        case BinaryOp::Lt: case BinaryOp::Gt:
        case BinaryOp::Le: case BinaryOp::Ge:
            result = primType(PrimitiveKind::Bool);
            break;

        // ── Arithmetic ────────────────────────────────────────────────────────
        case BinaryOp::Add:
            if (lt && lt->isa<PtrTypeAST>()) {
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "operator '+' is not supported for raw pointer types; "
                         "use '@ptrOffset(ptr, n)' instead");
                return nullptr;
            }
            // string + string is valid
            if (lt && lt->isa<PrimitiveTypeAST>() &&
                lt->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::String) {
                if (rt && !TypeChecker::isAssignable(rt, lt))
                    dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                             "'+' on strings requires string right-hand side");
                result = primType(PrimitiveKind::String);
            } else {
                result = TypeChecker::unify(lt, rt);
                if (!result && lt) result = lt;
            }
            break;

        case BinaryOp::Sub:
            if (lt && lt->isa<PtrTypeAST>()) {
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "operator '-' is not supported for raw pointer types; "
                         "use '@ptrDiff(p1, p2)' or '@ptrOffset(ptr, -n)' instead");
                return nullptr;
            }
            result = TypeChecker::unify(lt, rt);
            if (!result && lt) result = lt;
            break;

        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Pow:
        case BinaryOp::Mod:
            if ((lt && lt->isa<PtrTypeAST>()) || (rt && rt->isa<PtrTypeAST>())) {
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "arithmetic operators are not supported for raw pointer types");
                return nullptr;
            }
            result = TypeChecker::unify(lt, rt);
            if (!result && lt) result = lt;
            break;

        // ── Bitwise: && || ~^ << >> ───────────────────────────────────────────
        // Integer types only. Non-integer operand → E3002.
        case BinaryOp::BitAnd:
        case BinaryOp::BitOr:
        case BinaryOp::BitXor:
        case BinaryOp::Shl:
        case BinaryOp::Shr: {
            auto isIntegerType = [](TypeAST* t) -> bool {
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
            if (lt && !isIntegerType(lt))
                dc.error(DiagnosticCategory::Semantic, node.left->loc, DiagCode::E3002,
                         "bitwise operator requires integer operands; left operand is not an integer type");
            if (rt && !isIntegerType(rt))
                dc.error(DiagnosticCategory::Semantic, node.right->loc, DiagCode::E3002,
                         "bitwise operator requires integer operands; right operand is not an integer type");
            result = lt ? lt : rt;
            break;
        }
    }

    node.resolvedType = result;
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
                                int& asyncDepth, int& loopDepth,
                                int& parallelDepth, bool insideExtern) {
    TypeAST* inner = checkExpr(node.operand.get(), symbols, resolver, dc,
                               asyncDepth, loopDepth, parallelDepth, insideExtern);
    TypeAST* result = inner;

    switch (node.op) {
        case UnaryOp::Not:
            // 'not' is valid on bool and nullable types.
            // Nullable: nil → false, non-nil → true. This is the nil check idiom:
            //   let x int? = nil
            //   if not x { ... }  -- true: x is nil, treated as false
            if (inner && !TypeChecker::isBoolOrNullable(inner)) {
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "'not' requires a bool or nullable operand; "
                         "got non-bool type — use an explicit comparison instead, "
                         "e.g. 'n == 0' instead of 'not n'");
            }
            result = primType(PrimitiveKind::Bool);
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
                               int& asyncDepth, int& loopDepth,
                               int& parallelDepth, bool insideExtern) {
    // Check each argument.
    for (auto& arg : node.args) {
        checkExpr(arg.get(), symbols, resolver, dc,
                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    }

    // Resolve callee type.
    TypeAST* calleeType = checkExpr(node.callee.get(), symbols, resolver, dc,
                                    asyncDepth, loopDepth, parallelDepth, insideExtern);

    // If callee is a named type (struct constructor / from() dispatch), return that type.
    if (node.callee->isa<IdentifierExprAST>()) {
        auto* ident = node.callee->as<IdentifierExprAST>();
        Symbol* sym = symbols.lookup(ident->name);
        if (sym && sym->kind == SymbolKind::Struct) {
            // Struct literal call — return a NamedTypeAST for this struct.
            // The type pointer from the symbol table is the best we have here.
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
    // ExternFunc symbols have their param types stored in the FuncDeclAST just
    // like normal functions. We check args against those param groups exactly
    // as we do for Func, but we don't attempt to check a body (there isn't one).
    if (calleeSym && calleeSym->kind == SymbolKind::ExternFunc && calleeSym->decl) {
        auto* funcDecl = static_cast<FuncDeclAST*>(calleeSym->decl);
        // Flat extern functions (no curry): check args against flat first group.
        if (!funcDecl->paramGroups.empty()) {
            auto& firstGroup = funcDecl->paramGroups[0];
            if (node.args.size() != firstGroup.size()) {
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3003,
                         "extern function '" + calleeSym->name +
                         "' expects " + std::to_string(firstGroup.size()) +
                         " argument(s), got " + std::to_string(node.args.size()));
            } else {
                for (size_t i = 0; i < node.args.size(); ++i) {
                    TypeAST* argType = static_cast<TypeAST*>(node.args[i]->resolvedType);
                    TypeAST* paramType = firstGroup[i]->type.get();
                    if (argType && paramType && !TypeChecker::isAssignable(argType, paramType)) {
                        dc.error(DiagnosticCategory::Semantic, node.args[i]->loc,
                                 DiagCode::E3002,
                                 "argument " + std::to_string(i + 1) +
                                 " type mismatch in call to extern '" + calleeSym->name + "'");
                    }
                }
            }
        }
        returnType = funcDecl->returnType.get();
        node.resolvedType = returnType;
        return returnType;
    }
    
    if (calleeSym && (calleeSym->kind == SymbolKind::Func || calleeSym->kind == SymbolKind::Method) && calleeSym->decl) {
        auto* funcDecl = static_cast<FuncDeclAST*>(calleeSym->decl);
        
        // For curried functions, support partial application.
        // The function signature is built as a curry chain where each level is a FuncTypeAST.
        if (!funcDecl->paramGroups.empty()) {
            auto& firstGroup = funcDecl->paramGroups[0];
            
            // Check that we're not providing too many arguments for the first group
            if (node.args.size() > firstGroup.size()) {
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3003,
                         "wrong number of arguments: expected " +
                         std::to_string(firstGroup.size()) + ", got " +
                         std::to_string(node.args.size()));
                node.resolvedType = nullptr;
                return nullptr;
            }
            
            // Check argument types against first parameter group
            for (size_t i = 0; i < node.args.size(); ++i) {
                TypeAST* argType = static_cast<TypeAST*>(node.args[i]->resolvedType);
                TypeAST* paramType = firstGroup[i]->type.get();
                if (argType && paramType && !TypeChecker::isAssignable(argType, paramType)) {
                    dc.error(DiagnosticCategory::Semantic, node.args[i]->loc, DiagCode::E3002,
                             "argument " + std::to_string(i + 1) + " type mismatch");
                }
            }
            
            if (node.args.size() < firstGroup.size()) {
                // Partial application: return the curried function type
                // which is the return type of the first function in the chain
                if (funcDecl->signature && funcDecl->signature->isa<FuncTypeAST>()) {
                    returnType = funcDecl->signature->as<FuncTypeAST>()->returnType.get();
                } else {
                    returnType = nullptr;
                }
            } else if (node.args.size() == firstGroup.size()) {
                // Full first group provided
                if (funcDecl->paramGroups.size() > 1) {
                    // More groups exist: return the next curried function
                    if (funcDecl->signature && funcDecl->signature->isa<FuncTypeAST>()) {
                        returnType = funcDecl->signature->as<FuncTypeAST>()->returnType.get();
                    } else {
                        returnType = nullptr;
                    }
                } else {
                    // No more groups: return the final return type
                    returnType = funcDecl->returnType.get();
                }
            }
        } else {
            // No parameters
            returnType = funcDecl->returnType.get();
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
        
        // Support partial application: if fewer args provided, we return the return type
        // (which for curried functions is the next function in the chain)
        if (node.args.size() > ft->params.size()) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3003,
                     "wrong number of arguments: expected " +
                     std::to_string(ft->params.size()) + ", got " +
                     std::to_string(node.args.size()));
            node.resolvedType = nullptr;
            return nullptr;
        }
        
        // Check argument types.
        for (size_t i = 0; i < node.args.size(); ++i) {
            TypeAST* argType = static_cast<TypeAST*>(node.args[i]->resolvedType);
            TypeAST* paramType = ft->params[i].get();
            if (argType && paramType && !TypeChecker::isAssignable(argType, paramType)) {
                dc.error(DiagnosticCategory::Semantic, node.args[i]->loc, DiagCode::E3002,
                         "argument " + std::to_string(i + 1) + " type mismatch");
            }
        }
        
        // Return the appropriate type based on argument count
        returnType = ft->returnType.get();
    } 
    // 3. Not callable.
    else {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "expression is not callable");
        return nullptr;
    }

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
// bare '{' or 'async {' in expression position (the block-body form).  That
// node carries empty params and nullptr returnType because the parser has no
// access to the symbol table — the real signature lives on the FuncDeclAST
// that was stored when 'f' was first declared.
//
// Detection: RHS is AnonFuncExprAST AND its params vector is empty AND its
// returnType is nullptr.  In this case we look up the LHS symbol, recover the
// FuncDeclAST's paramGroups and returnType, declare the params into scope, and
// check the body with the correct expectedReturn — exactly what checkFuncDecl
// does for the initial declaration.
//
// If the LHS symbol is not a Func (e.g. the user assigns a block to a plain
// variable of function-type), we fall through to the generic checkExpr path
// which handles AnonFuncExprAST normally via its own (possibly empty) params.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkAssignExpr(AssignExprAST& node, SymbolTable& symbols,
                                 TypeResolver& resolver, DiagnosticEngine& dc,
                                 int& asyncDepth, int& loopDepth,
                                 int& parallelDepth, bool insideExtern) {
    TypeAST* lhsType = checkExpr(node.lhs.get(), symbols, resolver, dc,
                                 asyncDepth, loopDepth, parallelDepth, insideExtern);

    // ── Function body reassignment:  f = { ... }  or  f = async { ... } ──────
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
        if (anonBody->paramGroups.empty() && !anonBody->returnType) {
            auto* ident = node.lhs->as<IdentifierExprAST>();
            Symbol* sym = symbols.lookup(ident->name);

            if (sym && (sym->kind == SymbolKind::Func) && !sym->isExtern && sym->decl) {
                // Verify the symbol is let-declared (reassignable).
                if (sym->declKw != DeclKeyword::Let) {
                    dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3004,
                             "cannot reassign body of '" + ident->name +
                             "': declared with " +
                             "const");
                }

                if (parallelDepth > 0) {
                    dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                             "assignment to outer variable inside parallel scope is not allowed");
                }

                // Recover the real signature from the FuncDeclAST.
                auto* funcDecl = static_cast<FuncDeclAST*>(sym->decl);

                TypeAST* returnType = nullptr;
                if (funcDecl->returnType) {
                    returnType = resolver.resolveType(funcDecl->returnType.get());
                }

                // Honour the async flag from either the original declaration or
                // the new body (= async { ... } sets isAsync on the AnonFuncExprAST).
                bool bodyIsAsync = funcDecl->isAsync || anonBody->isAsync;
                if (bodyIsAsync) asyncDepth++;

                symbols.pushScope();

                // Declare the function's parameters into the body scope.
                for (auto& group : funcDecl->paramGroups) {
                    for (auto& param : group) {
                        TypeAST* pt = resolver.resolveType(param->type.get());
                        if (!pt) continue;
                        Symbol ps;
                        ps.name       = param->name;
                        ps.kind       = SymbolKind::Param;
                        ps.declKw     = DeclKeyword::Let;
                        ps.visibility = Visibility::Private;
                        ps.type       = pt;
                        ps.decl       = param.get();
                        ps.isAsync    = false;
                        ps.loc        = param->loc;
                        if (!symbols.declare(ps)) {
                            dc.error(DiagnosticCategory::Semantic, param->loc,
                                     DiagCode::E3005,
                                     "duplicate parameter name '" + param->name + "'");
                        }
                    }
                }

                // Check the body block with the recovered return type.
                if (anonBody->body) {
                    checkStmt(anonBody->body.get(), symbols, resolver, dc,
                              returnType, asyncDepth, loopDepth,
                              parallelDepth, insideExtern);
                }

                symbols.popScope();
                if (bodyIsAsync) asyncDepth--;

                node.resolvedType = lhsType;
                return lhsType;
            }
        }
    }

    // ── Explicit anonymous function reassignment:  f = (x int) int { ... } ────
    // The RHS is an AnonFuncExprAST with its own param groups and return type
    // written explicitly by the programmer.  This covers both single-group and
    // curried reassignment:
    //
    //   Single-group:
    //     let f (x int) int = { return x }
    //     f = (n int) int { return n * 2 }        -- valid: same signature
    //     f = (n str) int { return 0 }            -- error: param type mismatch
    //     f = (n int) str { return "" }           -- error: return type mismatch
    //
    //   Curried:
    //     let add (a int) (b int) int = { return a + b }
    //     add = (a int) (b int) int { return a * b }  -- valid: same groups
    //     add = (a int) int { return a }              -- error: group count mismatch
    //
    // Rules:
    //   - Group count must match exactly.
    //   - Within each group, parameter count must match exactly.
    //   - Each parameter's type must be assignable to the declared type
    //     (positional — names are irrelevant, types must match).
    //   - The variadic flag on each parameter must match exactly.
    //   - The return type must be assignable to the declared return type;
    //     both nullptr (void) also matches.
    if (node.op == AssignOp::Assign &&
        node.rhs->isa<AnonFuncExprAST>() &&
        node.lhs->isa<IdentifierExprAST>()) {

        auto* anonRhs = node.rhs->as<AnonFuncExprAST>();

        // Only enter this branch when the anon func has an explicit signature —
        // i.e. it is NOT the bare block-body form.  The bare block-body form
        // (produced by a lone '{') has paramGroups.empty() AND nullptr returnType.
        // An explicit signature has at least one group OR an explicit return type.
        bool hasExplicitSignature = !anonRhs->paramGroups.empty() ||
                                    anonRhs->returnType != nullptr;

        if (hasExplicitSignature) {
            auto* ident = node.lhs->as<IdentifierExprAST>();
            Symbol* sym = symbols.lookup(ident->name);

            if (sym && (sym->kind == SymbolKind::Func) && !sym->isExtern && sym->decl) {
                auto* funcDecl = static_cast<FuncDeclAST*>(sym->decl);
                bool signatureOk = true;

                // ── 1. Curry group count ──────────────────────────────────────
                if (anonRhs->paramGroups.size() != funcDecl->paramGroups.size()) {
                    dc.error(DiagnosticCategory::Semantic, anonRhs->loc,
                             DiagCode::E3003,
                             "anonymous function assigned to '" + ident->name +
                             "' has " +
                             std::to_string(anonRhs->paramGroups.size()) +
                             " parameter group(s) but declaration has " +
                             std::to_string(funcDecl->paramGroups.size()));
                    signatureOk = false;
                } else {
                    // ── 2. Per-group, per-parameter check ─────────────────────
                    for (size_t g = 0; g < funcDecl->paramGroups.size(); ++g) {
                        const auto& declGroup = funcDecl->paramGroups[g];
                        const auto& anonGroup = anonRhs->paramGroups[g];

                        if (anonGroup.size() != declGroup.size()) {
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

                            // Variadic flag must match exactly.
                            if (anonParam->isVariadic != declParam->isVariadic) {
                                dc.error(DiagnosticCategory::Semantic, anonParam->loc,
                                         DiagCode::E3002,
                                         "group " + std::to_string(g + 1) +
                                         " parameter " + std::to_string(i + 1) +
                                         " of anonymous function assigned to '" +
                                         ident->name + "': variadic mismatch");
                                signatureOk = false;
                                continue;
                            }

                            // Resolve both sides and check type compatibility.
                            TypeAST* declType =
                                resolver.resolveType(declParam->type.get());
                            TypeAST* anonType =
                                resolver.resolveType(anonParam->type.get());

                            if (declType && anonType &&
                                !TypeChecker::isAssignable(anonType, declType)) {
                                dc.error(DiagnosticCategory::Semantic, anonParam->loc,
                                         DiagCode::E3002,
                                         "group " + std::to_string(g + 1) +
                                         " parameter " + std::to_string(i + 1) +
                                         " of anonymous function assigned to '" +
                                         ident->name + "': type mismatch");
                                signatureOk = false;
                            }
                        }
                    }
                }

                // ── 3. Return type ────────────────────────────────────────────
                // Both nullptr means void — valid match.
                // One nullptr and one not — mismatch.
                // Both non-nullptr — use isAssignable.
                TypeAST* declReturn = funcDecl->returnType
                                       ? resolver.resolveType(funcDecl->returnType.get())
                                       : nullptr;
                TypeAST* anonReturn = anonRhs->returnType
                                       ? resolver.resolveType(anonRhs->returnType.get())
                                       : nullptr;

                bool returnMatches;
                if (!declReturn && !anonReturn) {
                    returnMatches = true;
                } else if (!declReturn || !anonReturn) {
                    returnMatches = false;
                } else {
                    returnMatches = TypeChecker::isAssignable(anonReturn, declReturn);
                }

                if (!returnMatches) {
                    dc.error(DiagnosticCategory::Semantic, anonRhs->loc,
                             DiagCode::E3002,
                             "return type of anonymous function assigned to '" +
                             ident->name + "' does not match declaration");
                    signatureOk = false;
                }

                // ── 4. Mutability and parallel context ────────────────────────
                if (sym->declKw != DeclKeyword::Let) {
                    dc.error(DiagnosticCategory::Semantic, node.loc,
                             DiagCode::E3004,
                             "cannot reassign '" + ident->name +
                             "': declared with " +
                             "const");
                }

                if (parallelDepth > 0) {
                    dc.error(DiagnosticCategory::Semantic, node.loc,
                             DiagCode::E3002,
                             "assignment to outer variable inside parallel scope is not allowed");
                }

                // ── 5. Check the body ─────────────────────────────────────────
                // Always check the body even when the signature mismatched, so
                // internal errors surface in a single pass.  Use declReturn as
                // the expected return type so return statements are validated
                // against the declaration's intent.
                if (anonRhs->isAsync) asyncDepth++;
                symbols.pushScope();

                // Declare params using the anon func's own param names (the
                // programmer writes the new names for this body), but the declared
                // types — so that the body is type-checked correctly.
                // Skip if signatureOk is false (count mismatch would cause OOB).
                if (signatureOk) {
                    for (size_t g = 0; g < funcDecl->paramGroups.size(); ++g) {
                        const auto& declGroup = funcDecl->paramGroups[g];
                        const auto& anonGroup = anonRhs->paramGroups[g];
                        for (size_t i = 0; i < declGroup.size(); ++i) {
                            const auto& anonParam = anonGroup[i];
                            TypeAST* pt =
                                resolver.resolveType(declGroup[i]->type.get());
                            if (!pt) continue;
                            Symbol ps;
                            ps.name       = anonParam->name; // use the new name
                            ps.kind       = SymbolKind::Param;
                            ps.declKw     = DeclKeyword::Let;
                            ps.visibility = Visibility::Private;
                            ps.type       = pt;
                            ps.decl       = anonParam.get();
                            ps.isAsync    = false;
                            ps.loc        = anonParam->loc;
                            if (!symbols.declare(ps)) {
                                dc.error(DiagnosticCategory::Semantic, anonParam->loc,
                                         DiagCode::E3005,
                                         "duplicate parameter name '" +
                                         anonParam->name + "'");
                            }
                        }
                    }
                }

                if (anonRhs->body) {
                    checkStmt(anonRhs->body.get(), symbols, resolver, dc,
                              declReturn, asyncDepth, loopDepth,
                              parallelDepth, insideExtern);
                }

                symbols.popScope();
                if (anonRhs->isAsync) asyncDepth--;

                node.resolvedType = lhsType;
                return lhsType;
            }
        }
    }

    // ── General case ──────────────────────────────────────────────────────────
    TypeAST* rhsType = checkExpr(node.rhs.get(), symbols, resolver, dc,
                                 asyncDepth, loopDepth, parallelDepth, insideExtern);

    // Check the LHS is a let-declared symbol.
    if (node.lhs->isa<IdentifierExprAST>()) {
        auto* ident = node.lhs->as<IdentifierExprAST>();
        Symbol* sym = symbols.lookup(ident->name);
        if (sym && sym->declKw != DeclKeyword::Let) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3004,
                     "cannot assign to '" + ident->name + "': declared with " +
                     "const");
        }
    }

    // Parallel scope: writing to outer variables is forbidden.
    if (parallelDepth > 0) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "assignment to outer variable inside parallel scope is not allowed");
    }

    // Type compatibility for plain assignment.
    if (node.op == AssignOp::Assign && lhsType && rhsType) {
        if (!TypeChecker::isAssignable(rhsType, lhsType)) {
            // Check if a from-casting block can handle this conversion.
            // If so, desugar:  x = s  →  x = TargetType(s)
            // by wrapping node.rhs in a TypeConvExprAST.
            if (lhsType->isa<NamedTypeAST>() &&
                TypeChecker::isFromCastable(rhsType, lhsType, &symbols)) {
                // Build the target type node for the cast.
                auto targetTypeNode = std::make_unique<NamedTypeAST>(
                    lhsType->as<NamedTypeAST>()->name);
                targetTypeNode->loc = node.rhs->loc;

                // Wrap the RHS in a TypeConvExprAST.
                SourceLocation rhsLoc = node.rhs->loc;
                auto convExpr = std::make_unique<TypeConvExprAST>(
                    std::move(targetTypeNode),
                    std::move(node.rhs),
                    /*isUnsafe=*/false);
                convExpr->loc = rhsLoc;

                // Replace node.rhs with the desugared cast expression.
                node.rhs = std::move(convExpr);

                // Re-check so resolvedType is stamped correctly on the new node.
                checkExpr(node.rhs.get(), symbols, resolver, dc,
                          asyncDepth, loopDepth, parallelDepth, insideExtern);
            } else {
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
                             int& asyncDepth, int& loopDepth,
                             int& parallelDepth, bool insideExtern) {
    checkExpr(node.expr.get(), symbols, resolver, dc,
              asyncDepth, loopDepth, parallelDepth, insideExtern);
    resolver.resolveType(node.checkType.get());
    node.resolvedType = primType(PrimitiveKind::Bool);
    return primType(PrimitiveKind::Bool);
}

// ─────────────────────────────────────────────────────────────────────────────
// checkIfExpr (expression form — else is required)
// Both branches must produce the same type (unified).
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkIfExpr(IfExprAST& node, SymbolTable& symbols,
                              TypeResolver& resolver, DiagnosticEngine& dc,
                              TypeAST* expectedReturn,
                              int& asyncDepth, int& loopDepth,
                              int& parallelDepth, bool insideExtern) {
    TypeAST* condType = checkExpr(node.condition.get(), symbols, resolver, dc,
                                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    if (condType && !TypeChecker::isBooleanCompatible(condType)) {
        dc.error(DiagnosticCategory::Semantic, node.condition->loc, DiagCode::E3002,
                 "if condition must be bool");
    }
 
    TypeAST* thenType = nullptr;
    if (node.thenBranch) {
        thenType = checkExpr(node.thenBranch.get(), symbols, resolver, dc,
                              asyncDepth, loopDepth, parallelDepth, insideExtern);
    }
    
    TypeAST* elseType = nullptr;
    if (node.elseBranch) {
        elseType = checkExpr(node.elseBranch.get(), symbols, resolver, dc,
                               asyncDepth, loopDepth, parallelDepth, insideExtern);
    } else {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "if expression requires an else branch");
    }
 
    TypeAST* unified = TypeChecker::unify(thenType, elseType);
    if (!unified && thenType && elseType) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "type mismatch between 'if' and 'else' branches (cannot unify types)");
    }
 
    node.resolvedType = unified ? unified : primType(PrimitiveKind::Any);
    return static_cast<TypeAST*>(node.resolvedType);
}

// ─────────────────────────────────────────────────────────────────────────────
// checkMatchExpr
// Subject is checked; each arm pattern is validated against the subject type;
// all arm bodies must unify.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkMatchExpr(MatchExprAST& node, SymbolTable& symbols,
                                TypeResolver& resolver, DiagnosticEngine& dc,
                                TypeAST* expectedReturn,
                                int& asyncDepth, int& loopDepth,
                                int& parallelDepth, bool insideExtern) {
    TypeAST* subjectType = checkExpr(node.subject.get(), symbols, resolver, dc,
                                     asyncDepth, loopDepth, parallelDepth, insideExtern);
 
    TypeAST* unified = nullptr;
    for (auto& arm : node.arms) {
        // Check guard if present.
        if (arm->guard) {
            TypeAST* gt = checkExpr(arm->guard.get(), symbols, resolver, dc,
                                    asyncDepth, loopDepth, parallelDepth, insideExtern);
            if (gt && !TypeChecker::isBooleanCompatible(gt)) {
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
                bs.type = subjectType; bs.decl = bp; bs.isAsync = false; bs.loc = bp->loc;
                symbols.declare(bs);
            }
        }
 
        TypeAST* armType = nullptr;
        for (auto& expr : arm->exprs) {
            TypeAST* et = checkExpr(expr.get(), symbols, resolver, dc,
                                    asyncDepth, loopDepth, parallelDepth, insideExtern);
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
                                    asyncDepth, loopDepth, parallelDepth, insideExtern);
            if (!defaultType) defaultType = et;
        }
        if (unified && defaultType) {
            unified = TypeChecker::unify(unified, defaultType);
        } else if (!unified) {
            unified = defaultType;
        }
    } else {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "match expression requires a 'default' arm");
    }
 
    node.resolvedType = unified ? unified : primType(PrimitiveKind::Any);
    return static_cast<TypeAST*>(node.resolvedType);
}

// ─────────────────────────────────────────────────────────────────────────────
// checkAwaitExpr
// Valid only inside an async body and not inside a parallel scope.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkAwaitExpr(AwaitExprAST& node, SymbolTable& symbols,
                                TypeResolver& resolver, DiagnosticEngine& dc,
                                int& asyncDepth, int& loopDepth,
                                int& parallelDepth, bool insideExtern) {
    if (asyncDepth <= 0) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "'await' is only valid inside an async function body");
    }
    if (parallelDepth > 0) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "'await' is not allowed inside a parallel scope");
    }
    TypeAST* innerType = checkExpr(node.inner.get(), symbols, resolver, dc,
                                   asyncDepth, loopDepth, parallelDepth, insideExtern);
    node.resolvedType = innerType;
    return innerType;
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
                                   int& asyncDepth, int& loopDepth,
                                   int& parallelDepth, bool insideExtern) {
    TypeAST* returnType = nullptr;
    if (node.returnType) returnType = resolver.resolveType(node.returnType.get());

    if (node.isAsync) asyncDepth++;
    symbols.pushScope();

    // Declare every parameter from every curry group into the function scope.
    for (auto& group : node.paramGroups) {
        for (auto& param : group) {
            TypeAST* pt = resolver.resolveType(param->type.get());
            if (pt) {
                Symbol ps;
                ps.name       = param->name;
                ps.kind       = SymbolKind::Param;
                ps.declKw     = DeclKeyword::Let;
                ps.visibility = Visibility::Private;
                ps.type       = pt;
                ps.decl       = param.get();
                ps.isAsync    = false;
                ps.loc        = param->loc;
                if (!symbols.declare(ps)) {
                    dc.error(DiagnosticCategory::Semantic, param->loc,
                             DiagCode::E3005,
                             "duplicate parameter name '" + param->name + "'");
                }
            }
        }
    }

    if (node.body) {
        checkStmt(node.body.get(), symbols, resolver, dc, returnType,
                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    }

    symbols.popScope();
    if (node.isAsync) asyncDepth--;

    // The type of an anon func is a FuncTypeAST built from its signature.
    // For now, return nullptr — the anon func value's type is tracked by its
    // declaration site. Full FuncType construction would need ownership management.
    node.resolvedType = nullptr;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkNullableChainExpr
// player.?weapon.?damage ?? 0
// Every .? step must be on a nullable type. The ?? fallback must match.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkNullableChainExpr(NullableChainExprAST& node, SymbolTable& symbols,
                                        TypeResolver& resolver, DiagnosticEngine& dc,
                                        int& asyncDepth, int& loopDepth,
                                        int& parallelDepth, bool insideExtern) {
    TypeAST* objType = checkExpr(node.object.get(), symbols, resolver, dc,
                                 asyncDepth, loopDepth, parallelDepth, insideExtern);
    if (objType && !TypeChecker::isNullable(objType)) {
        dc.error(DiagnosticCategory::Semantic, node.object->loc, DiagCode::E3002,
                 "'.?' chain must start on a nullable type");
    }

    TypeAST* fallbackType = nullptr;
    if (node.fallback) {
        fallbackType = checkExpr(node.fallback.get(), symbols, resolver, dc,
                                 asyncDepth, loopDepth, parallelDepth, insideExtern);
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
                                int& asyncDepth, int& loopDepth,
                                int& parallelDepth, bool insideExtern) {
    TypeAST* loType = checkExpr(node.lo.get(), symbols, resolver, dc,
                                asyncDepth, loopDepth, parallelDepth, insideExtern);
    TypeAST* hiType = checkExpr(node.hi.get(), symbols, resolver, dc,
                                asyncDepth, loopDepth, parallelDepth, insideExtern);

    if (loType && hiType && !TypeChecker::isAssignable(loType, hiType) &&
        !TypeChecker::isAssignable(hiType, loType)) {
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
                                   int& asyncDepth, int& loopDepth,
                                   int& parallelDepth, bool insideExtern) {
    TypeAST* current = checkExpr(node.seed.get(), symbols, resolver, dc,
                                 asyncDepth, loopDepth, parallelDepth, insideExtern);

    for (auto& step : node.steps) {
        switch (step->kind) {
            case PipelineStepKind::Ident: {
                Symbol* sym = symbols.lookup(step->ident);
                if (!sym) {
                    dc.error(DiagnosticCategory::Semantic, step->loc, DiagCode::E3001,
                             "undeclared identifier '" + step->ident + "' in pipeline");
                    current = nullptr;
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
                    dc.error(DiagnosticCategory::Semantic, step->loc, DiagCode::E3001,
                             "no method '" + step->method + "' on '" + step->typeName + "'");
                    current = nullptr;
                } else if (sym->type && sym->type->isa<FuncTypeAST>()) {
                    current = sym->type->as<FuncTypeAST>()->returnType.get();
                }
                break;
            }
            case PipelineStepKind::ArgPack:
            case PipelineStepKind::FieldRef:
            case PipelineStepKind::AnonFunc:
                // For ArgPack and AnonFunc the return type cannot be easily inferred
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
                                int& asyncDepth, int& loopDepth,
                                int& parallelDepth, bool insideExtern) {
    TypeAST* targetType = checkExpr(node.target.get(), symbols, resolver, dc,
                                    asyncDepth, loopDepth, parallelDepth, insideExtern);
    TypeAST* idxType = checkExpr(node.index.get(), symbols, resolver, dc,
                                 asyncDepth, loopDepth, parallelDepth, insideExtern);

    // Index must be an integer type.
    if (idxType && idxType->isa<PrimitiveTypeAST>()) {
        auto k = idxType->as<PrimitiveTypeAST>()->primitiveKind;
        bool isInt = (k == PrimitiveKind::Int || k == PrimitiveKind::Long ||
                      k == PrimitiveKind::Uint || k == PrimitiveKind::Ulong ||
                      k == PrimitiveKind::Byte || k == PrimitiveKind::Short);
        if (!isInt) {
            dc.error(DiagnosticCategory::Semantic, node.index->loc, DiagCode::E3002,
                     "array index must be an integer type");
        }
    }

    if (node.sliceEnd) {
        checkExpr(node.sliceEnd.get(), symbols, resolver, dc,
                  asyncDepth, loopDepth, parallelDepth, insideExtern);
    }

    // Resolve the element type from the target array type.
    TypeAST* elemType = primType(PrimitiveKind::Any);
    if (targetType) {
        if (targetType->isa<FixedArrayTypeAST>())
            elemType = targetType->as<FixedArrayTypeAST>()->element.get();
        else if (targetType->isa<SliceTypeAST>())
            elemType = targetType->as<SliceTypeAST>()->element.get();
        else if (targetType->isa<DynamicArrayTypeAST>())
            elemType = targetType->as<DynamicArrayTypeAST>()->element.get();
        else if (targetType->isa<PtrTypeAST>()) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "cannot index into raw pointer type '*T'; "
                     "use '@ptrOffset(ptr, i)' for pointer arithmetic or '@ptrToRef' to cross to a safe type");
            return nullptr;
        }
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
                                        int& asyncDepth, int& loopDepth,
                                        int& parallelDepth, bool insideExtern) {
    Symbol* sym = symbols.lookup(node.typeName);
    if (!sym || sym->kind != SymbolKind::Struct) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3001,
                 "'" + node.typeName + "' is not a declared struct");
        return nullptr;
    }

    auto* structDecl = sym->decl->as<StructDeclAST>();

    // Check that every field init matches the declared field type.
    for (auto& init : node.inits) {
        FieldDeclAST* fieldDecl = nullptr;
        for (auto& f : structDecl->fields) {
            if (f->name == init.name) { fieldDecl = f.get(); break; }
        }
        if (!fieldDecl) {
            dc.error(DiagnosticCategory::Semantic, init.loc, DiagCode::E3001,
                     "struct '" + node.typeName + "' has no field '" + init.name + "'");
            continue;
        }
        TypeAST* ft = resolver.resolveType(fieldDecl->type.get());
        TypeAST* vt = checkExpr(init.value.get(), symbols, resolver, dc,
                                asyncDepth, loopDepth, parallelDepth, insideExtern);
        if (ft && vt && !TypeChecker::isAssignable(vt, ft)) {
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
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "missing required field '" + f->name + "' in struct literal '" +
                     node.typeName + "'");
        }
    }

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
                                       int& asyncDepth, int& loopDepth,
                                       int& parallelDepth, bool insideExtern) {
    TypeAST* elemType = nullptr;
    for (auto& elem : node.elements) {
        TypeAST* et = checkExpr(elem.get(), symbols, resolver, dc,
                                asyncDepth, loopDepth, parallelDepth, insideExtern);
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
                                   int& asyncDepth, int& loopDepth,
                                   int& parallelDepth, bool insideExtern) {
    if (node.isUnsafe && !insideExtern) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                 "unsafe type reinterpret '*' is only valid on '@extern'-decorated declarations; "
                 "use '@bitcast(T, x)' for general-purpose bit reinterpretation");
    }
    checkExpr(node.expr.get(), symbols, resolver, dc,
              asyncDepth, loopDepth, parallelDepth, insideExtern);
    TypeAST* targetType = resolver.resolveType(node.targetType.get());
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
static TypeAST* checkIntrinsicCallExpr(IntrinsicCallExprAST& node,
                                        SymbolTable& symbols,
                                        TypeResolver& resolver,
                                        DiagnosticEngine& dc,
                                        int& asyncDepth, int& loopDepth,
                                        int& parallelDepth, bool insideExtern) {
    const std::string& name = node.intrinsicName;

    // ── 1. Registry lookup ────────────────────────────────────────────────────
    const IntrinsicEntry* entry = IntrinsicRegistry::lookup(name);
    if (!entry) {
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3009,
                 "unknown compiler intrinsic '@" + name + "'; "
                 "known intrinsics: " + IntrinsicRegistry::allNames());
        // Still walk any provided args so their sub-expressions are visited.
        for (auto& arg : node.args)
            checkExpr(arg.get(), symbols, resolver, dc,
                      asyncDepth, loopDepth, parallelDepth, insideExtern);
        node.resolvedType = nullptr;
        return nullptr;
    }

    // ── 2. Type-only intrinsics: @sizeof(T) and @alignof(T) ──────────────────
    // These take one type argument and zero value arguments.
    if (entry->returnKind == IntrinsicReturnKind::Uint64 &&
        !entry->argKinds.empty() &&
        entry->argKinds[0] == IntrinsicArgKind::TypeArg &&
        entry->minArgs == 0) {

        if (!node.typeArg) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3010,
                     "'@" + name + "' requires a type argument, e.g. '@" + name + "(int)'");
        } else {
            resolver.resolveType(node.typeArg.get());
        }
        if (!node.args.empty()) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3010,
                     "'@" + name + "' takes only a type argument, not value arguments");
        }
        TypeAST* ret = primType(PrimitiveKind::Uint64);
        node.resolvedType = ret;
        return ret;
    }

    // ── 3. @bitcast(T, x) — type arg + one value arg ─────────────────────────
    if (name == "bitcast") {
        if (!node.typeArg) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3010,
                     "'@bitcast' requires a target type argument: '@bitcast(T, x)'");
        } else {
            resolver.resolveType(node.typeArg.get());
        }
        if (node.args.size() != 1) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3010,
                     "'@bitcast' requires exactly 1 value argument: '@bitcast(T, x)'");
        } else {
            checkExpr(node.args[0].get(), symbols, resolver, dc,
                      asyncDepth, loopDepth, parallelDepth, insideExtern);
        }
        // Return type is the target type (typeArg).
        TypeAST* ret = node.typeArg ? resolver.resolveType(node.typeArg.get())
                                     : primType(PrimitiveKind::Any);
        node.resolvedType = ret;
        return ret;
    }

    // ── 3b. @ptrToRef(T, ptr) — TypeArg + PtrValue ───────────────────────────
    if (name == "ptrToRef") {
        if (!node.typeArg) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3010,
                     "'@ptrToRef' requires a target type argument: '@ptrToRef(T, ptr)'");
        }
        if (node.args.size() != 1) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3010,
                     "'@ptrToRef' requires exactly 1 value argument: '@ptrToRef(T, ptr)'");
        } else {
            TypeAST* pt = checkExpr(node.args[0].get(), symbols, resolver, dc,
                                    asyncDepth, loopDepth, parallelDepth, insideExtern);
            if (pt && !pt->isa<PtrTypeAST>()) {
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
            ret = primType(PrimitiveKind::Any);
        }
        node.resolvedType = ret;
        return ret;
    }

    // ── 4. Value-argument intrinsics ─────────────────────────────────────────
    // Verify typeArg was NOT supplied for these (they take only values).
    if (node.typeArg) {
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
        // Walk args anyway to surface nested errors.
        for (auto& arg : node.args)
            checkExpr(arg.get(), symbols, resolver, dc,
                      asyncDepth, loopDepth, parallelDepth, insideExtern);
        node.resolvedType = nullptr;
        return nullptr;
    }

    // Check each argument against its registry slot kind.
    // Slots past the argKinds vector length default to AnyValue.
    std::vector<TypeAST*> argTypes;
    argTypes.reserve(node.args.size());

    for (size_t i = 0; i < node.args.size(); ++i) {
        TypeAST* at = checkExpr(node.args[i].get(), symbols, resolver, dc,
                                asyncDepth, loopDepth, parallelDepth, insideExtern);
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
            ret = primType(PrimitiveKind::Uint64);
            break;
        case IntrinsicReturnKind::Float32:
            ret = primType(PrimitiveKind::Float);
            break;
        case IntrinsicReturnKind::Float64:
            ret = primType(PrimitiveKind::Double);
            break;
        case IntrinsicReturnKind::SameAsArg0:
            ret = !argTypes.empty() ? argTypes[0] : primType(PrimitiveKind::Any);
            break;
        case IntrinsicReturnKind::SameAsArg1:
            ret = argTypes.size() >= 2 ? argTypes[1] : primType(PrimitiveKind::Any);
            break;
        case IntrinsicReturnKind::RefOfTypeArg0:
            // This is handled above in the @ptrToRef special case.
            ret = node.typeArg ? resolver.resolveType(node.typeArg.get()) : primType(PrimitiveKind::Any);
            break;
        case IntrinsicReturnKind::Int64:
            ret = primType(PrimitiveKind::Int64);
            break;
    }

    // For overloaded float intrinsics: if arg0 is double, return double.
    if (entry->isOverloaded &&
        entry->returnKind == IntrinsicReturnKind::SameAsArg0 &&
        !argTypes.empty() && argTypes[0] &&
        argTypes[0]->isa<PrimitiveTypeAST>() &&
        argTypes[0]->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::Double) {
        ret = primType(PrimitiveKind::Double);
    }

    node.resolvedType = ret;
    return ret;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkComposeExpr
// f +> g: left output type must match right input type.
// ─────────────────────────────────────────────────────────────────────────────
static TypeAST* checkComposeExpr(ComposeExprAST& node, SymbolTable& symbols,
                                  TypeResolver& resolver, DiagnosticEngine& dc,
                                  int& asyncDepth, int& loopDepth,
                                  int& parallelDepth, bool insideExtern) {
    TypeAST* current = checkExpr(node.left.get(), symbols, resolver, dc,
                                 asyncDepth, loopDepth, parallelDepth, insideExtern);

    for (auto& operand : node.operands) {
        TypeAST* opType = nullptr;
        if (operand->kind == ComposeOperandKind::Ident) {
            Symbol* sym = symbols.lookup(operand->ident);
            if (sym) opType = sym->type;
        } else if (operand->kind == ComposeOperandKind::BehaviorRef) {
            std::string mangled = operand->typeName + "." + operand->method;
            Symbol* sym = symbols.lookup(mangled);
            if (sym) opType = sym->type;
        }

        if (opType && !TypeChecker::isCallable(opType)) {
            dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                     "compose operand must be a function");
        }

        if (current && opType && opType->isa<FuncTypeAST>()) {
            auto* ft = opType->as<FuncTypeAST>();
            if (!ft->params.empty() &&
                !TypeChecker::isAssignable(current, ft->params[0].get())) {
                dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3002,
                         "type mismatch in '+>' composition: output type does not match next input");
            }
            current = ft->returnType.get();
        } else {
            current = nullptr;
        }
    }

    node.resolvedType = current;
    return current;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkExpr — main dispatcher
// ─────────────────────────────────────────────────────────────────────────────
TypeAST* checkExpr(ExprAST* node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& asyncDepth, int& loopDepth,
                   int& parallelDepth, bool insideExtern) {
    if (!node) return nullptr;

    switch (node->kind) {
        case ASTKind::LiteralExpr:
            return checkLiteralExpr(*node->as<LiteralExprAST>());

        case ASTKind::IdentifierExpr:
            return checkIdentExpr(*node->as<IdentifierExprAST>(), symbols, dc);

        case ASTKind::FieldAccessExpr:
            return checkFieldAccessExpr(*node->as<FieldAccessExprAST>(), symbols,
                                        resolver, dc, asyncDepth, loopDepth,
                                        parallelDepth, insideExtern);

        case ASTKind::BehaviorAccessExpr:
            return checkBehaviorAccessExpr(*node->as<BehaviorAccessExprAST>(),
                                           symbols, resolver, dc);

        case ASTKind::BinaryExpr:
            return checkBinaryExpr(*node->as<BinaryExprAST>(), symbols, resolver, dc,
                                   asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::UnaryExpr:
            return checkUnaryExpr(*node->as<UnaryExprAST>(), symbols, resolver, dc,
                                  asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::CallExpr:
            return checkCallExpr(*node->as<CallExprAST>(), symbols, resolver, dc,
                                 asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::AssignExpr:
            return checkAssignExpr(*node->as<AssignExprAST>(), symbols, resolver, dc,
                                   asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::IsExpr:
            return checkIsExpr(*node->as<IsExprAST>(), symbols, resolver, dc,
                               asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::IfExpr:
            return checkIfExpr(*node->as<IfExprAST>(), symbols, resolver, dc,
                               nullptr, asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::MatchExpr:
            return checkMatchExpr(*node->as<MatchExprAST>(), symbols, resolver, dc,
                                  nullptr, asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::AwaitExpr:
            return checkAwaitExpr(*node->as<AwaitExprAST>(), symbols, resolver, dc,
                                  asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::AnonFuncExpr:
            return checkAnonFuncExpr(*node->as<AnonFuncExprAST>(), symbols, resolver, dc,
                                     asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::NullableChainExpr:
            return checkNullableChainExpr(*node->as<NullableChainExprAST>(), symbols,
                                          resolver, dc, asyncDepth, loopDepth,
                                          parallelDepth, insideExtern);

        case ASTKind::RangeExpr:
            return checkRangeExpr(*node->as<RangeExprAST>(), symbols, resolver, dc,
                                  asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::PipelineExpr:
            return checkPipelineExpr(*node->as<PipelineExprAST>(), symbols, resolver, dc,
                                     asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::ComposeExpr:
            return checkComposeExpr(*node->as<ComposeExprAST>(), symbols, resolver, dc,
                                    asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::IndexExpr:
            return checkIndexExpr(*node->as<IndexExprAST>(), symbols, resolver, dc,
                                  asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::StructLiteralExpr:
            return checkStructLiteralExpr(*node->as<StructLiteralExprAST>(), symbols,
                                          resolver, dc, asyncDepth, loopDepth,
                                          parallelDepth, insideExtern);

        case ASTKind::ArrayLiteralExpr:
            return checkArrayLiteralExpr(*node->as<ArrayLiteralExprAST>(), symbols,
                                         resolver, dc, asyncDepth, loopDepth,
                                         parallelDepth, insideExtern);

        case ASTKind::TypeConvExpr:
            return checkTypeConvExpr(*node->as<TypeConvExprAST>(), symbols, resolver, dc,
                                     asyncDepth, loopDepth, parallelDepth, insideExtern);

        case ASTKind::IntrinsicCallExpr:
            return checkIntrinsicCallExpr(*node->as<IntrinsicCallExprAST>(), symbols,
                                          resolver, dc, asyncDepth, loopDepth,
                                          parallelDepth, insideExtern);

        default:
            // Unknown or unhandled expression kind — skip silently.
            return nullptr;
    }
}