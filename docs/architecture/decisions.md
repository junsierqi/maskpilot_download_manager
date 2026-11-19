# 工程决策

## 离线安装更新与恢复

安装、更新、卸载共用 PowerShell 包归属与路径规则。离线 ZIP 的外部 SHA-256 来自独立可信渠道，在同一只读锁定句柄上先核对再解压；完整内部清单、固定必需文件及版本再作一致性检查。路径、重解析点、大小、条目数、大小写重复和文件／目录冲突均在替换前检查。目标互斥锁与实际进程路径检查保护协作操作，不自动终止用户进程。

更新使用安装目录旁的受控事务目录。先持久化准备记录，再写入并核验备份与待替换文件；进入应用阶段后逐文件原子替换，安装标记最后写入。单个日志封装不可变计划，避免清理多个元数据文件留下恢复缺口。中断恢复和明确回滚先核对所有当前／备份文件，只接受计划中的旧、新或合法缺失状态；外部修改保留现场。它提供可恢复更新，不声称整个目录与注册表跨文件原子提交。

成功升级保留一份回滚材料，明确完成后才清理已归属、哈希匹配的事务文件和空目录。原浏览器配置／注册日志保留原字节与旧归属哈希，避免把用户修改重新认领为可卸载文件。负载回滚不回退任务数据库，注册表仍使用既有精确归属日志独立处理。

捕获的写入错误仅清理当前调用以独占创建方式取得的文件，预先存在的临时文件不归该调用处理。进程终止留下的不可核验截断暂存或日志按冲突保留，需明确人工核验；可恢复声明限于有效持久检查点。此边界用于同时保留外部修改与诊断材料，不以删除不匹配文件制造恢复成功。

发布脚本拒绝覆盖已有阶段目录与 ZIP。资格基线只约束原生源码、运行文件和实际工具；安装／发布脚本可独立演进，但发行包中的运行文件必须与冻结长测的集合、大小和哈希一致。私有构建缓存和本机路径留在测试证据中，公开包只记录版本、源码编号及资格摘要。

## 2026-11-18：站点项目协调与不可变修订

项目协调器复用现有下载协调器，使用单个原子 JSON 清单保存一个项目的运行意图、稳定请求编号、完整子任务计划和发布回执。它不另建传输引擎，也不跨协调器持有数据库锁。预览获取 HTML／CSS 以发现资源，确认开始后处理已识别的二进制资源；同一地址的具体资源角色优先于通用页面链接。正文预算由已验证字节和未完成子任务的完整预留组成，零剩余额度不能变成不限量任务。

SQLite 13 保存不可变的项目编号与修订关联。原生协调器先暂停恢复中的项目子任务，再处理队列策略，避免等待窗口任务在项目核验前自动运行。创建确认、恢复、内容读取及发布均核对归属、请求编号、完整路径、URL、队列、代理、上限、响应类型与哈希；被删除、取消或修改的任务不会自动重建。项目清单写入失败关闭项目调度并暂停关联工作，普通下载仍可使用。

发布前先持久化暂存令牌及哈希清单，再写入同卷受控暂存目录，最后执行不可覆盖的修订目录重命名。重启只在不可变标记和全部哈希匹配时接受已重命名的结果；失败、取消或未完成写入保留先前发布。清理只删除清单所属且哈希未变的暂存文件，不递归删除未知目录。更新明确重新获取，不将旧本地哈希误当作远端新鲜度证明；历史删除保留原始下载和导出文件。

Qt 项目视图异步排空命令结果。主窗口把草稿、预览确认、暂停、错误和发现／发布间隙计入未完成项目，取消项目不会触发成功完成动作。销毁顺序先关闭窗口，再停止项目协调器，最后关闭下载协调器，避免悬空回调和退出时重新派发子任务。

## 2026-11-18：有界静态内容处理

站点内容端口只接受已经下载的字节、实际响应地址及类型，不提供网络或文件加载接口。HTML 使用固定版本 libxml2 的内存解析上下文，每个上下文拒绝全部外部资源；不依靠无效的 NONET 标志，不修改全局加载器。限制输入、树深度、节点、属性、引用及序列化输出；CSS 分词与重写逐次检查输出预算。已解码标识符重新转义，字符串中的尖括号使用 CSS 转义，防止 HTML 原始文本区域被重新打开。语法不支持、资源超限和调用方失败分开处理。

