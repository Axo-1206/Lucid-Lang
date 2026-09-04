/// @file SemaContext.cpp
/// @brief Implementation of SemaContext - semantic context management.
///
/// This file contains all concrete implementations for SemaContext,
/// keeping the header clean and focused on declarations.

#include "SemaContext.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"

namespace sema {

// ─── TypeCache Key Operators ─────────────────────────────────────────────

// NamedTypeKey
bool TypeCache::NamedTypeKey::operator==(const NamedTypeKey& other) const {
    if (name != other.name) return false;
    if (genericArgs.size() != other.genericArgs.size()) return false;
    for (size_t i = 0; i < genericArgs.size(); ++i) {
        if (genericArgs[i] != other.genericArgs[i]) return false;
    }
    return true;
}

size_t TypeCache::NamedTypeKeyHash::operator()(const NamedTypeKey& key) const {
    size_t h = std::hash<uint32_t>{}(key.name.id);
    for (TypeAST* arg : key.genericArgs) {
        h ^= std::hash<TypeAST*>{}(arg) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }
    return h;
}

// ArrayTypeKey
bool TypeCache::ArrayTypeKey::operator==(const ArrayTypeKey& other) const {
    return kind == other.kind && 
           size == other.size && 
           element == other.element;
}

size_t TypeCache::ArrayTypeKeyHash::operator()(const ArrayTypeKey& key) const {
    return std::hash<int>{}(static_cast<int>(key.kind)) ^
           std::hash<uint64_t>{}(key.size) ^
           std::hash<TypeAST*>{}(key.element);
}

// PtrTypeKey
bool TypeCache::PtrTypeKey::operator==(const PtrTypeKey& other) const {
    return inner == other.inner;
}

size_t TypeCache::PtrTypeKeyHash::operator()(const PtrTypeKey& key) const {
    return std::hash<TypeAST*>{}(key.inner);
}

// RefTypeKey
bool TypeCache::RefTypeKey::operator==(const RefTypeKey& other) const {
    return inner == other.inner;
}

size_t TypeCache::RefTypeKeyHash::operator()(const RefTypeKey& key) const {
    return std::hash<TypeAST*>{}(key.inner);
}

// ─── SemaContext Constructor ─────────────────────────────────────────────

SemaContext::SemaContext(StringPool& p, ASTArena& a, DiagnosticEngine& d)
    : pool(p)
    , arena(a)
    , diagnostics(d) {
    for (ModuleAST* m : modules) {
        if (m) modulesByPath[m->filePath] = m;
    }
}

// ─── Module Management ────────────────────────────────────────────────────

void SemaContext::enterModule(ModuleAST* module) {
    currentModule = module;
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

// ─── Scope Management ────────────────────────────────────────────────────

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
    assert(!scopes.empty() && "No scope open");
    return scopes.back();
}

const Scope& SemaContext::currentScope() const {
    assert(!scopes.empty() && "No scope open");
    return scopes.back();
}

// ─── Scope Queries ────────────────────────────────────────────────────────

bool SemaContext::isInCurrentScope(InternedString name) const {
    if (scopes.empty()) return false;
    return currentScope().values.find(name) != currentScope().values.end();
}

bool SemaContext::isModuleMember(InternedString name) const {
    if (!currentModuleTable) return false;
    return currentModuleTable->values.find(name) != currentModuleTable->values.end();
}

bool SemaContext::isTypeInCurrentScope(InternedString name) const {
    if (scopes.empty()) return false;
    return currentScope().types.find(name) != currentScope().types.end();
}

bool SemaContext::isModuleTypeMember(InternedString name) const {
    if (!currentModuleTable) return false;
    return currentModuleTable->types.find(name) != currentModuleTable->types.end();
}

bool SemaContext::isGenericParamInCurrentScope(InternedString name) const {
    if (scopes.empty()) return false;
    return currentScope().genericParams.find(name) != currentScope().genericParams.end();
}

// ─── Symbol Insertion ─────────────────────────────────────────────────────

bool SemaContext::insertValue(ValueDeclAST* decl) {
    if (isAtModuleLevel()) {
        if (currentModuleTable->values.find(decl->name) != currentModuleTable->values.end()) {
            diagnostics.error(DiagCode::Sem_Redeclaration, decl,
                              "redeclaration of '", pool.lookup(decl->name), 
                              "' in the same scope");
            return false;
        }
        currentModuleTable->values[decl->name] = decl;
        return true;
    } else {
        if (currentScope().values.find(decl->name) != currentScope().values.end()) {
            diagnostics.error(DiagCode::Sem_Redeclaration, decl,
                              "redeclaration of '", pool.lookup(decl->name), 
                              "' in the same scope");
            return false;
        }
        currentScope().values[decl->name] = decl;
        return true;
    }
}

bool SemaContext::insertType(TypeDeclAST* decl) {
    if (isAtModuleLevel()) {
        if (currentModuleTable->types.find(decl->name) != currentModuleTable->types.end()) {
            diagnostics.error(DiagCode::Sem_Redeclaration, decl,
                              "redeclaration of '", pool.lookup(decl->name), 
                              "' in the same scope");
            return false;
        }
        currentModuleTable->types[decl->name] = decl;
        return true;
    } else {
        if (currentScope().types.find(decl->name) != currentScope().types.end()) {
            diagnostics.error(DiagCode::Sem_Redeclaration, decl,
                              "redeclaration of '", pool.lookup(decl->name), 
                              "' in the same scope");
            return false;
        }
        currentScope().types[decl->name] = decl;
        return true;
    }
}

bool SemaContext::insertGenericParam(GenericParamDeclAST* param) {
    assert(!isAtModuleLevel() && "insertGenericParam() requires an open Scope");
    if (currentScope().genericParams.find(param->name) != currentScope().genericParams.end()) {
        diagnostics.error(DiagCode::Sem_GenericParamRedeclaration, param,
                          "redeclaration of generic parameter '", 
                          pool.lookup(param->name), "' in the same scope");
        return false;
    }
    currentScope().genericParams[param->name] = param;
    return true;
}

bool SemaContext::addImportAlias(InternedString alias, ModuleAST* module, BaseAST* node) {
    if (!currentModuleTable) return false;
    if (currentModuleTable->importAliases.find(alias) != currentModuleTable->importAliases.end()) {
        diagnostics.error(DiagCode::Sem_ImportAliasRedeclaration, node ? node : module,
                          "redeclaration of import alias '", 
                          pool.lookup(alias), "'");
        return false;
    }
    currentModuleTable->importAliases[alias] = module;
    return true;
}

// ─── Symbol Lookup ────────────────────────────────────────────────────────

TypeAST* SemaContext::getEffectiveType(ValueDeclAST* decl, InternedString name) const {
    if (!decl) return nullptr;
    
    TypeAST* narrowedType = stack.getNarrowedType(name);
    if (narrowedType) {
        return narrowedType;
    }
    
    return decl->type;
}

GenericParamDeclAST* SemaContext::lookupGenericParam(InternedString name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto found = it->genericParams.find(name);
        if (found != it->genericParams.end()) {
            return found->second;
        }
    }
    return nullptr;
}

