USE ai_cloud_storage;

ALTER TABLE file_ai_metadata
  ADD COLUMN processing_generation BIGINT UNSIGNED NOT NULL DEFAULT 0
    AFTER completed_event_id;
