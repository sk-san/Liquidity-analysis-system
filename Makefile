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
PACING_SPEED ?= 100000

FULL_SYSTEM_STATE ?= state/full-system

.PHONY: install-dev test build-calculation-engine run-calculation-engine abides-jpmc-install abides-jpmc-sim run run-all clean

install-dev:
	python -m pip install -r requirements-dev.txt
	python -m pip install --no-build-isolation -e packages/market-data-protocol
	python -m pip install --no-build-isolation -e components/abides/extensions/market_data_emitter
	python -m pip install --no-build-isolation -e components/pacing-server

test:
	pytest

build-calculation-engine:
	cmake -S components/calculation-engine -B build/calculation-engine -DCMAKE_BUILD_TYPE=Release
	cmake --build build/calculation-engine -j
	ctest --test-dir build/calculation-engine --output-on-failure

run-calculation-engine: build-calculation-engine
	./build/calculation-engine/calculation_engine_service \
		--endpoint $(CALCULATION_ENGINE_EGRESS)

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
		--pacing-speed $(PACING_SPEED) \
		--state-dir $(FULL_SYSTEM_STATE)

clean:
	rm -rf build .pytest_cache state
