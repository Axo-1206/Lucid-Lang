# Luc — `regex` Library Reference

> **Scope of this file:** Standard library documentation for the `regex` package.
> The `regex` package provides pattern compilation, matching, searching,
> replacement, and splitting on regular expression patterns.
>
> Import with: `use regex`

## Philosophy

Regular expressions in Luc are compiled patterns — not inline syntax. A pattern
string is compiled once into a `Regex` value and reused. This avoids re-compiling
the same pattern on every call and makes the type system aware of what a regex is.

Patterns are ordinary strings. Use raw string literals (`r"..."`) to avoid
double-escaping backslashes — `r"\d+"` is cleaner than `"\\d+"`.

Error handling uses the `error` library — `regex.compile` returns `Result<Regex>`
because a pattern may be syntactically invalid.

## Raw String Literals

Raw strings disable escape sequence processing. Every character is literal —
backslashes are not interpreted. This makes regex patterns, file paths, and
other backslash-heavy content readable.

```
raw_string      := 'r"' { any char except '"' } '"'
```

```luc
-- without raw string: double-escaping required
let pattern string = "^\\d{3}-\\d{4}$"

-- with raw string: backslashes are literal
let pattern string = r"^\d{3}-\d{4}$"

-- raw strings work anywhere a string is expected
let path string = r"C:\Users\luc\config.txt"
```

## Core Types

### `Regex`

A compiled regular expression pattern. Created via `regex.compile` — never
constructed directly.

### `Match`

The result of a successful match operation. Contains the matched text, its
position in the source string, and any captured groups.

```luc
pub struct Match {
    value   string      -- the full matched text
    start   int         -- start index in the source string (inclusive)
    end     int         -- end index in the source string (exclusive)
    groups  []string    -- captured groups in order, empty if no groups
}
```

### `RegexFlag`

Controls matching behaviour. Flags are combined with `|` (bitwise OR).

```luc
pub enum RegexFlag {
    None            = 0x00
    IgnoreCase      = 0x01    -- case-insensitive matching
    Multiline       = 0x02    -- ^ and $ match line boundaries, not just string boundaries
    DotAll          = 0x04    -- . matches newline characters
}
```

## Compiling Patterns

### `regex.compile`

Compiles a pattern string into a `Regex`. Returns `Result<Regex>` — fails if
the pattern is syntactically invalid.

```
regex.compile (pattern string) Result<Regex>
regex.compile (pattern string) (flags int) Result<Regex>
```

```luc
use regex
use error

-- basic compile
let re Result<Regex> = regex.compile(r"\d+")
let re Regex         = regex.compile(r"\d+") ?? panic("invalid pattern")

-- with flags
let re Regex = regex.compile(r"hello", int(RegexFlag.IgnoreCase)) ?? panic()

-- combining flags
let re Regex = regex.compile(
    r"^start.*end$",
    int(RegexFlag.IgnoreCase) | int(RegexFlag.Multiline)
) ?? panic()

-- error handling
let re Regex = regex.compile(r"\d+")
    .catch((e Error) Regex {
        io.printl("bad pattern: " + e.message)
        return regex.compile(r".*") ?? panic()
    })
```

## Matching

### `regex.matches`

Returns `true` if the **entire string** matches the pattern. No partial matches.

```
regex.matches (re Regex) (input string) bool
```

```luc
let re Regex = regex.compile(r"^\d{3}-\d{4}$") ?? panic()

regex.matches(re)("555-1234")   -- true
regex.matches(re)("555-12345")  -- false — extra digit, not full match
regex.matches(re)("abc-1234")   -- false

-- curried — partial application for reuse
let isPhoneNumber = regex.matches(re)
isPhoneNumber("555-1234")   -- true
isPhoneNumber("not-valid")  -- false

-- in a pipeline
"555-1234" -> isPhoneNumber -> io.printl
```

### `regex.contains`

Returns `true` if the pattern matches **anywhere** in the string.

```
regex.contains (re Regex) (input string) bool
```

```luc
let re Regex = regex.compile(r"\d+") ?? panic()

regex.contains(re)("abc123def")  -- true
regex.contains(re)("abcdef")     -- false
```

## Finding Matches

### `regex.find`

Returns the **first match** in the string, or `nil` if none.

```
regex.find (re Regex) (input string) Match?
```

```luc
let re Regex = regex.compile(r"\d+") ?? panic()

let m Match? = regex.find(re)("price: 42 dollars")

if m != nil {
    io.printl("found: " + m.value)   -- "42"
    io.printl("at: "   + string(m.start))  -- 7
}

-- with ?? fallback
let matched string = regex.find(re)("abc123").?value ?? "none"
```

### `regex.findAll`

Returns **all non-overlapping matches** in the string. Empty if none found.

```
regex.findAll (re Regex) (input string) []Match
```

```luc
let re Regex = regex.compile(r"\d+") ?? panic()

let matches []Match = regex.findAll(re)("1 cat, 2 dogs, 10 birds")

for m in matches {
    io.printl(m.value)   -- "1", "2", "10"
}

-- get just the matched strings
let numbers []string = regex.findAll(re)("1 cat, 2 dogs")
    -> map((m Match) string { return m.value })
```

