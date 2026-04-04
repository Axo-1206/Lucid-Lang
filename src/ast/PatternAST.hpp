/**
 * @file PatternAST.hpp
 *
 * @responsibility Defines pattern-matching nodes (Bind, StructDestructure, Wildcard).
 *
 * @hierarchy BaseAST -> PatternAST -> [Concrete Nodes]
 *
 * @related_files 
 *   - src/ast/ExprAST.hpp (MatchExprAST holds these patterns)
 */

#pragma once

#include "BaseAST.hpp"
#include "TypeAST.hpp"
#include "ExprAST.hpp"

#include <string>
#include <vector>
#include <memory>
#include <optional>

// ─────────────────────────────────────────────────────────────────────────────
// PatternAST.hpp — all pattern nodes
//
// Every concrete pattern node inherits from PatternAST (defined in BaseAST.hpp).
// Patterns appear exclusively inside match expressions — they are never used
// as statements or expressions on their own.
//
// Include order:
//   PatternAST.hpp  →  #include "ExprAST.hpp"
//       because MatchArmAST bodies are ExprPtr / StmtPtr,
//       and LiteralPatternAST reuses LiteralKind from ExprAST.hpp.
//
// MatchExprAST (in ExprAST.hpp) forward-declares MatchArmAST and holds a
// vector<MatchArmPtr>. The full definition of MatchArmAST lives here.
// Any translation unit that needs the complete MatchArmAST (parser, semantic
// pass) must include PatternAST.hpp after ExprAST.hpp.
//
// Pattern grammar (from LUC_GRAMMAR.md):
//
//   pattern         := literal
//                    | range_expr
//                    | IDENTIFIER                  -- bind pattern
//                    | IDENTIFIER 'is' type        -- type pattern
//                    | WILDCARD                    -- _
//                    | struct_pattern
//
//   struct_pattern  := IDENTIFIER '{' { field_pattern } '}'
//   field_pattern   := IDENTIFIER [ ':' pattern ]
//
//   pattern_list    := pattern { ',' pattern }
//   guard           := 'if' expr
//   match_arm       := pattern_list [ guard ] '->' ( expr | block )
//   default_arm     := 'default' '->' ( expr | block )
//
// Node inventory:
//   LiteralPatternAST     — 42, "ok", true, false, nil
//   RangePatternAST       — 1..10
//   BindPatternAST        — n  (captures matched value into name)
//   WildcardPatternAST    — _  (matches anything, discards value)
//   TypePatternAST        — v is Circle  (bind + type narrow)
//   FieldPatternAST       — field name or name: sub-pattern  (helper, not PatternAST)
//   StructPatternAST      — Vec2 { x: 0.0, y }
//   MatchArmAST           — pattern_list [guard] -> body
//   DefaultArmAST         — default -> body
// ─────────────────────────────────────────────────────────────────────────────

// ═════════════════════════════════════════════════════════════════════════════
// PATTERN NODES
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// LiteralPatternAST
//
// Matches when the subject equals a specific literal value.
//   200       — integer literal
//   "ok"      — string literal
//   true      — boolean
//   nil       — nil check
//   0xFF      — hex literal
//
// Reuses LiteralKind from ExprAST.hpp — the same enum covers all literal
// forms in both expression and pattern position. value stores the raw lexeme.
//
// Note: multiple literal patterns on the same arm are expressed as separate
// entries in MatchArmAST::patterns, not as a single combined node:
//   200, 201, 202 -> "success"
//   produces three LiteralPatternAST nodes in one arm's pattern list.
// ─────────────────────────────────────────────────────────────────────────────

struct LiteralPatternAST : PatternAST {
    static constexpr ASTKind staticKind = ASTKind::LiteralPattern;

    LiteralKind  kind;
    std::string  value;   // raw lexeme: "200", "ok", "true", "0xFF"

