CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wpedantic -Iinclude -Isrc
LDLIBS = -lm

.PHONY: test test-ui contract bench sanitize arm clean

test: build/host_sim test-ui
	./build/host_sim

build/host_sim: src/work_core.c src/work_core.h test/host_sim.c include/plugin_api_v1.h
	@mkdir -p build
	$(CC) $(CFLAGS) src/work_core.c test/host_sim.c -o $@ $(LDLIBS)

# The UI harness mocks the host against a contract generated FROM the engine,
# so a UI that reads a key the DSP does not serve fails here rather than on
# hardware. Regenerate the fixture on every run — a stale one proves nothing.
contract: build/contract.json

build/contract.json: build/dump_contract
	./build/dump_contract > $@

build/dump_contract: src/work_core.c src/work_core.h test/dump_contract.c
	@mkdir -p build
	$(CC) $(CFLAGS) src/work_core.c test/dump_contract.c -o $@ $(LDLIBS)

test-ui: build/contract.json
	node --no-warnings --experimental-vm-modules test/ui_overtake.mjs

bench: build/benchmark
	./build/benchmark

build/benchmark: src/work_core.c src/work_core.h test/benchmark.c include/plugin_api_v1.h
	@mkdir -p build
	$(CC) $(CFLAGS) src/work_core.c test/benchmark.c -o $@ $(LDLIBS)

sanitize:
	@mkdir -p build
	$(CC) -O1 -g -Wall -Wextra -Wpedantic -Iinclude -Isrc \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		src/work_core.c test/host_sim.c -o build/host_sim_san $(LDLIBS)
	ASAN_OPTIONS=detect_leaks=0 ./build/host_sim_san

arm:
	./scripts/build.sh

clean:
	rm -rf build
