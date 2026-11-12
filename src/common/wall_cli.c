/* wall_cli.c - the Wall toolchain option catalogue.
 *
 * See wall_cli.h.  The table is intentionally broad: it covers the GNU ld and
 * GNU as interfaces a build system is likely to probe.  Options that the Wall
 * backends do not implement are marked COMPAT (accepted, no semantic effect) or
 * REJECTED (an explicit, honest error), never silently misinterpreted.
 */
#include "wall_cli.h"
#include <string.h>

#define A0 0
#define A1 1
#define A2 2
#define OPT(l, s, a, an, t, d, sup) { l, s, a, an, t, d, sup }

const WallOption wall_options[] = {
  /* ---- linker: general -------------------------------------------------- */
  OPT("help", 'h', A0, NULL, "ld", "Print linker help", 1),
  OPT("version", 'V', A0, NULL, "ld", "Print version information", 1),
  OPT("verbose", 'v', A0, NULL, "ld", "Trace files and actions", 1),
  OPT("output", 'o', A1, "FILE", "ld", "Write output to FILE", 1),
  OPT("entry", 'e', A1, "SYM", "ld", "Set the entry symbol", 1),
  OPT("relocatable", 'r', A0, NULL, "ld", "Produce a relocatable object", 1),
  OPT("shared", 'B', A0, NULL, "ld", "Produce a shared object", 1),
  OPT("Bshareable", 0, A0, NULL, "ld", "Produce a shared object", 1),
  OPT("static", 0, A0, NULL, "ld", "Produce a static executable", 1),
  OPT("pie", 0, A0, NULL, "ld", "Produce a position-independent executable", 1),
  OPT("symbols", 's', A0, NULL, "ld", "Keep the symbol table (default kept)", 1),
  OPT("strip-all", 's', A0, NULL, "ld", "Strip all symbols", 1),
  OPT("strip-debug", 'S', A0, NULL, "ld", "Strip debug information", 1),
  OPT("strip-discarded", 0, A0, NULL, "ld", "Strip symbols of discarded sections", 1),
  OPT("no-strip-discarded", 0, A0, NULL, "ld", "Keep discarded-section symbols", 1),
  OPT("map", 'M', A1, "FILE", "ld", "Write a link map", 1),
  OPT("print-map", 'M', A0, NULL, "ld", "Print the link map", 1),
  OPT("print-output-format", 0, A0, NULL, "ld", "Print the output object format", 1),
  OPT("print-memory-usage", 0, A0, NULL, "ld", "Print memory region usage", 1),
  OPT("whole-archive", 0, A0, NULL, "ld", "Load every archive member", 1),
  OPT("no-whole-archive", 0, A0, NULL, "ld", "Restore lazy archive loading", 1),
  OPT("undefined", 'u', A1, "SYM", "ld", "Force SYM to be undefined", 1),
  OPT("defsym", 0, A1, "SYM=VAL", "ld", "Define an absolute symbol", 1),
  OPT("build-id", 0, A2, "STYLE", "ld", "Emit a GNU build-id note", 1),
  OPT("no-build-id", 0, A0, NULL, "ld", "Do not emit a build-id note", 1),
  OPT("comment", 0, A1, "TEXT", "ld", "Set the .comment text", 1),
  OPT("target", 0, A1, "TRIPLE", "ld", "Select the target triple", 1),
  OPT("oformat", 0, A1, "FMT", "ld", "Set the output format", 1),
  OPT("format", 'b', A1, "FMT", "ld", "Set the input object format", 1),
  OPT("no-undefined", 'z', A0, NULL, "ld", "Report unresolved symbols", 1),
  OPT("fatal-warnings", 0, A0, NULL, "ld", "Treat warnings as errors", 1),
  OPT("no-fatal-warnings", 0, A0, NULL, "ld", "Do not treat warnings as errors", 1),
  OPT("trace", 't', A0, NULL, "ld", "Trace input files", 1),
  OPT("no-trace", 0, A0, NULL, "ld", "Disable input tracing", 1),
  OPT("stats", 0, A0, NULL, "ld", "Print link statistics", 1),
  OPT("emit-relocs", 'q', A0, NULL, "ld", "Keep input relocations", 1),
  OPT("no-emit-relocs", 0, A0, NULL, "ld", "Discard input relocations", 1),
  OPT("section-start", 0, A1, "SEC=ADDR", "ld", "Set a section start address", 1),
  OPT("Ttext", 0, A1, "ADDR", "ld", "Set the .text address", 1),
  OPT("Tdata", 0, A1, "ADDR", "ld", "Set the .data address", 1),
  OPT("Tbss", 0, A1, "ADDR", "ld", "Set the .bss address", 1),
  OPT("Ttext-segment", 0, A1, "ADDR", "ld", "Set the text segment address", 1),
  OPT("section-alignment", 0, A1, "ADDR", "ld", "Set section alignment", 1),
  OPT("max-page-size", 0, A1, "SIZE", "ld", "Set the maximum page size", 0),
  OPT("common-page-size", 0, A1, "SIZE", "ld", "Set the common page size", 0),
  OPT("hash-style", 0, A1, "STYLE", "ld", "Select dynamic hash style", 0),
  OPT("hash-size", 0, A1, "N", "ld", "Set hash table size", 0),
  OPT("dynamic-linker", 0, A1, "FILE", "ld", "Set the ELF interpreter", 0),
  OPT("soname", 0, A1, "NAME", "ld", "Set DT_SONAME", 0),
  OPT("rpath", 0, A1, "PATH", "ld", "Set DT_RPATH", 0),
  OPT("rpath-link", 0, A1, "PATH", "ld", "Set the runtime search path", 0),
  OPT("enable-new-dtags", 0, A0, NULL, "ld", "Emit DT_RUNPATH", 0),
  OPT("disable-new-dtags", 0, A0, NULL, "ld", "Emit DT_RPATH", 0),
  OPT("export-dynamic", 'E', A0, NULL, "ld", "Export dynamic symbols", 0),
  OPT("no-export-dynamic", 0, A0, NULL, "ld", "Do not export dynamic symbols", 0),
  OPT("dynamic-list", 0, A1, "FILE", "ld", "Read a dynamic symbol list", 0),
  OPT("dynamic-list-cpp-new", 0, A0, NULL, "ld", "Dynamic-list C++ new/delete", 0),
  OPT("dynamic-list-cpp-typeinfo", 0, A0, NULL, "ld", "Dynamic-list C++ typeinfo", 0),
  OPT("version-script", 0, A1, "FILE", "ld", "Read a symbol version script", 0),
  OPT("allow-multiple-definition", 0, A0, NULL, "ld", "Allow duplicate definitions", 0),
  OPT("allow-shlib-undefined", 0, A0, NULL, "ld", "Allow undefined symbols in shared libs", 0),
  OPT("no-allow-shlib-undefined", 0, A0, NULL, "ld", "Reject undefined shared-lib symbols", 0),
  OPT("check-sections", 0, A0, NULL, "ld", "Check section addresses", 0),
  OPT("no-check-sections", 0, A0, NULL, "ld", "Skip section address checks", 0),
  OPT("compress-debug-sections", 0, A2, "Z", "ld", "Compress debug sections", 0),
  OPT("no-compress-debug-sections", 0, A0, NULL, "ld", "Do not compress debug sections", 0),
  OPT("cref", 0, A0, NULL, "ld", "Emit a cross-reference table", 0),
  OPT("no-cref", 0, A0, NULL, "ld", "Do not emit a cross-reference table", 0),
  OPT("demangle", 0, A0, NULL, "ld", "Demangle symbol names", 0),
  OPT("no-demangle", 0, A0, NULL, "ld", "Do not demangle symbol names", 0),
  OPT("define-common", 'd', A0, NULL, "ld", "Allocate common symbols", 0),
  OPT("no-define-common", 0, A0, NULL, "ld", "Do not allocate common symbols", 0),
  OPT("error-execstack", 0, A0, NULL, "ld", "Error on executable stacks", 0),
  OPT("no-error-execstack", 0, A0, NULL, "ld", "Do not error on executable stacks", 0),
  OPT("error-rwx-segments", 0, A0, NULL, "ld", "Error on RWX segments", 0),
  OPT("no-error-rwx-segments", 0, A0, NULL, "ld", "Do not error on RWX segments", 0),
  OPT("gc-sections", 0, A0, NULL, "ld", "Discard unreachable sections", 0),
  OPT("no-gc-sections", 0, A0, NULL, "ld", "Keep all sections", 0),
  OPT("gc-keep-exported", 0, A0, NULL, "ld", "Keep exported sections in GC", 0),
  OPT("gnu-unique", 0, A0, NULL, "ld", "Use GNU unique symbols", 0),
  OPT("no-gnu-unique", 0, A0, NULL, "ld", "Do not use GNU unique symbols", 0),
  OPT("keep-memory", 0, A0, NULL, "ld", "Keep input file data in memory", 0),
  OPT("no-keep-memory", 0, A0, NULL, "ld", "Release input data early", 0),
  OPT("noinhibit-exec", 0, A0, NULL, "ld", "Write output despite errors", 0),
  OPT("omagic", 'N', A0, NULL, "ld", "Set OMAGIC output", 0),
  OPT("nmagic", 'n', A0, NULL, "ld", "Set NMAGIC output", 0),
  OPT("no-omagic", 0, A0, NULL, "ld", "Clear OMAGIC output", 0),
  OPT("reduce-memory-overheads", 0, A0, NULL, "ld", "Reduce memory use", 0),
  OPT("relax", 0, A0, NULL, "ld", "Relax address computations", 0),
  OPT("no-relax", 0, A0, NULL, "ld", "Disable relaxation", 0),
  OPT("rosegment", 0, A0, NULL, "ld", "Generate a read-only segment", 0),
  OPT("no-rosegment", 0, A0, NULL, "ld", "Merge read-only data into text segment", 0),
  OPT("sort-common", 0, A0, NULL, "ld", "Sort common symbols by alignment", 0),
  OPT("no-sort-common", 0, A0, NULL, "ld", "Do not sort common symbols", 0),
  OPT("sort-section", 0, A1, "MODE", "ld", "Sort sections by name or alignment", 0),
  OPT("spare-dynamic-tags", 0, A1, "N", "ld", "Reserve dynamic tags", 0),
  OPT("threads", 0, A0, NULL, "ld", "Use threads", 0),
  OPT("no-threads", 0, A0, NULL, "ld", "Do not use threads", 0),
  OPT("unique", 0, A1, "N", "ld", "Set the unique symbol search size", 0),
  OPT("warn-common", 0, A0, NULL, "ld", "Warn about common symbols", 0),
  OPT("no-warn-common", 0, A0, NULL, "ld", "Do not warn about common symbols", 0),
  OPT("warn-constructors", 0, A0, NULL, "ld", "Warn about constructors", 0),
  OPT("no-warn-constructors", 0, A0, NULL, "ld", "Do not warn about constructors", 0),
  OPT("warn-execstack", 0, A0, NULL, "ld", "Warn about executable stacks", 0),
  OPT("no-warn-execstack", 0, A0, NULL, "ld", "Do not warn about executable stacks", 0),
  OPT("warn-execstack-objects", 0, A0, NULL, "ld", "Warn on executable-stack objects", 0),
  OPT("warn-multiple-gp", 0, A0, NULL, "ld", "Warn about multiple GP values", 0),
  OPT("warn-rwx-segments", 0, A0, NULL, "ld", "Warn about RWX segments", 0),
  OPT("no-warn-rwx-segments", 0, A0, NULL, "ld", "Do not warn about RWX segments", 0),
  OPT("warn-section-align", 0, A0, NULL, "ld", "Warn when section alignment changes", 0),
  OPT("warn-shared-textrel", 0, A0, NULL, "ld", "Warn about shared text relocations", 0),
  OPT("no-warn-shared-textrel", 0, A0, NULL, "ld", "Do not warn about shared text relocations", 0),
  OPT("warn-unresolved-symbols", 0, A0, NULL, "ld", "Warn about unresolved symbols", 0),
  OPT("no-warn-unresolved-symbols", 0, A0, NULL, "ld", "Do not warn about unresolved symbols", 0),
  OPT("no-warnings", 0, A0, NULL, "ld", "Suppress all warnings", 0),
  OPT("default-imported-symver", 0, A0, NULL, "ld", "Use default imported symbol versions", 0),
  OPT("no-default-imported-symver", 0, A0, NULL, "ld", "Do not use default imported versions", 0),
  OPT("no-undefined-version", 0, A0, NULL, "ld", "Reject undefined symbol versions", 0),
  OPT("undefined-version", 0, A0, NULL, "ld", "Allow undefined symbol versions", 0),
  OPT("orphan-handling", 0, A1, "MODE", "ld", "Control orphan section placement", 0),
  OPT("package-metadata", 0, A1, "JSON", "ld", "Embed package metadata", 0),
  OPT("pop-state", 0, A0, NULL, "ld", "Pop linker option state", 0),
  OPT("push-state", 0, A0, NULL, "ld", "Push linker option state", 0),
  OPT("require-defined", 0, A1, "SYM", "ld", "Require SYM to be defined", 0),
  OPT("retain-symbols-file", 0, A1, "FILE", "ld", "Keep only listed symbols", 0),
  OPT("script", 'T', A1, "FILE", "ld", "Read a linker script (not implemented)", -1),
  OPT("sysroot", 0, A1, "DIR", "ld", "Set the system root", 0),
  OPT("library-path", 'L', A1, "DIR", "ld", "Add a library search path", -1),
  OPT("library", 'l', A1, "LIB", "ld", "Link a library (disabled)", -1),
  OPT("just-symbols", 'R', A1, "FILE", "ld", "Read only symbols from FILE", -1),
  OPT("mri-script", 0, A1, "FILE", "ld", "Read an MRI script (not implemented)", -1),
  OPT("plugin", 0, A1, "FILE", "ld", "Load a linker plugin (not implemented)", -1),
  OPT("plugin-opt", 0, A1, "OPT", "ld", "Pass an option to a plugin", -1),
  OPT("wrap", 0, A1, "SYM", "ld", "Wrap symbol references (not implemented)", -1),
  OPT("exclude-libs", 0, A1, "LIBS", "ld", "Exclude symbols from libraries", 0),
  OPT("filter", 'F', A1, "NAME", "ld", "Set DT_FILTER", 0),
  OPT("no-filter", 0, A0, NULL, "ld", "Clear DT_FILTER", 0),
  OPT("auxiliary", 'f', A1, "NAME", "ld", "Set DT_AUXILIARY", 0),
  OPT("audit", 0, A1, "NAME", "ld", "Set DT_AUDIT", 0),
  OPT("architecture", 'A', A1, "ARCH", "ld", "Set the input architecture", -1),
  OPT("oformat", 0, A1, "FMT", "ld", "Set the output format", 1),
  OPT("section-start", 0, A1, "SEC=ADDR", "ld", "Set a section address", 1),
  OPT("shared", 0, A0, NULL, "ld", "Produce ET_DYN", 1),
  OPT("static", 0, A0, NULL, "ld", "Produce a static ELF", 1),
  OPT("pie", 0, A0, NULL, "ld", "Produce a PIE", 1),

  /* ---- linker short aliases -------------------------------------------- */
  OPT(NULL, 'o', A1, "FILE", "ld", "Output file", 1),
  OPT(NULL, 'e', A1, "SYM", "ld", "Entry symbol", 1),
  OPT(NULL, 'r', A0, NULL, "ld", "Relocatable output", 1),
  OPT(NULL, 's', A0, NULL, "ld", "Strip symbols", 1),
  OPT(NULL, 'S', A0, NULL, "ld", "Strip debug info", 1),
  OPT(NULL, 'M', A0, NULL, "ld", "Print map", 1),
  OPT(NULL, 't', A0, NULL, "ld", "Trace inputs", 1),
  OPT(NULL, 'u', A1, "SYM", "ld", "Force undefined symbol", 1),
  OPT(NULL, 'z', A1, "KEYWORD", "ld", "Set linker keyword", 1),
  OPT(NULL, 'T', A1, "FILE", "ld", "Linker script (not implemented)", -1),
  OPT(NULL, 'L', A1, "DIR", "ld", "Library path (disabled)", -1),
  OPT(NULL, 'l', A1, "LIB", "ld", "Library (disabled)", -1),
  OPT(NULL, 'B', A1, "MODE", "ld", "Static/dynamic mode", 0),
  OPT(NULL, 'c', A0, NULL, "ld", "Compatibility alias", 0),
  OPT(NULL, 'E', A0, NULL, "ld", "Export dynamic symbols", 0),
  OPT(NULL, 'g', A0, NULL, "ld", "Ignored (compatibility)", 0),
  OPT(NULL, 'i', A0, NULL, "ld", "Incremental link (not implemented)", -1),
  OPT(NULL, 'm', A1, "EMU", "ld", "Emulation target", 0),
  OPT(NULL, 'N', A0, NULL, "ld", "OMAGIC output", 0),
  OPT(NULL, 'n', A0, NULL, "ld", "NMAGIC output", 0),
  OPT(NULL, 'O', A1, "LEVEL", "ld", "Optimisation level", 0),
  OPT(NULL, 'p', A0, NULL, "ld", "Compatibility alias", 0),
  OPT(NULL, 'P', A0, NULL, "ld", "Compatibility alias", 0),
  OPT(NULL, 'q', A0, NULL, "ld", "Keep relocations", 1),
  OPT(NULL, 'Q', A0, NULL, "ld", "Compatibility alias", 0),
  OPT(NULL, 'U', A1, "SYM", "ld", "Allow undefined symbol", 0),
  OPT(NULL, 'v', A0, NULL, "ld", "Verbose", 1),
  OPT(NULL, 'V', A0, NULL, "ld", "Version", 1),
  OPT(NULL, 'x', A0, NULL, "ld", "Discard locals", 0),
  OPT(NULL, 'X', A0, NULL, "ld", "Discard locals", 0),
  OPT(NULL, 'y', A1, "SYM", "ld", "Trace symbol", 0),
  OPT(NULL, 'Y', A1, "PATH", "ld", "Default search path", 0),

  /* ---- assembler: general ----------------------------------------------- */
  OPT("help", 'h', A0, NULL, "as", "Print assembler help", 1),
  OPT("version", 'V', A0, NULL, "as", "Print version information", 1),
  OPT("target-help", 0, A0, NULL, "as", "Print target-specific help", 1),
  OPT("verbose", 'v', A0, NULL, "as", "Trace assembler actions", 1),
  OPT("output", 'o', A1, "FILE", "as", "Write object to FILE", 1),
  OPT("target", 0, A1, "TRIPLE", "as", "Select the target triple", 1),
  OPT("march", 0, A1, "ARCH", "as", "Select the target architecture", 1),
  OPT("mcpu", 0, A1, "CPU", "as", "Select the target CPU", 1),
  OPT("mabi", 0, A1, "ABI", "as", "Select the ABI", 1),
  OPT("mfloat-abi", 0, A1, "ABI", "as", "Select the float ABI", 1),
  OPT("mfpu", 0, A1, "FPU", "as", "Select the FPU", 1),
  OPT("mthumb", 0, A0, NULL, "as", "Select Thumb mode", 1),
  OPT("marm", 0, A0, NULL, "as", "Select ARM mode", 1),
  OPT("m64", 0, A0, NULL, "as", "Select 64-bit mode", 1),
  OPT("m32", 0, A0, NULL, "as", "Select 32-bit mode", 1),
  OPT("64", 0, A0, NULL, "as", "Select 64-bit mode", 1),
  OPT("32", 0, A0, NULL, "as", "Select 32-bit mode", 1),
  OPT("g", 'g', A0, NULL, "as", "Generate debug information", 1),
  OPT("gen-debug", 0, A0, NULL, "as", "Generate debug information", 1),
  OPT("gdwarf-2", 0, A0, NULL, "as", "Generate DWARF-2 debug info", 1),
  OPT("gdwarf-3", 0, A0, NULL, "as", "Generate DWARF-3 debug info", 1),
  OPT("gdwarf-4", 0, A0, NULL, "as", "Generate DWARF-4 debug info", 1),
  OPT("gdwarf-5", 0, A0, NULL, "as", "Generate DWARF-5 debug info", 1),
  OPT("compress-debug-sections", 0, A2, "Z", "as", "Compress debug sections", 1),
  OPT("no-compress-debug-sections", 0, A0, NULL, "as", "Do not compress debug sections", 1),
  OPT("fatal-warnings", 0, A0, NULL, "as", "Treat warnings as errors", 1),
  OPT("no-fatal-warnings", 0, A0, NULL, "as", "Do not treat warnings as errors", 1),
  OPT("warn", 0, A0, NULL, "as", "Enable warnings", 1),
  OPT("no-warn", 'w', A0, NULL, "as", "Suppress warnings", 1),
  OPT("W", 'W', A0, NULL, "as", "Enable warnings", 1),
  OPT("I", 'I', A1, "DIR", "as", "Add an include path", 1),
  OPT("D", 'D', A1, "SYM=VAL", "as", "Define a symbol", 1),
  OPT("U", 'U', A1, "SYM", "as", "Undefine a symbol", 1),
  OPT("defsym", 0, A1, "SYM=VAL", "as", "Define a symbol", 1),
  OPT("listing", 0, A0, NULL, "as", "Generate a listing", 1),
  OPT("a", 'a', A2, "FLAGS", "as", "Listing control flags", 1),
  OPT("al", 0, A0, NULL, "as", "Include listing", 1),
  OPT("ah", 0, A0, NULL, "as", "Include high-level listing", 1),
  OPT("ac", 0, A0, NULL, "as", "Include cross-reference listing", 1),
  OPT("ad", 0, A0, NULL, "as", "Omit debug directives from listing", 1),
  OPT("an", 0, A0, NULL, "as", "Omit listing", 1),
  OPT("as", 0, A0, NULL, "as", "Include symbol listing", 1),
  OPT("statistics", 0, A0, NULL, "as", "Print assembly statistics", 1),
  OPT("stats", 0, A0, NULL, "as", "Print assembly statistics", 1),
  OPT("keep-locals", 'L', A0, NULL, "as", "Keep local symbols", 1),
  OPT("no-keep-locals", 0, A0, NULL, "as", "Discard local symbols", 1),
  OPT("strip-local-absolute", 0, A0, NULL, "as", "Strip local absolute symbols", 1),
  OPT("alternate", 0, A0, NULL, "as", "Use alternate macro syntax", 0),
  OPT("divide", 0, A0, NULL, "as", "Use slash as a division operator", 0),
  OPT("mri", 0, A0, NULL, "as", "Enable MRI compatibility", 0),
  OPT("itbl", 0, A1, "FILE", "as", "Use an instruction table", 0),
  OPT("size-check", 0, A1, "MODE", "as", "Relocation size check", 0),
  OPT("execstack", 0, A0, NULL, "as", "Mark the stack executable", 1),
  OPT("noexecstack", 0, A0, NULL, "as", "Mark the stack non-executable", 1),
  OPT("pad-sections", 0, A0, NULL, "as", "Pad sections to alignment", 0),
  OPT("no-pad-sections", 0, A0, NULL, "as", "Do not pad sections", 0),
  OPT("scfi", 0, A0, NULL, "as", "Generate SCFI directives", 0),
  OPT("no-scfi", 0, A0, NULL, "as", "Do not generate SCFI directives", 0),
  OPT("gsframe", 0, A0, NULL, "as", "Generate GNU SFrame info", 0),
  OPT("no-gsframe", 0, A0, NULL, "as", "Do not generate SFrame info", 0),
  OPT("sectname-subst", 0, A0, NULL, "as", "Substitute section name variables", 0),
  OPT("dump-config", 0, A0, NULL, "as", "Dump the assembler configuration", 1),
  OPT("J", 'J', A0, NULL, "as", "Do not warn about signed overflow", 0),
  OPT("K", 'K', A0, NULL, "as", "Warn about signed overflow", 0),
  OPT("R", 'R', A0, NULL, "as", "Fold data sections into text", 0),
  OPT("Z", 'Z', A0, NULL, "as", "Accept thin archives", 0),
  OPT("f", 'f', A0, NULL, "as", "Fast mode", 0),
  OPT("n", 'n', A0, NULL, "as", "Do not optimise branch relaxation", 0),
  OPT("N", 'N', A0, NULL, "as", "Do not optimise branch relaxation", 0),
  OPT("x", 'x', A0, NULL, "as", "Ignored compatibility flag", 0),
  OPT("X", 'X', A0, NULL, "as", "Ignored compatibility flag", 0),
  OPT("O", 'O', A1, "LEVEL", "as", "Assembler optimisation level", 0),
  OPT("listing-lhs-width", 0, A1, "N", "as", "Listing left-hand width", 0),
  OPT("listing-lhs-width2", 0, A1, "N", "as", "Listing continuation width", 0),
  OPT("listing-rhs-width", 0, A1, "N", "as", "Listing right-hand width", 0),
  OPT("listing-cont-lines", 0, A1, "N", "as", "Listing continuation lines", 0),
  OPT("traditional-format", 0, A0, NULL, "as", "Use traditional object format", 0),
  OPT("flag-mismatch-warnings", 0, A0, NULL, "as", "Warn about flag mismatches", 0),
  OPT("no-flag-mismatch-warnings", 0, A0, NULL, "as", "Do not warn about flag mismatches", 0),
};

