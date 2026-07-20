/**
 * @file DiagnosticCodes.hpp
 * @brief Numeric codes for every diagnostic the compiler can report.
 *
 * @architectural_note Range table (the whole file follows this plan)
 *
 *   Every phase of the compiler gets its own reserved block of 1000 codes,
 *   even if it doesn't come close to using them yet — the point of
 *   reserving the whole block up front is that a phase can grow for years
 *   without ever needing to renumber anything or spill into a neighbor's
 *   territory. Existing code VALUES below are untouched by this pass
 *   (`E0001` is still 0, `E5001` is still 5000, etc.) — this is purely
 *   about naming and reserving the space around them so the next codes
 *   added, whenever that happens, have an obvious, pre-agreed home.
 *
 *   | Range     | Phase                          | DiagnosticCategory                 | Status                                    |
 *   | --------- | ------------------------------ | ---------------------------------- | ----------------------------------------- |
 *   | 0000–0099 | Environment / Driver           | General                            | in use (E0001–E0005)                      |
 *   | 0100–0999 | Lexical                        | Lexical                            | in use (E0101–E0105), room to grow        |
 *   | 1000–1999 | Syntax (Parser)                | Syntax                             | in use (E1001–E1108), see sub-split below |
 *   | 2000–2999 | Semantic — Name Resolution     | Semantic                           | in use (E2001–E2003), see sub-split below |
 *   | 3000–3999 | Semantic — Type Checking       | Semantic                           | in use (E3001, E3101), see sub-split below|
 *   | 4000–4999 | Semantic — Generics/Traits/FFI | Semantic                           | in use (E4001–E4003, E4101), see sub-split below|
 *   | 5000–5999 | Backend / Linker               | Backend                            | in use (E5001–E5005)                      |
 *   | 6000–6999 | Warnings (cross-cutting)       | (matches the phase that raised it) | in use (W0001–W0002)                      |
 *   | 7000+     | UNRESERVED                     | —                                  | not yet allocated to anything             |
 *
 *   Why Semantic gets three blocks instead of one: it's structurally the
 *   richest phase (see Sema.hpp) — name resolution, type checking, and
 *   generics/traits/FFI validation are distinct enough concerns that
 *   giving them one shared 1000-code block would just reproduce today's
 *   problem one level down once any of them gets busy. Splitting now,
 *   while there are zero codes to migrate, is free; splitting later
 *   wouldn't be.
 *
 *   Warnings are intentionally NOT split by phase the way Semantic is —
 *   a warning can originate from lexing, parsing, or analysis alike (see
 *   `DiagnosticSeverity::Warning` in Diagnostic.hpp, which is orthogonal
 *   to `DiagnosticCategory`), so splitting 6000–6999 by phase would mean
 *   every phase needs both an error block AND a warning sub-block, doubling
 *   the bookkeeping for what's so far only 2 codes. Revisit if the warning
 *   range ever gets close to full.
 *
 * @note Each range below ends with a `static_assert` pinning the last
 *       currently-defined code to stay under the next range's boundary —
 *       this is a compile-time tripwire, not documentation: if someone
 *       adds one code too many to a range, the build breaks right there
 *       instead of silently colliding with the next phase's numbers. When
 *       you add a new code, move the corresponding `static_assert` down to
 *       reference your new last-code-in-range (or just leave it — it'll
 *       still catch a real overflow, it just won't be checking the newest
 *       code specifically).
 */

#pragma once
#include <cstdint>

enum class DiagCode : uint32_t {
    // ═══════════════════════════════════════════════════════════════════════
    // 0001–0100: Environment / Driver
    // (file I/O, module resolution, CLI/config, cross-cutting compiler
    //  failures that aren't specific to any one source-processing phase)
    // ═══════════════════════════════════════════════════════════════════════
    E0001 = 1,      ///< Unknown Error
    E0002,          ///< Too many consecutive errors. Aborting...
    E0003,          ///< File not found or inaccessible.
    E0004,          ///< Module resolution failed.
    E0005,          ///< Cyclic module dependency.
    // 0006–0100 reserved for more Environment/Driver codes.

    // ═══════════════════════════════════════════════════════════════════════
    // 0101–1000: Lexical
    // (character stream → token stream; the scanner)
    // ═══════════════════════════════════════════════════════════════════════
    E0101 = 101,    ///< Invalid character in source (only ASCII allowed).
    E0102,          ///< Unterminated string literal.
    E0103,          ///< Unterminated raw string literal.
    E0104,          ///< Unterminated block comment.
    E0105,          ///< Uknown character.
    // 0106–1000 reserved for more Lexical codes. Given this range is 900
    // codes wide against a phase that's used 5 in years of design so far,
    // there's no expectation of ever needing a fourth digit's worth more
    // room here the way Semantic will.

