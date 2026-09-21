# VAL-20260920-027：Agent开发Skill首版

关联任务：[S01](../tasks/S01-Agent开发Skill.md)。用户批准创建可复用技能，首版范围为模拟上位机与结果输出插件。

## 交付范围

- 仓库内`.agents/skills/vision-framework-dev`，入口、六份工作参考、兼容清单、生成模板及辅助脚本。
- 可选VISION_EXTENSION_DIR源码接入，不修改默认应用/插件选择，不在业务生成时改写框架核心。
- Qt应用模板具备真实开始/停止；正常/NG/中途停止/再次启动四项测试。
- ResultOutput模板使用固定ABI和独立宿主，测试正常、拒绝和丢ACK，核对健康一路不受阻。
- 版本/接口不匹配、重复目标目录、无效名称、框架内部目录等均明确拒绝。

## 验证过程

- Skill创建器默认GBK读取中文入口校验失败；使用python -X utf8验证通过。
- 初始化器写出的中文UI元数据已统一UTF-8，另经YAML解析验证。
- 五项Python行为测试通过；已加入框架CTest回归。
- 实际在`out/skill-validation/business app`和`business output`独立目录生成项目。
- 应用首次验证：`business app/out/validation/20260919T171400646169Z`，配置、编译、正常/NG两项通过。
- 应用补充停止/重启后：`business app/out/validation/20260919T172226344996Z`，四项全部通过。
- 插件首次：`business output/out/validation/20260919T171700158752Z`，三项全部通过，包括丢ACK时健康一路先完成。
- 正式默认--scope all：`business output/out/validation/20260919T172345236610Z`全部阶段通过。
- 框架全量证据`out/validation/full-2026-09-19T17-23-45-330Z`：核心29/29、GUI231/231、IPC217/217；Schema/CLI13、IPC92、Frame7及真实RustFS全部通过。
- Skill内容检查194个本地链接有效，脚本语法/模板JSON/无开发机绝对路径/无未完成标记通过，见`out/validation/skill-content-check.json`。
- ZIP实际解压到新位置后，接口检查及插件生成通过，见`out/validation/skill-relocation.json`。
- 结束只读检查未发现vision-*、sample-*、rustfs或ctest测试进程残留。

## 边界

测试环境使用本机已准备的SDK，不能声称干净机器自动安装和跨平台已验证。
模板是源码集成而非正式SDK发行包；发布许可及Production限制不变。
本批未进行独立Agent新会话行为实验，未量化提速，未修改用户全局技能配置。

## 与框架计划的关系

这是面向使用者的S01开发辅助交付，不替代T24/T27的生产实现，也不关闭T31正式打包与许可任务。
业务源码通过可选CMake入口接入，框架默认不加载任何生成目录。
没有安装全局技能，没有上传发布，没有更改真实硬件控制配置。

## 最终结果

S01首版完成：可用Skill入口、参考资料、两个实际可运行模板、检查/生成/验证脚本及使用指南。
首版包为`out/packages/vision-framework-dev-0.1.0.zip`，包含26个文件，不包含缓存、SDK、凭据或测试产物。
生成器测试5项、应用模板4项、输出模板3项通过；框架旧测试全量保留。
当前不承诺新会话Agent成功率或提速百分比，后续按实际开发任务评估并迭代。
