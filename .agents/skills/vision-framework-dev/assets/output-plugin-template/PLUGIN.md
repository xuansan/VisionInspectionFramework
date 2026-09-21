# @PROJECT_NAME@

这是ResultOutput插件模板，ID为user.@PROJECT_NAME@，通过独立worker-host运行。
当前行为是有界内存接收；BusinessAcked只代表本接收器处理，不代表落盘或远端消费。
mode=reject和drop-ack用于反例，默认normal。

实现位于output.cpp，声明位于manifest.json，进程测试位于smoke.cpp。
使用vision-framework-dev的verify_project.py构建及测试，显式提供框架和本项目路径。
默认--scope all保留框架全量回归，开发定位才用--scope extension。
构建输出为out/build/user-project/Release/@PROJECT_NAME@.dll和@PROJECT_NAME@.json。

增加真实网络/文件IO时保持调用有界并使用独立IO执行单元；请求取消和进程退出必须有测试。
插件导出与manifest成套维护；当前固定Windows x64/MSVC v143动态CRT，Release已支持。
NOASSERTION只表示尚未确定发布许可，不是开源授权声明。
