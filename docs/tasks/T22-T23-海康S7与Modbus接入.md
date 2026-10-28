# 海康工业相机、S7 与 Modbus 接入

日期：2026-10-09。状态：本批软件实施及回归完成，真实设备验收待执行。

## 边界
Windows x64 优先；Camera/Communication ABI 不变。海康与 S7 在 plugins 内封装；协议实现不进入主控或 UI。SDK 为可选构建依赖，不提交厂商二进制。禁止自动操作现场 PLC；设备动作需明确测试地址。软件验收不解除 Production 限制。

## 顺序与验收
1. 保存全量基线，定位原有失败。
2. 海康：序列号选设备、软件触发、Mono8/RGB8/BGR8、有限图像缓冲、SDK 归还、租约及异常退出。假后端与实际宿主测试；本机 SDK 编译验证。
3. S7：固定版本 Snap7 动态链接、受限 DB 读写、统一设备握手；软件服务端及坏响应测试。PLC 型号和访问设置实机待确认。
4. Modbus：复用 FC03/FC16，补边界和取消检查，不改变已有业务确认语义，不自动重放动作。
5. 可配置运行入口及文档；逐阶段 windows-all，最终全量回归和模拟使用。

## 当前环境
本机 MVS ReleaseNote 为 5.1.0、SDK 4.8.2.2，运行 DLL 文件版本 4.8.2.1；头文件与 x64 lib 可用。需验证实际加载版本。S7 候选 Snap7 官方提交 30f37da3114024a71ba93f7fd855c680b97a406f，源码与许可在被忽略的开发环境目录中。

## 风险
宿主每个采集请求调用 open/close；插件须分离物理相机与单次图像映射生命周期，关闭后不能继续访问共享槽位。取消仅设置状态，不在并发线程直接销毁 SDK 句柄。SDK 永久阻塞由宿主期限和进程退出处置。
S7/Modbus 写入成功不代表业务成功，要求匹配会话、序号和结果回执；断线后动作状态不明不可盲目重发。

## 基线
全量基线目录 out/validation/full-2026-10-08T17-16-59-830Z。核心通过；GUI 首次出现 demo.durable.restart 与 runtime.host.auto-rotation 失败，新增代码前已存在，待复核。

## 2026-10-09 实施记录

### 上一轮交付（历史记录）

- 新增 `plugins/hik-camera`，将 MVS SDK 封装在 Camera 插件内部，未改变 Camera ABI、主控和 UI。
- 默认 Windows 核心构建不依赖 MVS；启用 `VISION_BUILD_HIK_CAMERA=ON` 时才检查并链接本机 MVS x64 开发包。
- 增加 fake backend 测试，覆盖插件创建、初始化、枚举和非法打开参数；不访问真实设备。
- 本机 MVS 真实适配器已完成编译验证：`vision-hik-camera.dll` 成功生成。
- `node tools/build.cjs windows-core` 全量回归通过：30/30，包括新增 `capture.hik.fake`。

### 上一轮状态更正

上一轮的 S7 仅为 write_hex/read_hex 原型，不兼容 DeviceSession；海康测试没有调用 poll_frame，
不能称为覆盖完整异常矩阵。仅 windows-core 30/30 也不能作为整批验收。
以下为 2026-10-09 继续实施的实际结果，保留前述记录以说明更正原因。

### 本轮实现

- S7 改为会话 token、uint64 序号、结果 token、ready/position/safety_ok 与 ACK 的完整 Communication 适配，移除原型 hex API。
- 修复 S7 manifest 缺少框架必需字段的问题；仍为可选构建和动态链接，SDK 不进入公共 ABI。
- S7/Modbus 限制数字 IPv4、配置范围、重叠地址、重复 JSON key/深度/大小、非法质量；停止后拒绝命令。
- 命令写入前占用序号；读写或回执错误使实例失效，不自动重发不确定动作；报告缓冲区不足不丢失待取报告。
- 海康契约测试实际执行触发/取帧/像素读取、Mono8/RGB8/BGR8、重复租约、重开映射、完成缓冲不足、无帧、断开、坏帧。
- 海康 fake DLL 通过真实 worker-host 验证 normal/missing/disconnect/bad-size/hang/crash；要求健康宿主在故障后继续采集，最终租约清零。
- S7 使用 Snap7 软件服务端，Modbus 使用本机 TCP 软件服务端；测试仅监听 127.0.0.1。
- PLC 契约测试校验重复序号和旧会话不产生写入、配置边界、连接拒绝、错误回执及停止；进程测试校验正常结果确认和错误 ACK 的 Unknown。
- 配置/报文布局/部署说明见 [T22-T23-deployment.md](T22-T23-deployment.md)。Snap7 许可依据固定提交源文件核验；不发布厂商二进制。

### 验收状态

阶段核心 30/30、GUI 251/251、S7 可选配置首轮 251/251 已通过。
最终 S7 全套 253/253；windows-all 核心 30/30、GUI 251/251、IPC 237/237，协议 13/92/7 及 RustFS 全部通过。
证据见 [VAL-028](../acceptance/VAL-20261009-028-海康S7与Modbus软件验收.md)。
本机 MVS 真实插件再次编译通过；未进行真实设备枚举、采集或 PLC 连接。

### 实机边界

软件验收与实机验收分开。真实相机/PLC 型号、固件、驱动、访问设置、实际节拍、物理断线及长稳仍待硬件测试。
T22/T23 父任务不因本次软件对端通过而关闭，Production 继续禁用。
