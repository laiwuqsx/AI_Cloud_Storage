USE ai_cloud_storage;

ALTER TABLE user_ai_index_entry
  ADD COLUMN processing_started_at TIMESTAMP NULL DEFAULT NULL AFTER next_attempt_at,
  ADD COLUMN processing_event_id BIGINT UNSIGNED NULL AFTER processing_started_at,
  ADD COLUMN completed_event_id BIGINT UNSIGNED NULL AFTER processing_event_id,
  ADD COLUMN processing_generation BIGINT UNSIGNED NOT NULL DEFAULT 0
    AFTER completed_event_id;
