# 消息队列结果输出

本目录按协议组织插件，每个插件在独立 worker-host 进程运行。
共享的是结果契约、异步输出工具和测试工具，不是一个包含全部协议的 DLL。

| 目录 | 插件 ID | SDK | 默认构建 |
|---|---|---|---|
| mqtt | vision.mqtt-output | Paho C 1.3.15 | 关闭 |
| kafka | vision.kafka-output | librdkafka 2.8.0 | 关闭 |
| rabbitmq | vision.rabbitmq-output | rabbitmq-c 0.15.0 | 关闭 |
| nats-jetstream | vision.nats-output | nats.c 3.9.1 | 关闭 |

本批 Windows 优先。Production 仍禁用，GUI 默认不启用这些输出。
新插件只连接 `127.0.0.1`，不提供远端 TLS/SASL/证书配置。
MQTT 原有 ID、参数和交付语义保持不变，见 mqtt/README.md。

## 构建

SDK 位于 `开发环境/SDK/{librdkafka,rabbitmq-c,nats-c}`，路径也可通过
VISION_RDKAFKA_ROOT、VISION_RABBITMQ_ROOT、VISION_NATS_ROOT 指定。
库为本批实际编译的 MSVC x64 Release 静态库，不依赖用户全局 PATH。

```powershell
cmake --preset windows-ipc -DVISION_BUILD_KAFKA_OUTPUT=ON -DVISION_BUILD_RABBITMQ_OUTPUT=ON -DVISION_BUILD_NATS_OUTPUT=ON
cmake --build --preset windows-ipc --config Release
ctest --preset windows-ipc -R 'output.(kafka|rabbitmq|nats)'
```

启用 NATS 的测试时，`VISION_TEST_NATS_SERVER` 必须指向原生 nats-server.exe，
可用 `-DVISION_TEST_NATS_SERVER=...` 覆盖本机默认位置。测试使用 JetStream
文件存储、临时目录与临时端口，结束后回收自建服务器。

客户端静态链接意味着再分发 DLL 也包含第三方代码。保留构建输出的
第三方 notices 目录。版本、来源和许可证见 dependencies/licenses。
本项目自身许可证选择仍待完成，不能由客户端许可证推导整个项目的授权。

## 发送与业务确认

发送框架原有 JSON envelope：`event`、`output_instance_id`、`attempt`、
`budget_ns`。包含结果和图像引用，不在 MQ 中发送完整原图。
成功必须同时满足协议服务确认和下面的业务回执：

```json
{"event_id":"event-1","output_instance_id":"mq","attempt":"1","state":"BusinessAcked"}
```

四个字段必须精确匹配，禁止多余字段、重复键、畸形 JSON 和超过 4096
字节的回执。服务收到消息、落盘或发出 publisher confirm 都不等于业务成功。
超时返回 Failed；宿主被终止等情况由分发器报告 Unknown，不能伪造成功。
Failed/Unknown 不证明消息从未到达，业务消费者必须按 event_id 去重。

自动续传沿用 durable-output-agent。仅在输出配置显式声明
`replay_policy: "NonPhysicalIdempotent"` 时准入。应用消费者应在幂等业务
提交后确认当前 attempt，不能将此通道用于可自动重放的物理执行器动作。
持久化的是框架结果/交付状态；本批并未实现通用 MQ 消费端或 MES。

## Kafka 参数

```json
{"port":9092,"timeout_ms":800,"topic":"results","ack_topic":"receipts","ack_partition":0,"partition_key":"station"}
```

结果 topic 和回执 topic 预先创建且必须不同；不自动创建。
结果使用 partition_key，broker acks=all、生产者幂等启用、库重试最多一次。
这不构成跨进程重启的 exactly-once 保证。每个请求建立连接并在发送前读取
回执分区高水位，随后只读新回执；旧回执不能确认新尝试。
消费端不自动提交 offset，也不使用 consumer group。

