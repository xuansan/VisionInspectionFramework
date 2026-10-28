# 海康、S7、Modbus：配置、部署与验收

日期：2026-10-09。Windows x64 / MSVC v143 / Release。

## 运行边界

Camera、Communication 插件均由已有 worker-host 独立进程加载。主控和 UI 不链接 MVS/Snap7。
本次不改变公共 ABI，也不新增 src/s7；SDK 适配在 plugins/s7-device。
plugins/common/plc_input.hpp 只复用输入校验，不执行网络操作。
S7/Modbus 为设备通信插件，既读取输入又输出握手结果，不是单向 ResultOutput 插件。
本次保持 Production 禁用，软件验收不代表现场验收。

## 构建

默认构建关闭两个可选真实设备插件，海康 fake 只在 BUILD_TESTING 时构建。
在正常 Qt 开发环境下，可用以下 CMake 参数生成独立验证目录：

```powershell
cmake -S . -B out/build/windows-s7 -G "Visual Studio 17 2022" -A x64 `
  -DBUILD_TESTING=ON -DVISION_BUILD_LOCAL_IPC=ON -DVISION_USE_LOCAL_SDK=ON `
  -DVISION_BUILD_S7_DEVICE=ON -DVISION_SNAP7_ROOT="<本机 Snap7 源码根目录>"
cmake --build out/build/windows-s7 --config Release --parallel 4
ctest --test-dir out/build/windows-s7 -C Release --output-on-failure

cmake -S . -B out/build/windows-hik -G "Visual Studio 17 2022" -A x64 `
  -DBUILD_TESTING=OFF -DVISION_BUILD_HIK_CAMERA=ON `
  -DVISION_MVS_ROOT="C:/Program Files (x86)/MVS/Development"
cmake --build out/build/windows-hik --config Release --target vision-hik-camera
```

若 cmake 不在 PATH，使用 Visual Studio 附带的 cmake.exe/ctest.exe 绝对路径。
默认全量回归仍执行 `node tools/build.cjs windows-all`；它不自动启用可选 Snap7，
因此必须额外执行上述 S7 配置测试，不能仅凭 windows-all 宣称 S7 验收通过。

## 相机配置

```json
{"serial":"替换为实际相机序列号","width":640,"height":480,"format":"Mono8","exposure_us":1000,"gain":0}
```

使用已有 capture::qt::Session，传入 vision-hik-camera.json 路径、上述参数和一致的 ImageLayout。
format 可取 Mono8、RGB8、BGR8。布局必须匹配，不能把不同分辨率的 SDK 帧直接交给下游。
支持 GigE/USB 枚举、按序列号打开和软件触发；本版不宣称支持硬件触发、自由运行或相机热插拔自动恢复。
正常请求 close 仅释放该请求的映射；物理相机句柄在实例结束时关闭。
未完成采集被取消/关闭，或 SDK 读取失败，会使实例失效；由宿主退出后重新创建，
不把上一次触发迟到的图像配给下一件产品。驱动或操作系统崩溃不在进程隔离保证范围内。

## PLC 配置

S7 参数示例（此地址仅示例；不自动运行）：

```json
{"address":"192.0.2.10","port":102,"rack":0,"slot":1,"db":1,"write_base":100,"read_base":200}
```

Modbus TCP：

```json
{"address":"192.0.2.10","port":502,"unit":1,"write_base":100,"read_base":200}
```

使用已有 application::qt::DeviceSession，把对应 manifest 和参数传给构造函数。
S7 的 base 是 DB **字节偏移**，写 48 字节、读 50 字节；Modbus 的 base 是 **16 位寄存器地址**，
写 24 个、读 25 个。两块区间不得重叠或越过 65536 边界。地址只接受数字 IPv4，避免隐式 DNS。
不再接受旧版 write_hex/read_hex 原型字段。
S7 按配置 DB 执行 DBRead/DBWrite，Modbus 使用 FC16/FC03。
不支持任意远程写入命令、PLC 编程、CPU 启停；也未实现 Modbus RTU。

PLC 侧必须有匹配握手程序。不能把任意已有 DB/寄存器地址直接配置进来就试写。
应由现场工程师确认 CPU 型号、固件、rack/slot、DB 访问权限、数据布局，以及所需的访问设置；
加密连接、符号寻址、不同 CPU 的访问模式不在本版通用兼容承诺内。

