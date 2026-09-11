CC := gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -Iinclude
FCGI_LIBS := -lfcgi
BIN_DIR := bin_cgi

COMMON := common/json_util.c common/http_response.c
MYSQL_LIBS := -lmysqlclient
REDIS_LIBS := -lhiredis

.PHONY: all test e2e clean

all: $(BIN_DIR)/login $(BIN_DIR)/register $(BIN_DIR)/myfiles $(BIN_DIR)/md5

$(BIN_DIR)/login: src_cgi/login_cgi.c $(COMMON) common/md5.c common/user_validation.c common/user_repository.c common/token_service.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(FCGI_LIBS) $(MYSQL_LIBS) $(REDIS_LIBS)

$(BIN_DIR)/register: src_cgi/reg_cgi.c $(COMMON) common/md5.c common/user_validation.c common/user_repository.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(FCGI_LIBS) $(MYSQL_LIBS)

$(BIN_DIR)/myfiles: src_cgi/myfiles_cgi.c $(COMMON) common/user_validation.c common/token_service.c common/file_repository.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(FCGI_LIBS) $(MYSQL_LIBS) $(REDIS_LIBS)

$(BIN_DIR)/md5: src_cgi/md5_cgi.c $(COMMON) common/user_validation.c common/token_service.c common/file_repository.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(FCGI_LIBS) $(MYSQL_LIBS) $(REDIS_LIBS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

test: tests/test_json_util.c common/json_util.c
	$(CC) $(CFLAGS) $^ -o /tmp/ai_cloud_storage_tests
	/tmp/ai_cloud_storage_tests
	$(CC) $(CFLAGS) tests/test_auth_helpers.c common/md5.c common/user_validation.c -o /tmp/ai_cloud_storage_auth_tests
	/tmp/ai_cloud_storage_auth_tests

e2e:
	sh tests/e2e_auth.sh

clean:
	rm -rf $(BIN_DIR) /tmp/ai_cloud_storage_tests /tmp/ai_cloud_storage_auth_tests
