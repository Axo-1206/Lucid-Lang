Here's the updated `ParserStructure.md` with the simplified `parseParamList` changes:

## Updated Helper Functions Section


### Helper Functions (`Helpers.cpp`)

```
parseAttributes(stream, ctx)
│
├── if '@' found
│   │
│   ├── consume '@' '[' 
│   ├── ScopedContext(Attribute)
│   ├── while not ']'
│   │   │
│   │   ├── handleCommaGap(stream, ctx, "attribute", isFirst)
│   │   │
│   │   └── parseAttribute(stream, ctx)
│   │       │
│   │       ├── parse name: IDENTIFIER
│   │       ├── parse optional args: '(' parseAttributeArgLiteral* ')'
│   │       └── create AttributeAST
│   │
│   ├── consume ']'
│   └── return ArenaSpan<AttributePtr>
│
└── else return empty span

parseGenericParamDecls(stream, ctx)
│
├── consume '<'
├── ScopedContext(GenericParams)
├── while not '>'
│   │
│   ├── handleCommaGap(stream, ctx, "generic parameter", isFirst)
│   │
│   └── parseGenericParamDecl(stream, ctx)
│       │
│       ├── parse name: IDENTIFIER
│       ├── parse optional constraints: ':' NamedTypeAST* (T : Trait)
│       └── create GenericParamDeclAST
│
├── consume '>'
└── return ArenaSpan<GenericParamDeclAST*>

parseGenericArgs(stream, ctx)
│
├── consume '<'
├── ScopedContext(GenericArgs)
├── while not '>'
│   │
│   ├── handleCommaGap(stream, ctx, "generic argument", isFirst)
│   ├── parseType(stream, ctx)  [ParseType.cpp]
│   └── collect type
├── consume '>'
└── return ArenaSpan<TypeAST*>

parseParamList(stream, ctx)  ◄── WITH names (function declarations)
│
├── consume '('
├── while not ')'
│   │
│   ├── handleCommaGap(stream, ctx, "parameter", isFirst)
│   ├── parse 'const' modifier (optional)
│   ├── parse name: IDENTIFIER  ◄── REQUIRED
│   ├── parse '...' (variadic, optional)
│   ├── parseType(stream, ctx)  [ParseType.cpp]
│   ├── if variadic: wrap type in ArrayTypeAST(Dynamic)
│   ├── create ParamAST (with name, type, variadic, const flags)
│   └── collect parameter
├── consume ')'
└── return std::vector<ParamAST*>

parseParamTypeList(stream, ctx)  ◄── WITHOUT names (function types)
│
├── consume '('
├── while not ')'
│   │
│   ├── handleCommaGap(stream, ctx, "parameter type", isFirst)
│   ├── parseType(stream, ctx)  [ParseType.cpp]  ◄── NO NAME ALLOWED
│   └── collect TypeAST*
├── consume ')'
└── return std::vector<TypeAST*>

parseArgList(stream, ctx)
│
├── consume '('
├── while not ')'
│   │
│   ├── handleCommaGap(stream, ctx, "argument", isFirst)
│   ├── parseExpr(stream, ctx)  [ParseExpr.cpp]
│   └── collect expression
├── consume ')'
└── return ArenaSpan<ExprAST*>

parseImportPath(stream, ctx)
│
├── while IDENTIFIER
│   │
│   ├── consume identifier
│   ├── if '.' found: consume '.' and continue
│   └── break
└── return std::vector<InternedString>

harvestDocComment(stream, ctx)
│
├── scan backward from current position
├── priority order:
│   ├── DOC_COMMENT (block) → highest priority
│   ├── stacked LINE_COMMENTs → second priority
│   └── trailing LINE_COMMENT → lowest priority
└── return optional<DocComment>

handleCommaGap(stream, ctx, what, isFirst)
│
├── stream.consumeTrailing(COMMA) → count commas
├── if count == 0: return 0
├── if count <= 2: report "expected <what>, got ','"
├── if count >= 3: report "unexpected trailing comma"
└── return count
```

## Updated Declaration Parsers Section


### Declaration Parsers (`ParseDecl.cpp`)

