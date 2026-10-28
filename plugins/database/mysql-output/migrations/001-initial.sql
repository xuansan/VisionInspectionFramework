-- Apply once to a new dedicated database with an administrator/migration account.
CREATE TABLE vision_schema_version (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  version INTEGER NOT NULL
) ENGINE=InnoDB;
CREATE TABLE vision_results (
  event_id VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY,
  run_id VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  inspection_id VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  station_id VARCHAR(128) NOT NULL,
  workpiece_id VARCHAR(128) NOT NULL,
  recipe_hash VARCHAR(128) NOT NULL,
  quality VARCHAR(16) NOT NULL,
  execution_state VARCHAR(32) NOT NULL,
  sequence_no DECIMAL(20,0) NOT NULL,
  emitted_at_unix_ns DECIMAL(20,0) NOT NULL,
  result_sha256 VARCHAR(71) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  result_json MEDIUMTEXT NOT NULL,
  UNIQUE KEY inspection_identity(run_id, inspection_id),
  KEY station_history(station_id, emitted_at_unix_ns)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
CREATE TABLE vision_defects (
  event_id VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  check_id VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  defect_index INTEGER NOT NULL,
  class_id INTEGER NOT NULL,
  label VARCHAR(256) NOT NULL,
  score DOUBLE NOT NULL CHECK (score BETWEEN 0 AND 1),
  x1 DOUBLE NOT NULL, y1 DOUBLE NOT NULL, x2 DOUBLE NOT NULL, y2 DOUBLE NOT NULL,
  PRIMARY KEY(event_id, check_id, defect_index),
  FOREIGN KEY(event_id) REFERENCES vision_results(event_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
CREATE TABLE vision_images (
  event_id VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  image_id VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  channel VARCHAR(128) NOT NULL,
  image_sha256 VARCHAR(71) NOT NULL,
  local_reference TEXT NOT NULL,
  descriptor_json TEXT NOT NULL,
  PRIMARY KEY(event_id, image_id),
  FOREIGN KEY(event_id) REFERENCES vision_results(event_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
INSERT INTO vision_schema_version(id, version) VALUES(1, 1);
