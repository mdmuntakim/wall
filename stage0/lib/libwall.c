/* libwall - Wall compiler backend library.
 *
 * Implements the Wall compiler frontend (lexer, parser, LLVM IR generator)
 * and exposes it to the `wall` driver through the C ABI declared in wall.h.
 *
 * The backend is pluggable: the `wall` driver dlopen()s a backend shared
 * library exposing `wall_version` and `wall_compile`.
 */
#include "wall.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

const char *wall_version_string(void) { return "Wall/1.0.0"; }

int wall_version(char *buf, int buflen) {
  snprintf(buf, (size_t)buflen, "wallc version 1.0.0");
  return 0;
}

/* ---------------- growable buffer ---------------- */
typedef struct { char *data; size_t len, cap; } Buf;
static void buf_init(Buf *b) { b->data = NULL; b->len = 0; b->cap = 0; }
static void buf_reserve(Buf *b, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + n + 1) nc *= 2;
    b->data = (char *)realloc(b->data, nc);
    if (!b->data) { fprintf(stderr, "wall: error: out of memory\n"); exit(1); }
    b->cap = nc;
  }
}
static void buf_puts(Buf *b, const char *s) {
  size_t n = strlen(s);
  buf_reserve(b, n);
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = 0;
}
static void buf_putf(Buf *b, const char *fmt, ...) {
  char tmp[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  buf_puts(b, tmp);
}

/* ---------------- helpers ---------------- */
static char *xstrdup(const char *s) {
  char *p = strdup(s ? s : "");
  if (!p) { fprintf(stderr, "wall: error: out of memory\n"); exit(1); }
  return p;
}
static char *xstrfmt(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return xstrdup(buf);
}

/* ---------------- types ---------------- */
enum { T_I32 = 1, T_I64 = 2, T_F32 = 3, T_F64 = 4, T_BOOL = 5, T_STR = 6 };
static const char *const TYPE_NAMES[] = { "", "i32", "i64", "float", "double", "bool", "string" };
static const char *llvm_ty(int t) { if (t == T_BOOL) return "i1"; if (t == T_STR) return "ptr"; return TYPE_NAMES[t]; }

/* ---------------- tokens ---------------- */
enum { TK_EOF = 0, TK_IDENT, TK_INT, TK_STRING, TK_PUNCT };
typedef struct { int type; int line, col; char *text; long long num; char *str; int slen; } Token;
typedef struct { const char *src; size_t pos; int line, col; Token *t; int n, cap; int had_err; char err[512]; } Lexer;

static void tok_push(Lexer *L, Token t) {
  if (L->n == L->cap) { L->cap = L->cap ? L->cap * 2 : 64; L->t = (Token *)realloc(L->t, (size_t)L->cap * sizeof(Token)); }
  L->t[L->n++] = t;
}

static void lex(Lexer *L) {
  for (;;) {
    char c = L->src[L->pos];
    if (c == 0) { Token t; memset(&t, 0, sizeof(t)); t.type = TK_EOF; t.line = L->line; t.col = L->col; tok_push(L, t); break; }
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { if (c=='\n'){L->line++; L->col=1;} else L->col++; L->pos++; continue; }
    Token t; memset(&t, 0, sizeof(t)); t.line = L->line; t.col = L->col;
    if (isalpha((unsigned char)c) || c == '_') {
      size_t st = L->pos;
      while (isalnum((unsigned char)L->src[L->pos]) || L->src[L->pos] == '_') { if (L->src[L->pos]=='\n'){L->line++; L->col=1;} else L->col++; L->pos++; }
      char *txt = (char *)malloc((L->pos - st) + 1); memcpy(txt, L->src + st, L->pos - st); txt[L->pos - st] = 0;
      t.type = TK_IDENT; t.text = txt; tok_push(L, t); continue;
    }
    if (isdigit((unsigned char)c)) {
      size_t st = L->pos;
      while (isdigit((unsigned char)L->src[L->pos])) { if (L->src[L->pos]=='\n'){L->line++; L->col=1;} else L->col++; L->pos++; }
      char tmp[64]; size_t len = (L->pos - st); if (len > 63) len = 63; memcpy(tmp, L->src + st, len); tmp[len] = 0;
      t.type = TK_INT; t.num = strtoll(tmp, NULL, 10); t.text = xstrdup(tmp); tok_push(L, t); continue;
    }
    if (c == '"') {
      L->pos++; L->col++;
      Buf b; buf_init(&b);
      while (L->src[L->pos] && L->src[L->pos] != '"') {
        char ch = L->src[L->pos];
        if (ch == '\n') { L->line++; L->col = 1; }
        else L->col++;
        L->pos++;
        char ev = 0;
        if (ch == '\\' && L->src[L->pos]) {
          char e = L->src[L->pos]; L->pos++; if (e=='\n'){L->line++; L->col=1;} else L->col++;
          if (e=='n') ev='\n'; else if (e=='t') ev='\t'; else if (e=='r') ev='\r'; else if (e=='0') ev=0; else if (e=='x') { char hx[3]={L->src[L->pos],L->src[L->pos+1],0}; L->pos+=2; if(!isxdigit((unsigned char)hx[0])||!isxdigit((unsigned char)hx[1])) break; ev=(char)strtol(hx,NULL,16); }
          ch = ev;
        }
        if (b.len + 2 > b.cap) { b.cap = b.cap? b.cap*2 : 32; b.data = (char*)realloc(b.data, b.cap); }
        b.data[b.len++] = ch;
      }
      if (L->src[L->pos] == '"') { L->pos++; L->col++; }
      t.type = TK_STRING; t.text = b.data; t.slen = (int)b.len; if (b.len + 1 <= b.cap) { b.data[b.len] = 0; } tok_push(L, t); continue;
    }
    {
      char c1 = L->src[L->pos];
      const char *two = NULL;
      if ((c1=='-'&&L->src[L->pos+1]=='>')||(c1=='&'&&L->src[L->pos+1]=='&')||(c1=='|'&&L->src[L->pos+1]=='|')||(c1=='='&&L->src[L->pos+1]=='=')||(c1=='!'&&L->src[L->pos+1]=='=')||(c1=='<'&&L->src[L->pos+1]=='=')||(c1=='>'&&L->src[L->pos+1]=='>')) two = L->src + L->pos;
      if (two) { char tb[3]; tb[0] = c1; tb[1] = L->src[L->pos + 1]; tb[2] = 0; t.type = TK_PUNCT; t.text = xstrdup(tb); tok_push(L, t); L->pos += 2; L->col += 2; continue; }
      if (strchr("+-*/<>=!&|^%~?:,(){}[];", c1)) { t.type = TK_PUNCT; t.text = xstrfmt("%c", c1); tok_push(L, t); L->pos++; L->col++; continue; }
      L->had_err = 1; snprintf(L->err, sizeof(L->err), "%d:%d: error: unexpected character '%c'", L->line, L->col, c); break;
    }
  }
}

/* ---------------- AST ---------------- */
typedef enum { ND_BLOCK, ND_LET, ND_IF, ND_WHILE, ND_FOR, ND_RETURN, ND_EXPR_STMT, ND_INT, ND_STR, ND_IDENT, ND_BIN, ND_CALL } NKind;
typedef struct Node Node;
struct Node {
  int kind; int line, col; int op; int ty; long long num; char *name;
  Node *a, *b, *c; Node **kids; int nkids, kidcap;
  char *str; int slen;
};
static Node *mk(int kind, int line, int col) {
  Node *n = (Node *)calloc(1, sizeof(Node));
  n->kind = kind; n->line = line; n->col = col; n->ty = T_I32; n->op = 0;
  n->kidcap = 4; n->kids = (Node **)calloc(4, sizeof(Node *)); n->nkids = 0;
  return n;
}
static void mkkid(Node *n, Node *k) { if (n->nkids == n->kidcap) { n->kidcap *= 2; n->kids = (Node **)realloc(n->kids, (size_t)n->kidcap * sizeof(Node *)); } n->kids[n->nkids++] = k; }

typedef struct { char *name; int ret; int nparams; int ptypes[8]; char pnames[8][32]; Node *body; } Func;
static Func *g_fns; static int g_nfn, g_fncap;
typedef struct { char name[32]; int ty; long long val; } Glob;
static Glob g_globs[256]; static int g_nglobs;

/* ---------------- parser ---------------- */
typedef struct { Lexer *L; int pos; char err[512]; int had_err; } Parser;
static Token *cur(Parser *p) { return &p->L->t[p->pos]; }
static void adv(Parser *p) { if (p->L->t[p->pos].type != TK_EOF) p->pos++; }
static int is_punct(Token *t, const char *s) { return t->type == TK_PUNCT && t->text && strcmp(t->text, s) == 0; }
static int is_kw(Token *t, const char *k) { return t->type == TK_IDENT && t->text && strcmp(t->text, k) == 0; }
static void perr(Parser *p, const char *fmt, ...) {
  va_list ap;
  if (!p->had_err) { va_start(ap, fmt); vsnprintf(p->err, sizeof(p->err), fmt, ap); va_end(ap); p->had_err = 1; }
}
static int accept_punct(Parser *p, const char *s) { if (is_punct(cur(p), s)) { adv(p); return 1; } return 0; }
static int accept_kw(Parser *p, const char *k) { if (is_kw(cur(p), k)) { adv(p); return 1; } return 0; }

static int parse_type(Parser *p, int *out) {
  Token *t = cur(p);
  if (t->type != TK_IDENT) { perr(p, "%d:%d: error: expected a type", t->line, t->col); return -1; }
  int ty = 0;
  if (strcmp(t->text, "i32") == 0) ty = T_I32;
  else if (strcmp(t->text, "i64") == 0) ty = T_I64;
  else if (strcmp(t->text, "float") == 0) ty = T_F32;
  else if (strcmp(t->text, "double") == 0) ty = T_F64;
  else if (strcmp(t->text, "bool") == 0) ty = T_BOOL;
  else if (strcmp(t->text, "string") == 0) ty = T_STR;
  else { perr(p, "%d:%d: error: unknown type '%s'", t->line, t->col, t->text); return -1; }
  adv(p);
  if (out) *out = ty;
  return 0;
}

static Node *parse_block(Parser *p);
static Node *parse_expr(Parser *p, int minprec);

static Node *parse_primary(Parser *p) {
  Token *t = cur(p);
  if (t->type == TK_INT) { Node *n = mk(ND_INT, t->line, t->col); n->num = t->num; n->ty = T_I32; adv(p); return n; }
  if (t->type == TK_STRING) { Node *n = mk(ND_STR, t->line, t->col); n->str = t->text; n->slen = t->slen; n->ty = T_STR; adv(p); return n; }
  if (t->type == TK_IDENT) {
    if (is_kw(t, "true") || is_kw(t, "false")) { Node *n = mk(ND_INT, t->line, t->col); n->num = (is_kw(t, "true")) ? 1 : 0; n->ty = T_BOOL; adv(p); return n; }
    char *nm = xstrdup(t->text);
    adv(p);
    if (is_punct(cur(p), "(")) {
      adv(p);
      Node *c = mk(ND_CALL, t->line, t->col);
      c->name = nm;
      if (!is_punct(cur(p), ")")) { for (;;) { Node *arg = parse_expr(p, 0); mkkid(c, arg); if (accept_punct(p, ",")) continue; break; } }
      if (!accept_punct(p, ")")) perr(p, "%d:%d: error: expected ')' in call to '%s'", cur(p)->line, cur(p)->col, nm);
      Node *n = mk(ND_CALL, t->line, t->col);
      n->name = nm;
      n->nkids = c->nkids; for (int i = 0; i < c->nkids; i++) n->kids[i] = c->kids[i];
      free(c);
      return n;
    }
    Node *n = mk(ND_IDENT, t->line, t->col);
    n->name = nm;
    return n;
  }
  if (is_punct(t, "(")) { adv(p); Node *e = parse_expr(p, 0); if (!accept_punct(p, ")")) perr(p, "%d:%d: error: expected ')'", cur(p)->line, cur(p)->col); return e; }
  perr(p, "%d:%d: error: expected an expression", t->line, t->col);
  return mk(ND_INT, t->line, t->col);
}

static Node *parse_unary(Parser *p) {
  Token *t = cur(p);
  if (is_punct(t, "-")) {
    adv(p);
    Node *c = parse_unary(p);
    Node *n = mk(ND_BIN, t->line, t->col);
    n->op = '-'; n->ty = c->ty;
    mkkid(n, c); mkkid(n, NULL);
    Node *zero = mk(ND_INT, t->line, t->col); zero->num = 0; zero->ty = c->ty;
    n->kids[1] = zero; n->nkids = 2;
    return n;
  }
  if (is_punct(t, "!")) {
    adv(p);
    Node *c = parse_unary(p);
    Node *n = mk(ND_BIN, t->line, t->col);
    n->op = '!'; n->ty = T_BOOL;
    mkkid(n, c); mkkid(n, NULL);
    Node *f = mk(ND_INT, t->line, t->col); f->num = 0; f->ty = T_BOOL;
    n->kids[1] = f; n->nkids = 2;
    return n;
  }
  return parse_primary(p);
}

static int tok_prec(Token *t) {
  if (t->type != TK_PUNCT || !t->text) return -1;
  if (strcmp(t->text, "||") == 0) return 1;
  if (strcmp(t->text, "&&") == 0) return 2;
  if (strcmp(t->text, "==") == 0) return 3;
  if (strcmp(t->text, "!=") == 0) return 3;
  if (strcmp(t->text, "<") == 0) return 4;
  if (strcmp(t->text, "<=") == 0) return 4;
  if (strcmp(t->text, ">") == 0) return 4;
  if (strcmp(t->text, ">=") == 0) return 4;
  if (strcmp(t->text, "+") == 0) return 5;
  if (strcmp(t->text, "-") == 0) return 5;
  if (strcmp(t->text, "*") == 0) return 6;
  if (strcmp(t->text, "/") == 0) return 6;
  return -1;
}
static int tok_op(Token *t) {
  if (strcmp(t->text, "||") == 0) return '|';
  if (strcmp(t->text, "&&") == 0) return '&';
  if (strcmp(t->text, "==") == 0) return '=';
  if (strcmp(t->text, "!=") == 0) return '!';
  if (strcmp(t->text, "<") == 0) return '<';
  if (strcmp(t->text, "<=") == 0) return 'L';
  if (strcmp(t->text, ">") == 0) return '>';
  if (strcmp(t->text, ">=") == 0) return 'G';
  if (strcmp(t->text, "+") == 0) return '+';
  if (strcmp(t->text, "-") == 0) return '-';
  if (strcmp(t->text, "*") == 0) return '*';
  if (strcmp(t->text, "/") == 0) return '/';
  return 0;
}

static Node *parse_expr(Parser *p, int minprec) {
  Node *lhs = parse_unary(p);
  for (;;) {
    Token *t = cur(p);
    int pr = tok_prec(t);
    if (pr < minprec) break;
    int op = tok_op(t);
    adv(p);
    Node *rhs = parse_expr(p, pr + 1);
    Node *n = mk(ND_BIN, lhs->line, lhs->col);
    n->op = op;
    if (op == '&' || op == '|' || op == '<' || op == 'L' || op == '>' || op == 'G' || op == '=' || op == '!') n->ty = T_BOOL;
    mkkid(n, lhs); mkkid(n, rhs);
    lhs = n;
  }
  return lhs;
}

static Node *parse_stmt(Parser *p) {
  Token *t = cur(p);
  if (t->type == TK_IDENT && t->text) {
    if (strcmp(t->text, "let") == 0 || strcmp(t->text, "const") == 0) {
      adv(p);
      Token *n2 = cur(p);
      if (n2->type != TK_IDENT) { perr(p, "%d:%d: error: expected a variable name after '%s'", n2->line, n2->col, t->text); return NULL; }
      char *vname = xstrdup(n2->text); adv(p);
      if (!accept_punct(p, ":")) { perr(p, "%d:%d: error: expected ':' in declaration of '%s'", cur(p)->line, cur(p)->col, vname); return NULL; }
      int ty = 0;
      if (parse_type(p, &ty) != 0) return NULL;
      if (!accept_punct(p, "=")) { perr(p, "%d:%d: error: expected '=' in declaration of '%s'", cur(p)->line, cur(p)->col, vname); return NULL; }
      Node *init = parse_expr(p, 0);
      if (!accept_punct(p, ";")) { perr(p, "%d:%d: error: expected ';'", cur(p)->line, cur(p)->col); return NULL; }
      Node *n = mk(ND_LET, t->line, t->col);
      n->name = vname; n->ty = ty; n->a = init;
      return n;
    }
    if (strcmp(t->text, "if") == 0) {
      adv(p);
      if (!accept_punct(p, "(")) { perr(p, "%d:%d: error: expected '(' after 'if'", cur(p)->line, cur(p)->col); return NULL; }
      Node *cond = parse_expr(p, 0);
      if (!accept_punct(p, ")")) { perr(p, "%d:%d: error: expected ')' after if condition", cur(p)->line, cur(p)->col); return NULL; }
      Node *thenb = parse_block(p);
      Node *els = NULL;
      if (accept_kw(p, "else")) els = parse_block(p);
      Node *n = mk(ND_IF, t->line, t->col);
      n->nkids = 3; n->kids[0] = cond; n->kids[1] = thenb; n->kids[2] = els;
      return n;
    }
    if (strcmp(t->text, "while") == 0) {
      adv(p);
      if (!accept_punct(p, "(")) { perr(p, "%d:%d: error: expected '(' after 'while'", cur(p)->line, cur(p)->col); return NULL; }
      Node *cond = parse_expr(p, 0);
      if (!accept_punct(p, ")")) { perr(p, "%d:%d: error: expected ')' after while condition", cur(p)->line, cur(p)->col); return NULL; }
      Node *body = parse_block(p);
      Node *n = mk(ND_WHILE, t->line, t->col);
      n->nkids = 2; n->kids[0] = cond; n->kids[1] = body;
      return n;
    }
    if (strcmp(t->text, "return") == 0) {
      adv(p);
      Node *e = NULL;
      if (!is_punct(cur(p), ";")) e = parse_expr(p, 0);
      if (!accept_punct(p, ";")) { perr(p, "%d:%d: error: expected ';' after return", cur(p)->line, cur(p)->col); return NULL; }
      Node *n = mk(ND_RETURN, t->line, t->col);
      n->a = e;
      return n;
    }
    if (strcmp(t->text, "for") == 0) {
      adv(p);
      if (!accept_punct(p, "(")) { perr(p, "%d:%d: error: expected '(' after 'for'", cur(p)->line, cur(p)->col); return NULL; }
      Node *init = NULL;
      if (!is_punct(cur(p), ";")) { init = parse_expr(p, 0); }
      if (!accept_punct(p, ";")) { perr(p, "%d:%d: error: expected ';' in for loop", cur(p)->line, cur(p)->col); return NULL; }
      Node *cond = NULL;
      if (!is_punct(cur(p), ";")) cond = parse_expr(p, 0);
      if (!accept_punct(p, ";")) { perr(p, "%d:%d: error: expected ';' in for loop", cur(p)->line, cur(p)->col); return NULL; }
      Node *step = NULL;
      if (!is_punct(cur(p), ")")) { step = parse_expr(p, 0); }
      if (!accept_punct(p, ")")) { perr(p, "%d:%d: error: expected ')' after for loop", cur(p)->line, cur(p)->col); return NULL; }
      Node *body = parse_block(p);
      Node *n = mk(ND_FOR, t->line, t->col);
      n->nkids = 4;
      n->kids[0] = init; n->kids[1] = cond; n->kids[2] = step; n->kids[3] = body;
      return n;
    }
  }
  Node *e = parse_expr(p, 0);
  if (!accept_punct(p, ";")) { perr(p, "%d:%d: error: expected ';'", cur(p)->line, cur(p)->col); return NULL; }
  Node *n = mk(ND_EXPR_STMT, t->line, t->col);
  n->a = e;
  return n;
}

static Node *parse_block(Parser *p) {
  if (!accept_punct(p, "{")) { perr(p, "%d:%d: error: expected '{'", cur(p)->line, cur(p)->col); return NULL; }
  Node *b = mk(ND_BLOCK, 0, 0);
  while (!is_punct(cur(p), "}") && cur(p)->type != TK_EOF) {
    Node *s = parse_stmt(p);
    if (!s) break;
    mkkid(b, s);
  }
  if (!accept_punct(p, "}")) { if (cur(p)->type != TK_EOF) perr(p, "%d:%d: error: expected '}'", cur(p)->line, cur(p)->col); }
  return b;
}

static void parse_program(Parser *p) {
  while (cur(p)->type != TK_EOF) {
    Token *t = cur(p);
    if (t->type == TK_IDENT && t->text && strcmp(t->text, "global") == 0) {
      adv(p);
      Token *n2 = cur(p);
      if (n2->type != TK_IDENT) { perr(p, "%d:%d: error: expected a global variable name", n2->line, n2->col); break; }
      Glob g; memset(&g, 0, sizeof(g));
      snprintf(g.name, sizeof(g.name), "%s", n2->text); adv(p);
      if (!accept_punct(p, ":")) { perr(p, "%d:%d: error: expected ':' in global '%s'", cur(p)->line, cur(p)->col, g.name); break; }
      if (parse_type(p, &g.ty) != 0) break;
      if (!accept_punct(p, "=")) { perr(p, "%d:%d: error: expected '=' in global '%s'", cur(p)->line, cur(p)->col, g.name); break; }
      Node *init = parse_expr(p, 0);
      if (!accept_punct(p, ";")) { perr(p, "%d:%d: error: expected ';'", cur(p)->line, cur(p)->col); break; }
      if (g.ty != T_I32 && g.ty != T_I64 && g.ty != T_F32 && g.ty != T_F64) { perr(p, "%d:%d: error: unsupported global type for '%s'", cur(p)->line, cur(p)->col, g.name); break; }
      g.val = (init->kind == ND_INT) ? init->num : 0;
      if (g_nglobs < 256) { g_globs[g_nglobs] = g; g_nglobs++; }
      continue;
    }
    if (t->type != TK_IDENT || !t->text || strcmp(t->text, "fn") != 0) {
      perr(p, "%d:%d: error: expected a function declaration 'fn'", t->line, t->col);
      break;
    }
    adv(p);
    Token *n2 = cur(p);
    if (n2->type != TK_IDENT) { perr(p, "%d:%d: error: expected a function name", n2->line, n2->col); break; }
    char *fname = xstrdup(n2->text); adv(p);
    if (!accept_punct(p, "(")) { perr(p, "%d:%d: error: expected '(' after function name", cur(p)->line, cur(p)->col); break; }
    Func f; memset(&f, 0, sizeof(f));
    f.name = fname; f.ret = T_I32; f.nparams = 0;
    if (is_punct(cur(p), ")")) { adv(p); }
    else {
      for (;;) {
        Token *pa = cur(p);
        if (pa->type != TK_IDENT) { perr(p, "%d:%d: error: expected parameter name", pa->line, pa->col); break; }
        char *pname = xstrdup(pa->text); adv(p);
        if (!accept_punct(p, ":")) { perr(p, "%d:%d: error: expected ':' in parameter '%s'", cur(p)->line, cur(p)->col, pname); break; }
        int pty = 0;
        if (parse_type(p, &pty) != 0) break;
        if (f.nparams < 8) { snprintf(f.pnames[f.nparams], sizeof(f.pnames[0]), "%s", pname); f.ptypes[f.nparams] = pty; }
        f.nparams++;
        if (accept_punct(p, ",")) continue;
        break;
      }
      if (!accept_punct(p, ")")) { perr(p, "%d:%d: error: expected ')'", cur(p)->line, cur(p)->col); break; }
    }
    if (!accept_punct(p, "->")) { perr(p, "%d:%d: error: expected '->' in function signature", cur(p)->line, cur(p)->col); break; }
    if (parse_type(p, &f.ret) != 0) break;
    Node *body = parse_block(p);
    f.body = body;
    if (g_nfn == g_fncap) { g_fncap = g_fncap ? g_fncap * 2 : 8; g_fns = (Func *)realloc(g_fns, (size_t)g_fncap * sizeof(Func)); }
    g_fns[g_nfn++] = f;
  }
}

/* ---------------- code generation ---------------- */
typedef struct { char reg[32]; int ty; } Val;
typedef struct { char line[2048]; } GStr;
static GStr g_strs[512];
static int g_nstrs;
typedef struct {
  Buf out; int tmpc, lblc, strc, varc; Func *cur; int cur_ret;
  char varnames[64][32]; char varregs[64][32]; int varty[64]; int nvars;
  int dead;
  char gnames[64][32]; int gntys[64]; int ngns;
  int loopc;
} Gen;

static void g_label(Gen *g, const char *l) { buf_putf(&g->out, "%s:\n", l); }

static Val gen_expr(Gen *g, Node *n) {
  Val r; memset(&r, 0, sizeof(r));
  switch (n->kind) {
  case ND_INT: {
    r.ty = n->ty;
    char r1[32];
    if (n->ty == T_BOOL) { snprintf(r1, sizeof(r1), "%%t%d", g->tmpc); g->tmpc++; buf_putf(&g->out, "  %s = icmp ne i32 %lld, 0\n", r1, n->num); snprintf(r.reg, sizeof(r.reg), "%s", r1); return r; }
    snprintf(r1, sizeof(r1), "%%t%d", g->tmpc); g->tmpc++;
    if (n->ty == T_F32 || n->ty == T_F64) buf_putf(&g->out, "  %s = fadd %s %lld.0, 0.0\n", r1, llvm_ty(n->ty), n->num);
    else buf_putf(&g->out, "  %s = add %s %lld, 0\n", r1, llvm_ty(n->ty), n->num);
    snprintf(r.reg, sizeof(r.reg), "%s", r1);
    return r;
  }
  case ND_STR: {
    char lbl[32]; snprintf(lbl, sizeof(lbl), "@.str%d", g->strc);
    int len = n->slen + 1;
    Buf g2; buf_init(&g2);
    buf_putf(&g2, "%s = private unnamed_addr constant [%d x i8] c\"", lbl, len);
    for (int i = 0; i < n->slen; i++) { unsigned char ch = (unsigned char)n->str[i]; char e[8]; if (ch == 0) strcpy(e, "\\00"); else if (ch == '\n') strcpy(e, "\\0A"); else if (ch == '\t') strcpy(e, "\\09"); else if (ch == '\r') strcpy(e, "\\0D"); else if (ch == '"') strcpy(e, "\\\""); else if (ch == '\\') strcpy(e, "\\\\"); else if (ch >= 32 && ch < 127) { e[0] = (char)ch; e[1] = 0; } else snprintf(e, sizeof(e), "\\%02X", ch); buf_puts(&g2, e); }
    { char nul[8]; strcpy(nul, "\\00"); buf_puts(&g2, nul); }
    buf_putf(&g2, "\", align 1\n");
    snprintf(g_strs[g_nstrs].line, sizeof(g_strs[0].line), "%s", g2.data);
    g_nstrs++;
    char r1[32]; snprintf(r1, sizeof(r1), "%%t%d", g->tmpc); g->tmpc++;
    buf_putf(&g->out, "  %s = getelementptr [%d x i8], ptr %s, i64 0, i64 0\n", r1, len, lbl);
    snprintf(r.reg, sizeof(r.reg), "%s", r1);
    r.ty = T_STR;
    g->strc++;
    return r;
  }
  case ND_IDENT: {
    if (g->cur) {
      for (int i = 0; i < g->cur->nparams; i++) {
        if (g->cur->nparams > 0 && g->cur->pnames[i][0] && strcmp(g->cur->pnames[i], n->name) == 0) {
          Val v; memset(&v, 0, sizeof(v)); snprintf(v.reg, sizeof(v.reg), "%%arg%d", i); v.ty = g->cur->ptypes[i];
          return v;
        }
      }
    }
    for (int j = 0; j < g->ngns; j++) {
      if (g->gntys[j] > 0 && strcmp(g->gnames[j], n->name) == 0) {
        char r1[32]; snprintf(r1, sizeof(r1), "%%t%d", g->tmpc); g->tmpc++;
        buf_putf(&g->out, "  %s = load %s, ptr @%s\n", r1, llvm_ty(g->gntys[j]), g->gnames[j]);
        Val v; memset(&v, 0, sizeof(v)); snprintf(v.reg, sizeof(v.reg), "%s", r1); v.ty = g->gntys[j];
        return v;
      }
    }
    for (int i = 0; i < g->nvars; i++) {
      if (g->varty[i] > 0 && strcmp(g->varnames[i], n->name) == 0) {
        char lr[32]; snprintf(lr, sizeof(lr), "%%t%d", g->tmpc); g->tmpc++;
        buf_putf(&g->out, "  %s = load %s, ptr %s\n", lr, llvm_ty(g->varty[i]), g->varregs[i]);
        Val v; memset(&v, 0, sizeof(v)); snprintf(v.reg, sizeof(v.reg), "%s", lr); v.ty = g->varty[i];
        return v;
      }
    }
    return r;
  }
  case ND_BIN: {
    Val a = gen_expr(g, n->kids[0]);
    Val b = gen_expr(g, n->kids[1]);
    Val r; r.ty = T_BOOL;
    char r1[32];
    if (n->op == '&' || n->op == '|') {
      snprintf(r1, sizeof(r1), "%%t%d", g->tmpc); g->tmpc++;
      buf_putf(&g->out, "  %s = %s i1 %s, %s\n", r1, (n->op == '&') ? "and" : "or", a.reg, b.reg);
      snprintf(r.reg, sizeof(r.reg), "%s", r1);
      r.ty = T_BOOL;
      return r;
    }
    int isfloat = (a.ty == T_F32 || a.ty == T_F64);
    if (n->op == '<' || n->op == 'L' || n->op == '>' || n->op == 'G' || n->op == '=' || n->op == '!') {
      const char *cc = isfloat ? ((n->op=='<')?"olt":(n->op=='L')?"ole":(n->op=='>')?"ogt":(n->op=='G')?"oge":(n->op=='=')?"oeq":"one")
                             : ((n->op=='<')?"slt":(n->op=='L')?"sle":(n->op=='>')?"sgt":(n->op=='G')?"sge":(n->op=='=')?"eq":"ne");
      const char *instr = isfloat ? "fcmp" : "icmp";
      const char *tv = isfloat ? ((n->ty == T_F32) ? "float" : "double") : "i32";
      snprintf(r1, sizeof(r1), "%%t%d", g->tmpc); g->tmpc++;
      buf_putf(&g->out, "  %s = %s %s %s %s, %s\n", r1, instr, cc, tv, a.reg, b.reg);
      snprintf(r.reg, sizeof(r.reg), "%s", r1);
      r.ty = T_BOOL;
      return r;
    }
    const char *instr = (n->op == '+') ? (isfloat ? "fadd" : "add") : (n->op == '-') ? (isfloat ? "fsub" : "sub") : (n->op == '*') ? (isfloat ? "fmul" : "mul") : (isfloat ? "fdiv" : "sdiv");
    const char *tv = isfloat ? ((n->ty == T_F32) ? "float" : "double") : llvm_ty(n->ty);
    snprintf(r1, sizeof(r1), "%%t%d", g->tmpc); g->tmpc++;
    buf_putf(&g->out, "  %s = %s %s %s, %s\n", r1, instr, tv, a.reg, b.reg);
    snprintf(r.reg, sizeof(r.reg), "%s", r1);
    r.ty = n->ty;
    return r;
  }
  case ND_CALL: {
    Val r; memset(&r, 0, sizeof(r)); r.ty = T_I32;
    if (n->name && strcmp(n->name, "printf") == 0) {
      Val f = gen_expr(g, n->kids[0]);
      char r1[32]; snprintf(r1, sizeof(r1), "%%t%d", g->tmpc); g->tmpc++;
      Buf c; buf_init(&c);
      buf_putf(&c, "  %s = call i32 (ptr, ...) @printf(ptr %s", r1, f.reg);
      for (int i = 1; i < n->nkids; i++) { Val v = gen_expr(g, n->kids[i]); char tmp[512]; snprintf(tmp, sizeof(tmp), ", %s %s", llvm_ty(v.ty), v.reg); buf_puts(&c, tmp); }
      buf_puts(&c, ")\n");
      buf_puts(&g->out, c.data);
      snprintf(r.reg, sizeof(r.reg), "%s", r1);
      return r;
    }
    int retty = T_I32;
    for (int i = 0; i < g_nfn; i++) { if (g_fns && i < g_nfn && g_fns[i].name && strcmp(g_fns[i].name, n->name) == 0) { retty = g_fns[i].ret; break; } }
    char r1[32]; snprintf(r1, sizeof(r1), "%%t%d", g->tmpc); g->tmpc++;
    Buf c; buf_init(&c);
    buf_putf(&c, "  %s = call %s @%s(", r1, llvm_ty(retty), n->name);
    for (int i = 0; i < n->nkids; i++) { Val v = gen_expr(g, n->kids[i]); char tmp[512]; snprintf(tmp, sizeof(tmp), "%s %s", llvm_ty(v.ty), v.reg); buf_puts(&c, tmp); if (i + 1 < n->nkids) buf_puts(&c, ", "); }
    buf_puts(&c, ")\n");
    buf_puts(&g->out, c.data);
    snprintf(r.reg, sizeof(r.reg), "%s", r1);
    r.ty = retty;
    return r;
  }
  default: return r;
  }
}

static void gen_stmts(Gen *g, Node *blk);

static void gen_stmt(Gen *g, Node *n) {
  switch (n->kind) {
  case ND_LET: {
    Val v = gen_expr(g, n->a);
    char vr[32];
    for (int i = 0; i < g->nvars; i++) { if (g->varty[i] > 0 && strcmp(g->varnames[i], n->name) == 0) { snprintf(vr, sizeof(vr), "%s", g->varregs[i]); break; } }
    if (!vr[0]) break;
    buf_putf(&g->out, "  store %s %s, ptr %s\n", llvm_ty(v.ty), v.reg, vr);
    break;
  }
  case ND_EXPR_STMT: { Val v = gen_expr(g, n->a); break; }
  case ND_RETURN: {
    Node lit; memset(&lit, 0, sizeof(lit)); lit.kind = ND_INT; lit.ty = g->cur_ret; lit.num = 0;
    Val v = gen_expr(g, n->a ? n->a : &lit);
    buf_putf(&g->out, "  ret %s %s\n", llvm_ty(v.ty), v.reg);
    g->dead = 1;
    break;
  }
  case ND_IF: {
    Val c = gen_expr(g, n->kids[0]);
    char lb[32], le[32], lf[32];
    snprintf(lb, sizeof(lb), "L%d", g->lblc); g->lblc++;
    snprintf(le, sizeof(le), "L%d", g->lblc); g->lblc++;
    snprintf(lf, sizeof(lf), "L%d", g->lblc); g->lblc++;
    buf_putf(&g->out, "  br i1 %s, label %s, label %s\n", c.reg, lb, le);
    g_label(g, lb);
    gen_stmts(g, n->kids[1]);
    buf_putf(&g->out, "  br label %s\n", lf);
    g_label(g, le);
    if (n->kids[2]) gen_stmts(g, n->kids[2]);
    buf_putf(&g->out, "  br label %s\n", lf);
    g_label(g, lf);
    g->dead = 0;
    break;
  }
  case ND_WHILE: {
    char lc[32], lb[32], le[32];
    snprintf(lc, sizeof(lc), "L%d", g->lblc); g->lblc++;
    snprintf(lb, sizeof(lb), "L%d", g->lblc); g->lblc++;
    snprintf(le, sizeof(le), "L%d", g->lblc); g->lblc++;
    buf_putf(&g->out, "  br label %s\n", lc);
    g_label(g, lc);
    Val c = gen_expr(g, n->kids[0]);
    buf_putf(&g->out, "  br i1 %s, label %s, label %s\n", c.reg, lb, le);
    g_label(g, lb);
    gen_stmts(g, n->kids[1]);
    buf_putf(&g->out, "  br label %s\n", lc);
    g_label(g, le);
    g->dead = 0;
    break;
  }
  case ND_FOR: {
    char vname[32]; snprintf(vname, sizeof(vname), "_loop%d", g->loopc);
    int varidx = -1;
    for (int j = 0; j < g->nvars; j++) { if (g->varty[j] > 0 && strcmp(g->varnames[j], vname) == 0) { varidx = j; break; } }
    if (varidx < 0) {
      varidx = g->nvars++;
      snprintf(g->varnames[varidx], sizeof(g->varnames[varidx]), "%s", vname);
      snprintf(g->varregs[varidx], sizeof(g->varregs[varidx]), "%%v%d", g->varc);
      g->varty[varidx] = T_I32;
      buf_putf(&g->out, "  %s = alloca i32, align 4\n", g->varregs[varidx]);
      g->varc++;
      g->loopc++;
    }
    if (n->kids[0]) { Val init = gen_expr(g, n->kids[0]); buf_putf(&g->out, "  store i32 %s, ptr %s\n", init.reg, g->varregs[varidx]); }
    char lc[32], lb[32], le[32];
    snprintf(lc, sizeof(lc), "L%d", g->lblc); g->lblc++;
    snprintf(lb, sizeof(lb), "L%d", g->lblc); g->lblc++;
    snprintf(le, sizeof(le), "L%d", g->lblc); g->lblc++;
    buf_putf(&g->out, "  br label %s\n", lc);
    g_label(g, lc);
    Val c;
    if (n->kids[1]) c = gen_expr(g, n->kids[1]); else { Node lit; memset(&lit, 0, sizeof(lit)); lit.kind = ND_INT; lit.ty = T_BOOL; lit.num = 1; c = gen_expr(g, &lit); }
    buf_putf(&g->out, "  br i1 %s, label %s, label %s\n", c.reg, lb, le);
    g_label(g, lb);
    gen_stmts(g, n->kids[3]);
    if (n->kids[2]) { Val st = gen_expr(g, n->kids[2]); buf_putf(&g->out, "  store i32 %s, ptr %s\n", st.reg, g->varregs[varidx]); }
    buf_putf(&g->out, "  br label %s\n", lc);
    g_label(g, le);
    g->dead = 0;
    break;
  }
  default: break;
  }
}

static void gen_stmts(Gen *g, Node *blk) {
  if (!blk) return;
  for (int i = 0; i < blk->nkids; i++) {
    Node *s = blk->kids[i];
    if (s->kind == ND_BLOCK) gen_stmts(g, s);
    else gen_stmt(g, s);
  }
}

/* walk: assign variable slots and emit allocas into the current block */
static void walk_allocs(Gen *g, Node *n, int *counter) {
  if (!n) return;
  switch (n->kind) {
  case ND_BLOCK:
    for (int i = 0; i < n->nkids; i++) walk_allocs(g, n->kids[i], counter);
    break;
  case ND_LET: {
    int idx = -1;
    for (int j = 0; j < g->nvars; j++) { if (g->varty[j] > 0 && strcmp(g->varnames[j], n->name) == 0) { idx = j; break; } }
    if (idx < 0) {
      idx = g->nvars++;
      snprintf(g->varnames[idx], sizeof(g->varnames[idx]), "%s", n->name);
      snprintf(g->varregs[idx], sizeof(g->varregs[idx]), "%%v%d", (*counter)++);
      g->varty[idx] = n->ty;
      int align = 4;
      if (n->ty == T_F32 || n->ty == T_F64) align = 8;
      if (n->ty == T_BOOL) align = 1;
      char vr[32]; snprintf(vr, sizeof(vr), "%s", g->varregs[idx]);
      buf_putf(&g->out, "  %s = alloca %s, align %d\n", vr, llvm_ty(n->ty), align);
    }
    break;
  }
  case ND_IF:
    walk_allocs(g, n->kids[1], counter);
    if (n->kids[2]) walk_allocs(g, n->kids[2], counter);
    break;
  case ND_WHILE:
    walk_allocs(g, n->kids[1], counter);
    break;
  case ND_FOR: {
    char vname[32]; snprintf(vname, sizeof(vname), "_loop%d", g->loopc);
    int idx = -1;
    for (int j = 0; j < g->nvars; j++) { if (g->varty[j] > 0 && strcmp(g->varnames[j], vname) == 0) { idx = j; break; } }
    if (idx < 0) {
      idx = g->nvars++;
      snprintf(g->varnames[idx], sizeof(g->varnames[idx]), "%s", vname);
      snprintf(g->varregs[idx], sizeof(g->varregs[idx]), "%%v%d", (*counter)++);
      g->varty[idx] = T_I32;
      buf_putf(&g->out, "  %s = alloca i32, align 4\n", g->varregs[idx]);
      g->loopc++;
    }
    walk_allocs(g, n->kids[3], counter);
    break;
  }
  default:
    break;
  }
}

static void gen_function(Gen *g, Func *f) {
  Buf *b = &g->out;
  g->cur = f;
  g->cur_ret = f->ret;
  buf_putf(b, "define %s @%s(", llvm_ty(f->ret), f->name);
  for (int i = 0; i < f->nparams; i++) { if (i) buf_puts(b, ", "); buf_putf(b, "%s %%arg%d", llvm_ty(f->ptypes[i]), i); }
  buf_puts(b, ") {\nentry:\n");
  g->dead = 0;
  walk_allocs(g, f->body, &g->tmpc);
  gen_stmts(g, f->body);
  if (!g->dead) {
    char z[64];
    if (f->ret == T_BOOL) strcpy(z, "false");
    else if (f->ret == T_F32 || f->ret == T_F64) strcpy(z, "0.0");
    else strcpy(z, "0");
    buf_putf(b, "  ret %s %s\n", llvm_ty(f->ret), z);
  }
  buf_puts(b, "}\n");
}

/* ---------------- module assembly ---------------- */
static void wall_compile_module(Buf *out) {
  Buf fbs[64]; int nfbs = 0;
  for (int i = 0; i < g_nfn; i++) {
    Gen g; memset(&g, 0, sizeof(g));
    for (int j = 0; j < g_nglobs && j < 64; j++) { snprintf(g.gnames[j], sizeof(g.gnames[j]), "%s", g_globs[j].name); g.gntys[j] = g_globs[j].ty; }
    g.ngns = g_nglobs < 64 ? g_nglobs : 64;
    Buf *fb = &fbs[nfbs++];
    memset(fb, 0, sizeof(Buf));
    gen_function(&g, &g_fns[i]);
    *fb = g.out;
  }
  buf_puts(out, "; ModuleID = 'wallc'\n");
  for (int i = 0; i < g_nstrs; i++) buf_puts(out, g_strs[i].line);
  for (int i = 0; i < g_nglobs; i++) {
    Buf gb; buf_init(&gb);
    if (g_globs[i].ty == T_F32 || g_globs[i].ty == T_F64) buf_putf(&gb, "@%s = global %s %lld.0\n", g_globs[i].name, llvm_ty(g_globs[i].ty), g_globs[i].val);
    else buf_putf(&gb, "@%s = global %s %lld\n", g_globs[i].name, llvm_ty(g_globs[i].ty), g_globs[i].val);
    buf_puts(out, gb.data);
  }
  buf_putf(out, "declare i32 @printf(ptr, ...)\n");
  for (int i = 0; i < nfbs; i++) buf_puts(out, fbs[i].data);
}

int wall_compile(const char *source, int optimize_level, char **out_ir, char **err) {
  *out_ir = NULL;
  *err = NULL;
  g_nfn = 0;
  g_nstrs = 0;
  g_nglobs = 0;
  {
    Lexer L; memset(&L, 0, sizeof(L));
    L.src = source; L.pos = 0; L.line = 1; L.col = 1;
    lex(&L);
    Parser P; memset(&P, 0, sizeof(P));
    P.L = &L; P.pos = 0;
    parse_program(&P);
    if (L.had_err || P.had_err) { *err = xstrdup(L.had_err ? L.err : P.err); return -1; }
    if (g_nfn == 0) { *err = xstrdup("1:1: error: no functions defined in module"); return -1; }
  }
  {
    Buf out; buf_init(&out);
    wall_compile_module(&out);
    *out_ir = xstrdup(out.data ? out.data : "");
  }
  return 0;
}