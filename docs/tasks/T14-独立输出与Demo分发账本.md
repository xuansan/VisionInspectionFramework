# T14：独立输出与Demo分发账本

依赖T12、SDK与ADR-012。每输出实例一个公共宿主、一个有界队列，
以DeliveryTask/DeliveryFinished核对event/output/attempt/worker epoch。
模拟接收插件提供ACK/失败/丢ACK/崩溃/挂死，所有插件代码在独立进程执行。

Demo outbox仅内存，单事务式插入不可变结果及各路intent，再分发；
状态包含Pending、Accepted、BusinessAcked、Failed、Expired、Unknown。
不得把内存接纳标为DurablyQueued；UI明确Volatile。Production入口禁用。
账本提供有界快照导出/恢复用于恢复算法测试；只有调用方已经持久保存的快照才
可能被恢复，运行端不承诺断电保存。T24负责真实本地数据库事务。

恢复测试分三个位置：intent已记录但未发；接收已执行但ACK未记录；
ACK已记录。未确认非物理事件允许同键重发，attempt增加，可能重复交付；
ACK已记录不再发；过期不发。PLC不得接入此重发器。
这验证F26的状态恢复规则，不能替代T24真实磁盘断电/损坏/事务故障验收。

每路容量8，账本最多128事件，已终态仍保留到显式清理/会话结束；
队列满该通道明确Failed，不静默丢失，也不能拖住健康通道。
每路独立处理结果与截止期限；旧/重复ACK不改变新attempt。
全量回归必须覆盖所有既有模块。
