/* MIT licensed (both this program, and what it creates); see LICENSE.md */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_help(FILE* f, const char* self) {
#if defined(__arm__) || defined(__arm) || defined(__ARM__) || defined(__ARM) || defined(__aarch64__) || defined(_M_ARM64)
  const char* self_isa = "neon";
#else
  const char* self_isa = "sse";
#endif
  if (!self) self = "./generate";
  fprintf(f, "Usage: %s [OPTION]...\n", self);
  fprintf(f, "Generate C code for computing CRC32.\n");
  fprintf(f, "Example: %s -i %s -p crc32c -a v8s1_s2\n\n", self, self_isa);
  fprintf(f, "Options controlling code generation:\n");
  fprintf(f, "  -i, --isa=ISA\n");
  fprintf(f, "  -p, --polynomial=POLY\n");
  fprintf(f, "  -a, --algorithm=ALGO\n");
  fprintf(f, "\nOutput control:\n");
  fprintf(f, "  -o, --output=FILE\n");
  fprintf(f, "\nPossible values for ISA are:\n");
  fprintf(f, "  neon (aarch64, tuned for pmull+eor fusion)\n");
  fprintf(f, "  neon_eor3 (aarch64, using pmull and eor3)\n");
  fprintf(f, "  sse, avx, avx2 (x86_64, using pclmulqdq)\n");
  fprintf(f, "  avx512 (x86_64, using pclmulqdq and vpternlogq)\n");
  fprintf(f, "  avx512_vpclmulqdq (x86_64, using vpclmulqdq and vpternlogq)\n");
  fprintf(f, "\nPossible values for POLY include:\n");
  fprintf(f, "  crc32   (0x04C11DB7) - hardware accelerated on aarch64\n");
  fprintf(f, "  crc32c  (0x1EDC6F41) - hardware accelerated on aarch64 and x86_64\n");
  fprintf(f, "  crc32k  (0x741B8CD7)\n");
  fprintf(f, "  crc32k2 (0x32583499)\n");
  fprintf(f, "  crc32q  (0x814141AB)\n");
  fprintf(f, "  or specify any 32-bit polynomial in hexadecimal form\n");
  fprintf(f, "\nThe ALGO string consists of multiple phases, separated by underscores.\n");
  fprintf(f, "Each phase can contain (with no spaces inbetween) any mixture of:\n");
  fprintf(f, "  vN[xM] use N vector accumulators, and NxM vector loads per iteration\n");
  fprintf(f, "  sN[xM] use N scalar accumulators, and NxM scalar loads per iteration\n");
  fprintf(f, "  kN     use an outer loop over N bytes\n");
  fprintf(f, "  e      use an end pointer for the (inner) loop condition\n");
  fprintf(f, "\nSee https://github.com/corsix/fast-crc32/\n");
}

