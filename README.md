# co

**co** is a small compiled language with **Go-like syntax** and **Rust-like ownership and borrow checking**,
written in C++ on top of LLVM. Memory is managed without a garbage collector: every value has one owner,
it is freed when the owner goes out of scope, and the compiler rejects dangling references, use-after-move,
and data races between aliasing references.

```go
type Person struct {
    name string
    tags []string
}

func (p &mut Person) addTag(tag string) {
    p.tags = append(p.tags, tag)
}

func longest(a &string, b &string) &string {
    if len(a) >= len(b) {
        return a
    }
    return b
}

func main() {
    bob := Person{name: "Bob"}
    bob.addTag("admin")              // auto-borrows `&mut bob`
    a := "hello"
    b := "hi"
    println(longest(&a, &b), len(bob.tags))
}
```

## Building

```sh
nix develop                       # shell with clang, LLVM 22, cmake, ninja
cmake -S . -B build -G Ninja
ninja -C build
./build/bin/coc run examples/hello.co
ctest --test-dir build            # or: python3 tests/run_tests.py build/bin/coc tests
```

Or just `nix build` / `nix run . -- run examples/hello.co`.

### Using the compiler

```
coc build file.co [-o out] [-O0..-O3] [--emit-llvm] [--emit-mir]
coc run   file.co
coc check file.co        # type-check and borrow-check only
```

`coc` links with the system C compiler (`cc`, or `$CO_CC`). Extra linker flags can go in `$CO_LDFLAGS`.
Setting `CO_DEBUG_ALLOC=1` when running a compiled program prints the number of live heap allocations
at exit (the test suite uses this to verify that nothing leaks or is freed twice).

## The language

### Syntax (Go-style)

* Semicolons are inserted automatically at line ends, as in Go.
* `x := expr` declares a variable; `var x T`, `var x = expr`, `var x T = expr` also work.
  `var x T` without a value gives the zero value (`0`, `0.0`, `false`, `""`, empty slice, zeroed struct).
* `if cond { } else if cond { } else { }`
* `for { }`, `for cond { }`, `for i := 0; i < n; i++ { }`, `for i := range n { }` (0..n-1)
* `break`, `continue`, `return`, `x++`, `x--`, `+= -= *= /= %=`
* Functions: `func add(a, b int) int { return a + b }`
* Methods: `func (p &Point) len() int { ... }` with receivers `T`, `&T` or `&mut T`.
  Receivers are auto-referenced like in Rust (`pt.shift(1, 2)` borrows `&mut pt`).

### Types

| type        | notes                                                                         |
|-------------|-------------------------------------------------------------------------------|
| `int`       | 64-bit signed                                                                 |
| `float`     | 64-bit                                                                        |
| `bool`      |                                                                               |
| `string`    | owned, heap-allocated UTF-8 bytes                                             |
| `[]T`       | owned, growable slice (like Rust's `Vec<T>`)                                  |
| `struct`    | `type P struct { x, y int }`, literal `P{x: 1, y: 2}` (missing fields are zero) |
| `&T`        | shared (read-only) reference                                                  |
| `&mut T`    | exclusive (mutable) reference                                                 |

### Builtins

`print(...)`, `println(...)`, `len(x)`, `append(v, x)`, `clone(x)`, `str(x)`, `int(x)`, `float(x)`, `panic(msg)`.
`len`, `clone`, `print` and string operators borrow their arguments automatically, so `len(s)` doesn't move `s`.

## Ownership & borrowing

The rules are Rust's, with less ceremony:

1. **Copy vs move.** `int`, `float`, `bool`, `&T`, and structs made only of those are copied.
   `string`, `[]T`, and structs containing them are *moved* on assignment, on being passed by value,
   and on being returned. A moved-from variable can't be used until it is assigned again.
   ```go
   s := "hi"
   t := s          // s moved into t
   println(s)      // error: borrow of moved value 's'
   u := clone(&t)  // explicit deep copy
   ```
2. **Borrowing.** At any point you can have *either* any number of `&T` *or* exactly one `&mut T`
   to a value, and you can't modify, move or drop a value while it is borrowed.
3. **Borrows end at their last use** (non-lexical lifetimes):
   ```go
   r := &v
   n := len(r)          // last use of r
   v = append(v, n)     // fine: the borrow already ended
   ```
4. **No dangling references.** A reference can't outlive what it points to:
   ```go
   func bad(p &int) &int {
       local := 1
       return &local     // error: 'local' does not live long enough
   }
   ```
5. **Reference returns are tied to reference parameters** (Rust's lifetime elision, simplified):
   a function returning `&T` must take at least one reference parameter, and the caller treats the
   result as borrowing from *all* reference arguments. No lifetime annotations are needed.
6. **References are not stored in structs or slices.** Containers own their data. This keeps the
   language free of lifetime parameters.
7. Passing a `&mut` reference to a function *reborrows* it, so you can keep using it afterwards;
   `&mut T` converts to `&T` implicitly.
8. `v = append(v, x)` (and `p.items = append(p.items, x)`) appends in place, which works even through
   a `&mut` reference.

Borrows of different struct fields don't conflict (`&mut p.a` and `&mut p.b` can coexist).
Index borrows are conservative: `&mut v[i]` conflicts with any other borrow of `v`.

Runtime checks: slice indexing is bounds-checked, and integer division by zero panics (exit code 101).

## How the compiler works

```
source ─► lexer ─► parser ─► AST ─► sema ─► MIR ─► borrow checker ─► LLVM IR ─► object ─► cc link
```

| file                  | role                                                                 |
|-----------------------|----------------------------------------------------------------------|
| `src/lexer.cpp`       | tokens + Go-style automatic semicolons                               |
| `src/parser.cpp`      | recursive-descent parser producing the AST (`src/ast.h`)             |
| `src/sema.cpp`        | name/type resolution, method lookup, auto-ref/reborrow insertion      |
| `src/mir_build.cpp`   | lowers to MIR, a control-flow graph with explicit moves, drops and scope ends |
| `src/borrowck.cpp`    | move/initialization dataflow, liveness, region inference, loan conflicts (NLL-style) |
| `src/codegen.cpp`     | MIR → LLVM IR, drop flags, generated drop/clone glue, optimization, object emission |
| `runtime/runtime.c`   | printing, strings, slices, panics                                    |

The borrow checker follows rustc's design: each reference-typed local gets a *region* (a set of
program points) that covers wherever it may still be used; each `&`/`&mut` creates a *loan* whose
region must contain the regions of all references derived from it. A forward dataflow then tracks
which loans are live at each point and reports any access that conflicts with one.

## Not yet supported

Generics, enums/`Option`, interfaces, closures, maps, modules/imports, string indexing, references
inside structs (would need lifetime parameters), unsigned/sized integer types.
