USE ai_cloud_storage;

CREATE TABLE IF NOT EXISTS chunk_upload_part (
  upload_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  chunk_index INT UNSIGNED NOT NULL,
  size BIGINT UNSIGNED NOT NULL,
  chunk_md5 CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  stored_path VARCHAR(512) NOT NULL,
  status ENUM('staging', 'ready') NOT NULL DEFAULT 'staging',
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (upload_id, chunk_index),
  KEY idx_chunk_part_status (upload_id, status, chunk_index),
  CONSTRAINT fk_chunk_part_session FOREIGN KEY (upload_id)
    REFERENCES chunk_upload_session(upload_id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
