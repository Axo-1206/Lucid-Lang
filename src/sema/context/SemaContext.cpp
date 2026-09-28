/// @file SemaContext.cpp
/// @brief Implementation of SemaContext and its RAII guards.

#include "SemaContext.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"

using namespace lucid::diag;

namespace lucid::sema {

// ─── Construction ─────────────────────────────────────────────────────────

SemaContext::SemaContext(StringPool& p, ASTArena& a, DiagnosticEngine& d)
    : pool(p)
    , arena(a)
    , diagnostics(d) {}

// ─── Module management ────────────────────────────────────────────────────

void SemaContext::addModule(ModuleAST* module) {
    if (!module) return;
    modules.push_back(module);
    if (module->filePath.isValid()) {
        modulesByPath[module->filePath] = module;
    }
}

void SemaContext::enterModule(ModuleAST* module) {
    currentModule      = module;
    currentModuleTable = &getOrCreateModuleTable(module);
}

ModuleTable& SemaContext::getOrCreateModuleTable(ModuleAST* module) {
    auto it = moduleTables.find(module);
    if (it != moduleTables.end()) {
        return it->second;
    }
    ModuleTable& table = moduleTables[module];
    table.module = module;
    return table;
}

ModuleTable* SemaContext::findModuleTable(ModuleAST* module) {
    auto it = moduleTables.find(module);
    return it != moduleTables.end() ? &it->second : nullptr;
}

ModuleAST* SemaContext::findModuleByPath(InternedString path) const {
    auto it = modulesByPath.find(path);
    return it != modulesByPath.end() ? it->second : nullptr;
}

// ─── Scope management ─────────────────────────────────────────────────────

bool SemaContext::isAtModuleLevel() const {
    return scopes.empty();
}

void SemaContext::pushScope() {
    scopes.emplace_back();
}

void SemaContext::popScope() {
    if (!scopes.empty()) {
        scopes.pop_back();
    }
}

Scope& SemaContext::currentScope() {
    assert(!scopes.empty() && "no scope open");
    return scopes.back();
}

const Scope& SemaContext::currentScope() const {
    assert(!scopes.empty() && "no scope open");
    return scopes.back();
}

// ─── Scope queries ────────────────────────────────────────────────────────

bool SemaContext::isInCurrentScope(InternedString name) const {
    if (scopes.empty()) return false;
    return currentScope().values.find(name) != currentScope().values.end();
}

bool SemaContext::isModuleMember(InternedString name) const {
    if (!currentModuleTable) return false;
    return currentModuleTable->values.find(name) != currentModuleTable->values.end()
        || currentModuleTable->types .find(name) != currentModuleTable->types .end();
}

// ─── Symbol insertion ─────────────────────────────────────────────────────

bool SemaContext::insertValue(ValueDeclAST* decl) {
    if (!decl) return false;

    if (isAtModuleLevel()) {
        if (!currentModuleTable) return false;
        if (currentModuleTable->values.count(decl->name)) {
            diagnostics.error(DiagCode::Name_Redeclaration, decl,
                              "redeclaration of '", pool.lookup(decl->name),
                              "' in module '",
                              pool.lookup(currentModule->filePath), "'");
            return false;
        }
        currentModuleTable->values[decl->name] = decl;
        return true;
    }

    if (currentScope().values.count(decl->name)) {
        diagnostics.error(DiagCode::Name_Redeclaration, decl,
                          "redeclaration of '", pool.lookup(decl->name),
                          "' in the same scope");
        return false;
    }
    currentScope().values[decl->name] = decl;
    return true;
}

bool SemaContext::insertType(TypeDeclAST* decl) {
    if (!decl) return false;

    // Types are module-level only (grammar §12.5). A local TABLE, FN, or
    // any future TypeDeclAST is a parser/Sema bug, not a user error —
    // the parser should have rejected it. The assert fires on the bug;
    // the diagnostic below is a defensive belt-and-suspenders for a
    // release build where NDEBUG turns the assert into a no-op.
    AST_ASSERT_MSG(isAtModuleLevel(),
                   "insertType() called from a non-module scope; "
                   "local type declarations are forbidden by the grammar");

    if (!currentModuleTable) return false;
    if (currentModuleTable->types.count(decl->name)) {
        diagnostics.error(DiagCode::Name_Redeclaration, decl,
                          "redeclaration of type '", pool.lookup(decl->name),
                          "' in module '",
                          pool.lookup(currentModule->filePath), "'");
        return false;
    }
    currentModuleTable->types[decl->name] = decl;
    return true;
}

bool SemaContext::addImportAlias(InternedString alias, ModuleAST* module,
                                 BaseAST* node) {
    if (!currentModuleTable) return false;
    if (currentModuleTable->importAliases.count(alias)) {
        diagnostics.error(DiagCode::Name_ImportAliasRedeclaration,
                          node ? node : module,
                          "redeclaration of import alias '",
                          pool.lookup(alias), "'");
        return false;
    }
    currentModuleTable->importAliases[alias] = module;
    return true;
}

// ─── Symbol lookup ────────────────────────────────────────────────────────

ValueDeclAST* SemaContext::lookupValue(InternedString name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto found = it->values.find(name);
        if (found != it->values.end()) {
            return found->second;
        }
    }
    if (currentModuleTable) {
        auto found = currentModuleTable->values.find(name);
        if (found != currentModuleTable->values.end()) {
            return found->second;
        }
    }
    return nullptr;
}

