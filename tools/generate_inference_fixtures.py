"""Original input-dependent ONNX contract fixture, not a trained defect model.
Requires pinned onnx 1.17.0; reference execution uses ONNX ReferenceEvaluator (not ORT).
"""
import hashlib
import json
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "out/model-tools/python"))
import numpy as np
import onnx
from onnx import helper as h, TensorProto as T, numpy_helper as nh
from onnx.reference import ReferenceEvaluator

OUT = ROOT / "examples/models"
OUT.mkdir(parents=True, exist_ok=True)
(OUT/"black.pgm").write_bytes(b"P5\n8 8\n255\n"+bytes(64))
base = np.array([[[2, 2, 6], [2, 2, 6], [2, 2, 2], [2, 2, 2],
                  [.6, .5, .1], [.1, .1, .7]]], dtype=np.float32)
weight = np.zeros_like(base)
weight[:, 4:, :] = .1
nodes = [h.make_node("ReduceMean", ["images"], ["mean"], keepdims=0),
         h.make_node("Mul", ["mean", "weight"], ["adjustment"]),
         h.make_node("Add", ["base", "adjustment"], ["output0"])]
model = h.make_model(h.make_graph(nodes, "vision-original-detection-fixture",
    [h.make_tensor_value_info("images", T.FLOAT, [1, 3, 8, 8])],
    [h.make_tensor_value_info("output0", T.FLOAT, [1, 6, 3])],
    [nh.from_array(base, "base"), nh.from_array(weight, "weight")]),
    opset_imports=[h.make_opsetid("", 13)], producer_name="vision-framework-contract-tests")
model.ir_version = 8
onnx.checker.check_model(model)
onnx.save(model, OUT / "detection.onnx")
digest = hashlib.sha256((OUT / "detection.onnx").read_bytes()).hexdigest()
config = dict(schema_version=1, model="detection.onnx", model_hash="sha256:"+digest,
              type="detection", input="images", output="output0", labels=["scratch", "spot"],
              width=8, height=8, max_results=64, score=.25, iou=.45)
(OUT / "detection.json").write_text(json.dumps(config, indent=2)+"\n", encoding="utf-8")
reference = {}
for value in [0, 128, 255]:
    tensor = np.full((1,3,8,8), value/255, dtype=np.float32)
    raw = ReferenceEvaluator(model).run(None, {"images":tensor})[0]
    # Independent fixture geometry/NMS oracle: candidates 0/1 identical class+box;
    # candidate 2 belongs to class1, with the highest score.
    reference[str(value)] = [
        dict(class_id=1, score=float(raw[0,5,2]), box=[5,5,7,7]),
        dict(class_id=0, score=float(raw[0,4,0]), box=[1,1,3,3])]
(OUT / "detection-reference.json").write_text(json.dumps(reference, indent=2)+"\n", encoding="utf-8")
(OUT / "provenance.json").write_text(json.dumps(dict(
    generator="tools/generate_inference_fixtures.py", onnx=onnx.__version__,
    reference="onnx.reference.ReferenceEvaluator", source="Original input-dependent contract fixture; no training weights",
    accuracy_scope="No production defect accuracy claim",
    model_sha256=digest), indent=2)+"\n", encoding="utf-8")
print("Generated detection model, manifest, independent reference and provenance")
base = np.array([[1000,1001,999]], dtype=np.float32)
weight = np.array([[2,0,-1]], dtype=np.float32)
model = h.make_model(h.make_graph(nodes, "vision-original-classification-fixture",
    [h.make_tensor_value_info("images", T.FLOAT, [1,3,8,8])],
    [h.make_tensor_value_info("output0", T.FLOAT, [1,3])],
    [nh.from_array(base,"base"),nh.from_array(weight,"weight")]),
    opset_imports=[h.make_opsetid("",13)], producer_name="vision-framework-contract-tests")
model.ir_version=8
onnx.checker.check_model(model)
onnx.save(model, OUT/"classification.onnx")
config.update(model="classification.onnx",model_hash="sha256:"+hashlib.sha256((OUT/"classification.onnx").read_bytes()).hexdigest(),
              type="classification",labels=["good","scratch","spot"],scores="logits",ok_label="good")
(OUT/"classification.json").write_text(json.dumps(config,indent=2)+"\n",encoding="utf-8")
reference={}
for value in [0,128,255]:
    raw=ReferenceEvaluator(model).run(None,{"images":np.full((1,3,8,8),value/255,dtype=np.float32)})[0][0].astype(np.float64)
    probabilities=np.exp(raw-raw.max());probabilities/=probabilities.sum()
    reference[str(value)]=probabilities.tolist()
(OUT/"classification-reference.json").write_text(json.dumps(reference,indent=2)+"\n",encoding="utf-8")
config["score"]=.99
(OUT/"classification-uncertain.json").write_text(json.dumps(config,indent=2)+"\n",encoding="utf-8")
base=np.array([[[2,2,6],[2,2,6],[2,2,2],[2,2,2],[.6,.5,.1],[.1,.1,.7],[1,1,-1]]],dtype=np.float32)
weight=np.zeros_like(base);weight[:,4:6,:]=.1
proto=np.array([[[[1 if x<2 else -1 for x in range(4)] for y in range(4)]]],dtype=np.float32)
model=h.make_model(h.make_graph(nodes+[h.make_node("Identity",["proto"],["output1"])],
    "vision-original-segmentation-fixture",[h.make_tensor_value_info("images",T.FLOAT,[1,3,8,8])],
    [h.make_tensor_value_info("output0",T.FLOAT,[1,7,3]),h.make_tensor_value_info("output1",T.FLOAT,[1,1,4,4])],
    [nh.from_array(base,"base"),nh.from_array(weight,"weight"),nh.from_array(proto,"proto")]),
    opset_imports=[h.make_opsetid("",13)],producer_name="vision-framework-contract-tests")
model.ir_version=8;onnx.checker.check_model(model);onnx.save(model,OUT/"segmentation.onnx")
config.update(model="segmentation.onnx",model_hash="sha256:"+hashlib.sha256((OUT/"segmentation.onnx").read_bytes()).hexdigest(),
    type="instance_segmentation",labels=["scratch","spot"],score=.25,prototypes="output1")
config.pop("scores");config.pop("ok_label")
(OUT/"segmentation.json").write_text(json.dumps(config,indent=2)+"\n",encoding="utf-8")
raw,prototypes=ReferenceEvaluator(model).run(None,{"images":np.zeros((1,3,8,8),dtype=np.float32)})
reference=[]
for index in [2,0]:
    logits=np.einsum("k,khw->hw",raw[0,6:,index],prototypes[0])
    mask=np.repeat(np.repeat(logits>=0,2,axis=0),2,axis=1)
    center=6 if index==2 else 2
    crop=np.zeros((8,8),dtype=bool);crop[center-1:center+1,center-1:center+1]=True
    reference.append((mask&crop).astype(np.uint8).tolist())
(OUT/"segmentation-reference.json").write_text(json.dumps(reference,indent=2)+"\n",encoding="utf-8")
provenance=json.loads((OUT/"provenance.json").read_text(encoding="utf-8"))
provenance["models"]={name:hashlib.sha256((OUT/(name+".onnx")).read_bytes()).hexdigest()
    for name in ["detection","classification","segmentation"]}
provenance["numpy"]=np.__version__
provenance["license"]="Project original fixture; project license remains NOASSERTION until selected"
(OUT/"provenance.json").write_text(json.dumps(provenance,indent=2)+"\n",encoding="utf-8")
