USE ai_cloud_storage;

CREATE TABLE IF NOT EXISTS storage_cleanup_job (
  id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  storage_key VARCHAR(256) NOT NULL,
  reason VARCHAR(64) NOT NULL,
  status ENUM('pending', 'running', 'done') NOT NULL DEFAULT 'pending',
  retry_count INT UNSIGNED NOT NULL DEFAULT 0,
  last_error VARCHAR(512) NOT NULL DEFAULT '',
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  completed_at TIMESTAMP NULL DEFAULT NULL,
  UNIQUE KEY uq_cleanup_storage_key (storage_key),
  KEY idx_cleanup_status_updated (status, updated_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
