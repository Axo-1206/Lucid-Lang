#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// Pointer Type (Sealed Conduit)
// ============================================================================
// 
// parsePtrType() parses a raw pointer type.
// 
// Grammar: '*' type
// 
// Example: `*uint8`, `*VkInstance`
// 
// ─── The Sealed Conduit Model ─────────────────────────────────────────────
//   - Raw pointers are "sealed conduits" – cannot be dereferenced directly
//   - Allowed: store, pass to @extern, nil check, pointer intrinsics
//   - Forbidden: dereference (*ptr), field access, indexing, arithmetic
//   - Boundary crossing: #ptrToRef(ptr) → &T, #refToPtr(ref) → *T
// 
// ─── Restrictions (Semantic Pass) ─────────────────────────────────────────
//   - Raw pointers only valid inside @extern‑decorated declarations
//   - Parser produces PtrTypeAST regardless; semantic pass reports error
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at '*'
// On exit:  positioned after the inner type
// 
// ─── Note ─────────────────────────────────────────────────────────────────
//   - Same note as RefType: inner type parsed via parseBaseType()
//   - '?' attaches to inner type, not to the pointer itself
// ============================================================================

TypePtr Parser::parsePtrType() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::MUL, "expected '*'");
    TypePtr inner = parseBaseType();
    if (!inner) {
        errorAt(DiagCode::E2005, "expected type after '*'");
        return arena_.make<UnknownTypeAST>();
    }
    auto node = arena_.make<PtrTypeAST>(std::move(inner));
    node->loc = loc;
    return node;
}