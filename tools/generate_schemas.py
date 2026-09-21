"""Generate the v1 JSON Schema contracts. Standard library only; --check detects drift."""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def obj(properties, optional=()):
    return {"type": "object", "properties": properties,
            "required": [k for k in properties if k not in optional], "additionalProperties": False}


def string(maximum=128):
    return {"type": "string", "minLength": 1, "maxLength": maximum, "pattern": "^[^\\u0000]+$"}


def enum(*values):
    return {"enum": list(values)}


def arr(items, maximum, minimum=0, unique=False):
    result = {"type": "array", "items": items, "maxItems": maximum, "minItems": minimum}
    if unique:
        result["uniqueItems"] = True
    return result


def integer(maximum, minimum=0):
    return {"type": "integer", "minimum": minimum, "maximum": maximum}


def ref(name):
    return {"$ref": "#/$defs/" + name}


ID = {"type": "string", "pattern": "^[A-Za-z0-9_.:-]{1,128}$"}
HASH = {"type": "string", "pattern": "^sha256:[a-f0-9]{64}$"}
U64 = {"type": "string", "pattern": "^(0|[1-9][0-9]{0,19})$",
       "description": "Canonical decimal uint64. Reader additionally enforces <=18446744073709551615."}
POS64 = {**U64, "pattern": "^[1-9][0-9]{0,19}$"}
NUMBER = {"type": "number"}
BOOL = {"type": "boolean"}

CORRELATION = obj({"run_id": ID, "worker_id": ID, "worker_epoch": POS64,
                   "inspection_id": ID, "check_id": ID, "task_id": ID, "attempt": POS64})
ERROR = obj({
    "code": string(), "category": enum("Configuration", "Protocol", "Device", "Execution", "Resource", "Persistence", "Delivery", "Internal"),
    "message": string(2048), "retryability": enum("Never", "AfterRecovery", "Safe"), "origin": string(),
    "correlation": ref("correlation"), "vendor_code": string(256)
}, ("correlation", "vendor_code"))
DEFECT = obj({"class_id": integer(2147483647), "label": string(256),
              "score": {"type": "number", "minimum": 0, "maximum": 1},
              "box_xyxy": arr({"type": "number", "minimum": 0}, 4, 4),
              "coordinate_space": {"const": "original_pixels"}})
MEASUREMENT = obj({"name": string(), "value": NUMBER, "unit": string(32)})
CHECK = obj({
    "correlation": ref("correlation"), "execution_state": enum("Succeeded", "Failed", "Cancelled", "TimedOut"),
    "quality": enum("Unknown", "OK", "NG"), "defects": arr(ref("defect"), 4096),
    "measurements": arr(ref("measurement"), 256), "elapsed_ns": U64, "classification": string(256),
    "error": ref("error"), "model_hash": HASH,
    "masks": arr(obj({"hash":HASH,"width":integer(1048576,1),"height":integer(1048576,1),
                      "instance":integer(63),"storage":{"const":"local_ephemeral"}}),64)
}, ("classification", "error", "model_hash","masks"))
CHECK["allOf"] = [{
    "if": {"properties": {"execution_state": {"const": "Succeeded"}}},
    "then": {"properties": {"quality": enum("OK", "NG")}, "not": {"required": ["error"]}},
    "else": {"properties": {"quality": {"const": "Unknown"}}, "required": ["error"]}
}]
RESULT = obj({
    "run_id": ID, "inspection_id": ID, "workpiece_id": ID, "station_id": ID,
    "mode": enum("Demo", "Replay", "Production"), "recipe_hash": HASH,
    "execution_state": enum("Completed", "Failed", "Cancelled", "TimedOut"), "quality": enum("Unknown", "OK", "NG"),
    "expected_checks": arr(obj({"check_id": ID, "required": BOOL}), 64, 1),
    "checks": arr(ref("check"), 64), "reason": string(2048)
})
RESULT["allOf"] = [{
    "if": {"properties": {"execution_state": {"const": "Completed"}}},
    "then": {"properties": {"quality": enum("OK", "NG")}},
    "else": {"properties": {"quality": {"const": "Unknown"}}}
}]
EVENT = obj({"schema_version": {"const": 1}, "event_type": {"const": "inspection.finalized"}, "event_id": ID,
             "sequence": U64, "emitted_at_unix_ns": U64, "result": ref("result")})