    // ═══════════════════════════════════════════════════════════════════════
    // 1001–2000: Syntax (Parser)
    // (token stream → AST; grammar rules, error recovery)
    //
    // Sub-split already implicit in the existing numbers, made explicit:
    //   1001–1100: general/structural syntax errors (apply across many
    //              grammar rules — "expected identifier", "unexpected
    //              token", ...)
    //   1101–1100: specialized/per-construct syntax errors (specific to
    //              one grammar rule — imports, attributes, switch, ...)
    // ═══════════════════════════════════════════════════════════════════════

    // ── 1001–1100: General ──
    E1001 = 1001,   ///< Expected keyword
    E1002,          ///< Expected an identifier.
    E1003,          ///< Expected type
    E1004,          ///< Expected '{, [, <, ('
    E1005,          ///< Expected '}, ], >, )'
    E1006,          ///< Expected expression
    E1007,          ///< Expected token
    E1008,          ///< Unexpected token
    E1009,          ///< Trailing tokens
    E1010,          ///< Invalid context
    E1011,          ///< Expected body (block)
    // 1012–1100 reserved for more general syntax codes.

    // ── 1101–2000: Specialized (per grammar rule) ──
    E1101,          ///< Expected module path after keyword 'use'
    // E1102,          ///< Expected branch after condition
    // E1103,          ///< Expected else branchs
    E1104,          ///< Expected attribute argument literal
    E1105,          ///< Expected switch subject
    E1106,          ///< Empty expression group
    E1107,          ///< Expected pipeline seed (expression)
    E1108,          ///< Multiple default clauses in switch
    // 1109–2000 reserved for more specialized syntax codes.

    // E1105,          ///< Unexpected trailing '+' in generic constraints
    // E1106,          ///< Unexpected trailing '.' in path

    // ═══════════════════════════════════════════════════════════════════════
    // 2001–3000: Semantic — Name Resolution
    // (AST → validated AST, the resolution half: does this name refer to
    //  a real declaration in the current context? See SemaContext's
    //  lookupValue()/lookupType(), Sema.hpp's resolveValueOrError()/
    //  resolveTypeNameOrError()/resolveCalleeOrError(), implemented in
    //  sema/support/Resolution.cpp)
    //
    // Sub-split, same reasoning as Syntax's 1001–1100/1101–2000 split below:
    //   2001–2100: general/structural name-resolution errors (the same
    //              three shapes -- "undefined value", "undefined type",
    //              "not callable" -- cover every resolveXOrError() case;
    //              see Resolution.cpp's own note on why nothing here has
    //              needed a specialized code yet)
    //   2101–3000: specialized/per-construct name-resolution errors
    //              (redeclaration in the same scope, ambiguous reference,
    //              use outside scope, ...) as those checks get wired up.
    // ═══════════════════════════════════════════════════════════════════════

    // ── 2001–2100: General ──
    E2001 = 2001,   ///< Undefined value (name folded into the single %s)
    E2002,          ///< Undefined type (name folded into the single %s)
    E2003,          ///< Not callable (resolves to a non-function value; folded into one %s)
    // 2004–2100 reserved for more general name-resolution codes as more
    // resolveXOrError()-shaped checks get wired up.

    // ── 2101–3000: Specialized ──
    E2101 = 2101,   ///< Redeclaration of a name already declared in this
                    ///< same tier (folded into one %s) -- see
                    ///< Resolution.cpp's note on why this is scoped to
                    ///< the CURRENT tier only, not ctx.lookupValue()'s
                    ///< full outer-scope search (legitimate shadowing is
                    ///< not redeclaration). Reused across every "two
                    ///< sibling declarations, same name" case: struct
                    ///< fields, trait fields, enum variants, generic
                    ///< params, and top-level values/types -- see
                    ///< SemaDecl.cpp.
    // 2102–3000 reserved for ambiguous-reference / use-outside-scope codes
    // as those checks get wired up.

