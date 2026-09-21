"""Generate protocol 1.0 schema and deterministic examples. No third-party dependency."""
import argparse
import json
from pathlib import Path
from generate_schemas import obj, string, enum, arr, ID, HASH, U64, POS64, CORRELATION, EVENT

ROOT = Path(__file__).resolve().parents[1]
NULL = {"type": "null"}
FRAME = json.loads((ROOT / "schemas/frame-descriptor-v1.schema.json").read_text(encoding="utf-8"))
FRAME_PROPERTIES = {}
for key, value in FRAME["properties"].items():
    FRAME_PROPERTIES[key] = FRAME["$defs"][value["$ref"].split("/")[-1]] if "$ref" in value else value
FRAME_VALUE = obj(FRAME_PROPERTIES)
PAYLOADS = {
    "Hello": obj({"token": {**string(128), "minLength": 32}, "required_capabilities": arr(ID, 32, unique=True)}),
    "Configure": obj({"recipe_hash": HASH, "config_revision": POS64}),
    "Ready": obj({"recipe_hash": HASH, "config_revision": POS64}),
    "SubmitTask": obj({"operation": enum("Inspect", "Capture", "Persist"), "input_ref": ID, "budget_ns": POS64}),
    "DeviceTask": obj({"session_id":ID,"command_sequence":POS64,"ready":{"type":"boolean"},
        "arrival":{"type":"boolean"},"position":{"type":"boolean"},"safety_ok":{"type":"boolean"},
        "result_id":{"oneOf":[NULL,ID]},"quality":enum("Unknown","OK","NG")}),
    "DeviceFinished": obj({"session_id":ID,"command_sequence":POS64,"ready":{"type":"boolean"},
        "arrival_sequence":U64,"position":{"type":"boolean"},"safety_ok":{"type":"boolean"},"ack_id":{"oneOf":[NULL,ID]}}),
    "TaskAccepted": obj({}),
    "CaptureTask": obj({"slot_count":{"type":"integer","minimum":1,"maximum":4096},
                       "slot_bytes":POS64,"write_frame":FRAME_VALUE,"budget_ns":POS64}),
    "InspectTask": obj({"slot_count":{"type":"integer","minimum":1,"maximum":4096},
                        "slot_bytes":POS64,"read_frame":FRAME_VALUE,"budget_ns":POS64}),
    "CaptureFinished": {"oneOf":[
        obj({"execution_state":{"const":"Succeeded"},"frame":FRAME_VALUE,"error_code":NULL}),
        obj({"execution_state":enum("Failed","Cancelled","TimedOut"),"frame":NULL,"error_code":ID})]},
    "TaskProgress": obj({"progress_sequence": POS64}),
    "TaskFinished": {"oneOf": [
        obj({"execution_state": {"const": "Succeeded"}, "quality": enum("OK", "NG"), "result_ref": ID, "error_code": NULL}),
        obj({"execution_state": enum("Failed", "Cancelled", "TimedOut"), "quality": {"const": "Unknown"}, "result_ref": NULL, "error_code": ID})]},
    "Cancel": obj({"reason": string(256)}),
    "Heartbeat": obj({"progress_sequence": U64}),
    "Fault": obj({"code": ID, "category": enum("Configuration", "Protocol", "Device", "Execution", "Resource", "Persistence", "Delivery", "Internal"),
                  "message": string(2048), "retryability": enum("Never", "AfterRecovery", "Safe")}),
    "Drain": obj({"budget_ns": POS64}),
    "Stop": obj({"reason": string(256)}),
    "LeaseRelease": obj({"pool_id": ID, "lease_id": ID, "slot_id": U64, "slot_generation": POS64}),
    "ResultEvent": obj({"event": {"$ref": "#/$defs/event"}}),
    "DeliveryReport": {"oneOf": [
        obj({"event_id": ID, "output_instance_id": ID, "state": enum("Pending", "Accepted", "DurablyQueued", "TransportConfirmed", "BusinessAcked"), "attempt": POS64, "error_code": NULL}),
        obj({"event_id": ID, "output_instance_id": ID, "state": enum("Failed", "Expired", "Unknown"), "attempt": POS64, "error_code": ID})]}
}
device_fields = PAYLOADS["DeviceTask"]["properties"]
PAYLOADS["DeviceTask"] = {"oneOf": [
    obj({**device_fields, "result_id": NULL, "quality": {"const": "Unknown"}}),
    obj({**device_fields, "result_id": ID, "quality": enum("OK", "NG")})]}
