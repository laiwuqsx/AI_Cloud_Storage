USE ai_cloud_storage;

ALTER TABLE file_ai_metadata
  ADD COLUMN processing_event_id BIGINT UNSIGNED NULL AFTER processing_started_at,
  ADD COLUMN completed_event_id BIGINT UNSIGNED NULL AFTER processing_event_id;