```
parseImportDecl(stream, ctx)
│
├── consume 'import'
├── parseImportPath(stream, ctx)                        [Helpers.cpp]
├── parse alias (optional 'as' IDENTIFIER)
├── ctx.resolver->resolveUsePath(usePath)
├── if not ctx.resolver->getParsedModule(filePath)     (check cache)
│   ├── std::string source = ctx.resolver->readModuleSource(filePath)
│   └── parse(filePath, source, ctx)  ◄── recursive call (imports)
│       │
│       └── parse() handles:
│           ├── cache checking (getParsedModule)
│           ├── circular import detection (isParsing)
│           │   └── if cycle → dummy ModuleAST with hasErrors = true
│           ├── ScopedParsingGuard (push/pop parsing stack)
│           ├── lexer::tokenize()
│           ├── parseInternal()
│           └── cacheModule()
│
└── create ImportDeclAST

Note: Circular import detection is handled EXCLUSIVELY by parse().
      parseImportDecl() delegates to parse() and trusts it to handle
      all file parsing concerns including cycle detection.

parseVarDecl(stream, ctx)
│
├── parse keyword: LET or CONST
├── parse name: IDENTIFIER
├── parseType(stream, ctx)                              [ParseType.cpp]
├── parse initializer: '=' parseExpr(stream, ctx)       [ParseExpr.cpp]
└── create VarDeclAST

parseFuncDecl(stream, ctx)
│
├── parse keyword: LET or CONST
├── parse name: IDENTIFIER
├── parse optional generic params: parseGenericParamDecls  [Helpers.cpp]
├── parse leading cluster (bound_cluster)  ─────────────────────────┐
│   │                                                               │
│   └── while '(' found                                             │
│       │                                                           │
│       └── parseParamList(stream, ctx)  ◄── WITH names            │
│           │                                                       │
│           ├── collect ParamAST* (with names) → paramGroups        │
│           └── collect TypeAST* (for FuncTypeAST) → paramTypes     │
│                                                                  │
├── while '->' found ──────────────────────────────────────────────┤
│   │                                                               │
│   ├── consume '->'                                                │
│   └── parseUnnamedCluster(stream, ctx)  ────────────────────────┐│
│       │                                                         ││
│       └── while '(' found                                       ││
│           │                                                     ││
│           └── parseParamTypeList(stream, ctx)  ◄── NO names    ││
│               │                                                 ││
│               └── collect TypeAST* → paramTypes                 ││
│                                                                  ││
├── parse final return type: parseType(stream, ctx)  ◄────────────┘│
├── create FuncTypeAST (params: paramTypes, returnType)            │
├── parse '=' and body                                             │
│   ├── block body: '{' parseBlock(stream, ctx) '}'                │
│   ├── expression body: parseExpr(stream, ctx)                    │
│   │   ├── if pure function ref → FuncRefStmtAST                  │
│   │   └── else → ReturnStmtAST                                   │
│   └── no '=' → foreign function (body = nullptr)                 │
└── create FuncDeclAST (name, keyword, genericParams, funcType, paramGroups, body)

parseStructDecl(stream, ctx)
│
├── consume 'struct'
├── parse name: IDENTIFIER
├── parse optional generic params: parseGenericParamDecls
├── parse trait implementations: ':' NamedTypeAST* ...
├── consume '{'
├── ScopedContext(StructBody)
├── while not '}': parseFieldDecl(stream, ctx)
├── consume '}'
└── create StructDeclAST

parseEnumDecl(stream, ctx)
│
├── consume 'enum'
├── parse name: IDENTIFIER
├── parse optional backing type: ':' PrimitiveTypeAST
├── consume '{'
├── ScopedContext(EnumBody)
├── while not '}': parseEnumVariant(stream, ctx)
├── consume '}'
└── create EnumDeclAST

parseTraitDecl(stream, ctx)
│
├── consume 'trait'
├── parse name: IDENTIFIER
├── parse optional generic params: parseGenericParamDecls
├── consume '{'
├── ScopedContext(TraitBody)
├── while not '}': parseTraitField(stream, ctx)
├── consume '}'
└── create TraitDeclAST

parseFieldDecl(stream, ctx)
│
├── parseAttributes(stream, ctx)
├── parse optional 'const' modifier
├── parse name: IDENTIFIER
├── parseType(stream, ctx)
├── parse optional default: '=' (block body or expression)
└── create FieldDeclAST

parseEnumVariant(stream, ctx)
│
├── parseAttributes(stream, ctx)
├── parse name: IDENTIFIER
├── consume '='
├── parse integer literal (INT_LITERAL, HEX_LITERAL, BINARY_LITERAL)
└── create EnumVariantAST

parseTraitField(stream, ctx)
│
├── parseAttributes(stream, ctx)
├── parse optional 'const' modifier
├── parse name: IDENTIFIER
├── parseType(stream, ctx)
└── create TraitFieldDeclAST
```


