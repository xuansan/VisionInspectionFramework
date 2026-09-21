# T09：插件 SDK 和公共宿主

日期：2026-09-19。前置T02/T05/T06，T08首轮通过。用户授权按计划连续推进。

状态：Windows插件基础与Algorithm公共宿主验收通过，见[VAL-010](../acceptance/VAL-20260919-010-共享图像与插件宿主验收.md)。其他类别为已定义接口，实际业务适配在对应T任务实现，不冒充已运行设备或输出能力。

## 设计与实施

- `plugin_sdk`只提供固定工具链下的POD视图、状态、抽象接口和三个C导出（describe/create/destroy）；不跨DLL传STL所有权，所有调用noexcept，销毁由创建模块执行。
- `plugin_runtime`检查manifest和实际导出身份、版本、平台、工具链、构建模式、kind、线程模型、容量；路径规范化必须留在包目录；依赖使用DLL自身目录及Windows系统目录，不从当前工作目录任意找DLL。首版每宿主仅一个实例，依赖插件包的解析留作明确拒绝，不能静默忽略。
- 公共 `vision-worker-host` 加载插件只发生在IPC Configure被接受之后；主控Job绑定成功才发送Configure。插件操作由一个工作线程串行执行，IPC线程保持响应；插件自己崩溃只影响宿主进程。
- 宿主启动即验证父PID+创建时间并持有父进程句柄；独立watchdog观测父退出和有界启动期限，必要时结束自身，避免Job绑定前死亡留下永久孤儿。该watchdog不代替主控续租，续租仅由当前会话递增Heartbeat在IPC线程验证后更新。
- initialize/execute/destroy/卸载都在专有线程；requestStop可与execute并发，要求插件线程安全且快速返回；违反要求时仍由Supervisor或watchdog结束进程，禁止仍有代码执行时卸载。
- 插件结果写入宿主提供的有界缓冲；结果长度和TaskFinished语义由宿主再校验。示例算法为确定性测试实现，不冒充ONNX/真实算法。
- Camera/Communication/ObjectStorage/ResultOutput各自提供类型接口；本批公共宿主的任务执行入口先接Algorithm，其余kind明确拒绝执行，适配入口按T10/T13/T24/T14逐步接入。SDK保留输出的快速trySubmit与pollReport，不能同步等待外部ACK。

## 验收

加载兼容/错版本/错导出构建身份、参数错误、缺少工厂、路径逃逸/缺依赖、创建初始化执行销毁失败；真实宿主加载DLL正常执行、算法卡死、原生崩溃、停止、父进程消失和同级宿主继续健康；已有三preset回归。

不能以SDK头文件存在宣称所有类型插件已经可生产运行。授权/插件供应链审查和真实设备均不在这次验收范围。
