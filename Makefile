CC := gcc
CXX := g++
CFLAGS := -std=c11 -Wall -Wextra -Werror -Iinclude
CXXFLAGS := -std=c++17 -Wall -Wextra -Werror -Iinclude
FCGI_LIBS := -lfcgi
BIN_DIR := bin_cgi

COMMON := common/json_util.c common/http_response.c common/runtime_config.c
FILE_REPOSITORY := common/file_repository.c common/cleanup_repository.c common/outbox_repository.c common/ai_index_event.c
MYSQL_LIBS := -lmysqlclient
REDIS_LIBS := -lhiredis
OPENSSL_PREFIX := $(shell brew --prefix openssl@3 2>/dev/null)
CRYPTO_CFLAGS := $(if $(OPENSSL_PREFIX),-I$(OPENSSL_PREFIX)/include)
CRYPTO_LIBS := $(if $(OPENSSL_PREFIX),-L$(OPENSSL_PREFIX)/lib) -lcrypto
CURL_HOME := $(shell brew --prefix curl 2>/dev/null)
CJSON_HOME := $(shell brew --prefix cjson 2>/dev/null)
CURL_PREFIX := $(if $(wildcard $(CURL_HOME)/include/curl/curl.h),$(CURL_HOME))
CJSON_PREFIX := $(if $(wildcard $(CJSON_HOME)/include/cjson/cJSON.h),$(CJSON_HOME))
AI_DEPS_AVAILABLE := $(shell test -f "$(CJSON_PREFIX)/include/cjson/cJSON.h" -o -f /usr/include/cjson/cJSON.h && echo 1)
AI_CFLAGS := $(if $(CURL_PREFIX),-I$(CURL_PREFIX)/include) $(if $(CJSON_PREFIX),-I$(CJSON_PREFIX)/include)
AI_LIBS := $(if $(CURL_PREFIX),-L$(CURL_PREFIX)/lib) $(if $(CJSON_PREFIX),-L$(CJSON_PREFIX)/lib) -lcurl -lcjson -lm
FAISS_LIBS := -lfaiss -lopenblas -fopenmp
FAISS_DEPS_AVAILABLE := $(shell test -f /usr/include/faiss/Index.h && echo 1)

.PHONY: all integration-tools test test-client e2e clean

all: $(BIN_DIR)/login $(BIN_DIR)/register $(BIN_DIR)/myfiles $(BIN_DIR)/md5 $(BIN_DIR)/dealfile $(BIN_DIR)/logout $(BIN_DIR)/upload $(BIN_DIR)/share $(BIN_DIR)/download $(BIN_DIR)/chunk_upload

$(BIN_DIR)/login: src_cgi/login_cgi.c $(COMMON) common/md5.c common/user_validation.c common/user_repository.c common/token_service.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(FCGI_LIBS) $(MYSQL_LIBS) $(REDIS_LIBS)

$(BIN_DIR)/register: src_cgi/reg_cgi.c $(COMMON) common/md5.c common/user_validation.c common/user_repository.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(FCGI_LIBS) $(MYSQL_LIBS)

$(BIN_DIR)/myfiles: src_cgi/myfiles_cgi.c $(COMMON) common/md5.c common/user_validation.c common/token_service.c $(FILE_REPOSITORY) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(FCGI_LIBS) $(MYSQL_LIBS) $(REDIS_LIBS)

$(BIN_DIR)/md5: src_cgi/md5_cgi.c $(COMMON) common/md5.c common/user_validation.c common/token_service.c $(FILE_REPOSITORY) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(FCGI_LIBS) $(MYSQL_LIBS) $(REDIS_LIBS)

$(BIN_DIR)/dealfile: src_cgi/dealfile_cgi.c $(COMMON) common/md5.c common/user_validation.c common/token_service.c $(FILE_REPOSITORY) common/share_id.c common/share_code.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CRYPTO_CFLAGS) $^ -o $@ $(FCGI_LIBS) $(MYSQL_LIBS) $(REDIS_LIBS) $(CRYPTO_LIBS)

$(BIN_DIR)/logout: src_cgi/logout_cgi.c $(COMMON) common/md5.c common/user_validation.c common/token_service.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(FCGI_LIBS) $(REDIS_LIBS)

$(BIN_DIR)/upload: src_cgi/upload_cgi.c $(COMMON) common/md5.c common/user_validation.c common/token_service.c $(FILE_REPOSITORY) common/upload_intake.c common/multipart_upload.c common/storage_client.c common/fastdfs_storage_client.c common/upload_service.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(FCGI_LIBS) $(MYSQL_LIBS) $(REDIS_LIBS)

