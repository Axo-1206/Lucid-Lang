# Parser Structure

The Lucid parser is a recursive-descent parser with a Pratt (precedence-climbing) expression parser. All parser functions are declared in `Parser.hpp`; implementations are split across the files below.

> [!NOTE]
> All code block here are `pseudo code` (or `cpp`), we use the `\```cpp` or `\```swift` for color effects

## File Layout

```
src/parser/
├── Parser.hpp                          # Public API, function declarations, error recovery
├── Parser.cpp                          # parse(), parseProgram(), parseInternal(), parseDecl()
├── ModuleResolver.hpp/cpp              # Module resolution, caching, circular import detection
├── context/
│   ├── ParserContext.hpp               # Shared parse state (pool, arena, diagnostics, context stack)
│   └── TokenStream.hpp/cpp             # Buffered token stream with lazy lexing & lookahead
├── lexer/
│   ├── Lexer.hpp/cpp                   # Pure lexing functions
│   └── TokenStream.hpp/cpp             # Token stream abstraction
├── rules/                              # Grammar rule implementations
│   ├── ParseDecl.cpp                   # Declarations: import, variable, function, struct, enum, trait
│   ├── ParseType.cpp                   # Type annotations: primitives, named, array, ref, ptr, function
│   ├── ParseExpr.cpp                   # Pratt parser: all expressions
│   └── ParseStmt.cpp                   # Statements: if, for, while, return, block, concurrency
├── support/                            # Parser infrastructure
│   ├── ErrorRecovery.cpp               # Panic-mode error recovery with bracket awareness
│   ├── Helpers.cpp                     # Attributes, generics, param/arg lists, doc comments
│   └── LookAhead.cpp                   # Non-consuming disambiguation helpers
└── ModuleResolver.hpp/cpp              # Multi-file resolution, cyclic import detection
```

## Dispatch Graph

### Program Entry Points (`Parser.cpp`)

```cpp
┌─────────────────────────────────────────────────────────────────────────────────┐
│                                                                                 │
│  parseProgram(rootPath, rootSource, ctx)  ◄── WHOLE-PROGRAM ENTRY POINT         │
│  │                                                                              │
│  └── parse(rootPath, rootSource, ctx)  ────────────────────────────────────┐    │
│                                                                            │    │
│                                                                            │    │
│  parse(path, source, ctx)  ◄── SINGLE-FILE ENTRY POINT (also recursive)    │    │
│  │                                                                         │    │
│  ├── InternedString filePath = ctx.pool.intern(path)                       │    │
│  │                                                                         │    │
│  ├── Check cache: ctx.resolver->getParsedModule(filePath)                  │    │
│  │   └── if found → return cached (avoids re-parsing)                      │    │
│  │                                                                         │    │
│  ├── ScopedFileContext fileContext(ctx)  (saves/restores context stack)    │    │
│  │                                                                         │    │
│  ├── Check circular import: ctx.resolver->isParsing(filePath)              │    │
│  │   └── if true → return dummy ModuleAST with hasErrors = true            │    │
│  │                                                                         │    │
│  ├── ScopedParsingGuard parsingGuard(ctx.resolver, filePath)               │    │
│  │   └── pushes filePath onto parsingStack_ (detects cycles)               │    │
│  │                                                                         │    │
│  ├── lexer::tokenize(source, ctx.diagnostics)  ───► vector<Token>          │    │
│  │                                                                         │    │
│  ├── Check for UNKNOWN tokens (lexer errors)                               │    │
│  │   └── if found → return error ModuleAST                                 │    │
│  │                                                                         │    │
│  ├── TokenStream stream(std::move(tokens))                                 │    │
│  │                                                                         │    │
│  ├── parseInternal(stream, ctx, outDecls)  ────────────────────────────┐   │    │
│  │                                                                     │   │    │
│  │   #NOTE: during parseInternal (dispatched by parseDecl)             │   │    │
│  │          if we see a new parseImportDecl we will recursively        │   │    │
│  │          call parse again                                           │   │    │
│  │                                                                     │   │    │
│  │   parseInternal(stream, ctx, outDecls)                              │   │    │
│  │   │                                                                 │   │    │
│  │   └── loop until 'EOF'                                                │   │    │
│  │       │                                                             │   │    │
│  │       ├── harvestDocComment(stream, ctx)  (collects doc comments)   │   │    │
│  │       │                                                             │   │    │
│  │       ├── parseDecl(stream, ctx)  ───────────────────────────────┐  │   │    │
│  │       │   │                                                      │  │   │    │
│  │       │   └── if decl found: outDecls.push_back(decl)            │  │   │    │
│  │       │                                                          │  │   │    │
│  │       └── error recovery on parse failure                        │  │   │    │
│  │           ├── synchronizeToContext(stream, ctx)                  │  │   │    │
│  │           └── aggressive recovery after repeated failures        │  │   │    │
│  │                                                                  │  │   │    │
│  │   ◄──────────────────────────────────────────────────────────────┘  │   │    │
│  │                                                                     │   │    │
│  ├── Build ModuleAST                                                   │   │    │
│  │   ├── thisModule->filePath = filePath                               │   │    │
│  │   ├── thisModule->decls = builder.build()  (all declarations)       │   │    │
│  │   └── thisModule->hasErrors = ctx.diagnostics.hasErrors()           │   │    │
│  │                                                                     │   │    │
│  ├── Cache module: ctx.resolver->cacheModule(filePath, thisModule)     │   │    │
│  │                                                                     │   │    │
│  └── return thisModule  ◄──────────────────────────────────────────────┘   │    │
│                                                                            │    │
│                                                                            │    │
│  parseProgram(rootPath, rootSource, ctx)  ◄── continues here ◄─────────────┘    │
│  │                                                                              │
│  ├── if no ctx.resolver: return { root }  (single-file mode)                    │
│  │                                                                              │
│  └── ctx.resolver->getModuleOrder()  ───► vector<ModuleAST*>                    │
│      │                                                                          │
│      └── modules in dependency order (imports before dependents)                │
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘
```

