/* MIT licensed; see LICENSE.md */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_help(FILE* f, const char* self) {
#if defined(__arm__) || defined(__arm) || defined(__ARM__) || defined(__ARM) || defined(__aarch64__) || defined(_M_ARM64)
  const char* self_isa = "neon,neon_eor3";
#else
  const char* self_isa = "sse,avx512";
#endif
  if (!self) self = "./autobench";
  fprintf(f, "Usage: %s [OPTION]...\n", self);
  fprintf(f, "Generate and compile and benchmark C code for computing CRC32.\n");
  fprintf(f, "Example: %s -i %s -p crc32c,crc32k -a s1,v4\n\n", self, self_isa);
  fprintf(f, "Options for ./generate:\n");
  fprintf(f, "  -i, --isa=ISA,ISA,...\n");
  fprintf(f, "  -p, --polynomial=POLY,POLY,...\n");
  fprintf(f, "  -a, --algorithm=ALGO,ALGO,ALGO,...\n");
  fprintf(f, "  An ISA of \"native\" will expand to some suitable values.\n");
  fprintf(f, "  Within any ALGO, START:STOP or START:STOP:STEP can be used\n");
  fprintf(f, "  in place of any number. A question mark character can also\n");
  fprintf(f, "  be placed after any term.\n");
  fprintf(f, "\nOptions for ./bench:\n");
  fprintf(f, "  -r, --rounds=N\n");
  fprintf(f, "  -d, --duration=N\n");
  fprintf(f, "  -s, --size=N\n");
  fprintf(f, "  -f, --format=FORMAT\n");
  fprintf(f, "      --aligned\n");
  fprintf(f, "      --assume-correct\n");
  fprintf(f, "\nOptions for make:\n");
  fprintf(f, "  -j\n");
  fprintf(f, "\nSee https://github.com/corsix/fast-crc32/\n");
}

