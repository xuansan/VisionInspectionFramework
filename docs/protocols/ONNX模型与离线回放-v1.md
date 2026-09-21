# ONNX模型与离线回放 v1

Windows CPU，ORT 1.30.0，原创契约模型在 `examples/models/`。
这不是训练平台，也不保证任意YOLO导出的模型可直接加载。

## 输入与输出

输入固定float32 `[1,3,H,W]`，RGB、NCHW、除255，
最近邻等比例letterbox，填充114，ROI按原图像素给出。
模型宽高最大2048，输入图像最多16MiB，标签唯一且有界。
配置中的model_hash校验实际交给ORT的模型字节。

| 类型 | 输出契约 | 判定 |
|---|---|---|
| detection | `[1,4+C,N]`，cx/cy/w/h + 类别概率，无objectness | NMS后有目标NG、无目标OK |
| classification | `[1,C]`，scores显式logits/probabilities | ok_label为OK，其他NG；未过阈值Failed/Unknown |
| instance_segmentation | `[1,4+C+K,N]`与`[1,K,Hm,Wm]` | 检测判定+原图掩码引用 |

N<=4096，最大64目标；class-aware NMS，分数相同保持输入顺序。
分类Top-K由max_results指定，概率和容差1e-4；logits稳定softmax。
掩码阈值固定0.5，最大32原型通道和256×256原型；单掩码1MiB，
总16MiB。本地临时缓冲目录64MiB/4096文件上限，不是RustFS持久化。
掩码storage固定local_ephemeral，JSON只传hash/尺寸/实例索引。

## Qt试运行

启动工作站后点击“打开模型试运行 / 图像工具”，或：

```powershell
& ".\out\build\windows-gui\apps\workstation\Release\vision-workstation.exe" --model-tool
```

选择模型配置JSON和二进制PGM/PPM；编辑阈值和ROI（宽高0表示全图）。
滚轮缩放、拖动平移、鼠标查看像素；执行后显示框/掩码及标准CheckResult。
UI读取图片交给独立工具进程；试运行只构造文件相机与算法worker。
取消会终止工具，Windows Job负责子进程清理，下一次运行使用新身份。

## CLI创建快照并运行

工具路径：
`out/build/windows-gui/apps/model-tool/Release/vision-model-tool.exe`
（IPC构建也提供同名工具）。

```powershell
$request = @{
    config = (Resolve-Path ".\examples\models\detection.json").Path
    image = (Resolve-Path ".\examples\models\black.pgm").Path
    width = 8; height = 8; channels = 1
    score = 0.25; roi = @(0,0,0,0)
    output = (New-Item -ItemType Directory -Force ".\out\my-replay").FullName
} | ConvertTo-Json -Compress
& ".\out\build\windows-gui\apps\model-tool\Release\vision-model-tool.exe" --request $request
```

成功返回mode=Replay、snapshot_hash、manifest和标准check。
使用返回的完整manifest和snapshot_hash重新运行：

```powershell
& ".\out\build\windows-gui\apps\model-tool\Release\vision-model-tool.exe" --replay "返回的manifest完整路径" "sha256:返回的哈希" ".\out\my-replay"
```

快照冻结模型、有效配置（含标签/阈值/ROI）及图像，并固定推理版本和预处理契约。
加载时校验manifest和全部资产，文件相机校验实际传输像素，模型worker校验加载字节。
失败不默认为OK，不允许请求PLC/HTTP等输出字段；不修改生产计数或原结果。
已有快照不覆盖更新，修改外部文件会导致校验失败。UI临时快照随窗口关闭清理；
要保留复检证据请使用CLI显式output目录，调用者负责磁盘生命周期。

## 测试模型与验证范围

`tools/generate_inference_fixtures.py`使用onnx 1.17.0生成输入相关小模型，
ONNX ReferenceEvaluator执行参考，NumPy生成分类概率和掩码。
无外部训练权重，不声明现场识别准确率；真实YOLO导出、GPU/其他算子覆盖需独立验收。
T24以后再接数据库历史与RustFS；当前CLI按显式资产包回放。
