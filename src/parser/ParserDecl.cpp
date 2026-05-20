/**
 * @file ParserDecl.cpp
 *
 * @responsibility Parses all declaration‑oriented grammar rules.
 *
 * This file implements the parsers for:
 *   - Attributes and their arguments (@extern, @inline, etc.)
 *   - Package, use, variable, function, struct, enum, trait, impl, from, and type alias declarations
 *   - Trait methods, impl methods, and from entries
 *   - Helper validateAnonFuncBodySig for verbose anon‑func body signatures
 *
 * All functions consume tokens from the parser’s stream and build corresponding
 * AST nodes. They include error recovery and infinite‑loop prevention mechanisms.
 *
 * @related
 *   - Parser.hpp – class declaration and shared utilities
 *   - Parser.cpp – core token stream primitives and top‑level dispatch
 *   - ParserExpr.cpp – expression parsing (used in initialisers and bodies)
 *   - ParserStmt.cpp – statement parsing (blocks, control flow)
 *   - ParserType.cpp – type annotation parsing
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * NAVIGATION – Functions in this file (in order of appearance)
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * ██ Shared Helpers
 *   validateAnonFuncBodySig()          – validate verbose anon‑func body signature
 *
 * ██ Attributes
 *   parseAttributes()                  – zero or more '@' attributes
 *   parseAttribute()                   – single '@' attribute
 *   parseAttributeArgLiteral()         – literal argument inside attribute
 *
 * ██ Top-level Declarations
 *   parsePackageDecl()                 – 'package' name
 *
 * ██ Declarations
 *   parseUseDecl()                     – 'use' module path [as alias]
 *   parseVarDecl()                     – let/const variable (keyword already consumed)
 *   parseFuncDecl()                    – let/const function (keyword already consumed)
 *
 * ██ Type Declarations
 *   parseStructDecl()                  – 'struct' with fields
 *   parseFieldDecl()                   – field inside struct
 *   parseEnumDecl()                    – 'enum' with variants
 *   parseEnumVariant()                 – variant [= value]
 *   parseTraitDecl()                   – 'trait' with methods
 *   parseTraitMethod()                 – method signature inside trait
 *   parseImplDecl()                    – 'impl' block for struct
 *   parseTraitRef()                    – ': TraitName[<args>]'
 *   parseMethodDecl()                  – method inside impl
 *   parseFromDecl()                    – 'from' conversion block
 *   parseTypeAliasDecl()               – 'type' alias definition
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * @note: Each declaration parser assumes the leading keyword (or visibility
 * modifier) has already been consumed by parseDeclaration() in Parser.cpp.
 * The functions are responsible for consuming their entire syntactic domain
 * and reporting errors with recovery.
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// validateAnonFuncBodySig
//
// Shared helper for parseFuncDecl and parseMethodDecl.
// Validates and consumes the verbose anonymous‑function body signature form:
//   = (params) -> ret { ... }   or   = (a int)(b int) -> ret { ... } (curried)
//
// ─── Purpose ────────────────────────────────────────────────────────────────
// When a function/method body is written in the verbose anon‑func syntax,
// the body repeats the parameter groups and return type. The declaration header
// is authoritative. This helper consumes the repeated signature, validates it
// against the declared signature, and issues appropriate diagnostics.
//
// ─── Token Consumption ──────────────────────────────────────────────────────
// On entry : parser is positioned at the first token after '=' (may be '(' or '{').
// On return: parser is positioned after the repeated signature (at '{' or at a
//            bad token if an error occurred).
//
// Consumption steps:
//   1. Rejects any '~' qualifiers (anonymous functions cannot have them) and
//      consumes them if present (for error recovery).
//   2. Consumes all repeated parameter groups (each '(' ... ')') by calling
//      parseParamGroup() – the groups are discarded; the declaration’s signature is
//      authoritative.
//   3. Optionally consumes '->' and parses the repeated return list via
//      parseReturnList().
//   4. Validates or adopts return types as described below.
//
// ─── Validation Logic ───────────────────────────────────────────────────────
//   - If the declaration has no return types (void) and the body has no '->':
//         OK (void function).
//   - If the declaration has no return types but the body has '->':
//         Adopts the body’s return types (the semantic pass may issue a warning).
//   - If the declaration has return types but the body has no '->':
//         Error: "expected '->' return list in body".
//   - If both have return types:
//         Compares count and kind (ASTKind) of each type.
//         Mismatches trigger an error (the declaration is authoritative).
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Qualifiers found in the body: error reported, then consumed (to avoid
//   infinite loop).
// - Missing return type after '->': error reported.
// - After validation, the caller must parse the body block (already positioned
//   at '{' or error token).
//
// ─── Infinite Loop Prevention ───────────────────────────────────────────────
// - The parameter group loop consumes one '(' each iteration and always makes
//   progress because parseParamGroup() consumes its entire group or reports an
//   error and advances.
// - The return list parser (parseReturnList) guarantees progress.
// - No unbounded recursion; all loops are bounded by the number of '(' tokens.
// ─────────────────────────────────────────────────────────────────────────────
void Parser::validateAnonFuncBodySig(FuncSignature& declaredSig, const std::string& declName) {
    // ── 1. Anonymous functions cannot have qualifiers ─────────────────────
    if (check(TokenType::TILDE)) {
        errorAt(DiagCode::E2002,
                "anonymous function body cannot have qualifiers (e.g., ~async, ~nullable). "
                "Qualifiers belong on the declaration itself.");
        // Skip qualifiers to recover (consume ~ and following identifier)
        while (check(TokenType::TILDE)) {
            advance(); // consume '~'
            if (!check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected qualifier name after '~'");
                break;
            }
            advance(); // consume qualifier name
        }
    }

    // ── 2. Consume all repeated parameter groups (curried) ────────────────
    // These are the parameter groups written in the body, e.g., (a int)(b int)
    while (check(TokenType::LPAREN)) {
        parseParamGroup(); // discard; the declaration's signature is authoritative
    }

    // ── 3. Parse the repeated return list after optional '->' ─────────────
    bool hasArrow = match(TokenType::ARROW);
    std::vector<TypePtr> bodyReturnTypes;

    if (hasArrow) {
        bodyReturnTypes = parseReturnList();
        if (bodyReturnTypes.empty()) {
            errorAt(DiagCode::E2005, "expected at least one return type after '->'");
        }
    }

    // ── 4. Validate or adopt return types ─────────────────────────────────
    // Case 1: Declaration has no return types (void function)
    if (declaredSig.returnTypes.empty()) {
        if (hasArrow) {
            if (!bodyReturnTypes.empty()) {
                // Adopt the body's return types (the semantic pass may still warn)
                LUC_LOG_PARSER("validateAnonFuncBodySig: adopting return types from body for '"
                               << declName << "'");
                declaredSig.returnTypes = std::move(bodyReturnTypes);
            } else {
                errorAt(DiagCode::E2005, "expected return type after '->' for function '" + declName + "'");
            }
        }
        // else: both void → OK
    }
    // Case 2: Declaration has return types, but body has no '->'
    else if (!hasArrow) {
        errorAt(DiagCode::E2001,
                "expected '->' return list in body for function '" + declName +
                "' because the declaration has a return type");
    }
    // Case 3: Both have return types – compare counts and kinds
    else {
        if (declaredSig.returnTypes.size() != bodyReturnTypes.size()) {
            errorAt(DiagCode::E2005,
                    "return type count mismatch for function '" + declName +
                    "': declaration has " + std::to_string(declaredSig.returnTypes.size()) +
                    ", body has " + std::to_string(bodyReturnTypes.size()));
        } else {
            for (size_t i = 0; i < declaredSig.returnTypes.size(); ++i) {
                // Simple structural comparison (kind equality)
                if (declaredSig.returnTypes[i]->kind != bodyReturnTypes[i]->kind) {
                    errorAt(DiagCode::W3001,
                            "return type #" + std::to_string(i) + " in body does not match "
                            "declared return type for function '" + declName +
                            "'; the declaration is authoritative");
                    break; // only report first mismatch
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Attributes
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parseAttributes
//
// Parses zero or more attribute directives that precede a declaration.
//
// Grammar:
//   attributes := { attribute }
//   attribute  := '@' IDENTIFIER [ '(' attr_arg_list ')' ]
//
// Examples:
//   @inline
//   @deprecated("Use newAPI")
//   @extern("malloc") @noalias
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - While the current token is AT_SIGN, calls parseAttribute().
// - Each call to parseAttribute() consumes the entire '@' directive (including
//   its parentheses and arguments).
// - Stops when the current token is not AT_SIGN.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - If parseAttribute() returns nullptr and no progress was made (pos_ unchanged),
//   reports an error and forcibly consumes the '@' token to avoid infinite loop.
// - Invalid attributes are skipped; parsing continues with the next attribute
//   or the following declaration.
//
// ─── Loop Safety ─────────────────────────────────────────────────────────────
// - Uses a progress guard: saves pos_ before calling parseAttribute().
// - If parseAttribute() fails to advance the parser, consumes one token (the '@')
//   and continues – guarantees forward progress.
// - The loop is bounded by the number of '@' tokens in the token stream.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<AttributePtr> Parser::parseAttributes() {
    LUC_LOG_PARSER_VERBOSE("parseAttributes: starting");
    std::vector<AttributePtr> attrs;
    while (check(TokenType::AT_SIGN)) {
        std::size_t savedPos = pos_;
        AttributePtr attr = parseAttribute();
        if (attr) {
            LUC_LOG_PARSER_VERBOSE("\tParsed attribute: @" << pool_.lookup(attr->name));
            attrs.push_back(std::move(attr));
        } else {
            // parseAttribute failed. Ensure we make progress.
            if (pos_ == savedPos && !isAtEnd()) {
                errorAt(DiagCode::E2002, "invalid attribute syntax, skipping '@'");
                advance(); // consume the '@' to avoid infinite loop
            }
        }
    }
    LUC_LOG_PARSER_VERBOSE("parseAttributes: found " << attrs.size() << " attributes");
    return attrs;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseAttribute
//
// Parses a single attribute directive starting with '@'.
//
// Grammar:
//   attribute := '@' IDENTIFIER [ '(' attr_arg_list ')' ]
//   attr_arg_list := attr_arg { ',' attr_arg }
//   attr_arg   := STRING_LITERAL | INT_LITERAL | HEX_LITERAL | BINARY_LITERAL
//               | 'true' | 'false' | IDENTIFIER
//
// Examples:
//   @inline
//   @extern("malloc")
//   @deprecated("Use newAlloc", true)
//
// Returns an AttributePtr (never nullptr on success; may return nullptr on error
// after reporting an error).
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the leading '@' token.
// - Consumes an IDENTIFIER as the attribute name.
// - If a '(' follows, enters argument list parsing:
//     * While not RPAREN and not EOF:
//         - Optionally consumes COMMA.
//         - Calls parseAttributeArgLiteral() to consume one argument.
//         - If argument parsing fails without progress, consumes one token to avoid stall.
//     * Consumes the closing ')'.
// - Does NOT consume any tokens beyond the attribute (i.e., stops at the next
//   token after the ')' or after the attribute name if no arguments).
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing attribute name after '@': reports error, returns nullptr.
// - Invalid token inside argument list: reports error, consumes the offending token,
//   and continues to parse next argument (or stops if progress is impossible).
// - Missing closing ')': reports error, consumes tokens until a safe boundary
//   (end of file or next declaration start) is found.
//
// ─── Loop Safety ─────────────────────────────────────────────────────────────
// - The argument list loop uses a progress guard: saves pos_ before each
//   parseAttributeArgLiteral() call.
// - If no progress is made, consumes one token, reports an error, and continues
//   to the next iteration (the loop will either find a comma or break).
// - The loop always advances because it consumes at least one token per iteration
//   (either a comma, an argument, or the closing parenthesis).
// ─────────────────────────────────────────────────────────────────────────────
AttributePtr Parser::parseAttribute() {
    if (!check(TokenType::AT_SIGN))
        return nullptr;

    SourceLocation loc = currentLoc();
    advance(); // consume '@'

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected attribute name after '@'");
        return nullptr;
    }

    // 1. Intern the attribute name
    auto attr = arena_.make<AttributeAST>();
    attr->loc  = loc;
    attr->name = pool_.intern(advance().value);

    // Optional argument list: '(' attr_arg { ',' attr_arg } ')'
    if (match(TokenType::LPAREN)) {
        while (!check(TokenType::RPAREN) && !isAtEnd()) {
            match(TokenType::COMMA); // optional separator
            if (check(TokenType::RPAREN)) break;

            std::size_t savedPos = pos_;
            AttributeArgPtr arg = parseAttributeArgLiteral();
            if (arg) {
                attr->args.push_back(std::move(arg));
            } else {
                if (pos_ == savedPos) {
                    errorAt(DiagCode::E2002, "unexpected token in attribute arguments: '" + peek().value + "'");
                    advance();
                }
            }
        }
        consume(TokenType::RPAREN, DiagCode::E2001, "expected ')' to close attribute argument list");
    }

    return attr;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseAttributeArgLiteral
//
// Parses a single literal argument inside an attribute's argument list.
//
// Grammar:
//   attr_arg := STRING_LITERAL
//             | INT_LITERAL | HEX_LITERAL | BINARY_LITERAL
//             | 'true' | 'false'
//             | IDENTIFIER   (type name, e.g., in @sizeof(T))
//
// Returns an AttributeArgPtr (never nullptr on success; returns nullptr on error
// after reporting an error).
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Inspects the current token and consumes it if it matches a literal type:
//     * STRING_LITERAL          → AttributeArgKind::StringLit
//     * INT/HEX/BINARY_LITERAL  → AttributeArgKind::IntLit
//     * TRUE/FALSE              → AttributeArgKind::BoolLit
//     * IDENTIFIER              → AttributeArgKind::TypeIdent
// - For any other token, returns nullptr without consuming anything.
// - Does NOT consume any tokens beyond the literal (stops after the matched token).
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - No error is reported by this function; it simply returns nullptr on mismatch.
//   The caller (parseAttribute) is responsible for reporting an appropriate error.
// - This allows the caller to attempt recovery (e.g., skip the offending token).
//
// ─── Notes ──────────────────────────────────────────────────────────────────
// - The value is stored as an InternedString of the raw token text.
// - No semantic validation is performed here (e.g., integer bounds, type name
//   resolution) – that is left to the semantic phase.
// ─────────────────────────────────────────────────────────────────────────────
AttributeArgPtr Parser::parseAttributeArgLiteral() {
    SourceLocation loc = currentLoc();

    if (check(TokenType::STRING_LITERAL)) {
        auto arg = arena_.make<AttributeArgAST>(
            AttributeArgKind::StringLit,
            pool_.intern(advance().value)
        );
        arg->loc = loc;
        return arg;
    }

    if (checkAny({TokenType::INT_LITERAL, TokenType::HEX_LITERAL, TokenType::BINARY_LITERAL})) {
        auto arg = arena_.make<AttributeArgAST>(
            AttributeArgKind::IntLit,
            pool_.intern(advance().value)
        );
        arg->loc = loc;
        return arg;
    }

    if (check(TokenType::TRUE)) {
        advance();
        auto arg = arena_.make<AttributeArgAST>(
            AttributeArgKind::BoolLit,
            pool_.intern("true")
        );
        arg->loc = loc;
        return arg;
    }

    if (check(TokenType::FALSE)) {
        advance();
        auto arg = arena_.make<AttributeArgAST>(
            AttributeArgKind::BoolLit,
            pool_.intern("false")
        );
        arg->loc = loc;
        return arg;
    }

    if (check(TokenType::IDENTIFIER)) {
        auto arg = arena_.make<AttributeArgAST>(
            AttributeArgKind::TypeIdent,
            pool_.intern(advance().value)
        );
        arg->loc = loc;
        return arg;
    }

    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Top-level Declarations
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parsePackageDecl
//
// Parses the mandatory package declaration at the start of every Luc file.
//
// Grammar:
//   package_decl := 'package' IDENTIFIER
//
// Example:
//   package math
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'package' keyword.
// - Consumes an IDENTIFIER as the package name.
// - Does NOT consume any tokens beyond the identifier.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - If 'package' is missing, the caller (parse()) handles the error and inserts
//   a dummy node.
// - If the package name is missing (no IDENTIFIER), reports an error and returns
//   a dummy node with name "<error>" to allow parsing to continue.
//
// ─── Return Value ───────────────────────────────────────────────────────────
// - Always returns a non-null PackageDeclAST node (even on error, a dummy node
//   is returned). The caller should check the diagnostic engine for errors.
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<PackageDeclAST> Parser::parsePackageDecl() {
    SourceLocation loc = currentLoc();
    consume(TokenType::PACKAGE, "expected 'package'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected package name");
        // Return a valid node with error name instead of nullptr
        auto node = arena_.make<PackageDeclAST>(pool_.intern("<error>"));
        node->loc = loc;
        return node;
    }
    
    std::string nameRaw = advance().value;
    InternedString name = pool_.intern(nameRaw);
    auto node = arena_.make<PackageDeclAST>(name);
    node->loc = loc;
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// Declarations
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parseUseDecl
//
// Parses a 'use' declaration that imports a module path into the current scope.
//
// Grammar:
//   use_decl := 'use' module_path [ 'as' IDENTIFIER ]
//   module_path := IDENTIFIER { '.' IDENTIFIER }
//
// Examples:
//   use math.vec2
//   use renderer.types
//   use math as m
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'use' keyword.
// - Consumes the module path (sequence of dot‑separated identifiers):
//     * First IDENTIFIER is mandatory.
//     * While the next token is '.' followed by an IDENTIFIER, consumes both.
// - Optionally consumes 'as' followed by an IDENTIFIER (alias).
// - Stops after the alias identifier (or after the last path segment if no alias).
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - If the module path is missing (no IDENTIFIER after 'use'), reports an error
//   and returns a node with an empty path.
// - If 'as' is present but no alias identifier follows, reports an error.
// - The returned node is always non‑null; errors are recorded in the diagnostic
//   engine.
//
// ─── Loop Safety ─────────────────────────────────────────────────────────────
// - The module path loop iterates once per dot‑separated segment. Each iteration
//   consumes a '.' and an IDENTIFIER, guaranteeing progress.
// - No unbounded loops; the loop stops when no '.' or following identifier is found.
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<UseDeclAST> Parser::parseUseDecl(Visibility vis) {
    SourceLocation loc = currentLoc();
    consume(TokenType::USE, "expected 'use'");

    auto node = arena_.make<UseDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    // Parse module path: IDENTIFIER { '.' IDENTIFIER }
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected module path after 'use'");
        return node;
    }

    node->path.push_back(pool_.intern(advance().value));

    while (check(TokenType::DOT) && peekNext().type == TokenType::IDENTIFIER) {
        advance(); // consume '.'
        node->path.push_back(pool_.intern(advance().value));
    }

    // Optional alias: 'as' IDENTIFIER
    if (match(TokenType::AS)) {
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected alias name after 'as'");
        } else {
            node->alias = pool_.intern(advance().value);
        }
    }

    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseVarDecl
//
// Parses a variable declaration (let or const) after the keyword has been consumed.
//
// Grammar:
//   var_decl := decl_keyword IDENTIFIER type_ann [ '=' expr ]
//
// Important: The caller (parseDeclaration) has already consumed the 'let' or 'const'
// keyword. This function is called with pos_ positioned on the variable name.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes an IDENTIFIER (variable name).
// - Consumes a type annotation via parseType() (required).
// - Optionally, if '=' is present, consumes it and parses an initialiser expression.
// - Does NOT consume any trailing semicolon (the caller may handle optional semicolons).
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing variable name: reports error, returns nullptr.
// - Type annotation missing or invalid: reports error, returns nullptr.
// - If '=' is present but no expression follows: reports error, returns node with
//   null init (the caller may recover).
// - Attributes are attached to the node but not validated here (semantic phase).
//
// ─── Notes ──────────────────────────────────────────────────────────────────
// - The keyword (let/const) is retrieved from the previous token (pos_ - 1).
// - The semantic phase enforces @extern constraints and const initialiser rules.
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<VarDeclAST> Parser::parseVarDecl(Visibility vis) {
    // The keyword was consumed by parseTopLevelDecl before this call.
    const Token &kwTok = tokens_[pos_ - 1];
    DeclKeyword kw;
    switch (kwTok.type) {
    case TokenType::LET:
        kw = DeclKeyword::Let;
        break;
    default:
        kw = DeclKeyword::Const;
        break;
    }

    SourceLocation loc = currentLoc();

    // Name
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected variable name");
        return nullptr;
    }
    std::string nameRaw = advance().value;
    InternedString name = pool_.intern(nameRaw);
    LUC_LOG_PARSER("Variable name: '" << nameRaw << "'");

    // Type annotation (required)
    if (!looksLikeType()) {
        errorAt(DiagCode::E2005, "expected type annotation for '" + nameRaw + "'");
        return nullptr;
    }

    TypePtr type = parseType();
    if (!type) {
        errorAt(DiagCode::E2005, "expected type for variable '" + nameRaw + "'");
        return nullptr;
    }

    // Optional initialiser — the semantic phase enforces whether an initialiser
    // is required or forbidden (e.g. @extern must not have one; const requires one).
    ExprPtr init;
    if (match(TokenType::ASSIGN)) {
        init = parseExpr();
        if (!init) {
            errorAt(DiagCode::E2008, "expected expression after '=' in variable declaration");
        }
    }

    // Allocate node via arena
    auto node = arena_.make<VarDeclAST>();
    node->loc = loc;
    node->keyword = kw;
    node->name = name;  
    node->type = std::move(type);
    node->init = std::move(init);
    node->visibility = vis;

    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseFuncDecl
//
// Parses a function declaration (let or const) after the keyword has been consumed.
//
// Grammar:
//   func_decl := decl_keyword IDENTIFIER [ generic_params ]
//                [ qualifier_list ] param_group { param_group }
//                [ '->' return_list ]
//                [ '=' func_body ]
//
//   qualifier_list := { '~' IDENTIFIER }
//   return_list     := return_type { ',' return_type }
//   return_type     := type | param_group { param_group } '->' return_list
//   func_body       := block | anon_func | expr
//
// Examples:
//   let square (x int) -> int = { return x * x }
//   let fetch ~async (url string) -> string = { return await httpGet(url) }
//   let add (a int)(b int) -> int = { return a + b }
//   @extern("malloc") const malloc (size uint64) -> *uint8?   -- no body
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes an IDENTIFIER (function name).
// - Optional generic parameters via parseGenericParams().
// - Optional qualifiers ('~' IDENTIFIER) – stored raw, resolved semantically.
// - One or more parameter groups via parseParamGroup().
// - Optional '->' followed by return list via parseReturnList().
// - Optional '=' followed by body (block, verbose anon‑func, or expression).
// - Does NOT consume trailing semicolon (caller handles optional semicolons).
//
// ─── Body Parsing Variants ───────────────────────────────────────────────────
//   1. Block body:          = { ... }                    → parseBlock()
//   2. Verbose anon‑func:   = (params) -> ret { ... }    → validateAnonFuncBodySig() + parseBlock()
//   3. Expression body:     = expr                       → wrapped in ReturnStmt + BlockStmt
//   4. No body:             (no '=')                     → node->body remains nullptr
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing function name after keyword: reports error, returns nullptr.
// - Missing '(' after name/generics/qualifiers: reports error, returns nullptr.
// - If parseParamGroup() fails during any group, the function returns nullptr.
// - Missing body after '=': reports error, returns nullptr.
// - The semantic phase enforces @extern body rules (no body required/forbidden).
//
// ─── Loop Safety ─────────────────────────────────────────────────────────────
// - Parameter group loop consumes one '(' per iteration; parseParamGroup() makes
//   progress or reports error and consumes tokens.
// - Qualifier loop consumes '~' and following IDENTIFIER per iteration.
// - No unbounded loops; each loop is bounded by the number of tokens present.
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<FuncDeclAST> Parser::parseFuncDecl(DeclKeyword kw, Visibility vis) {
    SourceLocation loc = currentLoc();

    LUC_LOG_PARSER("=== parseFuncDecl START ===");
    LUC_LOG_PARSER("Current token (peek filtered): '" << peek().value << "' (type: " << static_cast<int>(peek().type) << ")");

    // Name
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected function name");
        return nullptr;
    }
    std::string nameRaw = advance().value;
    InternedString name = pool_.intern(nameRaw);
    LUC_LOG_PARSER("Function name: '" << nameRaw << "'");
    
    // Allocate via arena
    auto node = arena_.make<FuncDeclAST>();
    node->loc = loc;
    node->keyword = kw;
    node->name = name;
    node->visibility = vis;

    // Optional generic params
    if (check(TokenType::LESS)) {
        LUC_LOG_PARSER("Found generic parameters");
        node->genericParams = parseGenericParams();
    }

    // ── Collect raw qualifier names (~async, ~nullable, ~parallel, ...) ───────
    // The parser does NOT validate qualifier names — it stores them as raw
    // InternedStrings. The semantic phase resolves names to bits via
    // QualifierRegistry and reports any unknown qualifier.
    while (check(TokenType::TILDE)) {
        advance(); // consume '~'
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        std::string qualRaw = advance().value;
        node->sig.rawQualifiers.push_back(pool_.intern(qualRaw));
        LUC_LOG_PARSER_VERBOSE("\traw qualifier stored: '~" << qualRaw << "'");
    }
    // sig.qualifiers bitmask starts at 0 — filled by the semantic phase.

    // ── Parse parameter groups ────────────────────────────────────────────────
    LUC_LOG_PARSER("Checking for '(' in raw token stream...");
    
    std::size_t nextNonComment = pos_;
    while (nextNonComment < tokens_.size() && tokens_[nextNonComment].type == TokenType::LINE_COMMENT) {
        nextNonComment++;
    }
    
    if (nextNonComment >= tokens_.size() || tokens_[nextNonComment].type != TokenType::LPAREN) {
        LUC_LOG_PARSER("ERROR: No '(' found for function '" << nameRaw << "'");
        errorAt(DiagCode::E2001, "expected '(' to start parameter list for function '" + nameRaw + "'");
        return nullptr;
    }
    
    // Skip comments before '('
    while (check(TokenType::LINE_COMMENT)) advance();
    
    // Parse parameter groups (one or more for curried functions)
    while (check(TokenType::LPAREN)) {
        LUC_LOG_PARSER("\tParsing parameter group at pos " << pos_);
        node->sig.paramGroups.push_back(parseParamGroup());
        LUC_LOG_PARSER("\t\tParsed " << node->sig.paramGroups.back().size() << " parameters");
    }

    LUC_LOG_PARSER("Total parameter groups: " << node->sig.paramGroups.size());

    // ── Parse return list after '->' ───────────────────────────────────────────
    if (match(TokenType::ARROW)) {
        node->sig.returnTypes = parseReturnList();
        LUC_LOG_PARSER("\tParsed " << node->sig.returnTypes.size() << " return type(s)");
    } else {
        LUC_LOG_PARSER("\tNo return types (void function)");
    }

    // ── Optional body ('=' ...) ────────────────────────────────────────────────
    // The semantic phase enforces the invariant:
    //   - @extern → must have NO body.
    //   - non-@extern → must have a body.
    // Here the parser simply accepts or omits the body without inspecting attrs.
    if (!check(TokenType::ASSIGN)) {
        // No body — valid for @extern declarations, semantic error otherwise.
        LUC_LOG_PARSER("parseFuncDecl: no body (node->body remains nullptr)");
        // Consume optional trailing semicolon common in @extern declarations.
        match(TokenType::SEMICOLON);
        LUC_LOG_PARSER("=== parseFuncDecl END (no body) ===");
        return node;
    }

    advance(); // consume '='
    LUC_LOG_PARSER("Parsing function body...");

    // Parse the body — block, verbose anon-func form, or expression
    if (check(TokenType::LBRACE)) {
        // Block body: = { ... }
        node->body = parseBlock();
    } else if (check(TokenType::LPAREN)) {
        // Verbose anon-func form: = (params) -> ret { ... }
        // The declaration header is authoritative. The helper consumes the
        // repeated signature and validates it against the declared signature.
        validateAnonFuncBodySig(node->sig, nameRaw);
        if (!check(TokenType::LBRACE)) {
            errorAt(DiagCode::E2001, "expected '{' to start function body");
            return nullptr;
        }
        node->body = parseBlock();
    } else {
        // Expression body: = existingFunc
        SourceLocation bodyLoc = currentLoc();
        ExprPtr expr = parseExpr();
        if (!expr) {
            LUC_LOG_PARSER("\tERROR: expected expression after '='");
            errorAt(DiagCode::E2008, "expected expression after '='");
            return nullptr;
        }

        auto ret = arena_.make<ReturnStmtAST>();
        ret->loc = bodyLoc;
        ret->values.push_back(std::move(expr));

        auto block = arena_.make<BlockStmtAST>();
        block->loc = bodyLoc;
        block->stmts.push_back(std::move(ret));
        node->body = std::move(block);
    }
    
    // Optional semicolon after body (for expression bodies)
    match(TokenType::SEMICOLON);
    
    LUC_LOG_PARSER("=== parseFuncDecl END (with body) ===");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// Type Declarations
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parseStructDecl
//
// Parses a struct type declaration.
//
// Grammar:
//   struct_decl := [ vis ] 'struct' IDENTIFIER [ generic_params ]
//                  '{' { field_decl } '}'
//
// Examples:
//   struct Vec2 { x float, y float }
//   pub struct Scene<T : Drawable> { objects []T }
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'struct' keyword.
// - Consumes an IDENTIFIER (struct name).
// - Optional generic parameters via parseGenericParams().
// - Consumes '{' to open the struct body.
// - Repeatedly calls parseFieldDecl() to consume field definitions until '}'.
// - Consumes the closing '}'.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing struct name: reports error, returns nullptr.
// - Missing '{' after name/generics: reports error, returns nullptr.
// - If parseFieldDecl() returns nullptr:
//     * If no progress was made (pos_ == savedPos), forcibly consumes one token.
//     * Calls synchronize() to skip to the next field or closing brace.
// - Missing closing '}': reports error (consume tries to recover).
//
// ─── Loop Safety ─────────────────────────────────────────────────────────────
// - The field loop uses a progress guard: saves pos_ before parseFieldDecl().
// - If parseFieldDecl() fails to advance, consumes one token, then calls
//   synchronize() – guarantees forward progress.
// - The loop terminates when RBRACE is found or EOF is reached.
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<StructDeclAST> Parser::parseStructDecl(Visibility vis) {
    LUC_LOG_PARSER("parseStructDecl: parsing struct");
    SourceLocation loc = currentLoc();
    consume(TokenType::STRUCT, "expected 'struct'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected struct name");
        return nullptr;
    }
    std::string name = advance().value;
    LUC_LOG_PARSER("\tstruct name: '" << name << "'");

    auto node = arena_.make<StructDeclAST>();
    node->loc = loc;
    node->name = std::move(pool_.intern(name));
    node->visibility = vis;

    // Optional generic params
    if (check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    consume(TokenType::LBRACE, "expected '{' to open struct body");

    // Parse fields until '}'
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        match(TokenType::SEMICOLON); // optional separator
        match(TokenType::COMMA);     // optional separator
        if (check(TokenType::RBRACE))
            break;

        // Harvest trailing/stacked doc comment for the field
        std::optional<DocComment> fdoc = harvestDocComment();

        // Save position for error recovery
        std::size_t savedPos = pos_;

        FieldDeclPtr field = parseFieldDecl();
        if (field) {
            attachDoc(*field, std::move(fdoc));
            node->fields.push_back(std::move(field));
        } else {
            // If we didn't advance, manually advance to avoid infinite loop
            if (pos_ == savedPos) {
                LUC_LOG_PARSER("parseStructDecl: parser didn't advance, forcing advance");
                advance();
            }
            synchronize();
        }
    }

    consume(TokenType::RBRACE, "expected '}' to close struct body");
    LUC_LOG_PARSER("\tparsed " << node->fields.size() << " fields");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseFieldDecl
//
// Parses a single field declaration inside a struct body.
//
// Grammar:
//   field_decl := IDENTIFIER type [ '=' expr ]
//
// Examples:
//   x float
//   r float = 1.0
//   items [*]string
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes an IDENTIFIER (field name).
// - Consumes a type annotation via parseType().
// - Optionally, if '=' is present, consumes it and parses an initialiser expression.
// - Does NOT consume trailing commas or semicolons (the caller handles optional
//   separators).
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing field name: reports error, returns nullptr.
// - Missing type annotation: reports error, returns nullptr.
// - If '=' is present but no expression follows: reports error, field node still
//   created with null defaultVal.
// - Returns non‑null on success, nullptr on error (caller handles recovery).
//
// ─── Notes ──────────────────────────────────────────────────────────────────
// - Default values are stored as ExprPtr; the semantic phase validates type
//   compatibility and const‑correctness.
// - The struct literal may omit fields with default values.
// ─────────────────────────────────────────────────────────────────────────────
FieldDeclPtr Parser::parseFieldDecl() {
    SourceLocation loc = currentLoc();

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected field name");
        return nullptr;
    }
    std::string name = advance().value;

    TypePtr type = parseType();
    if (!type) {
        errorAt(DiagCode::E2005, "expected type for field '" + name + "'");
        return nullptr;
    }

    ExprPtr defaultVal;
    if (match(TokenType::ASSIGN)) {
        defaultVal = parseExpr();
        if (!defaultVal) {
            errorAt(DiagCode::E2008, "expected expression after '=' in field default value");
        }
    }

    auto field = arena_.make<FieldDeclAST>();
    field->loc = loc;
    field->name = std::move(pool_.intern(name));
    field->type = std::move(type);
    field->defaultVal = std::move(defaultVal);
    return field;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseEnumDecl
//
// Parses an enum type declaration.
//
// Grammar:
//   enum_decl := [ vis ] 'enum' IDENTIFIER '{' enum_variant { [','] enum_variant } '}'
//
// Examples:
//   enum Direction { North, South, East, West }
//   pub enum ShaderStage { Vertex = 0x01, Fragment = 0x02, Compute = 0x04 }
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'enum' keyword.
// - Consumes an IDENTIFIER (enum name).
// - Consumes '{' to open the enum body.
// - Repeatedly calls parseEnumVariant() to consume variant definitions until '}'.
// - Consumes the closing '}'.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing enum name after 'enum': reports error, returns nullptr.
// - Missing '{' after name: reports error, returns nullptr.
// - If parseEnumVariant() returns nullptr:
//     * If no progress was made (pos_ == savedPos), forcibly consumes one token.
//     * Calls synchronize() to skip to the next variant or closing brace.
// - Missing closing '}': reports error (consume tries to recover).
//
// ─── Loop Safety ─────────────────────────────────────────────────────────────
// - The variant loop uses a progress guard: saves pos_ before parseEnumVariant().
// - If parseEnumVariant() fails to advance, consumes one token, then calls
//   synchronize() – guarantees forward progress.
// - Optional commas and semicolons are matched and consumed without stalling.
// - The loop terminates when RBRACE is found or EOF is reached.
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<EnumDeclAST> Parser::parseEnumDecl(Visibility vis) {
    LUC_LOG_PARSER("parseEnumDecl: parsing enum");
    SourceLocation loc = currentLoc();
    consume(TokenType::ENUM, "expected 'enum'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected enum name");
        return nullptr;
    }
    std::string name = advance().value;
    LUC_LOG_PARSER("\tenum name: '" << name << "'");

    auto node = arena_.make<EnumDeclAST>();
    node->loc = loc;
    node->name = std::move(pool_.intern(name));
    node->visibility = vis;

    consume(TokenType::LBRACE, "expected '{' to open enum body");

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        match(TokenType::COMMA);
        match(TokenType::SEMICOLON);
        if (check(TokenType::RBRACE))
            break;

        std::optional<DocComment> vdoc = harvestDocComment();

        std::size_t savedPos = pos_;

        EnumVariantPtr variant = parseEnumVariant();
        if (variant) {
            attachDoc(*variant, std::move(vdoc));
            node->variants.push_back(std::move(variant));
        } else {
            if (pos_ == savedPos) {
                LUC_LOG_PARSER("parseEnumDecl: parser didn't advance, forcing advance");
                advance();
            }
            synchronize();
        }
    }

    consume(TokenType::RBRACE, "expected '}' to close enum body");
    LUC_LOG_PARSER("\tparsed " << node->variants.size() << " variants");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseEnumVariant
//
// Parses a single variant inside an enum declaration.
//
// Grammar:
//   enum_variant := IDENTIFIER [ '=' ( INT_LITERAL | HEX_LITERAL ) ]
//
// Examples:
//   North
//   Vertex = 0x01
//   MaxValue = 65535
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes an IDENTIFIER (variant name).
// - Optionally, if '=' is present:
//     * Consumes the '=' token.
//     * Consumes an integer literal (decimal INT_LITERAL or HEX_LITERAL).
// - Does NOT consume trailing commas or semicolons (caller handles optional separators).
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing variant name: reports error, returns nullptr.
// - If '=' is present but no integer literal follows: reports error, returns variant
//   node with no explicit value (the caller may still accept it).
// - Invalid integer literal (malformed or out of range): reports error, variant
//   node created with no explicit value (the semantic phase will handle it).
//
// ─── Value Parsing ───────────────────────────────────────────────────────────
// - Strips underscore separators (e.g., 0xFF_FF_FF_FF) before conversion.
// - Supports decimal (base 10) and hexadecimal (base 16) literals.
// - Uses std::strtoll for conversion; overflow is detected and reported.
// - Explicit values are stored as std::optional<int64_t> (nullopt if no value given).
//
// ─── Notes ──────────────────────────────────────────────────────────────────
// - The semantic phase computes final integer values for all variants, handling
//   auto‑increment and duplicate detection.
// - Explicit values reset the auto‑increment counter for subsequent variants.
// ─────────────────────────────────────────────────────────────────────────────
EnumVariantPtr Parser::parseEnumVariant() {
    SourceLocation loc = currentLoc();

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected enum variant name");
        return nullptr;
    }
    std::string nameRaw = advance().value;
    InternedString name = pool_.intern(nameRaw);

    auto variant = arena_.make<EnumVariantAST>(name);
    variant->loc = loc;

    if (match(TokenType::ASSIGN)) {
        if (check(TokenType::INT_LITERAL) || check(TokenType::HEX_LITERAL)) {
            Token valTok = advance();
            std::string raw = valTok.value;

            // Strip underscores used as visual separators (e.g. 0xFF_FF)
            raw.erase(std::remove(raw.begin(), raw.end(), '_'), raw.end());

            int base = (valTok.type == TokenType::HEX_LITERAL) ? 16 : 10;
            char *endPtr = nullptr;
            errno = 0;
            long long val = std::strtoll(raw.c_str(), &endPtr, base);

            if (endPtr != raw.c_str() && *endPtr == '\0' && errno != ERANGE) {
                variant->explicitValue = val;
            } else {
                error(locOf(valTok), DiagCode::E2009,
                      "enum variant value '" + valTok.value + "' is not a valid integer or overflows 64-bit");
            }
        } else {
            errorAt(DiagCode::E2009, "expected integer literal after '=' in enum variant");
        }
    }

    return variant;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseTraitDecl
//
// Parses a trait declaration (method contract, no implementations).
//
// Grammar:
//   trait_decl := [ vis ] 'trait' IDENTIFIER [ generic_params ]
//                 '{' { trait_method } '}'
//
// Examples:
//   trait Drawable { draw () bounds () -> Rect }
//   pub trait Comparable<T> { compareTo (other T) -> int }
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'trait' keyword.
// - Consumes an IDENTIFIER (trait name).
// - Optional generic parameters via parseGenericParams().
// - Consumes '{' to open the trait body.
// - Repeatedly calls parseTraitMethod() to consume method signatures until '}'.
// - Consumes the closing '}'.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing trait name after 'trait': reports error, returns nullptr.
// - Missing '{' after name/generics: reports error, returns nullptr.
// - If parseTraitMethod() returns nullptr:
//     * If no progress was made (pos_ == savedPos), forcibly consumes one token.
//     * Calls synchronize() to skip to the next method or closing brace.
// - Missing closing '}': reports error (consume tries to recover).
//
// ─── Loop Safety ─────────────────────────────────────────────────────────────
// - The method loop uses a progress guard: saves pos_ before parseTraitMethod().
// - If parseTraitMethod() fails to advance, consumes one token, then calls
//   synchronize() – guarantees forward progress.
// - Optional commas and semicolons are matched and consumed without stalling.
// - The loop terminates when RBRACE is found or EOF is reached.
//
// ─── Notes ──────────────────────────────────────────────────────────────────
// - Trait methods are signatures only – no '=' and no body.
// - Trait declarations are top‑level only (parseDeclaration with Local context
//   rejects TRAIT token).
// - The semantic phase verifies that impl blocks provide all required methods.
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<TraitDeclAST> Parser::parseTraitDecl(Visibility vis) {
    LUC_LOG_PARSER("parseTraitDecl: parsing trait");
    SourceLocation loc = currentLoc();
    consume(TokenType::TRAIT, "expected 'trait'");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected trait name");
        return nullptr;
    }
    std::string name = advance().value;

    auto node = arena_.make<TraitDeclAST>();
    node->loc = loc;
    node->name = std::move(pool_.intern(name));
    node->visibility = vis;

    // Optional generic params
    if (check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    consume(TokenType::LBRACE, "expected '{' to open trait body");

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        match(TokenType::SEMICOLON);
        match(TokenType::COMMA);
        if (check(TokenType::RBRACE))
            break;

        std::optional<DocComment> mdoc = harvestDocComment();

        std::size_t savedPos = pos_;

        TraitMethodPtr method = parseTraitMethod();
        if (method) {
            attachDoc(*method, std::move(mdoc));
            node->methods.push_back(std::move(method));
        } else {
            if (pos_ == savedPos) {
                LUC_LOG_PARSER("parseTraitDecl: parser didn't advance, forcing advance");
                advance();
            }
            synchronize();
        }
    }

    consume(TokenType::RBRACE, "expected '}' to close trait body");
    LUC_LOG_PARSER("\ttrait name: '" << name << "', methods: " << node->methods.size());
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseTraitMethod
//
// Parses a single method signature inside a trait declaration.
//
// Grammar:
//   trait_method := IDENTIFIER [ qualifier_list ] param_group { param_group }
//                  [ '->' return_list ]
//
//   qualifier_list := { '~' IDENTIFIER }   -- ~async, ~nullable, ~parallel
//   return_list     := '(' [ return_type { ',' return_type } ] ')'   -- multiple returns
//                   | return_type                                    -- single return
//   return_type     := type
//                    | param_group { param_group } '->' return_list  -- curried return
//
// Examples:
//   draw ()
//   bounds () -> Rect
//   fetch ~async (url string) -> string
//   clamp (min int)(max int)(value int) -> int
//   process (data []byte) -> (int, string, bool)
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes an IDENTIFIER (method name).
// - Optional qualifiers ('~' IDENTIFIER) – stored raw, resolved semantically.
// - One or more parameter groups via parseParamGroup().
// - Optional '->' followed by return list via parseReturnList().
// - Does NOT consume any tokens beyond the return list (stops at '}' or separator).
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing method name: reports error, returns nullptr.
// - Missing '(' after name/qualifiers: reports error, returns nullptr.
// - If parseParamGroup() fails, returns nullptr.
// - Return list errors are reported by parseReturnList().
//
// ─── Loop Safety ─────────────────────────────────────────────────────────────
// - Qualifier loop consumes '~' and following IDENTIFIER per iteration.
// - Parameter group loop consumes one '(' per iteration; parseParamGroup() makes
//   progress or reports error.
// - No unbounded loops; each loop is bounded by the number of tokens present.
//
// ─── Notes ──────────────────────────────────────────────────────────────────
// - Trait methods are signatures only – no '=' and no body.
// - Supports curried methods (multiple parameter groups) and multiple returns.
// - The semantic phase resolves qualifier names to bitmask via QualifierRegistry.
// ─────────────────────────────────────────────────────────────────────────────
TraitMethodPtr Parser::parseTraitMethod() {
    SourceLocation loc = currentLoc();
    
    auto method = arena_.make<TraitMethodAST>();
    method->loc = loc;

    // Parse method name
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected trait method name");
        return nullptr;
    }
    method->name = pool_.intern(advance().value);
    
    // ── Collect raw qualifier names — resolved to bitmask by semantic phase ──
    while (check(TokenType::TILDE)) {
        advance(); // consume '~'
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        std::string qualRaw = advance().value;
        method->sig.rawQualifiers.push_back(pool_.intern(qualRaw));
        LUC_LOG_PARSER_VERBOSE("\traw qualifier stored: '~" << qualRaw << "'");
    }
    // sig.qualifiers bitmask starts at 0 — filled by the semantic phase.
    
    // ── Parse parameter groups ────────────────────────────────────────────
    if (!check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' for trait method parameters");
        return nullptr;
    }
    
    while (check(TokenType::LPAREN)) {
        method->sig.paramGroups.push_back(parseParamGroup());
    }
    
    // ── Parse return types after '->' (if present) ───────────────────────
    if (match(TokenType::ARROW)) {
        method->sig.returnTypes = parseReturnList();
        LUC_LOG_PARSER_VERBOSE("\tparsed " << method->sig.returnTypes.size() << " return type(s)");
    } else {
        // void trait method (no return types)
        LUC_LOG_PARSER_VERBOSE("\tvoid trait method (no return types)");
    }
    
    LUC_LOG_PARSER_VERBOSE("parseTraitMethod: parsed method '" << pool_.lookup(method->name)
                           << "' with " << method->sig.paramGroups.size() << " param group(s)");
    return method;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseImplDecl
//
// Parses an impl block that binds methods to a type (with optional trait conformance).
//
// Grammar:
//   impl_decl := [ vis ] 'impl' type_name [ generic_params ] [ ':' trait_ref ] '{' { method_decl } '}'
//   type_name := IDENTIFIER [ generic_args ]   -- any named type (struct, enum, type alias)
//
// Examples:
//   impl Vec2 { length () -> float = { ... } }                    -- direct on struct
//   impl Direction { isNorth () -> bool = { ... } }               -- direct on enum
//   impl IntOps { isEven () -> bool = { ... } }                   -- via type alias
//   impl Callback { andThen (next Callback) -> Callback = { ... } } -- via alias for function type
//   impl Circle : Drawable { draw () = { ... } }                  -- with trait conformance
//   impl Scene<T : Drawable> { drawAll () = { ... } }             -- generic impl
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'impl' keyword.
// - Parses the target type (named type with optional generic args).
// - Optional generic parameters (definition style) via parseGenericParams().
// - Optional trait conformance ':' followed by parseTraitRef().
// - Consumes '{' to open the impl body.
// - Repeatedly calls parseMethodDecl() to consume method definitions until '}'.
// - Consumes the closing '}'.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing target type after 'impl': reports error, returns nullptr.
// - Missing '{' after type/generics/trait: reports error, returns nullptr.
// - If parseMethodDecl() returns nullptr:
//     * If no progress was made (pos_ == savedPos), forcibly consumes one token.
//     * Calls synchronize() to skip to the next method or closing brace.
// - Unrecognised token inside impl body: reports error, synchronizes.
// - Missing closing '}': reports error (consume tries to recover).
//
// ─── Loop Safety ─────────────────────────────────────────────────────────────
// - The method loop uses a progress guard: saves pos_ before parseMethodDecl().
// - If parseMethodDecl() fails to advance, consumes one token, then calls
//   synchronize() – guarantees forward progress.
// - The loop terminates when RBRACE is found or EOF is reached.
//
// ─── Notes ──────────────────────────────────────────────────────────────────
// - genericParams stores the impl's own type parameters (e.g., <T : Drawable>).
// - The target type can be any named type (struct, enum, type alias).
// - The semantic phase merges multiple impl blocks for the same type.
// - Trait conformance is checked against the target type in the semantic pass.
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<ImplDeclAST> Parser::parseImplDecl(Visibility vis) {
    LUC_LOG_PARSER("parseImplDecl: parsing impl");
    SourceLocation loc = currentLoc();
    consume(TokenType::IMPL, "expected 'impl'");

    auto node = arena_.make<ImplDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    // ── Parse target type (any named type: struct, enum, type alias) ─────────
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected target type after 'impl'");
        return nullptr;
    }

    // Optional receiver alias: 'as' IDENTIFIER
    if (match(TokenType::AS)) {
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected identifier after 'as' for receiver alias");
        } else {
            node->receiverAlias = pool_.intern(advance().value);
        }
    } else {
        // Default: empty receiverAlias means "self"
        node->receiverAlias = InternedString(); // already default-initialized
    }

    // Parse the named type (may have generic arguments like Scene<T>)
    TypePtr targetType = parseNamedType();
    if (!targetType || targetType->isa<UnknownTypeAST>()) {
        errorAt(DiagCode::E2005, "invalid target type in impl block");
        return nullptr;
    }
    node->targetType = std::move(targetType);

    LUC_LOG_PARSER("\timpl for type: " << LucDebug::kindToString(node->targetType->kind)
                   << " (details omitted for brevity)");

    // ── Optional generic parameters (definition style): impl Scene<T : Drawable> ──
    // These are the impl's own type parameters, not to be confused with the
    // target type's generic arguments.
    if (check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
        LUC_LOG_PARSER("\timpl has " << node->genericParams.size() << " generic parameter(s)");
    }

    // ── Optional trait conformance: ':' trait_ref ────────────────────────────
    if (check(TokenType::COLON)) {
        node->traitRef = parseTraitRef();
        LUC_LOG_PARSER("\timpl conforms to trait: " << (node->traitRef ? "yes" : "no"));
    }

    // ── Consume opening brace ────────────────────────────────────────────────
    consume(TokenType::LBRACE, "expected '{' to open impl body");

    // ── Parse impl members (method declarations) ─────────────────────────────
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        match(TokenType::SEMICOLON);
        match(TokenType::COMMA);
        if (check(TokenType::RBRACE))
            break;

        std::optional<DocComment> mdoc = harvestDocComment();
        std::size_t savedPos = pos_;

        // Method declaration
        if (check(TokenType::IDENTIFIER)) {
            MethodDeclPtr md = parseMethodDecl();
            if (md) {
                attachDoc(*md, std::move(mdoc));
                node->methods.push_back(std::move(md));
            } else {
                if (pos_ == savedPos) advance();
                synchronize();
            }
            continue;
        }

        // Unrecognised token inside impl block
        errorAt(DiagCode::E2002, "expected method declaration inside impl block");
        if (pos_ == savedPos) advance();
        synchronize();
    }

    consume(TokenType::RBRACE, "expected '}' to close impl body");
    LUC_LOG_PARSER("\tparsed " << node->methods.size() << " methods for impl");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseTraitRef
//
// Parses a trait reference in an impl conformance declaration.
//
// Grammar:
//   trait_ref := ':' IDENTIFIER [ generic_args ]
//
// Called when ':' is seen in an impl header. Consumes the ':' itself.
//
// Examples:
//   : Drawable
//   : Comparable<int>
//   : Hashable + Comparable (not allowed – use multiple trait bounds on generic param)
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the ':' token (already checked by caller).
// - Consumes an IDENTIFIER (trait name).
// - Optional generic arguments via parseGenericArgs() (e.g., <int>).
// - Does NOT consume any tokens beyond the generic args (stops before '{' or next token).
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing trait name after ':': reports error, returns nullptr.
// - Generic argument parsing errors are reported by parseGenericArgs().
// - Returns nullptr on error; caller should handle recovery.
//
// ─── Notes ──────────────────────────────────────────────────────────────────
// - The semantic phase resolves the trait name to a TraitDeclAST and validates
//   that the impl struct provides all required methods.
// - Generic arguments are validated against the trait's generic parameters.
// - Multiple trait bounds (e.g., : Drawable + Serializable) are not supported
//   on impl conformance; use generic parameter constraints instead.
// ─────────────────────────────────────────────────────────────────────────────
TraitRefPtr Parser::parseTraitRef() {
    SourceLocation loc = currentLoc();
    consume(TokenType::COLON, "expected ':' before trait name");

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected trait name after ':'");
        return nullptr;
    }

    auto ref = arena_.make<TraitRefAST>();
    ref->loc = loc;
    ref->name = pool_.intern(advance().value);

    // Optional generic arguments: Comparable<int>
    if (check(TokenType::LESS)) {
        ref->genericArgs = parseGenericArgs();
    }

    return ref;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseMethodDecl
//
// Parses a method declaration inside an impl block.
//
// Grammar:
//   method_decl := IDENTIFIER [ qualifier_list ] param_group { param_group }
//                 [ '->' return_list ] '=' func_body
//
//   qualifier_list := { '~' IDENTIFIER }   -- ~async, ~nullable, ~parallel
//   return_list     := return_type { ',' return_type }
//   return_type     := type | param_group { param_group } '->' return_list
//   func_body       := block | anon_func | expr
//
// Examples:
//   length () -> float = { return #sqrt(x*x + y*y) }
//   fetch ~async (url string) -> string = { return await httpGet(url) }
//   clamp (min int)(max int)(value int) -> int = { ... }
//   process (data []byte) -> (int, string) = { ... }
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes an IDENTIFIER (method name).
// - Optional qualifiers ('~' IDENTIFIER) – stored raw, resolved semantically.
// - One or more parameter groups via parseParamGroup().
// - Optional '->' followed by return list via parseReturnList().
// - Consumes '=' (required).
// - Parses the body – block, verbose anon‑func, or expression (same as func body).
// - Does NOT consume trailing semicolon (caller handles optional semicolons).
//
// ─── Body Parsing Variants ───────────────────────────────────────────────────
//   1. Block body:          = { ... }                    → parseBlock()
//   2. Verbose anon‑func:   = (params) -> ret { ... }    → validateAnonFuncBodySig() + parseBlock()
//   3. Expression body:     = expr                       → wrapped in ReturnStmt + BlockStmt
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing method name: reports error, returns nullptr.
// - Missing '(' after name/qualifiers: reports error, returns nullptr.
// - Missing '=' before body: reports error, returns nullptr.
// - Missing body after '=': reports error, returns nullptr.
// - If validateAnonFuncBodySig() fails, returns nullptr (error already reported).
//
// ─── Loop Safety ─────────────────────────────────────────────────────────────
// - Qualifier loop consumes '~' and following IDENTIFIER per iteration.
// - Parameter group loop consumes one '(' per iteration; parseParamGroup() makes
//   progress or reports error.
// - No unbounded loops; each loop is bounded by the number of tokens present.
//
// ─── Notes ──────────────────────────────────────────────────────────────────
// - No visibility prefix per method – visibility comes from enclosing impl block.
// - The ~nullable qualifier marks the method binding as nullable (caller must guard).
// - The semantic phase resolves qualifier names to bitmask via QualifierRegistry.
// ─────────────────────────────────────────────────────────────────────────────
MethodDeclPtr Parser::parseMethodDecl() {
    SourceLocation loc = currentLoc();

    auto method = arena_.make<MethodDeclAST>();
    method->loc = loc;

    // Method name
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected method name");
        return nullptr;
    }
    method->name = pool_.intern(advance().value);

    // ── Collect raw qualifier names — resolved to bitmask by semantic phase ──
    while (check(TokenType::TILDE)) {
        advance(); // consume '~'
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        std::string qualRaw = advance().value;
        method->sig.rawQualifiers.push_back(pool_.intern(qualRaw));
        LUC_LOG_PARSER_VERBOSE("\traw qualifier stored: '~" << qualRaw << "'");
    }
    // sig.qualifiers bitmask starts at 0 — filled by the semantic phase.

    // Parse one or more parameter groups (curried method support)
    if (!check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' to start parameter list for method '" +
                std::string(pool_.lookup(method->name)) + "'");
        return nullptr;
    }

    while (check(TokenType::LPAREN)) {
        method->sig.paramGroups.push_back(parseParamGroup());
    }

    // ── Parse return types after '->' (if present) ─────────────────────────
    if (match(TokenType::ARROW)) {
        method->sig.returnTypes = parseReturnList();
        LUC_LOG_PARSER_VERBOSE("\tparsed " << method->sig.returnTypes.size() << " return type(s)");
    } else {
        // Void method (no return types)
        LUC_LOG_PARSER_VERBOSE("\tvoid method (no return types)");
    }

    // Must have '=' body
    if (!check(TokenType::ASSIGN)) {
        errorAt(DiagCode::E2001, "expected '=' before method body for '" +
                std::string(pool_.lookup(method->name)) + "'");
        return nullptr;
    }
    advance(); // Consume '='

    LUC_LOG_PARSER("parseMethodDecl: parsing body for method '" << pool_.lookup(method->name) << "'");

    // Determine body type
    if (check(TokenType::LBRACE)) {
        // Block body: = { ... }
        method->body = parseBlock();
        LUC_LOG_PARSER("parseMethodDecl: block body");
    } else if (check(TokenType::LPAREN)) {
        // Verbose anon-func form: = (params) -> ret { ... }  or curried form
        // The declaration header is authoritative. The helper consumes the
        // repeated signature and validates it against the declared signature.
        std::string methodName = std::string(pool_.lookup(method->name));
        validateAnonFuncBodySig(method->sig, methodName);
        if (!check(TokenType::LBRACE)) {
            errorAt(DiagCode::E2001, "expected '{' to start method body");
            return nullptr;
        }
        method->body = parseBlock();
        LUC_LOG_PARSER("parseMethodDecl: verbose anon-func body");
    } else {
        // Expression body: = existingFunc
        SourceLocation bodyLoc = currentLoc();
        ExprPtr expr = parseExpr();
        if (!expr) {
            errorAt(DiagCode::E2008, "expected expression after '=' for method '" +
                    std::string(pool_.lookup(method->name)) + "'");
            return nullptr;
        }

        auto ret = arena_.make<ReturnStmtAST>();
        ret->loc = bodyLoc;
        ret->values.push_back(std::move(expr));

        auto block = arena_.make<BlockStmtAST>();
        block->loc = bodyLoc;
        block->stmts.push_back(std::move(ret));
        method->body = std::move(block);
        LUC_LOG_PARSER("parseMethodDecl: expression body");
    }

    // Optional semicolon after body (for expression bodies in certain contexts)
    if (match(TokenType::SEMICOLON)) {
        LUC_LOG_PARSER("parseMethodDecl: optional semicolon consumed");
    }

    LUC_LOG_PARSER("parseMethodDecl: success for method '" << std::string(pool_.lookup(method->name)) << "'");
    return method;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseFromDecl
//
// Parses a 'from' declaration that defines implicit conversions to a target type.
//
// Grammar:
//   from_block  := [ vis ] 'from' type_name [ generic_params ] '{' from_entry* '}'
//   type_name   := IDENTIFIER [ generic_args ]   -- any named type (struct, enum, alias)
//   from_entry  := param_group { param_group } '->' type '=' func_body
//
// Examples:
//   export from Fahrenheit {                    -- direct on struct
//       (c Celsius) -> Fahrenheit = { ... }
//   }
//   from int {                                  -- via type alias
//       (s string) -> int = { return #parseInt(s) }
//   }
//   from Direction {                            -- direct on enum
//       (s string) -> Direction = { ... }
//   }
//   from Wrapper<T> {                           -- generic from block
//       (val T) -> Wrapper<T> = { ... }
//   }
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'from' keyword.
// - Parses a named type (the target type) with optional generic parameters.
// - Consumes '{' to open the from block.
// - Repeatedly parses from_entry blocks until '}'.
// - Consumes the closing '}'.
//
// ─── From Entry Parsing ──────────────────────────────────────────────────────
// - Each entry begins with one or more parameter groups (curried conversion sources).
// - Consumes '->' (mandatory).
// - Consumes a return type (must match the target type semantically).
// - Consumes '=' followed by the conversion body (block or expression).
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing target type name: reports error, returns nullptr.
// - Missing '{' after name/generics: reports error, returns nullptr.
// - If entry has no '(' (parameter group start): reports error, synchronizes.
// - Missing '->' before return type: reports error, synchronizes.
// - Missing return type: reports error, synchronizes.
// - Missing '=' before body: reports error, synchronizes.
// - Missing closing '}': reports error (consume tries to recover).
//
// ─── Loop Safety ─────────────────────────────────────────────────────────────
// - The entry loop uses a progress guard: saves pos_ before parsing each entry.
// - If entry parsing makes no progress, forcibly consumes one token and synchronizes.
// - The loop terminates when RBRACE is found or EOF is reached.
//
// ─── Notes ──────────────────────────────────────────────────────────────────
// - From blocks can be top‑level or local. When local, visibility modifiers are omitted.
// - The target type must be a named type (struct, enum, or type alias).
// - The semantic phase registers conversions for implicit casting contexts.
// - Multiple from blocks for the same target type are allowed in different scopes.
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<FromDeclAST> Parser::parseFromDecl(Visibility vis) {
    LUC_LOG_PARSER("parseFromDecl: parsing from block");
    SourceLocation loc = currentLoc();
    consume(TokenType::FROM, "expected 'from'");

    auto node = arena_.make<FromDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    // ── Parse the target type (must be a named type) ─────────────────────────
    // Grammar: type_name := IDENTIFIER [ generic_args ]
    // The target can be any named type (struct, enum, or type alias).
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected target type name after 'from'");
        return nullptr;
    }

    // Parse the named type (may have generic arguments like Wrapper<T>)
    TypePtr targetType = parseNamedType();
    if (!targetType || targetType->isa<UnknownTypeAST>()) {
        errorAt(DiagCode::E2005, "invalid target type in from block");
        return nullptr;
    }
    node->targetType = std::move(targetType);

    // ── Extract generic parameters from the target type (if any) ─────────────
    // For a generic from block like "from Wrapper<T> { ... }",
    // the generic parameter T should be stored in node->genericParams.
    if (node->targetType->isa<NamedTypeAST>()) {
        auto* named = node->targetType->as<NamedTypeAST>();
        // The generic arguments on the target type are the placeholders (T, U, etc.)
        // We need to convert them to GenericParamAST nodes.
        for (auto& arg : named->genericArgs) {
            if (arg && arg->isa<NamedTypeAST>()) {
                auto* argNamed = arg->as<NamedTypeAST>();
                if (argNamed->isGenericParam || argNamed->genericArgs.empty()) {
                    // This is a generic parameter placeholder like T
                    auto gp = arena_.make<GenericParamAST>(argNamed->name);
                    gp->loc = argNamed->loc;
                    node->genericParams.push_back(std::move(gp));
                }
            }
        }
    }

    LUC_LOG_PARSER("\tfrom target: " << node->targetType.get());

    // ── Consume opening brace ────────────────────────────────────────────────
    consume(TokenType::LBRACE, "expected '{' to open from block");

    // ── Parse from entries until '}' ─────────────────────────────────────────
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        match(TokenType::SEMICOLON);
        match(TokenType::COMMA);
        if (check(TokenType::RBRACE))
            break;

        SourceLocation entryLoc = currentLoc();
        std::size_t entrySavedPos = pos_;

        // Parse one or more parameter groups
        if (!check(TokenType::LPAREN)) {
            errorAt(DiagCode::E2001, "expected '(' to start parameter list for conversion entry");
            if (pos_ == entrySavedPos) advance();
            synchronize();
            if (looksLikeDeclStart() || check(TokenType::RBRACE))
                break;
            continue;
        }

        auto entry = arena_.make<FromEntryAST>();
        entry->loc = entryLoc;

        // Parse all parameter groups (curried conversion sources)
        while (check(TokenType::LPAREN)) {
            std::size_t groupSavedPos = pos_;
            entry->sig.paramGroups.push_back(parseParamGroup());
            if (pos_ == groupSavedPos) break; // emergency break
        }

        // Consume '->' (required)
        if (!check(TokenType::ARROW)) {
            errorAt(DiagCode::E2001, "expected '->' before return type for conversion entry");
            synchronize();
            if (looksLikeDeclStart() || check(TokenType::RBRACE))
                break;
            continue;
        }
        advance(); // consume '->'

        // Parse return type (must match the target type semantically)
        TypePtr returnType = parseType();
        if (!returnType) {
            errorAt(DiagCode::E2005, "expected return type after '->'");
            synchronize();
            if (looksLikeDeclStart() || check(TokenType::RBRACE))
                break;
            continue;
        }
        entry->returnType = std::move(returnType);

        // Consume '=' before body
        if (!check(TokenType::ASSIGN)) {
            errorAt(DiagCode::E2001, "expected '=' before body for conversion entry");
            synchronize();
            if (looksLikeDeclStart() || check(TokenType::RBRACE))
                break;
            continue;
        }
        advance();

        // Parse the body (block or expression)
        SourceLocation bodyLoc = currentLoc();
        if (check(TokenType::LBRACE)) {
            entry->body = parseBlock();
        } else {
            // Expression body: = expr
            ExprPtr expr = parseExpr();
            if (expr) {
                auto ret = arena_.make<ReturnStmtAST>();
                ret->loc = bodyLoc;
                ret->values.push_back(std::move(expr));

                auto block = arena_.make<BlockStmtAST>();
                block->loc = bodyLoc;
                block->stmts.push_back(std::move(ret));
                entry->body = std::move(block);
            } else {
                errorAt(DiagCode::E2008, "expected expression after '=' in conversion entry");
            }
        }

        node->entries.push_back(std::move(entry));
    }

    consume(TokenType::RBRACE, "expected '}' to close from block");
    LUC_LOG_PARSER("\tparsed " << node->entries.size() << " conversion entries");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseExtensionDecl
//
// Parses an extension declaration that adds static methods to an existing type.
//
// Grammar:
//   extension_decl := [ vis ] 'extension' type_name IDENTIFIER '{' { func_decl } '}'
//   type_name      := IDENTIFIER [ generic_args ]   -- only named types (structs, enums, aliases)
//
// Examples:
//   extension Vec2 math { ... }                     -- direct on struct
//   extension Direction helpers { ... }             -- direct on enum
//   extension IntOps std { ... }                    -- via type alias (for primitives)
//   extension Wrapper<T> container { ... }          -- generic extension
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'extension' keyword.
// - Parses a named type (the type being extended): IDENTIFIER [ generic_args ]
// - Consumes an IDENTIFIER (the namespace name).
// - Consumes '{' to open the extension block.
// - Repeatedly calls parseFuncDecl() to parse static methods until '}'.
// - Consumes the closing '}'.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing type name after 'extension': reports error, returns nullptr.
// - Missing namespace identifier: reports error, returns nullptr.
// - Missing '{' after namespace: reports error, returns nullptr.
// - If parseFuncDecl() returns nullptr:
//     * If no progress was made (pos_ == savedPos), forcibly consumes one token.
//     * Calls synchronize() to skip to the next method or closing brace.
// - Missing closing '}': reports error (consume tries to recover).
//
// ─── Loop Safety ─────────────────────────────────────────────────────────────
// - The method loop uses a progress guard: saves pos_ before parseFuncDecl().
// - If parseFuncDecl() fails to advance, consumes one token, then calls
//   synchronize() – guarantees forward progress.
// - The loop terminates when RBRACE is found or EOF is reached.
//
// ─── Notes ──────────────────────────────────────────────────────────────────
// - Extension blocks can be top‑level or local. When local, visibility modifiers are omitted.
// - Methods are static – they do not receive a self parameter.
// - The target type must be a named type (struct, enum, or type alias).
// - Generic parameters (e.g., Wrapper<T>) are stored in genericParams.
// - The semantic phase registers the methods under mangled names (Type::namespace.method).
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<ExtensionDeclAST> Parser::parseExtensionDecl(Visibility vis) {
    LUC_LOG_PARSER("parseExtensionDecl: parsing extension");
    SourceLocation loc = currentLoc();
    consume(TokenType::EXTENSION, "expected 'extension'");

    auto node = arena_.make<ExtensionDeclAST>();
    node->loc = loc;
    node->visibility = vis;

    // ── Parse the type being extended (must be a named type) ─────────────────
    // Grammar: type_name := IDENTIFIER [ generic_args ]
    // Only named types (structs, enums, type aliases) can be extended.
    // Primitives require a type alias: type IntOps = int; extension IntOps std { ... }
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2005, "expected type name to extend (e.g., 'Vec2', 'Direction', or 'IntOps')");
        return nullptr;
    }

    // Parse the named type (may have generic arguments like Wrapper<T>)
    TypePtr targetType = parseNamedType();
    if (!targetType || targetType->isa<UnknownTypeAST>()) {
        errorAt(DiagCode::E2005, "invalid type to extend");
        return nullptr;
    }
    node->targetType = std::move(targetType);

    // ── Extract generic parameters from the target type (if any) ─────────────
    // For a generic extension like "extension Wrapper<T> container { ... }",
    // the generic parameter T should be stored in node->genericParams.
    if (node->targetType->isa<NamedTypeAST>()) {
        auto* named = node->targetType->as<NamedTypeAST>();
        // The generic arguments on the target type are the placeholders (T, U, etc.)
        // We need to convert them to GenericParamAST nodes.
        for (auto& arg : named->genericArgs) {
            if (arg && arg->isa<NamedTypeAST>()) {
                auto* argNamed = arg->as<NamedTypeAST>();
                if (argNamed->isGenericParam || argNamed->genericArgs.empty()) {
                    // This is a generic parameter placeholder like T
                    auto gp = arena_.make<GenericParamAST>(argNamed->name);
                    gp->loc = argNamed->loc;
                    node->genericParams.push_back(std::move(gp));
                }
            }
        }
    }

    // ── Parse the namespace identifier (required) ────────────────────────────
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected namespace name for extension");
        return nullptr;
    }
    node->namespaceName = pool_.intern(advance().value);
    LUC_LOG_PARSER("\textension for type: " // << node->targetType.get()
                   << ", namespace: '" << pool_.lookup(node->namespaceName) << "'");

    // ── Consume opening brace ────────────────────────────────────────────────
    consume(TokenType::LBRACE, "expected '{' to open extension body");

    // ── Parse methods (static functions) until '}' ───────────────────────────
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        match(TokenType::SEMICOLON);
        match(TokenType::COMMA);
        if (check(TokenType::RBRACE))
            break;

        // Harvest doc comment for the method
        std::optional<DocComment> mdoc = harvestDocComment();
        std::size_t savedPos = pos_;

        // Extension methods are plain function declarations (static, no self parameter)
        // They use 'let' or 'const' keyword
        if (!checkAny({TokenType::LET, TokenType::CONST})) {
            errorAt(DiagCode::E2003, "expected 'let' or 'const' for extension method");
            if (pos_ == savedPos) advance();
            synchronize();
            continue;
        }

        // Determine the keyword from the current token
        TokenType kwType = peek().type;
        DeclKeyword kw = (kwType == TokenType::LET) ? DeclKeyword::Let : DeclKeyword::Const;
        advance(); // consume let/const

        // Parse a function declaration
        ASTPtr<FuncDeclAST> method = parseFuncDecl(kw, vis);
        if (method) {
            attachDoc(*method, std::move(mdoc));
            node->methods.push_back(std::move(method));
        } else {
            if (pos_ == savedPos) {
                LUC_LOG_PARSER("parseExtensionDecl: parser didn't advance, forcing advance");
                advance();
            }
            synchronize();
        }
    }

    consume(TokenType::RBRACE, "expected '}' to close extension body");
    LUC_LOG_PARSER("\tparsed " << node->methods.size() << " extension methods");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseTypeAliasDecl
//
// Parses a type alias declaration ('type' keyword).
//
// Grammar:
//   type_decl := [ vis ] 'type' IDENTIFIER [ generic_params ] '=' type
//
// Examples:
//   type ID = int
//   type AsyncOp = ~async (a int) -> int
//   type Transform<T> = (v T) -> T
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the 'type' keyword.
// - Consumes an IDENTIFIER (alias name).
// - Optional generic parameters via parseGenericParams().
// - Consumes '=' (required).
// - Consumes the aliased type via parseType().
// - Does NOT consume trailing semicolon (caller handles optional semicolons).
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing alias name after 'type': reports error, returns nullptr.
// - Missing '=' after name/generics: reports error, returns nullptr.
// - Missing type on right‑hand side: reports error, returns node with null aliasedType.
// - Returns non‑null even on error (aliasedType may be null).
//
// ─── Notes ──────────────────────────────────────────────────────────────────
// - Type aliases can be top‑level or local. When local, visibility modifiers are omitted.
// - Does NOT create a new nominal type – the alias is interchangeable with its target.
// - Generic parameters allow the alias to be instantiated with concrete types.
// - The semantic phase resolves the alias by substituting the aliased type.
// ─────────────────────────────────────────────────────────────────────────────
ASTPtr<TypeAliasDeclAST> Parser::parseTypeAliasDecl(Visibility vis) {
    SourceLocation loc = currentLoc();
    consume(TokenType::TYPE, "expected 'type' before type alias, found: " + peek().value);

    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected type alias name, found: " + peek().value);
        return nullptr;
    }
    std::string name = advance().value;

    auto node = arena_.make<TypeAliasDeclAST>();
    node->loc = loc;
    node->name = std::move(pool_.intern(name));
    node->visibility = vis;

    // Optional generic params: type Transform<T> = (value T) T
    if (check(TokenType::LESS)) {
        node->genericParams = parseGenericParams();
    }

    consume(TokenType::ASSIGN, "expected '=' in type alias");

    node->aliasedType = parseType();
    if (!node->aliasedType) {
        errorAt(DiagCode::E2005, "expected type on the right-hand side of type alias");
    }

    return node;
}