# 本地 IPC 协议 1.0

日期：2026-09-19。关联主计划8.1、T05、VAL-20260919-008。实现为 `vision_ipc` 和 `vision_ipc_qt`；Windows Release 已验证，Linux 未验证。

## 层次与入口

- `message.hpp/message.cpp`：17类消息严格编解码（原15类加capture-v1的2类）；公共接口不暴露 Qt/JSON 类型。
- `Channel`：单事件线程拥有，组合封包、双向握手、发送/接收队列、请求状态与期限。调用者传本地单调时间。
- `LocalConnection`：拥有 QLocalSocket，全部方法/回调在其 QObject 线程执行；没有阻塞 wait API。只依赖 Qt Core/Network，不依赖 Widgets。
- `vision-ipc-probe`：独立诊断入口，启动真实测试子进程使用正式 Channel/LocalConnection；不是生产监护器。
- CLI `--validate-message <file>`：校验协议格式，不能替代运行会话认证。

编解码器依赖固定 nlohmann/json。ResultEvent 使用既有最终结果编解码器，因此 `vision_ipc` 私有依赖 `vision_serialization`；runtime/inspection/application 不反向依赖 IPC。CMake 反例测试拒绝协议核心链接 Qt。

## 公共头

传输格式仍为4字节大端无符号长度 + UTF-8 JSON，不含结束符。

| 字段 | 规则 |
|---|---|
| protocol_major/minor | JSON整数，严格1/0；未协商的新次版本也拒绝 |
| message_type | 下表17种之一 |
| request_id | 1～128 ASCII ID；请求与回复相同，通知也有ID |
| run_id/worker_id/worker_epoch | 双向都表示绑定的工作宿主会话，不是发送进程角色；epoch为正uint64字符串 |
| sequence | 每方向从Hello的1开始严格连续；优先出队时才编号；不允许重复/缺口/回绕 |
| correlation | null或完整 run/worker/epoch/inspection/check/task/attempt；前三项必须与头一致 |
| payload | 严格对象，各消息不接受未声明字段 |

所有64位值均为规范十进制字符串。最大协议JSON为1MiB、深度32，实际连接可选更小max_payload；重复键、非法UTF-8、数字溢出、未知字段拒绝。Schema见 [ipc-v1.schema.json](../../schemas/ipc-v1.schema.json)，例子见 `examples/ipc/`；Schema不能独立验证全部跨字段语义。

## 消息与载荷

| 消息 | payload | correlation / 请求行为 |
|---|---|---|
| Hello | token、required_capabilities | null；凭据32～128字节，当前支持capture-v1，新Channel声明该必需能力，未知能力拒绝 |
| Configure | recipe_hash、config_revision | null；等待Ready，回复必须匹配hash/revision |
| Ready | recipe_hash、config_revision | null；仅结束匹配Configure，不表示产线就绪 |
| SubmitTask | operation、input_ref、budget_ns | 必需；operation=Inspect/Capture/Persist；等待TaskFinished |
| TaskAccepted | 空对象 | 必需；只表示接纳，不结束请求 |
| TaskProgress | progress_sequence | 必需；只表示报告，不延长权威请求期限 |
| TaskFinished | execution_state、quality、result_ref、error_code | 必需；Succeeded需OK/NG和结果引用、error=null；失败需Unknown和错误码、result_ref=null |
| CaptureTask | slot_count、slot_bytes、write_frame、budget_ns | 必需；帧写许可必须匹配会话，等待CaptureFinished |
| CaptureFinished | execution_state、frame、error_code | 必需；成功frame等于原请求write_frame；失败frame=null；无quality字段 |
| Cancel | reason | 必需；通知，不等于已停止或可回收资源 |
| Heartbeat | progress_sequence | null；不是主控健康租约，不替代任务期限 |
| Fault | code、category、message、retryability | 可选任务关联；错误通知 |
| Drain | budget_ns | null；有限排空请求通知，宿主语义待T06/T07 |
| Stop | reason | null；停止意图通知，不承诺设备已停 |
| LeaseRelease | pool_id、lease_id、slot_id、slot_generation | null；仅结构验证，权限/有效租约待T08 |
| ResultEvent | event | null；嵌入核心最终事件v1，run必须与头一致 |
| DeliveryReport | event_id、output_instance_id、state、attempt、error_code | null；失败/到期/未知必须有错误码，其余为null |

这是当前可运行的最小控制契约。Configure传内容寻址的配方引用，不传未校验任意参数JSON；input_ref/result_ref是已登记资产/结果标识，不传指针、整图或内存布局。解析成功不证明引用存在、调用已授权或设备已执行，后续T06～T09/T12/T14/T24仍需履约。载荷扩展必须明确协议升级。

