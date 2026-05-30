```
src/parser/
├── Parser.hpp                     # Main parser class declaration
├── Parser.cpp                     # Core infrastructure: TokenStream, error handling, top‑level dispatch
│
├── common/                        # Shared utilities
│   ├── ParserHelpers.cpp          # parseParamGroup, parseReturnList, parseQualifiers, parseModulePath
│   └── ParserAttributes.cpp       # parseAttributes, parseAttribute, parseAttributeArgLiteral
│
├── top/                           # Top‑level structure
│   ├── PackageParser.cpp          # parsePackageDecl
│   └── UseParser.cpp              # parseUseDecl
│
├── decl/                          # Data declarations
│   ├── VarParser.cpp              # parseVarDecl
│   ├── TypeAliasParser.cpp        # parseTypeAliasDecl
│   └── StructParser.cpp           # parseStructDecl, parseFieldDecl
│
├── function/                      # Function declarations
│   ├── FuncParser.cpp             # parseFuncDecl (entry point)
│   └── FuncSignatureParser.cpp    # parseFuncSignature (shared logic)
│
├── enum/                          # Enums
│   └── EnumParser.cpp             # parseEnumDecl, parseEnumVariant
│
├── trait/                         # Traits
│   └── TraitParser.cpp            # parseTraitDecl, parseTraitMethod, parseTraitRef
│
├── impl/                          # Impl blocks
│   └── ImplParser.cpp             # parseImplDecl, parseMethodDecl, parseFuncRef
│
├── from/                          # From blocks
│   └── FromParser.cpp             # parseFromDecl, parseFromEntry
│
├── type/                          # Type annotations
│   ├── TypeParser.cpp             # parseType, parseTypeWithNullable, parseBaseType
│   ├── PrimitiveParser.cpp        # parsePrimitiveType
│   ├── NamedParser.cpp            # parseNamedType
│   ├── ArrayParser.cpp            # parseArrayType + parseArrayTarget
│   ├── RefParser.cpp              # parseRefType
│   ├── PtrParser.cpp              # parsePtrType
│   └── FuncTypeParser.cpp         # parseFuncType (function type annotation)
│
├── generic/                       # Generics
│   ├── GenericParamParser.cpp     # parseGenericParams, parseGenericParam
│   └── GenericArgParser.cpp       # parseGenericArgs
│
├── literal/                       # Literal expressions
│   └── LiteralParser.cpp          # parseLiteralExpr, parseArrayLiteralExpr, parseStructLiteralExpr, parseAnonFuncExpr
│
├── operator/                      # Operators and pipelines
│   ├── UnaryParser.cpp            # parseUnaryExpr
│   ├── BinaryParser.cpp           # parseInfixBinary, parseInfixAssign, parseInfixIs, parseInfixNullCoalesce
│   ├── PipelineParser.cpp         # parsePipelineExpr, parsePipelineStep
│   └── ComposeParser.cpp          # parseComposeExpr, parseComposeOperand
│
├── special/                       # Special expressions
│   ├── AwaitParser.cpp            # parseAwaitExpr
│   ├── IfExprParser.cpp           # parseIfExpr
│   ├── RangeParser.cpp            # parseRangeExpr
│   └── CastParser.cpp             # parseTypeConvExpr
│
├── call/                          # Calls and indexing
│   ├── CallParser.cpp             # parseCallExpr, parseArgList
│   └── IndexParser.cpp            # parseIndexExpr
│
├── match/                         # Match expressions
│   ├── MatchParser.cpp            # parseMatchExpr, parseMatchArm, parseDefaultArm
│   └── PatternParser.cpp          # parsePattern, parseBindPattern, parseWildcardPattern, parseTypePattern, parseStructPattern, parseFieldPattern
│
├── resolve/                       # Resolve expressions
│   └── ResolveParser.cpp          # parseResolveExpr, parseOkArm, parseErrArm
│
├── intrinsic/                     # Intrinsic calls
│   └── IntrinsicParser.cpp        # parseIntrinsicCallExpr
│
├── stmt/                          # Statements
│   ├── StmtParser.cpp             # parseStmt (entry), parseExprStmt
│   ├── BlockParser.cpp            # parseBlock
│   ├── FlowControlParser.cpp      # parseIfStmt, parseReturnStmt, parseBreakStmt, parseContinueStmt
│   ├── SwitchParser.cpp           # parseSwitchStmt, parseSwitchCase
│   ├── LoopParser.cpp             # parseForStmt, parseWhileStmt, parseDoWhileStmt
│   └── LocalDeclParser.cpp        # parseMultiVarDecl, parseMultiAssignStmt
│
└── lookahead/                     # Non‑consuming lookahead helpers
    └── Lookahead.cpp              # all looksLike* functions
```