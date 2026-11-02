J ?= $(shell n=$$( (sysctl -n hw.ncpu 2>/dev/null || nproc) ); echo $$(( n/2 > 2 ? n/2 : 2 )))
BUILD ?= build
.PHONY: build test bench sanitize tsan fuzz lint demo data clean
build:
	cmake -S . -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Release -DMATCHBOX_NATIVE=ON
	cmake --build $(BUILD) -j $(J)
test: build
	ctest --test-dir $(BUILD) -j $(J) --output-on-failure
sanitize:
	cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMATCHBOX_SANITIZE=address,undefined -DMATCHBOX_BENCH=OFF
	cmake --build build-asan -j $(J) && ctest --test-dir build-asan -j $(J) --output-on-failure
tsan:
	cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMATCHBOX_SANITIZE=thread -DMATCHBOX_BENCH=OFF
	cmake --build build-tsan -j $(J) && ctest --test-dir build-tsan -j $(J) --output-on-failure
bench: build
	BUILD=$(BUILD) bash bench/run_bench.sh
FUZZ_CXX ?= clang++
fuzz:
	cmake -S . -B build-fuzz -G Ninja -DCMAKE_CXX_COMPILER=$(FUZZ_CXX) -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMATCHBOX_FUZZ=ON -DMATCHBOX_BENCH=OFF
	cmake --build build-fuzz --target fuzz_engine fuzz_protocol -j $(J)
	build-fuzz/fuzz_engine -max_total_time=$${FUZZ_SECONDS:-120} -rss_limit_mb=2048
	build-fuzz/fuzz_protocol -max_total_time=$${FUZZ_SECONDS:-60} -rss_limit_mb=2048
lint:
	@command -v clang-tidy >/dev/null && clang-tidy -p $(BUILD) src/*.cpp tools/*.cpp || echo "clang-tidy not installed; CI runs it"
data:
	@echo "matchbox needs no external data: order flow is generated (include/matchbox/gen.hpp)."
# Demo: start the exchange with a journal, drive it with the load generator, stop it, replay the
# journal and show the live and replayed event hashes match.
demo: build
	bash scripts/demo.sh
clean:
	rm -rf build build-asan build-tsan build-fuzz
