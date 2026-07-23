PYTHON ?= python3

ABIDES_VENV ?= .venv-abides
ABIDES_PYTHON ?= $(ABIDES_VENV)/bin/python
ABIDES_SCENARIO ?= rmsc04
ABIDES_SEED ?= 0
ABIDES_END_TIME ?= 10:00:00
ABIDES_READY := $(ABIDES_VENV)/.ready

.PHONY: install-dev test build-calculation-engine abides-jpmc-install abides-jpmc-sim clean

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

clean:
	rm -rf build .pytest_cache state