仅保留被允许的静态标记、同来源引用和验证过的相对导出路径，移除活动内容并插入严格 CSP。已知图片、字体和媒体还须匹配受支持的文件签名，不能直接导出声明为图片的 SVG／HTML。签名检查不等同于完整媒体解码或通用恶意文件扫描。支持基础 Unicode／Latin-1 编码，其他编码明确拒绝。完整项目状态机、预览与不可变修订发布属于后续独立模块。

SQLite 12 将 `response_content_type` 与响应地址一起持久化。该观察值从成功探测及各实际响应一致性聚合而来；缺失、重复冲突、参数差异或不安全字符使其为空，普通下载不因此失败。416 空资源响应的错误页类型不代表资源；完整缓存仅采用本次成功探测的类型。所有重试、重新开始、请求编辑与无效事件清除旧观察值，清单不能恢复或注入类型权限。

## 2026-11-17：界面与候选包

继续使用 Qt Widgets。`ui/src/style/appearance.*` 集中管理样式和矢量图标；任务委托只呈现不可变快照，协议、调度和文件操作不进入绘制代码。窄窗口收起次要列，保留名称、进度、状态与速率；状态栏常驻速率与限时通知分离，刷新不覆盖通知。

CMake 生成领域层版本头，桌面、默认 User-Agent 与验证客户端共用版本。打包可指定已验证的独立 Release 目录，检查版本及 SDK，收集实际新增运行库和完整依赖声明。协议夹具可仅清理产品子进程的开发环境，父进程继续提供本地 TLS／种源；部署正确性必须由真实包内下载验证。

## 2026-11-18：显式代理路由与凭据生命周期

领域模型保留兼容的无凭据代理地址，使用统一解析器验证协议、主机、IPv6、端口和凭据长度。HTTP／FTP 共用私有配置帮助函数：空地址显式直连，非空地址显式取消环境绕过，代理认证与源站认证分开。SOCKS5 目标 DNS 经代理；代理自身域名可由系统解析。HTTPS 代理保留证书验证，主动 FTP 与代理组合明确拒绝。

BT 工厂按不可变端点及内存凭据身份共享活动会话，不在任务间修改全局网络设置；闲置会话释放，过期路由键在下次获取或工厂销毁时移除。代理模式关闭 UDP 发现、uTP 与自动端口映射，不支持 HTTPS 代理。数字 IP 代理端点有额外的五秒可取消 TCP 连通性检查；主机名沿用原生解析器，不把操作系统 DNS 延迟算作已保证的五秒上限。源站或单个节点失败不使整个共享路由失效。

schema 5 只追加需要补充代理凭据的标记，升级前备份。配置校验允许缺少秘密的暂停／导入任务；实际适配器在联网前检查凭据。用户名和密码不写入任务库、清单、日志或辅助进程参数。标记约束凭据可用性，不强迫服务器选择某种认证方法。媒体认证代理使用与探测共享的私密标准输入通道。

## 2026-11-17：独立 BT 引擎

领域模型显式区分 HTTP、FTP、媒体、BT，持久化状态值只追加。应用端口传递不可变元数据／文件描述、上传统计、文件选择与结束做种控制，不暴露具体库类型。独立 `transfer/transfer_router.cpp` 路由到各协议适配器，媒体模块只负责媒体传输。共享工厂提供一个惰性 BT 会话及告警分发器，协调器继续掌握并发和优先级；不能为每个任务创建一个完整客户端。

schema 4 保存有界种子元数据、选择、上传策略与发布标记，兼容旧媒体字段并迁移原有协议类型。添加本地种子时先由适配器安全捕获字节，再持久化确认；导入先整体验证。原始路径后续失效不触发重新读取。原始元数据可能包含私有 Tracker 信息，属于需要保护的本地任务数据。