    // ═══════════════════════════════════════════════════════════════════════
    // 3001–4000: Semantic — Type Checking
    // (AST → validated AST, the typing half: does this expression/
    //  declaration's type satisfy the rules? See Sema.hpp's checkExpr()
    //  family, resolveType() family, typesEqual()/isAssignable())
    //
    // Covers things like: type mismatch, invalid implicit conversion,
    // nullable/fallible used without narrowing, wrong argument count/type,
    // non-callable called, missing return.
    //
    // Sub-split, same reasoning as Syntax's 1001–1100/1101–2000 split below:
    //   3001–3100: general/structural type-checking errors (apply across
    //              many checkExpr rules — argument-count mismatches, for
    //              instance, are the same shape whether the callee is an
    //              ordinary function or an intrinsic)
    //   3101–4000: specialized/per-construct type-checking errors (specific
    //              to one checkExpr rule — intrinsics, pipelines, ...)
    // ═══════════════════════════════════════════════════════════════════════

    // ── 3001–3100: General ──
    E3001 = 3001,   ///< Wrong number of arguments to a call (name + counts folded into one %s by ctx.error() — see AttributesRegistry.hpp's note)
    E3002,          ///< Missing a required initializer (e.g. a `const` var
                    ///< with no `= expr`) -- see SemaDecl.cpp's analyzeVarDecl()
    E3003,          ///< Type mismatch -- deliberately generic (folded into
                    ///< one %s), reused for var/field initializers, and
                    ///< eventually arguments, assignments, and returns as
                    ///< those callers get wired up. See TypeCompat.cpp's
                    ///< isAssignable(), which is what decides this.
    E3004,          ///< Invalid const + nullable/fallible combination --
                    ///< reused for both FieldDeclAST and TraitFieldDeclAST,
                    ///< which document the identical rule separately.
    E3005,          ///< Missing return on a path through a non-void function
                    ///< -- see analyzeStmt()'s control-flow-reachability
                    ///< return value, which is what decides this.
    E3006,          ///< Duplicate value (e.g. two enum variants with the
                    ///< same explicit integer value) -- distinct from
                    ///< E2101, which is about duplicate NAMES, not values.
    // 3007–3100 reserved for more general type-checking codes (invalid
    // implicit conversion, nullable/fallible without narrowing, non-callable
    // called, ...) as those callers get wired up.

    // ── 3101–4000: Specialized (per checkExpr rule) ──
    E3101 = 3101,   ///< Unknown intrinsic (name folded into the single %s)
    // See Sema.hpp's checkIntrinsicCallExpr(): the arg-count half of
    // validating a `#name(...)` call uses the general E3001 above (the same
    // check a plain function call will use once checkCallExpr is wired up);
    // "does this name even exist" has no general equivalent — an unresolved
    // ordinary identifier is a Name-Resolution-phase concern (2000s), not
    // Type Checking, so this is intrinsic-specific rather than reusable.
    // 3102–4000 reserved for more specialized type-checking codes.

    // ═══════════════════════════════════════════════════════════════════════
    // 4001–5000: Semantic — Generics, Traits & FFI
    // (See Sema.hpp's validateGenericParamUsage(),
    //  validateTraitImplementation(), checkGenericArgs(),
    //  checkRecursiveFieldType(), validateForeignFunc(), validateAttributes())
    //
    // Covers things like: unused generic parameter, trait requirement not
    // satisfied, generic argument arity/constraint mismatch, illegal direct
    // self-reference (see SemaContext::definingTypeStack), invalid FFI
    // signature, attribute used somewhere it isn't allowed.
    //
    // Sub-split, same reasoning as Syntax's 1001–1100/1101–2000 split below:
    //   4001–4100: general/structural codes (apply across generics, traits,
    //              FFI, AND attributes alike — e.g. "not legal here" is the
    //              same shape whether it's an attribute on the wrong kind of
    //              declaration or a trait requirement unmet)
    //   4101–5000: specialized/per-construct codes (specific to one
    //              validator — a single attribute's own argument rules, one
    //              FFI ABI rule, ...)
    // ═══════════════════════════════════════════════════════════════════════

    // ── 4001–4100: General ──
    E4001 = 4001,   ///< Attribute/rule not legal here (name + reason folded into one %s)
    E4002,          ///< Wrong number of arguments for an attribute (folded into one %s)
    E4003,          ///< Unknown attribute (name folded into the single %s)
    // 4004–4100 reserved for more general Generics/Traits/FFI/Attribute
    // codes (unused generic parameter, trait requirement not satisfied,
    // generic argument arity mismatch, illegal self-reference, ...) as
    // validateGenericParamUsage()/validateTraitImplementation()/
    // checkGenericArgs()/checkRecursiveFieldType() get wired up.