$(BIN_DIR)/share: src_cgi/share_cgi.c $(COMMON) $(FILE_REPOSITORY) common/share_id.c common/share_code.c common/share_access_service.c common/md5.c common/user_validation.c common/token_service.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CRYPTO_CFLAGS) $^ -o $@ $(FCGI_LIBS) $(MYSQL_LIBS) $(REDIS_LIBS) $(CRYPTO_LIBS)

$(BIN_DIR)/download: src_cgi/download_cgi.c $(COMMON) $(FILE_REPOSITORY) common/share_id.c common/share_code.c common/share_access_service.c common/md5.c common/user_validation.c common/token_service.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CRYPTO_CFLAGS) $^ -o $@ $(FCGI_LIBS) $(MYSQL_LIBS) $(REDIS_LIBS) $(CRYPTO_LIBS)

$(BIN_DIR)/chunk_upload: src_cgi/chunk_upload_cgi.c $(COMMON) common/chunk_upload_repository.c common/chunk_storage.c common/chunk_assembler.c common/upload_id.c common/upload_intake.c common/md5.c common/user_validation.c common/token_service.c $(FILE_REPOSITORY) common/storage_client.c common/fastdfs_storage_client.c common/upload_service.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(FCGI_LIBS) $(MYSQL_LIBS) $(REDIS_LIBS)

integration-tools: $(BIN_DIR)/upload_repository_probe $(BIN_DIR)/cleanup_repository_probe $(BIN_DIR)/share_save_probe $(BIN_DIR)/ai_content_repository_probe $(BIN_DIR)/ai_user_index_repository_probe $(BIN_DIR)/cleanup_worker $(BIN_DIR)/cleanup_metrics $(BIN_DIR)/outbox_publisher $(BIN_DIR)/outbox_metrics $(BIN_DIR)/ai_content_worker $(BIN_DIR)/faiss_index_worker

$(BIN_DIR)/upload_repository_probe: tests/upload_repository_probe.c $(FILE_REPOSITORY) common/runtime_config.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(MYSQL_LIBS)

$(BIN_DIR)/cleanup_repository_probe: tests/cleanup_repository_probe.c common/cleanup_repository.c common/runtime_config.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(MYSQL_LIBS)

$(BIN_DIR)/share_save_probe: tests/share_save_probe.c $(FILE_REPOSITORY) common/runtime_config.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(MYSQL_LIBS)

$(BIN_DIR)/ai_content_repository_probe: tests/ai_content_repository_probe.c common/ai_content_repository.c common/runtime_config.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(MYSQL_LIBS)

$(BIN_DIR)/ai_user_index_repository_probe: tests/ai_user_index_repository_probe.c common/ai_user_index_repository.c common/ai_index_event.c common/runtime_config.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(MYSQL_LIBS)

$(BIN_DIR)/cleanup_worker: tools/cleanup_worker.c common/cleanup_repository.c common/runtime_config.c common/storage_client.c common/fastdfs_storage_client.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(MYSQL_LIBS)

$(BIN_DIR)/cleanup_metrics: tools/cleanup_metrics.c common/cleanup_repository.c common/runtime_config.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(MYSQL_LIBS)

$(BIN_DIR)/outbox_publisher: tools/outbox_publisher.c common/outbox_repository.c common/ai_index_event.c common/rabbitmq_event_publisher.c common/runtime_config.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(MYSQL_LIBS) -lrabbitmq

$(BIN_DIR)/outbox_metrics: tools/outbox_metrics.c common/outbox_repository.c common/ai_index_event.c common/rabbitmq_event_publisher.c common/runtime_config.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(MYSQL_LIBS) -lrabbitmq

$(BIN_DIR)/ai_content_worker: tools/ai_content_worker.c common/ai_content_repository.c common/ai_content_task.c common/ai_index_event.c common/json_util.c common/dashscope_client.c common/rabbitmq_content_consumer.c common/storage_client.c common/fastdfs_storage_client.c common/runtime_config.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $(AI_CFLAGS) $^ -o $@ $(MYSQL_LIBS) -lrabbitmq $(AI_LIBS)

FAISS_WORKER_C_OBJECTS := $(BIN_DIR)/faiss_worker_main.o $(BIN_DIR)/faiss_worker_repository.o $(BIN_DIR)/faiss_worker_task.o $(BIN_DIR)/faiss_worker_event.o $(BIN_DIR)/faiss_worker_json.o $(BIN_DIR)/faiss_worker_consumer.o $(BIN_DIR)/faiss_worker_config.o $(BIN_DIR)/faiss_worker_md5.o

$(BIN_DIR)/faiss_index_worker: $(FAISS_WORKER_C_OBJECTS) $(BIN_DIR)/faiss_index_store.o
	$(CXX) $^ -o $@ $(MYSQL_LIBS) -lrabbitmq $(FAISS_LIBS)