> [!NOTE] 
> Key Relationships:
> - parseProgram() calls parse() once for the root file
> - parse() recursively calls itself for each import encountered
> - parseInternal() drives the declaration loop within a single file
> - parseDecl() dispatches to specific declaration parsers

```cpp
'Circular Import Detection':
───────────────────────────
ctx.resolver->isParsing(filePath)  ───► checks if filePath is in parsingStack_
                                        │
                                        └── if true → circular import detected

ScopedParsingGuard:
───────────────────
├── on 'construction': ctx.resolver->pushParsing(filePath)
└── on 'destruction':  ctx.resolver->popParsing()  (ensures stack cleanup)

Module. Order (Dependency Resolution):
─────────────────────────────────────
parse() → cacheModule() → moduleOrder_.push_back(filePath)
                           │
                           └── post-order: dependencies appear before dependents
```

### Declaration Dispatch (`parseDecl`)

```cpp
parseDecl(stream, ctx)                                      [Parser.cpp]
│
├── stream.consumeTrailing(TokenType::SEMICOLON)
├── harvestDocComment(stream, ctx)          (collects doc before decl)
├── parseAttributes(stream, ctx)            (collects @[...] attributes)
│
└── dispatch by keyword ─────────────────────────────────────────────────┐
                                                                         │
    ├── 'IMPORT'        → parseImportDecl(stream, ctx)        [ParseDecl.cpp]
    │
    ├── 'STRUCT'        → parseStructDecl(stream, ctx)        [ParseDecl.cpp]
    │
    ├── 'ENUM'          → parseEnumDecl(stream, ctx)          [ParseDecl.cpp]
    │
    ├── 'TRAIT'         → parseTraitDecl(stream, ctx)         [ParseDecl.cpp]
    │
    └── 'LET' / 'CONST'   → if looksLikeFuncDecl
                        │
                        ├── true  → parseFuncDecl(stream, ctx)
                        │
                        └── false → parseVarDecl(stream, ctx)
                                                                        │
└───────────────────────────────────────────────────────────────────────┘
```

### Declaration Parsers (`ParseDecl.cpp`)

```cpp
parseImportDecl(stream, ctx)
│
├── consume 'import'
├── parseImportPath(stream, ctx)                        [Helpers.cpp]
├── 'parse' alias (optional 'as' 'IDENTIFIER')
├── ctx.resolver->resolveImportPath(importPath)
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
```

