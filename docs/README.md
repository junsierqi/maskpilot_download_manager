# 文档索引

## 使用与现状

- [主 README](../README.md)：入门、下载格式、设置、浏览器接管和源码构建。
- [支持矩阵](product/support-matrix.md)：具体协议、格式、认证、恢复和平台边界。
- [功能映射](product/feature-matrix.md)：原始目标、已有实现、明确缺口及验收范围。

## 开发与接口

- [贡献指南](../CONTRIBUTING.md)：构建／测试入口、风格、提交和 PR 要求。
- [当前架构](architecture/overview.md)：实际目录、CMake 目标、依赖方向与任务流程。
- [工程蓝图](DEVELOPMENT_BLUEPRINT.md)：产品目标、开发规则和演进边界，不等同于全部已交付。
- [工程决策](architecture/decisions.md)：设计演进及历史取舍，当前支持以矩阵为准。
- [浏览器与本机协议](protocols/browser.md)、[任务清单](protocols/task-list.md)：接口格式、授权及兼容性。

## 验证与分发

- [测试场景](testing/scenarios.md)：用例分类、运行方式与证据要求。
- [Linux 验收](testing/linux-acceptance.md)：已测试的具体用户态／内核、构建及本地包限制。
- [macOS 验收入口](testing/macos-acceptance.md)：双架构 CI 准备及待真实执行的检查。
- [历史 Windows 仓库审计](testing/repository-audit.md)、[候选包验收记录](releases/acceptance.md)：仅代表各自版本和环境，不能替代当前提交验收。
- [原始代码许可证](../LICENSE)、[实际依赖声明](../third_party/NOTICES.md)：区分工程代码与依赖的许可及分发材料。

GitHub 的问题／PR 模板和贡献指南用于协作便利，不表示正式发行、完整社区配置或所有平台检查已完成。
