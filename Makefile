CC := gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -Iinclude
FCGI_LIBS := -lfcgi
BIN_DIR := bin_cgi

COMMON := common/json_util.c common/http_response.c

.PHONY: all test clean

all: $(BIN_DIR)/login

$(BIN_DIR)/login: src_cgi/login_cgi.c $(COMMON) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(FCGI_LIBS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

test: tests/test_json_util.c common/json_util.c
	$(CC) $(CFLAGS) $^ -o /tmp/ai_cloud_storage_tests
	/tmp/ai_cloud_storage_tests

clean:
	rm -rf $(BIN_DIR) /tmp/ai_cloud_storage_tests