业务消费者应将回执写入配置的 ack_partition。不同输出实例应分配专属
回执 topic/partition，不能共享一个混杂回执分区。收到不匹配回执会失败，
不会将它误认成当前业务确认。当前最大分区编号 1023。
首次建立连接包含在 100～800ms 期限内；这不是远端集群的通用时限策略。

## RabbitMQ 参数

```json
{"port":5672,"timeout_ms":800,"exchange":"","routing_key":"results","vhost":"/","username_env":"VISION_MQ_USER","password_env":"VISION_MQ_PASSWORD"}
```

账号密码从指定环境变量读取，不写在配方中；环境变量名不等于账号本身。
预先创建 exchange/业务队列/binding，默认 exchange 可留空。
使用 mandatory 发布、publisher confirm 和持久消息 delivery_mode=2。
持久消息仍需要持久队列及恰当的 broker 配置；插件不会替用户创建业务队列。
无法路由、拒绝或断线均不成功。

每次请求声明服务器命名的 exclusive/auto-delete 私有回执队列，
将 reply_to 和 event_id/message_id 发给消费者，消费者向 reply_to 回执。
回执队列在连接关闭后删除；超时后的业务提交不能靠旧队列恢复。
客户端 RPC/接收/握手有期限，socket 发送和库销毁仍以宿主进程硬停止
作为最后边界，不能声称 DLL 本身可强制中断所有系统调用。

## NATS JetStream 参数

```json
{"port":4222,"timeout_ms":800,"subject":"results","ack_subject":"receipts","stream":"VISION"}
```

必须预先创建包含 subject 的 stream。插件验证 JetStream 发布确认的
stream 和 sequence，普通 Core NATS 发布不替代此确认。
业务消费者读取 `Vision-Reply-Subject` header，向该 subject 发送回执。
各实例应使用专属 ack_subject。结果和回执 subject 必须不同。

插件不使用跨 attempt 的 broker MsgId 去重，确保新尝试可再次到达业务消费者。
业务消费者负责 event_id 幂等。回执使用当前连接的同步订阅，未实现持久回执
consumer；消费者的可靠订阅和业务落库由外部系统负责。

## 资源与故障边界

一个插件实例只有一个待处理请求及一个报告；未消费报告时继续提交返回 Busy。
输入最多 32768 字节；配置名称最多 128 字符，限字母数字及点、横线、下划线。
timeout_ms 限 100～800。Kafka 客户端生产队列限制一个消息/64KiB，
消费预取16KiB、fetch64KiB、接收上限1MiB；NATS 回执待处理队列4条/16KiB。
这些限制不等于整个协议库精确内存配额，也不构成对恶意 broker 的沙箱。

取消会设置停止标志，读循环分段检查；独立宿主继续承担进程超时和回收。
业务线程、UI 和健康输出不等待消息队列网络 IO。停机/重启/续传仍须按
框架既有交付契约处理，不把线程异常隔离误认为进程隔离。

## 验证范围

Kafka 使用 librdkafka 官方 mock cluster 做真实 TCP 协议和故障测试；
RabbitMQ 使用本仓库有界 AMQP 故障对端，不能称为实际 RabbitMQ broker。
NATS 使用真实 nats-server 2.10.24 JetStream 文件存储及模拟业务消费者。
实际 Kafka/RabbitMQ broker 的状态以本批验收文档为准。
本机另执行真实 Kafka 3.9.1 联调 10 项和 RabbitMQ 3.13.7 联调
12 项，均通过；业务消费者为测试程序，未测试 broker 断电。
详细证据及首次全量失败记录见
[VAL-030](../../docs/acceptance/VAL-20261010-030-消息队列插件.md)。
各协议覆盖重复发送、业务回执身份/格式/容量、离线、无响应、取消、
独立宿主与健康输出共存、持久 agent 准入和确认。它们不替代现场节拍、
长稳、真实 MES、故障集群、认证或断电验收。