const int wall_option_count = (int)(sizeof(wall_options) / sizeof(wall_options[0]));

static int tool_matches(const char *table_tool, const char *wanted) {
  if (!wanted || !wanted[0] || !strcmp(wanted, "all")) return 1;
  if (!strcmp(table_tool, wanted)) return 1;
  if (!strcmp(table_tool, "both")) return 1;
  return 0;
}

static void short_display(const WallOption *o, char *buf, size_t n) {
  if (o->long_name && o->short_name)
    snprintf(buf, n, "-%c, --%s", o->short_name, o->long_name);
  else if (o->long_name)
    snprintf(buf, n, "--%s", o->long_name);
  else
    snprintf(buf, n, "-%c", o->short_name);
  if (o->has_arg == A1 && o->arg_name)
    snprintf(buf + strlen(buf), n > strlen(buf) ? n - strlen(buf) : 0, " <%s>", o->arg_name);
  else if (o->has_arg == A2 && o->arg_name)
    snprintf(buf + strlen(buf), n > strlen(buf) ? n - strlen(buf) : 0, "[=<%s>]", o->arg_name);
}

void wall_print_options(FILE *f, const char *tool) {
  fprintf(f, "Wall toolchain option catalogue (%d options)\n", wall_option_count);
  const char *section = "";
  for (int i = 0; i < wall_option_count; i++) {
    const WallOption *o = &wall_options[i];
    if (!tool_matches(o->tool, tool)) continue;
    const char *sec = !strcmp(o->tool, "ld") ? "linker" : !strcmp(o->tool, "as") ? "assembler" : "both";
    if (strcmp(sec, section)) { section = sec; fprintf(f, "\n[%s]\n", section); }
    char disp[160];
    short_display(o, disp, sizeof(disp));
    const char *st = o->support == WALL_OPT_SUPPORTED ? "yes" :
                     o->support == WALL_OPT_COMPAT ? "compat" : "rejected";
    fprintf(f, "  %-34s %-8s %s\n", disp, st, o->desc);
  }
}

int wall_option_lookup(const char *tool, const char *arg) {
  if (!arg || arg[0] != '-') return -1;
  const char *name = arg + 1;
  if (*name == '-') name++;
  size_t len = strcspn(name, "=");
  char key[128];
  if (len >= sizeof(key)) return -1;
  memcpy(key, name, len);
  key[len] = 0;

  for (int i = 0; i < wall_option_count; i++) {
    const WallOption *o = &wall_options[i];
    if (!tool_matches(o->tool, tool)) continue;
    if (o->long_name && !strcmp(o->long_name, key)) return i;
    if (o->short_name && !strcmp(key, "") && arg[1] == o->short_name) return i;
    if (o->short_name && arg[0] == '-' && arg[1] == o->short_name && arg[1] != '-') {
      /* -O2, -Ifoo and friends. */
      if (o->has_arg == A1) return i;
      if (!arg[2]) return i;
    }
  }
  return -1;
}

int wall_option_known(const char *tool, const char *arg, int *support_out) {
  int i = wall_option_lookup(tool, arg);
  if (i < 0) {
    if (support_out) *support_out = WALL_OPT_REJECTED;
    return 0;
  }
  if (support_out) *support_out = wall_options[i].support;
  return 1;
}
