PYTHON ?= python3

ABIDES_VENV ?= .venv-abides
ABIDES_PYTHON ?= $(ABIDES_VENV)/bin/python
ABIDES_SCENARIO ?= rmsc04
ABIDES_SEED ?= 0
ABIDES_END_TIME ?= 12:00:00
ABIDES_READY := $(ABIDES_VENV)/.ready
PACING_INGRESS ?= tcp://127.0.0.1:5557
CALCULATION_ENGINE_EGRESS ?= tcp://127.0.0.1:5558
PACING_CONTROL ?= tcp://127.0.0.1:5559
METRICS_INGRESS ?= tcp://127.0.0.1:5560
PACING_SPEED ?= 100000
METRICS_BRIDGE_HOST ?= 127.0.0.1
METRICS_BRIDGE_PORT ?= 8765
METRICS_BRIDGE_HISTORY ?= 10000
METRICS_INPUT ?= -
METRICS_UI_DIR ?= components/metrics-ui
METRICS_HOLD ?= 0

FULL_SYSTEM_STATE ?= state/full-system

.PHONY: install-dev install-lint lint lint-fix format-cpp test build-calculation-engine run-calculation-engine run-metrics-bridge abides-jpmc-install abides-jpmc-sim run run-all clean

install-dev:
	python -m pip install -r requirements-dev.txt
	python -m pip install --no-build-isolation -e packages/market-data-protocol
	python -m pip install --no-build-isolation -e components/abides/extensions/market_data_emitter
	python -m pip install --no-build-isolation -e components/pacing-server
	python -m pip install --no-build-isolation -e components/metrics-bridge

install-lint:
	python -m pip install -r requirements-lint.txt

# Same two commands the CI lint job runs, at the same pinned versions, so a
# clean run here means a clean run there.
lint: install-lint
	ruff check .
	ruff format --check --diff .

lint-fix: install-lint
	ruff check --fix .
	ruff format .

# Not wired into CI. The C++ predates .clang-format, so CI only enforces the
# style on lines a change touches; this target reformats the whole tree when
# that ~1900-line diff is worth taking deliberately.
format-cpp: install-lint
	find components/calculation-engine \
		\( -name '*.cpp' -o -name '*.hpp' \) \
		-not -path '*/build/*' -print0 | xargs -0 clang-format -i

# The same three paths CI runs. A bare `pytest` from the root collects the
# vendored ABIDES fork's own test tree, which needs the simulator's full
# dependency set and is not this repository's to keep green.
test:
	pytest \
		packages/market-data-protocol/tests \
		components/pacing-server/tests \
		components/metrics-bridge/tests

build-calculation-engine:
	cmake -S components/calculation-engine -B build/calculation-engine -DCMAKE_BUILD_TYPE=Release
	cmake --build build/calculation-engine -j
	ctest --test-dir build/calculation-engine --output-on-failure

run-calculation-engine: build-calculation-engine
	./build/calculation-engine/calculation_engine_service \
		--endpoint $(CALCULATION_ENGINE_EGRESS)

run-metrics-bridge:
	PYTHONPATH=components/metrics-bridge/src $(PYTHON) -m metrics_bridge.cli \
		--input $(METRICS_INPUT) \
		--follow \
		--host $(METRICS_BRIDGE_HOST) \
		--port $(METRICS_BRIDGE_PORT) \
		--history-size $(METRICS_BRIDGE_HISTORY) \
		$(if $(METRICS_UI_DIR),--ui-dir $(METRICS_UI_DIR),)

$(ABIDES_READY):
	$(PYTHON) -m venv --system-site-packages $(ABIDES_VENV)
	$(ABIDES_PYTHON) -m pip install numpy pandas scipy psutil coloredlogs termcolor pyzmq msgpack
	touch $(ABIDES_READY)

abides-jpmc-install: $(ABIDES_READY)

abides-jpmc-sim: $(ABIDES_READY)
	$(ABIDES_PYTHON) scripts/run_abides_sim.py \
		--scenario $(ABIDES_SCENARIO) \
		--seed $(ABIDES_SEED) \
		--end-time $(ABIDES_END_TIME)

run: run-all

run-all: build-calculation-engine $(ABIDES_READY)
	$(ABIDES_PYTHON) scripts/run_full_system.py \
		--engine build/calculation-engine/calculation_engine_service \
		--scenario $(ABIDES_SCENARIO) \
		--seed $(ABIDES_SEED) \
		--end-time $(ABIDES_END_TIME) \
		--ingress $(PACING_INGRESS) \
		--egress $(CALCULATION_ENGINE_EGRESS) \
		--control $(PACING_CONTROL) \
		--metrics-ingress $(METRICS_INGRESS) \
		--pacing-speed $(PACING_SPEED) \
		--metrics-host $(METRICS_BRIDGE_HOST) \
		--metrics-port $(METRICS_BRIDGE_PORT) \
		--metrics-history-size $(METRICS_BRIDGE_HISTORY) \
		--state-dir $(FULL_SYSTEM_STATE) \
		$(if $(METRICS_UI_DIR),--ui-dir $(METRICS_UI_DIR),--no-ui) \
		$(if $(filter 1,$(METRICS_HOLD)),--hold,)

clean:
	rm -rf build .pytest_cache state
