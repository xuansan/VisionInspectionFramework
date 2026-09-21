# ADR-006：IPC 1.0 与可复用传输边界

日期：2026-09-19。状态：采用。依据已批准的T05完成批次，不改变运行进程架构。

1. `vision_ipc` 使用纯C++公共类型，私有依赖JSON及既有ResultEnvelope编解码；CMake白名单防止直接引入Qt。
2. `vision_ipc_qt` 是单独Qt Core/Network适配，负责socket所有权、有限批次IO、联合发送限额与期限检查；不依赖GUI。
3. `VISION_BUILD_LOCAL_IPC` 单独控制适配及诊断工具，`windows-ipc` preset 不构建Widgets界面。默认纯核心不查找Qt。
4. 正式协议1.0固定15种消息公共头及严格payload，规范与例子见IPC-v1。业务资产使用引用，不通过任意JSON绕过校验。
5. Configure/SubmitTask进入有限请求表，接纳/进度不等于完成；已发请求超时或断线不能推断未执行，不自动重放。
6. Hello只完成通信认证，不表示生产就绪。健康租约、唯一主控、宿主恢复仍是A03/A07与T06的前置工作。
7. 拒绝未知主/次版本及未知必需能力。跨版本扩展必须新决策，不静默忽略字段。
8. `vision-ipc-probe` 是可运行诊断程序，测试使用正式适配器；故障注入仅作用于自身端点和子进程，不是生产Supervisor。

验证：VAL-20260919-008。本次不改变第三方版本或许可路线；Qt Network此前已在白名单中。
