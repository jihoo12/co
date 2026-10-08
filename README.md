# co

**co** is a small compiled language that aims to be **as easy to write as Go**, with
**C's speed and reach** (no garbage collector, direct calls into C), while staying **memory-safe**.
Everyday code has no pointers to manage and no borrow checker to fight: every variable holds its own
value, and the compiler makes that cheap. When you need C's level of control, `unsafe` blocks give you
raw pointers, and C libraries can be called directly. It's written in C++ on top of LLVM and compiles to fast native
executables.

```go
type Shape enum {
    Circle(radius float)
    Rect(w, h float)
    Empty
}

func area(s Shape) float {
    switch s {
    case Circle(r): return 3.14 * r * r
    case Rect(w, h): return w * h
    case Empty:      return 0.0
    }
}

func find(names []string, want string) ?int {
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
   No references, no lifetimes, no "use of moved value", no generics syntax, no traits, no header files.
2. **Safe by default.** No null pointers, no dangling references, no use-after-free, no double free,
   no iterator invalidation, no data changing behind your back.
3. **Helpful errors.** Every error says what went wrong *and* what to write instead.
4. **Fast.** Values are freed the moment their variable goes away, with no garbage collector, and
   copies are made only where a program could tell the difference.

## Building

```sh
nix develop                       # shell with clang, LLVM 22, lld, cmake, ninja
cmake -S . -B build -G Ninja
ninja -C build
./build/bin/coc run examples/hello.co
ctest --test-dir build            # or: python3 tests/run_tests.py build/bin/coc tests
```

Or just `nix build` / `nix run . -- run examples/shapes.co`.

### Install with Nix

With [Nix flakes enabled](https://nixos.wiki/wiki/Flakes), install `coc` directly from GitHub:

```sh
nix profile install github:jihoo12/co
coc --help
```

Run without installing, or build the package locally:

```sh
nix run github:jihoo12/co -- --help
nix build github:jihoo12/co
./result/bin/coc --help
```

To upgrade or remove a profile installation, use `nix profile upgrade` or
`nix profile remove` with the entry name reported by `nix profile list`.

For a NixOS or Home Manager flake, add `co` as an input:

```nix
inputs.co.url = "github:jihoo12/co";
```

Then add `inputs.co.packages.${pkgs.system}.default` to
`environment.systemPackages` (NixOS) or `home.packages` (Home Manager),
passing `inputs` to the module as usual. The package is also available as
`packages.${system}.co`. Supported flake systems are x86_64/aarch64 Linux and macOS.


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

Types: `int` (64-bit), `float` (64-bit), `bool`, `string`, slices `[]T`, fixed-size arrays `[N]T`,
maps `map[K]V`, structs, enums, optionals `?T`, results `!T`, `error`, and functions `func(T...) R`. For
data with a fixed layout (and
for C) there are also `int8` `int16` `int32` `uint8` (`byte`) `uint16` `uint32` `uint64` and `float32`;
they wrap around on overflow. Number literals take whatever numeric type is needed (`var b byte = 200`,
`x * 2.5`), and other conversions are explicit: `int32(n)`, `float(i)`, `uint8(x)` (float to integer
conversions saturate).

Integers can be written `255`, `0xff`, `0b1111_1111` or `0o377`. The bitwise operators are Go's:
`&` `|` `^` `&^` (and not) `<<` `>>`, with `^x` (or `~x`) flipping every bit, and `&=`, `|=`, ... to
update in place. Shifting by the type's width or more gives 0 (or -1 for negative signed numbers), never
undefined behavior.

```go
flags := 0b0101
flags |= 1 << 3              // set bit 3
on := flags & (1 << 2) != 0  // test bit 2
var b uint8 = 0xF0
println(b >> 4, ^b)          // 15 15
```

Builtins: `println(...)`, `print(...)`, `len(x)`, `append(v, x)`, `clone(x)`, `str(x)`, `int(x)`,
`float(x)`, `error(msg)`, `delete(m, k)`, `panic(msg)`. `println` can print anything, including structs, slices, enums and optionals.

## Arrays

`[N]T` holds exactly N values, stored inline (no heap), like C arrays. Arrays are values: assigning one
copies it. Indexing is bounds-checked, `len(a)` is N, and `range` works as for slices.

```go
a := [5]int{1, 2, 3}         // the rest start at zero: [1, 2, 3, 0, 0]
b := a                       // a copy
b[0] = 100
var grid [3][3]int
grid[1][2] = 5
for i, x := range a { }
```

## Functions as values

A function's name, without calling it, is a function value of type `func(T...) R`. Function values can
be stored, passed and called. They're plain function pointers, so C can call them too (they take and
return numbers, bool, pointers and functions; closures aren't supported yet).

```go
func twice(x int) int { return x * 2 }
func apply(f func(int) int, x int) int { return f(x) }

