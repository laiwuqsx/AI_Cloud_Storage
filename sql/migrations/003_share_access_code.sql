USE ai_cloud_storage;

ALTER TABLE share_file_list
  ADD COLUMN access_code_salt CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NULL AFTER share_id,
  ADD COLUMN access_code_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL AFTER access_code_salt;