bool SemaContext::isGenericParam(InternedString name) const {
    return lookupGenericParam(name) != nullptr;
}

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

FuncDeclAST* SemaContext::lookupFunction(InternedString name) const {
    ValueDeclAST* v = lookupValue(name);
    return (v && v->isa<FuncDeclAST>()) ? v->as<FuncDeclAST>() : nullptr;
}

TypeDeclAST* SemaContext::lookupType(InternedString name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto gen = it->genericParams.find(name);
        if (gen != it->genericParams.end()) {
            return nullptr;
        }
        auto found = it->types.find(name);
        if (found != it->types.end()) {
            return found->second;
        }
    }
    if (currentModuleTable) {
        auto found = currentModuleTable->types.find(name);
        if (found != currentModuleTable->types.end()) {
            return found->second;
        }
    }
    return nullptr;
}

ModuleAST* SemaContext::lookupImport(InternedString alias) const {
    if (!currentModuleTable) return nullptr;
    auto it = currentModuleTable->importAliases.find(alias);
    return it != currentModuleTable->importAliases.end() ? it->second : nullptr;
}

// ─── Type Lookup with Context ────────────────────────────────────────────

TypeDeclAST* SemaContext::lookupTypeDecl(InternedString name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto gen = it->genericParams.find(name);
        if (gen != it->genericParams.end()) {
            return gen->second;
        }
        auto found = it->types.find(name);
        if (found != it->types.end()) {
            return found->second;
        }
    }
    
    if (currentModuleTable) {
        auto found = currentModuleTable->types.find(name);
        if (found != currentModuleTable->types.end()) {
            return found->second;
        }
    }
    
    return nullptr;
}

