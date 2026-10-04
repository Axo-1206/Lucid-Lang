/// @file compile/LambdaLift.cpp
/// @brief Synthesize top-level functions for the lambdas Sema collected.

#include "LambdaLift.hpp"

#include "core/ast/BaseAST.hpp"   // for AST_ASSERT_MSG
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"

namespace lucid::bytecode::compile {

LambdaLift::LambdaLift(Compiler& compiler, StringPool& pool)
    : m_compiler(compiler), m_pool(pool) {}

void LambdaLift::lift(const std::vector<ModuleAST*>& modules) {
    for (ModuleAST* mod : modules) {
        if (mod == nullptr) continue;

        for (LambdaExprAST* lambda : mod->lambdas) {
            if (lambda == nullptr) continue;

            AST_ASSERT_MSG(lambda->body != nullptr,
                "LambdaLift::lift: a lambda has no body — Sema "
                "should have rejected a lambda without one");
            AST_ASSERT_MSG(lambda->body->resolvedType != nullptr,
                "LambdaLift::lift: a lambda's body has no resolved "
                "type — Sema should have resolved it");

            // Synthesize the FnDeclAST. Emplace into the stable
            // deque; the reference `fn` is valid until the deque is
            // destroyed.
            m_functions.emplace_back(InternedString{});
            FnDeclAST& fn = m_functions.back();

            // The mangled name. A synthetic name, unique across the
            // compilation.
            const std::string mangled =
                "_L" + std::to_string(m_nextIndex++);
            fn.mangledName = m_pool.intern(mangled);

            // The parameters come from the lambda. The lambda's
            // params are in the AST arena; the span is copied.
            fn.params = lambda->params;

            // The return type is the body's resolved type. Sema
            // resolved it during the lambda's own resolution.
            fn.returnType = lambda->body->resolvedType;

            // No block body. Pass B reads the expression via
            // bodyOf().
            fn.body        = nullptr;
            fn.isHostBound = false;
            fn.isSequence  = false;

            // Register the function with the compiler. This assigns
            // its FunctionProto index and appends a placeholder
            // proto that pass B replaces.
            m_compiler.registerSynthesizedFunction(&fn, lambda->loc);

            // Record the mappings.
            m_byLambda.emplace(lambda, &fn);
            m_bodies.emplace(&fn, lambda->body);
            m_moduleOf.emplace(&fn, mod);
            m_synthesized.push_back(&fn);
        }
    }
}

const FnDeclAST* LambdaLift::functionFor(
    const LambdaExprAST* lambda) const {
    auto it = m_byLambda.find(lambda);
    if (it == m_byLambda.end()) return nullptr;
    return it->second;
}

ExprAST* LambdaLift::bodyOf(const FnDeclAST* fn) const {
    auto it = m_bodies.find(fn);
    if (it == m_bodies.end()) return nullptr;
    return it->second;
}

ModuleAST* LambdaLift::moduleOf(const FnDeclAST* fn) const {
    auto it = m_moduleOf.find(fn);
    if (it == m_moduleOf.end()) return nullptr;
    return it->second;
}

bool LambdaLift::isSynthesized(const FnDeclAST* fn) const {
    return m_bodies.find(fn) != m_bodies.end();
}

} // namespace lucid::bytecode::compile