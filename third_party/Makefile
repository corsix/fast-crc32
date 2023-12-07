CC= gcc
CCP= g++
CCOPT= -O3 -march=native

chromium_neon_crc32_s3k95760_s3.dylib: crc32.c crc32_simd.c crc32_simd.h
	$(CC) $(CCOPT) -DARMCRC32_CANONICAL_ZLIB=1 -dynamiclib -o $@ $<

chromium_neon_crc32_v4_v1.dylib: crc32.c crc32_simd.c crc32_simd.h
	$(CC) $(CCOPT) -DCRC32_ARMV8_CRC32 -dynamiclib -o $@ $<

dougallj_neon_crc32_v12e_v1.dylib: dougallj.cpp
	$(CCP) $(CCOPT) -std=c++17 -dynamiclib -o $@ $<

chromium_neon_crc32_s3k95760_s3.so: crc32.c crc32_simd.c crc32_simd.h
	$(CC) $(CCOPT) -DARMCRC32_CANONICAL_ZLIB=1 -shared -o $@ $<

chromium_neon_crc32_v4_v1.so: crc32.c crc32_simd.c crc32_simd.h
	$(CC) $(CCOPT) -DCRC32_ARMV8_CRC32 -shared -o $@ $<

dougallj_neon_crc32_v12e_v1.so: dougallj.cpp
	$(CCP) $(CCOPT) -march=armv8-a+crypto+crc -flax-vector-conversions -std=c++17 -shared -o $@ $<

chromium_sse_crc32_v4_v1.so: crc32.c crc32_simd.c crc32_simd.h
	$(CC) $(CCOPT) -DCRC32_SIMD_SSE42_PCLMUL -shared -o $@ $<

chromium_avx512_vpclmulqdq_crc32_v4_v1.so: crc32.c crc32_simd.c crc32_simd.h
	$(CC) $(CCOPT) -DCRC32_SIMD_SSE42_PCLMUL -DCRC32_SIMD_AVX512_PCLMUL -shared -o $@ $<

corsix4k_sse_crc32c_s3k4096e.so: corsix4k.c
	$(CC) $(CCOPT) -DKERNEL=crc32_4k_three_way -shared -o $@ $<

corsix4k_sse_crc32c_v4k4096e.so: corsix4k.c
	$(CC) $(CCOPT) -DKERNEL=crc32_4k_pclmulqdq -shared -o $@ $<

corsix4k_sse_crc32c_v4s3x3k4096e.so: corsix4k.c
	$(CC) $(CCOPT) -DKERNEL=crc32_4k_fusion -shared -o $@ $<