## Updated Type Dispatch Section


### Type Dispatch (`ParseType.cpp`)

```
parseType(stream, ctx)
│
└── parseBaseType(stream, ctx)
    │
    └── parseTypeWithQualifier(stream, ctx, type)
        │
        ├── '?' → NullableTypeAST
        ├── '!' → FallibleTypeAST  
        └── '?' '!' → CombinedTypeAST

parseBaseType(stream, ctx)
│
├── primitive token → parsePrimitiveType(stream, ctx)
│
├── '[' → parseArrayType(stream, ctx)
│   │
│   ├── consume '['
│   ├── parse array kind:
│   │   ├── '[*]' → ArrayKind::Dynamic
│   │   ├── '[_]' → ArrayKind::Slice
│   │   └── INT_LITERAL → ArrayKind::Fixed + size
│   ├── consume ']'
│   ├── parseType(stream, ctx)  (element type)  ◄── recursive call
│   └── create ArrayTypeAST
│
├── '&' → parseRefType(stream, ctx)
│   │
│   ├── consume '&'
│   ├── parseType(stream, ctx)  (inner type)  ◄── recursive call
│   └── create RefTypeAST
│
├── '*' → parsePtrType(stream, ctx)
│   │
│   ├── consume '*'
│   ├── parseType(stream, ctx)  (inner type)  ◄── recursive call
│   └── create PtrTypeAST
│
├── '(' → parseFuncType(stream, ctx)
│   │
│   ├── parseParamTypeList(stream, ctx)  ◄── NO names (types only)
│   │   │
│   │   ├── consume '('
│   │   ├── while not ')'
│   │   │   ├── handleCommaGap(stream, ctx, "parameter type", isFirst)
│   │   │   ├── parseType(stream, ctx)  ◄── recursive call
│   │   │   └── collect type
│   │   ├── consume ')'
│   │   └── return std::vector<TypeAST*>
│   │
│   ├── if '->' found
│   │   ├── consume '->'
│   │   ├── if '(' → parseFuncType(stream, ctx)  ◄── recursive call (curried)
│   │   └── else parseType(stream, ctx)  (return type)  ◄── recursive call
│   ├── create FuncTypeAST (params: TypeAST*, returnType: TypeAST*)
│   └── return FuncTypeAST
│
├── IDENTIFIER → parseNamedType(stream, ctx)
│   │
│   ├── if IDENTIFIER ':' IDENTIFIER → ModuleTypeAccessAST
│   └── else → NamedTypeAST + optional generic args
│
└── else → synchronizeToContext (caller handles diagnostic)
```

**Key Change:** `parseParamList` now always requires parameter names (used only for function declarations). `parseParamTypeList` handles the no-name case for function types. This separation is clean and matches the grammar exactly.


## Updated Expression Dispatch Section


### Expression Dispatch (Pratt Parser) (`ParseExpr.cpp`)

