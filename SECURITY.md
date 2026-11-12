# Security policy

## Supported versions

Security fixes are applied to the current `main` branch and the latest tagged
release.

## Reporting a vulnerability

Do not open a public issue for a suspected vulnerability. Report it privately
to the maintainers through the repository's private security advisory channel,
or by email to the address listed in the package metadata.

Include:

- affected version (`wall --version`);
- a description of the issue and its impact;
- a minimal reproducer or proof of concept;
- any suggested mitigation.

The maintainers will acknowledge the report and coordinate disclosure. Please
allow time for a fix before publishing details.

## Scope

The toolchain compiles and links untrusted source and object files. Relevant
classes of issues include:

- memory-safety defects in the compiler, assembler, or linker;
- arbitrary code execution during compilation;
- path traversal in include or library resolution;
- malformed ELF handling that crashes or corrupts output;
- package layout or permission errors in the installer.