#define FATAL(fmt, ...) \
  (fprintf(stderr, "FATAL error at %s:%d - " fmt "\n", __FILE__, __LINE__, ## __VA_ARGS__), fflush(stderr), exit(1))

static const char* g_makefile_path = "ab_Makefile";
static int g_samples_mode = 0;

typedef struct impl_t {
  char* name;
  char* arguments;
  int original_order;
} impl_t;

typedef struct ptr_array_t {
  void** contents;
  size_t size;
  size_t capacity;
} ptr_array_t;

static void ptr_array_append(ptr_array_t* arr, void* ptr) {
  size_t size = arr->size;
  if (size == arr->capacity) {
    if (size == 0) {
      arr->contents = (void**)malloc(sizeof(void*) * (arr->capacity = 4));
    } else {
      arr->contents = (void**)realloc(arr->contents, sizeof(void*) * (arr->capacity *= 2));
    }
  }
  arr->contents[size] = ptr;
  arr->size = size + 1;
}

static ptr_array_t g_impls;
static ptr_array_t g_make_args;
static ptr_array_t g_bench_args;

static void create_impl(const char* isa, const char* poly, const char* algo) {
  size_t sz = sizeof(impl_t) + (strlen(isa) + strlen(poly) + strlen(algo)) * 2 + 32;
  impl_t* impl = (impl_t*)malloc(sz);
  int n;
  impl->name = (char*)(impl + 1);
  n = sprintf(impl->name, "%s_%s_%s_%s", g_samples_mode ? "sample" : "ab", isa, poly, algo);
  impl->arguments = impl->name + n + 1;
  n = 0;
  if (*isa) n += sprintf(impl->arguments + n, " -i %s", isa);
  if (*poly) n += sprintf(impl->arguments + n, " -p %s", poly);
  if (*algo) n += sprintf(impl->arguments + n, " -a %s", algo);
  impl->original_order = (int)g_impls.size;
  ptr_array_append(&g_impls, (void*)impl);
}

typedef struct string_array_t {
  uint32_t* offsets;
  char* data;
  uint32_t string_count;
  uint32_t string_capacity;
  uint32_t data_size;
  uint32_t data_capacity;
} string_array_t;

static void string_array_append(string_array_t* sa, const char* str) {
  char* dst;
  size_t len;
  if (sa->string_count == sa->string_capacity) {
    sa->string_capacity = (sa->string_capacity + 1) * 2;
    sa->offsets = (uint32_t*)realloc(sa->offsets, sa->string_capacity * sizeof(uint32_t));
  }
  len = strlen(str) + 1;
  if (sa->data_size + len > sa->data_capacity) {
    sa->data_capacity = (sa->data_capacity + len) * 2;
    sa->data = (char*)realloc(sa->data, sa->data_capacity);
  }
  dst = sa->data + sa->data_size;
  sa->offsets[sa->string_count++] = sa->data_size;
  sa->data_size += len;
  memcpy(dst, str, len);
}

static void expand_colons(const char* src, char* tmp, uint32_t cursor, string_array_t* dst) {
  /* Consume string from `src`, write it out to `tmp+cursor`, then append `tmp` to `dst` when done. */
  /* Any instances of START:STOP or START:STOP:STEP in `src` are expanded. */
  uint32_t n = 0;
  uint32_t nlen = 0;
  while (*src == '?') {
    ++src;
  }
  for (;;) {
    char c = *src++;
    if (c == '?') {
      expand_colons(src, tmp, cursor, dst);
      cursor -= nlen;
      if (cursor) --cursor;
      expand_colons(src, tmp, cursor, dst);
      return;
    } else if (c == ':' && nlen != 0) {
      uint32_t start = n;
      uint32_t stop = 0;
      for (; (c = *src), ('0' <= c && c <= '9'); ++src) {
        stop = stop * 10u + (c - '0');
      }
      uint32_t step = 1;
      if (c == ':') {
        step = 0;
        while ((c = *++src), ('0' <= c && c <= '9')) {
          step = step * 10u + (c - '0');
        }
      }
      cursor -= nlen;
      if (step == 0) {
        if (start <= stop) {
          expand_colons(src, tmp, cursor + sprintf(tmp + cursor, "%u", (unsigned)start), dst);
        }
      } else {
        for (n = start; n <= stop; n += step) {
          expand_colons(src, tmp, cursor + sprintf(tmp + cursor, "%u", (unsigned)n), dst);
        }
      }
      if (*src == '?') {
        if (cursor) --cursor;
        expand_colons(src, tmp, cursor, dst);
      }
      return;
    } else {
      tmp[cursor++] = c;
      if (c == '\0') {
        string_array_append(dst, tmp);
        return;
      } else if ('0' <= c && c <= '9') {
        n = n * 10u + (c - '0');
        nlen += 1;
      } else {
        n = 0;
        nlen = 0;
      }
    }
  }
}

static void split_commas(const char* str, string_array_t* dst) {
  size_t n = (str ? strlen(str) : 0) + 1;
  char* mut = malloc(n * 2);
  char* base = mut;
  char* itr = base;
  memcpy(mut, str ? str : "", n);
  for (;;) {
    char c = *itr++;
    if (c == ',') {
      itr[-1] = '\0';
      expand_colons(base, mut + n, 0, dst);
      base = itr;
    } else if (c == '\0') {
      expand_colons(base, mut + n, 0, dst);
      break;
    }
  }
  free(mut);
}

static void create_impls(const char* isa, const char* poly, const char* algo) {
  string_array_t sa = {};
  uint32_t isa_end, poly_end, algo_end, isa_itr, poly_itr, algo_itr;
  if (isa && !strcmp(isa, "native")) {
#if defined(__arm__) || defined(__arm) || defined(__ARM__) || defined(__ARM) || defined(__aarch64__) || defined(_M_ARM64)
    isa = "neon,neon_eor3";
#else
    isa = "sse,avx512,avx512_vpclmulqdq";
#endif
  }
  split_commas(isa, &sa), isa_end = sa.string_count;
  split_commas(poly, &sa), poly_end = sa.string_count;
  split_commas(algo, &sa), algo_end = sa.string_count;
  for (isa_itr = 0; isa_itr < isa_end; ++isa_itr) {
    char* isa_val = sa.data + sa.offsets[isa_itr];
    for (poly_itr = isa_end; poly_itr < poly_end; ++poly_itr) {
      char* poly_val = sa.data + sa.offsets[poly_itr];
      for (algo_itr = poly_end; algo_itr < algo_end; ++algo_itr) {
        char* algo_val = sa.data + sa.offsets[algo_itr];
        create_impl(isa_val, poly_val, algo_val);
      }
    }
  }
}

typedef struct cli_arg_t {
  const char* const* spellings;
  const char* value;
  int used;
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

static void parse_args(int argc, const char* const* argv) {
  int bench_arg = -1; /* Arbitrary sentinel. */
#define ARGS \
  DEF_ARG(0, isa, "-i") \
  DEF_ARG(0, poly, "-p", "--polynomial") \
  DEF_ARG(0, algo, "-a", "--algorithm") \
  DEF_ARG(bench_arg, duration, "-d") \
  DEF_ARG(bench_arg, size, "-s") \
  DEF_ARG(bench_arg, rounds, "-r") \
  DEF_ARG(bench_arg, format, "-f")
#define DEF_ARG(init, name, ...) static const char* name##_spellings[] = {"--" #name, __VA_ARGS__, NULL};
  ARGS
#undef DEF_ARG
#define DEF_ARG(init, name, ...) cli_arg_t name = {name##_spellings, NULL, init};
  ARGS
#undef DEF_ARG
#define DEF_ARG(init, name, ...) &name,
  cli_arg_t* args[] = { ARGS NULL };
#undef DEF_ARG
#undef ARGS
  int i;
  for (i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (!strcmp(arg, "--help") || !strcmp(arg, "-h") || !strcmp(arg, "-?")) {
      print_help(stdout, argv[0]);
      exit(0);
    } else if (arg[0] == '-' && arg[1] == 'j') {
      ptr_array_append(&g_make_args, (void*)arg);
    } else if (!strcmp(arg, "--assume-correct") || !strcmp(arg, "--aligned")) {
      ptr_array_append(&g_bench_args, (void*)arg);
    } else if (!strcmp(arg, "--samples")) {
      g_samples_mode = 1;
    } else {
      const char* eq = strchr(arg, '=');
      size_t n = eq ? (size_t)(eq - arg) : strlen(arg);
      cli_arg_t* m = match_arg(args, arg, n);
      if (m) {
        if (m->used == bench_arg) {
          ptr_array_append(&g_bench_args, (void*)arg);
          if (!eq && ++i < argc) {
            ptr_array_append(&g_bench_args, (void*)argv[i]);
          }
        } else {
          if (m->value && !m->used) {
            create_impls(isa.value, poly.value, algo.value);
            isa.used = 1, poly.used = 1, algo.used = 1;
          }
          if (eq) {
            m->value = eq + 1;
          } else if (++i < argc) {
            m->value = argv[i];
          } else {
            FATAL("missing value for option %.*s", (int)n, arg);
          }
          m->used = 0;
        }
      } else {
        FATAL("unknown option %.*s", (int)n, arg);
      }
    }
  }
  if (isa.value || poly.value || algo.value) {
    create_impls(isa.value, poly.value, algo.value);
  } else {
#if defined(__arm__) || defined(__arm) || defined(__ARM__) || defined(__ARM) || defined(__aarch64__) || defined(_M_ARM64)
    create_impls("neon,neon_eor3", "crc32c", "s1,s3,v1,v4,v12,v9s3x2k4096?");
#else
    create_impls("sse,avx512", "crc32c", "s1,s3,v1,v4,v4s3x3k4096?");
    create_impls("avx512_vpclmulqdq", "crc32c", "v3s1k4096?");
#endif
  }
}

static int cmp_impl_name(const void* lhs0, const void* rhs0) {
  impl_t* lhs = *(impl_t**)lhs0;
  impl_t* rhs = *(impl_t**)rhs0;
  int cmp = strcmp(lhs->name, rhs->name);
  if (cmp == 0) {
    cmp = lhs->original_order - rhs->original_order;
  }
  return cmp;
}

static int cmp_impl_original_order(const void* lhs0, const void* rhs0) {
  impl_t* lhs = *(impl_t**)lhs0;
  impl_t* rhs = *(impl_t**)rhs0;
  return lhs->original_order - rhs->original_order;
}

static void deduplicate_impls(void) {
  size_t i, j;
  impl_t* prev = NULL;
  qsort(g_impls.contents, g_impls.size, sizeof(void*), cmp_impl_name);
  for (i = j = 0; i < g_impls.size; ++i) {
    impl_t* impl = (impl_t*)g_impls.contents[i];
    if (!prev || strcmp(impl->name, prev->name)) {
      g_impls.contents[j++] = impl;
    }
    prev = impl;
  }
  g_impls.size = j;
  qsort(g_impls.contents, g_impls.size, sizeof(void*), cmp_impl_original_order);
}

static void generate_makefile(void) {
  size_t i, j;
#if defined(__MACH__) && defined(__APPLE__)
  const char* so_suffix = ".dylib";
  const char* cc_shared = "-dynamiclib";
#else
  const char* so_suffix = ".so";
  const char* cc_shared = "-shared";
#endif
  FILE* f = fopen(g_makefile_path, "wt");
  ptr_array_t cc_opt = {0};
  if (g_samples_mode) {
    fprintf(f, "run:");
    for (i = 0; i < g_impls.size; ++i) {
      impl_t* impl = (impl_t*)g_impls.contents[i];
      fprintf(f, " %s.c", impl->name);
    }
  } else {
    fprintf(f, "run: bench");
    for (i = 0; i < g_impls.size; ++i) {
      impl_t* impl = (impl_t*)g_impls.contents[i];
      fprintf(f, " %s%s", impl->name, so_suffix);
    }
    for (j = 0; j < g_impls.size; j += 100) {
      uint32_t limit = j + 100;
      if (limit > g_impls.size) limit = g_impls.size;
      fprintf(f, "\n\t./bench");
      for (i = 0; i < g_bench_args.size; ++i) {
        char* arg = (char*)g_bench_args.contents[i];
        fprintf(f, " %s", arg);
      }
      fprintf(f, " --");
      for (i = j; i < limit; ++i) {
        impl_t* impl = (impl_t*)g_impls.contents[i];
        fprintf(f, " ./%s%s", impl->name, so_suffix);
      }
    }
  }
  fprintf(f, "\n\n");
  ptr_array_append(&cc_opt, (void*)cc_shared);
  for (i = 0; i < g_impls.size; ++i) {
    impl_t* impl = (impl_t*)g_impls.contents[i];
    fprintf(f, "%s.c: generate\n", impl->name);
    fprintf(f, "\t./generate%s -o $@\n\n", impl->arguments);
    if (g_samples_mode) {
      continue;
    }
    fprintf(f, "%s%s: %s.c\n", impl->name, so_suffix, impl->name);
    cc_opt.size = 1;
    if (strstr(impl->arguments, "-i avx") || strstr(impl->arguments, "-i sse")) {
      ptr_array_append(&cc_opt, (void*)"-msse4.2 -mpclmul");
      if (strstr(impl->arguments, "-i avx512")) {
        ptr_array_append(&cc_opt, (void*)"-mavx512f -mavx512vl");
        if (strstr(impl->arguments, "-i avx512_vpclmulqdq")) {
          ptr_array_append(&cc_opt, (void*)"-mvpclmulqdq");
        }
      }
    }
#if !(defined(__MACH__) && defined(__APPLE__))
    if (strstr(impl->arguments, "-i neon")) {
      if (strstr(impl->arguments, "-i neon_eor3")) {
        ptr_array_append(&cc_opt, (void*)"-march=armv8.2-a+crypto+sha3");
      } else {
        ptr_array_append(&cc_opt, (void*)"-march=armv8-a+crypto+crc");
      }
    }
#endif    
    fprintf(f, "\t$(CC) $(CCOPT)");
    for (j = 0; j < cc_opt.size; ++j) {
      fprintf(f, " %s", (const char*)cc_opt.contents[j]);
    }
    fprintf(f, " -o $@ $<\n\n");
  }
  fprintf(f, "include Makefile\n");
  fclose(f);
}

static void init_make_args(const char* self_path) {
  char* dirsep = strrchr(self_path, '/');
  ptr_array_append(&g_make_args, (void*)"make");
  if (dirsep && !(dirsep == self_path + 1 && self_path[0] == '.')) {
    size_t dirlen = (dirsep + 1 - self_path);
    size_t bothlen = dirlen + strlen(g_makefile_path) + 1;
    char* joined = (char*)malloc(bothlen * 2);
    char* separate = joined + bothlen;
    memcpy(joined, self_path, dirlen);
    memcpy(joined + dirlen, g_makefile_path, bothlen - dirlen);
    g_makefile_path = joined;
    memcpy(separate, joined, bothlen);
    separate[dirlen - 1] = 0;
    ptr_array_append(&g_make_args, (void*)"-C");
    ptr_array_append(&g_make_args, (void*)separate);
    ptr_array_append(&g_make_args, (void*)"-f");
    ptr_array_append(&g_make_args, (void*)(separate + dirlen));
  } else {
    ptr_array_append(&g_make_args, (void*)"-f");
    ptr_array_append(&g_make_args, (void*)g_makefile_path);
  }
}

static void exec_make(void) {
  ptr_array_append(&g_make_args, (void*)"run");
  ptr_array_append(&g_make_args, NULL);
  execvp((const char*)g_make_args.contents[0], (char* const*)g_make_args.contents);
  FATAL("failed to exec: make %s", g_makefile_path);
}

int main(int argc, const char* const* argv) {
  init_make_args(argv[0]);
  parse_args(argc, argv);
  deduplicate_impls();
  generate_makefile();
  exec_make();
  return 0;
}
