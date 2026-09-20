USE ai_cloud_storage;

ALTER TABLE share_file_list
  ADD COLUMN user_file_id BIGINT NULL AFTER id,
  ADD COLUMN share_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL AFTER file_name,
  ADD COLUMN expires_at TIMESTAMP NULL DEFAULT NULL AFTER share_id;

UPDATE share_file_list s
JOIN user_file_list u
  ON u.user_name = s.user_name AND u.md5 = s.md5
SET s.user_file_id = u.id,
    s.share_id = LOWER(HEX(RANDOM_BYTES(32))),
    s.expires_at = TIMESTAMPADD(DAY, 7, UTC_TIMESTAMP());

DELETE FROM share_file_list WHERE user_file_id IS NULL;

ALTER TABLE share_file_list
  MODIFY COLUMN user_file_id BIGINT NOT NULL,
  MODIFY COLUMN share_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  ADD UNIQUE KEY uq_share_user_file_id (user_file_id),
  ADD UNIQUE KEY uq_share_id (share_id),
  ADD CONSTRAINT fk_share_user_file FOREIGN KEY (user_file_id)
    REFERENCES user_file_list(id) ON DELETE CASCADE;
