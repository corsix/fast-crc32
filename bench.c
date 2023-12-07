/* MIT licensed; see LICENSE.md */
#include <dlfcn.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int      g_check_correctness = 1;
static uint64_t g_bench_duration    = 200000000u; /* nanoseconds */
static size_t   g_bench_size        = 512 * 1024; /* bytes */
static uint32_t g_bench_rounds      = 5;
static uint32_t g_bench_misalign    = 63;         /* byte mask */

static void print_help(FILE* f, const char* self) {
#if defined(__MACH__) && defined(__APPLE__)
  const char* so_suffix = ".dylib";
#else
  const char* so_suffix = ".so";
#endif
  if (!self) self = "./bench";
  fprintf(f, "Usage: %s [OPTION]... DYLIB...\n", self);
  fprintf(f, "Benchmark compiled CRC32 implementations.\n");
  fprintf(f, "Example: %s ./crc32c_s1%s ./crc32k_v4%s\n\n", self, so_suffix, so_suffix);
  fprintf(f, "Options:\n");
  fprintf(f, "  -r, --rounds=N     (default: %u)\n", (unsigned)g_bench_rounds);
  fprintf(f, "  -d, --duration=N   (default: %ums)\n", (unsigned)(g_bench_duration / 1000000u));
  fprintf(f, "  -s, --size=N       (default: %uKiB)\n", (unsigned)(g_bench_size >> 10));
  fprintf(f, "  -f, --format=human|csv\n");
  fprintf(f, "      --aligned\n");
  fprintf(f, "      --assume-correct\n");
  fprintf(f, "\nSee https://github.com/corsix/fast-crc32/\n");
}

