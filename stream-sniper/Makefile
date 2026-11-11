CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra -std=gnu11
CFLAGS   += -pthread -I../modules/http_client
LDLIBS   += -pthread $(shell pkg-config --libs libcurl 2>/dev/null || echo -lcurl)

# make debug: rebuild everything with ASan + UBSan and run the tests under
# them. Sanitizers are how a per-channel malloc that is never freed, or a
# fprintf between fork() and execvp, gets caught.
ifdef DEBUG
CFLAGS := -O1 -g -Wall -Wextra -std=gnu11 -pthread -fsanitize=address,undefined -fno-omit-frame-pointer
LDLIBS := -pthread $(shell pkg-config --libs libcurl 2>/dev/null || echo -lcurl) -fsanitize=address,undefined
endif

# make tsan: the concurrency half of `make debug`, and the only target that
# can see the defect class roadmap.md §3.10 is about - the poll thread
# handing a URL to a recorder thread through rec_slot_t. ASan and UBSan
# both stay silent on a pointer that two threads share without ordering:
# at worst the child is handed the wrong URL, at worst the URL leaks.
ifdef TSAN
ifneq ($(DEBUG),)
$(error DEBUG and TSAN are separate builds: run one of them at a time)
endif
CFLAGS := -O1 -g -Wall -Wextra -std=gnu11 -pthread -fsanitize=thread -fno-omit-frame-pointer
LDLIBS := -pthread $(shell pkg-config --libs libcurl 2>/dev/null || echo -lcurl) -fsanitize=thread
endif

# All build output lives under build/; the sources stay flat in the root
# (spec.md §6.1). ffmpeg-quiet and ffprobe are copied next to the binary so
# the /proc/self/exe resolution in resolve_ffmpeg_wrapper() keeps working.
BUILD    := build
TARGET   := $(BUILD)/stream-sniper

# The flags are part of the build state, not just of the link line: objects
# compiled with -fsanitize=address cannot be linked by a plain `make install`,
# and nothing else in the tree would notice that it is stale. The stamp is
# rewritten only when the flags really change, so an ordinary rebuild of an
# unchanged tree still costs nothing.
FLAGSTAMP := $(BUILD)/.flags

# Local sources (service-specific). http.c is a cross-service module living
# in modules/http_client/.
SRCS     := main.c config.c probe.c recorder.c twitch_auth.c ../modules/http_client/http.c log.c util.c jsonlite.c urlparse.c notify.c
OBJS     := $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o)))
DEPS     := $(OBJS:.o=.d)

ASSETS   := $(BUILD)/ffmpeg-quiet $(BUILD)/ffprobe

# Unit tests link against the real objects; main.o is the only
# one filtered out (it defines main()). test_probe is the exception:
# it compiles ../probe.c into itself with the HTTP, Twitch-auth and
# notification layers stubbed (fixtures for the first, no-ops for
# the last), so probe.o, http.o, twitch_auth.o and notify.o must
# stay out of its link (their symbols would collide with the stubs).
# test_recorder and test_notify are the opposite seam: they drive
# the real recorder against a fake yt-dlp on PATH - test_notify
# additionally points SS_NOTIFY_URL at a loopback server, so what
# stream-sniper POSTs can be read back.
TESTS     := test_util test_urlparse test_jsonlite test_probe test_http test_recorder test_notify
TESTBIN   := $(TESTS:%=$(BUILD)/%)
TEST_OBJS := $(filter-out $(BUILD)/main.o,$(OBJS))
TESTSRC   := $(TESTS:%=tests/%.c)
TESTOBJ   := $(TESTSRC:.c=$(BUILD)/tests/%.o)
TDEPS     := $(TESTOBJ:.o=.d)

PROBE_TEST_SKIP := $(BUILD)/probe.o $(BUILD)/http.o $(BUILD)/twitch_auth.o $(BUILD)/notify.o
PROBE_TEST_OBJS := $(filter-out $(PROBE_TEST_SKIP),$(TEST_OBJS))
PLAIN_TESTBIN   := $(filter-out $(BUILD)/test_probe,$(TESTBIN))

PREFIX     ?= /usr/local
BINDIR     := $(PREFIX)/bin
LIBEXECDIR := $(PREFIX)/libexec/stream-sniper

.PHONY: all clean debug tsan test install uninstall FORCE

all: $(TARGET)

$(TARGET): $(OBJS) $(ASSETS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

# Recomputed on every run, but mv'd into place only when the content differs,
# so the stamp's mtime moves exactly when the flags do - which is the moment
# every object has to be rebuilt.
$(FLAGSTAMP): FORCE | $(BUILD)
	@printf 'CC=%s\nCFLAGS=%s\nLDLIBS=%s\n' '$(CC)' '$(CFLAGS)' '$(LDLIBS)' > $@.tmp
	@cmp -s $@.tmp $@ || mv -f $@.tmp $@
	@rm -f $@.tmp

FORCE:

# Static pattern rule: each asset depends on the like-named file in the
# root. Writing $(ASSETS): %: % would make each asset depend on itself.
$(ASSETS): $(BUILD)/%: % | $(BUILD)
	cp $< $@
	chmod +x $@

$(BUILD) $(BUILD)/tests:
	mkdir -p $@

# Local sources.
$(BUILD)/%.o: %.c $(FLAGSTAMP) | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Module source: http.c lives in modules/http_client/.
$(BUILD)/http.o: ../modules/http_client/http.c $(FLAGSTAMP) | $(BUILD)
	$(CC) $(CFLAGS) -I. -MMD -MP -c $< -o $@

# $(ASSETS) is a prerequisite of `test`, not just of the binary: test_recorder
# resolves its ffmpeg wrapper the way the program does, by looking beside
# /proc/self/exe, so the wrappers have to be in build/ when it runs.
test: $(TESTBIN) $(ASSETS)
	@set -e; for t in $(TESTBIN); do ./$$t; done

$(PLAIN_TESTBIN): $(BUILD)/%: $(BUILD)/tests/%.o $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $(BUILD)/tests/$*.o $(TEST_OBJS) $(LDLIBS)

$(BUILD)/test_probe: $(BUILD)/tests/test_probe.o $(PROBE_TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $(BUILD)/tests/test_probe.o $(PROBE_TEST_OBJS) $(LDLIBS)

$(BUILD)/tests/%.o: tests/%.c $(FLAGSTAMP) | $(BUILD)/tests
	$(CC) $(CFLAGS) -MMD -MP -I. -c $< -o $@

debug: clean
	$(MAKE) DEBUG=1 all test

tsan: clean
	$(MAKE) TSAN=1 all test

install: all
	install -d $(BINDIR) $(LIBEXECDIR)
	install -m 755 $(TARGET) $(BINDIR)/stream-sniper
	install -m 755 $(ASSETS) $(LIBEXECDIR)/
	@echo "installed: $(BINDIR)/stream-sniper"
	@echo "wrappers:  $(LIBEXECDIR)/"
	@echo "hint: export SS_WRAPPER_DIR=$(LIBEXECDIR) so stream-sniper finds them"

uninstall:
	rm -f $(BINDIR)/stream-sniper
	rm -f $(LIBEXECDIR)/ffmpeg-quiet $(LIBEXECDIR)/ffprobe
	rmdir $(LIBEXECDIR) 2>/dev/null || true

clean:
	rm -rf $(BUILD)

-include $(DEPS) $(TDEPS)
