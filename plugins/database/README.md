# 数据库结果输出插件

本目录提供两个独立的 ResultOutput 插件，默认不构建、不启用：

| 目录 | 插件 ID | 客户端 | 软件验证数据库 |
|---|---|---|---|
| mysql-output | vision.mysql-output | MariaDB Connector/C 3.4.5，动态链接，OpenSSL 后端 | MySQL Community 8.4.4 |
| postgresql-output | vision.postgresql-output | libpq 17.4，动态链接 | PostgreSQL 17.4 |

它们输出已经完成的检测结果，不承担框架内部 SQLite 交付账本、相机采集、
任务调度或数据库管理。GUI 默认不启动它们。Production 仍禁用。
当前为 Windows x64 / MSVC Release 验证范围；不能据此宣称 Linux 或现场生产验收完成。

## 模块边界

```text
检测完成 -> ResultEnvelope -> Dispatcher
                             ├─ worker-host -> MySQL 插件 -> MySQL
                             ├─ worker-host -> PostgreSQL 插件 -> PostgreSQL
                             └─ worker-host -> 其他输出插件

本地 SQLite 意图/租约 -> durable-output-agent -> 同样的插件宿主
```

每个输出实例使用独立宿主。插件内部有一个 IO 线程、一个待处理请求和一个待领取回执。
提交/查询回执不执行数据库 IO；已有请求或未领取回执时返回 Busy。
数据库等待有期限并检查停止标记，主控继续处理其他宿主。
客户端库、DNS、操作系统或驱动仍可能阻塞；最终隔离边界是宿主进程及其监护，
不是“线程能免疫所有卡死”，也不是恶意插件安全沙箱。

`common/model.hpp` 定义参数校验和统一事务步骤；
`common/mysql.hpp`、`common/postgresql.hpp` 封装对应客户端；
两种数据库各自拥有 DLL、manifest、迁移脚本及部署依赖。
没有新增数据库专用主控分支或修改公共插件 ABI。

## 构建

```powershell
cmake --preset windows-ipc -DVISION_BUILD_MYSQL_OUTPUT=ON -DVISION_BUILD_POSTGRESQL_OUTPUT=ON
cmake --build --preset windows-ipc --config Release
ctest --preset windows-ipc -R output.database
```

本机 SDK 默认位置：

- `开发环境/SDK/mariadb`：Connector/C 动态客户端。
- `开发环境/SDK/PostgreSQL/pgsql`：libpq、OpenSSL 和必要运行依赖。
- MySQL 服务端只用于开发测试，不链接进插件，不随插件部署。

可覆盖 `VISION_MARIADB_ROOT`、`VISION_LIBPQ_ROOT`、`VISION_DATABASE_OPENSSL_ROOT`。
库文件必须是匹配架构的动态客户端。不要将 SChannel 版本的 Connector/C
替换到本构建：该版本的非阻塞 TLS 路径不满足本插件要求。
Connector/C 本机固定构建方法见 [依赖构建](../../docs/development/数据库依赖构建.md)。

部署每个插件目录中的 DLL、`*-output.json`、`migrations`、许可证和 notices。
保留动态客户端 DLL 的替换能力。插件加载器通过 manifest 定位插件，不加载源码目录的模板 manifest。
本机同时启用两种插件时，构建工具可能复制额外客户端 DLL；这不表示一个插件依赖另一个插件。

## 连接参数

```json
{
  "host": "127.0.0.1",
  "port": 3306,
  "database": "vision_results",
  "username_env": "VISION_DB_USER",
  "password_env": "VISION_DB_PASSWORD",
  "timeout_ms": 1500,
  "tls": "disable-loopback",
  "ca_file": "",
  "archive_root": ""
}
```

PostgreSQL 默认端口通常为 5432。实际端口必须显式填写。
9 个参数全部必填，不接受额外字段。账号和密码从环境变量读入，
变量名不是账号本身；业务应用负责将变量传入宿主环境。
不把密码写入配方、命令行、错误回执或提交到仓库。

`disable-loopback` 只允许 `127.0.0.1` 且 CA 路径必须为空。
网络连接使用 `verify-full`，提供有效 CA PEM 文件，验证证书链和主机身份。
不提供“只加密不验证证书”模式。CA 文件路径按 UTF-8 输入；
第三方客户端的 Windows 中文路径支持仍需在部署位置复核。
当前配置不支持 IPv6 字面量、客户端证书认证、连接池或复杂连接串。

`timeout_ms` 为每个请求 100～5000ms 的上限，同时受框架 `budget_ns` 限制。
它包括连接、所有 SQL、事务提交和图像元数据读取。
持久 agent 还施加自身 1000ms 请求预算，不因插件配置更长就扩大期限。
每个请求重新连接；初版优先保持生命周期清晰，后续凭吞吐测试决定是否引入连接池。

## 数据库初始化与权限

用管理员在独立数据库中执行对应 `migrations/001-initial.sql`，只执行一次。
插件不自动建库、改表或升级 schema；不兼容版本返回 `DB.SCHEMA_VERSION`。
MySQL 要求 InnoDB/utf8mb4，验证基线为 8.4.4，未验收旧版本和其他兼容产品。
PostgreSQL 使用 `public` schema，必须由受信任的迁移账号拥有，
禁止不可信账号修改表结构或创建影响查询解析的对象。