## 握手布局

以下偏移以 16 位大端 word 为单位；S7 换算成字节需要乘 2。
会话/结果 token 为 UTF-8 标识的 SHA-256 前 16 字节，按大端 word 编码；它是相关性标记，不是认证。
uint64 顺序为高位 word 在前。

| 写区 word | 内容 |
|---|---|
| 0–7 | session token |
| 8–11 | command_sequence，uint64 |
| 12 | 上位机 ready：0/1 |
| 13–20 | result token，无结果时全零 |
| 21 | 本命令是否携带结果：0/1 |
| 22 | 现有协议约定租约值 200 ms |
| 23 | Unknown=0、OK=1、NG=2 |

| 读区 word | 内容 |
|---|---|
| 0–7 | 匹配本次写区的 session token |
| 8–11 | 匹配本次写区的 command_sequence |
| 12 | bit0=ready、bit1=position、bit2=safety_ok；其他 bit 必须为零 |
| 13–16 | arrival_sequence，uint64 |
| 17–24 | 本次结果的确认 token；无结果时全零 |

PLC 应在处理完整请求后发布一致的读区快照；不能仅看到写寄存器成功就确认物理动作。
PLC 端必须实现会话接管核对、结果去重及 200 ms 看门狗过期清 ready。
主机或进程死亡后不能保证还能发送撤销命令；设备安全行为必须由 PLC 及现场安全系统保证。
ready 为真而 position/safety_ok 为假被拒绝。安全字段只是业务状态，不构成功能安全系统。
应用层以到位/安全快照和严格匹配的结果回执决定是否业务确认；通信失败归 Unknown。
同一会话中序号必须递增，写入前占用序号；坏回执、超时、断开后插件实例失效，
不重新提交状态不明的动作。重启后的结果去重与物理确认仍须 PLC/现场流程配合，
不能由 TCP 或 Snap7 自动实现 exactly-once。

## 第三方依赖与发布

海康：使用安装的 MVS 开发包，不提交头文件、lib、DLL 或安装包到项目源码。
真实部署需按海康对应版本的再分发条款安装运行时和驱动，并校验目标机器 x64 版本。
宿主使用受限 DLL 搜索路径；仅把 SDK 目录加入 PATH 不足以保证加载。
按厂商许可将必要运行库及其依赖部署到插件目录，或使用支持的系统安装方案；
不得把在开发机能链接当作目标机能加载。无需安装 MVS 也应能构建默认框架。

Snap7：本机来源为官方 davenardella/snap7，核验提交
`30f37da3114024a71ba93f7fd855c680b97a406f`。
核验文件包括 src/lib/snap7_libmain.h、release/wrappers/c-cpp/snap7.h、
lgpl-3.0.txt 和 gpl.txt；源码/包装头声明 LGPL-3.0-or-later，
不是根据根目录同时有 GPL/LGPL 文本猜测。此次在线许可页未获得可用内容，
许可记录依据该固定提交的本地源文件，不宣称完成独立法律审查。

采用可替换 snap7.dll 动态链接。测试构建只复制 DLL 到被忽略的 out 目录，不修改系统目录。
对外发布时至少逐项核对：

- 保留 LGPL、GPL 全文、版权声明和 SNAP7-NOTICE；不能只附一份 LGPL 文本就认为完成。
- 对实际分发的 DLL 提供对应源码、修改记录及构建说明，采用许可证允许的源码交付方式。
- 允许用户替换兼容库、按 LGPL 权利调试修改；安装器、签名策略或 EULA 不得抵消这些权利。
- 记录 DLL 哈希、版本、来源和所用头文件，不混用不明第三方预编译包。
- 框架自有代码许可证仍为 NOASSERTION；Snap7 的许可不替框架选定开源许可证。

本次仅在本机验证，没有生成或上传可对外分发的包含厂商 DLL 的安装包。

## 实机验收清单（仍待执行）

海康：目标型号与 SDK 版本、枚举/加载、三种输出格式、曝光及 ROI 范围、连续触发、
物理拔线、重新启动、图像与工件关联、帧丢失和长稳。
PLC：明确批准的测试地址、CPU/固件及访问模式、DB/寄存器映射、业务 ACK、
异常断电/断线、动作已执行但 ACK 丢失后的人工处置及节拍。
这些项目需要真实硬件与现场条件，软件模拟通过不能替代。
