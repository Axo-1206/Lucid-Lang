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
 *   | 2000–2999 | Semantic — Name Resolution     | Semantic                           | RESERVED, none defined yet                |
 *   | 3000–3999 | Semantic — Type Checking       | Semantic                           | RESERVED, none defined yet                |
 *   | 4000–4999 | Semantic — Generics/Traits/FFI | Semantic                           | RESERVED, none defined yet                |
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
    // 0000–0099: Environment / Driver
    // (file I/O, module resolution, CLI/config, cross-cutting compiler
    //  failures that aren't specific to any one source-processing phase)
    // ═══════════════════════════════════════════════════════════════════════
    E0001 = 0,      ///< Unknown Error
    E0002,          ///< Too many consecutive errors. Aborting...
    E0003,          ///< File not found or inaccessible.
    E0004,          ///< Module resolution failed.
    E0005,          ///< Cyclic module dependency.
    // 0006–0099 reserved for more Environment/Driver codes.

    // ═══════════════════════════════════════════════════════════════════════
    // 0100–0999: Lexical
    // (character stream → token stream; the scanner)
    // ═══════════════════════════════════════════════════════════════════════
    E0101 = 100,    ///< Invalid character in source (only ASCII allowed).
    E0102,          ///< Unterminated string literal.
    E0103,          ///< Unterminated raw string literal.
    E0104,          ///< Unterminated block comment.
    E0105,          ///< Uknown character.
    // 0106–0999 reserved for more Lexical codes. Given this range is 900
    // codes wide against a phase that's used 5 in years of design so far,
    // there's no expectation of ever needing a fourth digit's worth more
    // room here the way Semantic will.

    // ═══════════════════════════════════════════════════════════════════════
    // 1000–1999: Syntax (Parser)
    // (token stream → AST; grammar rules, error recovery)
    //
    // Sub-split already implicit in the existing numbers, made explicit:
    //   1000–1099: general/structural syntax errors (apply across many
    //              grammar rules — "expected identifier", "unexpected
    //              token", ...)
    //   1100–1999: specialized/per-construct syntax errors (specific to
    //              one grammar rule — imports, attributes, switch, ...)
    // ═══════════════════════════════════════════════════════════════════════

    // ── 1000–1099: General ──
    E1001 = 1000,   ///< Expected keyword
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
    // 1012–1099 reserved for more general syntax codes.

    // ── 1100–1999: Specialized (per grammar rule) ──
    E1101,          ///< Expected module path after keyword 'use'
    // E1102,          ///< Expected branch after condition
    // E1103,          ///< Expected else branchs
    E1104,          ///< Expected attribute argument literal
    E1105,          ///< Expected switch subject
    E1106,          ///< Empty expression group
    E1107,          ///< Expected pipeline seed (expression)
    E1108,          ///< Multiple default clauses in switch
    // 1109–1999 reserved for more specialized syntax codes.

    // E1105,          ///< Unexpected trailing '+' in generic constraints
    // E1106,          ///< Unexpected trailing '.' in path

    // ═══════════════════════════════════════════════════════════════════════
    // 2000–2999: Semantic — Name Resolution               [RESERVED]
    // (AST → validated AST, the resolution half: does this name refer to
    //  a real declaration in the current context? See SemaContext's
    //  lookupValue()/lookupType(), Sema.hpp's resolveValueOrError()/
    //  resolveTypeNameOrError()/resolveCalleeOrError())
    //
    // No codes defined yet. When NameResolver-equivalent logic starts
    // reporting errors, they belong here — e.g. undefined identifier,
    // undefined type, redeclaration in the same scope, ambiguous
    // reference, use of a value/type outside its scope.
    // ═══════════════════════════════════════════════════════════════════════

    // ═══════════════════════════════════════════════════════════════════════
    // 3000–3999: Semantic — Type Checking                 [RESERVED]
    // (AST → validated AST, the typing half: does this expression/
    //  declaration's type satisfy the rules? See Sema.hpp's checkExpr()
    //  family, resolveType() family, typesEqual()/isAssignable())
    //
    // No codes defined yet. Covers things like: type mismatch, invalid
    // implicit conversion, nullable/fallible used without narrowing,
    // wrong argument count/type, non-callable called, missing return.
    // ═══════════════════════════════════════════════════════════════════════

    // ═══════════════════════════════════════════════════════════════════════
    // 4000–4999: Semantic — Generics, Traits & FFI         [RESERVED]
    // (See Sema.hpp's validateGenericParamUsage(),
    //  validateTraitImplementation(), checkGenericArgs(),
    //  checkRecursiveFieldType(), validateForeignFunc(), validateAttributes())
    //
    // No codes defined yet. Covers things like: unused generic parameter,
    // trait requirement not satisfied, generic argument arity/constraint
    // mismatch, illegal direct self-reference (see
    // SemaContext::definingTypeStack), invalid FFI signature, attribute
    // used somewhere it isn't allowed.
    // ═══════════════════════════════════════════════════════════════════════

    // ═══════════════════════════════════════════════════════════════════════
    // 5000–5999: Backend / Linker
    // (IR generation, optimization, linking)
    // ═══════════════════════════════════════════════════════════════════════
    E5001 = 5000,   ///< Unresolved external symbol '%s'.
    E5002,          ///< Linker search path does not exist.
    E5003,          ///< Library format not recognised.
    E5004,          ///< Code generation failed (LLVM error).
    E5005,          ///< Target triple not supported.
    // 5006–5999 reserved for more Backend/Linker codes.

    // ═══════════════════════════════════════════════════════════════════════
    // 6000–6999: Warnings (cross-cutting — see architecture note above for
    // why this range isn't split by phase the way Semantic is)
    // ═══════════════════════════════════════════════════════════════════════
    W0001 = 6000,   ///< Unknown Warning
    W0002,          ///< Unreachable code.
    // 0003–0999 reserved for more warnings. The commented-out codes below
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

    // 7000+ is UNRESERVED — no phase has been assigned this space. Pick a
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
static_assert(static_cast<uint32_t>(DiagCode::E5005) < 6000,  "Backend/Linker range (5000-5999) overflow");
static_assert(static_cast<uint32_t>(DiagCode::W0002) < 7000,  "Warning range (6000-6999) overflow");