# RustFS / S3 对象存储插件

插件 ID：`vision.s3-storage`，类型：`ObjectStorage`。
公共加载器和 worker-host 已接入 ObjectStorage 调用，不把上传放进主控。

```json
{
  "root": "D:/inspection/archive",
  "endpoint": "http://127.0.0.1:9000",
  "bucket": "inspection-images",
  "access_env": "VISION_S3_ACCESS",
  "secret_env": "VISION_S3_SECRET"
}
```

复用现有 S3 签名客户端及 Store；bucket 必须已存在。密钥只通过环境变量
传入。请求为结果投递 envelope，不接受任意文件路径上传或删除命令。

处理步骤：

1. 核对 event_id 对应的结果已经提交且内容一致。
2. 查找同一 run / inspection 的本地归档原图，数量 1..4。
3. 加 `.upload-lock`，防止与存储工具同时上传，冲突时报告失败。
4. 持久保存目标绑定，按 `frames/<image_id>/<sha256>` 上传。
5. GET 回读并核对哈希，之后才将本地图像状态改为 Available。
6. 所有关联图像完成后回报 BusinessAcked，供持久分发清算。

这里 BusinessAcked 指存储操作已验证，不是 MES 或 PLC 消费确认。
重复投递对 Available 图像再次核验，不重复 PUT；每张图像最多三次
上传尝试，连续失败转 Failed。宿主被终止时可能留下 Pending，
由后续持久运行恢复，不能把中断当成功。

对象内容是归档原始像素，不是 PNG/JPEG，描述符仍在本地追溯库。
多个上传共用有界任务预算；慢服务会导致 Unknown/宿主终止，而不是
无限等待。本插件不提供远端删除、保留策略、签名下载 URL 或掩码归档。

已具备 agent 配置调用路径，但 Qt 演示默认仍使用文件输出；
没有自动修改其配方或开启后台自动上传。Production 保持禁用。
