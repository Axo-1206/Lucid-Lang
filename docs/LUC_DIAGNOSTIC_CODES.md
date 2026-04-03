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
| **E0001** | Fatal | System | File not found: '{0}' |
| **E1001** | Error | Lexical | Invalid character: '{0}' |
| **E1002** | Error | Lexical | Unterminated string literal |
| **E2001** | Error | Syntax | Expected '{0}' but found '{1}' |
| **E2002** | Error | Syntax | Unexpected token: '{0}' |
| **E2003** | Error | Syntax | Missing identifier after '{0}' |
| **E2004** | Error | Syntax | Expected 'in' after variable in for-loop |
| **E2005** | Error | Syntax | Expected type after '{0}' |
| **E2006** | Error | Syntax | '{0}' is not valid in this context |
| **E2007** | Error | Syntax | Duplicate '{0}' in '{1}' |
| **E2008** | Error | Syntax | Expected expression after '{0}' |
| **E2009** | Error | Syntax | Invalid '{0}' literal: '{1}' |
| **E2999** | Error | Syntax | Generic syntax error: {0} |
| **E3001** | Error | Semantic | Undeclared identifier: '{0}' |

