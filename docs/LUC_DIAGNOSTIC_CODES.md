# LUC Compiler Diagnostic codes

> [!IMPORTANT]
> This file is the **Source of Truth** for compiler diagnostics. It MUST be 
> synchronized with `src/diagnostics/DiagnosticCodes.hpp`. New codes should 
> be documented here before being added to the source code.

This registry tracks every error code reported by the LUC compiler. 

## Grouping Logic
- `E0000-E0999`: General / System / Driver (File not found, Invalid CLI args)
- `E1000-E1999`: Lexical (Invalid characters, unterminated strings)
- `E2000-E2999`: Syntax (Parser errors, missing keywords, unexpected tokens)
- `E3000-E3999`: Semantic (Type mismatch, undeclared variables, trait violations)
- `E4000-E4999`: Backend / Codegen

---

| Code | Severity | Category | Template Message |
|---|---|---|---|
<!-- ── 0000-0999: System / Driver ─────────────────────────────────────────── -->
| **E0001** | Fatal | System | File not found or inaccessible. |

<!-- ── 1000-1999: Lexical ─────────────────────────────────────────────────── -->
| **E1001** | Error | Lexical | Invalid character encountered in source. |
| **E1002** | Error | Lexical | String literal was not terminated before EOF. |

<!-- ── 2000-2999: Syntax ──────────────────────────────────────────────────── -->
| **E2001** | Error | Syntax | Expected a specific token but found another. |
| **E2002** | Error | Syntax | Token found in a context where it is not allowed. |
| **E2003** | Error | Syntax | Expected an IDENTIFIER (e.g., name of a struct or enum). |
| **E2004** | Error | Syntax | Expected the 'in' keyword in a for-loop. |
| **E2005** | Error | Syntax | Expected a type annotation (e.g., int, bool, or custom). |
| **E2006** | Error | Syntax | Invalid context for a statement or expression. |
| **E2007** | Error | Syntax | Duplicate clause in switch or match. |
| **E2008** | Error | Syntax | Expected an expression but found none. |
| **E2009** | Error | Syntax | Literal value is malformed (e.g., invalid hex sequence). |
| **E2010** | Error | Syntax | Unknown or unsupported '@' attribute name. |
| **E2011** | Error | Syntax | Wrong number of arguments for '@' attribute. |
| **E2012** | Error | Syntax | Unexpected keyword found in a position where an identifier or type was expected. |
| **E2999** | Error | Syntax | Generic fallback for syntax errors. |

<!-- ── 3000-3999: Semantic ────────────────────────────────────────────────── -->
| **E3001** | Error | Semantic | Identifier used before it was declared. |
| **E3002** | Error | Semantic | Type mismatch between expected and actual expression. |
| **E3003** | Error | Semantic | Mismatch between function parameters and call arguments. |
| **E3004** | Error | Semantic | Attempted to assign to an immutable value. |
| **E3005** | Error | Semantic | Symbol already declared in this scope. |
| **E3006** | Error | Semantic | Missing 'main' entry point. |
| **E3007** | Error | Semantic | Invalid signature for the 'main' function. |
| **E3008** | Error | Semantic | Implicit type conversion not allowed; suggest explicit casting. |
| **E3009** | Error | Semantic | Unknown '@' intrinsic name. |
| **E3010** | Error | Semantic | Wrong argument count or type for '@' intrinsic. |
| **E3011** | Error | Semantic | Cannot use '==' on struct type; implement Equatable<T> and use :equals() instead. |
| **E3012** | Error | Semantic | Cannot use '==' on function type; function bodies are incomparable. |
| **E3013** | Error | Semantic | Cannot use '==' on array type; use collection library comparison function. |
| **E3014** | Error | Semantic | Chained comparison not allowed; use 'and' explicitly: 0 < x and x < 10. |
| **E3015** | Error | Semantic | '@aot' and '@jit' are mutually exclusive on the same declaration. |
| **E3016** | Error | Semantic | '@aot' / '@jit' are only valid on the 'main' entry point. |

<!-- ── W3000-W3999: Semantic Warnings ────────────────────────────────────── -->
| **W3001** | Warning | Semantic | '@extern' function declared with 'let' — should be 'const'. |
| **W3002** | Warning | Warning | '@extern' function has an empty body '= {}' that will be ignored. |
