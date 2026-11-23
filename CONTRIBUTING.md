# 贡献指南

先阅读 [README](README.md)、[实际架构](docs/architecture/overview.md) 和 [功能范围](docs/product/feature-matrix.md)。产品规划与已验收功能分开记录，macOS 当前仍待真实 CI 验收。完整导航见 [文档索引](docs/README.md)。

既有 `AGENTS.md` 按项目约束保留，其中“空工作区／未选择工具链”是初期记录；当前目录、命令与风格以本指南及上述文档为准。不要因文档任务改写它或删除依赖声明。

## 开始修改

- 先检查 `git status --short`、当前分支及最新主干；保留无关改动和用户数据。
- 维护者日常直接在 `master` 上工作，每个已验证阶段作聚焦提交；不把功能长期分散在多个分支。外部贡献可通过 fork 和 Pull Request 提交，最终接受的变更汇总到 `master`。
- 修改前说明目标、边界和验证方式。新协议、持久化格式、权限、发行或平台支持变更先说明兼容性与验收范围。
- 目录按实际职责演进，避免空模块、跨层依赖和无关格式化；同步受影响文档。

## 构建与测试

准备依赖后，按 [源码构建说明](README.md#从源码构建) 运行对应平台入口：Windows `scripts/build.ps1`／`scripts/verify.ps1`，Linux `scripts/verify-linux.sh`；macOS 的原生预设、托管 CI 及个人机器检查边界见 [平台指南](docs/testing/macos-acceptance.md)。命令在仓库根目录执行。

已有 Windows Release 预设构建时，可针对改动运行，例如：

```powershell
ctest --test-dir out/build/windows-msvc-release -R 'Checksums\.' --output-on-failure --no-tests=error
```

按实际构建目录和受影响用例调整；完整验证及外部媒体工具前置条件见 [测试场景](docs/testing/scenarios.md)。浏览器扩展用 `npm --prefix browser_extension ci` 和 `npm --prefix browser_extension test`。不要把未运行、跳过或模拟的检查写成真实平台通过。

## 代码与数据约定

- C++20、Qt 6；按 `.clang-format` 使用四空格缩进、110 列目标及现有命名。公共类型／方法匹配所在模块，不进行批量重命名。
- `.clang-tidy` 提供适用诊断配置；`verify.ps1` 不自动运行两个 clang 工具。格式与静态检查按本次改动补充，编译警告应核对。
- 领域／应用层保持独立，具体网络、存储和系统 API 留在基础设施；Qt 控件留在 UI。迁移新增递增 SQL，不改写已发布迁移。
- 为行为变更覆盖成功、失败和适用边界；下载测试优先使用自有本地服务，验证中断、恢复、文件冲突和错误路径。
- 构建、下载、临时数据放在忽略的 `out/` 等目录。不要提交凭据、私钥、登录头、带秘密的链接或真实浏览器配置。实际依赖名称、版权和许可证必须保留。

## 提交与 Pull Request

提交主题使用简洁祈使句，如 `Fix queue selection after restart`，每次聚焦一项完整变更。PR 描述问题与修正后的行为、相关 issue、影响范围及实际运行的命令／平台；未验证项明确列出。

界面修改附正常和窄窗口截图；协议／迁移修改说明兼容性，发行相关修改说明包和运行环境。提交前检查 `git diff --check` 和生成内容，避免混入无关文件。GitHub CI 失败应核对日志并修正，配置文件存在不代表检查已通过。