FnDeclAST* SemaContext::lookupFunction(InternedString name) const {
    ValueDeclAST* v = lookupValue(name);
    return (v && v->isa<FnDeclAST>()) ? v->as<FnDeclAST>() : nullptr;
}

TypeDeclAST* SemaContext::lookupType(InternedString name) const {
    // Types are not lexically scoped — local type declarations are
    // forbidden — so only the module table is consulted. The loop over
    // scopes is omitted deliberately: there is nothing to find.
    if (currentModuleTable) {
        auto found = currentModuleTable->types.find(name);
        if (found != currentModuleTable->types.end()) {
            return found->second;
        }
    }
    return nullptr;
}

// ─── Import lookup ────────────────────────────────────────────────────────

ModuleAST* SemaContext::lookupImport(InternedString alias) const {
    if (!currentModuleTable) return nullptr;
    auto it = currentModuleTable->importAliases.find(alias);
    return it != currentModuleTable->importAliases.end() ? it->second : nullptr;
}

ValueDeclAST* SemaContext::lookupImportedValue(InternedString alias,
                                               InternedString member) const {
    ModuleAST* module = lookupImport(alias);
    if (!module) return nullptr;
    ModuleTable* table = const_cast<SemaContext*>(this)->findModuleTable(module);
    if (!table) return nullptr;
    auto found = table->values.find(member);
    return found != table->values.end() ? found->second : nullptr;
}

TypeDeclAST* SemaContext::lookupImportedType(InternedString alias,
                                             InternedString member) const {
    ModuleAST* module = lookupImport(alias);
    if (!module) return nullptr;
    ModuleTable* table = const_cast<SemaContext*>(this)->findModuleTable(module);
    if (!table) return nullptr;
    auto found = table->types.find(member);
    return found != table->types.end() ? found->second : nullptr;
}

// ─── Type canonicalization ────────────────────────────────────────────────

PrimitiveTypeAST* SemaContext::getPrimitiveType(PrimitiveKind kind) {
    auto it = typeCache.primitives.find(kind);
    if (it != typeCache.primitives.end()) return it->second;

    PrimitiveTypeAST* t = arena.make<PrimitiveTypeAST>(kind);
    typeCache.primitives[kind] = t;
    return t;
}

UnknownTypeAST* SemaContext::getUnknownType() {
    if (!typeCache.unknownType) {
        typeCache.unknownType = arena.make<UnknownTypeAST>();
    }
    return typeCache.unknownType;
}

NamedTypeAST* SemaContext::getNamedType(InternedString name) {
    TypeCache::NamedTypeKey key{name};
    auto it = typeCache.namedTypes.find(key);
    if (it != typeCache.namedTypes.end()) return it->second;

    NamedTypeAST* t = arena.make<NamedTypeAST>(name);
    typeCache.namedTypes[key] = t;
    return t;
}

ArrayTypeAST* SemaContext::getArrayType(ArrayKind kind, uint64_t size,
                                        TypeAST* element) {
    TypeCache::ArrayTypeKey key{kind, size, element};
    auto it = typeCache.arrayTypes.find(key);
    if (it != typeCache.arrayTypes.end()) return it->second;

    ArrayTypeAST* t = arena.make<ArrayTypeAST>(kind, size, element);
    typeCache.arrayTypes[key] = t;
    return t;
}