T10b扩展依据ADR-010，完整语义见[正式采集工作流](正式采集工作流-v1.md)。原SubmitTask的Capture枚举保留兼容结构，正式采集使用CaptureTask，不通过质量结果消息伪造“拍照成功=工件OK”。头版本仍1.0，必须能力capture-v1阻止旧实现误接入；不宣称不同批次二进制可混用。

## 握手与连接所有权

客户端先发送Hello，服务端验证run/worker/epoch/token/版本/序号/能力及期限后返回自己的Hello；客户端验证后连接可提交业务。业务API禁止自行发送Hello或通过普通send伪造回复。

每次宿主启动必须生成新的不可预测凭据和端点；启动器负责安全传递。本批探针通过子进程环境传递，读取后清除子进程环境中的凭据，不打印它。服务端使用UserAccessOption和单连接监听，建立连接后关闭监听；不删除其他程序端点。

LocalConnection接收调用者提供的socket，接管其所有权；端点命名、监听ACL和允许哪个进程连接属于启动器职责。当前探针证明本账户条件下功能，不能推导跨账户ACL已验证，更不是抵御同账户恶意DLL的安全沙箱。

回调必须短且不阻塞，不直接删除连接对象，使用deleteLater。回调抛异常导致连接关闭并记录CallbackFailure，不转换成成功。关闭回调自身抛异常在已关闭边界内截断，不能重新打开连接。

## 有界发送、接收与期限

默认Channel：单包64KiB，普通发送32条/256KiB，控制发送8条/16KiB，接收32条/256KiB，待请求32条，每连接累计业务请求4096次。累计请求达到上限时业务请求返回Full，停止/心跳仍可发送；上层应排空并建立新会话。请求ID不复用。

控制类别为Hello/Heartbeat/Stop/Cancel/Fault/Drain，优先于尚未出队的普通消息。已发送部分帧必须发完，不能在帧中间插入控制消息，因此控制消息优先不意味着零延迟。

Qt默认read buffer=64KiB、write buffer=64KiB，单次读/写块16KiB，每轮事件处理最多64KiB。`bytesToWrite`达到上限后不继续写入；额外只允许一条活动帧。总发送有效载荷上界：

```text
normal_bytes + control_bytes + (max_payload+4) + write_buffer
```

接收端同时约束socket读缓存、当前帧、待交付消息；Channel每完成一个帧即验证，不先构建无界消息列表。JSON临时对象、容器容量和分配器开销不在有效载荷公式中，不等于进程RSS上限。

默认握手2秒、半包1秒、请求2秒、写停滞2秒。半包期限从首字节计算，后续零星字节不续期；请求期限从本地接纳计算，排队时间计入。Qt按10ms定时检查，事件循环必须可推进；不承诺实时OS精度。

## 请求终态与断线

- Configure只有匹配Ready结束；SubmitTask只有匹配TaskFinished结束。task correlation必须完整一致。
- 未到发送阶段即超时的请求从出队时跳过；已经进入活动帧的请求不能撤回字节，仍可能被对端执行。
- 请求超时生成Timeout，后续回复不交付业务，只报LateResponse。未知方向/编号的回复关闭连接，合法已使用编号的重复/迟到回复记录诊断。
- 断线待请求生成DisconnectedUnknown。此状态不能解释为“对端未执行”，不自动重发。
- 2026-09-19修补：匹配的Ready/TaskFinished到达后，直到pop交付仍占有pending；期间不再请求超时，队列满或交付前断线按Unknown结算。超时诊断入队成功后才移除pending，诊断溢出关闭仍保留逐请求结算。
- 接收、诊断、发送或请求预算均有限；诊断未被消费且超过上限时关闭连接，不能无限记录错误。
- 重连必须创建新Channel/LocalConnection；本批不自动重启宿主，也不恢复生产。

## 验证命令

```powershell
node tools/build.cjs windows-core
node tools/build.cjs windows-gui
node tools/build.cjs windows-ipc
.\out\build\windows-ipc\apps\ipc-probe\Release\vision-ipc-probe.exe roundtrip
.\out\build\windows-ipc\apps\ipc-probe\Release\vision-ipc-probe.exe slow-reader
python tools/generate_ipc_schema.py --check
python tools/check_ipc_schema.py --cli out/build/windows-core/apps/vision-cli/Release/vision-cli.exe
```

正式F10的控制封包/本地通道范围由本批验证；故障宿主强制终止、退避恢复、主控健康租约与唯一工位控制者留给T06及A03/A07。

后续实现：T06/T07软件基础已在[VAL-009](../acceptance/VAL-20260919-009-监护与调度验收.md)验收，见[ADR-007](../decisions/ADR-007-监护健康与调度边界.md)。ChannelSnapshot新增rotation_required；ProcessHost在应用空闲、pending和发送全部排空后停止宿主并以新epoch重建，任务不可跨会话自动重放。生产设备的健康和控制权仍由T13/T23完成。