> [!NOTE]
> Circular import detection is handled **EXCLUSIVELY** by parse().
> parseImportDecl() delegates to parse() and trusts it to handle
> all file parsing concerns including cycle detection.


```cpp
parseVarDecl(stream, ctx)
│
├── parse keyword: 'LET' or 'CONST'
├── parse name: 'IDENTIFIER'
├── parseType(stream, ctx)                                      [ParseType.cpp]
├── parse initializer: '=' parseExpr(stream, ctx)               [ParseExpr.cpp]
└── create VarDeclAST.

parseFuncDecl(stream, ctx)
│
├── parse keyword: 'LET' or 'CONST'
├── parse name: 'IDENTIFIER'
├── parse optional generic params: parseGenericParamDecls       [Helpers.cpp]
├── parse leading cluster (with names)
│   │
│   └── while '(' found
│       │
│       ├── parseParamList(stream, ctx, allowNames = true)      [Helpers.cpp]
│       ├── collect ParamAST* → leadingParams
│       └── if '->' found: break
│
├── parse rest of function type (after '->')
│   ├── consume '->'
│   └── parseType(stream, ctx) → restType
│
├── build FuncTypeAST (params: leadingParams, returnType: restType, hasArrow)
├── parse '=' and body
│   ├── block body: '{' parseBlock(stream, ctx) '}'
│   ├── expression body: parseExpr(stream, ctx)
│   │   ├── if looksLikeAnonFunc → report error & return nullptr
│   │   ├── if pure function ref → FuncRefStmtAST
│   │   └── else → ReturnStmtAST
│   └── no '=' → foreign function (body = nullptr)
└── create FuncDeclAST (name, keyword, genericParams, funcType, body)


parseStructDecl(stream, ctx)
│
├── consume 'struct'
├── parse name: 'IDENTIFIER'
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
├── parse name: 'IDENTIFIER'
├── parse optional backing type: ':' PrimitiveTypeAST
├── consume '{'
├── ScopedContext(EnumBody)
├── while not '}': parseEnumVariant(stream, ctx)
├── consume '}'
└── create EnumDeclAST

parseTraitDecl(stream, ctx)
│
├── consume 'trait'
├── parse name: 'IDENTIFIER'
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
├── parse name: 'IDENTIFIER'
├── parseType(stream, ctx)
├── parse optional default: '=' (block body or expression)
└── create FieldDeclAST

parseEnumVariant(stream, ctx)
│
├── parseAttributes(stream, ctx)
├── parse name: 'IDENTIFIER'
├── consume '='
├── parse integer literal (INT_LITERAL, HEX_LITERAL, BINARY_LITERAL)
└── create EnumVariantAST

parseTraitField(stream, ctx)
│
├── parseAttributes(stream, ctx)
├── parse optional 'const' modifier
├── parse name: 'IDENTIFIER'
├── parseType(stream, ctx)
└── create TraitFieldDeclAST
```

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
│   ├── std::vector<ParamAST*> allParams
│   ├── while '(' found
│   │   ├── parseParamList(stream, ctx, allowNames = false)     [Helpers.cpp]
│   │   ├── append groupParams → allParams
│   │   └── if '->' found: break
│   │
│   ├── create FuncTypeAST (params: allParams)
│   ├── if no '->': return funcType (hasArrow = false)
│   │
│   ├── consume '->'
│   ├── funcType->hasArrow = true
│   ├── parse return type
│   │   ├── if '(' → parseFuncType(stream, ctx)        ◄── curried function
│   │   └── else → parseType(stream, ctx)              ◄── normal return type
│   └── return funcType
│
├── 'IDENTIFIER' → parseNamedType(stream, ctx)
│   │
│   ├── if 'IDENTIFIER' ':' 'IDENTIFIER' → ModuleTypeAccessAST
│   └── else → NamedTypeAST + optional generic args
│
└── else → synchronizeToContext (caller handles diagnostic)
```

### Statement Dispatch (`ParseStmt.cpp`)

```cpp
parseStmt(stream, ctx)                                      [ParseStmt.cpp]
│
├── skip stray semicolons
│
└── dispatch by keyword ───────────────────────────────────────────────┐
                                                                       │
    ├── IF        → parseIfStmt(stream, ctx)                           │
    │   │                                                              │
    │   ├── consume 'if'                                               │
    │   ├── parseExpr(stream, ctx)  (condition)                        │
    │   ├── parseBlock(stream, ctx)  (then branch)                     │
    │   ├── if 'else' found                                            │
    │   │   ├── if 'if' → parseIfStmt(stream, ctx)  ◄── recursive      │
    │   │   └── else parseBlock(stream, ctx)  (else branch)            │
    │   └── create IfStmtAST                                           │
    │
    ├── SWITCH    → parseSwitchStmt(stream, ctx)                       │
    │   │                                                              │
    │   ├── consume 'switch'                                           │
    │   ├── parseExpr(stream, ctx)  (subject)                          │
    │   ├── consume '{'                                                │
    │   ├── while not '}'                                              │
    │   │   ├── 'default' → parse default clause                       │
    │   │   └── 'case' → parseSwitchCase(stream, ctx)                  │
    │   ├── consume '}'                                                │
    │   └── create SwitchStmtAST                                       │
    │
    ├── FOR       → parseForStmt(stream, ctx)                          │
    │   │                                                              │
    │   ├── consume 'for'                                              │
    │   ├── parse index binding: 'IDENTIFIER' TypeAST (or '_')         │
    │   ├── if ',' found                                               │
    │   │   ├── parse value binding: 'IDENTIFIER' TypeAST (or '_')     │
    │   │   ├── consume 'in'                                           │
    │   │   ├── parse iterable expression                              │
    │   │   └── parseBlock(stream, ctx)  → collection loop             │
    │   ├── else                                                       │
    │   │   ├── consume 'in'                                           │
    │   │   ├── parse range expression (RangeExprAST)                  │
    │   │   ├── parse optional step: '..' parseExpr(stream, ctx)       │
    │   │   └── parseBlock(stream, ctx)  → range loop                  │
    │   └── create ForStmtAST                                          │
    │
    ├── WHILE     → parseWhileStmt(stream, ctx)                        │
    │   │                                                              │
    │   ├── consume 'while'                                            │
    │   ├── parseExpr(stream, ctx)  (condition)                        │
    │   ├── parseBlock(stream, ctx)  (body)                            │
    │   └── create WhileStmtAST                                        │
    │
    ├── DO        → parseDoWhileStmt(stream, ctx)                      │
    │   │                                                              │
    │   ├── consume 'do'                                               │
    │   ├── parseBlock(stream, ctx)  (body)                            │
    │   ├── consume 'while'                                            │
    │   ├── parseExpr(stream, ctx)  (condition)                        │
    │   └── create DoWhileStmtAST                                      │
    │
    ├── RETURN    → parseReturnStmt(stream, ctx)                       │
    │   │                                                              │
    │   ├── consume 'return'                                           │
    │   ├── parseExpr(stream, ctx)  (optional value)                   │
    │   └── create ReturnStmtAST                                       │
    │
    ├── BREAK     → parseBreakStmt(stream, ctx)                        │
    │   │                                                              │
    │   ├── consume 'break'                                            │
    │   └── create BreakStmtAST                                        │
    │
    ├── CONTINUE  → parseContinueStmt(stream, ctx)                     │
    │   │                                                              │
    │   ├── consume 'continue'                                         │
    │   └── create ContinueStmtAST                                     │
    │
    ├── ASYNC     → parseAsyncStmt(stream, ctx)                        │
    │   │                                                              │
    │   ├── consume 'async'                                            │
    │   ├── parse keyword: 'LET' or 'CONST'                            │
    │   ├── parse name: 'IDENTIFIER'                                   │
    │   ├── parseType(stream, ctx)  (inner type)                       │
    │   ├── wrap type in FutureTypeAST                                 │
    │   ├── consume '='                                                │
    │   ├── parseExpr(stream, ctx)  (call expression)                  │
    │   └── create AsyncStmtAST                                        │
    │
    ├── AWAIT     → parseAwaitStmt(stream, ctx)                        │
    │   │                                                              │
    │   ├── consume 'await'                                            │
    │   ├── parse target list: 'IDENTIFIER' [ , 'IDENTIFIER' ... ]     │
    │   └── create AwaitStmtAST                                        │
    │
    ├── SPAWN     → parseSpawnStmt(stream, ctx)                        │
    │   │                                                              │
    │   ├── consume 'spawn'                                            │
    │   ├── if '_' found                                               │
    │   │   ├── consume '_'                                            │
    │   │   ├── consume '='                                            │
    │   │   ├── parseExpr(stream, ctx)  (call expression)              │
    │   │   └── create SpawnStmtAST (no binding)                       │
    │   ├── else                                                       │
    │   │   ├── parse keyword: 'LET' or 'CONST'                        │
    │   │   ├── parse name: 'IDENTIFIER'                               │
    │   │   ├── parseType(stream, ctx)  (inner type)                   │
    │   │   ├── wrap type in ThreadTypeAST                             │
    │   │   ├── consume '='                                            │
    │   │   ├── parseExpr(stream, ctx)  (call expression)              │
    │   │   └── create SpawnStmtAST (with binding)                     │
    │   └── return                                                     │
    │
    ├── JOIN      → parseJoinStmt(stream, ctx)                         │
    │   │                                                              │
    │   ├── consume 'join'                                             │
    │   ├── parse target list: 'IDENTIFIER' [ , 'IDENTIFIER' ... ]     │
    │   └── create JoinStmtAST                                         │
    │
    ├── 'LET'/'CONST'/'STRUCT'/'ENUM'/'TRAIT' → parseDeclStmt(stream, ctx) │
    │   │                                                              │
    │   ├── parseDecl(stream, ctx)                                     │
    │   └── create DeclStmtAST                                         │
    │
    ├── 'IMPORT'    → error: import only valid at top level            │
    │
    └── default   → parseExprStmt(stream, ctx)                         │
        │                                                              │
        ├── parseExpr(stream, ctx)                                     │
        └── create ExprStmtAST                                         │
                                                                       │
