CC= gcc
CCOPT= -O3 -Wall -Wextra -Wshadow -march=native

autobench_default: autobench
	./autobench

generate: generate.c
	$(CC) $(CCOPT) -o $@ $<

bench: bench.c
	$(CC) $(CCOPT) -o $@ $< -ldl

autobench: autobench.c
	$(CC) $(CCOPT) -o $@ $<

# Not sure what is going to be fastest? Run a sweep.
# It'll take a while, but try lots of things, and then print the best.
# A more targetted search can then be done around those, using a higher -r and -d.
sweep: autobench
	./autobench -r=1 -d=100ms -f=csv -i native -p crc32c -a v0:12x2?s0:3x2:4?k4096?e? --assume-correct | tee ab_sweep.csv
	grep -v ! ab_sweep.csv | sort -n -k 2 -t ',' | tail -10

test: autobench
	./autobench -r=0 -p crc32,crc32c,crc32k -a s1x2:3?k256?e?
	./autobench -r=0 -i native -p crc32c,crc32k -a s1:3x2:3?k4096?e?,s4e?_s1
	./autobench -r=0 -i native -p crc32c,crc32k -a v1:3x2:3?e?,v4e?_v1,v4k4096e,v4:16:2e?
	./autobench -r=0 -i native -p crc32c,crc32k -a v4s3x3:6:3?k4096?e?

samples: autobench
	./autobench --samples -i neon_eor3 -p crc32 -a v9s3x2e_s3 -i neon -p crc32 -a v3s4x2e_v2 -i avx512 -p crc32c -a v9s3x4e -i avx512_vpclmulqdq -p crc32c -a v4s5x3 -i avx512_vpclmulqdq -p crc32c -a v3s1_s3

clean:
	rm -rf ab_* sample_*
	rm -f generate bench autobench
