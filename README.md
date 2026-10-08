# co

**co** is a small compiled language that aims to be **as easy to write as Go** while being
**memory-safe without a garbage collector**, like Rust. It's written in C++ on top of LLVM and
compiles to fast native executables.

```go
type Shape enum {
    Circle(radius float)
    Rect(w, h float)
    Empty
}

func area(s &Shape) float {
    switch s {
    case Circle(r): return 3.14 * r * r
    case Rect(w, h): return w * h
    case Empty:      return 0.0
    }
}

func find(names &[]string, want &string) ?int {
    for i, n := range names {
        if n == want { return i }
    }
    return none
}

func main() {
    shapes := []Shape{Circle(1.0), Rect(2.0, 3.0), Empty}
    for _, s := range shapes {
        println(s, area(s))
    }
    names := []string{"ann", "bob"}
    println("bob is at", find(names, "bob") or -1)
}
```

## Design goals

1. **Easy first.** If you know Go (or Python, or JavaScript) you should be productive in an afternoon.
   No lifetimes, no generics syntax, no traits, no `unwrap()` chains, no header files.
2. **Safe by default.** No null pointers, no dangling references, no use-after-free, no double free,
   no iterator invalidation. These are compile errors, not crashes.
3. **Helpful errors.** Every error says what went wrong *and* what to write instead.
4. **Fast.** Values are freed the moment they're no longer owned, with no garbage collector.

## Building

```sh
nix develop                       # shell with clang, LLVM 22, lld, cmake, ninja
cmake -S . -B build -G Ninja
ninja -C build
./build/bin/coc run examples/hello.co
ctest --test-dir build            # or: python3 tests/run_tests.py build/bin/coc tests
```

Or just `nix build` / `nix run . -- run examples/shapes.co`.

```
coc run   file.co                 # compile and run
coc build file.co [-o out] [-O0..-O3] [--emit-llvm] [--emit-mir]
coc check file.co                 # only check for errors
```

`coc` is a single self-contained binary: the runtime and a linker (lld) are built in, so building
programs needs no C toolchain, only the system C library. On x86-64 Linux, `coc` links programs
itself, against the same `libc.so.6` and dynamic loader it runs with. Elsewhere, or when `$CO_CC`
or `$CO_LDFLAGS` is set, it links with a C compiler driver instead (`$CO_CC`, default `cc`, plus any
flags in `$CO_LDFLAGS`).
Running a program with `CO_DEBUG_ALLOC=1` prints the number of heap allocations still alive at exit
(the test suite uses it to check that nothing leaks or is freed twice).

---

# Language guide

## Basics

Syntax is Go's: no semicolons, `:=` declares, `for` is the only loop.

```go
x := 10                 // a variable (all variables can be changed)
var name string         // zero value: "", 0, 0.0, false, empty slice, none
var total float = 0.5

if x > 5 { println("big") } else { println("small") }

for i := 0; i < 3; i++ { }
for i := range 10 { }          // 0..9
for x < 100 { x *= 2 }         // while loop
for { break }                  // forever

func add(a, b int) int { return a + b }
```

Types: `int` (64-bit), `float` (64-bit), `bool`, `string`, slices `[]T`, maps `map[K]V`, structs, enums,
optionals `?T`, results `!T`, `error`, and references `&T` / `&mut T`.

Builtins: `println(...)`, `print(...)`, `len(x)`, `append(v, x)`, `clone(x)`, `str(x)`, `int(x)`,
`float(x)`, `error(msg)`, `delete(m, k)`, `panic(msg)`. `println` can print anything, including structs, slices, enums and optionals.

## Structs and methods

```go
type Point struct {
    x, y int
}

func (p &Point) dist2() int { return p.x*p.x + p.y*p.y }   // reads p
func (p &mut Point) move(dx int) { p.x += dx }              // changes p

p := Point{x: 1, y: 2}      // missing fields get their zero value
p.move(3)                   // no need to write &mut p: methods borrow automatically
println(p, p.dist2())       // Point{x: 4, y: 2} 20
```

## Slices and loops

```go
nums := []int{1, 2, 3}
nums = append(nums, 4)

for i, n := range nums { println(i, n) }   // index and element
for _, n := range nums { total += n }      // just the element
for i := range nums { nums[i] *= 2 }       // just the index (lets you modify nums)
```

## Maps