#define FATAL(fmt, ...) \
  (fprintf(stderr, "FATAL error at %s:%d - " fmt "\n", __FILE__, __LINE__, ## __VA_ARGS__), fflush(stderr), exit(1))
#define FATAL_ISA() FATAL("bad ISA in %s", __func__)

/*
** Little string buffer library.
**
** Main entry points are:
** - `sb = sbuf_new();`
** - `put_str(sb, str);`
** - `put_fmt(sb, fmt, ...);`
** - `put_deferred_sbuf(sb, sb2);`
** - `put_deferred_fn(sb, fn);`
*/

typedef struct sbuf_t {
  char* base;
  uint32_t size;
  uint32_t capacity;
} sbuf_t;

#define sbuf_new() (sbuf_t*)calloc(1, sizeof(sbuf_t))

static char* sbuf_emplace(sbuf_t* sb, uint32_t size) {
  /* Reserve `size` bytes, but do not commit them. */
  if (sb->size + size > sb->capacity) {
    uint32_t new_capacity = (sb->capacity + size) * 2;
    if (new_capacity < 32) new_capacity = 32;
    sb->base = (char*)realloc(sb->base, new_capacity);
    sb->capacity = new_capacity;
  }
  return sb->base + sb->size;
}

static char* sbuf_append(sbuf_t* sb, uint32_t size) {
  /* Reserve and commit `size` bytes (caller has to populate them). */
  char* result = sbuf_emplace(sb, size);
  sb->size += size;
  return result;
}

static void put_str_len(sbuf_t* sb, const char* str, size_t len) {
  memcpy(sbuf_append(sb, (uint32_t)len), str, len);
}

static void put_str(sbuf_t* sb, const char* str) {
  if (!str) str = "(null)";
  put_str_len(sb, str, strlen(str));
}

/* For compile-time string literals. */
#define put_lit(sb, str) (put_str_len((sb), (str), sizeof((str))-1))

typedef enum sbuf_op_t {
  SBUF_OP_END,
  SBUF_OP_DEFERRED,
  SBUF_OP_DEFERRED_FN
} sbuf_op_t;

static sbuf_t* put_deferred_sbuf(sbuf_t* sb, sbuf_t* x) {
  char* dst = sbuf_append(sb, 2 + sizeof(x));
  dst[0] = 0;
  dst[1] = SBUF_OP_DEFERRED;
  memcpy(dst + 2, &x, sizeof(x));
  return x;
}

#define put_new_sbuf(sb) (put_deferred_sbuf((sb), sbuf_new()))

typedef void (*sbuf_fn_t)(sbuf_t*);

static void put_deferred_fn(sbuf_t* sb, sbuf_fn_t x) {
  char* dst = sbuf_append(sb, 2 + sizeof(x));
  dst[0] = 0;
  dst[1] = SBUF_OP_DEFERRED_FN;
  memcpy(dst + 2, &x, sizeof(x));
}

static void put_u32(sbuf_t* sb, uint32_t x) {
  char tmp[17];
  char* dst = sbuf_emplace(sb, 10);
  char* itr = tmp + 10;
  do {
    *--itr = '0' + (x % 10u);
  } while ((x /= 10u));
  sb->size += (tmp + 10 - itr);
  memcpy(dst, itr, 8);
}

static void put_u32_hex(sbuf_t* sb, uint32_t x) {
  uint32_t i = 8;
  char* s = sbuf_append(sb, i);
  for (; i--; x >>= 4) {
    s[i] = "0123456789abcdef"[x & 15u];
  }
}

static void put_fmt(sbuf_t* sb, const char* fmt, ...) {
  va_list args;
  const char* base;
  char c;
  va_start(args, fmt);
  for (;;) {
    c = *fmt;
    if (c == '%') {
      c = fmt[1];
      fmt += 2;
      switch (c) {
        case 's': put_str(sb, va_arg(args, const char*)); break;
        case 'u': put_u32(sb, va_arg(args, uint32_t)); break;
        case 'x': put_u32_hex(sb, va_arg(args, uint32_t)); break;
        case '%': base = fmt - 1; goto lit;
        default: FATAL("bad format char %c", c);
      }
    } else if (c != '\0') {
      base = fmt;
    lit:
      do { ++fmt; } while (*fmt != '%' && *fmt != '\0');
      put_str_len(sb, base, (size_t)(fmt - base));
    } else {
      break;
    }
  }
  va_end(args);
}

static sbuf_t* g_out;
static sbuf_t* g_includes;

/* Other end of string buffers; write one out to file. */

typedef struct indent_state_t {
  uint16_t stack;
  uint8_t level;
  uint8_t state;
} indent_state_t;

static void write_indenting(FILE* f, const char* base, const char* end, indent_state_t* state) {
  const char* itr = base;
  while (itr != end) {
    char c = *itr++;
    /*
    ** state == 0 means nothing interesting has happened
    ** state == 1 means previous character was '{'
    ** state == 2 means previous character was '\n'
    ** state == 2 + N means N '}' characters have been observed and removed, and before them was '\n'
    */
    if (state->state >= 2) {
      if (c == '}') {
        /* Un-indent if matching '{' caused an indent. */
        state->level -= (state->stack & 1);
        state->stack >>= 1;
        /* Store the '}' in state rather than writing it out. */
        state->state += 1;
        if ((itr - base) > 1) fwrite(base, 1, itr - base - 1, f);
        base = itr;
        continue;
      }
      if (c != '\n' || state->state > 2) {
        /* Write out everything that came prior to c. */
        if ((itr - base) > 1) fwrite(base, 1, itr - base - 1, f);
        base = itr - 1;
        /* Write indent before writing c. */
        fwrite("                                ", 2, state->level, f);
        /* If any '}' were stored, write them out before writing c. */
        fwrite("}}}}}}}}}}}}}}}}", 1, state->state - 2, f);
      }
      state->state = 0;
    }
    switch (c) {
    case '{':
      if (state->stack & 0x8000) FATAL("nesting too deep");
      state->stack <<= 1;
      state->state = 1;
      break;
    case '\n':
      state->stack |= state->state;
      state->level += state->state;
      if (state->level > 16) FATAL("nesting too deep");
      state->state = 2;
      break;
    case '}':
      state->level -= (state->stack & 1);
      state->stack >>= 1;
      /* fallthrough */
    default:
      state->state = 0;
      break;
    }
  }
  if (itr != base) fwrite(base, 1, itr - base, f);
}

static void sbuf_set_end_marker(sbuf_t* b, sbuf_t* return_to, uint32_t return_idx) {
  char* dst = sbuf_emplace(b, 2 + sizeof(sbuf_t*) + sizeof(uint32_t));
  dst[0] = '\0';
  dst[1] = SBUF_OP_END;
  b->size = 0;
  memcpy(dst + 2, &return_to, sizeof(sbuf_t*));
  memcpy(dst + 2 + sizeof(sbuf_t*), &return_idx, sizeof(uint32_t));
}

static void flush_sbuf_to(sbuf_t* b, FILE* f) {
  indent_state_t indent_state = {0};
  char* itr;
  char c;
  sbuf_set_end_marker(b, NULL, 0);
  itr = b->base;
  for (;;) {
    char* start = itr;
    while ((c = *itr)) {
      ++itr;
    }
    if (itr != start) {
      write_indenting(f, start, itr, &indent_state);
    }
    c = itr[1];
    if (c == SBUF_OP_DEFERRED) {
      sbuf_t* b2;
      itr += 2;
      memcpy(&b2, itr, sizeof(b2));
      itr += sizeof(b2);
      if (b2->size) {
        sbuf_set_end_marker(b2, b, (uint32_t)(itr - b->base));
        itr = b2->base;
        b = b2;
      }
    } else if (c == SBUF_OP_DEFERRED_FN) {
      sbuf_fn_t fn;
      sbuf_t* tmp = sbuf_new();
      itr[1] = SBUF_OP_DEFERRED;
      memcpy(&fn, itr + 2, sizeof(fn));
      memcpy(itr + 2, &tmp, sizeof(sbuf_t*));
      fn(tmp);
    } else if (c == SBUF_OP_END) {
      b->size = itr - b->base;
      itr += 2;
      memcpy(&b, itr, sizeof(sbuf_t*));
      if (b) {
        uint32_t idx;
        memcpy(&idx, itr + sizeof(sbuf_t*), sizeof(uint32_t));
        itr = b->base + idx;
      } else {
        break;
      }
    } else {
      FATAL("bad sbuf op %u", (unsigned)(unsigned char)c);
    }
  }
  fflush(f);
}

/* Command line parsing. */

typedef enum isa_t {
  ISA_NONE,
  ISA_NEON,
  ISA_NEON_EOR3,
  ISA_SSE,
  ISA_AVX512,
  ISA_AVX512_VPCLMULQDQ
} isa_t;

#define REV_POLY_CRC32  0xedb88320
#define REV_POLY_CRC32C 0x82f63b78

typedef struct algo_phase_t {
  uint32_t v_acc;  /* Number of vector accumulators. */
  uint32_t v_load; /* Number of vector loads (must be multiple of v_acc). */
  uint32_t s_acc;  /* Number of scalar accumulators. */
  uint32_t s_load; /* Number of scalar loads (must be multiple of s_acc). */
  uint32_t kernel_size; /* Outer loop step size, or 0. */
  uint32_t use_end_ptr;
  struct algo_phase_t* next;
} algo_phase_t;

static isa_t g_isa = ISA_NONE;
static uint32_t g_poly = REV_POLY_CRC32;
static algo_phase_t* g_algo;
static const char* g_out_path;

typedef struct cli_arg_t {
  const char* const* spellings;
  const char* value;
} cli_arg_t;

static const char* match_spelling(const char* const* spellings, const char* str, size_t n) {
  const char* spelling;
  while ((spelling = *spellings++)) {
    if (strlen(spelling) == n && memcmp(spelling, str, n) == 0) {
      break;
    }
  }
  return spelling;
}

static cli_arg_t* match_arg(cli_arg_t** args, const char* str, size_t n) {
  cli_arg_t* arg;
  while ((arg = *args++)) {
    if (match_spelling(arg->spellings, str, n)) {
      break;
    }
  }
  return arg;
}

static isa_t parse_isa(const char* isa) {
  if (!strcmp(isa, "none")) return ISA_NONE;
  else if (!strcmp(isa, "neon")) return ISA_NEON;
  else if (!strcmp(isa, "neon_eor3")) return ISA_NEON_EOR3;
  else if (!strcmp(isa, "sse") || !strcmp(isa, "avx") || !strcmp(isa, "avx2")) return ISA_SSE;
  else if (!strcmp(isa, "avx512")) return ISA_AVX512;
  else if (!strcmp(isa, "avx512_vpclmulqdq")) return ISA_AVX512_VPCLMULQDQ;
  else FATAL("unknown ISA %s", isa);
}

static uint32_t rev32(uint32_t poly) {
  uint32_t lo = 1u, hi = 0x80000000u;
  do {
    uint32_t mask = lo + hi;
    uint32_t bits = poly & mask;
    if (bits != 0 && bits != mask) {
      poly ^= mask;
    }
    lo <<= 1;
    hi >>= 1;
  } while (lo < hi);
  return poly;
}

static uint32_t parse_poly(const char* value) {
  if (!strcmp(value, "crc32") || !strcmp(value, "CRC32")) return REV_POLY_CRC32;
  else if (!strcmp(value, "crc32c") || !strcmp(value, "CRC32C")) return REV_POLY_CRC32C;
  else if (!strcmp(value, "crc32k") || !strcmp(value, "CRC32K")) return 0xEB31D82E;
  else if (!strcmp(value, "crc32k2") || !strcmp(value, "CRC32K2")) return 0x992C1A4C;
  else if (!strcmp(value, "crc32q") || !strcmp(value, "CRC32Q")) return 0xD5828281;
  else {
    uint32_t poly = 0, i = 0;
    char c;
    if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X') && value[2]) {
      value += 2;
    }
    while ((c = value[i++])) {
      if ('0' <= c && c <= '9') c -= '0';
      else if ('a' <= c && c <= 'f') c = c - 'a' + 10;
      else if ('A' <= c && c <= 'F') c = c - 'A' + 10;
      else FATAL("invalid polynomial %s", value);
      if (i > (8 + (value[0] == '1'))) FATAL("polynomial %s too long", value);
      poly = (poly << 4) + (uint8_t)c;
    }
    if (i < 9) {
      FATAL("polynomial %s too short", value);
    }
    return rev32(poly);
  }
}

static algo_phase_t* parse_algo(const char* value) {
  algo_phase_t* first = (algo_phase_t*)calloc(1, sizeof(algo_phase_t));
  algo_phase_t* cur = first;
  uint32_t i = 0, n, x;
  char c, c2;
  while ((c = value[i++])) {
    if (c == 'v' || c == 's' || c == 'k') {
      c2 = value[i++];
      if (c2 < '0' || c2 > '9') {
        FATAL("expected digit sequence after character %c in algorithm string %s", c, value);
      }
      n = c2 - '0';
      for (; (c2 = value[i]), ('0' <= c2 && c2 <= '9'); ++i) {
        n = n * 10 + (c2 - '0');
      }
      x = 1;
      if (c2 == 'x' && c != 'k') {
        c2 = value[++i];
        if (c2 < '0' || c2 > '9') {
          FATAL("expected digit sequence after character x in algorithm string %s", value);
        }
        x = c2 - '0';
        for (; (c2 = value[++i]), ('0' <= c2 && c2 <= '9');) {
          x = x * 10 + (c2 - '0');
        }
      }
      if (c == 'v') {
        cur->v_load += n * x;
        if (cur->v_acc < n) cur->v_acc = n;
      } else if (c == 's') {
        cur->s_load += n * x;
        if (cur->s_acc < n) cur->s_acc = n;
      } else {
        cur->kernel_size = n;
      }
    } else if (c == 'e') {
      cur->use_end_ptr = 1;
    } else if (c == '_') {
      algo_phase_t* next = (algo_phase_t*)calloc(1, sizeof(algo_phase_t));
      cur->next = next;
      cur = next;
    } else {
      FATAL("unrecognised character %c in algorithm string %s", c, value);
    }
  }
  for (cur = first; cur; cur = cur->next) {
    if (!cur->s_acc && !cur->v_acc) {
      cur->s_acc = cur->s_load = 1;
    }
    if (cur->s_acc && (cur->s_load % cur->s_acc)) {
      FATAL("algorithm %s has s load count (%u) not an integer multiple of s acc count (%u)", value, cur->s_load, cur->s_acc);
    }
    if (cur->v_acc && (cur->v_load % cur->v_acc)) {
      FATAL("algorithm %s has v load count (%u) not an integer multiple of v acc count (%u)", value, cur->v_load, cur->v_acc);
    }
    if (g_isa == ISA_NONE) {
      if (cur->v_load) FATAL("need to specify an ISA to use vector accumulators");
      if (cur->s_acc > 1) FATAL("need to specify an ISA to use more than one scalar accumulator");
    }
  }
  return first;
}

static void parse_args(int argc, const char* const* argv) {
  sbuf_t* b;
#define ARGS \
  DEF_ARG(isa, "-i") \
  DEF_ARG(poly, "-p", "--polynomial") \
  DEF_ARG(algo, "-a", "--algorithm") \
  DEF_ARG(out, "-o", "--output")
#define DEF_ARG(name, ...) static const char* name##_spellings[] = {"--" #name, __VA_ARGS__, NULL};
  ARGS
#undef DEF_ARG
#define DEF_ARG(name, ...) cli_arg_t name = {name##_spellings, NULL};
  ARGS
#undef DEF_ARG
#define DEF_ARG(name, ...) &name,
  cli_arg_t* args[] = { ARGS NULL };
#undef DEF_ARG
#undef ARGS
  int i;
  for (i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (!strcmp(arg, "--help") || !strcmp(arg, "-h") || !strcmp(arg, "-?")) {
      print_help(stdout, argv[0]);
      exit(0);
    } else {
      const char* eq = strchr(arg, '=');
      size_t n = eq ? (size_t)(eq - arg) : strlen(arg);
      cli_arg_t* m = match_arg(args, arg, n);
      if (m) {
        if (eq) {
          m->value = eq + 1;
        } else if (++i < argc) {
          m->value = argv[i];
        } else {
          FATAL("missing value for option %.*s", (int)n, arg);
        }
      } else {
        FATAL("unknown option %.*s", (int)n, arg);
      }
    }
  }

  if (isa.value && *isa.value) g_isa = parse_isa(isa.value);
  if (poly.value && *poly.value) g_poly = parse_poly(poly.value);
  if (algo.value && *algo.value) g_algo = parse_algo(algo.value);
  g_out_path = out.value;

  b = g_includes;
  put_fmt(b, "/* Generated by https://github.com/corsix/fast-crc32/ using: */\n/* %s", argv[0]);
  for (i = 0; i < (int)(sizeof(args)/sizeof(args[0]))-1; ++i) {
    cli_arg_t* arg = args[i];
    if (arg->value && arg != &out) {
      put_fmt(b, " %s %s", arg->spellings[1], arg->value);
    }
  }
  put_lit(b, " */\n");
  put_lit(b, "/* MIT licensed */\n\n");
}

/* Polynomial math helpers. */
/* These operate on GF(2^n) polynomials, expressed as reversed bit strings. */

static uint64_t xndivp(uint32_t n) /* x^n div P (n <= 95) */ {
  uint64_t q = 0;
  uint32_t r = 1;
  for (n = 95 - n; n < 64; ++n) {
    q ^= (r & 1ull) << n;
    r = (r >> 1) ^ ((r & 1) * g_poly);
  }
  return q;
}

static uint32_t xnmodp(uint64_t n) /* x^n mod P, in log(n) time */ {
  uint64_t stack = ~(uint64_t)1, r;
  uint32_t i;
  for (; n > 31; n >>= 1) {
    stack = (stack << 1) + (n & 1);
  }
  stack = ~stack;
  r = ((uint32_t)0x80000000) >> n; /* r = x^n (n <= 31) */
  while ((i = stack & 1), stack >>= 1) {
    /* r = r^2 * x^1, and expand from 32 to 64 bits. */
    /* In GF(2), (a + b)^2 == a^2 + b^2, so r^2 is computed by expressing r as
    ** the sum of its bits and then squaring each bit individually. The extra
    ** x^1 appears because the product of two 32-bit polynomials is a 63-bit
    ** polynomial, and going from 63 to 64 in the reversed domain is *x^1. */
    r ^= r << 16, r &= 0x0000ffff0000ffffull;
    r ^= r <<  8, r &= 0x00ff00ff00ff00ffull;
    r ^= r <<  4, r &= 0x0f0f0f0f0f0f0f0full;
    r ^= r <<  2, r &= 0x3333333333333333ull;
    r ^= r <<  1, r &= 0x5555555555555555ull;
    /* r = r / x^i (i <= 1) */
    /* This conditionally removes the *x^1 from the previous step. */
    r <<= i;
    /* r = r mod P, and narrow back down to 32 bits. */
    for (i = 0; i < 32; ++i) {
      r = (r >> 1) ^ ((r & 1) * g_poly);
    }
  }
  return (uint32_t)r;
}

/* Code generator. */

static const char* g_scalar1_fn = "crc_u8";
static const char* g_scalar4_fn = "crc_u32";
static const char* g_scalar8_fn = "crc_u64";
static const char* g_vec16_type;
static const char* g_vec16_lane8_fn;
static const char* g_vector_type;
static uint32_t g_scalar_natural_bytes = 8;
static uint32_t g_vector_bytes = 16;
static uint32_t g_table_planes = 0;

#define possible_header(name) \
  static void need_##name##_h() { \
    static int done = 0; \
    if (!done) put_lit(g_includes,  "#include <" #name ".h>\n"); \
    done = 1; \
  }
possible_header(arm_acle)
possible_header(arm_neon)
possible_header(nmmintrin)
possible_header(immintrin)
possible_header(wmmintrin)
#undef possible_header

static void emit_standard_preprocessor(void) {
  put_lit(g_includes, "#include <stddef.h>\n");
  put_lit(g_includes, "#include <stdint.h>\n");
  put_lit(g_out, "\n#if defined(_MSC_VER)\n");
  put_lit(g_out, "#define CRC_AINLINE static __forceinline\n");
  put_lit(g_out, "#define CRC_ALIGN(n) __declspec(align(n))\n");
  put_lit(g_out, "#else\n");
  put_lit(g_out, "#define CRC_AINLINE static __inline __attribute__((always_inline))\n");
  put_lit(g_out, "#define CRC_ALIGN(n) __attribute__((aligned(n)))\n");
  put_lit(g_out, "#endif\n");
  put_lit(g_out, "#define CRC_EXPORT extern\n\n");
}

static void generate_table(sbuf_t* b) {
  uint32_t i, j, k;
  put_fmt(b, "[%u][256] = {", g_table_planes);
  for (i = 0; i < g_table_planes; ) {
    put_lit(b, "{\n");
    for (j = 0; j < 256; ) {
      uint32_t crc = j;
      for (k = (i + 1) * 8; k; --k) {
        crc = (crc >> 1) ^ ((crc & 1) * g_poly);
      }
      ++j;
      put_fmt(b, "0x%x%s", crc, j >= 256 ? "" : j % 6 ? ", " : ",\n");
    }
    if (++i < g_table_planes) {
      put_lit(b, "},");
    } else {
      put_lit(b, "\n}};\n\n");
    }
  }
}

static const char* need_crc_table(uint32_t planes) {
  const char* table_var = "g_crc_table";
  if (planes > g_table_planes) {
    if (g_table_planes == 0) {
      put_fmt(g_out, "static const uint32_t %s", table_var);
      put_deferred_fn(g_out, generate_table);
    }
    g_table_planes = planes;
  }
  return table_var;
}

static void need_clmul_fn(const char* which, isa_t isa) {
  static uint32_t done = 0;
  uint32_t lo = which[0] == 'l';
  uint32_t mask = 1u << (lo + 2 * (uint32_t)isa);
  sbuf_t* b = g_out;
  if (done & mask) return;
  done |= mask;

  switch (isa) {
  case ISA_NEON:
    need_arm_neon_h();
    put_fmt(b, "CRC_AINLINE %s clmul_%s_e(%s a, %s b, %s c) {\n", g_vector_type, which, g_vector_type, g_vector_type, g_vector_type);
    put_fmt(b,   "%s r;\n", g_vector_type);
    put_fmt(b,   "__asm(\"pmull%s %%0.1q, %%2.%ud, %%3.%ud\\neor %%0.16b, %%0.16b, %%1.16b\\n\" : \"=w\"(r), \"+w\"(c) : \"w\"(a), \"w\"(b));\n", &"2"[lo], 2u - lo, 2u - lo);
    put_lit(b,   "return r;\n");
    put_lit(b, "}\n\n");
    break;
  case ISA_NEON_EOR3:
    need_arm_neon_h();
    put_fmt(b, "CRC_AINLINE %s clmul_%s(%s a, %s b) {\n", g_vector_type, which, g_vector_type, g_vector_type);
    put_fmt(b,   "%s r;\n", g_vector_type);
    put_fmt(b,   "__asm(\"pmull%s %%0.1q, %%1.%ud, %%2.%ud\\n\" : \"=w\"(r) : \"w\"(a), \"w\"(b));\n", &"2"[lo], 2u - lo, 2u - lo);
    put_lit(b,   "return r;\n");
    put_lit(b, "}\n\n");
    break;
  case ISA_SSE:
  case ISA_AVX512:
    need_wmmintrin_h();
    put_fmt(b, "#define clmul_%s(a, b) (_mm_clmulepi64_si128((a), (b), %u))%s\n", which, (uint32_t)(0x11 * !lo), &"\n"[lo]);
    break;
  case ISA_AVX512_VPCLMULQDQ:
    need_immintrin_h();
    put_fmt(b, "#define clmul_%s(a, b) (_mm512_clmulepi64_epi128((a), (b), %u))%s\n", which, (uint32_t)(0x11 * !lo), &"\n"[lo]);
    break;
  default:
    FATAL_ISA();
  }
}

static void need_crc_scalar(uint32_t size) {
  static uint32_t done = 0;
  sbuf_t* b;
  if (done & size) return;
  done |= size;
  if (size > 8) return;

  b = sbuf_new();
  if (size == 1) {
    const char* table_var = need_crc_table(1);
    put_fmt(b, "CRC_AINLINE uint32_t %s(uint32_t crc, uint8_t val) {\n", g_scalar1_fn);
    put_fmt(b,   "return (crc >> 8) ^ %s[0][(crc & 0xFF) ^ val];\n", table_var);
    put_lit(b, "}\n\n");
  } else if (size == 4) {
    put_fmt(b, "CRC_AINLINE uint32_t %s(uint32_t crc, uint32_t val) {\n", g_scalar4_fn);
    if (g_isa == ISA_NONE) {
      const char* table_var = need_crc_table(4);
      put_lit(b, "crc ^= val;\n");
      put_fmt(b, "return %s[0][crc >>  24] ^ %s[1][(crc >> 16) & 0xFF] ^\n", table_var, table_var);
      put_fmt(b, "       %s[3][crc & 0xFF] ^ %s[2][(crc >>  8) & 0xFF];\n", table_var, table_var);
    } else {
      uint64_t q = xndivp(63);
      if (g_isa == ISA_NEON || g_isa == ISA_NEON_EOR3) {
        need_clmul_fn("lo", ISA_NEON_EOR3);
        put_lit(b, "uint64x2_t a = vmovq_n_u64(crc ^ val);\n");
        put_fmt(b, "a = clmul_lo(a, vmovq_n_u64(0x%x%xull));\n", (uint32_t)(q >> 32), (uint32_t)q);
        put_fmt(b, "a = clmul_lo(a, vmovq_n_u64(0x%x%xull));\n", g_poly >> 31, g_poly * 2u + 1u);
        put_lit(b, "return vgetq_lane_u32(vreinterpretq_u32_u64(a), 2);\n");
      } else {
        need_nmmintrin_h();
        need_wmmintrin_h();
        put_fmt(b, "__m128i k = _mm_setr_epi32(0x%x, 0x%x, 0x%x, %u);\n",
          (uint32_t)q, (uint32_t)(q >> 32), g_poly * 2u + 1u, g_poly >> 31);
        put_lit(b, "__m128i a = _mm_cvtsi32_si128(crc ^ val);\n");
        put_lit(b, "__m128i b = _mm_clmulepi64_si128(a, k, 0x00);\n");
        put_lit(b, "__m128i c = _mm_clmulepi64_si128(b, k, 0x10);\n");
        put_lit(b, "return _mm_extract_epi32(c, 2);\n");
      }
    }
    put_lit(b, "}\n\n");
  } else if (size == 8) {
    put_fmt(b, "CRC_AINLINE uint32_t %s(uint32_t crc, uint64_t val) {\n", g_scalar8_fn);
    if (g_isa == ISA_NONE) {
      need_crc_scalar(4);
      put_fmt(b, "crc = %s(crc, (uint32_t)val);\n", g_scalar4_fn);
      put_fmt(b, "return %s(crc, (uint32_t)(val >> 32));\n", g_scalar4_fn);
    } else {
      uint64_t q = xndivp(95);
      if (g_isa == ISA_NEON || g_isa == ISA_NEON_EOR3) {
        need_clmul_fn("lo", ISA_NEON_EOR3);
        put_lit(b, "uint64x2_t a = vmovq_n_u64(crc ^ val);\n");
        put_fmt(b, "a = clmul_lo(a, vmovq_n_u64(0x%x%xull));\n", (uint32_t)(q >> 32), (uint32_t)q);
        put_fmt(b, "a = clmul_lo(a, vmovq_n_u64(0x%x%xull));\n", g_poly >> 31, g_poly * 2u + 1u);
        put_lit(b, "return vgetq_lane_u32(vreinterpretq_u32_u64(a), 2);\n");
      } else {
        need_nmmintrin_h();
        need_wmmintrin_h();
        put_fmt(b, "__m128i k = _mm_setr_epi32(0x%x, 0x%x, 0x%x, %u);\n",
          (uint32_t)q, (uint32_t)(q >> 32), g_poly * 2u + 1u, g_poly >> 31);
        put_lit(b, "__m128i a = _mm_cvtsi64_si128(crc ^ val);\n");
        put_lit(b, "__m128i b = _mm_clmulepi64_si128(a, k, 0x00);\n");
        put_lit(b, "__m128i c = _mm_clmulepi64_si128(b, k, 0x10);\n");
        put_lit(b, "return _mm_extract_epi32(c, 2);\n");
      }
    }
    put_lit(b, "}\n\n");
  }
  put_deferred_sbuf(g_out, b);
}

static void init_isa(void) {
  switch (g_isa) {
  case ISA_NEON:
  case ISA_NEON_EOR3:
    g_vec16_type = "uint64x2_t";
    g_vec16_lane8_fn = "vgetq_lane_u64";
    break;
  case ISA_AVX512_VPCLMULQDQ:
    g_vector_bytes = 64;
    g_vector_type = "__m512i";
    /* fallthrough */
  case ISA_SSE:
  case ISA_AVX512:
    g_vec16_type = "__m128i";
    g_vec16_lane8_fn = "_mm_extract_epi64";
    break;
  case ISA_NONE:
    g_scalar_natural_bytes = 4;
    break;
  }
  if (g_vector_bytes == 16) {
    g_vector_type = g_vec16_type;
  }

  if (g_poly == REV_POLY_CRC32) {
    if (g_isa == ISA_NEON || g_isa == ISA_NEON_EOR3) {
      need_arm_acle_h();
      g_scalar1_fn = "__crc32b";
      g_scalar4_fn = "__crc32w";
      g_scalar8_fn = "__crc32d";
      need_crc_scalar(15);
    }
  } else if (g_poly == REV_POLY_CRC32C) {
    if (g_isa == ISA_NEON || g_isa == ISA_NEON_EOR3) {
      need_arm_acle_h();
      g_scalar1_fn = "__crc32cb";
      g_scalar4_fn = "__crc32cw";
      g_scalar8_fn = "__crc32cd";
      need_crc_scalar(15);
    } else if (g_isa == ISA_SSE || g_isa == ISA_AVX512 || g_isa == ISA_AVX512_VPCLMULQDQ) {
      need_nmmintrin_h();
      g_scalar1_fn = "_mm_crc32_u8";
      g_scalar4_fn = "_mm_crc32_u32";
      g_scalar8_fn = "_mm_crc32_u64";
      need_crc_scalar(15);
    }
  }
}

static void need_clmul_scalar(void) {
  static int done = 0;
  sbuf_t* b = g_out;
  if (done) return;
  done = 1;

  put_fmt(b, "CRC_AINLINE %s clmul_scalar(uint32_t a, uint32_t b) {\n", g_vec16_type);
  if (g_isa == ISA_NEON || g_isa == ISA_NEON_EOR3) {
    need_arm_neon_h();
    put_lit(b, "uint64x2_t r;\n");
    put_lit(b, "__asm(\"pmull %0.1q, %1.1d, %2.1d\\n\" : \"=w\"(r) : \"w\"(vmovq_n_u64(a)), \"w\"(vmovq_n_u64(b)));\n");
    put_lit(b, "return r;\n");
  } else {
    need_wmmintrin_h();
    put_lit(b, "return _mm_clmulepi64_si128(_mm_cvtsi32_si128(a), _mm_cvtsi32_si128(b), 0);\n");
  }
  put_lit(b, "}\n\n");
}

static void need_crc_shift(void) {
  static int done = 0;
  sbuf_t* b = g_out;
  if (done) return;
  done = 1;
  need_clmul_scalar();
  need_crc_scalar(4);
  need_crc_scalar(8);

  put_lit(b, "static uint32_t xnmodp(uint64_t n) /* x^n mod P, in log(n) time */ {\n");
  put_lit(b,   "uint64_t stack = ~(uint64_t)1;\n");
  put_lit(b,   "uint32_t acc, low;\n");
  put_lit(b,   "for (; n > 191; n = (n >> 1) - 16) {\n");
  put_lit(b,     "stack = (stack << 1) + (n & 1);\n");
  put_lit(b,   "}\n");
  put_lit(b,   "stack = ~stack;\n");
  put_lit(b,   "acc = ((uint32_t)0x80000000) >> (n & 31);\n");
  put_lit(b,   "for (n >>= 5; n; --n) {\n");
  put_fmt(b,     "acc = %s(acc, 0);\n", g_scalar4_fn);
  put_lit(b,   "}\n");
  put_lit(b,   "while ((low = stack & 1), stack >>= 1) {\n");
  if (g_isa == ISA_NEON || g_isa == ISA_NEON_EOR3) {
    put_lit(b,     "poly8x8_t x = vreinterpret_p8_u64(vmov_n_u64(acc));\n");
    put_lit(b,     "uint64_t y = vgetq_lane_u64(vreinterpretq_u64_p16(vmull_p8(x, x)), 0);\n");
  } else {
    put_lit(b,     "__m128i x = _mm_cvtsi32_si128(acc);\n");
    put_lit(b,     "uint64_t y = _mm_cvtsi128_si64(_mm_clmulepi64_si128(x, x, 0));\n");
  }
  put_fmt(b,     "acc = %s(0, y << low);\n", g_scalar8_fn);
  put_lit(b,   "}\n");
  put_lit(b,   "return acc;\n");
  put_lit(b, "}\n\n");

  put_fmt(b, "CRC_AINLINE %s crc_shift(uint32_t crc, size_t nbytes) {\n", g_vec16_type);
  put_lit(b,   "return clmul_scalar(crc, xnmodp(nbytes * 8 - 33));\n");
  put_lit(b, "}\n\n");
}

static void emit_scalar_fn_mem(sbuf_t* b, uint32_t acc, uint32_t size) {
  need_crc_scalar(size);
  put_fmt(b, "crc%u = ", acc);
  if (size == 8) {
    put_fmt(b, "%s(crc%u, *(const uint64_t*)", g_scalar8_fn, acc);
  } else if (size == 4) {
    put_fmt(b, "%s(crc%u, *(const uint32_t*)", g_scalar4_fn, acc);
  } else if (size == 1) {
    put_fmt(b, "%s(crc%u, *(const uint8_t*)", g_scalar1_fn, acc);
  } else {
    FATAL("bad size %d", (int)size);
  }
}

static void emit_vector_load(sbuf_t* b, const char* base, uint32_t offset) {
  switch (g_isa) {
  case ISA_NEON:
  case ISA_NEON_EOR3:
    put_lit(b, "vld1q_u64((const uint64_t*)");
    break;
  case ISA_SSE:
  case ISA_AVX512:
    put_lit(b, "_mm_loadu_si128((const __m128i*)");
    break;
  case ISA_AVX512_VPCLMULQDQ:
    put_lit(b, "_mm512_loadu_si512((const void*)");
    break;
  default:
    FATAL_ISA();
  }
  if (offset) put_lit(b, "(");
  put_str(b, base);
  if (offset) put_fmt(b, " + %u)", offset);
  put_lit(b, ")");
}

static void emit_product(sbuf_t* b, const char* lhs, uint32_t rhs) {
  if (rhs == 0) {
    put_lit(b, "0");
  } else {
    put_str(b, lhs);
    if (rhs > 1) {
      put_fmt(b, " * %u", rhs);
    }
  }
}

static void emit_vc_xor_tree(sbuf_t* b, uint32_t lo, uint32_t hi) {
  uint32_t range = hi - lo;
  if (range == 1) {
    put_fmt(b, "vc%u", lo);
  } else if (range >= 3 && (g_isa == ISA_NEON_EOR3 || g_isa == ISA_AVX512 || g_isa == ISA_AVX512_VPCLMULQDQ)) {
    uint32_t m1 = lo + range / 3;
    uint32_t m2 = hi - range / 3;
    if (g_isa == ISA_NEON_EOR3) {
      put_lit(b, "veor3q_u64(");
    } else {
      need_immintrin_h();
      put_lit(b, "_mm_ternarylogic_epi64(");
    }
    emit_vc_xor_tree(b, lo, m1);
    put_lit(b, ", ");
    emit_vc_xor_tree(b, m1, m2);
    put_lit(b, ", ");
    emit_vc_xor_tree(b, m2, hi);
    if (g_isa != ISA_NEON_EOR3) {
      put_lit(b, ", 0x96");
    }
    put_lit(b, ")");
  } else {
    uint32_t mid = lo + range / 2;
    if (g_isa == ISA_NEON_EOR3 || g_isa == ISA_NEON) {
      put_lit(b, "veorq_u64(");
    } else {
      put_lit(b, "_mm_xor_si128(");
    }
    emit_vc_xor_tree(b, lo, mid);
    put_lit(b, ", ");
    emit_vc_xor_tree(b, mid, hi);
    put_lit(b, ")");
  }
}

static void emit_vector_set_k(sbuf_t* b, uint32_t k) {
  uint32_t k1 = xnmodp(k * g_vector_bytes * 8 + 32 - 1);
  uint32_t k2 = xnmodp(k * g_vector_bytes * 8 - 32 - 1);
  if (g_isa == ISA_NEON || g_isa == ISA_NEON_EOR3) {
    put_fmt(b, "{ static const uint64_t CRC_ALIGN(16) k_[] = {0x%x, 0x%x}; ", k1, k2);
    put_lit(b, "k = vld1q_u64(k_); }\n");
  } else {
    put_lit(b, "k = ");
    if (g_vector_bytes > 16) put_lit(b, "_mm512_broadcast_i32x4(");
    put_fmt(b, "_mm_setr_epi32(0x%x, 0, 0x%x, 0)", k1, k2);
    if (g_vector_bytes > 16) put_lit(b, ")");
    put_lit(b, ";\n");
  }
}

static void emit_xor_scalar_into_vector(sbuf_t* b, const char* scalar, const char* vector) {
  switch (g_isa) {
  case ISA_NEON:
  case ISA_NEON_EOR3:
    put_fmt(b, "%s = veorq_u64((uint64x2_t){%s, 0}, %s);\n", vector, scalar, vector);
    break;
  case ISA_SSE:
  case ISA_AVX512:
    put_fmt(b, "%s = _mm_xor_si128(_mm_cvtsi32_si128(%s), %s);\n", vector, scalar, vector);
    break;
  case ISA_AVX512_VPCLMULQDQ:
    put_fmt(b, "%s = _mm512_xor_si512(_mm512_castsi128_si512(_mm_cvtsi32_si128(%s)), %s);\n", vector, scalar, vector);
    break;
  default:
    FATAL_ISA();
  }
}

static void emit_vector_fma(sbuf_t* p1, sbuf_t* p2, uint32_t reg, const char* addend, uint32_t offset) {
  /* Does `x{reg} = x{reg} * k + addend` in two parts; part one written to `p1`, part two to `p2`. */
  /* A previous emit_vector_set_k call will have set `k`. */
  need_clmul_fn("lo", g_isa);
  need_clmul_fn("hi", g_isa);
  if (g_isa != ISA_NEON) {
    put_fmt(p1, "y%u = clmul_lo(x%u, k), x%u = clmul_hi(x%u, k);\n", reg, reg, reg, reg);
  }
  switch (g_isa) {
  case ISA_NEON: put_fmt(p2, "y%u = clmul_lo_e(x%u, k, ", reg, reg); break;
  case ISA_NEON_EOR3: put_fmt(p2, "x%u = veor3q_u64(x%u, y%u, ", reg, reg, reg); break;
  case ISA_SSE: put_fmt(p2, "y%u = _mm_xor_si128(y%u, ", reg, reg); break;
  case ISA_AVX512: put_fmt(p2, "x%u = _mm_ternarylogic_epi64(x%u, y%u, ", reg, reg, reg); break;
  case ISA_AVX512_VPCLMULQDQ: put_fmt(p2, "x%u = _mm512_ternarylogic_epi64(x%u, y%u, ", reg, reg, reg); break;
  default: FATAL_ISA();
  }
  if (addend[1]) {
    emit_vector_load(p2, addend, offset);
  } else {
    put_fmt(p2, "%s%u", addend, offset);
  }
  switch (g_isa) {
  case ISA_NEON: put_fmt(p2, "), x%u = clmul_hi_e(x%u, k, y%u);\n", reg, reg, reg); break;
  case ISA_NEON_EOR3: put_lit(p2, ");\n"); break;
  case ISA_SSE: put_fmt(p2, "), x%u = _mm_xor_si128(x%u, y%u);\n", reg, reg, reg); break;
  case ISA_AVX512: case ISA_AVX512_VPCLMULQDQ: put_lit(p2, ", 0x96);\n"); need_immintrin_h(); break;
  default: FATAL_ISA();
  }
}

static void emit_scalar_main(sbuf_t* b, algo_phase_t* ap) {
  uint32_t i, j;
  for (i = 0; i < ap->s_load; i += ap->s_acc) {
    for (j = 0; j < ap->s_acc; ++j) {
      emit_scalar_fn_mem(b, j, g_scalar_natural_bytes);
      if (i || j) put_lit(b, "(");
      put_lit(b, "buf");
      if (j) put_lit(b, " + "), emit_product(b, "klen", j);
      if (i) put_fmt(b, " + %u", (i / ap->s_acc) * g_scalar_natural_bytes);
      if (i || j) put_lit(b, ")");
      put_lit(b, ");\n");
    }
  }
}

static void emit_vector_tree_reduce(sbuf_t* b, uint32_t n) {
  /* Collapse vector registers x0 ... x{n-1} down to just x0. */
  uint32_t d;
  for (d = 1; n > 1; n >>= 1, d <<= 1) {
    uint32_t i;
    sbuf_t* p1;
    emit_vector_set_k(b, d);
    if (n & 1) {
      /* Odd number of registes; merge the first pair. */
      emit_vector_fma(b, b, 0, "x", d);
      /* This shuffle looks ugly, but the C compiler should make it free. */
      for (i = 1, n -= 1; i < n; i += 1) {
        put_fmt(b, "%sx%u = x%u", i == 1 ? "" : ", ", i * d, i * d + d);
      }
      put_lit(b, ";\n");
    }
    /* Even number of registers; merge adjacent pairs. */
    p1 = put_new_sbuf(b);
    for (i = 0; i < n; i += 2) {
      emit_vector_fma(p1, b, i * d, "x", i * d + d);
    }
  }
}

static void emit_main_fn() {
  sbuf_t* b = sbuf_new();
  algo_phase_t* ap;
  uint32_t current_alignment = g_scalar_natural_bytes;
  put_lit(b, "CRC_EXPORT uint32_t crc32_impl(uint32_t crc0, const char* buf, size_t len) {\n");
  put_lit(b,   "crc0 = ~crc0;\n");
  if (current_alignment > 1) {
    need_crc_scalar(1);
    put_fmt(b, "for (; len && ((uintptr_t)buf & %u); --len) {\n", current_alignment - 1u);
    put_fmt(b,   "crc0 = %s(crc0, *buf++);\n", g_scalar1_fn);
    put_lit(b, "}\n");
  }
  for (ap = g_algo; ap; ap = ap->next) {
    if (ap->v_acc && g_vector_bytes > current_alignment) {
      current_alignment = g_vector_bytes;
      put_fmt(b, "%s (((uintptr_t)buf & %u) && len >= %u) {\n",
        g_vector_bytes == g_scalar_natural_bytes * 2 ? "if" : "while",
        g_vector_bytes - g_scalar_natural_bytes, g_scalar_natural_bytes);
      emit_scalar_fn_mem(b, 0, g_scalar_natural_bytes); put_lit(b, "buf);\n");
      put_fmt(b,   "buf += %u;\n", g_scalar_natural_bytes);
      put_fmt(b,   "len -= %u;\n", g_scalar_natural_bytes);
      put_lit(b, "}\n");
    }
    if (ap->v_load != 0 || ap->s_load > 1) {
      /* The block size is the number of bytes loaded per iteration. */
      uint32_t block_size = ap->v_load * g_vector_bytes + ap->s_load * g_scalar_natural_bytes;
      /* Take the requested kernel size, then round down for alignment, then round down to block size. */
      uint32_t kernel_align = ap->v_load ? g_vector_bytes : g_scalar_natural_bytes;
      uint32_t kernel_ideal_size = ap->kernel_size / kernel_align * kernel_align;
      uint32_t kernel_itrs = kernel_ideal_size / block_size;
      
      uint32_t i, j;
      const char* vbuf = "buf"; /* Base pointer for vector loads. */
      sbuf_t* vars; /* Insertion point for variable declarations. */

      /* Number of input bytes consumed by the post-loop accumulator merging. */
      uint32_t scalar_tail = 0;
      if (!ap->v_load) {
        /* Only have scalars, so of course the tail is scalar. */
        if (ap->s_acc > 1) {
          scalar_tail = g_scalar_natural_bytes;
        } else {
          /* But if there is only one accumulator, no merging is required. */
        }
      } else if (ap->s_load) {
        /* Mixture of vectors and scalars; if the scalars go last, then a tail is required. */
        /* If scalar loads maintain vector alignment, scalars go first. Otherwise vectors go first. */
        if (kernel_itrs) {
          if ((kernel_itrs * ap->s_load * g_scalar_natural_bytes) % g_vector_bytes) {
            scalar_tail = g_scalar_natural_bytes;
          }
        } else {
          if ((ap->s_load * g_scalar_natural_bytes) % g_vector_bytes) {
            scalar_tail = g_scalar_natural_bytes;
          }
        }
      }
      if (kernel_itrs && scalar_tail) {
        /* Need to recompute kernel_itrs because of scalar_tail. */
        kernel_itrs = (kernel_ideal_size - scalar_tail) / block_size;
        if (kernel_itrs) {
          /* Extend the tail to maintain alignment over the entire kernel. */
          uint32_t excess = (block_size * kernel_itrs + scalar_tail) % kernel_align;
          if (excess) {
            scalar_tail += kernel_align - excess;
          }
        }
      }
      
      if (kernel_itrs) {
        put_fmt(b, "while (len >= %u) {\n", block_size * kernel_itrs + scalar_tail);
        if (!ap->use_end_ptr && kernel_itrs != (ap->v_acc != 0)) {
          put_fmt(b, "uint32_t kitrs = %u;\n", kernel_itrs - (ap->v_acc != 0));
        }
      } else {
        put_fmt(b, "if (len >= %u) {\n", block_size + scalar_tail);
      }
      vars = put_new_sbuf(b);
      if (!kernel_itrs && ap->use_end_ptr) put_fmt(vars, "const char* end = buf + len;\n");
      if (!ap->v_load && ap->s_acc > 1) {
        if (kernel_itrs) {
          put_fmt(vars, "const size_t klen = %u;\n", kernel_itrs * (ap->s_load / ap->s_acc) * g_scalar_natural_bytes);
        } else {
          put_fmt(vars, "size_t klen = ((len - %u) / %u) * %u;\n", scalar_tail, block_size, (ap->s_load / ap->s_acc) * g_scalar_natural_bytes);
        }
        if (ap->use_end_ptr) {
          put_fmt(vars, "const char* limit = buf + klen - %u;\n", (ap->s_load / ap->s_acc) * g_scalar_natural_bytes);
        }
      } else if (ap->v_load && ap->s_acc) {
        vbuf = "buf2";
        if (kernel_itrs) {
          put_fmt(vars, "const size_t blk = %u;\n", kernel_itrs);
          if (ap->s_acc > 1 || !scalar_tail || ap->use_end_ptr) {
            put_fmt(vars, "const size_t klen = blk * %u;\n", (ap->s_load / ap->s_acc) * g_scalar_natural_bytes);
          }
        } else {
          put_fmt(vars, "size_t blk = (len - %u) / %u;\n", scalar_tail, block_size);
          put_fmt(vars, "size_t klen = blk * %u;\n", (ap->s_load / ap->s_acc) * g_scalar_natural_bytes);
        }
        put_fmt(vars, "const char* %s = buf + ", vbuf);
        emit_product(vars, "klen", scalar_tail ? 0u : ap->s_acc);
        put_lit(vars, ";\n");
        if (ap->use_end_ptr) {
          if (scalar_tail) {
            put_fmt(vars, "const char* limit = buf + blk * %u + klen - %u;\n", ap->v_load * g_vector_bytes, (ap->s_load / ap->s_acc) * g_scalar_natural_bytes * 2);
          } else {
            put_fmt(vars, "const char* limit = buf + klen - %u;\n", (ap->s_load / ap->s_acc) * g_scalar_natural_bytes * 2);
          }
        }
      } else {
        if (ap->use_end_ptr) {
          if (kernel_itrs) {
            put_fmt(vars, "const char* limit = buf + %u;\n", (kernel_itrs - 1) * block_size);
          } else {
            put_fmt(vars, "const char* limit = buf + len - %u;\n", block_size);
          }
        }
      }
      /* Scalar accumulators initialise to zero. */
      for (i = 1; i < ap->s_acc; ++i) {
        put_fmt(vars, "uint32_t crc%u = 0;\n", i);
      }
      /* Vectors do one iteration pre-loop to initialise accumulators. */
      if (ap->v_acc) put_lit(b, "/* First vector chunk. */\n");
      for (i = 0; i < ap->v_acc; ++i) {
        put_fmt(b, "%s x%u = ", g_vector_type, i);
        emit_vector_load(b, vbuf, i * g_vector_bytes);
        put_fmt(b, ", y%u;\n", i);
      }
      if (ap->v_acc) {
        put_fmt(b, "%s k;\n", g_vector_type);
        emit_vector_set_k(b, ap->v_acc);
        if (ap->s_load == 0 || scalar_tail) {
          emit_xor_scalar_into_vector(b, "crc0", "x0");
          if (scalar_tail) put_lit(b, "crc0 = 0;\n");
        }
        for (i = ap->v_acc; i < ap->v_load; i += ap->v_acc) {
          sbuf_t* p1 = put_new_sbuf(b);
          for (j = 0; j < ap->v_acc; ++j) {
            emit_vector_fma(p1, b, j, vbuf, (i + j) * g_vector_bytes);
          }
        }
        put_fmt(b, "%s += %u;\n", vbuf, ap->v_load * g_vector_bytes);
        if (!kernel_itrs && !ap->use_end_ptr) put_fmt(b, "len -= %u;\n", block_size);
        if (scalar_tail) put_fmt(b, "buf += blk * %u;\n", ap->v_load * g_vector_bytes);
      }
      if (!kernel_itrs || kernel_itrs != (ap->v_acc != 0)) {
        sbuf_t* loop_cond = sbuf_new();
        put_lit(b, "/* Main loop. */\n");
        if (kernel_itrs) {
          if (ap->use_end_ptr) {
            put_lit(loop_cond, "while (buf <= limit)");
          } else {
            put_lit(loop_cond, "while (--kitrs)");
          }
        } else {
          if (ap->use_end_ptr) {
            put_lit(loop_cond, "while (buf <= limit)");
          } else {
            put_fmt(loop_cond, "while (len >= %u)", block_size + scalar_tail);
          }
          if (ap->v_load) {
            put_deferred_sbuf(b, loop_cond), loop_cond = NULL;
            put_lit(b, " {\n");
          }
        }
        if (loop_cond) put_lit(b, "do {\n");
        for (i = 0; i < ap->v_load; i += ap->v_acc) {
          sbuf_t* p1 = put_new_sbuf(b);
          for (j = 0; j < ap->v_acc; ++j) {
            emit_vector_fma(p1, b, j, vbuf, (i + j) * g_vector_bytes);
          }
        }
        emit_scalar_main(b, ap);
        if (ap->s_load != 0) put_fmt(b, "buf += %u;\n", (ap->s_load / ap->s_acc) * g_scalar_natural_bytes);
        if (ap->v_load != 0) put_fmt(b, "%s += %u;\n", vbuf, ap->v_load * g_vector_bytes);
        if (!kernel_itrs && !ap->use_end_ptr) put_fmt(b, "len -= %u;\n", block_size);
        put_lit(b, "}");
        if (loop_cond) {
          put_lit(b, " ");
          put_deferred_sbuf(b, loop_cond);
          put_lit(b, ";");
        }
        put_lit(b, "\n");
      }
      /* Loop is over, now need to merge the various accumulators. */
      if (ap->v_acc > 1) {
        put_fmt(b, "/* Reduce x0 ... x%u to just x0. */\n", ap->v_acc - 1u);
        emit_vector_tree_reduce(b, ap->v_acc);
      }
      if (ap->s_acc > 1 || (ap->v_load && ap->s_acc)) {
        if (ap->v_load) {
          /* Vectors did one iteration pre-loop, so scalars need one post-loop. */
          put_lit(b, "/* Final scalar chunk. */\n");
          emit_scalar_main(b, ap);
          if (scalar_tail) put_fmt(b, "buf += %u;\n", (ap->s_load / ap->s_acc) * g_scalar_natural_bytes);
        }
        /* Shift each scalar accumulator by the number of bytes after it. */
        for (i = 0; i < ap->s_acc; ++i) {
          if ((i + 1) >= ap->s_acc && scalar_tail) {
            /* The last scalar accumulator has nothing after it. */
            break;
          }
          put_fmt(vars, "%s vc%u;\n", g_vec16_type, i);
          put_fmt(b, "vc%u = %s(crc%u, ", i, kernel_itrs ? "clmul_scalar" : "crc_shift", i);
          if (kernel_itrs) {
            uint32_t amount = kernel_itrs * (ap->s_load / ap->s_acc) * g_scalar_natural_bytes * (ap->s_acc - 1 - i);
            amount += scalar_tail ? scalar_tail : kernel_itrs * ap->v_load * g_vector_bytes;
            put_fmt(b, "0x%x", xnmodp(amount * 8 - 33));
            need_clmul_scalar();
          } else {
            need_crc_shift();
            emit_product(b, "klen", ap->s_acc - 1 - i);
            if (scalar_tail) {
              put_fmt(b, " + %u", scalar_tail);
            } else if (ap->v_load) {
              put_fmt(b, " + blk * %u", ap->v_load * g_vector_bytes);
            }
          }
          put_lit(b, ");\n");
        }
        /* Merge the shifted accumulators into vc. */
        put_lit(vars, "uint64_t vc;\n");
        if (ap->s_acc == (scalar_tail != 0)) {
          put_lit(b, "vc = 0;\n");
        } else {
          put_fmt(b, "vc = %s(", g_vec16_lane8_fn);
          emit_vc_xor_tree(b, 0, ap->s_acc - (scalar_tail != 0));
          put_lit(b, ", 0);\n");
        }
      }
      if (ap->v_load) {
        const char* x0 = "x0";
        if (g_isa == ISA_AVX512_VPCLMULQDQ) {
          put_lit(b, "/* Reduce 512 bits to 128 bits. */\n");
          need_immintrin_h();
          need_clmul_fn("lo", g_isa);
          need_clmul_fn("hi", g_isa);
          put_lit(b, "k = _mm512_setr_epi32(");
          for (i = 415; i >= 95; i -= 64) {
            put_fmt(b, "0x%x, 0, ", xnmodp(i));
          }
          put_lit(b, "0, 0, 0, 0);\n");
          put_lit(b, "y0 = clmul_lo(x0, k), k = clmul_hi(x0, k);\n");
          put_lit(b, "y0 = _mm512_xor_si512(y0, k);\n");
          put_fmt(vars, "%s z0;\n", g_vec16_type);
          put_lit(b, "z0 = _mm_ternarylogic_epi64(_mm512_castsi512_si128(y0), _mm512_extracti32x4_epi32(y0, 1), _mm512_extracti32x4_epi32(y0, 2), 0x96);\n");
          put_lit(b, "z0 = _mm_xor_si128(z0, _mm512_extracti32x4_epi32(x0, 3));\n");
          x0 = "z0";
        }
        put_lit(b, "/* Reduce 128 bits to 32 bits, and multiply by x^32. */\n");
        if (scalar_tail) {
          put_fmt(b, "vc ^= %s(%s(%s(%s(0, %s(%s, 0)), %s(%s, 1)), ",
            g_vec16_lane8_fn, kernel_itrs ? "clmul_scalar" : "crc_shift", g_scalar8_fn, g_scalar8_fn, g_vec16_lane8_fn, x0, g_vec16_lane8_fn, x0);
          if (kernel_itrs) {
            uint32_t amount = kernel_itrs * ap->s_load * g_scalar_natural_bytes + scalar_tail;
            put_fmt(b, "0x%x", xnmodp(amount * 8 - 33));
            need_clmul_scalar();
          } else {
            need_crc_shift();
            put_fmt(b, "klen * %u + %u", ap->s_acc, scalar_tail);
          }
          put_lit(b, "), 0);\n");
        } else {
          need_crc_scalar(8);
          put_fmt(b, "crc0 = %s(0, %s(%s, 0));\n", g_scalar8_fn, g_vec16_lane8_fn, x0);
          put_fmt(b, "crc0 = %s(crc0, %s%s(%s, 1));\n", g_scalar8_fn, ap->s_load ? "vc ^ " : "", g_vec16_lane8_fn, x0);
        }
      }
      if (scalar_tail) {
        put_fmt(b, "/* Final %u bytes. */\n", scalar_tail);
        if (ap->s_acc > 1) {
          put_lit(b, "buf += "); emit_product(b, "klen", ap->s_acc - 1); put_lit(b, ";\n");
          put_fmt(b, "crc0 = crc%u;\n", ap->s_acc - 1);
        }
        for (i = scalar_tail; i > g_scalar_natural_bytes; i -= g_scalar_natural_bytes) {
          emit_scalar_fn_mem(b, 0, g_scalar_natural_bytes), put_lit(b, "buf), ");
          put_fmt(b, "buf += %u;\n", g_scalar_natural_bytes);
        }
        emit_scalar_fn_mem(b, 0, g_scalar_natural_bytes), put_lit(b, "buf ^ vc), ");
        put_fmt(b, "buf += %u;\n", g_scalar_natural_bytes);
        if (!kernel_itrs && !ap->use_end_ptr) put_fmt(b, "len -= %u;\n", scalar_tail);
      } else if (ap->v_load && ap->s_load) {
        put_fmt(b, "buf = %s;\n", vbuf);
      }
      if (kernel_itrs) {
        uint32_t amount = kernel_itrs * block_size + scalar_tail;
        put_fmt(b, "len -= %u;\n", amount);
        if (amount % g_vector_bytes) {
          current_alignment = g_scalar_natural_bytes;
        }
      } else {
        if (ap->use_end_ptr) {
          put_fmt(b, "len = end - buf;\n");
        }
        if ((block_size % g_vector_bytes) || (scalar_tail % g_vector_bytes)) {
          current_alignment = g_scalar_natural_bytes;
        }
      }
      put_lit(b, "}\n");
    }
  }
  put_fmt(b, "for (; len >= %u; buf += %u, len -= %u) {\n", g_scalar_natural_bytes, g_scalar_natural_bytes, g_scalar_natural_bytes);
  emit_scalar_fn_mem(b, 0, g_scalar_natural_bytes); put_lit(b, "buf);\n");
  put_lit(b, "}\n");
  if (g_scalar_natural_bytes > 1) {
    need_crc_scalar(1);
    put_lit(b, "for (; len; --len) {\n");
    put_fmt(b,   "crc0 = %s(crc0, *buf++);\n", g_scalar1_fn);
    put_lit(b, "}\n");
  }
  put_lit(b,   "return ~crc0;\n");
  put_lit(b, "}\n");
  put_deferred_sbuf(g_out, b);
}

static FILE* open_output_file(const char* path) {
  if (!path || !*path || !strcmp(path, "-")) {
    return stdout;
  } else {
    FILE* result = fopen(path, "wt");
    if (!result) FATAL("could not open %s for writing", path);
    return result;
  }
}

int main(int argc, const char* const* argv) {
  g_out = sbuf_new();
  g_includes = put_new_sbuf(g_out);
  parse_args(argc, argv);
  emit_standard_preprocessor();
  init_isa();
  emit_main_fn();
  flush_sbuf_to(g_out, open_output_file(g_out_path));
  return 0;
}
