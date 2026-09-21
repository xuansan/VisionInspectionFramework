# T15：完整模拟闭环入口

使用已验收Station、DeviceSession、Dispatcher，不在CLI/UI里实现第二套检测逻辑。
新增application/qt/DemoRunner持有运行组件；应用事件循环真实pulse维护控制租约。
单工位双相机配方作为独立JSON随程序部署，计算规范JSON的SHA256用于结果快照身份。
当前仅启动时读取；运行中切换/原子保存/回退仍属T16。

CLI提供normal/ng/algorithm-crash/output-hang/camera-missing情景，输出标准
ResultEnvelope和交付状态，进程/票据/租约清算后退出；失败情景也必须验证期望终态。
Qt保留原五种领域规则演示测试，同时新增真实多进程页、启动/暂停/继续/停止、
模式/计数/进程epoch/业务状态/PLC ACK/每路输出/最新结果。
旧合成界面必须清楚标记，删除“多进程未实现”的过时说明。

主控不执行DLL、同步外部输出或图像计算。模拟PLC到位信号由Runner驱动，
检测结果只提交一次到各输出；Unknown不能转成物理OK。非物理输出失败不改质量。
UI显示Demo/Volatile，Production禁用；独立HTTP前端、RustFS、ONNX不在本阶段。

测试：全部旧、新内容；CLI逐场景+GUI实际按钮驱动正常/算法崩溃/输出挂死；
关闭窗口及停止后无工作进程泄漏。无SDK/GPU/对象存储环境仍能运行。
