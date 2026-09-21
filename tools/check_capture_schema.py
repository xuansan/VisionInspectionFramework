"""Independent frame schema and actual camera manifest checks."""
import argparse
import json
import subprocess
from pathlib import Path
from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    args = parser.parse_args()
    schema = json.loads((ROOT / "schemas/frame-descriptor-v1.schema.json").read_text(encoding="utf-8"))
    Draft202012Validator.check_schema(schema)
    validator = Draft202012Validator(schema)
    example = json.loads((ROOT / "examples/contracts/frame-descriptor-v1.json").read_text(encoding="utf-8"))
    validator.validate(example)
    for key, value in (("schema_version", 2), ("width", 0), ("worker_epoch", "0"),
                       ("lease_id", 2), ("pixel_format", "unknown"), ("extra", 1)):
        assert list(validator.iter_errors({**example, key: value})), f"Frame schema accepted {key}"
    manifest_path = ROOT / "plugins/sim-camera/manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    plugin_schema = json.loads((ROOT / "schemas/plugin-manifest-v1.schema.json").read_text(encoding="utf-8"))
    Draft202012Validator.check_schema(plugin_schema)
    Draft202012Validator(plugin_schema).validate(manifest)
    subprocess.run([args.cli, "--validate-manifest", str(manifest_path)], check=True, capture_output=True)
    assert (manifest_path.parent / manifest["license_metadata"]["file"]).is_file()
    print("Frame schema: 7 checks passed; camera manifest: schema, C++ CLI, metadata file passed")


if __name__ == "__main__":
    main()