$(BIN_DIR)/faiss_worker_main.o: tools/faiss_index_worker.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(BIN_DIR)/faiss_worker_repository.o: common/ai_user_index_repository.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(BIN_DIR)/faiss_worker_task.o: common/ai_user_index_task.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(BIN_DIR)/faiss_worker_event.o: common/ai_index_event.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(BIN_DIR)/faiss_worker_json.o: common/json_util.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(BIN_DIR)/faiss_worker_consumer.o: common/rabbitmq_content_consumer.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(BIN_DIR)/faiss_worker_config.o: common/runtime_config.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(BIN_DIR)/faiss_worker_md5.o: common/md5.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
$(BIN_DIR)/faiss_index_store.o: common/faiss_index_store.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

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
	$(CC) $(CFLAGS) tests/test_upload_id.c common/upload_id.c -o /tmp/ai_cloud_storage_upload_id_tests
	/tmp/ai_cloud_storage_upload_id_tests
	$(CC) $(CFLAGS) tests/test_chunk_storage.c common/chunk_storage.c common/upload_id.c -o /tmp/ai_cloud_storage_chunk_storage_tests
	/tmp/ai_cloud_storage_chunk_storage_tests
	$(CC) $(CFLAGS) tests/test_chunk_assembler.c common/chunk_assembler.c common/upload_intake.c common/md5.c -o /tmp/ai_cloud_storage_chunk_assembler_tests
	/tmp/ai_cloud_storage_chunk_assembler_tests
	$(CC) $(CFLAGS) tests/test_ai_index_event.c common/ai_index_event.c -o /tmp/ai_cloud_storage_ai_index_event_tests
	/tmp/ai_cloud_storage_ai_index_event_tests
	$(CC) $(CFLAGS) tests/test_ai_content_task.c common/ai_content_task.c common/ai_index_event.c common/json_util.c -o /tmp/ai_cloud_storage_ai_content_task_tests
	/tmp/ai_cloud_storage_ai_content_task_tests
	$(CC) $(CFLAGS) tests/test_ai_user_index_task.c common/ai_user_index_task.c common/ai_index_event.c common/json_util.c -o /tmp/ai_cloud_storage_ai_user_index_task_tests
	/tmp/ai_cloud_storage_ai_user_index_task_tests
	@if [ "$(AI_DEPS_AVAILABLE)" = "1" ]; then \
		$(CC) $(CFLAGS) $(AI_CFLAGS) tests/test_dashscope_client.c common/dashscope_client.c -o /tmp/ai_cloud_storage_dashscope_tests $(AI_LIBS) && \
		/tmp/ai_cloud_storage_dashscope_tests; \
	else \
		echo "dashscope_client tests skipped: cJSON development headers unavailable (covered by Docker build)"; \
	fi
	$(CC) $(CFLAGS) $(CRYPTO_CFLAGS) tests/test_share_code.c common/share_code.c -o /tmp/ai_cloud_storage_share_code_tests $(CRYPTO_LIBS)
	/tmp/ai_cloud_storage_share_code_tests
	@if [ "$(FAISS_DEPS_AVAILABLE)" = "1" ]; then \
		$(CXX) $(CXXFLAGS) tests/test_faiss_index_store.cpp common/faiss_index_store.cpp -o /tmp/ai_cloud_storage_faiss_tests $(FAISS_LIBS) && \
		/tmp/ai_cloud_storage_faiss_tests; \
	else \
		echo "FAISS index tests skipped: FAISS development headers unavailable (covered by Docker build)"; \
	fi

e2e:
	sh tests/e2e_auth.sh

test-client:
	node --test client/upload_client.test.mjs

clean:
	rm -rf $(BIN_DIR) /tmp/ai_cloud_storage_tests /tmp/ai_cloud_storage_auth_tests /tmp/ai_cloud_storage_upload_tests /tmp/ai_cloud_storage_fastdfs_tests /tmp/ai_cloud_storage_intake_tests /tmp/ai_cloud_storage_multipart_tests /tmp/ai_cloud_storage_share_id_tests /tmp/ai_cloud_storage_upload_id_tests /tmp/ai_cloud_storage_chunk_storage_tests /tmp/ai_cloud_storage_chunk_assembler_tests /tmp/ai_cloud_storage_ai_index_event_tests /tmp/ai_cloud_storage_ai_content_task_tests /tmp/ai_cloud_storage_ai_user_index_task_tests /tmp/ai_cloud_storage_dashscope_tests /tmp/ai_cloud_storage_share_code_tests /tmp/ai_cloud_storage_faiss_tests
