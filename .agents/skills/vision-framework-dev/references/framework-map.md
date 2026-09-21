# 框架地图

以下路径均相对用户传入的框架根目录，不是Skill安装目录。

| 改什么 | 先看哪里 |
|---|---|
| 数据身份、标准结果、帧描述符 | src/contracts/include/vision/contracts/ |
| 工件检查汇总和OK/NG/Unknown | src/inspection/ |
| 业务工位和配方流程 | src/application/qt/station.cpp、recipe.cpp、demo_runner.cpp |
| 独立输出调度 | src/application/qt/dispatcher.cpp及对应include目录 |
| 监督、超时、租约与资源预算 | src/runtime/及src/runtime/qt/ |
| 共享图像映射 | src/frame_transport/ |
| 相机采集通用流程 | src/capture/及src/capture/qt/ |
| 算法调用、ONNX推理 | src/algorithm/、src/inference/ |
| 插件接口和加载约束 | src/plugin_sdk/include/vision/plugin_sdk/api.hpp、src/plugin_runtime/ |
| 持久结果、图像和S3 | src/storage/、docs/protocols/持久存储与输出-v1.md |
| 现有程序 | apps/workstation、worker-host、frame-archive、storage-tool |
| 具体适配 | plugins/各插件目录；output-common只是复用代码 |
| 数据格式 | schemas/、src/serialization/ |
| 测试与历史失败 | tests/、docs/acceptance/ |

选择规则：阈值修改先看配方；新判定看业务规则；新品牌SDK看插件；新外部输出看ResultOutput。
只有证明现有接口无法表达需求时，才提出公共框架变更及兼容性测试。
不要把apps里的main.cpp当可链接库，也不要让业务工程通过相对路径include框架内部.cpp。
源码接入点在cmake/Extensions.cmake；还没有可对外承诺的install/export SDK包。