BT 使用相邻的任务专属临时目录及身份标记，拒绝不安全路径、文件冲突和重解析路径。仅选择文件通过分片校验后，等待暂停告警和磁盘释放，再同步文件并禁止覆盖地发布整个目录。恢复数据不能指定存储路径或跳过校验；做种模式禁止修复或重新下载已发布内容。取消做种不会删除已发布目录。代理未实现时显式失败。

磁力先获取并验证哈希绑定的元数据，释放元数据阶段句柄后，以完整文件优先级启动载荷阶段，避免零选中文件的完成状态和重连退避。尚不完整的 v2 分片层不写成完整种子；保留权威信息字典，由引擎获取并验证必要哈希。只携带有界的当前在线 Peer，不信任磁盘恢复数据中的网络目标。

自动监听若因系统端口权限失败且没有任何成功监听，最多尝试 8 个随机非临时端口；不重写显式配置或已有成功监听。任务加入前最多等待 5 秒并响应暂停／取消，失败给出明确错误。网络分发线程的控制状态先初始化，再启动线程。首次发布立即保存标记，持续做种不能延迟其持久化。

## 2026-11-17：协议适配与 FTP

FTP/FTPS 使用独立 `network/ftp_transfer.cpp`，不复用 HTTP 状态码、Range 或 ETag 逻辑。`filesystem/transfer_files.*` 提供已有的同步写入、恢复元数据原子替换及禁止覆盖发布；协议判定和重试策略仍属于各适配器。现有传输端口、串行任务协调器和 Qt 任务表保持共用。

FTP 默认单流／被动模式；显式 TLS 要求控制和数据通道全部加密，隐式 TLS 由 `ftps://` 选择。用户名和密码仅存内存，URL 禁止嵌入登录信息。schema 3 只增加 TLS、主动模式与 CA 路径，升级前备份旧数据库，旧凭据不因新增协议而写入磁盘。

FTP 的 SIZE／MDTM 不是强内容标识。缓存复用必须同时有预期 SHA-256 和一致的 URL、大小、修改时间及 TLS 选项，最终校验失败不会发布；不满足条件或服务器不支持 REST 就重新下载。该取舍避免假冒可靠续传，尚未验证的服务器组合继续显式列为边界。BT 使用独立共享会话与适配器，其上传／做种生命周期不进入 FTP 或 HTTP 状态机。

按阶段保留本地提交以便回退。数据库迁移不自动降级；若切回不识别新 schema 的旧程序，先退出全部实例并单独备份当前数据，再在隔离数据目录使用升级前的 `.before-v*-*.db` 副本验证。恢复旧备份不会包含备份之后的新任务状态；下载文件与未知用户文件不由代码回滚删除。

## 2026-11-16：开发基线

用户授权按总蓝图自主开发。首发 Windows x64，C++20，Qt 6 Widgets。已检查本机 Qt 6.11.0、MSVC 2022、CMake 3.31；原生依赖通过固定 baseline 获取。主要目录和依赖方向沿用总蓝图。参考研究在工程外进行，不复制参考项目源码、名称或标识。

## 网络、落盘与线程

任务控制器和数据库由一个协调线程串行访问。每个活跃任务一个有界工作线程，独占自己的 multi transport 和文件；不为每个分段创建线程。相较独立磁盘线程池，这个方案减少跨线程缓冲与背压协议，内存有界，磁盘阻塞不影响 UI。后续仅在测量证明必要时增加磁盘执行器。

严格验证状态码、Content-Range、字节长度与资源身份后才写入。强验证器支持可靠跨会话续传；缺少可靠身份时重新下载。分段同步后原子更新可恢复偏移，恢复按该偏移截断不确定尾部。文件最终组装在目标卷，再以不覆盖既有文件的方式发布。

## 敏感信息与持久化

Cookie 和认证头仅在内存存活，不写入任务表。重新启动后需要登录态的任务可能要求重新从浏览器发送。原始 URL 本身可能含短期凭据，保存在当前用户本地数据库。用户主动导出的任务清单也包含原始 URL，导出对话框会提示；清单不含 Cookie 和认证头，应自行保护导出文件。诊断界面隐藏查询参数。

## 浏览器确认顺序

