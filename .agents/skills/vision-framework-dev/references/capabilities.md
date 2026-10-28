# 已验证范围与限制

此表是本Skill发布时的基线；使用时必须与框架当前进度和源码复核。

- 模拟采集、多进程宿主、图像租约、基础算法、Qt工作站：已有Windows演示与故障测试。
- ONNX检测/分类/实例分割：真实CPU推理、契约样例模型；外部模型形状与生产准确率须另验。
- SQLite、文件、HTTP、RustFS工具：已有软件实现；原图本地归档已接入持久模拟，不等于自动远端上传。
- ResultOutput：本Skill有可生成、可运行模板。接收示例是有界内存接收，不宣称持久或远端消费。
- 相机、Algorithm、Communication：框架已有接口/宿主路径，本Skill首版不提供对应生成器。
- 2026-10-09：可选 MVS 插件已编译，fake 采集/宿主已测；S7 与 Modbus 已测软件对端，真实相机和现场 PLC 未验收。配置和部署见框架 docs/tasks/T22-T23-deployment.md。
- ObjectStorage：S3 插件已接公共宿主及持久 agent；根据已提交结果上传关联原图，GUI 默认未启用。
- Webhook：主动 POST 与业务回执；可选 MQTT 使用 Paho，当前仅本机 broker。两者持久重放要求显式非物理幂等声明。
- 2026-10-10：plugins/message-queue 按协议组织 MQTT/Kafka/RabbitMQ/NATS JetStream；三个新插件均可选、本机连接、独立宿主和业务确认。实际 broker 与模拟测试的区别见该目录 README 和 VAL-030，远端 TLS/认证未完成。
- 自动上传、掩码持久化、远端保留清理/对账、完整生产恢复：仍待补。
- 2026-10-10：可选 plugins/database/mysql-output 与 postgresql-output，复用 ResultOutput/独立宿主/持久 agent；真实本地数据库与 TLS 反例通过。只输出结果、缺陷和已归档本地图像引用，不替代 SQLite 账本，不自动迁移或重试终态 Failed。配置和限制见 plugins/database/README.md、VAL-031。
- Linux/GPU、长稳、现场节拍、正式发布包与开源许可证选择：未完成相关验收。
- 2026-10-10：可选 plugins/algorithm/opencv 复用 Algorithm/独立宿主/共享图像；
  阈值形态学、连通域、像素轴对齐尺寸、固定尺度角度模板。模板是单最佳位置，
  没有毫米标定、旋转尺度匹配、轮廓多边形或 HALCON；
  配置/资源边界与实际验收见该目录 README、VAL-032。

遇到缺SDK：列出需要的厂商版本/型号和接口资料，可继续模拟开发；不生成猜测的SDK调用并宣布可用。
遇到能力差异：引用当前源码和验收范围；文档规划不能替代已实现证据。
