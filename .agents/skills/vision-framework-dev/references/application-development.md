# 创建自己的上位机

先读取模块地图和最新能力边界。首版模板有Qt窗口、开始/停止、JSON结果展示；
复用DemoRunner串联双模拟相机、基础亮度算法、模拟PLC和演示输出，分发为Volatile。
它不是已经完成的可配置生产工作站；正式业务编排可以基于Station、Dispatcher等已有接口逐步实现。

在用户选择的业务目录父目录已存在时执行：

```text
python <skill>/scripts/create_application.py --framework-root <framework> --destination <new-project> --name my-station
python <skill>/scripts/verify_project.py --framework-root <framework> --project <new-project> --scope extension
```

路径含空格时按所用shell正确引用，不能拼接未经校验的shell命令。
生成器只写新目录，不修改框架src/apps/plugins。已有目录需人工核对差异，不删除用户项目重建。

生成内容：main.cpp、recipe.json、CMakeLists.txt、vision-project.json和APPLICATION.md。
业务代码与配置在这里修改；CMake以框架为源码根，通过VISION_EXTENSION_DIR加载业务目标。
verify日志位于业务目录out/validation；程序位于out/build/user-project/Release/<name>.exe。
目前包含开发时插件绝对路径，不能直接复制这个exe到另一台机器并宣称可部署。

先用normal/ng/stop/restart烟雾测试得到三件结果和零残留租约。然后按需求修改配方、界面或编排。
新输出可独立测试后配置Dispatcher；DemoRunner的演示输出选择是固定的，不能假造运行参数。
需要持久模拟时参照DurableDemoConfig和apps/workstation/pipeline.cpp的真实接口，记录启用的归档/存储能力。

阶段交付执行默认--scope all，既验证框架全量，也构建业务项目并运行user.<name>.*测试。
若增加业务路径，补对应行为测试；不能把模板四项烟雾通过视为新业务已经验收。
