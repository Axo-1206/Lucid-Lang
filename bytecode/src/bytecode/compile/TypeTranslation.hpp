/**
 * @file compile/TypeTranslation.hpp
 *
 * @responsibility Translate a Sema-resolved TypeAST into the artifact's
 *                 serializable TypeDescriptor. The one place the
 *                 compiler reads the AST's type nodes.
 *
 * ─── Design: a total function over the resolved types ─────────────────────
 * Sema resolves every type before the compiler runs. A NamedTypeAST's
 * resolvedDecl is populated; a FunctionTypeAST's parameter and return
 * types are resolved; an ArrayTypeAST's element type is resolved. So
 * the translation never needs to look anything up — it is a structural
 * copy from one representation to another.
 *
 * ─── Design: UnknownTypeAST translates to Kind::Unknown ───────────────────
 * A declaration whose type failed to resolve has an UnknownTypeAST.
 * Sema rejects the module before the compiler runs, so the compiler
 * should never see an UnknownTypeAST. If it does, the translation
 * produces a TypeDescriptor::Kind::Unknown, and the compiler's
 * post-translation assertions fire. This is a compiler-bug indicator,
 * not a graceful fallback.
 */

#pragma once

#include "bytecode/TypeDescriptor.hpp"

class StringPool;
struct TypeAST;

namespace lucid::bytecode::compile {

/// Translate a resolved type. The returned descriptor is fully owned.
///
/// The pool is required because a NamedTypeAST's mangled name is an
/// InternedString; resolving it to an owned string requires the pool
/// that interned it.
TypeDescriptor translateType(const TypeAST* type, StringPool& pool);

} // namespace lucid::bytecode::compile