#include "BuiltinMethodRegistry.hpp"
#include "ast/ExprAST.hpp"
#include "semantic/header/SemanticHelpers.hpp"
#include "semantic/header/SymbolTable.hpp"  // Add this

// Forward declare checkExpr
TypeAST* checkExpr(ExprAST* node, SymbolTable& symbols, TypeResolver& resolver,
                   DiagnosticEngine& dc, int& loopDepth, int& parallelDepth, bool insideExtern);

// Individual method checkers with SymbolTable&
static BuiltinMethodResult checkArrayLen(CallExprAST& node, TypeAST* receiverType,
                                          SymbolTable& symbols, TypeResolver& resolver,
                                          DiagnosticEngine& dc, int& loopDepth,
                                          int& parallelDepth, bool insideExtern) {
    BuiltinMethodResult result;
    
    if (!node.args.empty()) {
        result.errorMessage = "'.len()' takes no arguments";
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3003, result.errorMessage);
        return result;
    }
    
    result.returnType = SemanticHelpers::getPrimitiveType(PrimitiveKind::Int);
    result.isHandled = true;
    return result;
}

static BuiltinMethodResult checkArrayIsEmpty(CallExprAST& node, TypeAST* receiverType,
                                              SymbolTable& symbols, TypeResolver& resolver,
                                              DiagnosticEngine& dc, int& loopDepth,
                                              int& parallelDepth, bool insideExtern) {
    BuiltinMethodResult result;
    
    if (!node.args.empty()) {
        result.errorMessage = "'.isEmpty()' takes no arguments";
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3003, result.errorMessage);
        return result;
    }
    
    result.returnType = SemanticHelpers::getPrimitiveType(PrimitiveKind::Bool);
    result.isHandled = true;
    return result;
}

static BuiltinMethodResult checkArrayCap(CallExprAST& node, TypeAST* receiverType,
                                          SymbolTable& symbols, TypeResolver& resolver,
                                          DiagnosticEngine& dc, int& loopDepth,
                                          int& parallelDepth, bool insideExtern) {
    BuiltinMethodResult result;
    
    if (!node.args.empty()) {
        result.errorMessage = "'.cap()' takes no arguments";
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3003, result.errorMessage);
        return result;
    }
    
    result.returnType = SemanticHelpers::getPrimitiveType(PrimitiveKind::Int);
    result.isHandled = true;
    return result;
}

static BuiltinMethodResult checkArrayFirst(CallExprAST& node, TypeAST* receiverType,
                                            SymbolTable& symbols, TypeResolver& resolver,
                                            DiagnosticEngine& dc, int& loopDepth,
                                            int& parallelDepth, bool insideExtern) {
    BuiltinMethodResult result;
    
    if (!node.args.empty()) {
        result.errorMessage = "'.first()' takes no arguments";
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3003, result.errorMessage);
        return result;
    }
    
    TypeAST* elemType = nullptr;
    if (receiverType->isa<FixedArrayTypeAST>()) {
        elemType = receiverType->as<FixedArrayTypeAST>()->element.get();
    } else if (receiverType->isa<SliceTypeAST>()) {
        elemType = receiverType->as<SliceTypeAST>()->element.get();
    } else if (receiverType->isa<DynamicArrayTypeAST>()) {
        elemType = receiverType->as<DynamicArrayTypeAST>()->element.get();
    }
    
    result.returnType = elemType;
    result.isHandled = true;
    return result;
}

static BuiltinMethodResult checkArrayLast(CallExprAST& node, TypeAST* receiverType,
                                           SymbolTable& symbols, TypeResolver& resolver,
                                           DiagnosticEngine& dc, int& loopDepth,
                                           int& parallelDepth, bool insideExtern) {
    return checkArrayFirst(node, receiverType, symbols, resolver, dc, 
                          loopDepth, parallelDepth, insideExtern);
}

static BuiltinMethodResult checkArrayPush(CallExprAST& node, TypeAST* receiverType,
                                           SymbolTable& symbols, TypeResolver& resolver,
                                           DiagnosticEngine& dc, int& loopDepth,
                                           int& parallelDepth, bool insideExtern) {
    BuiltinMethodResult result;
    
    if (node.args.size() != 1) {
        result.errorMessage = "'.push()' expects exactly one argument";
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3003, result.errorMessage);
        return result;
    }
    
    TypeAST* elemType = nullptr;
    if (receiverType->isa<DynamicArrayTypeAST>()) {
        elemType = receiverType->as<DynamicArrayTypeAST>()->element.get();
    }
    
    if (elemType) {
        // Now passing the correct symbol table reference
        TypeAST* argType = checkExpr(node.args[0].get(), symbols, resolver, dc,
                                     loopDepth, parallelDepth, insideExtern);
        if (argType && !TypeChecker::isAssignable(argType, elemType)) {
            result.errorMessage = "argument type mismatch in '.push()'";
            dc.error(DiagnosticCategory::Semantic, node.args[0]->loc, DiagCode::E3002, 
                     result.errorMessage);
            return result;
        }
    }
    
    result.returnType = nullptr;  // void
    result.isHandled = true;
    return result;
}