浏览器先暂停，原生端验证并持久化，返回与 request ID 对应的确认后，才取消浏览器原下载。无法确认时恢复浏览器下载，避免任务丢失。

## 原生资源约束与响应观察

站点子任务复用原生 HTTP 引擎，领域请求只追加文件正文大小与同来源重定向约束。已知总长先于工作文件检查；未知长度在写入前逐块检查，合并阶段再次校验，不把每流或进度数当作总上限。流量账本独立计量已接收的探测／重试正文。重定向比较初始来源的协议、主机与有效端口，保留更严格的精确 URL 登录授权，数据连接继续禁止自动跟随重定向。

HTTP 适配器通过可信端口报告实际响应 URL，协调器只接收当前代次，并在新尝试／请求编辑前清除旧观察；损坏观察触发终止而不成为输入权限。schema 11 持久化约束和响应地址，清单只导出请求约束。请求重放同时核对大小、来源策略、摘要、代理登录需求与媒体选择，不能用旧的较弱任务确认新请求；保留原请求文件名、归属和路由校验。

## 浏览器页面与平台适配边界

浏览器平台入口共享 TypeScript 行为，拆分 Chromium 服务工作线程和 Firefox 模块事件页清单。后台组合独立 Cookie 上下文与有界资源会话模块；页面渲染器为无模块导入的隔离经典脚本，不开放 `postMessage` 或 native 桥。可信扫描生成候选资源、不透明编号和稳定请求编号，真实选择保存在隔离闭包，异步 Cookie 查询后重新核实文档及会话。Firefox 固定相邻清单和扩展编号与 Chrome／broker 分支分开；注册日志按当前所有权恢复原值。两种扩展仍使用独立下载区。

## 媒体隔离与恢复边界

领域层保存有界的格式描述与类型化选择，不保存解析器原始 JSON／格式 URL。应用层提供独立探测端口，通过现有路由和凭据解析取得短期执行快照。桌面探测仅用于独立下载区；受环境授权的任务不能借此扩大源站授权。Qt 控件在独立可取消工作线程探测，代次失效阻止旧结果覆盖新地址。显式格式在传输前通过同一路由再次核实；精确 ID 转成过滤条件，避免保留字被当作选择表达式。

共享进程模块负责清理环境、私密输入、Windows job 及退出清理。包内两个无 Qt 依赖的静态运行库启动器覆盖 FFmpeg 和 ffprobe 的显式／隐式输入，只允许任务目录中的普通文件及非播放列表容器；不允许本地后处理重新联网或读取任意目录。实际组件路径由可信启动环境绑定，解析器固定使用自有启动器目录。该限制针对受支持组件调用，不能代替不可信可执行文件沙箱。schema 10 追加模式、主格式、音轨与容器列；迁移前备份，旧任务保持原有自动预设。

媒体通过独立外部进程运行，Windows job 在进程创建时绑定，取消时清理进程树。公开地址可用；为防跨站凭据传播，媒体任务拒绝认证头、Cookie 和自定义头。组件不能证明跨次内容身份，因此每次重新开始都清理专属缓存，禁止把组件普通续传当作安全恢复。

媒体子进程不支持原地调整限速，实际预算变化会安全重启媒体任务。调度器在一个批次中保留稳定媒体槽位，降低并发时按既有活跃数预留，批次排空后再收缩，避免带宽超配或因普通队列波动反复重启。

## 发布目录与维护

UI 的私有状态、表格展示和不同对话框分置于 `windows/`、`models/`、`dialogs/`，公开接口保持不变。无需预建没有代码的空目录。系统完成动作及解压程序使用 Windows 系统目录绝对路径，不从下载目录搜索同名程序。

便携包和当前用户安装使用相同动态运行库；包内附许可证、Qt 模块源码、SBOM 和文件哈希。可选媒体组件由用户安装或从工具菜单独立下载，默认不再分发。安装与卸载只操作专属应用目录；用户数据库与下载文件保留。外部发布、签名、更新源和浏览器商店不在本地候选包完成声明内。

## 稳定性诊断与依赖补丁

