# Parser Structure

The Luc parser is a recursive-descent parser with a Pratt (precedence-climbing) expression parser. All `Parser` methods are declared in `Parser.hpp`; implementations are split across the files below.

## File layout

```
src/parser/
├── Parser.hpp                          # Parser class, TokenStream, QualifierSet
├── Parser.cpp                          # TokenStream, errors, parse(), declaration dispatch, Pratt core
│
├── common/                             # Shared list/param/qualifier helpers
│   ├── ParserHelpers.cpp               # parseParamList, parseArgList, parseParamGroup, parseReturnList,
│   │                                   # parseQualifiers, parseModulePath
│   └── ParserAttributes.cpp            # parseAttributes, parseAttribute, parseAttributeArgLiteral
│
├── top/                                # Program preamble
│   ├── PackageParser.cpp               # parsePackageDecl
│   └── UseParser.cpp                   # parseUseDecl
│
├── decl/                               # Declarations
│   ├── VarParser.cpp                   # parseVarDecl
│   ├── FuncParser.cpp                  # parseFuncDecl
│   ├── StructParser.cpp                # parseStructDecl, parseFieldDecl
│   ├── EnumParser.cpp                  # parseEnumDecl, parseEnumVariant
│   ├── TraitParser.cpp                 # parseTraitDecl, parseTraitMethod, parseTraitRef
│   ├── ImplParser.cpp                  # parseImplDecl, parseMethodDecl, parseFuncRef
│   ├── FromParser.cpp                  # parseFromDecl (from entries parsed inline)
│   └── TypeAliasParser.cpp             # parseTypeAliasDecl
│
├── type/                               # Type annotations
│   ├── TypeParser.cpp                  # parseType, parseTypeWithNullable, parseBaseType
│   ├── PrimitiveParser.cpp             # parsePrimitiveType
│   ├── NamedParser.cpp                 # parseNamedType
│   ├── ArrayParser.cpp                 # parseArrayType, parseArrayTarget
│   ├── RefParser.cpp                   # parseRefType
│   ├── PtrParser.cpp                   # parsePtrType
│   └── FuncTypeParser.cpp              # parseFuncType
│
├── generic/                            # Generics
│   ├── GenericParamParser.cpp          # parseGenericParams, parseGenericParam
│   └── GenericArgParser.cpp            # parseGenericArgs
│
├── expr/                               # Expressions
│   ├── literal/
│   │   └── LiteralParser.cpp           # parseLiteralExpr, parseArrayLiteralExpr,
│   │                                   # parseStructLiteralExpr, parseAnonFuncExpr
│   ├── operator/
│   │   ├── BinaryParser.cpp            # parseInfixAssign, parseInfixIs,
│   │                                   # parseInfixNullCoalesce, parseInfixBinary
│   │   ├── PipelineParser.cpp          # parsePipelineExpr, parsePipelineStep,
│   │                                   # parseAnonFuncPipelineStep, parseBehaviorPipelineStep,
│   │                                   # parseFieldPipelineStep, parseIndexPipelineStep,
│   │                                   # parseArgPackPipelineStep, parseComposeOperand
│   │   └── ComposeParser.cpp           # parseComposeExpr
│   ├── special/
│   │   ├── AwaitParser.cpp             # parseAwaitExpr
│   │   ├── IfExprParser.cpp            # parseIfExpr
│   │   ├── IntrinsicParser.cpp         # parseIntrinsicCallExpr
│   │   ├── ResolveParser.cpp           # parseResolveExpr, parseOkArm, parseErrArm
│   │   ├── RangeParser.cpp             # parseRangeExpr
│   │   └── TypeConvParser.cpp          # parseTypeConvExpr
│   ├── other/
│   │   ├── CallParser.cpp              # parseCallExpr
│   │   └── IndexParser.cpp             # parseIndexExpr
│   └── match/
│       ├── MatchParser.cpp             # parseMatchExpr, parseMatchArm, parseDefaultArm
│       └── PatternParser.cpp           # parsePattern, parseLiteralOrRangePattern,
│                                       # parseBindPattern, parseTypePattern,
│                                       # parseWildcardPattern, parseStructPattern,
│                                       # parseFieldPattern
│
├── stmt/                               # Statements
│   ├── StmtParser.cpp                  # parseStmt (entry)
│   ├── BlockParser.cpp                 # parseBlock
│   ├── FlowControlParser.cpp           # parseIfStmt, parseReturnStmt,
│   │                                   # parseBreakStmt, parseContinueStmt
│   ├── SwitchParser.cpp                # parseSwitchStmt, parseSwitchCase
│   ├── LoopParser.cpp                  # parseForStmt, parseWhileStmt, parseDoWhileStmt
│   └── LocalDeclParser.cpp             # parseMultiVarDecl, parseMultiAssignStmt
│
└── lookahead/                          # Non-consuming disambiguation
    └── ParserLookahead.cpp             # looksLikeType, looksLikeFuncDecl, looksLikeAnonFunc,
                                        # looksLikeStructLiteral, looksLikeStmtStart,
                                        # looksLikeDeclStart, looksLikeMultiAssignStart,
                                        # looksLikeBehaviorAccess, isPrimitiveTypeToken
```