RowRefTypeAST* SemaContext::getRowRefType(TypeAST* table) {
    if (!table) return nullptr;

    TypeCache::RowRefTypeKey key{table};
    auto it = typeCache.rowRefTypes.find(key);
    if (it != typeCache.rowRefTypes.end()) return it->second;

    RowRefTypeAST* t = arena.make<RowRefTypeAST>(table);
    typeCache.rowRefTypes[key] = t;
    return t;
}

NullableTypeAST* SemaContext::getNullableType(TypeAST* inner) {
    if (!inner) return nullptr;

    TypeCache::NullableTypeKey key{inner};
    auto it = typeCache.nullableTypes.find(key);
    if (it != typeCache.nullableTypes.end()) return it->second;

    NullableTypeAST* t = arena.make<NullableTypeAST>(inner);
    typeCache.nullableTypes[key] = t;
    return t;
}

FunctionTypeAST* SemaContext::getFunctionType(ArenaSpan<TypeAST*> params,
                                              TypeAST* returnType) {
    TypeCache::FunctionTypeKey key{params, returnType};
    auto it = typeCache.functionTypes.find(key);
    if (it != typeCache.functionTypes.end()) return it->second;

    FunctionTypeAST* t = arena.make<FunctionTypeAST>();
    t->params     = params;
    t->returnType = returnType;
    typeCache.functionTypes[key] = t;
    return t;
}

// ─── RAII Guards ──────────────────────────────────────────────────────────

ScopedContext::ScopedContext(SemaContext& ctx, ContextKind kind, BaseAST* node)
    : ctx_(ctx) {
    ctx_.stack.push(kind, node);
}

ScopedContext::~ScopedContext() {
    ctx_.stack.pop();
}

// ─────────────────────────────────────────────────────────────────────────

ScopedIfCondition::ScopedIfCondition(SemaContext& ctx, bool hasElse)
    : ctx_(ctx) {
    ctx_.stack.setIfConditionCtx(true);
    ctx_.stack.setHasElse(hasElse);
    ctx_.stack.clearPendingNarrowing();
}

ScopedIfCondition::~ScopedIfCondition() {
    ctx_.stack.setIfConditionCtx(false);
}

// ─────────────────────────────────────────────────────────────────────────

SymbolScope::SymbolScope(SemaContext& ctx)
    : ctx_(ctx) {
    ctx_.pushScope();
}

SymbolScope::~SymbolScope() {
    ctx_.popScope();
}

// ─────────────────────────────────────────────────────────────────────────

ScopedNarrowing::ScopedNarrowing(SemaContext& ctx, InternedString varName,
                                 TypeAST* narrowedType, bool isInverse)
    : ctx_(ctx) {
    ctx_.stack.pushNarrowingLevel(isInverse);
    ctx_.stack.narrowVariable(varName, narrowedType);
}

ScopedNarrowing::ScopedNarrowing(
    SemaContext& ctx,
    const std::unordered_map<InternedString, TypeAST*>& narrowings,
    bool isInverse)
    : ctx_(ctx) {
    ctx_.stack.pushNarrowingLevel(isInverse);
    for (const auto& [name, type] : narrowings) {
        ctx_.stack.narrowVariable(name, type);
    }
}

ScopedNarrowing::~ScopedNarrowing() {
    ctx_.stack.popNarrowingLevel();
}

// ─────────────────────────────────────────────────────────────────────────

ScopedFunction::ScopedFunction(SemaContext& ctx, FnDeclAST* decl)
    : ctx_(ctx)
    , paramScope_(ctx) {                      // pushes the parameter scope
    ContextKind kind = decl->isSequence ? ContextKind::SequenceBody
                                        : ContextKind::FuncBody;
    ctx_.stack.pushFunction(decl, kind, decl->returnType);
}

ScopedFunction::~ScopedFunction() {
    // Destruction order:
    //   1. Destructor body: pop the function frame.
    //   2. Member destructors: pop the parameter scope (via paramScope_).
    //
    // Reverse of construction: the frame was pushed after the scope, so
    // it is popped before it. Both stacks are independent, so this is
    // correctness-neutral today; the comment is here to record the
    // invariant so a future reader does not "correct" the order.
    ctx_.stack.pop();
    // ─── paramScope_ destructor pops the parameter scope ────────────────
}

} // namespace lucid::sema