运行账号仅对版本表授予 SELECT，对结果、缺陷、图像表授予 SELECT/INSERT/UPDATE；
不授予 DROP、ALTER 或管理权限。
MySQL 的幂等插入使用 no-op UPDATE；PostgreSQL 的 FOR UPDATE 也需要相应权限。
迁移权限与运行权限分开。数据库备份、复制、保留策略和服务运维由部署方负责。

| 表 | 内容 |
|---|---|
| vision_schema_version | 当前 schema 版本，仅管理员升级 |
| vision_results | 事件、工位、工件、质量、执行状态、原始结果 JSON 和 SHA256 |
| vision_defects | 检查项、缺陷类别/名称/置信度/框坐标 |
| vision_images | 可选本地图像关联、哈希、通道和描述符 |

序号与纳秒时间使用 DECIMAL/NUMERIC(20,0)，保留完整 uint64 范围。
JSON 保留框架原始结构；实例分割掩码不在本插件内转成独立数据库对象。
值使用绑定参数，包含引号或 SQL 片段的标签仍作为数据写入。
字段、行数和结果 JSON 有界，超限明确失败，不静默截断业务数据。

## 事务与确认语义

一次请求在一个事务内写入结果、缺陷及可选图像引用，COMMIT 返回后才报告 BusinessAcked。
数据库持久性仍受服务端 fsync/复制配置和实际硬件影响，插件不承诺跨灾难的数据绝不丢失。

`event_id` 是主键；同一事件且规范化结果完全相同，重复提交成功且不增加行。
同一事件内容不同则失败；`run_id + inspection_id` 也唯一，防止改换事件 ID
重复写入同一次检测。并发输出使用数据库唯一约束与事务行锁处理竞争。
失败关闭连接回滚未提交事务，不能留下只有结果没有缺陷的半次写入。

网络断开或宿主退出可能发生在数据库提交后、回执到达前。
Failed/Unknown 均不证明数据未写入；复核或补投必须使用相同事件 ID 和相同结果。
这是幂等写入，不宣称端到端 exactly-once。

持久 agent 准入要求输出显式声明：

```json
{"replay_policy": "NonPhysicalIdempotent"}
```

沿用现有 SQLite 账本的租约、次数及过期约束。丢失回执的 Pending 意图在租约到期后
可再次认领；明确 Failed 是终态，当前不会自动变成 Pending。
本插件没有新增无限重连、定时重试或操作员重开失败意图的功能。
不能把此重放配置用于 PLC 物理执行器。

## 图像边界

`archive_root` 为空则只写检测结果和缺陷。设置时必须为已有的本地归档目录；
先由框架 Store 提交完全相同的事件及关联图像，再提交此插件。
每个事件最多读取 4 条图像关联，记录 `.stage` 引用、哈希和描述符。
目录不存在、事件不匹配或元数据损坏时整次事务失败。

数据库不保存大图 BLOB，也不负责上传 RustFS/S3。
本批没有自动把对象存储状态/远端 URL 同步到关系表；
远端可用状态、对象键对账和本地图像清理仍需后续独立设计。
本地绝对路径不一定可被远端消费者访问，不应直接作为公网下载地址。

## 验证

普通 CTest 包含每库无服务场景：非法配置、拒绝连接、静默服务、取消/重载、宿主隔离。
真实数据库验收：

```powershell
cmake --preset windows-ipc -DVISION_TEST_DATABASE_SERVERS=ON
cmake --build --preset windows-ipc --config Release
ctest --preset windows-ipc -R output.database
node tools/build.cjs windows-all
```

真实测试需要本机上述便携数据库 SDK 与 Git OpenSSL
（可用 `VISION_TEST_OPENSSL` 指定），不自动下载依赖。
脚本使用临时目录、loopback 随机端口和专用测试账号，
不安装 Windows 服务，不连接用户已有数据库，结束后停止自建服务。
原生工具对中文路径有限制，测试复制必要 SDK 目录到 ASCII 临时目录。

每库覆盖 17 个真实场景：正常、重复、内容冲突、检测身份冲突、错误版本、
缺表、权限不足、写入回滚、锁超时、图像引用、并发实例、独立宿主、
持久补投、提交后失去确认的租约恢复、有效 TLS、错误 CA、主机名不匹配。
恢复场景构造已提交事务与过期本地租约，不等同于真实网络代理丢回执或断电测试。
通过情况和最终全量证据见验收文档；不能把“存在测试代码”当成“已经通过”。

仍未验收：远程真实网络、数据库集群切换、长时间负载、断电、
生产账户治理、备份恢复、Linux，以及所有现场节拍。

## 许可与发布

Connector/C 动态客户端采用 LGPL 2.1 系列授权；OpenSSL、libpq 及其
附带 DLL 各有许可证。动态链接不等于没有义务，也不自动要求业务代码全部开源。
保留 notices、版权文本、客户端替换能力，并按实际分发版本准备对应源码与构建材料。
见 `dependencies/licenses/mysql` 和 `dependencies/licenses/postgresql`。
MySQL Community 服务端测试包属于独立 GPL 软件，不随插件发布。
当前依赖版本用于验证，不声明为最新安全版本，正式发布需单独进行更新审查。
项目自身 LICENSE 尚未选定，第三方许可不能替代项目授权。