static BuiltinMethodResult checkArrayPop(CallExprAST& node, TypeAST* receiverType,
                                          SymbolTable& symbols, TypeResolver& resolver,
                                          DiagnosticEngine& dc, int& loopDepth,
                                          int& parallelDepth, bool insideExtern) {
    BuiltinMethodResult result;
    
    if (!node.args.empty()) {
        result.errorMessage = "'.pop()' takes no arguments";
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3003, result.errorMessage);
        return result;
    }
    
    if (receiverType->isa<DynamicArrayTypeAST>()) {
        result.returnType = receiverType->as<DynamicArrayTypeAST>()->element.get();
    }
    
    result.isHandled = true;
    return result;
}

static BuiltinMethodResult checkArrayInsert(CallExprAST& node, TypeAST* receiverType,
                                             SymbolTable& symbols, TypeResolver& resolver,
                                             DiagnosticEngine& dc, int& loopDepth,
                                             int& parallelDepth, bool insideExtern) {
    BuiltinMethodResult result;
    
    if (node.args.size() != 2) {
        result.errorMessage = "'.insert()' expects two arguments: index and value";
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3003, result.errorMessage);
        return result;
    }
    
    // Check index expression (type validation happens in checkExpr)
    TypeAST* indexType = checkExpr(node.args[0].get(), symbols, resolver, dc,
                                   loopDepth, parallelDepth, insideExtern);
    
    // Verify index is integer type
    if (indexType && !TypeChecker::isIntegerType(indexType)) {
        result.errorMessage = "'.insert()' index must be an integer type";
        dc.error(DiagnosticCategory::Semantic, node.args[0]->loc, DiagCode::E3002, 
                 result.errorMessage);
        return result;
    }
    
    TypeAST* elemType = nullptr;
    if (receiverType->isa<DynamicArrayTypeAST>()) {
        elemType = receiverType->as<DynamicArrayTypeAST>()->element.get();
    }
    
    if (elemType) {
        TypeAST* valType = checkExpr(node.args[1].get(), symbols, resolver, dc,
                                     loopDepth, parallelDepth, insideExtern);
        if (valType && !TypeChecker::isAssignable(valType, elemType)) {
            result.errorMessage = "value type mismatch in '.insert()'";
            dc.error(DiagnosticCategory::Semantic, node.args[1]->loc, DiagCode::E3002, 
                     result.errorMessage);
            return result;
        }
    }
    
    result.returnType = nullptr;  // void
    result.isHandled = true;
    return result;
}

static BuiltinMethodResult checkArrayRemove(CallExprAST& node, TypeAST* receiverType,
                                             SymbolTable& symbols, TypeResolver& resolver,
                                             DiagnosticEngine& dc, int& loopDepth,
                                             int& parallelDepth, bool insideExtern) {
    BuiltinMethodResult result;
    
    if (node.args.size() != 1) {
        result.errorMessage = "'.remove()' expects one argument (index)";
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3003, result.errorMessage);
        return result;
    }
    
    // Check index is integer type
    TypeAST* indexType = checkExpr(node.args[0].get(), symbols, resolver, dc,
                                   loopDepth, parallelDepth, insideExtern);
    
    if (indexType && !TypeChecker::isIntegerType(indexType)) {
        result.errorMessage = "'.remove()' index must be an integer type";
        dc.error(DiagnosticCategory::Semantic, node.args[0]->loc, DiagCode::E3002, 
                 result.errorMessage);
        return result;
    }
    
    if (receiverType->isa<DynamicArrayTypeAST>()) {
        result.returnType = receiverType->as<DynamicArrayTypeAST>()->element.get();
    }
    
    result.isHandled = true;
    return result;
}

static BuiltinMethodResult checkArrayClear(CallExprAST& node, TypeAST* receiverType,
                                            SymbolTable& symbols, TypeResolver& resolver,
                                            DiagnosticEngine& dc, int& loopDepth,
                                            int& parallelDepth, bool insideExtern) {
    BuiltinMethodResult result;
    
    if (!node.args.empty()) {
        result.errorMessage = "'.clear()' takes no arguments";
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3003, result.errorMessage);
        return result;
    }
    
    result.returnType = nullptr;  // void
    result.isHandled = true;
    return result;
}

