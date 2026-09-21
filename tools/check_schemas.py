"""Independent schema checks plus CLI semantic checks. Requires jsonschema 4.18.4+."""
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
    schemas = {}
    for name in ("recipe", "plugin-manifest", "result-event"):
        schema = json.loads((ROOT / "schemas" / (name + "-v1.schema.json")).read_text(encoding="utf-8"))
        Draft202012Validator.check_schema(schema)
        schemas[name] = Draft202012Validator(schema)
    total = 0
    for name, filename, command in (("recipe", "recipe-v1.json", "--validate-recipe"),
                                     ("plugin-manifest", "manifest-v1.json", "--validate-manifest")):
        target = ROOT / "examples/contracts" / filename
        value = json.loads(target.read_text(encoding="utf-8"))
        schemas[name].validate(value)
        subprocess.run([args.cli, command, str(target)], check=True, capture_output=True)
        bad = {**value, "schema_version": 2}
        assert list(schemas[name].iter_errors(bad)), "Unsupported major accepted by schema"
        total += 2
    for scenario in ("ok", "ng", "failure", "timeout"):
        completed = subprocess.run([args.cli, "--demo", scenario], check=True, capture_output=True, encoding="utf-8")
        value = json.loads(completed.stdout)
        schemas["result-event"].validate(value)
        value["sequence"] = 9007199254740993
        assert list(schemas["result-event"].iter_errors(value)), "Numeric uint64 accepted"
        total += 2
    example = ROOT / "examples/contracts/result-event-v1.json"
    schemas["result-event"].validate(json.loads(example.read_text(encoding="utf-8")))
    subprocess.run([args.cli, "--validate-result", str(example)], check=True, capture_output=True)
    total += 1
    print(f"Schema/CLI checks passed: {total} checks; 3 schemas")


if __name__ == "__main__":
    main()