bool SemaContext::isGenericTypeParam(InternedString name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto gen = it->genericParams.find(name);
        if (gen != it->genericParams.end()) {
            return true;
        }
    }
    return false;
}

TypeDeclAST* SemaContext::lookupTypeDeclWithAlias(InternedString name) const {
    TypeDeclAST* decl = lookupTypeDecl(name);
    if (decl) return decl;
    return nullptr;
}

// ─── Module Member Lookup ─────────────────────────────────────────────────

ValueDeclAST* SemaContext::lookupModuleValueMember(ModuleAST* module, InternedString memberName) const {
    if (!module) return nullptr;
    auto it = moduleTables.find(module);
    if (it == moduleTables.end()) return nullptr;
    auto found = it->second.values.find(memberName);
    return found != it->second.values.end() ? found->second : nullptr;
}

TypeDeclAST* SemaContext::lookupModuleTypeMember(ModuleAST* module, InternedString memberName) const {
    if (!module) return nullptr;
    auto it = moduleTables.find(module);
    if (it == moduleTables.end()) return nullptr;
    auto found = it->second.types.find(memberName);
    return found != it->second.types.end() ? found->second : nullptr;
}

ValueDeclAST* SemaContext::lookupValueByAlias(InternedString alias, InternedString memberName) const {
    ModuleAST* module = lookupImport(alias);
    if (!module) return nullptr;
    return lookupModuleValueMember(module, memberName);
}

TypeDeclAST* SemaContext::lookupTypeByAlias(InternedString alias, InternedString memberName) const {
    ModuleAST* module = lookupImport(alias);
    if (!module) return nullptr;
    return lookupModuleTypeMember(module, memberName);
}

// ─── Export Checking ─────────────────────────────────────────────────────

bool SemaContext::isExported(DeclAST* decl) const {
    if (!decl) return false;
    for (AttributeAST* attr : decl->attributes) {
        if (attr->name == pool.intern("export")) {
            return true;
        }
    }
    return false;
}

bool SemaContext::isTypeExported(TypeDeclAST* decl) const {
    return isExported(decl);
}

bool SemaContext::isValueExported(ValueDeclAST* decl) const {
    return isExported(decl);
}

// ─── Module Member Keyword Info ──────────────────────────────────────────

DeclKeyword SemaContext::lookupModuleMemberKeyword(ModuleAST* module, InternedString memberName) const {
    ValueDeclAST* decl = lookupModuleValueMember(module, memberName);
    if (!decl) return DeclKeyword::Let;
    if (decl->isa<VarDeclAST>()) {
        return decl->as<VarDeclAST>()->keyword;
    }
    if (decl->isa<FuncDeclAST>()) {
        return decl->as<FuncDeclAST>()->keyword;
    }
    return DeclKeyword::Let;
}

bool SemaContext::isModuleMemberMutable(ModuleAST* module, InternedString memberName) const {
    ValueDeclAST* decl = lookupModuleValueMember(module, memberName);
    if (!decl) return false;
    if (decl->isa<VarDeclAST>()) {
        return decl->as<VarDeclAST>()->keyword == DeclKeyword::Let;
    }
    if (decl->isa<FuncDeclAST>()) {
        return decl->as<FuncDeclAST>()->keyword == DeclKeyword::Let;
    }
    return false;
}

bool SemaContext::isModuleMemberConst(ModuleAST* module, InternedString memberName) const {
    ValueDeclAST* decl = lookupModuleValueMember(module, memberName);
    if (!decl) return false;
    if (decl->isa<VarDeclAST>()) {
        return decl->as<VarDeclAST>()->keyword == DeclKeyword::Const;
    }
    if (decl->isa<FuncDeclAST>()) {
        return decl->as<FuncDeclAST>()->keyword == DeclKeyword::Const;
    }
    if (decl->isa<EnumVariantAST>()) {
        return true;
    }
    return false;
}

