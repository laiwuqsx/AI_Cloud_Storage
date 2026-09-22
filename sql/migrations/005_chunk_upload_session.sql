USE ai_cloud_storage;

CREATE TABLE IF NOT EXISTS chunk_upload_session (
  upload_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL PRIMARY KEY,
  user_name VARCHAR(32) NOT NULL,
  file_name VARCHAR(128) NOT NULL,
  file_md5 CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  total_size BIGINT UNSIGNED NOT NULL,
  chunk_size INT UNSIGNED NOT NULL,
  total_chunks INT UNSIGNED NOT NULL,
  status ENUM('receiving', 'completing', 'completed', 'cancelled', 'expired')
    NOT NULL DEFAULT 'receiving',
  expires_at TIMESTAMP NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  KEY idx_chunk_session_user_status (user_name, status, updated_at),
  KEY idx_chunk_session_expiry (status, expires_at),
  CONSTRAINT fk_chunk_session_user FOREIGN KEY (user_name)
    REFERENCES user_info(user_name) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