PAYLOADS["DeliveryTask"] = obj({"event":{"$ref":"#/$defs/event"},"output_instance_id":ID,"attempt":POS64,"budget_ns":POS64})
PAYLOADS["DeliveryFinished"] = PAYLOADS["DeliveryReport"]
PAYLOADS["CheckFinished"] = obj({"check":{"$ref":"#/$defs/check"}})
TASK_TYPES = {"SubmitTask", "TaskAccepted", "TaskProgress", "TaskFinished", "Cancel","CaptureTask","CaptureFinished","InspectTask","DeviceTask","DeviceFinished","DeliveryTask","DeliveryFinished","CheckFinished"}
SCHEMA = {
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "title": "Vision local IPC protocol 1.0",
    **obj({"protocol_major": {"type": "integer", "const": 1}, "protocol_minor": {"type": "integer", "const": 0},
           "message_type": enum(*PAYLOADS), "request_id": ID, "run_id": ID, "worker_id": ID,
           "worker_epoch": POS64, "sequence": POS64,
           "correlation": {"oneOf": [NULL, {"$ref": "#/$defs/correlation"}]}, "payload": {"type": "object"}}),
    "$defs": {**EVENT["$defs"], "event": {k: v for k, v in EVENT.items() if k != "$defs"}},
    "allOf": []
}
for kind, payload in PAYLOADS.items():
    properties = {"payload": payload}
    if kind in TASK_TYPES:
        properties["correlation"] = {"$ref": "#/$defs/correlation"}
    elif kind != "Fault":
        properties["correlation"] = NULL
    SCHEMA["allOf"].append({"if": {"properties": {"message_type": {"const": kind}}},
                          "then": {"properties": properties}})


def examples():
    config = {"recipe_hash": "sha256:" + "a" * 64, "config_revision": "1"}
    event = json.loads((ROOT / "examples/contracts/result-event-v1.json").read_text(encoding="utf-8"))
    frame = json.loads((ROOT / "examples/contracts/frame-descriptor-v1.json").read_text(encoding="utf-8"))
    frame.update(run_id="run-1",worker_id="worker-1",worker_epoch="1")
    payloads = {
        "Hello": {"token": "EXAMPLE-NOT-A-LIVE-CREDENTIAL-00000000", "required_capabilities": ["capture-v1","inspect-v1","device-v1","delivery-v1","inference-v1"]},
        "Configure": config, "Ready": config,
        "SubmitTask": {"operation": "Inspect", "input_ref": "frame-1", "budget_ns": "1000000000"},
        "DeviceTask":{"session_id":"session-1","command_sequence":"1","ready":True,"arrival":False,"position":True,"safety_ok":True,"result_id":None,"quality":"Unknown"},
        "DeviceFinished":{"session_id":"session-1","command_sequence":"1","ready":True,"arrival_sequence":"0","position":True,"safety_ok":True,"ack_id":None},
        "TaskAccepted": {}, "TaskProgress": {"progress_sequence": "1"},
        "CaptureTask":{"slot_count":2,"slot_bytes":"786432","write_frame":frame,"budget_ns":"1000000000"},
        "InspectTask":{"slot_count":2,"slot_bytes":"786432","read_frame":frame,"budget_ns":"1000000000"},
        "CaptureFinished":{"execution_state":"Succeeded","frame":frame,"error_code":None},
        "TaskFinished": {"execution_state": "Succeeded", "quality": "OK", "result_ref": "result-1", "error_code": None},
        "Cancel": {"reason": "operator cancel"}, "Heartbeat": {"progress_sequence": "0"},
        "Fault": {"code": "MODEL.FAILED", "category": "Execution", "message": "example failure", "retryability": "AfterRecovery"},
        "Drain": {"budget_ns": "1000000000"}, "Stop": {"reason": "normal stop"},
        "LeaseRelease": {"pool_id": "pool-1", "lease_id": "lease-1", "slot_id": "0", "slot_generation": "1"},
        "ResultEvent": {"event": event},
        "DeliveryTask":{"event":event,"output_instance_id":"test-output","attempt":"1","budget_ns":"1000000000"},
        "DeliveryFinished":{"event_id":event["event_id"],"output_instance_id":"test-output","state":"BusinessAcked","attempt":"1","error_code":None},
        "DeliveryReport": {"event_id": "event-1", "output_instance_id": "http-1", "state": "BusinessAcked", "attempt": "1", "error_code": None}
    }
    check = json.loads(json.dumps(event["result"]["checks"][0]))
    check["correlation"] = {"run_id":"run-1","worker_id":"worker-1","worker_epoch":"1","inspection_id":"inspection-1","check_id":"surface","task_id":"task-1","attempt":"1"}
    payloads["CheckFinished"] = {"check":check}
    for kind, payload in payloads.items():
        run = event["result"]["run_id"] if kind == "ResultEvent" else "run-1"
        corr = None
        if kind in TASK_TYPES:
            corr = {"run_id": run, "worker_id": "worker-1", "worker_epoch": "1", "inspection_id": "inspection-1",
                    "check_id": "surface", "task_id": "task-1", "attempt": "1"}
        yield kind, {"protocol_major": 1, "protocol_minor": 0, "message_type": kind, "request_id": "c-1",
                     "run_id": run, "worker_id": "worker-1", "worker_epoch": "1", "sequence": "1",
                     "correlation": corr, "payload": payload}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    outputs = {ROOT / "schemas/ipc-v1.schema.json": SCHEMA}
    outputs.update({ROOT / "examples/ipc" / (kind + ".json"): value for kind, value in examples()})
    for target, value in outputs.items():
        text = json.dumps(value, ensure_ascii=False, indent=2) + "\n"
        if args.check:
            if not target.exists() or target.read_text(encoding="utf-8") != text:
                raise SystemExit(f"Drift: {target}")
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8")
    print(f"IPC schema/examples: {len(outputs)} files {'checked' if args.check else 'generated'}")


if __name__ == "__main__":
    main()