DeclKeyword SemaContext::lookupModuleMemberKeywordByAlias(InternedString alias, InternedString memberName) const {
    ModuleAST* module = lookupImport(alias);
    if (!module) return DeclKeyword::Let;
    return lookupModuleMemberKeyword(module, memberName);
}

bool SemaContext::isModuleMemberMutableByAlias(InternedString alias, InternedString memberName) const {
    ModuleAST* module = lookupImport(alias);
    if (!module) return false;
    return isModuleMemberMutable(module, memberName);
}

bool SemaContext::isModuleMemberConstByAlias(InternedString alias, InternedString memberName) const {
    ModuleAST* module = lookupImport(alias);
    if (!module) return false;
    return isModuleMemberConst(module, memberName);
}

// ─── Concurrency Helpers ──────────────────────────────────────────────────

void SemaContext::addPendingAsync(InternedString name, ExprAST* call, const SourceLocation& loc) {
    if (isAtModuleLevel()) return;
    PendingAsync pending{name, call, loc};
    currentScope().pendingAsync[name] = pending;
}

void SemaContext::addPendingSpawn(InternedString name, ExprAST* call, const SourceLocation& loc) {
    if (isAtModuleLevel()) return;
    PendingSpawn pending{name, call, loc};
    currentScope().pendingSpawn[name] = pending;
}

bool SemaContext::hasPendingAsync(InternedString name) const {
    if (scopes.empty()) return false;
    return currentScope().pendingAsync.find(name) != currentScope().pendingAsync.end();
}

bool SemaContext::hasPendingSpawn(InternedString name) const {
    if (scopes.empty()) return false;
    return currentScope().pendingSpawn.find(name) != currentScope().pendingSpawn.end();
}

bool SemaContext::isPendingFuture(InternedString name) const {
    return hasPendingAsync(name) || hasPendingSpawn(name);
}

void SemaContext::resolveAsync(InternedString name) {
    if (!scopes.empty()) {
        currentScope().pendingAsync.erase(name);
    }
}

void SemaContext::resolveSpawn(InternedString name) {
    if (!scopes.empty()) {
        currentScope().pendingSpawn.erase(name);
    }
}

std::vector<InternedString> SemaContext::getPendingAsyncNames() const {
    std::vector<InternedString> result;
    if (!scopes.empty()) {
        for (const auto& [name, _] : currentScope().pendingAsync) {
            result.push_back(name);
        }
    }
    return result;
}

std::vector<InternedString> SemaContext::getPendingSpawnNames() const {
    std::vector<InternedString> result;
    if (!scopes.empty()) {
        for (const auto& [name, _] : currentScope().pendingSpawn) {
            result.push_back(name);
        }
    }
    return result;
}

bool SemaContext::hasPendingAsync() const {
    return !scopes.empty() && !currentScope().pendingAsync.empty();
}

bool SemaContext::hasPendingSpawn() const {
    return !scopes.empty() && !currentScope().pendingSpawn.empty();
}

// ─── Type Cache Accessors ─────────────────────────────────────────────────

PrimitiveTypeAST* SemaContext::getBoolType() {
    if (!typeCache.boolType) {
        typeCache.boolType = arena.make<PrimitiveTypeAST>(PrimitiveKind::Bool);
    }
    return typeCache.boolType;
}

PrimitiveTypeAST* SemaContext::getIntType() {
    if (!typeCache.intType) {
        typeCache.intType = arena.make<PrimitiveTypeAST>(PrimitiveKind::Int);
    }
    return typeCache.intType;
}

PrimitiveTypeAST* SemaContext::getFloatType() {
    if (!typeCache.floatType) {
        typeCache.floatType = arena.make<PrimitiveTypeAST>(PrimitiveKind::Float);
    }
    return typeCache.floatType;
}

PrimitiveTypeAST* SemaContext::getStringType() {
    if (!typeCache.stringType) {
        typeCache.stringType = arena.make<PrimitiveTypeAST>(PrimitiveKind::String);
    }
    return typeCache.stringType;
}

