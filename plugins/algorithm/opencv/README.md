# OpenCV 传统视觉算法插件

可选 Algorithm 插件：`vision.opencv-algorithm`，OpenCV 4.12.0；
Windows x64 / MSVC v143 / 动态 CRT / Release 已作为软件验收基线。
HALCON 不在本次实现中，GUI 默认算法不切换。

## 构建与接入

默认 `VISION_BUILD_OPENCV=OFF`，关闭时不查找或链接 OpenCV。
使用已准备的 SDK：

```powershell
cmake --preset windows-ipc -DVISION_BUILD_OPENCV=ON "-DOpenCV_DIR=C:/path/to/opencv/build"
cmake --build --preset windows-ipc --config Release --parallel 4
ctest --preset windows-ipc -R "^opencv\."
```

本机 SDK 位于 `开发环境/SDK/OpenCV/opencv/build`。
部署目录包含 `vision-opencv-algorithm.dll`、`opencv-algorithm.json`、
配置对应的 OpenCV DLL、框架许可记录及 `opencv-notices/`。
公共 worker-host 加载 manifest；不要在 UI 直接加载算法 DLL。
StationConfig 的 `algorithm_manifest` 指向部署的 manifest，
`algorithm_parameters` 每个工位设置 `{"config":"绝对路径/config.json"}`。
配置文件为 UTF-8 JSON，最多 64 KiB；相对模板路径以配置文件目录为基准。
首次接入可复制同目录 `example.json` 后按工件调整阈值。

## 算法配置

`mode` 必填，只允许 `threshold`、`components`、`measure`、`template`。
不提供任意算子图、脚本或低代码流程。

| 字段 | 默认及约束 | 含义 |
|---|---|---|
| roi | 默认整图；x/y 为非负整数，width/height 为正整数 | 必须完整、不能超出图像；结果坐标始终回到原图 |
| threshold | 128，整数 0..255 | 灰度严格大于阈值为前景；invert=true 时反转 |
| invert | false，布尔 | 黑色目标可使用反向阈值 |
| morphology | none / open / close | 单次矩形开/闭运算 |
| kernel | 3，奇数 1..31 | 形态学核尺寸 |
| min_area / max_area | 1 / 16777216 | 按连通域像素面积筛选，包含边界 |
| min_count / max_count | 1 / 1，整数 0..64 | 筛选后数量是否符合要求 |
| min_width / max_width | 0 / 4096 | measure 模式轴对齐包围框宽度范围，px |
| min_height / max_height | 0 / 4096 | measure 模式轴对齐包围框高度范围，px |
| template | template 模式必填路径 | 二进制 PGM P5、maxval=255，无注释；2..1024 像素每边 |
| min_score | 0.8，有限数值 0..1 | 固定尺度和角度的 TM_CCOEFF_NORMED 最佳位置门限 |

前三种模式复用二值化、可选形态学、8 邻域连通域统计。
threshold 输出前景数量与组件统计；components 增加面积/数量判定；
measure 在此基础上检查轴对齐宽高。当前共用数量判定，不是只生成二值图的变换插件。
没有目标或数量不符合要求为执行成功但 NG。
面积筛选后的每个组件输出面积、质心 x/y、宽/高；
不合格尺寸以原图 `box_xyxy` 缺陷记录表示。上界坐标不包含在框内。
这不是轮廓多边形提取、亚像素测量、旋转矩形或毫米标定。

template 输出最佳匹配分数，达标时输出 match_x/match_y。
未达标为 NG；多个候选仅取 OpenCV 的一个最佳位置，不保证唯一性。
常量模板拒绝。无旋转/尺度搜索、多模板或多目标定位。
模板最多 16 MiB，受限头部检查后解码；搜索位置数乘模板像素数
最多 5 亿，超出范围必须缩小 ROI/模板。

## 内存、期限与结果

只接受 Mono8/RGB8/BGR8，验证 stride、offset、length 及 ROI。
Mapping::read 已按 offset 切片；在回调租约内克隆/转换，不修改共享图像，
不把图像指针、cv::Mat 或 OpenCV 类型传入公共 ABI。
图像每边最多 4096、总像素最多 4194304、含填充长度最多 64 MiB。
OpenCV 内部线程固定为 1；此设置只作用于加载该插件的 worker 进程。
筛选后最多 50 个组件（252 条测量），超限返回执行失败/Unknown，不能静默截断。

沿用标准 evidence：execution_state、quality、defects、measurements、elapsed_ns。
面积使用 px2，长度使用 px，数量 count，分数 ratio。
参数、图像、ROI、模板、资源上限、期限错误均不可伪装为 NG。
成功记录实际算法耗时；失败保留有界错误信息，elapsed_ns 当前记 0。
主动停止返回 Cancelled；任务预算在运算阶段之间检查。
OpenCV 单次原生运算无法承诺立即中断，最终硬期限和异常隔离依靠
独立 worker-host 的监督与真实进程退出；未退出前不可提前回收图像租约。

## 测试与发布限制

专项涵盖精确像素几何、ROI、填充、RGB/BGR、形态学、模板、
无目标/多目标、资源上限、取消、期限、真实 DLL/共享图像、
双相机结果、重建工位及故障插件与健康工位共存。
详细证据见 `docs/acceptance/VAL-20261010-032-OpenCV算法插件.md`。
重启测试建立新的 Station 生命周期，不表示已停止的 Station 对象可再次 start。

本机使用官方预编译 opencv_world4120.dll，实际包含额外模块；
find_package 请求 core/imgproc/imgcodecs 不会裁剪 world 二进制。
没有额外部署 FFmpeg 视频插件。来源与哈希见 dependencies/artifacts.lock.json。
`notices/` 保留 SDK 的 Apache 文本及源码自带的第三方许可资料，
它不是已审定的完整二进制 SBOM。world 包含 IPP 等额外组件，
正式再分发前须补齐所用构建实际内嵌组件清单和相关通知，
或重建仅需模块且关闭不需要后端的 OpenCV。此项纳入发布门槛；
软件测试通过不等于许可证包装和生产现场验收已经完成。