```go
ages := map[string]int{"ann": 30, "bob": 25}
ages["cat"] = 7                    // insert or update
ages["ann"]++                      // a missing key starts from zero

a := ages["ann"] or 0              // reading gives an optional (?int): handle "not found"
if ages["zed"] == none { println("no zed") }

delete(ages, "bob")
println(len(ages), ages)           // 2 {ann: 31, cat: 7}

for name, age := range ages { }    // insertion order, every time

groups := map[string][]string{}
groups["a"] = append(groups["a"], "x")   // grows the slice inside the map
```

Keys can be `int`, `string` or `bool`. Keys are copied into the map, so `m[name] = 1` doesn't use up
`name`. An empty `var m map[K]V` is ready to use, unlike Go's nil maps. A value read from a map of
strings (or other owned data) is borrowed, so the map can't change while you're still using it.

## Enums and switch

Enums list the shapes a value can take, and each variant can carry data:

```go
type Dir enum { North, East, South, West }

type Event enum {
    Click(x, y int)
    Key(name string)
    Quit
}

e := Click(10, 20)          // or Event.Click(10, 20) if the name is ambiguous

switch e {
case Click(x, y): println("click at", x, y)
case Key(k):      println("key", k)
case Quit:        println("bye")
}
```

`switch` must handle every variant (or have a `default`), so adding a variant later shows you every
place that needs updating. Switch also works on plain values and conditions, like Go:

```go
switch n {
case 1:    println("one")
case 2, 3: println("a few")
default:   println("many")
}

switch {
case score >= 90: println("A")
default:          println("keep going")
}
```

No `fallthrough`. `break` leaves the switch, as in Go.

## Optionals instead of null

There is no `nil`. A value that might be missing has type `?T`:

```go
func find(names &[]string, want &string) ?int {
    for i, n := range names {
        if n == want { return i }    // automatically becomes "some i"
    }
    return none
}

i := find(names, "bob") or -1        // use a default when missing
if find(names, "zoe") == none { println("no zoe") }

switch find(names, "bob") {          // or look inside with switch
case some(i): println("at", i)
case none:    println("missing")
}
```

Struct fields can be optional too (`email ?string`), and they start as `none`.

## Errors

A function that can fail returns `!T`: "a `T`, or an error". There is one built-in `error` type that
holds a message, so you never define error types.

```go
func parsePort(text &string) !int {
    if text == "8080" { return 8080 }          // success is wrapped automatically
    return error("not a port: " + text)        // failure
}

func load(path &string) !Config {
    port := try parsePort(path)                // on error, return it to my caller
    return Config{port: port}
}

func save(c &Config) ! {                       // `!` alone: nothing, or an error
    if c.port == 0 { return error("no port") }
}                                              // reaching the end means success
```

Three ways to deal with an error, from shortest to most explicit:

```go
p := try parsePort(s)            // pass it on (the function must return !something)
p := parsePort(s) or 80          // use a default
switch parsePort(s) {            // handle both
case ok(p):  println("port", p)
case err(e): println("bad input:", e)
}
```

Errors can't be ignored by accident: calling a `!` function without `try`, `or` or `switch` is a
compile error. `func main() !` may use `try`, and an error that reaches it is printed as
`error: ...` with exit status 1, which is handy for scripts. `try` also works on optionals inside a
function returning `?T`, where it passes `none` on. Add context with `+`: `error("loading config: " + e)`.

## Packages

As in Go, a directory is a package: all of its `.co` files share one namespace, so splitting a
package into files needs no imports. Names starting with an upper-case letter (functions, types,
fields, methods) are visible to other packages; everything else stays private.

```
project/
  co.mod          // marks the project root (can be empty)
  main.co
  geom/
    point.co
    shape.co
```

```go
// geom/point.co
type Point struct {
    X, Y int
    label string               // private to package geom
}

func New(x, y int) Point { return Point{X: x, Y: y, label: "new"} }
func (p &Point) Far() bool { return dist2(p) > 100 }
func dist2(p &Point) int { return p.X*p.X + p.Y*p.Y }     // private
```

```go
// main.co
import (
    "geom"
    s "util/strs"              // import under another name
)

func main() {
    p := geom.New(3, 4)
    q := geom.Point{X: 20}
    println(p.X, q.Far())
    shapes := []geom.Shape{geom.Circle(2), geom.Shape.Rect(3, 4)}
}
```