旧运行时在混合压力测试中发生访问异常，调用栈位于请求校验的正则匹配；尚未确认根因。项目代码两组 15 分钟 AddressSanitizer 压力运行分别完成 5,300／5,400 个文件校验，未报告越界或释放后访问；这些诊断不能代替发布稳定性验收。Windows 默认关闭的分配／释放类别检查保持默认，避免将未插桩依赖的分配标记视作已确认根因。

依赖升级至 Qt 6.11.3：其包含已独立复现的容器清理后访问缺陷修复；该缺陷尚未证实可由旧压力场景触达，因此不将升级表述为已定位原始崩溃。配套源码、许可证、运行库及验证必须使用同一版本。Release 保留独立 PDB，以便诊断优化构建，发布程序仅记录 PDB 文件名。

后续长测暴露历史删除超时。逐条完整同步写入的隔离测量不足以解释该次卡顿，因此暂不引入影响新增确认顺序的通用事务合并。仅跳过不存在缓存目录下的重复删除请求，继续独立清理经过验证的专属组装文件；既有最终文件始终保留。压力测试在失败后采集隔离桌面线程转储，保持 10 秒删除断言及所有既定预算，以继续定位历史卡顿。

测试框架的报告发布及结束状态回归归入 `tests/system/stress_report_test.py`，与原生消息交互集成分开维护。Windows 报告发布对短暂读取占用作有界重试，持续错误仍失败；实时监测读取进程输出的有界进度摘要，避免打开正在替换的报告文件。只有结束报告成功发布后才进入接受清理，任何异常都撤销接受状态，避免错误被后续清理误标为通过。

## Optional native credential vault foundation

The application defines `ICredentialVault` independently of Windows. Its native adapter stores only explicitly supplied proxy credentials in current-user Windows Credential Manager (`CRED_TYPE_GENERIC`, `CRED_PERSIST_LOCAL_MACHINE`). Random installation and reference IDs form a private target namespace. An entry binds its owner and canonical proxy route; mismatching reads and deletes fail. HTTP and HTTPS remain distinct; SOCKS5 aliases share the product's remote-DNS semantics. Native blob limits are checked before writes; malformed entries fail closed. Other platforms report unavailable without plaintext fallback.

This protects persisted credentials at the Windows logon-user boundary; it does not isolate hostile processes running as the same user. The adapter does not enumerate entries or accept caller-selected native target names. Native-read buffers are cleared before release, but complete in-process secret zeroization is not claimed. Callers must store a returned reference transactionally, clean up a failed reference commit, and revoke active use before deleting owned entries. Factory creation itself performs no writes.

The context core connects optional storage to task/policy persistence, desktop Remember/Forget controls and scoped IPC. Browser cookies are not stored in this vault. Tests use random owned installation IDs, real native writes/reads/deletes and a second process; they never enumerate or delete unrelated credentials.

## Actor-owned download contexts

One coordinator owns context policies, task changes, capability sessions and both SQLite repository facades. Task ownership, scoped request ID and route revision are server metadata outside editable download specifications. Existing standalone task/cache IDs remain unchanged. Profile submissions receive independent task IDs and replay within their owner, source, requested filename and pinned route identity. Ordinary projections contain only names, IDs, state and byte counts; owner filtering happens before pagination.

Context changes invalidate old caller sessions and affect new tasks. Accepted tasks retain their previous proxy snapshot/reference until explicitly rebound while inactive. Credentials are resolved into a worker copy before transport creation. Browser-origin grants are exact-URL, owner-bound and short-lived; only a required marker survives restart. Grant-bearing HTTP redirects stop before requesting any changed URL. Actual browser storage must supply correctly filtered grants; an extension origin alone never identifies a profile.

Optional native writes have a durable ownership journal before the OS call. A separate per-write nonce binds that journal to the native entry, preventing failed-write cleanup from claiming an older reference. SQLite and the OS vault are not one transaction: failed cleanup retains metadata for retry; startup loads all task/policy references before exact unused-entry cleanup. Completed history intentionally retains a referenced credential until removal or context revocation. No credential enumeration is used.

