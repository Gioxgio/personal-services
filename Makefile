# Single Makefile for the merged repo (goal: "1 Makefile").
#
# One folder per service lives under services/ and keeps its own
# (duplicated) Makefile. This file builds, tests and cleans every service.
#
#   make            build all services
#   make <svc>      build one service  (download-service | notification-service | stream-sniper)
#   make test       run every service's test suite
#   make test-<svc> run one service's tests
#   make clean      clean every service
#   make clean-<svc> clean one service

SERVICES := download-service notification-service stream-sniper

.PHONY: all $(SERVICES) test clean modules test-% clean-%

all: $(SERVICES)

$(SERVICES):
	$(MAKE) -C $@ all

test: $(addprefix test-,$(SERVICES))
test-%:
	$(MAKE) -C $* test

clean: $(addprefix clean-,$(SERVICES))
clean-%:
	$(MAKE) -C $* clean

modules:
	@echo "shared module sources live under modules/ (copies of the JSON"
	@echo "serializer, HTTP server/client, Telegram, config reader and a"
	@echo "scrape stub); services remain self-contained copies."

.DEFAULT_GOAL := all
