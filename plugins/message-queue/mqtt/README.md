# MQTT 输出插件

可选构建：VISION_BUILD_MQTT_OUTPUT=ON，VISION_PAHO_ROOT 指向 Paho C 静态 SDK。
目前只允许 tcp://127.0.0.1:port，使用本机 broker；远端 TLS、认证和正式
broker 压力验收未完成，不应向公网暴露测试 broker。

依赖 Eclipse Paho C 1.3.15，tag 的实际 commit 为
62782406bcf511ee8e22bc34e826f8c6b8254c7c。
来源为 https://github.com/eclipse-paho/paho.mqtt.c ，未修改上游源码。
上游双许可 EPL-2.0 或 EDL-1.0；本集成选择 EDL-1.0 路径，
二进制必须携带 EDL、版权和 notices。框架自身许可证仍待选定。

构建上游 SDK：

```powershell
cmake -S <paho-source> -B <build> -G "Visual Studio 17 2022" -A x64 `
  -DPAHO_BUILD_SHARED=OFF -DPAHO_BUILD_STATIC=ON -DPAHO_ENABLE_TESTING=OFF `
  -DPAHO_ENABLE_CPACK=OFF -DCMAKE_INSTALL_PREFIX=<sdk>
cmake --build <build> --config Release --target install
```

配置 endpoint、publish_topic、ack_topic、client_id、timeout_ms。
timeout_ms 为一次完整连接、订阅、发布、业务回执的预算，范围 100..800。
client_id 和 ack_topic 必须在运行的输出实例之间唯一；不得共用回执主题。
发布 QoS1、非 retained 的原始 envelope；接收端按照 event_id 去重，
收到重复事件也应发布关联当前 output_instance_id / attempt 的回执。

业务回执为非 retained JSON，只有以下四个字段：
event_id、output_instance_id、attempt、state，其中前三者与请求完全一致，
state 为 BusinessAcked。仅 PUBACK 或仅业务回执均不足以报告成功。
连接失败、拒绝、错回执、超时均报告 Failed；宿主超时或退出仍为 Unknown。
重试由应用的持久交付管理负责，插件不无限重连。

持久 agent 输出配置必须显式声明 replay_policy 为 NonPhysicalIdempotent。
这是使用方对接收端非物理、幂等行为的确认，不是自动校验保证；
禁止借此对执行器动作启用自动重放。重复投递可能发生，不保证恰好一次。

生产限制：Paho 在回调之前解析收到的报文，4096 字节回执限制不能代替
broker 的 maximum-packet-size 限制；部署需配置 broker 报文/连接限制，
并补齐可信 broker、TLS、认证、实服务与恶意大包的验收。