`import "a/b"` loads the directory `a/b` under the project root: the nearest directory, going up
from the program, that contains a `co.mod` file (or else the program's own directory). Import
cycles are an error. `coc run main.co` compiles just that file as the main package;
`coc run .` compiles every `.co` file in the directory. Variants of an exported enum can be written
`geom.Circle(2)` or `geom.Shape.Circle(2)`, and the same goes for `case` patterns.

---

# Ownership in 5 rules

This is what lets co be safe without a garbage collector. You don't need to memorize it, because the
compiler tells you exactly what to change. But here's the whole model:

**1. Each value has one owner, and it's freed when the owner goes away.**
`string`, slices, and structs/enums containing them *move* when you assign them or pass them to
a function by value. Numbers, bools, and plain structs of numbers are simply copied.

```go
a := "hello"
b := a            // the string moves to b
println(a)        // error: borrow of moved value 'a'
c := clone(b)     // want two copies? say so.
```

**2. Lend values with references instead of giving them away.**
A parameter declared `&T` *borrows* the argument. You don't write anything at the call site:

```go
func shout(s &string) { println(s + "!") }

msg := "hi"
shout(msg)        // msg is lent, not given away
println(msg)      // still yours
```

If you forget and declare `func shout(s string)`, the error message tells you to add the `&`.

**3. Changing something requires `&mut`, visibly.**
A function that modifies its argument takes `&mut T`, and the caller writes `&mut x`. You can always see
at a glance which calls may change your data. (Method calls borrow automatically.)

```go
func reset(c &mut Counter) { c.n = 0 }
reset(&mut counter)
```

**4. Either many readers or one writer.**
While something is borrowed, it can't be changed, moved or freed. A borrow lasts only until its
last use, so this works:

```go
first := &v[0]
println(first)        // last use of `first`
v = append(v, 4)      // fine
```

but this is caught at compile time instead of crashing at runtime:

```go
for _, x := range v {
    v = append(v, x)  // error: v is borrowed by the loop
}
```

**5. References can't outlive what they point to.**
Functions can return references only into their reference parameters, never to their own local
variables, and references can't be stored inside structs or slices. That's why co never needs
Rust-style lifetime annotations.

```go
func longest(a &string, b &string) &string {   // fine: returns one of its parameters
    if len(a) >= len(b) { return a }
    return b
}
```

### Looking without taking

`switch`, `for ... range` and `or` never take ownership of a variable they look into. Owned data inside
comes out *borrowed*. For example, `u.nick or "anon"` doesn't empty `u.nick`. When used on a fresh value
(like a function's result), you get the value itself.

### Runtime checks

Slice indexing is bounds-checked, and integer division by zero stops the program with a clear
message (exit code 101). `panic("message")` does the same on purpose.

---

# How the compiler works

```
source ─► lexer ─► parser ─► AST ─► sema ─► MIR ─► borrow checker ─► LLVM IR ─┐
                                                       runtime bitcode ─► link ─┴─► optimize ─► object ─► lld
```

| file                  | role                                                                 |
|-----------------------|----------------------------------------------------------------------|
| `src/loader.cpp`      | finds the main package's files and, through imports, every package   |
| `src/lexer.cpp`       | tokens + Go-style automatic semicolons                               |
| `src/parser.cpp`      | recursive-descent parser producing the AST (`src/ast.h`)             |
| `src/sema.cpp`        | types, methods, enums, auto-borrowing, optionals, exhaustiveness      |
| `src/mir_build.cpp`   | lowers to MIR, a control-flow graph with explicit moves, drops and scope ends |
| `src/borrowck.cpp`    | move/initialization dataflow, liveness, region inference, loan conflicts (NLL-style) |
| `src/codegen.cpp`     | MIR → LLVM IR, drop flags, generated drop/clone/print glue, optimization |
| `runtime/runtime.c`   | printing, strings, slices, maps, panics                              |
| `runtime/co_abi.h`    | memory layouts shared by the runtime and codegen                     |
| `src/linker.cpp`      | links executables: built-in lld with its own `_start`, or `cc`       |

The runtime is written in C, compiled to LLVM bitcode at build time and embedded in `coc`. Every
program is linked with it *before* optimization and everything but `main` is made internal, so the
optimizer sees the whole program: runtime calls such as `append` or map lookups inline into user code
and get specialized to the element and key types, and unused runtime code is dropped. Codegen checks
each runtime call against the runtime's real signature, and both sides share the layouts in
`co_abi.h`; maps are opaque to codegen apart from `len`.

## Performance

`bench/` has small programs with Go twins in `bench/go/`; `python3 bench/run.py build/bin/coc`
times both (set `$GO` if `go` isn't on PATH).

The borrow checker follows rustc's design: each local holding a reference gets a *region* (the set of
program points where it may still be used); each `&`/`&mut` creates a *loan* that must stay valid
for the regions of all references derived from it. A forward dataflow tracks live loans and reports
any access that conflicts with one.

## Not yet supported

Generics, interfaces, closures, a standard library of packages, string functions (split, indexing, ...), references
inside structs.
