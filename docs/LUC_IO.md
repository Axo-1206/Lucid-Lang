# Luc — `io` Library Reference

> **Scope of this file:** Standard library documentation for the `io` package.
> The `io` package covers console output, keyboard input, and mouse input.
> It is designed as a first-class input/output library — platform support for
> desktop is the initial target, with mobile and other platforms planned as
> the language grows.
>
> Import with: `use io`

---

## Philosophy

Most languages treat keyboard and mouse input as an afterthought buried in
platform-specific APIs. Luc's `io` library makes input a first-class citizen
of the standard library — keycodes, mouse events, and touch (future) are
available with the same ergonomics as printing to the console.

---

## Print

### `io.print`

Writes a value to standard output. No newline appended.

```
io.print (value string)
```

```luc
io.print("Hello")
io.print("World")
-- output: HelloWorld
```

### `io.printl`

Writes a value to standard output followed by a newline. The `l` stands for
*line* — the most common print form for general output.

```
io.printl (value string)
```

```luc
io.printl("Hello, World!")
-- output: Hello, World!
--         (newline)

io.printl("x = " + string(x))
io.printl("done")
```

### Notes

- Both functions accept `string` only — use the `string()` type conversion for
  other types before printing
- Pipeline-friendly: `io.printl` is a valid pipeline step since it takes one
  `string` parameter

```luc
-- pipeline usage
42 -> float -> string -> io.printl

-- compose into a pipeline
let logValue = string +> io.printl
logValue(3.14)
```

---

## Keyboard Input

Keyboard events are accessed through `io.key`. Each key is a value of type
`io.Key` — a struct with event-binding methods. Keys are identified by name
under `io.key`.

### Event model

Keyboard events fire callbacks when the key state changes. Callbacks are
anonymous functions with no parameters and no return value.

```
io.key.NAME.onPressed  (handler ())   -- fires once when key goes down
io.key.NAME.onReleased (handler ())   -- fires once when key goes up
io.key.NAME.onHeld     (handler ())   -- fires every frame while key is held
```

### Supported keys (initial set)

```luc
io.key.W        -- move forward
io.key.A        -- move left
io.key.S        -- move backward
io.key.D        -- move right
io.key.Space    -- jump / confirm
io.key.Escape   -- pause / cancel
io.key.Enter    -- confirm / submit
io.key.Tab      -- cycle / switch
```

> This is the initial set. The full keycode table will expand as the library
> grows. All standard keyboard keys are planned.

### Examples

```luc
package main

use io

let main () = {

    -- fire once when W is pressed
    io.key.W.onPressed(() {
        io.printl("W pressed — move forward")
    })

    -- fire once when W is released
    io.key.W.onReleased(() {
        io.printl("W released — stop")
    })

    -- fire every frame while Space is held
    io.key.Space.onHeld(() {
        io.printl("Space held — charging jump")
    })

    -- multiple keys bound independently
    io.key.Escape.onPressed(() {
        io.printl("Escape — opening pause menu")
    })

}
```

### Combining keys

Multiple key handlers compose naturally — bind them independently and the
runtime fires each registered handler in order.

```luc
-- WASD movement
io.key.W.onHeld(() { player.pos.y -= speed })
io.key.S.onHeld(() { player.pos.y += speed })
io.key.A.onHeld(() { player.pos.x -= speed })
io.key.D.onHeld(() { player.pos.x += speed })
```

---

## Mouse Input

Mouse events are accessed through `io.mouse`. Position, button press, and
release events are provided.

### Mouse position

```
io.mouse.x () float    -- current X position in window coordinates
io.mouse.y () float    -- current Y position in window coordinates
```

```luc
let px float = io.mouse.x()
let py float = io.mouse.y()
io.printl("mouse at " + string(px) + ", " + string(py))
```

### Mouse button events

```
io.mouse.left.onPressed  (handler ())   -- left button down
io.mouse.left.onReleased (handler ())   -- left button up
io.mouse.right.onPressed (handler ())   -- right button down
io.mouse.right.onReleased(handler ())   -- right button up
```

```luc
io.mouse.left.onPressed(() {
    let x float = io.mouse.x()
    let y float = io.mouse.y()
    io.printl("left click at " + string(x) + ", " + string(y))
})

io.mouse.right.onPressed(() {
    io.printl("right click — context menu")
})

io.mouse.left.onReleased(() {
    io.printl("left released — drag end")
})
```

---

## Full Example

```luc
package main

use io

struct Player {
    x     float = 0.0
    y     float = 0.0
    speed float = 5.0
}

let main () = {

    let player Player = Player {}

    -- movement
    io.key.W.onHeld(() { player.y -= player.speed })
    io.key.S.onHeld(() { player.y += player.speed })
    io.key.A.onHeld(() { player.x -= player.speed })
    io.key.D.onHeld(() { player.x += player.speed })

    -- actions
    io.key.Space.onPressed(() {
        io.printl("jump!")
    })

    io.key.Escape.onPressed(() {
        io.printl("paused")
    })

    -- mouse
    io.mouse.left.onPressed(() {
        io.printl("clicked at "
            + string(io.mouse.x()) + ", "
            + string(io.mouse.y()))
    })

}
```

---

## Planned (future platforms)

| Feature | Platform | Status |
|---|---|---|
| Touch events (`io.touch`) | mobile | planned |
| Gamepad input (`io.gamepad`) | console / desktop | planned |
| Window resize events | desktop | planned |
| Clipboard read/write | desktop | planned |
| File I/O (`io.file`) | all | planned |