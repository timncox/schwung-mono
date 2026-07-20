CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wpedantic -Iinclude -Isrc
LDLIBS = -lm

.PHONY: test test-ui bench sanitize arm clean

test: build/host_sim test-ui
	./build/host_sim

test-ui:
	node --no-warnings --experimental-vm-modules test/ui_overtake.mjs

build/host_sim: src/mono_core.c src/mono_core.h test/host_sim.c include/plugin_api_v1.h
	@mkdir -p build
	$(CC) $(CFLAGS) src/mono_core.c test/host_sim.c -o $@ $(LDLIBS)

bench: build/benchmark
	./build/benchmark

build/benchmark: src/mono_core.c src/mono_core.h test/benchmark.c include/plugin_api_v1.h
	@mkdir -p build
	$(CC) $(CFLAGS) src/mono_core.c test/benchmark.c -o $@ $(LDLIBS)

sanitize:
	@mkdir -p build
	$(CC) -O1 -g -Wall -Wextra -Wpedantic -Iinclude -Isrc \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		src/mono_core.c test/host_sim.c -o build/host_sim_san $(LDLIBS)
	ASAN_OPTIONS=detect_leaks=0 ./build/host_sim_san

arm:
	./scripts/build.sh

clean:
	rm -rf build
