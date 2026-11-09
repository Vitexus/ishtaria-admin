CMAKE ?= cmake
BUILD_DIR ?= build
CMAKE_BUILD_TYPE ?= Release
JOBS ?= $(shell nproc 2>/dev/null || echo 4)
PREFIX ?= /usr

# Local development server: ../ishtaria-server on a throw-away database.
SERVER_DIR ?= ../ishtaria-server
SERVER_BIN := $(SERVER_DIR)/target/debug/ishtaria-server
DEV_DB ?= ishtaria_dev
DEV_PORT ?= 7400
DEV_SEED ?= 42
DEV_PGM ?= ../ishtaria-worldgen/examples/planet-seed42-256.pgm
DEV_DIR := $(BUILD_DIR)/dev
DEV_CONFIG := $(DEV_DIR)/server.toml
DEV_LOG := $(DEV_DIR)/server.log
DEV_PID := $(DEV_DIR)/server.pid
DEV_URL := postgresql:///$(DEV_DB)?host=/var/run/postgresql
DEV_HEALTH := http://127.0.0.1:$(DEV_PORT)/health

.PHONY: all build test clean install package run stop status logs dev-reset dev-db help

all: build

build:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE)
	$(CMAKE) --build $(BUILD_DIR) --parallel $(JOBS)

test: build
	cd $(BUILD_DIR) && ctest --output-on-failure

clean: stop
	rm -rf $(BUILD_DIR)

install: build
	$(CMAKE) --install $(BUILD_DIR) --prefix $(PREFIX)

package:
	dpkg-buildpackage -us -uc -b

# --- local development server ---

$(DEV_CONFIG): $(SERVER_DIR)/etc/server.toml
	mkdir -p $(DEV_DIR)
	sed -e 's|^listen = .*|listen = "127.0.0.1:$(DEV_PORT)"|' \
	    -e 's|^server_name = .*|server_name = "localhost"|' \
	    -e 's|^database_url = .*|database_url = "$(DEV_URL)"|' $< > $@

dev-db:
	psql -lqt postgres | cut -d'|' -f1 | grep -qw $(DEV_DB) || createdb $(DEV_DB)

# Builds and starts the local dev server in the background; the first start
# imports the sample world. Manage it with stop/status/logs/dev-reset and point
# the admin tool at it with:
#   ./$(BUILD_DIR)/ishtaria-admin --database-url='$(DEV_URL)'
run: $(DEV_CONFIG) dev-db
	cd $(SERVER_DIR) && cargo build --locked
	@if [ -f $(DEV_PID) ] && kill -0 $$(cat $(DEV_PID)) 2>/dev/null; then \
	    echo "dev server already running (pid $$(cat $(DEV_PID)), $(DEV_HEALTH))"; exit 0; \
	fi
	@first=""; \
	if [ "$$(psql -tA $(DEV_DB) -c 'select exists(select 1 from worlds)' 2>/dev/null)" != "t" ]; then \
	    first="--import $(abspath $(DEV_PGM)) --seed $(DEV_SEED)"; \
	    echo "importing $(DEV_PGM) (seed $(DEV_SEED)) into $(DEV_DB)"; \
	fi; \
	DATABASE_URL='$(DEV_URL)' nohup $(abspath $(SERVER_BIN)) $(abspath $(DEV_CONFIG)) $$first > $(DEV_LOG) 2>&1 & echo $$! > $(DEV_PID); \
	for i in $$(seq 1 80); do \
	    kill -0 $$(cat $(DEV_PID)) 2>/dev/null || { echo "dev server exited, see $(DEV_LOG):"; tail -20 $(DEV_LOG); exit 1; }; \
	    curl -sf $(DEV_HEALTH) >/dev/null && { echo "dev server up: $(DEV_HEALTH) (database $(DEV_DB), log $(DEV_LOG))"; exit 0; }; \
	    sleep 0.5; \
	done; \
	echo "dev server did not answer $(DEV_HEALTH), see $(DEV_LOG):"; tail -20 $(DEV_LOG); exit 1

stop:
	@pkill -f '[i]shtaria-server $(abspath $(DEV_CONFIG))' 2>/dev/null || true
	@rm -f $(DEV_PID)
	@echo "dev server stopped"

status:
	@if [ -f $(DEV_PID) ] && kill -0 $$(cat $(DEV_PID)) 2>/dev/null; then echo "running (pid $$(cat $(DEV_PID)))"; else echo "not running"; fi; \
	curl -sf $(DEV_HEALTH) >/dev/null && echo "health: ok ($(DEV_HEALTH))" || echo "health: no answer on $(DEV_HEALTH)"

logs:
	tail -f $(DEV_LOG)

dev-reset: stop
	dropdb --if-exists $(DEV_DB)
	@echo "dropped $(DEV_DB); 'make run' recreates it and imports the sample world"

help:
	@echo "Targets:"
	@echo "  all (default)  configure and build into $(BUILD_DIR)"
	@echo "  test           build and run ctest (needs PostgreSQL and $(SERVER_BIN))"
	@echo "  run            start the local dev server on 127.0.0.1:$(DEV_PORT) (database $(DEV_DB); first start imports $(DEV_PGM))"
	@echo "  stop           stop the dev server"
	@echo "  status         dev server pid and health"
	@echo "  logs           follow the dev server log"
	@echo "  dev-reset      stop and drop the dev database"
	@echo "  clean          stop the dev server and remove $(BUILD_DIR)"
	@echo "  install        install into PREFIX (default $(PREFIX))"
	@echo "  package        build the .deb with dpkg-buildpackage"
	@echo "Variables: BUILD_DIR, CMAKE_BUILD_TYPE, JOBS, PREFIX, SERVER_DIR, DEV_DB, DEV_PORT, DEV_SEED, DEV_PGM"