Revocation first persists disabled policy and denies new work, then pauses owned workers. Acknowledgement waits for their teardown and exact owned credential cleanup; cleanup errors keep the context disabled and report incomplete cleanup. Published files survive. Failed unused-entry cleanup reports a warning without disabling unrelated downloads. Per-context processes, schedulers and SQLite writers are unnecessary. BT sessions include owner identity even when route credentials match.

The typed core is tested independently of desktop composition. Framed broker authorization and actual profile/Remember/rebind/revoke widgets have separate product-path tests. Genuine sibling-browser issuance remains separate acceptance work; downloader fixtures alone do not establish that path.

## Scoped native broker composition

Desktop startup supplies the shared task/context store and native vault to the single coordinator. A Qt Core protocol adapter replaces window-owned native request handling. Administrative broker authority and legacy extension authority have separate per-start tokens; ordinary bearer requests use the core session registry and never fall back to another authority. The native host has a deliberate local broker invocation distinct from browser-origin invocation. Credentials use framed standard input, not command-line arguments.

The current extension remains a standalone v1 client. v2 fixtures establish downloader-side context readiness; they do not establish actual fingerprint-browser Profile/Storage/Bridge issuance. The UI remains trusted local administration, while browser projections and controls are owner-scoped inside the application actor.

## Context controls and authentication recovery

Profile administration and narrow profile submission live in a dedicated dialog implementation. QObject-bound future polling keeps actor persistence, worker teardown and vault calls off the UI thread. Accepted tasks retain their route snapshot; only explicit inactive rebind adopts current policy. Profile selection filters presentation, while the actor independently authorizes every scoped call. Revocation cleanup clears the durable policy reference before successful acknowledgement; incomplete cleanup cannot reactivate a profile.

Source-login requirements are part of the specification and task persistence contract. Cookie/Authorization/custom header material and FTP credentials derive the marker automatically. Restart and interchange remove material but retain the marker. Standalone edits can deliberately clear it; profile rebind retains an earlier grant requirement. Each adapter checks requirements before network/helper creation. Bound HTTP requests stop at any changed URL, including same-origin changes, until fresh authorization is supplied.

Task-list JSON is handled by a bounded codec separate from dialogs. Version 2 exports authentication markers and advisory context provenance without authorities or secrets. Import validates everything, obtains explicit standalone preview acceptance, then persists sequentially with stable request IDs. A retained in-memory batch makes partial-save retry idempotent without pretending the entire batch is transactional.

## Durable adaptive HTTP extents

HTTP recovery uses a bounded versioned layout containing the strong ETag, resource length, effective URL, generation and complete ordered extent partition. Stable numeric IDs address `part-N` and `offset-N`; they do not determine assembly order. At most 256 stored extents coexist with the task's configured 1–8 live connections. Valid legacy count-based layouts migrate without renaming their existing parts. Invalid or changed identities invalidate owned numeric artifacts before new-resource writes; unknown-length or unvalidated resources remain single-stream.

After completed messages are drained and pending extents admitted, an idle connection may split a large active remainder. The transport detaches that handle outside callbacks, synchronizes and checkpoints its prefix, closes the file, then atomically commits a shortened parent and a fresh child. Child data files are created after that commit. A crash before commit retains the old parent range; a crash after commit uses the new partition and treats missing child data as zero. No retained prefix is copied or assigned twice. Historical HTTP requests may overlap abandoned tails; durable ownership never overlaps.

The private layout codec enforces bounded metadata, unique IDs and complete contiguous coverage. A prospective child ID's unreferenced numeric artifacts are cleared before committing its ownership. Layout replacement retries the same generation for at most about 240 ms of delays to tolerate transient readers; persistent failure reports attention and keeps the previous generation authoritative. Curl manages only active handles, and bandwidth allocation uses their count. Assembly follows byte position. Application cleanup enumerates existing canonical owned names within the shared numeric limit; restart reuses the same cleanup path. Unrelated cache entries and published files survive. This protocol change does not require a database migration or another scheduling actor.

## Named queues and manual intent

