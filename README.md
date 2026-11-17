# MaskPilot

基于 C++20 和 Qt 6 Widgets 的 Windows 下载管理器。HTTP/HTTPS、FTP/FTPS、媒体适配器及独立 BT 引擎负责传输、恢复、校验和安全落盘；桌面、命令行与浏览器共用任务调度和持久化层。

当前版本是核心流程可运行的 **0.2.0 候选版**，尚未实现完整功能目标。动态分段再分配、多命名队列／周期调度、递归站点抓取、Firefox 集成、完成后扫描及在线更新等仍缺失，详见 [功能与缺口](docs/product/feature-matrix.md)。本地测试通过不代表这些功能已完成。

## 使用

解压发布 ZIP 后直接运行 `maskpilot.exe`，或双击 `Install.cmd` 安装到当前用户目录，无需管理员权限。升级前退出程序，再运行新版本的安装脚本。`Uninstall.cmd` 保留任务数据库、配置和已下载文件。

1. 点击“新建下载”，填写地址和保存位置；高级选项提供连接数、校验值、代理、请求头、任务限速与计划时间。
2. 使用工具栏或右键菜单暂停、恢复、调整优先级、重新下载、刷新链接或登录信息。删除记录保留最终文件；取消任务清理其缓存。
3. 设置中调整并发、全局限速、分类目录、中英文、托盘和完成动作。关机／退出动作有可取消的倒计时。
4. “批量添加”可提取文本链接或展开数字 URL 模板；导入任务默认暂停，确认后恢复。

HTTP 文件只有在服务器提供可验证的强资源标识及 Range 支持时才续传；其余情况安全重下。认证头与 Cookie 仅保存在本次进程内，重启后需补充。带签名参数的原始 URL 保存在本机任务数据库和主动导出的任务清单中，请保护应用数据目录与导出文件。

## FTP / FTPS

在新建任务中粘贴 `ftp://` 或 `ftps://` 地址，“FTP / FTPS”页提供用户名、密码、主动模式与私有 CA 证书。`ftp://` 默认明文／被动模式，勾选“必须使用显式 TLS”可要求 AUTH TLS；`ftps://` 使用隐式 TLS。始终校验证书和主机名，不自动降级为明文。请勿将账号密码写进 URL；账号密码不保存，重启后通过“更新请求”重新输入。

FTP 使用单数据连接。只有提供预期 SHA-256 且 URL、大小、修改时间、TLS 模式匹配时才尝试 REST 续传，最终摘要必须一致；服务器拒绝续传则重下。不满足条件时安全重下。支持文件下载，不提供 FTP 上传或远程目录浏览。浏览器扩展目前仍只接管 HTTP/HTTPS。

```powershell
maskpilot_cli ftp://example.test/file.zip C:/Downloads/file.zip C:/Downloads/state --ftp-user user --ftp-explicit-tls --sha256 <expected-sha256>
```

有密码的登录优先使用桌面密码框；命令行参数可能出现在系统进程列表和 shell 历史中。

## BT / 磁力

工具菜单可打开本地 `.torrent` 或添加 `magnet:?` 链接；新建任务的 BT 页可设置全部文件、先获取元数据再选择文件、完成后做种及上传限速。任务详情提供文件勾选、上传和连接信息，设置中提供全局上传预算。v1／v2／混合种子、磁力及纯 Tracker 元数据获取已通过本地真实传输验证。0.2.0 候选包包含 FTP/FTPS 和 BT 引擎，验证范围见 [验收记录](docs/releases/acceptance.md)。

每个 BT 任务使用独立目标目录，保留种子内部目录结构。选择的内容校验完成后才整体发布，已有同名目录不会被覆盖。暂停保留缓存；取消清理该任务拥有的未发布缓存，已发布文件始终保留。做种占用活跃任务名额，可暂停或选择“结束做种”。恢复时重新校验已有分片。

原始种子元数据随任务保存，删除原 `.torrent` 不影响任务恢复。元数据可能包含私有 Tracker 地址或 passkey，**也会包含在主动导出的任务清单中**，请保护数据库与清单。自动导入的原始种子文件必须位于直接挂载的本地磁盘，不读取网络共享、设备或重解析路径；可先复制到本地再添加。

单个种子元数据上限 16 MiB、文件上限 20,000。BT 暂不支持代理路由；带代理的任务会报错，不会悄悄直连。浏览器扩展仍只接管 HTTP/HTTPS。公网连通性受网络、系统端口限制、Tracker 和种源影响，本地夹具不代表所有公网种子可用。