PrimitiveTypeAST* SemaContext::getCharType() {
    if (!typeCache.charType) {
        typeCache.charType = arena.make<PrimitiveTypeAST>(PrimitiveKind::Char);
    }
    return typeCache.charType;
}

PrimitiveTypeAST* SemaContext::getUint64Type() {
    // Check cache first
    if (!typeCache.uint64Type) {
        typeCache.uint64Type = arena.make<PrimitiveTypeAST>(PrimitiveKind::Uint64);
    }
    return typeCache.uint64Type;
}

UnknownTypeAST* SemaContext::getUnknownType() {
    if (!typeCache.unknownType) {
        typeCache.unknownType = arena.make<UnknownTypeAST>();
    }
    return typeCache.unknownType;
}

NamedTypeAST* SemaContext::getNamedType(InternedString name, const ArenaSpan<TypeAST*>& genericArgs) {
    TypeCache::NamedTypeKey key{name, genericArgs};
    auto it = typeCache.namedTypes.find(key);
    if (it != typeCache.namedTypes.end()) {
        return it->second;
    }
    
    NamedTypeAST* type = arena.make<NamedTypeAST>(name);
    type->genericArgs = genericArgs;
    
    typeCache.namedTypes[key] = type;
    return type;
}

ArrayTypeAST* SemaContext::getArrayType(ArrayKind kind, uint64_t size, TypeAST* element) {
    TypeCache::ArrayTypeKey key{kind, size, element};
    auto it = typeCache.arrayTypes.find(key);
    if (it != typeCache.arrayTypes.end()) {
        return it->second;
    }
    ArrayTypeAST* type = arena.make<ArrayTypeAST>(kind, size, element);
    typeCache.arrayTypes[key] = type;
    return type;
}

PtrTypeAST* SemaContext::getPtrType(TypeAST* inner) {
    if (!inner) return nullptr;
    
    TypeCache::PtrTypeKey key{inner};
    auto it = typeCache.ptrTypes.find(key);
    if (it != typeCache.ptrTypes.end()) {
        return it->second;
    }
    
    PtrTypeAST* type = arena.make<PtrTypeAST>(inner);
    typeCache.ptrTypes[key] = type;
    return type;
}

RefTypeAST* SemaContext::getRefType(TypeAST* inner) {
    if (!inner) return nullptr;
    
    TypeCache::RefTypeKey key{inner};
    auto it = typeCache.refTypes.find(key);
    if (it != typeCache.refTypes.end()) {
        return it->second;
    }
    
    RefTypeAST* type = arena.make<RefTypeAST>(inner);
    typeCache.refTypes[key] = type;
    return type;
}

// ─── Built-in Type Accessors ─────────────────────────────────────────────

NamedTypeAST* SemaContext::getArenaType() {
    InternedString name = pool.intern("Arena");
    TypeCache::NamedTypeKey key{name, arena.makeBuilder<TypeAST*>().build()};
    auto it = typeCache.namedTypes.find(key);
    if (it != typeCache.namedTypes.end()) {
        return it->second;
    }
    
    // Create the type directly, don't call getArenaType() again
    NamedTypeAST* type = arena.make<NamedTypeAST>(name);
    type->genericArgs = arena.makeBuilder<TypeAST*>().build();
    
    typeCache.namedTypes[key] = type;
    return type;
}

NamedTypeAST* SemaContext::getArenaDescriptorType() {
    InternedString name = pool.intern("ArenaDescriptor");
    TypeCache::NamedTypeKey key{name, arena.makeBuilder<TypeAST*>().build()};
    auto it = typeCache.namedTypes.find(key);
    if (it != typeCache.namedTypes.end()) {
        return it->second;
    }
    
    // Create the type directly
    NamedTypeAST* type = arena.make<NamedTypeAST>(name);
    type->genericArgs = arena.makeBuilder<TypeAST*>().build();
    
    typeCache.namedTypes[key] = type;
    return type;
}

// ─── Self-Reference Helpers ──────────────────────────────────────────────

bool SemaContext::isDefiningType(TypeDeclAST* decl) const {
    for (TypeDeclAST* d : definingTypes) {
        if (d == decl) return true;
    }
    return false;
}

