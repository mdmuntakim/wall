#!/bin/bash
# ctowa.sh - C to Wall (.wa) source translator.
#
# Usage:
#   tools/ctowa.sh <input.c> [-o output.wa] [--report report.txt]
#   tools/ctowa.sh --self-test
#
# Translates the C subset that the Wall compiler can express: functions,
# scalar locals and globals, assignment, compound assignment, ++/--,
# if/else, while, for, return, calls and arithmetic/comparisons.
# Constructs without a Wall equivalent (structs, arrays, pointers, switch,
# goto, preprocessor macros) are kept as comments so nothing is silently
# dropped.
#
# Exit status: 0 on success, 1 on usage/IO errors, 2 on self-test failure.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

usage() {
  sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
}

# ---------------------------------------------------------------------------
# The translator proper (POSIX awk).
# ---------------------------------------------------------------------------
translate() {
  awk '
  BEGIN {
    split("void char short int long float double bool _Bool signed unsigned struct enum union", _tw, " ")
    for (_i in _tw) TYPEWORD[_tw[_i]] = 1
    depth = 0; in_comment = 0; in_struct = 0; struct_depth = 0
  }

  function trim(s) { sub(/^[ \t\r]+/, "", s); sub(/[ \t\r]+$/, "", s); return s; }
  function ltrim(s) { sub(/^[ \t\r]+/, "", s); return s; }

  # Return the Wall spelling of a C type.
  function walltype(t,    x) {
    x = tolower(trim(t))
    gsub(/const|volatile|static|register|extern|inline|struct|enum|union|signed|unsigned/, " ", x)
    x = trim(x); gsub(/[ \t]+/, " ", x)
    if (index(x, "char") > 0) return "string"
    if (index(x, "double") > 0) return "double"
    if (index(x, "float") > 0) return "float"
    if (index(x, "bool") > 0) return "bool"
    if (index(x, "long") > 0) return "i64"
    return "i32"
  }

  function ascii_of(c) {
    return index(" !\"#$%&\047()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~", c) + 31
  }

  # Convert C character literals to their numeric byte value.  Wall has no
  # character type, so \047\n\047 becomes 10 and \047A\047 becomes 65.
  function conv_chars(s,    out, i, c, d, j, n, instr) {
    out = ""; n = length(s); instr = 0
    for (i = 1; i <= n; i++) {
      c = substr(s, i, 1)
      if (c == "\"") { instr = !instr; out = out c; continue }
      if (!instr && c == "\047") {
        j = i + 1
        if (substr(s, j, 1) == "\\") { d = substr(s, j + 1, 1); j += 2 }
        else { d = substr(s, j, 1); j += 1 }
        if (substr(s, j, 1) == "\047") {
          if (d == "n") d = 10
          else if (d == "t") d = 9
          else if (d == "r") d = 13
          else if (d == "0") d = 0
          else if (d == "a") d = 7
          else if (d == "b") d = 8
          else if (d == "f") d = 12
          else if (d == "v") d = 11
          else if (d == "\\") d = 92
          else if (d == "\047") d = 39
          else if (d == "\"") d = 34
          else d = ascii_of(d)
          out = out d
          i = j
          continue
        }
      }
      out = out c
    }
    return out
  }

  function strip_strings(s,    out, i, c, instr) {
    out = ""; instr = 0
    for (i = 1; i <= length(s); i++) {
      c = substr(s, i, 1)
      if (c == "\"") { instr = !instr; out = out c; continue }
      if (!instr) out = out c
    }
    return out
  }

  # Drop C-only expression noise that Wall does not have.
  function clean_expr(e) {
    e = conv_chars(e)
    gsub(/\([ \t]*(const[ \t]+)?(int|long|short|char|double|float|unsigned|signed)[ \t]*(\*)?\)/, "", e)
    gsub(/sizeof[ \t]*\([^)]*\)/, "0", e)
    gsub(/\bNULL\b/, "0", e)
    return trim(e)
  }

  function strip_quals(t) {
    while (t ~ /^(const|static|register|volatile|extern|inline|_Atomic|restrict)[ \t]+/) sub(/^[A-Za-z_]+[ \t]+/, "", t)
    return t
  }

  # Split s on separator `sep` only at nesting depth 0. Fills arr, returns count.
  function split_top(s, arr, sep,    n, i, c, d, cur) {
    n = 0; d = 0; cur = ""
    for (i = 1; i <= length(s); i++) {
      c = substr(s, i, 1)
      if (c == "(" || c == "[" || c == "{") d++
      else if (c == ")" || c == "]" || c == "}") d--
      if (c == sep && d == 0) { arr[++n] = cur; cur = "" }
      else cur = cur c
    }
    arr[++n] = cur
    return n
  }

  function ident_last(s,    n, i, c, out) {
    s = trim(s); n = length(s); out = ""
    for (i = n; i >= 1; i--) {
      c = substr(s, i, 1)
      if (c ~ /[A-Za-z0-9_]/) out = c out
      else break
    }
    return out
  }

  function paramsink(p,    n, arr, i, k, name, ty, out) {
    out = ""; p = trim(p)
    if (p == "" || p == "void") return out
    n = split_top(p, arr, ",")
    for (i = 1; i <= n; i++) {
      k = trim(arr[i])
      if (k == "" || k == "void") continue
      gsub(/\[[^]]*\]/, "", k)
      name = ident_last(k)
      ty = k
      sub(/[A-Za-z0-9_]+[ \t]*$/, "", ty)
      if (name == "") { name = "arg" i; ty = k }
      vartype[name] = walltype(ty)
      if (out != "") out = out ", "
      out = out name ": " walltype(ty)
    }
    return out
  }

  function is_func(line,    p, q, pre) {
    p = index(line, "("); if (p == 0) return 0
    q = index(line, ")"); if (q == 0) return 0
    pre = trim(substr(line, 1, p - 1))
    if (pre ~ /^(if|while|for|switch|return|else)[ \t]*$/) return 0
    if (pre !~ /[A-Za-z_][A-Za-z0-9_]*[ \t]*$/) return 0
    if (substr(line, q + 1) !~ /^[ \t]*\{/) return 0
    if (index(line, ";") > 0) return 0
    return 1
  }

  # Emit one or more Wall declarations from a C declaration (type already
  # stripped into ty, rest contains "name = init, name2 = init2").
  function emit_decl(ty, rest, is_global,    n, arr, j, d, nm, init, wt) {
    wt = walltype(ty)
    n = split_top(rest, arr, ",")
    for (j = 1; j <= n; j++) {
      d = trim(arr[j])
      if (d == "") continue
      init = ""
      if (index(d, "=") > 0) {
        nm = substr(d, 1, index(d, "=") - 1)
        init = substr(d, index(d, "=") + 1)
      } else nm = d
      nm = trim(nm)
      if (index(nm, "[") > 0) { print "// unsupported array: " nm; continue }
      gsub(/\*/, "", nm)
      if (nm !~ /^[A-Za-z_][A-Za-z0-9_]*$/) { print "// unsupported declarator: " d; continue }
      init = clean_expr(trim(init))
      if (init == "") init = "0"
      if (index(init, "->") > 0 || index(init, "[") > 0) {
        print "// unsupported initializer for " nm ": " init
        init = "0"
      }
      vartype[nm] = wt
      if (is_global) {
        if (wt == "string" || wt == "bool") {
          print "// unsupported global type: " nm
        } else {
          printf("global %s: %s = %s;\n", nm, wt, init)
        }
      } else {
        printf("let %s: %s = %s;\n", nm, wt, init)
      }
    }
  }

  # Parse a C declaration line "int x = 0, *p;" and emit Wall declarations.
  function parse_decl(t, is_global,    n, tok, i, w, ty, rest) {
    t = strip_quals(trim(t))
    sub(/[ \t]*;[ \t]*$/, "", t)
    n = split(t, tok, /[ \t]+/)
    ty = ""; rest = ""; i = 1
    while (i <= n) {
      w = tok[i]
      if (w ~ /^\*+$/) { ty = ty " *"; i++; continue }
      if (w ~ /^\*+[A-Za-z_]/) { ty = ty " *"; tok[i] = substr(w, index(w, substr(w, match(w, /[A-Za-z_]/), 1))); break }
      if (w in TYPEWORD) { ty = ty " " w; i++; continue }
      break
    }
    if (ty == "") return 0
    for (; i <= n; i++) rest = rest (rest == "" ? "" : " ") tok[i]
    if (rest == "") return 0
    emit_decl(ty, rest, is_global)
    return 1
  }

  function is_decl(t,    n, tok, i, w) {
    t = strip_quals(trim(t))
    n = split(t, tok, /[ \t]+/)
    if (n < 2) return 0
    for (i = 1; i <= n; i++) {
      w = tok[i]
      if (w ~ /^\*+$/) continue
      if (w ~ /^\*+[A-Za-z_]/) return 1
      if (w in TYPEWORD) return 1
      return 0
    }
    return 0
  }

  # Convert a for-loop step expression into a Wall statement.
  function step_stmt(s,    v, op, rhs) {
    s = trim(s)
    if (s == "") return ""
    if (s ~ /^\+\+[A-Za-z_][A-Za-z0-9_]*$/) { v = substr(s, 3); return v " = " v " + 1;" }
    if (s ~ /^--[A-Za-z_][A-Za-z0-9_]*$/) { v = substr(s, 3); return v " = " v " - 1;" }
    if (s ~ /^[A-Za-z_][A-Za-z0-9_]*\+\+$/) { v = substr(s, 1, length(s) - 2); return v " = " v " + 1;" }
    if (s ~ /^[A-Za-z_][A-Za-z0-9_]*--$/) { v = substr(s, 1, length(s) - 2); return v " = " v " - 1;" }
    if (s ~ /^[A-Za-z_][A-Za-z0-9_]*[ \t]*[+\-*\/%]=/) {
      v = substr(s, 1, index(s, "=") - 2); v = trim(v)
      op = substr(s, index(s, "=") - 1, 1)
      rhs = trim(substr(s, index(s, "=") + 1))
      return v " = " v " " op " (" clean_expr(rhs) ");"
    }
    s = clean_expr(s)
    return s ";"
  }

  # --- block comments -------------------------------------------------------
  {
    raw = $0
    line = raw

    if (in_comment) {
      if (index(line, "*/") > 0) { in_comment = 0; sub(/^.*\*\//, "", line) }
      else { c = trim(line); sub(/^\*+[ \t]?/, "", c); print "// " c; next }
    }
    while ((a = index(line, "/*")) > 0) {
      b = index(substr(line, a + 2), "*/")
      if (b > 0) {
        inner = substr(line, a + 2, b - 1)
        line = substr(line, 1, a - 1) "// " inner substr(line, a + 2 + b + 1)
      } else {
        print "// " trim(substr(line, a + 2))
        in_comment = 1
        line = substr(line, 1, a - 1)
        break
      }
    }
    if (in_comment) next

    # --- preprocessor -------------------------------------------------------
    if (ltrim(line) ~ /^#/) {
      d = ltrim(line)
      if (d ~ /^#define[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]+-?[0-9]+[ \t]*$/) {
        sub(/^#define[ \t]+/, "", d)
        name = d; sub(/[ \t].*$/, "", name)
        val = d; sub(/^[^ \t]+[ \t]+/, "", val)
        print "global " name ": i32 = " trim(val) ";"
      } else {
        print "// preprocessor: " trim(d)
      }
      next
    }

    # --- struct / enum / union / typedef bodies ----------------------------
    if (in_struct) {
      t1 = line; o = gsub(/\{/, "{", t1)
      t2 = line; c = gsub(/\}/, "}", t2)
      struct_depth += o - c
      print "// " trim(raw)
      if (struct_depth <= 0) in_struct = 0
      next
    }
    if (ltrim(line) ~ /^(typedef[ \t]+)?(struct|enum|union)([ \t]+[A-Za-z_][A-Za-z0-9_]*)?[ \t]*\{/) {
      t1 = line; o = gsub(/\{/, "{", t1)
      t2 = line; c = gsub(/\}/, "}", t2)
      struct_depth = o - c
      in_struct = (struct_depth > 0)
      print "// unsupported type declaration: " trim(raw)
      next
    }
    if (ltrim(line) ~ /^typedef[^;]*;[ \t]*$/) {
      print "// unsupported typedef: " trim(raw)
      next
    }

    t = trim(line)
    if (t == "") { print ""; next }

    # --- function definitions ----------------------------------------------
    if (is_func(t)) {
      p = index(t, "("); q = index(t, ")")
      pre = trim(substr(t, 1, p - 1))
      name = ident_last(pre)
      ret = pre; sub(/[A-Za-z0-9_]+[ \t]*$/, "", ret)
      params = substr(t, p + 1, q - p - 1)
      delete vartype
      printf("fn %s(%s) -> %s {\n", name, paramsink(params), walltype(ret))
      depth++
      next
    }
    if (t ~ /^[A-Za-z_][A-Za-z0-9_ \t\*]*\([^;]*\)[ \t]*;[ \t]*$/) {
      pre = trim(substr(t, 1, index(t, "(") - 1))
      if (pre ~ /[ \t]/ || (pre in TYPEWORD)) {
        print "// prototype: " t
        next
      }
    }

    # --- for loops -> while loops ------------------------------------------
    if (t ~ /^for[ \t]*\(/) {
      p = index(t, "("); q = 0; d = 0
      for (i = p; i <= length(t); i++) {
        c = substr(t, i, 1)
        if (c == "(") d++
        else if (c == ")") { d--; if (d == 0) { q = i; break } }
      }
      if (q == 0) { print "// unsupported for-loop: " t; next }
      header = substr(t, p + 1, q - p - 1)
      n = split_top(header, hp, ";")
      init = trim(hp[1]); cond = trim(hp[2]); step = trim(hp[3])
      if (init != "") {
        if (is_decl(init ";")) parse_decl(init ";", depth == 0)
        else print clean_expr(init) ";"
      }
      if (cond == "") cond = "true"
      print "while (" clean_expr(cond) ") {"
      depth++
      ss = step_stmt(step)
      if (ss != "") forstep[depth] = ss
      next
    }

    # --- closing braces: flush pending for-step ----------------------------
    if (t ~ /^\}/) {
      if (forstep[depth] != "") { print forstep[depth]; delete forstep[depth] }
    }

    # --- declarations -------------------------------------------------------
    if (is_decl(t) && t ~ /;[ \t]*$/) {
      parse_decl(t, depth == 0)
      next
    }

    # --- statement rewrites -------------------------------------------------
    s = t
    if (s ~ /^\+\+[A-Za-z_][A-Za-z0-9_]*;/) { v = substr(s, 3); sub(/;.*$/, "", v); s = v " = " v " + 1;" }
    else if (s ~ /^--[A-Za-z_][A-Za-z0-9_]*;/) { v = substr(s, 3); sub(/;.*$/, "", v); s = v " = " v " - 1;" }
    else if (s ~ /^[A-Za-z_][A-Za-z0-9_]*\+\+;/) { v = substr(s, 1, length(s) - 3); s = v " = " v " + 1;" }
    else if (s ~ /^[A-Za-z_][A-Za-z0-9_]*--;/) { v = substr(s, 1, length(s) - 3); s = v " = " v " - 1;" }
    else if (s ~ /^[A-Za-z_][A-Za-z0-9_]*[ \t]*[+\-*\/%]=/) {
      v = substr(s, 1, index(s, "=") - 2); v = trim(v)
      op = substr(s, index(s, "=") - 1, 1)
      rhs = trim(substr(s, index(s, "=") + 1)); sub(/;.*$/, "", rhs)
      s = v " = " v " " op " (" clean_expr(rhs) ");"
    }

    s = clean_expr(s)
    if (s ~ /^switch[ \t]*\(/) { print "// unsupported switch: " t; next }
    if (s ~ /^(case|default)[ \t:]/) { print "// unsupported case: " t; next }
    if (s ~ /^goto[ \t]/) { print "// unsupported goto: " t; next }
    if (s ~ /^(break|continue)[ \t]*;/) { print "// unsupported " t; next }
    if (index(s, "->") > 0) { print "// unsupported member access: " t; next }
    if (s ~ /^[A-Za-z_][A-Za-z0-9_]*:[ \t]*$/) { print "// unsupported label: " t; next }
    code = strip_strings(s)
    # Never emit a C-only line as Wall code; keep it as a comment instead.
    if (s ~ /^(static|extern|typedef|inline|register|volatile|const)[ \t]/ ||
        s ~ /^(unsigned|signed|void|char|short|int|long|float|double|bool|_Bool|struct|union|enum)[ \t]/ ||
        s ~ /\\[ \t]*$/ ||
        code ~ /\[/ ||
        code ~ /[A-Za-z0-9_)\]]\.[A-Za-z_]/ ||
        code ~ /\?/ ||
        code ~ /(^|[^&])&([^&]|$)/ ||
        code ~ /(^|[^|])\|([^|]|$)/ ||
        code ~ /[~^%]/ ||
        code ~ /<</ || code ~ />>/ ||
        (s !~ /[;{}][ \t]*$/ && s !~ /^else([ \t]*\{)?$/)) {
      print "// unsupported: " s
      tb = s; o = gsub(/\{/, "{", tb)
      tb = s; c = gsub(/\}/, "}", tb)
      depth += o - c
      next
    }
    print s

    # --- brace depth bookkeeping -------------------------------------------
    tb = s; o = gsub(/\{/, "{", tb)
    tb = s; c = gsub(/\}/, "}", tb)
    depth += o - c
  }

  END {
    if (in_struct) print "// (end of skipped type declaration)"
  }
  ' "$1"
}

report() {
  awk '
  /^fn / { f++ }
  /^let / { l++ }
  /^global / { g++ }
  /^while / { w++ }
  /^\/\/ unsupported/ { u++ }
  /^\/\/ preprocessor/ { p++ }
  END {
    printf("functions: %d\n", f + 0)
    printf("locals:    %d\n", l + 0)
    printf("globals:   %d\n", g + 0)
    printf("while:     %d\n", w + 0)
    printf("skipped:   %d\n", u + 0)
    printf("cpp lines: %d\n", p + 0)
  }' "$1"
}

# ---------------------------------------------------------------------------
# Self-test: translate, compile, link and run a small program.
# ---------------------------------------------------------------------------
self_test() {
  dir="$ROOT/.ctowa_selftest"
  rm -rf "$dir"
  mkdir -p "$dir"
  cat > "$dir/sample.c" <<'EOF'
int add(int a, int b) {
  return a + b;
}
int mul(int a, int b) {
  int r = 0;
  int i = 0;
  while (i < b) {
    r = r + a;
    i = i + 1;
  }
  return r;
}
int main() {
  int x = 10;
  int y = 20;
  int z = add(x, y);
  int w = mul(x, y);
  int s = 0;
  int i;
  for (i = 0; i < 5; i++) {
    s += i;
  }
  printf("%d %d %d\n", z, w, s);
  return 0;
}
EOF
  for t in wall obasm linker; do
    if [ ! -x "$ROOT/$t" ]; then
      echo "ctowa: self-test: missing $ROOT/$t (run ./build.sh)" >&2
      return 2
    fi
  done
  if ! bash "$0" "$dir/sample.c" -o "$dir/sample.wa"; then
    echo "ctowa: self-test: translation failed" >&2
    return 2
  fi
  if ! grep -q '^fn add(a: i32, b: i32) -> i32 {' "$dir/sample.wa"; then
    echo "ctowa: self-test: bad function translation" >&2
    sed -n '1,60p' "$dir/sample.wa" >&2
    return 2
  fi
  ( cd "$dir" && "$ROOT/wall" sample.wa ) || { echo "ctowa: self-test: wall failed" >&2; return 2; }
  s=$(ls -t "$dir"/tmp_*.s 2>/dev/null | head -1)
  [ -n "$s" ] || { echo "ctowa: self-test: no .s produced" >&2; return 2; }
  "$ROOT/obasm" "$s" -o "$dir/sample.o" || { echo "ctowa: self-test: obasm failed" >&2; return 2; }
  "$ROOT/linker" "$dir/sample.o" -o "$dir/sample_bin" || { echo "ctowa: self-test: linker failed" >&2; return 2; }
  out=$("$dir/sample_bin" 2>&1 | tr -d '\r')
  echo "ctowa self-test output: $out"
  if [ "$out" != "30 200 10" ]; then
    echo "ctowa: self-test: unexpected output (want '30 200 10')" >&2
    return 2
  fi
  echo "ctowa: self-test: PASS"
  return 0
}

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
  self_test
  exit $?
fi

in=""
out=""
rep=""
while [ $# -gt 0 ]; do
  case "$1" in
    -o) shift; out="${1:-}" ;;
    --report) shift; rep="${1:-}" ;;
    -h|--help) usage; exit 0 ;;
    -*) echo "ctowa: error: unknown option '$1'" >&2; exit 1 ;;
    *) in="$1" ;;
  esac
  shift
done

if [ -z "$in" ]; then
  echo "ctowa: error: no input file" >&2
  usage >&2
  exit 1
fi
if [ ! -f "$in" ]; then
  echo "ctowa: error: cannot open '$in'" >&2
  exit 1
fi
if [ -z "$out" ]; then
  out="${in%.*}.wa"
  [ "$out" = "$in" ] && out="$in.wa"
fi

{
  echo "// Generated from $in by tools/ctowa.sh"
  echo "// C -> Wall source transformation."
  echo ""
  translate "$in"
} > "$out" || { echo "ctowa: error: translation failed" >&2; exit 1; }

if [ -n "$rep" ]; then
  report "$out" > "$rep"
fi
echo "ctowa: wrote $out"