    LiteralPatternAST(LiteralKind k, std::string v)
        : PatternAST(ASTKind::LiteralPattern), kind(k), value(std::move(v)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// RangePatternAST
//
// Matches when the subject falls within an inclusive range.
//   1..10   — matches 1, 2, 3, ..., 10
//   11..50  — matches 11 through 50
//
// Both lo and hi are stored as LiteralExprAST nodes — the grammar requires
// range patterns to use integer literals only. The semantic pass verifies
// this and checks that lo <= hi.
//
// Range patterns are inclusive on both ends, matching the grammar's general
// range semantics (same as slice indexing and for-loop ranges).
// ─────────────────────────────────────────────────────────────────────────────

struct RangePatternAST : PatternAST {
    static constexpr ASTKind staticKind = ASTKind::RangePattern;

    ExprPtr lo;   // LiteralExprAST — inclusive start
    ExprPtr hi;   // LiteralExprAST — inclusive end

    RangePatternAST() : PatternAST(ASTKind::RangePattern) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// BindPatternAST
//
// Matches any value and binds it to a name for use in the guard and arm body.
//   n            — bind without guard: captures matched value as 'n'
//   n if n < 50  — bind with guard: only matches when n < 50
//   arr          — bind array: arr.len() usable in guard and body
//
// The bound name is declared by the pattern itself — it is not looked up
// in an outer scope. The semantic pass introduces 'n' (or whatever the name
// is) as a new variable in the arm's scope with the type of the match subject.
//
// A BindPatternAST without a guard on its arm matches everything. The
// semantic pass enforces that an unconditional bind must come after all
// more-specific patterns (literal, range, type, struct) — otherwise
// subsequent arms would be unreachable.
//
// Note: the guard lives on MatchArmAST, not on the pattern. A single arm
// with a bind pattern has at most one guard. The pattern itself is just
// the name binding.
// ─────────────────────────────────────────────────────────────────────────────

struct BindPatternAST : PatternAST {
    static constexpr ASTKind staticKind = ASTKind::BindPattern;

    std::string name;   // "n", "arr", "s", "v"

    explicit BindPatternAST(std::string n)
        : PatternAST(ASTKind::BindPattern), name(std::move(n)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// WildcardPatternAST
//
// Matches any value and discards it — the '_' token.
//   _       -> "non-zero"
//
// Semantically identical to BindPatternAST except no name is introduced
// into scope. A wildcard pattern may appear with a guard, but since the
// value is discarded the guard cannot reference the matched value by name.
//
// '_' and 'default' are distinct:
//   _       — a pattern that matches anything and discards the value.
//             May appear in a normal arm (with or without a guard).
//   default — the required final fallback arm keyword. Not a pattern.
//
// The semantic pass enforces that _ does not appear as the last pattern
// before 'default' in a way that makes 'default' unreachable.
// ─────────────────────────────────────────────────────────────────────────────

struct WildcardPatternAST : PatternAST {
    static constexpr ASTKind staticKind = ASTKind::WildcardPattern;

    WildcardPatternAST() : PatternAST(ASTKind::WildcardPattern) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// TypePatternAST
//
// Matches when the subject is of a specific type, and binds the narrowed
// value to a name. Combines a bind and a runtime type check in one pattern.
//   s is Circle    — matches if subject is Circle, binds as 's' typed Circle
//   v is Rect      — matches if subject is Rect,   binds as 'v' typed Rect
//   e is Error     — matches if subject is Error,  binds as 'e' typed Error
//
// Grammar: IDENTIFIER 'is' type
//   bindName  — the name introduced into the arm's scope
//   checkType — the type being tested against
//
// After a successful match the semantic pass narrows bindName's type to
// checkType for the duration of the arm body. Outside the arm the original
// subject type is unchanged.
//
// Used for union type dispatch and 'any' type dispatch:
//   type Shape = Circle | Rect | Triangle
//   match shape {
//       s is Circle   -> s.radius * s.radius * 3.14159
//       s is Rect     -> s.width * s.height
//       default       -> 0.0
//   }
// ─────────────────────────────────────────────────────────────────────────────

struct TypePatternAST : PatternAST {
    static constexpr ASTKind staticKind = ASTKind::TypePattern;

    std::string  bindName;    // "s", "v", "e" — introduced into arm scope
    TypePtr      checkType;   // Circle, Rect, Error, ...

    TypePatternAST() : PatternAST(ASTKind::TypePattern) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// FieldPatternAST
//
// One field entry inside a struct pattern — a plain struct, not a PatternAST.
// Never appears independently — always owned by StructPatternAST.
//
// Two syntactic forms:
//   x        — shorthand: matches field 'x' and binds it to name 'x'
//              field="x",  subPattern=nullptr  (bind name = field name)
//   x: 0.0   — full form: matches field 'x' against sub-pattern 0.0
//              field="x",  subPattern=LiteralPatternAST(0.0)
//   x: Vec2 { ... } — nested: field 'x' matched against a nested struct pattern
//              field="x",  subPattern=StructPatternAST(...)
//
// When subPattern is nullptr the field is bound by name (shorthand form).
// When subPattern is present the field value must match the sub-pattern.
//
// Note: struct patterns use ':' as the field separator — this is pattern
// syntax only. Struct literals always use '=' (never ':').
// ─────────────────────────────────────────────────────────────────────────────

struct FieldPatternAST {
    std::string               field;        // field name from the struct
    std::unique_ptr<PatternAST> subPattern; // nullptr → shorthand bind by name
    SourceLocation            loc;
};

using FieldPatternPtr = std::unique_ptr<FieldPatternAST>;

// ─────────────────────────────────────────────────────────────────────────────
// StructPatternAST
//
// Matches when the subject is a struct of the named type and its fields
// satisfy the given field patterns.
//
//   Vec2 { x: 0.0, y: 0.0 }   — exact match on both fields
//   Vec2 { x, y }              — shorthand: binds x and y from subject
//   Player { health: 0 }       — matches only when health == 0, other fields ignored
//   Player { pos: Vec2 { x: 0.0, y: 0.0 }, health }  — nested pattern
//
// typeName   — the struct type name ("Vec2", "Player")
// fields     — the field patterns in source order
//
// Fields not listed in the pattern are ignored — the match succeeds as long
// as the listed fields satisfy their patterns. This is intentional: you don't
// have to list every field.
//
// The semantic pass verifies:
//   - typeName resolves to a struct in the symbol table
//   - every listed field name exists on that struct
//   - sub-pattern types are compatible with the field types
// ─────────────────────────────────────────────────────────────────────────────

struct StructPatternAST : PatternAST {
    static constexpr ASTKind staticKind = ASTKind::StructPattern;

    std::string                   typeName;  // "Vec2", "Player"
    std::vector<FieldPatternPtr>  fields;    // field patterns in source order

    StructPatternAST() : PatternAST(ASTKind::StructPattern) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

