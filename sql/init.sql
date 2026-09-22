CREATE DATABASE IF NOT EXISTS ai_cloud_storage
  DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

USE ai_cloud_storage;

CREATE TABLE IF NOT EXISTS user_info (
  id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  user_name VARCHAR(32) NOT NULL UNIQUE,
  nick_name VARCHAR(32) NOT NULL UNIQUE,
  password CHAR(32) NOT NULL,
  salt CHAR(32) NOT NULL,
  create_time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS file_info (
  id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  md5 CHAR(32) NOT NULL UNIQUE,
  storage_key VARCHAR(256) NOT NULL,
  url VARCHAR(512) NOT NULL,
  size BIGINT UNSIGNED NOT NULL DEFAULT 0,
  type VARCHAR(32) NOT NULL DEFAULT '',
  reference_count INT UNSIGNED NOT NULL DEFAULT 1,
  create_time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS user_file_list (
  id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  user_name VARCHAR(32) NOT NULL,
  md5 CHAR(32) NOT NULL,
  file_name VARCHAR(128) NOT NULL,
  shared_status TINYINT NOT NULL DEFAULT 0,
  pv INT UNSIGNED NOT NULL DEFAULT 0,
  create_time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uq_user_file (user_name, md5),
  CONSTRAINT fk_user_file_user FOREIGN KEY (user_name)
    REFERENCES user_info(user_name) ON DELETE CASCADE,
  CONSTRAINT fk_user_file_info FOREIGN KEY (md5)
    REFERENCES file_info(md5) ON DELETE RESTRICT
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS share_file_list (
  id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  user_file_id BIGINT NOT NULL,
  user_name VARCHAR(32) NOT NULL,
  md5 CHAR(32) NOT NULL,
  file_name VARCHAR(128) NOT NULL,
  share_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  access_code_salt CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NULL,
  access_code_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,
  expires_at TIMESTAMP NULL DEFAULT NULL,
  pv INT UNSIGNED NOT NULL DEFAULT 0,
  create_time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uq_share_user_file_id (user_file_id),
  UNIQUE KEY uq_share_id (share_id),
  UNIQUE KEY uq_shared_user_file (user_name, md5),
  KEY idx_share_created_at (create_time),
  CONSTRAINT fk_share_user_file FOREIGN KEY (user_file_id)
    REFERENCES user_file_list(id) ON DELETE CASCADE,
  CONSTRAINT fk_share_file_info FOREIGN KEY (md5)
    REFERENCES file_info(md5) ON DELETE RESTRICT
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS storage_cleanup_job (
  id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  storage_key VARCHAR(256) NOT NULL,
  reason VARCHAR(64) NOT NULL,
  status ENUM('pending', 'running', 'done', 'failed') NOT NULL DEFAULT 'pending',
  retry_count INT UNSIGNED NOT NULL DEFAULT 0,
  last_error VARCHAR(512) NOT NULL DEFAULT '',
  next_attempt_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  completed_at TIMESTAMP NULL DEFAULT NULL,
  UNIQUE KEY uq_cleanup_storage_key (storage_key),
  KEY idx_cleanup_status_attempt (status, next_attempt_at, id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

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
