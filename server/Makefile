CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -pthread
LDFLAGS ?=
LDLIBS ?= -lsqlite3 -lcrypto -lpthread

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
OPENSSL_PREFIX ?= $(shell brew --prefix openssl@3 2>/dev/null)
SQLITE_PREFIX ?= $(shell brew --prefix sqlite 2>/dev/null)

ifneq ($(strip $(OPENSSL_PREFIX)),)
CPPFLAGS += -I$(OPENSSL_PREFIX)/include
LDFLAGS += -L$(OPENSSL_PREFIX)/lib
endif

ifneq ($(strip $(SQLITE_PREFIX)),)
CPPFLAGS += -I$(SQLITE_PREFIX)/include
LDFLAGS += -L$(SQLITE_PREFIX)/lib
endif
endif

SRC_DIR := src
INCLUDE_DIR := include
BUILD_DIR := build
TARGET := ott_server

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

.PHONY: clean run

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