TypeDeclAST* SemaContext::currentDefiningType() const {
    return definingTypes.empty() ? nullptr : definingTypes.back();
}

// ─── Closure Helpers ──────────────────────────────────────────────────────

size_t SemaContext::getClosureDepth() const {
    return stack.getClosureDepth();
}

bool SemaContext::insideNestedFunction() const {
    return stack.insideNestedFunction();
}

FuncDeclAST* SemaContext::getInnermostFunction() const {
    return stack.getInnermostFunction();
}

BaseAST* SemaContext::getInnermostFunctionNode() const {
    return stack.getInnermostFunctionNode();
}

// ─── RAII Guards ─────────────────────────────────────────────────────────

ScopedSemanticContext::ScopedSemanticContext(SemaContext& ctx, ContextKind kind, BaseAST* node)
    : ctx_(ctx) {
    ctx_.stack.push(kind, node);
}

ScopedSemanticContext::~ScopedSemanticContext() {
    ctx_.stack.pop();
}



ScopedIfCondition::ScopedIfCondition(SemaContext& ctx, bool hasElse)
    : ctx_(ctx) {
    ctx_.stack.setIfConditionCtx(true);
    ctx_.stack.setHasElse(hasElse);
    ctx_.stack.clearPendingNarrowing();
}

ScopedIfCondition::~ScopedIfCondition() {
    ctx_.stack.setIfConditionCtx(false);
}



SymbolScope::SymbolScope(SemaContext& ctx)
    : ctx_(ctx) {
    ctx_.pushScope();
}

SymbolScope::~SymbolScope() {
    ctx_.popScope();
}



ScopedNarrowing::ScopedNarrowing(SemaContext& ctx, InternedString varName, 
                                 TypeAST* narrowedType, bool isInverse)
    : ctx_(ctx) {
    ctx_.stack.pushNarrowingLevel(isInverse);
    ctx_.stack.narrowVariable(varName, narrowedType);
}

ScopedNarrowing::ScopedNarrowing(SemaContext& ctx, 
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



ScopedTypeDefinition::ScopedTypeDefinition(SemaContext& ctx, TypeDeclAST* decl)
    : ctx_(ctx) {
    ctx_.definingTypes.push_back(decl);
}

ScopedTypeDefinition::~ScopedTypeDefinition() {
    if (!ctx_.definingTypes.empty()) {
        ctx_.definingTypes.pop_back();
    }
}

// ─── ScopedFunction Implementation ────────────────────────────────────────

ScopedFunction::ScopedFunction(SemaContext& ctx, FuncDeclAST* decl, TypeAST* returnType)
    : ctx_(ctx)
    , paramScope_(ctx) {  // SymbolScope is constructed FIRST (pushes parameter scope)
    // ─── Then push the function context ─────────────────────────────────────
    ctx_.stack.pushFunction(decl, returnType);
}

ScopedFunction::ScopedFunction(SemaContext& ctx, AnonFuncExprAST* expr, TypeAST* returnType)
    : ctx_(ctx)
    , paramScope_(ctx) {  // SymbolScope is constructed FIRST (pushes parameter scope)
    // ─── Then push the anonymous function context ───────────────────────────
    ctx_.stack.pushAnonFunction(expr, returnType);
}

ScopedFunction::~ScopedFunction() {
    // ─── Destructor order is REVERSE of construction order ─────────────────
    // 1. paramScope_ destructor runs LAST? No, member destructors run in
    //    REVERSE order of construction.
    // 
    // Construction order:
    //   1. ctx_ (reference, no destructor)
    //   2. paramScope_ (SymbolScope) → pushes parameter scope
    //   3. Function context push happens in constructor body
    // 
    // Destruction order:
    //   1. Function context pop happens in destructor body (explicit)
    //   2. paramScope_ destructor → pops parameter scope
    // 
    // So the function context is popped BEFORE the parameter scope,
    // which is correct because the function context depends on the parameters.
    ctx_.stack.pop();  // ← Pop function context first (explicit)
    // ─── paramScope_ destructor automatically pops the parameter scope ────
}

} // namespace sema