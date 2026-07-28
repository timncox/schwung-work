CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wpedantic -Iinclude -Isrc
LDLIBS = -lm

.PHONY: test test-ui test-site contract module-json bench sanitize arm clean

# module.json's ui_hierarchy is GENERATED from PARAM_NAME[][] in work_core.c,
# so adding or renaming a machine parameter cannot leave the Shadow UI showing
# a stale label. Run this after touching that table.
module-json: build/gen_hierarchy
	./build/gen_hierarchy > build/hierarchy.json
	@python3 -c "import json; \
	h=json.load(open('build/hierarchy.json')); \
	[ (lambda p: (json.dump({**json.load(open(p)), 'capabilities': {**json.load(open(p))['capabilities'], 'ui_hierarchy': h}}, open(p,'w'), indent=2, ensure_ascii=False), open(p,'a').write(chr(10))))(p) \
	  for p in ['modules/audio_fx/work/module.json','modules/sound_generators/work-in/module.json'] ]; \
	print('module.json ui_hierarchy regenerated:', len(h['levels']), 'levels')"

build/gen_hierarchy: src/work_core.c src/work_core.h test/gen_hierarchy.c
	@mkdir -p build
	$(CC) $(CFLAGS) src/work_core.c test/gen_hierarchy.c -o $@ $(LDLIBS)

test: build/host_sim test-ui test-site
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

# The manual site keeps its own copy of the machine table; this proves it still
# matches the engine's. A swapped label already slipped through once.
test-site: build/contract.json build/hierarchy.json
	node --no-warnings test/site_matches_engine.mjs

build/hierarchy.json: build/gen_hierarchy
	./build/gen_hierarchy > $@

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
