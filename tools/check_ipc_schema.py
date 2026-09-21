"""Validate every public message fixture using JSON Schema and the C++ CLI."""
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
    schema = json.loads((ROOT / "schemas/ipc-v1.schema.json").read_text(encoding="utf-8"))
    Draft202012Validator.check_schema(schema)
    validator = Draft202012Validator(schema)
    bad_path = ROOT / "out/validation/ipc-invalid-example.json"
    bad_path.parent.mkdir(parents=True, exist_ok=True)
    count = 0
    for file in sorted((ROOT / "examples/ipc").glob("*.json")):
        value = json.loads(file.read_text(encoding="utf-8"))
        validator.validate(value)
        good = subprocess.run([args.cli, "--validate-message", str(file)], capture_output=True)
        assert good.returncode == 0, f"C++ rejected {file.name}: {good.stderr!r}"
        value["payload"]["unexpected"] = True
        assert list(validator.iter_errors(value)), f"Schema accepted extra payload field: {file.name}"
        bad_path.write_text(json.dumps(value), encoding="utf-8")
        bad = subprocess.run([args.cli, "--validate-message", str(bad_path)], capture_output=True)
        assert bad.returncode == 2, f"C++ accepted extra payload field: {file.name}"
        count += 1
    assert count == 23
    print("23 message types: schema and C++ positive/negative checks all passed (92 checks)")


if __name__ == "__main__":
    main()