EVENT["$defs"] = {"correlation": CORRELATION, "error": ERROR, "defect": DEFECT, "measurement": MEASUREMENT,
                  "check": CHECK, "result": RESULT}

RULE = {"oneOf": [
    obj({"kind": {"const": "forbidden_class"}, "class_id": integer(2147483647)}),
    obj({"kind": {"const": "count_range"}, "class_id": integer(2147483647), "minimum": integer(4294967295), "maximum": integer(4294967295)}),
    obj({"kind": {"const": "allowed_classification"}, "labels": arr(string(256), 256, 1, True)}),
    obj({"kind": {"const": "measurement_range"}, "name": string(), "unit": string(32), "minimum": NUMBER,
         "maximum": NUMBER, "include_minimum": BOOL, "include_maximum": BOOL})
]}
RECIPE = obj({
    "schema_version": {"const": 1}, "recipe_id": ID, "version": string(), "content_hash": HASH,
    "deadline_ms": POS64, "trigger_mode": enum("Software", "Hardware"), "association": {"const": "TriggerId"},
    "checks": arr(obj({"check_id": ID, "camera_id": ID, "required": BOOL, "rule": ref("rule"), "model_hash": HASH}, ("model_hash",)), 64, 1),
    "storage": obj({"image_policy": enum("none", "ng_only", "all"), "traceability": enum("optional", "required")}),
    "outputs": arr(ID, 32, unique=True)
})
RECIPE["$defs"] = {"rule": RULE}
PARAMETER = obj({
    "type": enum("string", "integer", "number", "boolean"),
    "default": {"type": ["string", "integer", "number", "boolean"]},
    "minimum": NUMBER, "maximum": NUMBER, "minLength": integer(4096), "maxLength": integer(4096),
    "enum": arr({"type": ["string", "integer", "number", "boolean"]}, 256, 1, True),
    "description": string(2048), "unit": string(32), "sensitive": BOOL, "mutable_during_run": BOOL
}, ("default", "minimum", "maximum", "minLength", "maxLength", "enum", "description", "unit", "sensitive", "mutable_during_run"))
PARAMETERS = obj({"type": {"const": "object"},
                  "properties": {"type": "object", "maxProperties": 128, "propertyNames": ID, "additionalProperties": PARAMETER},
                  "additionalProperties": {"const": False}, "required": arr(ID, 128, unique=True)}, ("required",))
MANIFEST = obj({
    "schema_version": {"const": 1}, "plugin_id": ID, "plugin_version": string(),
    "kind": enum("Camera", "Algorithm", "Communication", "ObjectStorage", "ResultOutput"), "sdk_api_version": {"const": 1},
    "build_id": string(), "platform": enum("windows", "linux"), "architecture": {"const": "x64"},
    "compiler_abi": string(), "runtime_variant": enum("Release", "Debug"),
    "entry_library": {**string(256), "pattern": "^[^/\\\\:]+\\.(dll|so)$"},
    "capabilities": arr(string(), 64, unique=True), "threading_model": enum("Serialized", "Reentrant", "DedicatedThread"),
    "max_instances": integer(1024, 1), "parameters_schema": PARAMETERS,
    "dependencies": arr(string(), 64, unique=True),
    "license_metadata": obj({"spdx": string(), "file": {**string(), "pattern": "^[^/\\\\:]+$"}})
})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    for name, schema in {"result-event": EVENT, "recipe": RECIPE, "plugin-manifest": MANIFEST}.items():
        schema = {"$schema": "https://json-schema.org/draft/2020-12/schema",
                  "title": "Vision " + name + " v1", **schema}
        text = json.dumps(schema, ensure_ascii=False, indent=2) + "\n"
        target = ROOT / "schemas" / (name + "-v1.schema.json")
        if args.check:
            if not target.exists() or target.read_text(encoding="utf-8") != text:
                raise SystemExit(f"Schema drift: {target}")
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8")
        print(("checked " if args.check else "generated ") + target.name)


if __name__ == "__main__":
    main()