```
parseExpr(stream, ctx)
│
└── parsePrattExpr(stream, ctx, -1)

parsePrattExpr(stream, ctx, minPrec)
│
├── parsePrefixExpr(stream, ctx)  ────────────────────────────────────────┐
│                                                                         │
│   parsePrefixExpr(stream, ctx)                                          │
│   │                                                                     │
│   ├── unary operator (-, not, ~)                                        │
│   │   ├── consume operator                                              │
│   │   ├── parsePrattExpr(stream, ctx, prec+1)  (operand)  ◄── recurse   │
│   │   └── create UnaryExprAST                                           │
│   │                                                                     │
│   └── parsePrimaryExpr(stream, ctx)  ───────────────────────────────────┐
│                                                                         │
│   parsePrimaryExpr(stream, ctx)                                         │
│   │                                                                     │
│   ├── literal → parseLiteralExpr(stream, ctx)                           │
│   │   │                                                                 │
│   │   └── TokenType: INT_LITERAL, FLOAT_LITERAL, STRING_LITERAL,        │
│   │       RAW_STRING_LITERAL, CHAR_LITERAL, HEX_LITERAL,                │
│   │       BINARY_LITERAL, TRUE, FALSE, NIL, ERR                         │
│   │                                                                     │
│   ├── '_' → IdentifierExprAST("_")                                      │
│   │                                                                     │
│   ├── '#' → parseIntrinsicCallExpr(stream, ctx)                         │
│   │   │                                                                 │
│   │   ├── consume '#'                                                   │
│   │   ├── parse intrinsic name: IDENTIFIER                              │
│   │   ├── parseArgList(stream, ctx)  (arguments)                        │
│   │   └── create IntrinsicCallExprAST                                   │
│   │                                                                     │
│   ├── '[' → parseArrayLiteralExpr(stream, ctx)                          │
│   │   │                                                                 │
│   │   ├── consume '['                                                   │
│   │   ├── parse elements: parseExpr(stream, ctx)*  ◄── recursive        │
│   │   ├── consume ']'                                                   │
│   │   └── create ArrayLiteralExprAST                                    │
│   │                                                                     │
│   ├── 'if' → parseIfExpr(stream, ctx)                                   │
│   │   │                                                                 │
│   │   ├── consume 'if'                                                  │
│   │   ├── parseExpr(stream, ctx)  (condition)                           │
│   │   ├── consume '??'                                                  │
│   │   ├── parseExpr(stream, ctx)  (then branch)                         │
│   │   ├── consume 'else'                                                │
│   │   ├── parseExpr(stream, ctx)  (else branch)                         │
│   │   └── create IfExprAST                                              │
│   │                                                                     │
│   ├── '(' → parse parenthesized expression                              │
│   │   │                                                                 │
│   │   ├── consume '('                                                   │
│   │   ├── parseExpr(stream, ctx)  ◄── recursive                         │
│   │   ├── consume ')'                                                   │
│   │   └── return expr                                                   │
│   │                                                                     │
│   ├── looksLikeAnonFunc → parseAnonFuncExpr(stream, ctx)                │
│   │   │                                                                 │
│   │   ├── parse leading cluster (bound_cluster)                        │
│   │   │   │                                                             │
│   │   │   └── while '(' found                                           │
│   │   │       │                                                         │
│   │   │       └── parseParamList(stream, ctx)  ◄── WITH names          │
│   │   │           │                                                     │
│   │   │           ├── collect ParamAST* (with names) → paramGroups      │
│   │   │           └── collect TypeAST* (for FuncTypeAST) → paramTypes   │
│   │   │                                                                 │
│   │   ├── while '->' found                                              │
│   │   │   │                                                             │
│   │   │   ├── consume '->'                                              │
│   │   │   └── parseUnnamedCluster(stream, ctx)                         │
│   │   │       │                                                         │
│   │   │       └── while '(' found                                       │
│   │   │           │                                                     │
│   │   │           └── parseParamTypeList(stream, ctx)  ◄── NO names    │
│   │   │               │                                                 │
│   │   │               └── collect TypeAST* → paramTypes                 │
│   │   │                                                                 │
│   │   ├── parse final return type: parseType(stream, ctx)               │
│   │   ├── create FuncTypeAST (params: paramTypes, returnType)           │
│   │   ├── consume '{'                                                   │
│   │   ├── ScopedContext(FuncBody)                                       │
│   │   ├── parseBlock(stream, ctx)  (body)  ◄── recursive                │
│   │   ├── consume '}'                                                   │
│   │   └── create AnonFuncExprAST (funcType, paramGroups, body)          │
│   │                                                                     │
│   ├── IDENTIFIER ':' → parseModuleAccessExpr(stream, ctx)               │
│   │   │                                                                 │
│   │   ├── parse module name: IDENTIFIER                                 │
│   │   ├── consume ':'                                                   │
│   │   ├── parse member name: IDENTIFIER                                 │
│   │   ├── parse optional generic args: parseGenericArgs(stream, ctx)    │
│   │   └── create ModuleAccessExprAST                                    │
│   │                                                                     │
│   ├── looksLikeStructLiteral → parseStructLiteralExpr(stream, ctx)      │
│   │   │                                                                 │
│   │   ├── parse type name: IDENTIFIER                                   │
│   │   ├── parse optional generic args: parseGenericArgs(stream, ctx)    │
│   │   ├── consume '{'                                                   │
│   │   ├── parse field inits: IDENTIFIER '=' parseExpr(stream, ctx)*     │
│   │   ├── consume '}'                                                   │
│   │   └── create StructLiteralExprAST                                   │
│   │                                                                     │
│   └── IDENTIFIER → parseIdentifierExpr(stream, ctx)                     │
│       │                                                                 │
│       ├── parse name: IDENTIFIER                                        │
│       ├── parse optional generic args: parseGenericArgs(stream, ctx)    │
│       └── create IdentifierExprAST                                      │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
│
├── while precedence loop (prec >= minPrec)  ───────────────────────────┐
│                                                                       │
│   for each infix operator in order of precedence:                     │
│   │                                                                   │
│   ├── COMPOSE → parseComposeExpr(stream, ctx, lhs)                    │
│   │   │                                                               │
│   │   ├── consume '+>'                                                │
│   │   ├── parseComposeOperand(stream, ctx)*                           │
│   │   └── create ComposeExprAST                                       │
│   │                                                                   │
│   ├── assignment ops → parseInfixAssign(stream, ctx, lhs, op)         │
│   │   │                                                               │
│   │   ├── consume operator                                            │
│   │   ├── parsePrattExpr(stream, ctx, prec)  (rhs)  ◄── recursive     │
│   │   └── create AssignExprAST                                        │
│   │                                                                   │
│   ├── '??' → parseInfixNullCoalesce(stream, ctx, lhs)                 │
│   │   │                                                               │
│   │   ├── consume '??'                                                │
│   │   ├── parsePrattExpr(stream, ctx, prec)  (rhs)  ◄── recursive     │
│   │   └── create NullCoalesceExprAST                                  │
│   │                                                                   │
│   ├── binary op → parseInfixBinary(stream, ctx, lhs, op, prec)        │
│   │   │                                                               │
│   │   ├── consume operator                                            │
│   │   ├── parsePrattExpr(stream, ctx, prec+1)  (rhs)  ◄── recursive   │
│   │   └── create BinaryExprAST or RangeExprAST                        │
│   │                                                                   │
│   └── postfix ops → parsePostfixExpr(stream, ctx, lhs)                │
│       │                                                               │
│       ├── '(' → parseCallExpr(stream, ctx, callee, genericArgs)       │
│       │   │                                                           │
│       │   ├── parse optional generic args                             │
│       │   ├── parseArgList(stream, ctx)  (arguments)                  │
│       │   ├── parse optional '!' (arg pack)                           │
│       │   └── create CallExprAST                                      │
│       │                                                               │
│       ├── '[' → parseIndexExpr/parseSliceExpr(stream, ctx, target)    │
│       │   │                                                           │
│       │   ├── consume '['                                             │
│       │   ├── if range operator found                                 │
│       │   │   └── parseSliceExpr(stream, ctx, target)                 │
│       │   ├── else                                                    │
│       │   │   └── parseIndexExpr(stream, ctx, target)                 │
│       │   └── return expression                                       │
│       │                                                               │
│       ├── '|>' → parsePipelineExpr(stream, ctx, lhs)                  │
│       │   │                                                           │
│       │   ├── consume '|>'                                            │
│       │   ├── parsePipelineStep(stream, ctx)*                         │
│       │   └── create PipelineExprAST                                  │
│       │                                                               │
│       └── '.' → parseFieldAccessExpr(stream, ctx, lhs)                │
│           │                                                           │
│           ├── consume '.'                                             │
│           ├── parse field name: IDENTIFIER                            │
│           ├── validate: check for invalid ':' after field             │
│           └── create FieldAccessExprAST                               │
│                                                                       │
└───────────────────────────────────────────────────────────────────────┘
```
```

## Updated Grammar Notes Section


## Grammar Notes

### Function Type vs Function Declaration

**Key Distinction:** A `FuncTypeAST` only stores parameter **types**, never parameter **names**. Parameter names are stored separately in `FuncDeclAST.paramGroups` and `AnonFuncExprAST.paramGroups`.

This separation allows:
1. **Function types** (e.g., `[*](string, int) -> int`) to be used anywhere without names
2. **Function declarations** (e.g., `const add (a int)(b int) -> int`) to have named parameters in the leading cluster
3. **Anonymous functions** (e.g., `(x int) -> int { return x * 2 }`) to have named parameters
4. **Curried functions** to have names only in the first cluster, with unnamed types in subsequent clusters

**Parameter Name Rules:**
- Only the **leading cluster** of a `func_decl` or `func_literal` may name parameters
- All clusters after the first `->` use unnamed types only
- This matches the grammar: `chain = bound_cluster { '->' unnamed_cluster } '->' type`

### Function Type Parsing

```
func_type = unnamed_cluster { '->' unnamed_cluster } '->' type
unnamed_cluster = '(' type { ',' type } ')'

