# Parser Structure

The Luc parser is a recursive-descent parser with a Pratt (precedence-climbing) expression parser. All `Parser` methods are declared in `Parser.hpp`; implementations are split across the files below.

## File layout

```
src/parser/
├── Parser.hpp                          # Parser class, TokenStream, QualifierSet
├── Parser.cpp                          # TokenStream, errors, parse(), precedence helpers
├── Dispatcher.cpp                      # declaration, type, expression, and statement dispatch
│
├── common/                             # Shared param/qualifier helpers
│   ├── ParserHelpers.cpp               # parseParamList, parseArgList, parseParamGroup,
│   │                                   # parseReturnList, parseQualifiers, parseModulePath
│   └── ParserAttributes.cpp            # parseAttributes, parseAttribute, parseAttributeArgLiteral
│
├── decl/                               # Declarations
│   ├── topLevel/                       # Program preamble
│   │   ├── PackageParser.cpp           # parsePackageDecl
│   │   └── UseParser.cpp               # parseUseDecl
│   │
│   ├── VarParser.cpp                   # parseVarDecl
│   ├── FuncParser.cpp                  # parseFuncDecl
│   ├── StructParser.cpp                # parseStructDecl, parseFieldDecl
│   ├── EnumParser.cpp                  # parseEnumDecl, parseEnumVariant
│   ├── TraitParser.cpp                 # parseTraitDecl, parseTraitMethod, parseTraitRef
│   ├── ImplParser.cpp                  # parseImplDecl, parseMethodDecl, parseFuncRef
│   ├── FromParser.cpp                  # parseFromDecl (from entries parsed inline)
│   └── TypeAliasParser.cpp             # parseTypeAliasDecl
│
├── type/                               # Concrete type parsers
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
│   ├── BlockParser.cpp                 # parseBlock
│   ├── FlowControlParser.cpp           # parseIfStmt, parseReturnStmt,
│   │                                   # parseBreakStmt, parseContinueStmt
│   ├── SwitchParser.cpp                # parseSwitchStmt, parseSwitchCase
│   ├── LoopParser.cpp                  # parseForStmt, parseWhileStmt, parseDoWhileStmt
│   └── LocalDeclParser.cpp             # parseLvalue, parseMultiVarDecl, parseMultiAssignStmt
│
└── lookahead/                          # Non-consuming disambiguation
    └── ParserLookahead.cpp             # looksLikeType, looksLikeFuncDecl, looksLikeAnonFunc,
                                        # looksLikeStructLiteral, looksLikeStmtStart,
                                        # looksLikeDeclStart, looksLikeMultiAssignStart,
                                        # looksLikeBehaviorAccess, isPrimitiveTypeToken
```

## Dispatch graph

All dispatch functions below live in `Dispatcher.cpp` unless noted (declarations, types, expressions, statements).

### Program (`Parser.cpp`)

```
parse                                          [Parser.cpp]
├── harvestDocComment                          (before each top-level decl)
├── parsePackageDecl                           (mandatory; error recovery if missing)
└── loop until EOF
    └── parseTopLevelDecl                      [Dispatcher.cpp]
        └── parseDeclaration(TopLevel)
            ├── parseAttributes
            ├── parseVisibility                  (top-level only)
            └── dispatch by keyword ─────────────┐
                                                 │
```

### Declaration dispatch (`parseDeclaration`)

```
parseDeclaration(ctx)                          [Dispatcher.cpp]
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
parseType                                      [Dispatcher.cpp]
└── parseTypeWithNullable                      [Dispatcher.cpp]
    ├── parseBaseType                          [Dispatcher.cpp]
    │   ├── primitive keyword → parsePrimitiveType   [type/PrimitiveParser.cpp]
    │   ├── identifier        → parseNamedType       → parseGenericArgs?
    │   │                                              [type/NamedParser.cpp]
    │   ├── [                 → parseArrayType         [type/ArrayParser.cpp]
    │   ├── &                 → parseRefType           → parseType
    │   │                      [type/RefParser.cpp]
    │   ├── *                 → parsePtrType           → parseType
    │   │                      [type/PtrParser.cpp]
    │   └── ( or ~            → parseFuncType          → parseQualifiers, parseParamGroup,
    │                                                    parseReturnList
    │                         [type/FuncTypeParser.cpp]
    ├── ?                     → NullableTypeAST
    └── ! [type]              → ResultTypeAST
```

`parseArrayTarget` (used by `parseImplDecl`, in `type/ArrayParser.cpp`) accepts concrete and generic array forms.

### Statement dispatch (`parseStmt`)

```
parseStmt                                      [Dispatcher.cpp]
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

parseBlock                                     [stmt/BlockParser.cpp]
└── loop: parseStmt*
```

Control-flow and function bodies call `parseBlock`, which loops on `parseStmt`.

### Expression dispatch (Pratt)

```
parseExpr                                      [Dispatcher.cpp]
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
    └── infix loop (while prec > minPrec)      [infixPrec in Parser.cpp]
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
parsePrimaryExpr                               [Dispatcher.cpp]
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
parseMatchArm                                  [expr/match/MatchParser.cpp]
└── parsePattern                               [expr/match/PatternParser.cpp]
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
parseGenericArgs       → parseType*             (inline loop, not parseTypeList)
parseArgList           → parseExpr*
parseQualifiers
parseModulePath        (use declarations)
parseLvalue            (multi-assign, infix assign)   [stmt/LocalDeclParser.cpp]
harvestDocComment      (top-level only)               [Parser.cpp]
parseVisibility                                       [Parser.cpp]
infixPrec / tokenToBinaryOp / tokenToAssignOp / isAssignOp   [Parser.cpp]
synchronize / synchronizeTo                           [Parser.cpp]
```

### Lookahead (non-consuming)

Used before committing to a parse path (`lookahead/ParserLookahead.cpp`):

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
