# 插件SDK v1：实现要求

依据ADR-008；头文件`src/plugin_sdk/include/vision/plugin_sdk/api.hpp`，加载器`src/plugin_runtime`，公共宿主`apps/worker-host`。

## 实现与构建

- 实现对应类型接口，方法为noexcept，内部捕获可恢复C++异常并转换Status。
- 导出vision_plugin_describe/create/destroy，使用VISION_PLUGIN_EXPORT和VISION_PLUGIN_CALL。describe填写全字段，固定字符数组必须NUL结尾。
- compiler_abi采用msvc-v143-md，要求与本项目固定工具链和动态CRT一致；不能据此声称支持其他编译器或任意MSVC版本。
- 工厂分配，destroy释放；调用方不能delete插件。测试示例见`tests/plugins/test_plugin.cpp`，仅为故障测试，不是生产算法或正式开源授权模板。
- 一个manifest目录一个入口DLL；manifest格式沿用T02，实际build_id等字段必须与导出一致。dependencies非空当前拒绝加载。

## 线程和数据寿命

initialize/execute/destroy/卸载由同一专用线程串行执行。request_stop是唯一可能并发的方法，必须线程安全且快速返回；不得在它内部等执行线程退出。

Bytes只在本次调用有效；不能持有输入指针跨调用。Buffer由宿主分配、拥有、回收，插件只能写capacity范围并设置size，不能替换data/capacity。execute最多64KiB输入/输出；JSON UTF8输出须满足IPC TaskFinished语义。

Camera已有模拟DLL、加载器camera_*和公共宿主CaptureTask入口，见[模拟采集v1](模拟采集-v1.md)与[正式采集工作流](正式采集工作流-v1.md)。Communication/ObjectStorage/ResultOutput仅有类型接口。尤其ResultOutput::try_submit不能同步等HTTP/PLC/MQ远端ACK；Busy必须快速返回，poll_report独立报告投递状态。

## 生命周期与失败

宿主验证父身份→建立受认证IPC→收到Configure→加载DLL→核对导出→create→initialize→Ready→执行→停止→destroy→卸载。

Ready是宿主初始化完成，不等同工位生产就绪。控制租约未建立时拒绝SubmitTask。执行Failed/Cancelled输出Unknown；畸形结果关闭连接，应用按原任务期限/失败规则处理。

request_stop不是“已停止”证明；只有TaskFinished或对应进程退出可以归还相关执行资源。destroy失败则保留DLL至进程退出，防止仍在运行的代码被卸载。

T10a加载器关闭Camera时先调用camera.close，再destroy；第一次close/destroy失败后，再次Library::close必须保留首次结果，不返回伪成功。Camera轮询方法本身已经结束且close明确停止访问时，测试协调方才归还对应写许可；正式跨进程确认由T10b落实。

Algorithm接入SubmitTask，Camera接入CaptureTask并在camera_close确认访问结束后发送CaptureFinished；其他kind在对应业务任务接入前明确拒绝，不返回假成功。