Examples:
  (int) -> bool                    → params: [int], returnType: bool
  (int)(string) -> bool            → params: [int, string], returnType: bool (adjacency)
  (int) -> (string) -> bool        → params: [int], returnType: FuncTypeAST (curried)
  (string, int) -> int             → params: [string, int], returnType: int
```

### Function Declaration Parsing

```
func_decl = ('let' | 'const') IDENTIFIER [ generic_params ] chain '=' func_body
chain = bound_cluster { '->' unnamed_cluster } '->' type

Examples:
  const add (a int)(b int) -> int = { ... }
           └─ bound_cluster ─┘   └─ return type ─┘

  const makeAdder (base int) -> (int) -> int = { ... }
                   └─ bound ─┘   └─ unnamed ─┘
```

### Parameter Parsing Helper Functions

| Function                          | Purpose                        | Parameter Names | Used For                                   |
| --------------------------------- | ------------------------------ | --------------- | ------------------------------------------ |
| `parseParamList(stream, ctx)`     | Parse parameters WITH names    | Required        | Function declarations, anonymous functions |
| `parseParamTypeList(stream, ctx)` | Parse parameters WITHOUT names | Not allowed     | Function types, type annotations           |

This clean separation eliminates the need for boolean flags and makes the parser's intent explicit.
```