## Dispatch graph

### Program

```
parse
├── harvestDocComment                          (before each top-level decl)
├── parsePackageDecl                           (mandatory; error recovery if missing)
└── loop until EOF
    └── parseTopLevelDecl
        └── parseDeclaration(TopLevel)
            ├── parseAttributes
            ├── parseVisibility                  (top-level only)
            └── dispatch by keyword ─────────────┐
                                                 │
```

### Declaration dispatch (`parseDeclaration`)

```
parseDeclaration(ctx)
├── use      → parseUseDecl
├── struct   → parseStructDecl        → parseGenericParams?, parseFieldDecl*
├── enum     → parseEnumDecl          → parseEnumVariant*
├── trait    → parseTraitDecl         → parseTraitMethod*
├── impl     → parseImplDecl          → parseTraitRef?, parseMethodDecl*
├── from     → parseFromDecl          → parseParamGroup*, parseBlock | parseExpr
├── type     → parseTypeAliasDecl
└── let/const
    ├── looksLikeFuncDecl → parseFuncDecl   → parseGenericParams?, parseParamGroup,
    │                                         parseReturnList?, parseBlock | parseExpr
    └── else              → parseVarDecl     → parseType?, parseExpr?
```

Local declarations reuse `parseDeclaration(Local)` from `parseStmt` (wrapped in `DeclStmtAST`). `use` and visibility modifiers are rejected in local context.

### Type dispatch

```
parseType
└── parseTypeWithNullable
    ├── parseBaseType
    │   ├── primitive keyword → parsePrimitiveType
    │   ├── identifier        → parseNamedType        → parseGenericArgs?
    │   ├── [                 → parseArrayType
    │   ├── &                 → parseRefType          → parseType
    │   ├── *                 → parsePtrType          → parseType
    │   └── ( or ~            → parseFuncType           → parseQualifiers, parseParamGroup,
    │                                                    parseReturnList
    ├── ?                     → NullableTypeAST
    └── ! [type]              → ResultTypeAST
```

`parseArrayTarget` (used by `parseImplDecl`) accepts concrete and generic array forms.

### Statement dispatch

```
parseStmt
├── looksLikeMultiAssignStart → parseMultiAssignStmt     → parseLvalue*, parseExpr
├── let/const + comma pattern → parseMultiVarDecl        → parseVarDecl*
├── decl start                → parseDeclaration(Local)  → DeclStmtAST
├── if                        → parseIfStmt              → parseExpr, parseBlock ×2
├── switch                    → parseSwitchStmt          → parseExpr, parseSwitchCase*
│                                                         → parseRangeExpr (case ranges)
├── for                       → parseForStmt             → parseExpr?, parseBlock
├── while                     → parseWhileStmt           → parseExpr, parseBlock
├── do                        → parseDoWhileStmt         → parseBlock, parseExpr
├── return                    → parseReturnStmt          → parseExpr*
├── break                     → parseBreakStmt
├── continue                  → parseContinueStmt
└── else                      → parseExpr                → ExprStmtAST

parseBlock
└── loop: parseStmt*
```

Control-flow and function bodies call `parseBlock`, which loops on `parseStmt`.

### Expression dispatch (Pratt)