Strict daily native payload accounting uses a separate worker-safe facade on the same SQLite backend. Schema 9 stores immutable transfer-run identities and UTC days, cumulative observed and reserved high-water marks, per-queue charged usage and a monotone period floor. Absolute reservation targets make duplicate/stale requests idempotent. Credits commit before a shared native lease admits callback bytes; all HTTP extents and FTP retries share that lease. No worker waits for the actor while the actor may join it. Callbacks accept only a credited prefix; deliberate short writes become policy holds rather than network retries. Existing recovery prefixes and local merge/hash work are excluded.

Each run belongs to one day. Every callback checks the effective day before spending cached credit. Midnight stops and settles the old run; later admission opens a new run, so delayed settlement cannot refund today's allowance or carry yesterday's credit forward. Clock rollback cannot lower the persisted day. Clean settlement refunds only provably unused reservation once, after callbacks stop; abandoned runs retain uncertain charged credit. History deletion, queue moves and policy edits do not erase usage. Confirmed observations may lag a credit batch and are displayed separately from charged/outstanding values. Budget edits require inactive queue jobs. Positive budgets reject BT/media before factory creation; local torrent imports are rejected before ACK to preserve their durable metadata contract. This metric covers admitted native body bytes, not ISP/wire traffic or estimated external-engine usage.

Weekly queue windows use explicit IANA time zones and a pure local-minute predicate. The scheduling actor obtains one UTC instant through an application port; the QtCore adapter converts that instant to civil time, so DST gaps have no instants and repeated minutes follow the same predicate twice. Overnight and equal-endpoint 24-hour windows belong to their starting weekday. Missing zones hold closed without a local/UTC fallback. A per-UTC-minute cache is invalidated on forward or backward clock changes and policy updates. Window closure preserves Run intent, while manual pause and source authorization retain their existing authority. Schema 8 appends window policy to queue CAS records. Desktop global limits are supplied before the coordinator starts; widget construction is not an admission gate. This clock port does not replace the credential/session security clock.

Queue membership is trusted task metadata independent of source owner, context, proxy, grants and recovery identity. The existing coordinator owns a focused queue registry; queues add no worker coordinators or database connections. The shared SQLite backend exposes a queue facade alongside task/context facades. Migration 7 installs the default queue and persists membership and Run/Paused intent without moving task files or changing old state ordinals.

Queue configuration uses revision checks. Moving an inactive selection validates every member and conditionally updates all queue IDs and timestamps in one transaction; any mismatch rolls back the batch. Task owners, route snapshots, credentials and paths remain unchanged. The default queue cannot be removed, and a named queue must contain no task records before removal. Browser drafts and interchange files cannot choose queue policy or membership. Existing scoped submissions and imports use the default queue; trusted desktop controls can subsequently move inactive tasks.

The actor admits work in queue turns, preserving priority within a queue and intersecting global and queue active limits. Persistent manual intent distinguishes a user pause from queue policy waiting. Disabling a queue cooperatively pauses its running work; enabling it reopens only eligible policy-held Run tasks, preserving retry and one-shot gates. Completed, failed, revoked and manually paused work does not start through queue enable. Startup retains safe recovery semantics.

Rates are allocated across global, queue and task caps with unused-cap redistribution. Media helpers retain their actual launch reservation until exit; incompatible reductions return an ordinary pause-first error. Settings await that result before updating saved preferences. Finite exhausted allocations must never become the existing zero/unlimited control value. HTTP reduces live handles when a tiny positive allocation cannot fund one byte per handle; the durable extents remain pending. These are bandwidth controls, separate from the later traffic ledger and payload allowances. Completion notification and power countdown also check unfinished work, including tasks that have never been active.

## Portable Release build policy

Release dependencies use a separate `x64-windows-portable` triplet and install
tree. A shared CMake policy supplies empirically checked MSVC prefix mapping and
basename-only PDB references to both the application and dependencies. Debug and
AddressSanitizer remain diagnostic builds. Relative source locations retain their
filenames and lines; native tests run from the source root.

Pinned overlay patches change only libxml2's implicit portable catalog default
and OpenSSL's public build-information rendering. Existing per-context resource
denial remains in force. Signed supplier libraries are preserved byte-for-byte;
their SDK hashes and expected valid publisher signatures define narrowly reported
provenance exceptions. Private host paths remain forbidden in all runtime files.
