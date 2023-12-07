This repository:
  * Establishes [a terse syntax](#terse-syntax-for-describing-crc32-implementations) for describing a broad range of CRC32 implementations.
  * Contains a [code generator](generate.c) for turning terse descriptions into executable C code.
  * Contains benchmarking apparatus for CRC32 implementations, along with a range of [benchmark results](#benchmark-results).
  * Presents some novel CRC32C implementations which are faster than the previous state of the art.
    - Apple M1: [9.6% faster](#apple-m1-performance-single-core) (77.55 -> 85.05 GB/s)
    - Cascade Lake: [25% faster](#x86_64-cascade-lake-performance-single-core) (25.32 -> 31.55 GB/s)
    - Ice Lake: [56% faster](#x86_64-ice-lake-performance-single-core) (41.00 -> 63.98 GB/s)
    - Sapphire Rapids: [19% faster](#x86_64-sapphire-rapids-performance-single-core) (81.68 -> 97.30 GB/s)

Running `make` will generate and compile and benchmark a handful of different implementations.
Alternatively, run `make generate` and then use `./generate` to generate a particular implementation.

# Terse syntax for describing CRC32 implementations

## Step 1: Instruction set (-i)

| Terse syntax           | gcc equivalent                             | Example CPUs                                |
| ---------------------- | ------------------------------------------ | ------------------------------------------- |
| `-i neon`              | aarch64 `-march=armv8-a+crypto+crc`        | Apple M1, GCP Tau T2A (Ampere Altra Arm)    |
| `-i neon_eor3`         | aarch64 `-march=armv8.2-a+crypto+sha3`     | Apple M1                                    |
| `-i sse`               | x86_64 `-msse4.2 -mpclmul`                 | Any Intel or AMD CPU from the last 10 years |
| `-i avx512`            | x86_64 `-mavx512f -mavx512vl`              | Intel Skylake X and newer, AMD Zen 4        |
| `-i avx512_vpclmulqdq` | x86_64 `-mavx512f -mavx512vl -mvpclmulqdq` | Intel Ice Lake and newer, AMD Zen 4         |

For CRC32, the key feature required of any instruction set is a vector carryless multiplication instruction, e.g.
`pclmulqdq` on x86_64 or `pmull` on aarch64. Three-way exclusive-or is also useful, e.g. `vpternlogq` on x86_64 or
`eor3` on aarch64.

## Step 2: Choice of polynomial (-p)

| Terse syntax    | Polynomial | Hardware acceleration | Example applications      |
| --------------- | ---------- | --------------------- | ------------------------- |
| `-p crc32`      | 0x04C11DB7 | aarch64               | Ethernet, SATA, zlib, png |
| `-p crc32c`     | 0x1EDC6F41 | aarch64, x86_64       | iSCSI, Btrfs, ext4        |
| `-p crc32k`     | 0x741B8CD7 | no                    |                           |
| `-p crc32q`     | 0x814141AB | no                    | AIXM                      |
| `-p 0x87654321` | 0x87654321 | no                    | none                      |

Most new applications should probably use `-p crc32c`. A large number of existing applications use `-p crc32`. If you
really care about this, then you can also specify an arbitrary polynomial.

Some instruction sets contain scalar instructions for accelerating particular CRC32 polynomials. Notably, aarch64 has
scalar acceleration for `-p crc32` and `-p crc32c`, whereas x86_64 only has scalar acceleration for `-p crc32c`.

## Step 3: Algorithm string (-a)

Six different parameters control the generated algorithm:
  1. Number of vector accumulators to use (16 or 64 bytes each).
  2. Number of vector loads to perform per iteration (defaults to number of vector accumulators, and must be a multiple of the number of vector accumulators).
  3. Number of scalar accumulators to use (4 bytes each).
  4. Number of scalar 8-byte loads to perform per iteration (defaults to number of scalar accumulators, and must be a multiple of the number of scalar accumulators).
  5. The outer loop size, if any.
  6. Whether inner loop termination is length-based or pointer-based.

The number of vector accumulators is specified by the letter `v`, followed by a decimal number. If the number of vector loads per iteration is non-default, this is followed by the letter `x`, followed by the ratio of loads to accumulators as a decimal number. For example, `v4` denotes four vector accumulators and four vector loads per iteration, and `v3x2` denotes three vector accumulators and six vector loads per iteration.

The number of scalar accumulators is specified in a similar way, but with `s` instead of `v`. For example, `s3` denotes three scalar accumulators and three scalar loads per iteration, and `s5x3` denotes five scalar accumulators and 15 scalar loads per iteration.

An outer loop size (in bytes) can be specified by the letter `k`, followed by a decimal number. Using an outer loop slightly decreases the complexity of the inner loop (because the inner loop trip count becomes constant), but makes the implementation less well suited to input sizes that aren't a multiple of the outer loop size.

The inner loop condition can be based on the remaining length (the default), or based on pointer comparisons (specify the letter `e`). Length-based is easier for humans to comprehend. Pointer-based can make the inner loop slightly simpler, at the expense of slightly more logic outside the loop.

The various parameters are concatenated to give the algorithm string, for example:
  * `-a v4` to use four vector registers.
  * `-a v4x2s3x5k4096e` to use four vector registers (8 vector loads per iteration), three scalar registers (15 scalar loads per iteration), outer block size 4096 bytes, and pointer-based loop termination.

Multiple sets of parameters can be separated by an `_` character, for example `-a v4_v1` uses a `v4` algorithm for as long as possible, then switches to `v1` for any remaining bytes. If there are still any remaining bytes, then an implicit `_s1` is appended to deal with them.

# Benchmark results

## Apple M1 performance (single core)

| Implementation | Speed | Our equivalent | Speed |
| -------------- | ----: | -------------- | ----: |
| [Chromium scalar](https://github.com/chromium/chromium/blob/359c9827be9d69e59da9398d03d88ca6840dd450/third_party/zlib/crc32.c#L578-L672) | 25.03 GB/s | `-i neon -p crc32 -a s3k95760_s3` | 25.50 GB/s |
| [Chromium vector](https://github.com/chromium/chromium/blob/359c9827be9d69e59da9398d03d88ca6840dd450/third_party/zlib/crc32_simd.c#L483-L618) | 26.57 GB/s | `-i neon -p crc32 -a v4_v1` | 34.03 GB/s |
| [dougallj Faster CRC32](https://gist.github.com/dougallj/263f132023f590aec31a11bbc746b897) | 77.55 GB/s | `-i neon -p crc32 -a v12e_v1` | 77.69 GB/s |
| [fusion](https://www.corsix.org/content/fast-crc32c-4k)-inspired | N/A | [`-i neon_eor3 -p crc32 -a v9s3x2e_s3`](sample_neon_eor3_crc32_v9s3x2e_s3.c) | **85.05 GB/s** |

To reproduce these measurements:

```
$ make bench autobench
$ make -C third_party chromium_neon_crc32_s3k95760_s3.dylib chromium_neon_crc32_v4_v1.dylib dougallj_neon_crc32_v12e_v1.dylib
$ ./bench -r 40 third_party/chromium_neon_crc32_s3k95760_s3.dylib third_party/chromium_neon_crc32_v4_v1.dylib third_party/dougallj_neon_crc32_v12e_v1.dylib
$ ./autobench -r 40 -i neon -p crc32 -a s3k95760_s3,v4_v1,v12e_v1 -i neon_eor3 -a v9s3x2e_s3
```

[Rich microarchitecture details are available](https://dougallj.github.io/applecpu/firestorm.html), though the key
details for CRC32 distill down to the following table:

| Primitive                       | Uops | Load         | Scalar | Vector | Bytes | Score | Latency |
| ------------------------------- | ---: | -----------: | -----: | -----: | ----: | ----: | ------: |
| `ldr, pmull+eor, pmull2+eor`    |  5/8 | 1/3 or 0.5/3 |        |    2/4 |    16 |  25.6 |       6 |
| `ldr, pmull, pmull2, eor3`      |  4/8 | 1/3 or 0.5/3 |        |    3/4 |    16 |  21.3 |       5 |
| `ldr, pmull, pmull2, eor, eor`  |  5/8 | 1/3 or 0.5/3 |        |    4/4 |    16 |  16.0 |       7 |
| `ldr, crc32x`                   |  2/8 | 1/3 or 0.5/3 |    1/1 |        |     8 |   8.0 |       3 |

Score is computed as `Bytes / max(Uops, Load, Scalar, Vector)`, and gives the optimal bytes per cycle that we can
hope to achieve with the corresponding primitive. The compiler might fuse two consecutive `ldr` instructions into a
single `ldp` instruction, which reduces `Load` from `1/3` to `0.5/3`, but does _not_ decrease `Uops`. The CPU can
fuse `eor` into an immediately preceding `pmull` or `pmull2`, which decreases the demand on the vector execution
units, but such fused instructions still count as two for `Uops`. Chromium's scalar implementation is a decent
application of the `ldr, crc32x` primitive, with three scalar accumulators sufficient to match the latency of 3.
Chromium's vector implementation is a poor application of the `ldr, pmull, pmull2, eor, eor` primitive, as the four
vector accumulators are insufficient to match the latency of 7. Our `v4_v1` does better by using the
`ldr, pmull+eor, pmull2+eor` primitive, but the four vector accumulators are still insufficient to match the latency
of 6. Dougallj does a good application of the `ldr, pmull+eor, pmull2+eor` primitive, using 12 vector accumulators to
match the latency. The fun twist is the `ldr, pmull, pmull2, eor3` primitive; it scores worse than
`ldr, pmull+eor, pmull2+eor`, so if just one primitive is used, `ldr, pmull, pmull2, eor3` will lose out to
`ldr, pmull+eor, pmull2+eor`, but `ldr, pmull, pmull2, eor3` is less demanding on `Uops`, so a blend of
`ldr, pmull, pmull2, eor3` with `ldr, crc32x` can come out ahead. The `v9s3x2e_s3` implementation blends nine copies
of `ldr, pmull, pmull2, eor3` with six copies of `ldr, crc32x`, which gives Uops = 48/8, Scalar = 6/1, Vector = 27/4,
Bytes = 192, and thus a score of 28.4, which puts it just ahead of `ldr, pmull+eor, pmull2+eor`.

## GCP Tau T2A (Ampere Altra Arm) performance (single core)

| Implementation | Speed | Our equivalent | Speed |
| -------------- | ----: | -------------- | ----: |
| [Chromium scalar](https://github.com/chromium/chromium/blob/359c9827be9d69e59da9398d03d88ca6840dd450/third_party/zlib/crc32.c#L578-L672) | 22.68 GB/s | `-i neon -p crc32 -a s3k95760_s3` | 23.57 GB/s |
| [Chromium vector](https://github.com/chromium/chromium/blob/359c9827be9d69e59da9398d03d88ca6840dd450/third_party/zlib/crc32_simd.c#L483-L618) | 16.91 GB/s | `-i neon -p crc32 -a v4_v1` | 20.94 GB/s |
| [dougallj Faster CRC32](https://gist.github.com/dougallj/263f132023f590aec31a11bbc746b897) | 17.41 GB/s | `-i neon -p crc32 -a v12e_v1` | 21.81 GB/s |
| [fusion](https://www.corsix.org/content/fast-crc32c-4k)-inspired | N/A | [`-i neon -p crc32 -a v3s4x2e_v2`](sample_neon_crc32_v3s4x2e_v2.c) | **35.87 GB/s** |

To reproduce these measurements:

```
$ make bench autobench
$ make -C third_party chromium_neon_crc32_s3k95760_s3.so chromium_neon_crc32_v4_v1.so dougallj_neon_crc32_v12e_v1.so
$ ./bench -r 40 third_party/chromium_neon_crc32_s3k95760_s3.so third_party/chromium_neon_crc32_v4_v1.so third_party/dougallj_neon_crc32_v12e_v1.so
$ ./autobench -r 40 -i neon -p crc32 -a s3k95760_s3,v4_v1,v12e_v1,v3s4x2e_v2
```

The [CPU datasheet](https://uawartifacts.blob.core.windows.net/upload-files/Altra_Rev_A1_DS_v1_40_20230613_e002fe0be6.pdf) says:

> Four-wide superscalar aggressive out-of-order execution CPU

> Dual full-width (128-bit wide) SIMD execution pipes

Meanwhile, [microarchitecture measurements](https://github.com/ocxtal/insn_bench_aarch64/blob/master/results/gcp_ampere_altra.md) suggest that:
  * `crc32x` / `crc32cx` have latency 2, throughput 1
  * `pmull` has latency 2, throughput 1
  * vector `eor` has latency 2, throughput 0.5

These latency and throughput numbers are consistent with Chromium scalar being faster than Chromium vector. The optimal fusion also involves more bytes being processed by the scalar pipes (64 bytes per iteration) than the vector pipes (48 bytes per iteration).

## x86_64 Cascade Lake performance (single core)

Relevant `/proc/cpuinfo` of test machine:
```
vendor_id       : GenuineIntel
cpu family      : 6
model           : 85
model name      : Intel(R) Xeon(R) Silver 4214 CPU @ 2.20GHz
stepping        : 7
microcode       : 0x5003006
cpu MHz         : 2199.998
cache size      : 16384 KB
```

| Implementation | Speed | Our equivalent | Speed |
| -------------- | ----: | -------------- | ----: |
| [Chromium vector](https://github.com/chromium/chromium/blob/359c9827be9d69e59da9398d03d88ca6840dd450/third_party/zlib/crc32_simd.c#L214-L345) | 15.87 GB/s | `-i sse -p crc32 -a v4_v1` | 16.48 GB/s |
| [crc32_4k_three_way](https://www.corsix.org/content/fast-crc32c-4k) | 15.04 GB/s | `-i sse -p crc32c -a s3k4096e` | 15.11 GB/s |
| [crc32_4k_pclmulqdq](https://www.corsix.org/content/fast-crc32c-4k) | 15.78 GB/s | `-i sse -p crc32c -a v4k4096e` | 15.63 GB/s |
| retuned crc32_4k_pclmulqdq | N/A | `-i sse -p crc32c -a v4e` | 16.03 GB/s |
| AVX512 crc32_4k_pclmulqdq | N/A | `-i avx512 -p crc32c -a v4e` | 17.36 GB/s |
| [crc32_4k_fusion](https://www.corsix.org/content/fast-crc32c-4k) | 25.32 GB/s | `-i sse -p crc32c -a v4s3x3k4096e` | 25.42 GB/s |
| retuned crc32_4k_fusion | N/A | `-i sse -p crc32c -a v8s3x3` | 28.55 GB/s |
| AVX512 crc32_4k_fusion | N/A | [`-i avx512 -p crc32c -a v9s3x4e`](sample_avx512_crc32c_v9s3x4e.c) | **31.55 GB/s** |

To reproduce these measurements:

```
$ make bench autobench
$ make -C third_party chromium_sse_crc32_v4_v1.so corsix4k_sse_crc32c_s3k4096e.so corsix4k_sse_crc32c_v4k4096e.so corsix4k_sse_crc32c_v4s3x3k4096e.so
$ ./bench -r 40 third_party/chromium_sse_crc32_v4_v1.so third_party/corsix4k_sse_crc32c_s3k4096e.so third_party/corsix4k_sse_crc32c_v4k4096e.so third_party/corsix4k_sse_crc32c_v4s3x3k4096e.so
$ ./autobench -r 40 -i sse -p crc32 -a v4_v1 -i sse -p crc32c -a s3k4096e,v4k4096e,v4e -i avx512 -a v4e -i sse -a v4s3x3k4096e,v8s3x3 -i avx512 -a v9s3x4e
```

Note that Cascade Lake lacks VPCLMULQDQ, so the only useful part of AVX512 is VPTERNLOGQ, hence the only very limited performance increase from AVX512 here.

## x86_64 Ice Lake performance (single core)

Relevant `/proc/cpuinfo` of test machine:
```
vendor_id       : GenuineIntel
cpu family      : 6
model           : 106
model name      : Intel(R) Xeon(R) CPU @ 2.60GHz
stepping        : 6
microcode       : 0xffffffff
cpu MHz         : 2600.028
cache size      : 55296 KB
```

| Implementation | Speed | Our equivalent | Speed |
| -------------- | ----: | -------------- | ----: |
| [Chromium vector](https://github.com/chromium/chromium/blob/359c9827be9d69e59da9398d03d88ca6840dd450/third_party/zlib/crc32_simd.c#L214-L345) | 24.08 GB/s | `-i sse -p crc32 -a v4_v1` | 24.02 GB/s |
| [Chromium vector AVX512](https://github.com/chromium/chromium/blob/359c9827be9d69e59da9398d03d88ca6840dd450/third_party/zlib/crc32_simd.c#L24-L201) | 41.00 GB/s | `-i avx512_vpclmulqdq -p crc32 -a v4_v1` | 47.31 GB/s |
| [crc32_4k_three_way](https://www.corsix.org/content/fast-crc32c-4k) | 19.68 GB/s | `-i sse -p crc32c -a s3k4096e` | 19.80 GB/s |
| retuned crc32_4k_three_way | N/A | `-i sse -p crc32c -a s3` | 22.50 GB/s |
| [crc32_4k_pclmulqdq](https://www.corsix.org/content/fast-crc32c-4k) | 22.80 GB/s | `-i sse -p crc32c -a v4k4096e` | 22.80 GB/s |
| retuned crc32_4k_pclmulqdq | N/A | `-i sse -p crc32c -a v4e` | 24.05 GB/s |
| AVX512 crc32_4k_pclmulqdq | N/A | `-i avx512_vpclmulqdq -p crc32c -a v4x2e` | 47.50 GB/s |
| [crc32_4k_fusion](https://www.corsix.org/content/fast-crc32c-4k) | 35.12 GB/s | `-i sse -p crc32c -a v4s3x3k4096e` | 34.72 GB/s |
| retuned crc32_4k_fusion | N/A | `-i sse -p crc32c -a v7s3x3` | 42.58 GB/s |
| AVX512 crc32_4k_fusion | N/A | [`-i avx512_vpclmulqdq -p crc32c -a v4s5x3`](sample_avx512_vpclmulqdq_crc32c_v4s5x3.c) | **63.98 GB/s** |

To reproduce these measurements:

```
$ make bench autobench
$ make -C third_party chromium_sse_crc32_v4_v1.so chromium_avx512_vpclmulqdq_crc32_v4_v1.so corsix4k_sse_crc32c_s3k4096e.so corsix4k_sse_crc32c_v4k4096e.so corsix4k_sse_crc32c_v4s3x3k4096e.so
$ ./bench -r 40 third_party/chromium_sse_crc32_v4_v1.so third_party/chromium_avx512_vpclmulqdq_crc32_v4_v1.so third_party/corsix4k_sse_crc32c_s3k4096e.so third_party/corsix4k_sse_crc32c_v4k4096e.so third_party/corsix4k_sse_crc32c_v4s3x3k4096e.so
$ ./autobench -r 40 -i sse -p crc32 -a v4_v1 -i avx512_vpclmulqdq -p crc32 -a v4_v1 -i sse -p crc32c -a s3k4096e,s3,v4k4096e,v4e -i avx512_vpclmulqdq -a v4e -i sse -a v4s3x3k4096e,v7s3x3 -i avx512_vpclmulqdq -a v4s5x3
```

Ice Lake introduces VPCLMULQDQ, but VPCLMULQDQ on 64-byte registers is 1\*p05+2\*p5 (versus regular PCLMULQDQ on 16-byte registers being 1\*p5), so going from 16-byte to 64-byte registers only gives a ~2x speedup. Chromium vector AVX512 leaves some performance on the table due to not using VPTERNLOGQ.

## x86_64 Sapphire Rapids performance (single core)

Relevant `/proc/cpuinfo` of test machine:
```
vendor_id       : GenuineIntel
cpu family      : 6
model           : 143
model name      : Intel(R) Xeon(R) Platinum 8481C CPU @ 2.70GHz
stepping        : 8
microcode       : 0xffffffff
cpu MHz         : 2699.998
cache size      : 107520 KB
```

| Implementation | Speed | Our equivalent | Speed |
| -------------- | ----: | -------------- | ----: |
| [Chromium vector](https://github.com/chromium/chromium/blob/359c9827be9d69e59da9398d03d88ca6840dd450/third_party/zlib/crc32_simd.c#L214-L345) | 23.42 GB/s | `-i sse -p crc32 -a v4_v1` | 23.31 GB/s |
| [Chromium vector AVX512](https://github.com/chromium/chromium/blob/359c9827be9d69e59da9398d03d88ca6840dd450/third_party/zlib/crc32_simd.c#L24-L201) | 65.20 GB/s | `-i avx512_vpclmulqdq -p crc32 -a v4_v1` | 94.00 GB/s |
| (with aligned inputs) | 81.68 GB/s | `-i avx512_vpclmulqdq -p crc32 -a v4_v1` | 94.00 GB/s |
| [crc32_4k_three_way](https://www.corsix.org/content/fast-crc32c-4k) | 17.45 GB/s | `-i sse -p crc32c -a s3k4096e` | 17.59 GB/s |
| retuned crc32_4k_three_way | N/A | `-i sse -p crc32c -a s3` | 23.84 GB/s |
| [crc32_4k_pclmulqdq](https://www.corsix.org/content/fast-crc32c-4k) | 22.79 GB/s | `-i sse -p crc32c -a v4k4096e` | 22.79 GB/s |
| retuned crc32_4k_pclmulqdq | N/A | `-i sse -p crc32c -a v4e` | 23.29 GB/s |
| AVX512 crc32_4k_pclmulqdq | N/A | `-i avx512_vpclmulqdq -p crc32c -a v4e` | 94.84 GB/s |
| [crc32_4k_fusion](https://www.corsix.org/content/fast-crc32c-4k) | 33.87 GB/s | `-i sse -p crc32c -a v4s3x3k4096e` | 34.68 GB/s |
| retuned crc32_4k_fusion | N/A | `-i sse -p crc32c -a v8s3x3` | 36.98 GB/s |
| AVX512 crc32_4k_fusion | N/A | [`-i avx512_vpclmulqdq -p crc32c -a v3s1_s3`](sample_avx512_vpclmulqdq_crc32c_v3s1_s3.c) | **97.30 GB/s** |

To reproduce these measurements:

```
$ make bench autobench
$ make -C third_party chromium_sse_crc32_v4_v1.so chromium_avx512_vpclmulqdq_crc32_v4_v1.so corsix4k_sse_crc32c_s3k4096e.so corsix4k_sse_crc32c_v4k4096e.so corsix4k_sse_crc32c_v4s3x3k4096e.so
$ ./bench -r 40 third_party/chromium_sse_crc32_v4_v1.so third_party/chromium_avx512_vpclmulqdq_crc32_v4_v1.so third_party/corsix4k_sse_crc32c_s3k4096e.so third_party/corsix4k_sse_crc32c_v4k4096e.so third_party/corsix4k_sse_crc32c_v4s3x3k4096e.so
$ ./autobench -r 40 -i sse -p crc32 -a v4_v1 -i avx512_vpclmulqdq -p crc32 -a v4_v1 -i sse -p crc32c -a s3k4096e,s3,v4k4096e,v4e -i avx512_vpclmulqdq -a v4e -i sse -a v4s3x3k4096e,v8s3x3 -i avx512_vpclmulqdq -a v3s1_s3
```

Sapphire Rapids improves VPCLMULQDQ on 64-byte registers to 1\*p5, meaning that AVX512 implementations can approach 4x the performance of implementations using 16-byte vectors (and _slightly exceed_ 4x when also gaining VPTERNLOGQ). One catch is that we now observe a 20% performance penalty for unaligned 64-byte loads, which the Chromium code doesn't take care to handle.

# Related reading

Background mathematics:
* [Barrett reduction for polynomials](https://www.corsix.org/content/barrett-reduction-polynomials)
* [Polynomial remainders by direct computation](https://www.corsix.org/content/polynomial-remainders-direct-computation)

CRC implementations:
* [An alternative exposition of crc32_4k_pclmulqdq](https://www.corsix.org/content/alternative-exposition-crc32_4k_pclmulqdq)
* [Faster CRC32-C on x86](https://www.corsix.org/content/fast-crc32c-4k)
* [Faster CRC32 on the Apple M1](https://dougallj.wordpress.com/2022/05/22/faster-crc32-on-the-apple-m1/)
