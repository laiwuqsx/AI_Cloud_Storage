CC := gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -Iinclude
FCGI_LIBS := -lfcgi
BIN_DIR := bin_cgi

COMMON := common/json_util.c common/http_response.c common/runtime_config.c
MYSQL_LIBS := -lmysqlclient
REDIS_LIBS := -lhiredis

.PHONY: all integration-tools test e2e clean

all: $(BIN_DIR)/login $(BIN_DIR)/register $(BIN_DIR)/myfiles $(BIN_DIR)/md5 $(BIN_DIR)/dealfile $(BIN_DIR)/logout $(BIN_DIR)/upload $(BIN_DIR)/share

$(BIN_DIR)/login: src_cgi/login_cgi.c $(COMMON) common/md5.c common/user_validation.c common/user_repository.c common/token_service.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(FCGI_LIBS) $(MYSQL_LIBS) $(REDIS_LIBS)

$(BIN_DIR)/register: src_cgi/reg_cgi.c $(COMMON) common/md5.c common/user_validation.c common/user_repository.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(FCGI_LIBS) $(MYSQL_LIBS)

$(BIN_DIR)/myfiles: src_cgi/myfiles_cgi.c $(COMMON) common/md5.c common/user_validation.c common/token_service.c common/file_repository.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(FCGI_LIBS) $(MYSQL_LIBS) $(REDIS_LIBS)

$(BIN_DIR)/md5: src_cgi/md5_cgi.c $(COMMON) common/md5.c common/user_validation.c common/token_service.c common/file_repository.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(FCGI_LIBS) $(MYSQL_LIBS) $(REDIS_LIBS)

$(BIN_DIR)/dealfile: src_cgi/dealfile_cgi.c $(COMMON) common/md5.c common/user_validation.c common/token_service.c common/file_repository.c common/share_id.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(FCGI_LIBS) $(MYSQL_LIBS) $(REDIS_LIBS)

$(BIN_DIR)/logout: src_cgi/logout_cgi.c $(COMMON) common/md5.c common/user_validation.c common/token_service.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(FCGI_LIBS) $(REDIS_LIBS)

$(BIN_DIR)/upload: src_cgi/upload_cgi.c $(COMMON) common/md5.c common/user_validation.c common/token_service.c common/file_repository.c common/cleanup_repository.c common/upload_intake.c common/multipart_upload.c common/storage_client.c common/fastdfs_storage_client.c common/upload_service.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(FCGI_LIBS) $(MYSQL_LIBS) $(REDIS_LIBS)

$(BIN_DIR)/share: src_cgi/share_cgi.c $(COMMON) common/file_repository.c common/share_id.c common/md5.c common/user_validation.c common/token_service.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(FCGI_LIBS) $(MYSQL_LIBS) $(REDIS_LIBS)

integration-tools: $(BIN_DIR)/upload_repository_probe $(BIN_DIR)/cleanup_repository_probe $(BIN_DIR)/share_save_probe $(BIN_DIR)/cleanup_worker

$(BIN_DIR)/upload_repository_probe: tests/upload_repository_probe.c common/file_repository.c common/runtime_config.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(MYSQL_LIBS)

$(BIN_DIR)/cleanup_repository_probe: tests/cleanup_repository_probe.c common/cleanup_repository.c common/runtime_config.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(MYSQL_LIBS)

$(BIN_DIR)/share_save_probe: tests/share_save_probe.c common/file_repository.c common/runtime_config.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(MYSQL_LIBS)

$(BIN_DIR)/cleanup_worker: tools/cleanup_worker.c common/cleanup_repository.c common/runtime_config.c common/storage_client.c common/fastdfs_storage_client.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(MYSQL_LIBS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

test: tests/test_json_util.c common/json_util.c
	$(CC) $(CFLAGS) $^ -o /tmp/ai_cloud_storage_tests
	/tmp/ai_cloud_storage_tests
	$(CC) $(CFLAGS) tests/test_auth_helpers.c common/md5.c common/user_validation.c -o /tmp/ai_cloud_storage_auth_tests
	/tmp/ai_cloud_storage_auth_tests
	$(CC) $(CFLAGS) tests/test_upload_service.c common/storage_client.c common/upload_service.c -o /tmp/ai_cloud_storage_upload_tests
	/tmp/ai_cloud_storage_upload_tests
	$(CC) $(CFLAGS) tests/test_fastdfs_storage_client.c common/storage_client.c common/fastdfs_storage_client.c -o /tmp/ai_cloud_storage_fastdfs_tests
	/tmp/ai_cloud_storage_fastdfs_tests
	$(CC) $(CFLAGS) tests/test_upload_intake.c common/upload_intake.c common/md5.c -o /tmp/ai_cloud_storage_intake_tests
	/tmp/ai_cloud_storage_intake_tests
	$(CC) $(CFLAGS) tests/test_multipart_upload.c common/multipart_upload.c common/upload_intake.c common/md5.c -o /tmp/ai_cloud_storage_multipart_tests
	/tmp/ai_cloud_storage_multipart_tests
	$(CC) $(CFLAGS) tests/test_share_id.c common/share_id.c -o /tmp/ai_cloud_storage_share_id_tests
	/tmp/ai_cloud_storage_share_id_tests

e2e:
	sh tests/e2e_auth.sh

clean:
	rm -rf $(BIN_DIR) /tmp/ai_cloud_storage_tests /tmp/ai_cloud_storage_auth_tests /tmp/ai_cloud_storage_upload_tests /tmp/ai_cloud_storage_fastdfs_tests /tmp/ai_cloud_storage_intake_tests /tmp/ai_cloud_storage_multipart_tests /tmp/ai_cloud_storage_share_id_tests