└──────────────────────────────────────────────────────────────────────┘

parseBlock(stream, ctx)
│
├── consume '{'
├── create BlockStmtAST
├── while not '}' and not 'EOF'
│   ├── parseStmt(stream, ctx)  ◄── recursive call
│   └── collect statements
├── consume '}'
└── return BlockStmtAST
```

### Expression Dispatch (Pratt Parser) (`ParseExpr.cpp`)

```cpp
parseExpr(stream, ctx)                                          [ParseExpr.cpp]
│
└── parsePrattExpr(stream, ctx, -1)

parsePrattExpr(stream, ctx, minPrec)
│
├── parsePrefixExpr(stream, ctx)  ────────────────────────────────────────┐
│                                                                         │
│   parsePrefixExpr(stream, ctx)                                          │
│   │                                                                     │
│   ├── unary operator. (-, not, ~)                                       │
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
│   │   ├── parse intrinsic name: 'IDENTIFIER'                            │
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
│   │   ├── parse leading cluster (with names)                            │
│   │   │   └── while '(' found                                           │
│   │   │       ├── parseParamList(stream, ctx, allowNames = true)        │
│   │   │       ├── collect ParamAST* → leadingParams                     │
│   │   │       └── if '->' found: break                                  │
│   │   │                                                                 │
│   │   ├── parse rest of function type (after '->')                      │
│   │   │   ├── consume '->'                                              │
│   │   │   └── parseType(stream, ctx) → restType                         │
│   │   │                                                                 │
│   │   ├── build FuncTypeAST (params: leadingParams, returnType: restType)│
│   │   ├── parse function body ({ parseBlock })                          │
│   │   └── create AnonFuncExprAST (funcType, body)                       │
│   │                                                                     │
│   ├── 'IDENTIFIER' ':' → parseModuleAccessExpr(stream, ctx)             │
│   │   │                                                                 │
│   │   ├── parse module name: 'IDENTIFIER'                               │
│   │   ├── consume ':'                                                   │
│   │   ├── parse member name: 'IDENTIFIER'                               │
│   │   ├── parse optional generic args: parseGenericArgs(stream, ctx)    │
│   │   └── create ModuleAccessExprAST                                    │
│   │                                                                     │
│   ├── looksLikeStructLiteral → parseStructLiteralExpr(stream, ctx)      │
│   │   │                                                                 │
│   │   ├── parse type name: 'IDENTIFIER'                                 │
│   │   ├── parse optional generic args: parseGenericArgs(stream, ctx)    │
│   │   ├── consume '{'                                                   │
│   │   ├── parse field inits: 'IDENTIFIER' '=' parseExpr(stream, ctx)*   │
│   │   ├── consume '}'                                                   │
│   │   └── create StructLiteralExprAST                                   │
│   │                                                                     │
│   └── 'IDENTIFIER' → parseIdentifierExpr(stream, ctx)                   │
│       │                                                                 │
│       ├── parse name: 'IDENTIFIER'                                      │
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
│           ├── parse field name: 'IDENTIFIER'                          │
│           ├── validate: check for invalid ':' after field             │
│           └── create FieldAccessExprAST                               │
│                                                                       │
└───────────────────────────────────────────────────────────────────────┘
```

### Helper Functions (`Helpers.cpp`)

```cpp
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
│   │       ├── parse name: 'IDENTIFIER'
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
│       ├── parse name: 'IDENTIFIER'
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

