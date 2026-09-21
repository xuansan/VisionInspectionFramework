# @PROJECT_NAME@

这是使用框架源码构建的Qt模拟上位机，包含开始/停止和结果展示。
双模拟相机、基础亮度算法、模拟PLC与内存输出；不是生产或持久化模板。

业务入口main.cpp；配方recipe.json；依赖在CMakeLists.txt。
使用创建本项目的vision-framework-dev Skill运行verify_project.py，显式提供框架和本项目路径。
开发定位可用--scope extension；正式验收默认--scope all。
程序在out/build/user-project/Release/@PROJECT_NAME@.exe；测试与构建证据在out/validation。
框架开发插件路径在配置时绑定，这不是可独立部署的发行包。

新增业务流程时保留原有进程、租约和截止时间约束；不要把设备IO写入窗口事件回调。
Windows x64/MSVC v143/Release/Qt6.8.3为当前验证目标，现场设备、Linux/GPU另验。
本项目及框架对外发布许可证尚需维护者确认，生成模板不授予额外第三方使用权。