## Capture Groups

Groups are defined with `(...)` in the pattern. Captured text is available
in `Match.groups` in the order they appear left to right.

```luc
-- named groups use (?P<name>...) syntax
let re Regex = regex.compile(r"(\d{4})-(\d{2})-(\d{2})") ?? panic()

let m Match? = regex.find(re)("date: 2026-03-15")

if m != nil {
    io.printl(m.groups[0])   -- "2026"  (year)
    io.printl(m.groups[1])   -- "03"    (month)
    io.printl(m.groups[2])   -- "15"    (day)
}
```

## Replacement

### `regex.replace`

Replaces the **first match** with a replacement string. Returns the modified string.

```
regex.replace (re Regex) (replacement string) (input string) string
```

```luc
let re Regex = regex.compile(r"\d+") ?? panic()

regex.replace(re)("NUM")("abc 42 def 99")   -- "abc NUM def 99"

-- curried for reuse
let redactNumbers = regex.replace(re)("***")
redactNumbers("call 555-1234 or 555-5678")  -- "call ***-1234 or 555-5678"
```

### `regex.replaceAll`

Replaces **all matches** with a replacement string.

```
regex.replaceAll (re Regex) (replacement string) (input string) string
```

```luc
let re Regex = regex.compile(r"\d+") ?? panic()

regex.replaceAll(re)("NUM")("abc 42 def 99")  -- "abc NUM def NUM"

-- in a pipeline
"price: 42, count: 7"
    -> regex.replaceAll(re)("X")
    -> io.printl                              -- "price: X, count: X"
```

## Splitting

### `regex.split`

Splits the string at each match of the pattern. Returns the pieces between
matches as an array.

```
regex.split (re Regex) (input string) []string
```

```luc
let re Regex = regex.compile(r"\s+") ?? panic()   -- split on whitespace

let words []string = regex.split(re)("one  two   three")
-- ["one", "two", "three"]

for w in words {
    io.printl(w)
}

-- split on comma + optional space
let csv Regex = regex.compile(r",\s*") ?? panic()
let fields []string = regex.split(csv)("a, b,c , d")
-- ["a", "b", "c", "d"]
```

## Common Patterns

A quick reference for frequently used patterns:

```luc
-- digits
let digits     = regex.compile(r"\d+")        ?? panic()   -- one or more digits
let integer    = regex.compile(r"^-?\d+$")    ?? panic()   -- optional minus, digits only
let decimal    = regex.compile(r"^-?\d*\.\d+$") ?? panic() -- floating point

-- text
let word       = regex.compile(r"\w+")        ?? panic()   -- word characters
let whitespace = regex.compile(r"\s+")        ?? panic()   -- one or more spaces/tabs
let email      = regex.compile(r"^[\w.+-]+@[\w-]+\.\w+$") ?? panic()

-- Vulkan / graphics
let hexColor   = regex.compile(r"^#?[0-9a-fA-F]{6}$") ?? panic()
let hexValue   = regex.compile(r"^0[xX][0-9a-fA-F]+$") ?? panic()
```

## Escape Sequences in Patterns

Within a regex pattern string, the following sequences have special meaning.
These are **regex** escapes interpreted by the pattern engine — separate from
Luc's string escape sequences.

| Sequence | Meaning |
|---|---|
| `\d` | digit `[0-9]` |
| `\D` | non-digit |
| `\w` | word character `[a-zA-Z0-9_]` |
| `\W` | non-word character |
| `\s` | whitespace `[ \t\n\r]` |
| `\S` | non-whitespace |
| `\b` | word boundary |
| `\B` | non-word boundary |
| `\n` | newline |
| `\t` | tab |
| `\.` | literal dot (`.` alone matches any char) |
| `\(` `\)` | literal parentheses |
| `\[` `\]` | literal brackets |

Use raw strings `r"..."` to avoid double-escaping: `r"\d+"` instead of `"\\d+"`.

## Full Example

```luc
package main

use io
use regex
use error

struct LogEntry {
    timestamp string
    level     string
    message   string
}

let parseLogLine (line string) LogEntry? = {

    let re Regex = regex.compile(
        r"^(\d{4}-\d{2}-\d{2}) \[(\w+)\] (.+)$"
    ) ?? return nil

    let m Match? = regex.find(re)(line)
    if m == nil { return nil }

    return LogEntry {
        timestamp = m.groups[0]
        level     = m.groups[1]
        message   = m.groups[2]
    }
}

let main () = {

    let lines []string = [
        "2026-03-15 [INFO] server started",
        "2026-03-15 [ERROR] connection refused",
        "not a valid log line",
        "2026-03-15 [WARN] retry attempt 2",
    ]

    for line in lines {
        let entry LogEntry? = parseLogLine(line)
        if entry != nil {
            io.printl("[" + entry.level + "] " + entry.message)
        } else {
            io.printl("skipped: " + line)
        }
    }
}
```