type Button struct { onClick func(int) int }

println(apply(twice, 21))           // 42
b := Button{onClick: twice}
println(b.onClick(4))               // 8
var f func(int) int                 // nil until set
```

## Structs and methods

```go
type Point struct {
    x, y int
}

func (p Point) dist2() int { return p.x*p.x + p.y*p.y }    // reads p
func (p mut Point) move(dx int) { p.x += dx }              // changes p

p := Point{x: 1, y: 2}      // missing fields get their zero value
p.move(3)
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

Keys can be `int`, `string` or `bool`. An empty `var m map[K]V` is ready to use, unlike Go's nil maps.

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
func find(names []string, want string) ?int {
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
func parsePort(text string) !int {
    if text == "8080" { return 8080 }          // success is wrapped automatically
    return error("not a port: " + text)        // failure
}

func load(path string) !Config {
    port := try parsePort(path)                // on error, return it to my caller
    return Config{port: port}
}

func save(c Config) ! {                        // `!` alone: nothing, or an error
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
func (p Point) Far() bool { return dist2(p) > 100 }
func dist2(p Point) int { return p.X*p.X + p.Y*p.Y }      // private
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

## Calling C

Declare C functions in an `extern` block, naming the library they come from (linked as `-lname`; leave
the name out for the C library itself). Parameters and results are numbers, `bool`, C pointers `*T` and
functions:

```go
extern "sqlite3" {
    func sqlite3_libversion() *byte
    func sqlite3_open(name *byte, db **void) int32
    func sqlite3_close(db *void) int32
}

extern {
    func printf(format *byte, ...) int32      // variadic
    func memset(p *void, c int32, n uint64) *void
    func getenv(name *byte) *byte
    func qsort(base *void, n uint64, size uint64, cmp func(a, b *void) int32)
}

func byValue(a *void, b *void) int32 {
    unsafe { return *(*int32)(a) - *(*int32)(b) }
}

func main() {
    println(cstr(sqlite3_libversion()))      // cstr copies a C string into a co string
    var db *void
    sqlite3_open(":memory:", &mut db)        // &mut db is a **void out-parameter
    sqlite3_close(db)

    buf := []byte{0, 0, 0}
    memset(&mut buf, 65, 3)                  // a slice passes its elements
    printf("%s %d\n", "hi", int32(42))       // a string becomes a NUL-terminated copy
    if getenv("NOPE") == nil { println("unset") }

    nums := []int32{5, 3, 9}
    qsort(nums, 3, 4, byValue)               // C calls back into co
}
```

At a call to C, a `string` passed as `*byte` (or `*void`) becomes a temporary NUL-terminated copy, a
slice or array passes a pointer to its elements, and `&x` / `&mut x` pass x's address; these are only
valid during the call. C pointers can be `nil`, compared and stored anywhere; using what they point to
takes an `unsafe` block (below). `*void` converts to and from any pointer, like in C. Use
`coc build -L <dir>` for libraries outside the usual system directories (programs will look there at run
time too). Not yet supported: C structs passed by value.

## Pointers and `unsafe`

Inside `unsafe { ... }`, C pointers work as they do in C. The compiler can't check that a pointer is
valid, so the block marks where that's on you; everything outside it stays checked.

```go
extern {
    func malloc(n uint64) *void
    func free(p *void)
}

type Vec3 struct { x, y, z float32 }

unsafe {
    var p *Vec3 = malloc(uint64(sizeof(Vec3) * 10))
    p[3].x = 1.5                    // index like an array (no bounds check)
    p.y = 2                         // fields through a pointer, like Go
    q := &p[3]                      // & gives a pointer, *T
    *q = Vec3{x: 1}                 // read or write through it
    reg := (*uint32)(0x4000_0000)   // a pointer from an address (memory-mapped hardware)
    addr := uint64(p)               // and an address from a pointer (allowed anywhere)
    free(p)
}
```

Pointers can point to numbers, bool, pointers, functions, arrays and structs of those, which are the
types C uses. A `string` or a slice can't be reached through a pointer, so co's own memory management
can't be fooled. `sizeof(T)` gives a type's size in bytes, laid out as C lays it out.

---

# Values

This is the whole memory model, and it's what lets co be safe without a garbage collector or a borrow
checker you have to satisfy.

**1. Every variable holds its own value.** Assigning or passing a value gives the receiver a value of
its own. Changing one never changes the other, for strings, slices, maps and structs alike:

```go
a := []int{1, 2}
b := a            // b is a separate slice
b[0] = 9
println(a, b)     // [1, 2] [9, 2]
```

This doesn't mean copying everything. When the original isn't used again, the value is simply moved:
no copy at all. A copy is made only where the program could tell the difference, as in the example
above, where `a` is printed after `b` changed.

**2. Parameters are read-only views; `mut` parameters change the caller's value.**
A function gets its arguments without copying them. If it changes a parameter (or keeps it, say by
returning it or storing it in a struct), it simply gets its own copy instead, as if the argument had been
assigned to it. To change the caller's variable, write `mut` before the type:

```go
func shout(s string) string { return s + "!" }     // looks at s; nothing is copied
func reset(c mut Counter) { c.n = 0 }               // changes the caller's counter

msg := "hi"
shout(msg)
reset(counter)
println(msg, counter.n)                             // hi 0
```

Methods work the same way: `func (p Point) dist2() int` reads the point, and
`func (p mut Point) move(dx int)` changes it.

**3. Values are freed when their variable goes away.** Strings, slices and maps live on the heap and are
freed at the end of the block that owns them. There are no references that could outlive them, so
nothing can dangle.

**4. Looping over something while changing it is fine.** `for x := range v` goes over `v` as it
was when the loop started. If the loop body changes `v`, the compiler loops over a copy; otherwise it
looks at `v` in place. `switch` works the same way.

```go
for _, x := range v {
    v = append(v, x)   // fine: the loop still sees the original two elements
}
```

The one thing co rejects is passing the same variable to two parameters at once when one of them
may change it, like `swap(x, x)` or `fill(v, v)`. A function changing one of its arguments shouldn't
see another of its arguments change underneath it. Different parts of one value are fine:
`swap(p.a, p.b)`, and `swap(v[i], v[j])`, where the program checks that `i != j` when it runs (and stops
with a clear message if they're equal).

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
| `src/sema.cpp`        | types, methods, enums, how parameters are passed, optionals, exhaustiveness |
| `src/mir_build.cpp`   | lowers to MIR, a control-flow graph with explicit moves, copies, drops and scope ends |
| `src/borrowck.cpp`    | checks the references the compiler made: initialization, liveness, regions, conflicts |
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

### How values stay cheap

Under the hood co still uses references; they're just never written by the programmer.

- **Parameters.** Sema decides how each parameter is passed. Numbers and plain structs go by value.
  Other types are passed as a read-only reference, unless the function body changes the parameter or
  keeps it (returns it, stores it, ...); then the function takes the value itself. `mut T` is a
  mutable reference. Inside the function, every reference reads as the value it refers to.
- **Moves and copies.** The MIR builder hands values on by moving them. A liveness pass then turns each
  move out of a variable that is still used afterwards into a move out of a fresh copy. Values taken
  out of fields, elements or references are always copied.
- **The borrow checker** (rustc's design: regions, loans, NLL-style liveness) checks the references the
  compiler made. With the rules above, the only conflict a program can produce is one variable passed
  to two parameters where one changes it; everything else is a check on the compiler itself. When two
  uses differ only in a slice index (`v[i]` and `v[j]`), it inserts a run-time check that the indices
  differ instead of rejecting the program.

## Not yet supported

Generics, interfaces, closures, a standard library of packages, string functions (split, indexing, ...),
C structs passed by value, and building without the C library (for kernels and microcontrollers).