static BuiltinMethodResult checkArrayReserve(CallExprAST& node, TypeAST* receiverType,
                                              SymbolTable& symbols, TypeResolver& resolver,
                                              DiagnosticEngine& dc, int& loopDepth,
                                              int& parallelDepth, bool insideExtern) {
    BuiltinMethodResult result;
    
    if (node.args.size() != 1) {
        result.errorMessage = "'.reserve()' expects one argument (capacity)";
        dc.error(DiagnosticCategory::Semantic, node.loc, DiagCode::E3003, result.errorMessage);
        return result;
    }
    
    // Check capacity is integer type
    TypeAST* capType = checkExpr(node.args[0].get(), symbols, resolver, dc,
                                 loopDepth, parallelDepth, insideExtern);
    
    if (capType && !TypeChecker::isIntegerType(capType)) {
        result.errorMessage = "'.reserve()' argument must be an integer type";
        dc.error(DiagnosticCategory::Semantic, node.args[0]->loc, DiagCode::E3002, 
                 result.errorMessage);
        return result;
    }
    
    result.returnType = nullptr;  // void
    result.isHandled = true;
    return result;
}

std::string getBuiltinTypeKey(TypeAST* type) {
    if (!type) return "unknown";
    
    if (type->isa<FixedArrayTypeAST>()) return "fixed_array";
    if (type->isa<SliceTypeAST>()) return "slice";
    if (type->isa<DynamicArrayTypeAST>()) return "dynamic_array";
    if (type->isa<PrimitiveTypeAST>()) {
        auto* prim = type->as<PrimitiveTypeAST>();
        switch (prim->primitiveKind) {
            case PrimitiveKind::String: return "string";
            default: break;
        }
    }
    return "unknown";
}

void BuiltinMethodRegistry::initialize() {
    // ── Fixed Array [N]T methods ─────────────────────────────────────────────
    BuiltinMethodInfo len{ "len", checkArrayLen, "Returns number of elements" };
    BuiltinMethodInfo isEmpty{ "isEmpty", checkArrayIsEmpty, "Returns true if len() == 0" };
    BuiltinMethodInfo first{ "first", checkArrayFirst, "Returns first element (panics if empty)" };
    BuiltinMethodInfo last{ "last", checkArrayLast, "Returns last element (panics if empty)" };
    
    registerMethod("fixed_array", len);
    registerMethod("fixed_array", isEmpty);
    registerMethod("fixed_array", first);
    registerMethod("fixed_array", last);
    
    // ── Slice []T methods ────────────────────────────────────────────────────
    registerMethod("slice", len);
    registerMethod("slice", isEmpty);
    registerMethod("slice", first);
    registerMethod("slice", last);
    
    BuiltinMethodInfo cap{ "cap", checkArrayCap, "Returns allocated capacity" };
    registerMethod("slice", cap);
    
    // ── Dynamic Array [*]T methods ───────────────────────────────────────────
    registerMethod("dynamic_array", len);
    registerMethod("dynamic_array", isEmpty);
    registerMethod("dynamic_array", first);
    registerMethod("dynamic_array", last);
    registerMethod("dynamic_array", cap);
    
    BuiltinMethodInfo push{ "push", checkArrayPush, "Appends element to end" };
    BuiltinMethodInfo pop{ "pop", checkArrayPop, "Removes and returns last element" };
    BuiltinMethodInfo insert{ "insert", checkArrayInsert, "Inserts element at index" };
    BuiltinMethodInfo remove{ "remove", checkArrayRemove, "Removes element at index" };
    BuiltinMethodInfo clear{ "clear", checkArrayClear, "Removes all elements" };
    BuiltinMethodInfo reserve{ "reserve", checkArrayReserve, "Pre-allocates capacity" };
    
    registerMethod("dynamic_array", push);
    registerMethod("dynamic_array", pop);
    registerMethod("dynamic_array", insert);
    registerMethod("dynamic_array", remove);
    registerMethod("dynamic_array", clear);
    registerMethod("dynamic_array", reserve);
}

void BuiltinMethodRegistry::registerMethod(const std::string& typeKey, const BuiltinMethodInfo& method) {
    registry_[typeKey][method.name] = method;
}

const BuiltinMethodInfo* BuiltinMethodRegistry::lookup(const std::string& typeKey, const std::string& methodName) const {
    auto typeIt = registry_.find(typeKey);
    if (typeIt == registry_.end()) return nullptr;
    
    auto methodIt = typeIt->second.find(methodName);
    if (methodIt == typeIt->second.end()) return nullptr;
    
    return &methodIt->second;
}

bool BuiltinMethodRegistry::hasMethods(const std::string& typeKey) const {
    auto it = registry_.find(typeKey);
    return it != registry_.end() && !it->second.empty();
}

std::vector<BuiltinMethodInfo> BuiltinMethodRegistry::getMethodsForType(const std::string& typeKey) const {
    std::vector<BuiltinMethodInfo> result;
    auto it = registry_.find(typeKey);
    if (it != registry_.end()) {
        for (const auto& pair : it->second) {
            result.push_back(pair.second);
        }
    }
    return result;
}