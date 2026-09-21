# 新增结果输出插件

先确认现有file-output/http-output是否通过配置即可满足；不重复实现已有协议。
确需新插件时：

```text
python <skill>/scripts/create_plugin.py --framework-root <framework> --destination <new-project> --name my-output
python <skill>/scripts/verify_project.py --framework-root <framework> --project <new-project> --scope extension
```

生成output.cpp、manifest.json、CMakeLists.txt、smoke.cpp和PLUGIN.md。
插件ID为user.<name>，DLL导出describe/create/destroy，与manifest的kind、版本、ABI、平台、线程模型保持一致。
当前固定MSVC v143 x64动态CRT，不是跨编译器ABI；模板只验证Release，Debug需匹配manifest后另测。
不支持非空依赖图，不能随意填dependencies期待宿主自动解析。

模板实现ResultOutput的有界内存接收：最多一个待回执，正常BusinessAcked只代表本接收器处理。
reject用于拒绝反例，drop-ack用于超时不确定反例；都不是实际外部系统推送。
原始event_id、output_instance_id及attempt须原样关联；不能生成新身份冒充原请求回执。

扩展真实IO时使用有界IO线程与独立宿主，关闭必须能请求取消，不能把文件/网络等待放进主控。
可参考plugins/output-common/async.hpp的实现，但这是内部示例，不在模板中直接依赖这个私有头。
需要持久交付时接入现有存储意图流程；默认Dispatcher是Demo/Volatile，不能改文字伪装持久。

smoke通过Dispatcher及真实worker-host加载生成的DLL，包含正常、拒绝、丢ACK；
健康输出与故障输出并行，检查健康一路先完成及停止后子进程退出。插件修改后保留这些边界测试。
项目LICENSE与依赖许可尚未定，NOASSERTION不是发布授权；对外分发前单独完成许可材料。
