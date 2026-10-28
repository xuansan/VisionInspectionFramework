# Webhook 输出

插件 ID：`vision.webhook-output`，类型：`ResultOutput`。
由独立 worker-host 执行，DLL 使用 libcurl，不依赖 Qt。
这是主动发送结果，与已有只读 HTTP REST/SSE 插件不同。

配置示例：

```json
{
  "url": "https://mes.example.com/inspection/results",
  "token_env": "VISION_WEBHOOK_TOKEN",
  "timeout_ms": 500
}
```

令牌通过环境变量提供，为 32..128 个可打印 ASCII 字符，不写入配方。
只允许 HTTPS 或显式 `http://127.0.0.1` 测试地址；禁止 URL 内嵌凭证、
查询参数、fragment、代理、重定向及关闭 TLS 证书校验。
整体请求期限为 100..5000 毫秒，实际还受上层任务预算约束。

POST 内容为框架原始投递 envelope；携带 Bearer 令牌和
`Idempotency-Key: <event_id>`。接收方必须按 event_id 去重，核对同一事件
内容一致，重复投递返回一致的业务确认。插件不自行重试。

只有 HTTP 2xx 且返回以下完整 JSON 才报告业务成功：

```json
{"event_id":"与请求相同的事件标识","state":"BusinessAcked"}
```

回执最多 4096 字节，禁止重复键和额外字段。空 200/202、错标识、
失败状态、超时、断连均不能确认成功。远端回执是其处理声明，
不等于框架能独立证明对方数据库已提交。

持久 agent 配置中的该输出必须显式添加：

```json
"replay_policy": "NonPhysicalIdempotent"
```

该声明意味着应用负责人已核实接收端是非物理、幂等的结果接收服务，
不是自动检测或强制保证。不得用它为执行器、剔除、运动控制接口开启重放。
重放仍可能多次发送；不能宣称跨网络恰好一次。