## Updated Implementation Notes Section


## Implementation Notes

### Function Type vs Function Declaration Separation

The parser now clearly separates **function types** (which have no parameter names) from **function declarations** (which have named parameters in the leading cluster):

1. **`parseParamTypeList()`** → Parses parameter types only (no names)
   - Used for: `parseFuncType()` (function types, array element types, type annotations)
   - Produces: `std::vector<TypeAST*>`

2. **`parseParamList()`** → Parses parameters WITH names (names required)
   - Used for: `parseFuncDecl()` (function declarations) and `parseAnonFuncExpr()` (anonymous functions)
   - Produces: `std::vector<ParamAST*>`

3. **`parseFuncDecl()`** → Uses `parseParamList()` for the leading cluster (with names) and `parseParamTypeList()` for subsequent clusters (no names)
   - Produces: `FuncDeclAST` with both `funcType: FuncTypeAST*` and `paramGroups: ArenaSpan<ParamAST*>`

4. **`parseAnonFuncExpr()`** → Uses the same pattern as `parseFuncDecl()`
   - Produces: `AnonFuncExprAST` with both `funcType: FuncTypeAST*` and `paramGroups: ArenaSpan<ParamAST*>`

This separation fixes the bug where `[*](string, int) -> int` was being mis-parsed as if it had parameter names.

### Helper Function Design Principles

1. **No Boolean Flags**: Functions clearly state their purpose in their name
   - `parseParamList` = with names
   - `parseParamTypeList` = without names

2. **Single Responsibility**: Each function does exactly one thing

3. **Explicit Intent**: The function name tells you what to expect
```