parseParamList(stream, ctx, allowNames)
│
├── params = std::vector<ParamAST*>
├── consume '('
├── if next is ')': consume ')' and return params
├── while not at end and not ')'
│   │
│   ├── handleCommaGap(stream, ctx, "parameter", isFirst)
│   ├── parse 'const' modifier (optional)
│   ├── if allowNames:
│   │   └── parse name: 'IDENTIFIER' (REQUIRED when allowNames is true)
│   ├── parse '...' (variadic, optional)
│   ├── parseType(stream, ctx)                                  [ParseType.cpp]
│   ├── if variadic: wrap type in ArrayTypeAST(Dynamic, 0, type)
│   ├── create ParamAST (with name, finalType, isVariadic, isConst)
│   ├── collect parameter → params
│   ├── validate name rules against allowNames flag
│   └── check variadic comma constraint
├── consume ')'
└── return params

parseArgList(stream, ctx)
│
├── consume '('
├── while not ')'
│   │
│   ├── handleCommaGap(stream, ctx, "argument", isFirst)
│   ├── parseExpr(stream, ctx)                                  [ParseExpr.cpp]
│   └── collect expression
├── consume ')'
└── return ArenaSpan<ExprAST*>

parseImportPath(stream, ctx)
│
├── while 'IDENTIFIER'
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