```powershell
maskpilot_cli C:/Downloads/example.torrent C:/Downloads/example C:/Downloads/state --select-files 0,2 --upload-limit 65536
```

命令行是限时验证客户端；需要持续下载或做种时使用桌面程序。
在命令前加 `--bt-no-discovery` 可关闭 DHT、局域网发现和自动端口映射；显式 Tracker 与 Peer 仍启用，这不是禁止联网的开关。

## 浏览器

安装脚本注册当前用户 Chrome/Edge 通道。便携版可运行：

```powershell
./scripts/register-browser.ps1 -InstallDirectory 'C:/Apps/MaskPilot'
```

在浏览器扩展管理页启用开发者模式，加载包内 `browser_extension/`。右键发送链接，或点击扩展图标发现页面资源。自动接管默认关闭；Cookie 传递需单独开启，仅对所选下载地址取 Cookie。商店签名安装包尚未发布。协议和回退规则见 [浏览器协议](docs/protocols/browser.md)。

## 媒体

工具菜单提供“媒体组件”，可下载校验过的解析／合并组件，也可在新建任务中选择已有可执行文件。组件可选，默认安装包不捆绑。公开 MP4、HLS、DASH 点播支持视频 MKV 或音频 M4A。媒体暂停后恢复、重试或更改带宽会重新下载，以避免拼接不同版本。媒体模式拒绝登录 Cookie／认证请求头；受保护内容、直播及 DRM 不支持。网站适配取决于组件和站点当前行为，详见 [支持矩阵](docs/product/support-matrix.md)。

## 从源码构建

Windows x64，Visual Studio 2022 C++ 桌面工具、Windows SDK、CMake ≥3.25、Ninja、Qt **6.11.3 MSVC x64**、vcpkg、Python 3、Node ≥22。设好环境变量，仓库脚本会初始化编译器环境：

```powershell
$env:QT_ROOT_DIR = 'C:/Qt/6.11.3/msvc2022_64'
$env:VCPKG_ROOT = 'C:/Tools/vcpkg'
./scripts/build.ps1 -Configuration Debug
./scripts/verify.ps1 -Configuration Release
npm.cmd --prefix browser_extension ci
npm.cmd --prefix browser_extension test
./scripts/dependency-sources.ps1
./scripts/package.ps1
./scripts/smoke.ps1 -PackageDirectory out/package/MaskPilot-0.2.0-windows-x64
```

已有独立 Release 构建时，可用 `./scripts/package.ps1 -SkipBuild -BuildDirectory out/build/windows-msvc-protocols` 明确选择产物。脚本检查项目版本、运行客户端、Qt 和编译器匹配，避免将旧二进制混入新包。

固定依赖由 vcpkg 清单获取；生成文件全部放在 `out/`。更换 Qt 版本时同步更新依赖源码清单和哈希，打包脚本会拒绝版本不一致。安装包包含所用 Qt 模块源码与许可证；原始工程代码采用 [MIT](LICENSE)，实际依赖见 [NOTICES](third_party/NOTICES.md)。

脚本选取已安装的最新 MSVC 工具集。更换 Qt 安装位置或编译工具集后，在初始化的编译环境中执行 `cmake --fresh --preset windows-msvc-release` 再编译；打包会检查构建使用的 Qt、编译器及 Release 配置，并拒绝检测构建。

## 工程与验证

`apps/` 为三个入口，`src/domain/` 为模型，`src/application/` 为调度，`src/infrastructure/` 为网络／SQLite／媒体，`src/ui/` 为窗口、模型和对话框，`browser_extension/` 为浏览器扩展，`tests/` 为单元、故障与真实应用测试，`packaging/` 为安装流程。业务层不依赖 Qt UI 或具体网络实现。

开发顺序、状态不变量和主要目录见 [工程蓝图](docs/DEVELOPMENT_BLUEPRINT.md)。具体测试命令见 [验证场景](docs/testing/scenarios.md)，发布证据与待验收项见 [验收记录](docs/releases/acceptance.md)。未完成的压力、干净系统或商店验证不代表已通过。

贡献代码使用 C++20、4 空格缩进与 `.clang-format`（LLVM 基线，110 列）；类型用 `PascalCase`，函数和局部变量沿用 `camelCase`。`.clang-tidy` 提供静态检查配置，但 `verify.ps1` 不自动运行它。GoogleTest 测试采用 `TEST(Suite, DescriptiveBehavior)`，网络场景优先使用本地故障服务；新增行为须说明验证方法。提交主题使用简短的祈使句；PR 说明行为变化、关联问题及验证结果，界面变化附截图。既有 `AGENTS.md` 按用户要求保留，当前工程说明以本文与蓝图补充。
