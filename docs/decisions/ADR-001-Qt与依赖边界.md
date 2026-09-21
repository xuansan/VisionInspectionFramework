# ADR-001：Qt 与构建依赖边界

日期：2026-09-18。状态：采用，依据用户批准的第一批实施范围。关联 A01/T01。

## 决定

当前 Qt 基线固定 6.8.3 MSVC2022 x64，动态链接。默认采用审查记录确定的开源 LGPLv3 路线，不要求 Qt 商业授权。自有代码最终 LICENSE 仍待维护者选择，不在本次替维护者作许可声明。

Qt 白名单为 Core/Gui/Widgets/Network。本批窗口只需前三项，Network 留给后续 IPC 适配。Qt HTTP Server、MQTT 及其他附加模块不自动加入。

- contracts、inspection、runtime 核心、application 和 plugin-sdk 公共接口保持纯 C++。
- Qt 用于 UI、传输/进程外围适配和需要事件循环的宿主。
- 原生相机、推理、存储及输出库未来只在对应 worker target 链接；不因本机已安装就全部引入主控。
- `VISION_BUILD_GUI=OFF` 时不能查找 Qt 或把 Qt 头文件/运行库变成核心构建条件。

当前真实 target：`vision_contracts → 无第三方链接`；`vision_application → vision_contracts`；CLI/GUI 使用同一 application 的构建信息接口。

inspection/runtime/插件实现尚未创建，不用空目录或空 target 声称已完成。新增时必须登记允许依赖并扩展 CMake 架构检查。当前检查是链接依赖白名单，不是完整 C++ include 静态分析工具。

后续更新：ADR-004 批次已创建 inspection/runtime 并扩展依赖白名单及负向测试；JSON 单独放在 serialization。上段为第一批骨架历史，当前依赖见 `docs/protocols/核心契约-v1.md`。

## 构建策略

- C++20；本机验证 MSVC 19.42.34436.0 / Windows SDK 10.0.22621.0 / CMake 3.29.5-msvc4。
- 仓库最低 CMake 3.25。v143 是工具系列选择，实际补丁版本记入验证报告，不谎称各机器自动锁定相同编译器。
- doctest 2.5.3 为唯一测试库，校验单头文件 SHA256；本机优先已安装头文件，干净环境可显式允许 HTTPS 下载。默认配置不静默访问网络。
- 项目默认不包含 SDK 二进制、源码解包、参考仓库、密钥或 out 构建产物；离线 SDK 放本机 `开发环境/`。
- 当前无需引入完整包管理器；以后 worker 按固定版本逐个接入。不可为了简化配置直接 include 会引入全部依赖的本机辅助文件。

## 依赖状态

| 组件 | 本机已验证版本 | 正式骨架是否使用 |
|---|---|---|
| Qt Base | 6.8.3 | GUI 可选 |
| doctest | 2.5.3 | 测试可选 |
| OpenCV | 4.12.0 | 尚未链接 |
| ONNX Runtime CPU | 1.30.0 | 尚未链接 |
| SQLite | 3.53.4 | 尚未链接 |
| JSON | 3.12.0 | 尚未链接 |
| spdlog | 1.17.0 | 尚未链接 |
| cpp-httplib | 0.56.0 | 尚未链接 |
| curl | 8.22.0 | 尚未链接 |
| RustFS | 1.0.0 | 外部开发服务，默认不启动 |

上述 SDK 版本与来源详见本机清单，未将其全部定为永远不变的生产版本。

## 发布要求与边界

保留原审查文档的许可义务：具体 Qt 源码、声明、替换机制、平台插件和第三方条款在 T31/T32 核查。本批复制 DLL 到构建目录只用于开发运行，不能当成合规发行包。

审查映射：审查第 1～3、11 节 → 本决策/主计划 3.4 → T01/T02/T09/T31。A01 关闭的是设计边界，非最终发行许可审核。