```
parseExpr
└── parsePrattExpr(minPrec)
    ├── parsePrefixExpr
    │   ├── unary (-, not, ~, &) → recurse parsePrefixExpr
    │   └── else                 → parsePrimaryExpr
    ├── parsePostfixExpr
    │   ├── (                   → parseCallExpr              → parseArgList
    │   ├── < ... > (           → parseGenericArgs, parseCallExpr
    │   ├── [                   → parseIndexExpr             → parseExpr (slice uses ..)
    │   ├── .                   → FieldAccessExprAST
    │   └── ?.                  → NullableChainExprAST
    └── infix loop (while prec > minPrec)
        ├── assign ops          → parseInfixAssign           → parseLvalue, parsePrattExpr
        ├── is                  → parseInfixIs               → parseType
        ├── |>                  → parsePipelineExpr          → parsePipelineStep*
        │                           ├── looksLikeAnonFunc    → parseAnonFuncPipelineStep
        │                           ├── :                    → parseBehaviorPipelineStep
        │                           ├── .                    → parseFieldPipelineStep
        │                           ├── [                    → parseIndexPipelineStep
        │                           └── (                    → parseArgPackPipelineStep
        ├── +>                  → parseComposeExpr           → parseComposeOperand
        ├── ??                  → parseInfixNullCoalesce
        └── other binary        → parseInfixBinary           → parsePrattExpr, parsePostfixExpr
```

### Primary expression dispatch (`parsePrimaryExpr`)

```
parsePrimaryExpr
├── match          → parseMatchExpr       → parseExpr, parseMatchArm*, parseDefaultArm
├── if             → parseIfExpr          → parseExpr, parseBlock | parseExpr
├── resolve        → parseResolveExpr     → parseOkArm, parseErrArm
├── #              → parseIntrinsicCallExpr
├── await          → parseAwaitExpr       → parsePrefixExpr
├── [              → parseArrayLiteralExpr
├── looksLikeAnonFunc → parseAnonFuncExpr → parseParamGroup, parseReturnList?, parseBlock | parseExpr
├── (              → parsePrattExpr       (grouped)
├── * type (       → parseTypeConvExpr    (unsafe cast)
├── identifier
│   ├── struct literal  → parseStructLiteralExpr
│   ├── behavior access → BehaviorAccessExprAST (Type:method)
│   └── plain           → IdentifierExprAST
├── primitive (    → parseTypeConvExpr    (safe cast)
└── else           → parseLiteralExpr
```

### Match arm / pattern dispatch

```
parseMatchArm
└── parsePattern
    ├── _                → parseWildcardPattern
    ├── literal tokens   → parseLiteralOrRangePattern
    └── identifier
        ├── is type      → parseTypePattern
        ├── {            → parseStructPattern     → parseFieldPattern*
        ├── .            → PatternExprAST (qualified constant via parseExpr)
        └── else         → parseBindPattern

parseDefaultArm
└── parseExpr
```

### Shared helpers (called from many paths)

```
parseAttributes        → parseAttribute*        → parseAttributeArgLiteral*
parseParamGroup        → parseParamList         → parseType
parseReturnList        → parseType*
parseGenericParams     → parseGenericParam*
parseGenericArgs       → parseTypeList
parseExprList          → parseExpr*
parseTypeList          → parseType*
parseStmtList          → parseStmt*
parseArgList           → parseExpr*
parseQualifiers
parseModulePath        (use declarations)
parseLvalue            (multi-assign, infix assign)
harvestDocComment      (top-level only)
parseVisibility
synchronize / synchronizeTo
```

### Lookahead (non-consuming)

Used before committing to a parse path:

| Function                    | Disambiguates                             |
| --------------------------- | ----------------------------------------- |
| `looksLikeType`             | type position vs expression               |
| `looksLikeFuncDecl`         | `let`/`const` function vs variable        |
| `looksLikeAnonFunc`         | grouped expr vs anon func / pipeline step |
| `looksLikeStructLiteral`    | `Ident {` struct literal                  |
| `looksLikeBehaviorAccess`   | `Ident : method`                          |
| `looksLikeMultiAssignStart` | `a, b = …` reassignment                   |
| `looksLikeStmtStart`        | valid expression-statement start          |
| `looksLikeDeclStart`        | declaration vs other statement            |
| `isPrimitiveTypeToken`      | primitive type keyword check              |