#define FATAL(fmt, ...) \
  (fprintf(stderr, "FATAL error at %s:%d - " fmt "\n", __FILE__, __LINE__, ## __VA_ARGS__), fflush(stderr), exit(1))

#if !defined(UNLIKELY) && !defined(_MSC_VER)
#define UNLIKELY(x)	__builtin_expect(!!(x), 0)
#endif
#ifndef UNLIKELY
#define UNLIKELY(x) (x)
#endif
#if !defined(NOINLINE) && !defined(_MSC_VER)
#define NOINLINE __attribute__((noinline))
#endif
#ifndef NOINLINE
#define NOINLINE
#endif

/* Command line parsing. */

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

static uint64_t parse_duration(const char* value) {
  uint64_t result = 0;
  int i = 0, dp = 0, dot = 0;
  char c;
  while ((c = value[i++])) {
    if (c == '.') {
      if (dot) FATAL("invalid duration %s", value);
      dot = 1;
    } else if ('0' <= c && c <= '9') {
      result = result * 10 + (c - '0');
      dp += dot;
    } else {
      const char* unit = value + i - 1;
      if (*unit == ' ') ++unit;
      if (!strcmp(unit, "ns") || !strcmp(unit, "nanos")) {}
      else if (!strcmp(unit, "us") || !strcmp(unit, "micros")) dp -= 3;
      else if (!strcmp(unit, "ms") || !strcmp(unit, "millis")) dp -= 6;
      else if (!strcmp(unit, "s") || !strcmp(unit, "secs")) dp -= 9;
      else FATAL("invalid duration %s", value);
      break;
    }
  }
  for (; dp > 0; --dp) {
    result = result / 10u;
  }
  for (; dp < 0; ++dp) {
    result = result * 10u;
  }
  return result;
}

static uint64_t parse_size(const char* value) {
  uint64_t result = 0;
  int i = 0;
  char c;
  while ((c = value[i++])) {
    if ('0' <= c && c <= '9') {
      result = result * 10 + (c - '0');
    } else {
      if (c == ' ') {
        c = value[i++];
      }
      if (c == 'k' || c == 'K') result <<= 10;
      else if (c == 'm' || c == 'M') result <<= 20;
      else if (c == 'g' || c == 'G') result <<= 30;
      else if (c) FATAL("invalid size %s", value);
      if (c) {
        c = value[i++];
      }
      if (c == 'i') {
        c = value[i++];
      }
      if (c == 'b' || c == 'B') {
        c = value[i++];
      }
      if (c) FATAL("invalid size %s", value);
      break;
    }
  }
  return result;
}

static uint32_t parse_rounds(const char* value) {
  int res = atoi(value);
  if (res <= 0) res = 0;
  return (uint32_t)(unsigned)res;
}

static const char* g_sep = ": ";
static const char* g_gb_suffix = " GB/s";

static void parse_format(const char* value) {
  if (!value || !*value || !strcmp(value, "human")) {
    /* Nothing to do */
  } else if (!strcmp(value, "csv")) {
    g_sep = ",";
    g_gb_suffix = "";
  } else {
    FATAL("unknown format %s", value);
  }
}

typedef uint32_t (*crc_fn_t)(uint32_t, const char*, size_t);

static const char** parse_args(int argc, const char* const* argv) {
#define ARGS \
  DEF_ARG(duration, "-d") \
  DEF_ARG(size, "-s") \
  DEF_ARG(rounds, "-r") \
  DEF_ARG(format, "-f")
#define DEF_ARG(name, ...) static const char* name##_spellings[] = {"--" #name, __VA_ARGS__, NULL};
  ARGS
#undef DEF_ARG
#define DEF_ARG(name, ...) cli_arg_t a_##name = {name##_spellings, NULL};
  ARGS
#undef DEF_ARG
#define DEF_ARG(name, ...) &a_##name,
  cli_arg_t* args[] = { ARGS NULL };
#undef DEF_ARG
#undef ARGS
  const char** paths = (const char**)malloc(argc * sizeof(const char*));
  int i, seen_dash_dash = 0, n_paths = 0;

  for (i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (arg[0] == '-' && !seen_dash_dash) {
      if (!strcmp(arg, "--")) seen_dash_dash = 1;
      else if (!strcmp(arg, "--assume-correct")) g_check_correctness = 0;
      else if (!strcmp(arg, "--aligned")) g_bench_misalign = 0;
      else if (!strcmp(arg, "--help") || !strcmp(arg, "-h") || !strcmp(arg, "-?")) {
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
    } else {
      paths[n_paths++] = arg;
    }
  }
  paths[n_paths] = NULL;

  if (a_duration.value) g_bench_duration = parse_duration(a_duration.value);
  if (a_size.value) g_bench_size = parse_size(a_size.value);
  if (a_rounds.value) g_bench_rounds = parse_rounds(a_rounds.value);
  parse_format(a_format.value);
  return paths;
}

/* Correctness-checking. */
/* There is no point in being fast but wrong. */

static char* g_buf;

#define CHECK_BUF_SIZE (4096+64)

static void check_impl(const char* name, crc_fn_t fn) {
  static uint32_t table[256] = {0};
  static uint32_t table_poly = 0;
  uint32_t i, entire, expected = ~(uint32_t)0;
  /* Build a table for checking this fn. */
  uint32_t poly = ~fn(~(uint32_t)0, "\x80", 1);
  if (poly != table_poly) {
    table_poly = poly;
    for (i = 0; i < 256; ++i) {
      uint32_t crc = i, j;
      for (j = 0; j < 8; ++j) {
        crc = (crc >> 1) ^ ((crc & 1) * poly);
      }
      table[i] = crc;
    }
  }
  /* Actually check this fn. */
  entire = fn(0, g_buf, CHECK_BUF_SIZE);
  for (i = 0; i < CHECK_BUF_SIZE; ) {
    uint32_t actual = fn(0, g_buf, i + 1);
    expected = (expected >> 8) ^ table[(expected ^ g_buf[i]) & 0xFF];
    ++i;
    if (UNLIKELY(~expected != actual)) {
      FATAL("bad impl %s (expected %08x but got %08x for %d bytes)", name,
        (unsigned)~expected, (unsigned)actual, (int)i);
    }
    actual = fn(actual, g_buf + i, CHECK_BUF_SIZE - i);
    if (UNLIKELY(actual != entire)) {
      FATAL("bad impl %s (whole buffer gives %08x, but split at byte %d gives %08x)", name,
        (unsigned)entire, (int)i, (unsigned)actual);
    }
  }
}

/* Actual benchmarking logic. */

#define barrier __sync_synchronize()

static uint64_t now(void) {
#if defined(__MACH__) && defined(__APPLE__)
  return clock_gettime_nsec_np(CLOCK_MONOTONIC);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
}

volatile uint32_t g_sink;

static NOINLINE double bench_fn(crc_fn_t fn, char* buf, size_t size) {
  uint32_t i = 0, thr = (1u << 27) / (size + 1024);
  uint32_t misalign = g_bench_misalign;
  uint64_t t0;
  uint32_t crc = fn(fn(0, NULL, 0), buf, size);
  barrier;
  t0 = now();
  barrier;
  for (;;) {
    crc = fn(crc, buf + (i & misalign), size);
    if (UNLIKELY(++i >= thr)) {
      uint64_t elapsed = now() - t0;
      if (elapsed > g_bench_duration) {
        g_sink = crc;
        barrier;
        elapsed = now() - t0;
        barrier;
        return (double)(i * size) / (double)elapsed;
      } else {
        thr = (i * g_bench_duration) / (elapsed + 20000u);
        if (UNLIKELY(thr < i)) thr = 0; else thr -= i;
        if (i < thr) thr = thr / 2;
        thr += thr / 32;
        thr += i;
      }
    }
  }
}

static void bench_impl(const char* name, crc_fn_t fn) {
  char* ptr = g_buf;
  if (!g_bench_misalign) {
    ptr = ptr + 64 - (63 & (uintptr_t)ptr);
  }
  double best = 0.;
  uint32_t r = g_bench_rounds;
  do {
    double rate = bench_fn(fn, ptr, g_bench_size);
    if (rate > best) best = rate;
  } while (--r);
  printf("%s%s%.2f%s\n", name, g_sep, best, g_gb_suffix);
}

/* Putting it all together. */

static void bench_path(const char* path) {
  const char* fn_name = "crc32_impl";
  const char* colon = strchr(path, ':');
  void* lib;
  const char* name = path + 2 * (path[0] == '.' && path[1] == '/');
  crc_fn_t fn;
  if (colon) {
    char* mut = strdup(path);
    colon = mut + (colon - path);
    path = mut;
    *(char*)colon = '\0';
    fn_name = colon + 1;
  }
  lib = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
  if (UNLIKELY(!lib)) FATAL("could not dlopen %s (%s)", path, dlerror());
  fn = (crc_fn_t)dlsym(lib, fn_name);
  if (UNLIKELY(!fn)) FATAL("could not find function %s in %s", fn_name, path);

  if (g_check_correctness) check_impl(name, fn);
  if (g_bench_rounds) bench_impl(name, fn);

  if (colon) {
    free((char*)path);
  }
  dlclose(lib);
}

static void rand_fill(char* buf, size_t n) {
  size_t i;
  for (i = 0; i < n; ++i) {
    buf[i] = rand();
  }
}

static void alloc_buf(void) {
  uint32_t size = g_bench_size + 64;
  if (size < g_bench_size) FATAL("buffer size overflow");
  if (size < CHECK_BUF_SIZE) size = CHECK_BUF_SIZE;
  g_buf = malloc(size);
  rand_fill(g_buf, size);
}

static jmp_buf g_catch_signal;
static void signal_handler(int signal) {
  longjmp(g_catch_signal, signal);
}

int main(int argc, const char* const* argv) {
  volatile int status = EXIT_SUCCESS;
  const char** paths = parse_args(argc, argv);
  volatile uint32_t i;
  struct sigaction sa;
  if (UNLIKELY(!paths[0])) FATAL("no inputs specified");
  alloc_buf();
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_handler;
  sa.sa_flags = SA_NODEFER;
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGSEGV, &sa, NULL);
  for (i = 0; paths[i]; ++i) {
    const char* path = paths[i];
    int sig;
    if (UNLIKELY(sig = setjmp(g_catch_signal))) {
      const char* what = sig == SIGILL  ? "illegal instruction" :
                         sig == SIGSEGV ? "segfault" : "signal";
      if (sig != SIGILL) status = EXIT_FAILURE;
      if (path[0] == '.' && path[1] == '/') path += 2;
      printf("%s%s%s!\n", path, g_sep, what);
    } else {
      bench_path(path);
    }
    fflush(stdout);
  }
  return status;
}