### Lookahead Helpers (`LookAhead.cpp`)

```cpp
looksLikeFuncDecl(stream, ctx)
│
├── check 'LET' or 'CONST'
├── check 'IDENTIFIER'
├── check optional generic params: '<' ... '>'
├── check '(' (parameter group start)
└── return bool (non-consuming, saves position)

looksLikeAnonFunc(stream, ctx)
│
├── check '(' at current position
├── parse complete parameter group (through matching ')')
├── check optional '->' and return type
├── check '{' at end
└── return bool

looksLikeStructLiteral(stream, ctx)
│
├── check 'IDENTIFIER'
├── check optional generic args: '<' ... '>'
├── check '{' after identifier
└── return bool
```

### Error Recovery (`ErrorRecovery.cpp`)

```cpp
synchronizeToContext(stream, ctx)
│
├── switch on ctx.currentContext()
│   │
│   ├── 'Attribute': skip until COMMA, RBRACKET, SEMICOLON, declaration keyword
│   │
│   ├── 'GenericParams': skip until COMMA, GREATER, LBRACE, LPAREN, SEMICOLON, declaration
│   │
│   ├── 'GenericArgs': skip until COMMA, GREATER, LPAREN, SEMICOLON, declaration
│   │
│   ├── 'FuncParams': skip until COMMA, RPAREN, LBRACE, SEMICOLON, declaration
│   │
│   ├── 'FuncBody/StructBody/EnumBody/TraitBody': skip until SEMICOLON, RBRACE, declaration
│   │
│   └── 'TopLevel': skip until SEMICOLON, declaration keyword
│
└── return SyncOutcome (Continuable if stopped at list separator)

synchronizeTo<T>(stream, ctx, stopTokens...)
│
└── synchronizeUntil(stream, ctx, [stopTokens](t) { ... })

synchronizeUntil(stream, ctx, stopAt)
│
├── track bracket nesting (LPAREN, LBRACKET, LBRACE)
├── skip tokens until:
│   ├── stopAt() returns true AND no brackets open
│   ├── matching closer is found (belongs to enclosing construct)
│   └── 'EOF' reached
└── return
```

