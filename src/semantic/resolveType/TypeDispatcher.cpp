/**
 * @file TypeDispatcher.cpp
 * @brief Implementation of TypeDispatcher - main dispatch logic.
 */

#include "TypeDispatcher.hpp"
#include "primitive/PrimitiveResolver.hpp"
#include "named/NamedResolver.hpp"
#include "composite/NullableResolver.hpp"
#include "composite/ResultResolver.hpp"
#include "composite/ArrayResolver.hpp"
#include "composite/RefResolver.hpp"
#include "composite/PtrResolver.hpp"
#include "composite/FuncResolver.hpp"
#include "decl/TypeAliasResolver.hpp"
#include "decl/StructResolver.hpp"
#include "decl/FuncSignatureResolver.hpp"
#include "decl/ImplResolver.hpp"
#include "decl/FromResolver.hpp"
#include "decl/VarResolver.hpp"
#include "injection/InjectionTransformer.hpp"
#include "callable/CallableExtractor.hpp"
#include "debug/DebugMacros.hpp"

TypeDispatcher::TypeDispatcher(SemanticContext& ctx)
    : ctx_(ctx)
    , genericParams_()
    // Simple resolvers
    , primitiveResolver_(std::make_unique<PrimitiveResolver>(ctx))
    , namedResolver_(std::make_unique<NamedResolver>(ctx, genericParams_))
    , resultResolver_(std::make_unique<ResultResolver>(ctx))
    , arrayResolver_(std::make_unique<ArrayResolver>(ctx))
    , refResolver_(std::make_unique<RefResolver>(ctx))
    , ptrResolver_(std::make_unique<PtrResolver>(ctx))
    , funcResolver_(std::make_unique<FuncResolver>(ctx, genericParams_))
    , typeAliasResolver_(std::make_unique<TypeAliasResolver>(ctx, genericParams_))
    , structResolver_(std::make_unique<StructResolver>(ctx, genericParams_))
    , funcSignatureResolver_(std::make_unique<FuncSignatureResolver>(ctx, genericParams_))
    , varResolver_(std::make_unique<VarResolver>(ctx))
    // Dependencies (initialized before complex resolvers)
    , nullableResolver_(std::make_unique<NullableResolver>(ctx))
    , injectionTransformer_(std::make_unique<InjectionTransformer>(ctx))
    , callableExtractor_(std::make_unique<CallableExtractor>(ctx))
    // Complex resolvers
    , implResolver_(std::make_unique<ImplResolver>(ctx, genericParams_, *injectionTransformer_, *callableExtractor_))
    , fromResolver_(std::make_unique<FromResolver>(ctx, genericParams_, *callableExtractor_)) {

    // Set cross-dependency after construction
    nullableResolver_->setFuncResolver(funcResolver_.get());

    LUC_LOG_SEMANTIC("TypeDispatcher constructed");
}

TypeDispatcher::~TypeDispatcher() = default;

TypeAST* TypeDispatcher::resolveType(TypeAST* typeNode) {
    if (!typeNode) return nullptr;
    LUC_LOG_SEMANTIC_EXTREME("TypeDispatcher::resolveType: kind=" << static_cast<int>(typeNode->kind));
    switch (typeNode->kind) {
        case ASTKind::PrimitiveType:
            return primitiveResolver_->resolve(*typeNode->as<PrimitiveTypeAST>());
        case ASTKind::NamedType:
            return namedResolver_->resolve(*typeNode->as<NamedTypeAST>());
        case ASTKind::NullableType:
            return nullableResolver_->resolve(*typeNode->as<NullableTypeAST>());
        case ASTKind::ResultType:
            return resultResolver_->resolve(*typeNode->as<ResultTypeAST>());
        case ASTKind::ArrayType:
            return arrayResolver_->resolve(*typeNode->as<ArrayTypeAST>());
        case ASTKind::RefType:
            return refResolver_->resolve(*typeNode->as<RefTypeAST>());
        case ASTKind::PtrType:
            return ptrResolver_->resolve(*typeNode->as<PtrTypeAST>());
        case ASTKind::FuncType:
            return funcResolver_->resolve(*typeNode->as<FuncTypeAST>());
        default:
            ctx_.error(typeNode->loc, DiagCode::E2002, "Unknown type kind in resolution");
            return nullptr;
    }
}

void TypeDispatcher::resolveTypeAlias(TypeAliasDeclAST& node) {
    typeAliasResolver_->resolve(node);
}

void TypeDispatcher::resolveStructFields(StructDeclAST& node) {
    structResolver_->resolve(node);
}

void TypeDispatcher::resolveFunctionSignature(FuncDeclAST& node) {
    funcSignatureResolver_->resolve(node);
}

void TypeDispatcher::resolveImplMethods(ImplDeclAST& node) {
    implResolver_->resolve(node);
}

void TypeDispatcher::resolveFromEntries(FromDeclAST& node) {
    fromResolver_->resolve(node);
}

void TypeDispatcher::resolveVarType(VarDeclAST& node) {
    varResolver_->resolve(node);
}

TypeAST* TypeDispatcher::getFunctionReturnType(const FuncTypeAST& type, const SourceLocation* loc) {
    return funcResolver_->getReturnType(type, loc);
}

std::vector<TypeAST*> TypeDispatcher::getFunctionReturnTypes(const FuncTypeAST& type) {
    return funcResolver_->getReturnTypes(type);
}