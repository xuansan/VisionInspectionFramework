# nats.c

固定版本 3.9.1，来源 https://github.com/nats-io/nats.c，
源码归档 https://codeload.github.com/nats-io/nats.c/tar.gz/refs/tags/v3.9.1。
本地 SHA256：56836bb30a2da93eaa6df0dfa27e796e6be0933b5b3d4d83b5c76d3b80304290。
这是本地完整性记录，不冒充上游签名验证。

MSVC x64 Release 静态构建，TLS/NATS Streaming/示例关闭。
JetStream 使用成熟客户端 API。原始 Apache-2.0 LICENSE 保留。
本批仅测试普通本机连接，不宣称 NKey/JWT/TLS 认证已验收。

集成测试单独使用 nats-server 2.10.24 官方 Windows amd64 发行 zip：
https://github.com/nats-io/nats-server/releases/tag/v2.10.24
本地 SHA256：bf94c9a9f1685147fd95f6c62f26d16fb72dc8c8c592e2d8c9115e2491c508c3。
服务器并不编译到插件中，也不随框架源码发布二进制。