### Module Resolution (`ModuleResolver.hpp/cpp`)

```cpp
ModuleResolver
│
├── 'packageRoot_' : filesystem::path
├── 'pool_' : StringPool&
├── 'importPathToFile_' : unordered_map<InternedString, InternedString>
├── 'parsedModules_' : unordered_map<InternedString, ModuleAST*>
├── 'moduleOrder_' : vector<InternedString>
├── 'parsingStack_' : vector<InternedString>
└── 'resolvedPathCache_' : unordered_map<InternedString, filesystem::path>.

resolveImportPath(importPath)
│
├── check cache: importPathToFile_
├── convert "std.io" → "std/io.luc"
├── resolveRelativePath(relativePath)
├── cache result
└── return InternedString

isParsing(modulePath)
│
├── check if modulePath is in parsingStack_
└── return bool

isModuleParsed(modulePath)
│
├── check if modulePath is in parsedModules_
└── return bool

getParsedModule(modulePath)
│
├── check parsedModules_
└── return ModuleAST* or nullptr

cacheModule(modulePath, ast)
│
├── parsedModules_[modulePath] = ast
├── moduleOrder_.push_back(modulePath)
└── return

readModuleSource(filePath)
│
├── getModuleFilePath(filePath)
├── read file contents
└── return string
```

### TokenStream (`TokenStream.hpp/cpp`)

```cpp
TokenStream
│
├── tokens_ : vector<Token>
├── pos_ : size_t
├── diagnostics_ : DiagnosticEngine*
├── ensureTokens(count) → lazy lexing
└── skipCommentsFrom(start) → skip LINE_COMMENT, DOC_COMMENT, BLOCK_COMMENT

Public Methods:
│
├── peek() → const Token& (skips comments)
├── consume() → Token (skips comments)
├── check(type) → bool
├── match(type) → bool (consume if matches)
├── isAtEnd() → bool
├── consumeTrailing(type) → int (count consumed)
├── peekNext() → const Token&
├── peekNextType() → TokenType
├── peekAt(offset) → const Token&
├── getPos() / setPos(pos) → size_t
└── currentLoc() → SourceLocation
```

### ParserContext (`ParserContext.hpp`)

```cpp
ParserContext
│
├── pool : StringPool&
├── arena : ASTArena&
├── diagnostics : DiagnosticEngine&
├── resolver : ModuleResolver* (optional)
├── contextStack : vector<ContextFrame>
├── pendingDoc : optional<DocComment>
├── pushContext(kind, loc)
├── popContext()
├── currentContext() → SyntacticContext
├── isInsideContext(kind) → bool
└── contextDepth() → size_t

SyntacticContext enum
│
├── TopLevel
├── Attribute
├── GenericParams
├── GenericArgs
├── FuncParams
├── FuncBody
├── FieldBody
├── StructBody
├── EnumBody
└── TraitBody
.
ScopedContext (RAII guard)
│
├── pushes context on construction
└── pops context on destruction
.
ScopedFileContext (RAII guard)
│
├── saves context stack on construction
└── restores context stack on destruction
```

### Lexer (`Lexer.hpp/cpp`)

```cpp
lexer::tokenize(source, diagnostics)
│
├── create LexerState
├── while not 'EOF'
│   │
│   ├── skipWhitespace()
│   │
│   └── dispatch by character:
│       │
│       ├── '/' '-' → lexBlockComment / lexDocComment
│       │
│       ├── identifier start → lexIdentifier()
│       │   │
│       │   ├── collect identifier characters
│       │   ├── is_keyword(value) → keyword_to_type(value)
│       │   └── return Token ('IDENTIFIER' or keyword type)
│       │
│       ├── digit or '.' digit → lexNumber()
│       │   │
│       │   ├── '0x' → HEX_LITERAL
│       │   ├── '0b' → BINARY_LITERAL
│       │   ├── '0o' → INT_LITERAL (octal)
│       │   ├── decimal integer → INT_LITERAL
│       │   └── decimal float → FLOAT_LITERAL
│       │
│       ├── '"' → lexString() / lexRawString()
│       │   │
│       │   ├── consume opening '"'
│       │   ├── handle escapes: \n, \t, \r, \\, \", \0
│       │   ├── handle interpolation: \(expr)
│       │   ├── consume closing '"'
│       │   └── return STRING_LITERAL
│       │
│       ├── ''' → lexChar()
│       │
│       └── else → lexOperatorOrPunctuation()
│           │
│           ├── two-character operators: **, <<, >>, .., +>, |>, ?., ??, ->
│           ├── single-character operators: =, +, -, *, /, %, &, |, ^, ~, <, >, !
│           ├── punctuation: ., :, ,, ;, (, ), {, }, [, ]
│           └── unknown → UNKNOWN token
│
└── return vector<Token>
```

