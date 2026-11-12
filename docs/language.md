# Language

The Wall implementation in `src/compiler/libwall.c` is the bootstrap compiler.
It targets LLVM IR and uses the C ABI for calls into the runtime.

## Source files

- Extension: `.wa`
- Comments: `// ...` and `/* ... */`
- Statements are terminated with `;`
- Blocks use `{ ... }`

Textual includes are processed by the driver before compilation:

```wall
include "local.wa";
include <shared.wa>;
```

Quoted includes are resolved relative to the including file first, then through
each `-I` directory. Angle-bracket includes search the `-I` directories first.
Nesting is bounded and missing files are hard errors.

## Items

```wall
fn name(param: type, ...) -> type {
  ...
}

global name: type = expression;

let name: type = expression;

const name: type = expression;
```

Functions and globals may carry `export`. The current bootstrap compiler
exports the functions and globals it defines with external linkage, which is
what the linker needs for cross-object references.

## External declarations

`extern` introduces symbols that are defined in another object or by the
runtime:

```wall
extern fn puts(s: string) -> i32;
extern fn printf(fmt: string, ...) -> i32;
extern global errno: i32;
extern let argc: i32;
extern type opaque_handle;
```

- `extern fn` emits an LLVM `declare`. A trailing `...` enables variadic calls.
- `extern global` and `extern let` emit external global declarations.
- `extern type` records an opaque name; aggregate layout is not implemented in
  the bootstrap compiler.

## Types

The code generator understands:

| Type | LLVM representation |
| --- | --- |
| `i32`, `u32` | `i32` |
| `i64`, `u64` | `i64` |
| `float`, `f32` | `float` |
| `double`, `f64` | `double` |
| `bool` | `i1` / `i32` at C ABI boundaries |
| `string` | pointer to NUL-terminated bytes |
| `void` | no value |

A function without `-> type` returns `void`.

## Statements

```wall
let x: i32 = 1;
if (x > 0) { ... } else { ... }
while (x > 0) { x = x - 1; }
for (let i: i32 = 0; i < n; i = i + 1) { ... }
return x;
break;
continue;
```

`if`/`else` and loops generate explicit LLVM basic blocks. Boolean `&&` and
`||` short-circuit.

## Expressions

Operators, from lowest to highest precedence:

```text
=                        assignment
||                       logical or
&&                       logical and
== !=                    equality
< <= > >=                comparison
+ -                      additive
* / %                    multiplicative (where implemented)
! -                      unary
()                       grouping
IDENT, INT, FLOAT, STRING, true, false
IDENT(args...)
```

Integer arithmetic uses LLVM `add`, `sub`, `mul`, `sdiv`, and `srem`.
Comparisons produce `i1`, which is widened when used as an integer value.
String literals become private `[N x i8]` globals and are passed as pointers.

## Current limitations

The bootstrap compiler is intentionally small. It does not implement:

- user-defined structs, enums, arrays, slices, or pointers as first-class
  language features;
- pattern matching, generics, traits, or modules;
- closures, exceptions, or garbage collection;
- a full C preprocessor (only textual `include` is implemented).

These limitations are tracked against the self-hosted implementation in
`stage1/`. Unsupported syntax is rejected with a diagnostic rather than
silently miscompiled.

## ABI

The target is AArch64 ELF64 with the platform C ABI. Generated code uses the
standard integer, floating-point, and variadic calling conventions. The
runtime is freestanding and enters through `_start`; `main` is called by the
runtime shim.
