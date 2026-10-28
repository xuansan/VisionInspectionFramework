BEGIN;
CREATE TABLE vision_schema_version (
  id INTEGER PRIMARY KEY CHECK (id = 1), version INTEGER NOT NULL
);
CREATE TABLE vision_results (
  event_id VARCHAR(128) COLLATE "C" PRIMARY KEY,
  run_id VARCHAR(128) COLLATE "C" NOT NULL,
  inspection_id VARCHAR(128) COLLATE "C" NOT NULL,
  station_id VARCHAR(128) NOT NULL, workpiece_id VARCHAR(128) NOT NULL,
  recipe_hash VARCHAR(128) NOT NULL, quality VARCHAR(16) NOT NULL,
  execution_state VARCHAR(32) NOT NULL,
  sequence_no NUMERIC(20,0) NOT NULL, emitted_at_unix_ns NUMERIC(20,0) NOT NULL,
  result_sha256 VARCHAR(71) NOT NULL, result_json TEXT NOT NULL,
  UNIQUE(run_id, inspection_id)
);
CREATE INDEX station_history ON vision_results(station_id, emitted_at_unix_ns);
CREATE TABLE vision_defects (
  event_id VARCHAR(128) COLLATE "C" NOT NULL REFERENCES vision_results(event_id),
  check_id VARCHAR(128) COLLATE "C" NOT NULL, defect_index INTEGER NOT NULL,
  class_id INTEGER NOT NULL, label VARCHAR(256) NOT NULL,
  score DOUBLE PRECISION NOT NULL CHECK (score BETWEEN 0 AND 1),
  x1 DOUBLE PRECISION NOT NULL, y1 DOUBLE PRECISION NOT NULL,
  x2 DOUBLE PRECISION NOT NULL, y2 DOUBLE PRECISION NOT NULL,
  PRIMARY KEY(event_id, check_id, defect_index)
);
CREATE TABLE vision_images (
  event_id VARCHAR(128) COLLATE "C" NOT NULL REFERENCES vision_results(event_id),
  image_id VARCHAR(128) COLLATE "C" NOT NULL, channel VARCHAR(128) NOT NULL,
  image_sha256 VARCHAR(71) NOT NULL, local_reference TEXT NOT NULL,
  descriptor_json TEXT NOT NULL, PRIMARY KEY(event_id, image_id)
);
INSERT INTO vision_schema_version(id, version) VALUES(1, 1);
COMMIT;
