CC = cc
CFLAGS = -Wall -Wextra -Werror -O2 -std=c11 -MMD -MP -I../modules/json -I../modules/http_server -I../modules/telegram -I.
LDFLAGS = -lcurl

BUILD_DIR = build

TARGET = $(BUILD_DIR)/notification-service

# Local sources (service-specific).
LOCAL_SRCS = main.c download.c jobmap.c
# Cross-service modules (live in ../modules/).
MODULE_SRCS = ../modules/json/json.c ../modules/http_server/server.c ../modules/telegram/telegram.c
LOCAL_OBJS = $(addprefix $(BUILD_DIR)/,$(LOCAL_SRCS:.c=.o))
MODULE_OBJS = $(addprefix $(BUILD_DIR)/,$(notdir $(MODULE_SRCS:.c=.o)))
OBJS = $(LOCAL_OBJS) $(MODULE_OBJS)
DEPS = $(OBJS:.o=.d)

MODULE_CFLAGS = $(CFLAGS)

TEST_SRCS = tests/test_json.c tests/test_telegram.c tests/test_server.c \
            tests/test_download.c tests/test_jobmap.c
TEST_OBJS = $(TEST_SRCS:tests/%.c=$(BUILD_DIR)/%.o)
TEST_DEPS = $(TEST_OBJS:.o=.d)
TEST_CFLAGS = $(CFLAGS)

TEST_JSON_BIN      = $(BUILD_DIR)/test_json
TEST_JSON_OBJS     = $(BUILD_DIR)/test_json.o $(BUILD_DIR)/json.o

TEST_TELEGRAM_BIN  = $(BUILD_DIR)/test_telegram
TEST_TELEGRAM_OBJS = $(BUILD_DIR)/test_telegram.o $(BUILD_DIR)/json.o

TEST_SERVER_BIN    = $(BUILD_DIR)/test_server
TEST_SERVER_OBJS   = $(BUILD_DIR)/test_server.o $(BUILD_DIR)/telegram.o \
                     $(BUILD_DIR)/json.o $(BUILD_DIR)/download.o \
                     $(BUILD_DIR)/jobmap.o

TEST_DOWNLOAD_BIN  = $(BUILD_DIR)/test_download
TEST_DOWNLOAD_OBJS = $(BUILD_DIR)/test_download.o $(BUILD_DIR)/download.o \
                     $(BUILD_DIR)/json.o

TEST_JOBMAP_BIN    = $(BUILD_DIR)/test_jobmap
TEST_JOBMAP_OBJS   = $(BUILD_DIR)/test_jobmap.o $(BUILD_DIR)/jobmap.o

TEST_BINS = $(TEST_JSON_BIN) $(TEST_TELEGRAM_BIN) $(TEST_SERVER_BIN) \
            $(TEST_DOWNLOAD_BIN) $(TEST_JOBMAP_BIN)

all: $(TARGET) compile_commands.json

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

# Local sources.
$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Module sources.
$(BUILD_DIR)/json.o: ../modules/json/json.c | $(BUILD_DIR)
	$(CC) $(MODULE_CFLAGS) -c $< -o $@
$(BUILD_DIR)/server.o: ../modules/http_server/server.c | $(BUILD_DIR)
	$(CC) $(MODULE_CFLAGS) -c $< -o $@
$(BUILD_DIR)/telegram.o: ../modules/telegram/telegram.c | $(BUILD_DIR)
	$(CC) $(MODULE_CFLAGS) -c $< -o $@

# Tests.
$(BUILD_DIR)/%.o: tests/%.c | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

$(TEST_JSON_BIN): $(TEST_JSON_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

$(TEST_TELEGRAM_BIN): $(TEST_TELEGRAM_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

$(TEST_SERVER_BIN): $(TEST_SERVER_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

$(TEST_DOWNLOAD_BIN): $(TEST_DOWNLOAD_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

$(TEST_JOBMAP_BIN): $(TEST_JOBMAP_OBJS)
	$(CC) $^ -o $@

test: $(TEST_BINS)
	$(TEST_JSON_BIN)
	$(TEST_TELEGRAM_BIN)
	$(TEST_SERVER_BIN)
	$(TEST_DOWNLOAD_BIN)
	$(TEST_JOBMAP_BIN)

compile_commands.json: Makefile
	@rm -f $@.tmp
	@printf '[\n' > $@.tmp
	@first=1; \
	for f in $(LOCAL_SRCS) $(MODULE_SRCS) $(TEST_SRCS); do \
		case "$$f" in \
			tests/*) flags='$(TEST_CFLAGS)' ;; \
			../modules/*) flags='$(MODULE_CFLAGS)' ;; \
			*) flags='$(CFLAGS)' ;; \
		esac; \
		[ $$first -eq 0 ] && printf ',\n' >> $@.tmp; \
		first=0; \
		cmd="$(CC) -c $$flags $$f"; \
		printf '  { "directory": "%s", "command": "%s", "file": "%s" }' \
			'$(CURDIR)' "$$cmd" "$$f" >> $@.tmp; \
	done; \
	printf '\n]\n' >> $@.tmp
	@mv $@.tmp $@

clean:
	rm -rf $(BUILD_DIR)
	rm -f *.o *.d notification-service test_json test_telegram test_server \
	    test_download test_jobmap

-include $(DEPS) $(TEST_DEPS)

.PHONY: all clean test compile_commands.json
