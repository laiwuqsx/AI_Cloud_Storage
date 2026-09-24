USE ai_cloud_storage;

ALTER TABLE user_ai_index_entry
  ADD COLUMN user_file_id BIGINT UNSIGNED NOT NULL AFTER md5,
  ADD UNIQUE KEY uq_user_ai_relation (user_file_id);

CREATE TABLE IF NOT EXISTS outbox_event (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  event_type VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  aggregate_key VARCHAR(160) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  idempotency_key VARCHAR(255) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  payload JSON NOT NULL,
  status ENUM('pending', 'publishing', 'published', 'failed')
    NOT NULL DEFAULT 'pending',
  retry_count INT UNSIGNED NOT NULL DEFAULT 0,
  last_error VARCHAR(512) NOT NULL DEFAULT '',
  next_attempt_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  published_at TIMESTAMP NULL DEFAULT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  UNIQUE KEY uq_outbox_idempotency (idempotency_key),
  KEY idx_outbox_publish (status, next_attempt_at, id),
  KEY idx_outbox_aggregate (aggregate_key, id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
