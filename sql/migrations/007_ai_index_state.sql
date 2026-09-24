USE ai_cloud_storage;

CREATE TABLE IF NOT EXISTS file_ai_metadata (
  md5 CHAR(32) NOT NULL PRIMARY KEY,
  status ENUM('pending', 'processing', 'ready', 'failed')
    NOT NULL DEFAULT 'pending',
  description TEXT NULL,
  embedding LONGBLOB NULL,
  embedding_model VARCHAR(64) NOT NULL DEFAULT '',
  embedding_dimension INT UNSIGNED NULL,
  embedding_version INT UNSIGNED NOT NULL DEFAULT 1,
  retry_count INT UNSIGNED NOT NULL DEFAULT 0,
  last_error VARCHAR(512) NOT NULL DEFAULT '',
  next_attempt_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  processing_started_at TIMESTAMP NULL DEFAULT NULL,
  ready_at TIMESTAMP NULL DEFAULT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  KEY idx_file_ai_work (status, next_attempt_at, updated_at),
  CONSTRAINT fk_file_ai_metadata_file FOREIGN KEY (md5)
    REFERENCES file_info(md5) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS user_ai_index_entry (
  user_name VARCHAR(32) NOT NULL,
  md5 CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  status ENUM('pending', 'waiting_content', 'indexing', 'indexed', 'removing', 'failed')
    NOT NULL DEFAULT 'pending',
  vector_id BIGINT UNSIGNED NULL,
  embedding_version INT UNSIGNED NOT NULL DEFAULT 1,
  index_version BIGINT UNSIGNED NOT NULL DEFAULT 0,
  retry_count INT UNSIGNED NOT NULL DEFAULT 0,
  last_error VARCHAR(512) NOT NULL DEFAULT '',
  next_attempt_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  indexed_at TIMESTAMP NULL DEFAULT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (user_name, md5),
  UNIQUE KEY uq_user_ai_vector (user_name, vector_id),
  KEY idx_user_ai_work (status, next_attempt_at, updated_at),
  CONSTRAINT fk_user_ai_entry_user FOREIGN KEY (user_name)
    REFERENCES user_info(user_name) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