## Precedence Table

| Precedence | Operators                        | Associativity |
| ---------- | -------------------------------- | ------------- |
| 9          | `.` (field access)               | Left          |
| 8          | `+>` (composition)               | Left          |
| 6          | `*`, `/`, `%`, `**`              | Left          |
| 5          | `+`, `-`                         | Left          |
| 4          | `..`, `..<` (range)              | Left          |
| 3          | `==`, `!=`, `<`, `<=`, `>`, `>=` | Left          |
| 2          | `and`                            | Left          |
| 1          | `or`                             | Left          |
| 0          | `??` (null coalesce)             | Left          |
| -1         | `\|>` (pipeline)                 | Left          |
| -2         | Assignment ops (`=`, `+=`, etc.) | Right         |

## Grammar Coverage

### Declarations
- `import path [as alias]` — module import
- `let name Type [= expr]` — mutable variable
- `const name Type = expr` — immutable variable
- `const name<T> (params) -> ReturnType = { body }` — function
- `const name<T> (params) -> ReturnType = existingFn` — function reference
- `struct Name<T> : Trait { field Type [= default], ... }` — struct
- `enum Name : IntType { Variant = value, ... }` — enum
- `trait Name<T> { field Type, ... }` — trait

### Types
- Primitives: `bool`, `int8`, `int16`, `int32`, `int64`, `uint8`, `uint16`, `uint32`, `uint64`, `byte`, `short`, `int`, `long`, `ubyte`, `ushort`, `uint`, `ulong`, `float`, `double`, `decimal`, `string`, `char`
- Named: `Name`, `module:Name`, `Name<Type>`
- Arrays: `[*]T` (dynamic), `[_]T` (slice), `[N]T` (fixed)
- References: `&T`
- Pointers: `*T`
- Functions: `(T, U) -> R`, `(T) -> (U) -> R` (curried)
- Nullable: `T?`
- Fallible: `T!`
- Combined: `T?!`

### Statements
- Block: `{ stmt* }`
- If: `if expr { ... } [else { ... }]` / `if expr ?? expr else expr`
- Switch: `switch expr { case v1, v2, ...: { ... } default: { ... } }`
- For: `for i Type in start..end [.. step] { ... }` / `for i Type, v Type in collection { ... }`
- While: `while expr { ... }`
- Do-While: `do { ... } while expr`
- Return: `return [expr]`
- Break: `break`
- Continue: `continue`
- Async: `async [let|const] name Type = call()`
- Await: `await name [ , name ... ]`
- Spawn: `spawn _ = call()` / `spawn [let|const] name Type = call()`
- Join: `join name [ , name ... ]`
- Expression statement: `expr`
- Declaration statement: `let`, `const`, `struct`, `enum`, `trait`

### Expressions
- Literals: `42`, `3.14`, `"hello"`, `"""raw"""`, `'a'`, `0xFF`, `0b1010`, `true`, `false`, `nil`, `err`
- Array literal: `[1, 2, 3]`
- Struct literal: `Point { x = 1, y = 2 }`
- Anonymous function: `(x int) -> int { return x * 2 }`
- If expression: `if cond ?? thenExpr else elseExpr`
- Intrinsic call: `#sizeof(T)`, `#sqrt(x)`
- Module access: `math:sqrt`
- Field access: `obj.field`
- Call: `f(args)`, `f<T>(args)`
- Index: `arr[0]`
- Slice: `arr[1..3]`, `arr[..<5]`, `arr[2..]`
- Pipeline: `expr |> step1 |> step2`
- Composition: `f +> g +> h`
- Binary: arithmetic, comparison, logical, bitwise, range
- Unary: `-x`, `not x`, `~x`
- Assignment: `x = y`, `x += y`, etc.
- Null coalesce: `x ?? y`
```