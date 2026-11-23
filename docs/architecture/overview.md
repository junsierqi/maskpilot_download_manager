# 当前架构

本文描述当前源码与 CMake 目标，产品目标和演进规则见 [工程蓝图](../DEVELOPMENT_BLUEPRINT.md)，支持边界见 [功能映射](../product/feature-matrix.md)。规划中的目录不等同于已实现模块。

## 目录与职责

| 路径 | 实际职责 |
|---|---|
| `apps/desktop/` | Qt 启动、单实例、依赖组合、用户数据目录和本机 IPC |
| `apps/cli/` | 命令行验证客户端及站点项目命令；不是无界面后台服务 |
| `apps/native_host/` | 浏览器原生消息帧、身份检查及向桌面转发 |
| `apps/media_guard/` | 限定可信媒体组件的本地文件输入／输出 |
| `src/domain/` | 纯 C++ 任务、请求、队列、代理、媒体、直播与上下文模型及校验 |
| `src/application/` | 用例与端口、任务协调、队列／额度／上下文、站点项目及浏览器恢复 |
| `src/infrastructure/src/` | `network/`、`transfer/`、`torrent/`、`media/`、`site/`、`persistence/`、`filesystem/`、`security/`、`process/`、`bridge/`、`scheduling/` 的具体适配器 |
| `src/ui/src/` | `windows/`、`dialogs/`、`models/`、`style/`，使用代码构建 Qt Widgets 界面 |
| `browser_extension/` | TypeScript 扩展、浏览器清单、构建脚本和 Node 测试；生成目录不入库 |
| `database/migrations/` | 递增 SQL 迁移；升级备份与未知未来版本拒绝 |
| `tests/` | 单元、集成、系统用例，本地服务及 Linux／macOS 专属夹具 |
| `resources/`、`packaging/` | 应用资源及安装／浏览器注册材料 |
| `cmake/`、`scripts/` | 原生依赖、平台构建、验证、依赖源码和打包入口 |
| `third_party/` | 实际依赖与分发声明；名称和许可证必须保留 |
| `out/` | 忽略的构建、包和本地验证输出 |

各层公开头文件位于其 `include/maskpilot/<层>/` 下；CMake 为四个层分别建立目标。不为将来可能用到的能力创建空目录或占位实现。

## 依赖方向

`maskpilot_domain` 只依赖工程编译选项；`maskpilot_application` 依赖领域层；`maskpilot_ui` 依赖应用层与 Qt；`maskpilot_infrastructure` 实现应用端口，连接 curl、SQLite、BT、加密、内容处理及原生平台接口。桌面和 CLI 入口组合应用层与基础设施，界面不直接实现协议引擎。

领域／应用源码不依赖 QWidget、curl、SQLite 或操作系统凭据 API。基础设施不依赖 UI；应用对象通过接口取得传输、存储、时钟和扫描能力。具体链接关系见各层 `CMakeLists.txt`，测试可组合实际适配器或注入故障实现。

## 任务流程与所有权

1. 桌面、CLI 或浏览器桥接校验输入，明确任务归属、路由、登录需求及输出路径。
2. 应用协调器管理任务状态、调度、队列窗口、预算预留和持久化；工作线程处理传输，事件回到协调器更新状态。UI 使用事件循环消费状态及异步命令结果。
3. 传输路由选择原生文件、BT、可信媒体组件或原生直播适配器。登录材料留在内存／私密输入，持久层只保存公开配置、需求标记和明确授权的凭据引用。
4. 内容先写入任务拥有的暂存文件／目录，核验资源身份和完整性，按要求扫描，再无覆盖发布。暂停、取消、失败、迟到回调和重启均按既有状态机核对，不直接改成成功。

站点项目复用同一任务协调器，通过独立原子清单管理预览、子任务及不可变修订，不另建网络引擎。浏览器恢复通过有界原任务会话刷新链接／登录材料，不扩大上下文权限。原生协议详见 [浏览器接口](../protocols/browser.md)，清单格式见 [任务清单](../protocols/task-list.md)。

## 平台与验证边界

平台差异主要位于依赖配置、凭据库、进程生命周期和文件发布适配器。Windows 使用原生凭据管理器和进程作业；Linux 使用 Secret Service、会话／pidfd 保护；macOS 已增加 Keychain、EOF guardian 和独占目录发布代码，真实编译／运行仍待 CI 验收。没有明文凭据回退；辅助组件须可信，进程约束不等于恶意代码沙箱。

构建入口见 [README](../../README.md#从源码构建)，平台资格见 [Linux](../testing/linux-acceptance.md)／[macOS](../testing/macos-acceptance.md)，测试方法见 [场景](../testing/scenarios.md)，取舍和历史迁移见 [工程决策](decisions.md)。源码边界检查和历史测试结果不替代当前提交的实机、浏览器、长期负载或正式发行验收。
