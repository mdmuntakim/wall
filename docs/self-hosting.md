# Self-hosting

The long-term goal is to replace the C bootstrap with the Wall implementation
in `stage1/`.

## Current state

- `src/` is the stage 0 bootstrap and is the default build.
- `stage1/` contains lexer, parser, code generator, driver, assembler, and
  linker sources written in Wall.
- The stage 1 compiler is not yet able to compile the stage 1 linker. The
  immediate blocker is a language gap in the bootstrap compiler: the
  translated linker uses pointers, structs, arrays, `%`, and `switch`, which
  the bootstrap front end does not implement.

The repository does not pretend the stage 1 path is complete. The build
scripts only build stage 0 unless a future stage 1 bootstrap is explicitly
added.

## Why the split exists

Bootstrapping a compiler in a smaller language keeps the trusted core
auditable:

1. A small C implementation establishes the ABI and code generation contract.
2. The Wall implementation is translated incrementally, with C and Wall
   sources kept side by side.
3. Features are added to the bootstrap only when they are required to compile
   the Wall implementation.
4. Once stage 1 can compile and link itself, stage 0 becomes optional.

## Working on stage 1

- Keep `stage1/` source changes separate from stage 0 changes.
- Use `scripts/ctowa.sh` for mechanical translations and review the generated
  comments for constructs it could not translate.
- Add the missing language features to stage 0 before translating code that
  depends on them.
- Never mark a stage 1 file as buildable until it actually compiles with the
  current bootstrap.

## Translation notes

`scripts/ctowa.sh` translates a deliberately small C subset: functions, scalar
locals/globals, assignment, compound assignment, increment/decrement, `if`,
`else`, `while`, `for`, `return`, calls, and arithmetic/comparison operators.
Constructs without a Wall equivalent are emitted as comments, not silently
dropped.

`scripts/gen_wa_tables.sh` appends deterministic ABI and opcode reference
tables to the stage 1 sources. `scripts/gen_linker_tables.sh` generates the
ELF relocation reference block embedded in the stage 0 linker.
