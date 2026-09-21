# 使用Agent开发自己的上位机

Skill位置：`.agents/skills/vision-framework-dev/`，入口为其中的`SKILL.md`。
版本0.1.0，首版支持本框架Windows源码集成：模拟Qt上位机与ResultOutput插件。
本机已生成可复制包`out/packages/vision-framework-dev-0.1.0.zip`；包内不包含框架源码或SDK，使用时仍需传入完整框架位置。

## 给Agent的使用指令

支持技能调用的环境中可尝试：

```text
使用 $vision-framework-dev，框架根目录是……，在……新建我的模拟检测上位机。
先使用双模拟相机，显示检测结果，完成后执行全部测试。
```

另一个示例：

```text
使用 $vision-framework-dev，为这个框架创建一个新的结果输出插件。
先生成可运行模板，验证正常、拒绝及丢失回执时的隔离行为，再根据我提供的协议实现外部推送。
```

不同Agent的自动发现与技能安装目录可能不同，以其实际支持为准。
如果当前会话没有发现该技能，可明确让Agent读取入口SKILL.md及相关资料；
不要只复制入口而遗漏references、assets和scripts。本次不修改任何全局Agent配置。

## 手工执行脚本

从框架根目录执行，Python要求3.9及以上：

```powershell
python -X utf8 .agents/skills/vision-framework-dev/scripts/inspect_workspace.py --framework-root .
New-Item -ItemType Directory -Force user-projects
python -X utf8 .agents/skills/vision-framework-dev/scripts/create_application.py --framework-root . --destination user-projects/my-station --name my-station
python -X utf8 .agents/skills/vision-framework-dev/scripts/verify_project.py --framework-root . --project user-projects/my-station
```

输出插件则使用create_plugin.py，将destination和name换成自己的新目录及插件名。
目录必须尚不存在，父目录必须存在；脚本拒绝覆盖用户文件，也不允许生成到框架内部src/apps/plugins等目录。
user-projects是示例，业务目录可以由使用者另外选择或独立管理版本。
框架源码接入为`VISION_EXTENSION_DIR`，不是已经发行的安装式SDK。

默认verify执行框架全量回归、业务工程构建及业务测试。
开发定位可显式使用`--scope extension`；该模式不能写成完整框架验收。
日志在业务目录`out/validation`，应用通常位于`out/build/user-project/Release`。
框架已有本地Qt/ORT/native依赖布局仍需准备，脚本不会下载或安装它们。

## 版本、发布与限制

- 兼容清单固定接口文件的SHA256（忽略CRLF/LF差异）。接口变化时拒绝自动生成，需复核模板和测试后更新。
- 首版是单工件、双模拟相机和亮度算法示例，业务UI与流程可以继续开发；不是低代码生产平台。
- 输出模板是有界内存接收器，BusinessAcked不代表持久存储或远端用户已经消费。
- 动态CRT/ABI、平台和运行配置需要匹配；Linux/GPU、真实设备与Production并未因此验收。
- 生成项目包含开发路径依赖，不是可以直接复制到客户机器的发布包。
- 框架及模板发布许可证尚未选定，不能因创建Skill而跳过许可材料。
- 独立新会话Agent试用及提速效果尚未评估；脚本/编译通过不证明所有Agent都能正确使用。

维护者修改公共接口后，应同步Skill、模板和契约清单并重跑验收。
本批实际记录见[VAL-027](acceptance/VAL-20260920-027-Agent开发Skill.md)。
