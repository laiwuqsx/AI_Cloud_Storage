USE ai_cloud_storage;

ALTER TABLE storage_cleanup_job
  MODIFY COLUMN status ENUM('pending', 'running', 'done', 'failed')
    NOT NULL DEFAULT 'pending',
  ADD COLUMN next_attempt_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
    AFTER last_error,
  DROP INDEX idx_cleanup_status_updated,
  ADD KEY idx_cleanup_status_attempt (status, next_attempt_at, id);