    // ── 4101–5000: Specialized (per validator) ──
    E4101 = 4101,   ///< Unsupported foreign ABI (folded into one %s) — only "C" is supported
    // See AttributesRegistry.hpp's validateAttribute(): placement rules for
    // @[export]/@[foreign]/@[link]/@[inline] and argument-count rules for
    // @[foreign]/@[link] all reuse E4001/E4002/E4003 above — only the ABI
    // *value* check (is the string literally "C") has no general
    // equivalent, since no other attribute or FFI rule validates a specific
    // argument's content this way.
    // 4102–5000 reserved for more specialized codes (validateForeignFunc()'s
    // parameter/return-type FFI rules, trait/generic specifics, ...).

    // ═══════════════════════════════════════════════════════════════════════
    // 5001–6000: Backend / Linker
    // (IR generation, optimization, linking)
    // ═══════════════════════════════════════════════════════════════════════
    E5001 = 5001,   ///< Unresolved external symbol '%s'.
    E5002,          ///< Linker search path does not exist.
    E5003,          ///< Library format not recognised.
    E5004,          ///< Code generation failed (LLVM error).
    E5005,          ///< Target triple not supported.
    // 5006–5999 reserved for more Backend/Linker codes.

    // ═══════════════════════════════════════════════════════════════════════
    // 6001–7000: Warnings (cross-cutting — see architecture note above for
    // why this range isn't split by phase the way Semantic is)
    // ═══════════════════════════════════════════════════════════════════════
    W0001 = 6001,   ///< Unknown Warning
    W0002,          ///< Unreachable code.
    // 0003–1000 reserved for more warnings. The commented-out codes below
    // are placeholders for known future warnings, renumbered here to
    // continue the W0001/W0002 sequence (they previously read W6003 etc.,
    // a leftover naming inconsistency from before this range was
    // consolidated at 6000+ — fixed here; still commented out, still not
    // real codes):
    // W0003,          ///< Unused parameter '%s' (consider `_` or @phantom).
    // W0004,          ///< Unused function '%s'.
    // W0005,          ///< Deprecated item used: %s.
    // W0006,          ///< @extern with 'let' (should be 'const').
    // W0007,          ///< @extern function with non‑empty body (body ignored).
    // W0008,          ///< Nullable operation without explicit guard.
    // W0009,          ///< Default case in match may never be reached.
    // W0010,          ///< Inefficient slice range (exclusive vs inclusive).
    // W0011,          ///< ~async function called but result ignored.
    // W0012,          ///< ~nullable function called without checking result for nil.
    // W0013,          ///< Constant folding overflow.
    // W0014,          ///< ~nullable call without nil guard.
    // W0015,          ///< Duplicate implementation for trait.

    // 7001+ is UNRESERVED — no phase has been assigned this space. Pick a
    // range table entry above before adding anything at 7000+; don't just
    // start a new phase's codes there without updating the table.
};

// ─────────────────────────────────────────────────────────────────────────────
// Range-boundary tripwires — see this file's own "Range table" note for why
// these exist and how to maintain them.
// ─────────────────────────────────────────────────────────────────────────────
static_assert(static_cast<uint32_t>(DiagCode::E0005) < 100,   "Environment/Driver range (0-99) overflow");
static_assert(static_cast<uint32_t>(DiagCode::E0105) < 1000,  "Lexical range (100-999) overflow");
static_assert(static_cast<uint32_t>(DiagCode::E1011) < 1100,  "Syntax/General sub-range (1000-1099) overflow");
static_assert(static_cast<uint32_t>(DiagCode::E1108) < 2000,  "Syntax range (1000-1999) overflow");
static_assert(static_cast<uint32_t>(DiagCode::E2003) < 2100,  "Semantic/NameResolution General sub-range (2000-2099) overflow");
static_assert(static_cast<uint32_t>(DiagCode::E2101) < 3000,  "Semantic/NameResolution range (2000-2999) overflow");
static_assert(static_cast<uint32_t>(DiagCode::E3001) < 3100,  "Semantic/TypeChecking General sub-range (3000-3099) overflow");
static_assert(static_cast<uint32_t>(DiagCode::E3101) < 4000,  "Semantic/TypeChecking range (3000-3999) overflow");
static_assert(static_cast<uint32_t>(DiagCode::E4003) < 4100,  "Semantic/GenericsTraitsFFI General sub-range (4000-4099) overflow");
static_assert(static_cast<uint32_t>(DiagCode::E4101) < 5000,  "Semantic/GenericsTraitsFFI range (4000-4999) overflow");
static_assert(static_cast<uint32_t>(DiagCode::E5005) < 6000,  "Backend/Linker range (5000-5999) overflow");
static_assert(static_cast<uint32_t>(DiagCode::W0002) < 7000,  "Warning range (6000-6999) overflow");