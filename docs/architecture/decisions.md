# 工程决策

## 2026-11-17：界面与候选包

继续使用 Qt Widgets。`ui/src/style/appearance.*` 集中管理样式和矢量图标；任务委托只呈现不可变快照，协议、调度和文件操作不进入绘制代码。窄窗口收起次要列，保留名称、进度、状态与速率；状态栏常驻速率与限时通知分离，刷新不覆盖通知。

CMake 生成领域层版本头，桌面、默认 User-Agent 与验证客户端共用版本。打包可指定已验证的独立 Release 目录，检查版本及 SDK，收集实际新增运行库和完整依赖声明。协议夹具可仅清理产品子进程的开发环境，父进程继续提供本地 TLS／种源；部署正确性必须由真实包内下载验证。

## 2026-11-18：显式代理路由与凭据生命周期

领域模型保留兼容的无凭据代理地址，使用统一解析器验证协议、主机、IPv6、端口和凭据长度。HTTP／FTP 共用私有配置帮助函数：空地址显式直连，非空地址显式取消环境绕过，代理认证与源站认证分开。SOCKS5 目标 DNS 经代理；代理自身域名可由系统解析。HTTPS 代理保留证书验证，主动 FTP 与代理组合明确拒绝。

BT 工厂按不可变端点及内存凭据身份共享活动会话，不在任务间修改全局网络设置；闲置会话释放，过期路由键在下次获取或工厂销毁时移除。代理模式关闭 UDP 发现、uTP 与自动端口映射，不支持 HTTPS 代理。数字 IP 代理端点有额外的五秒可取消 TCP 连通性检查；主机名沿用原生解析器，不把操作系统 DNS 延迟算作已保证的五秒上限。源站或单个节点失败不使整个共享路由失效。

schema 5 只追加需要补充代理凭据的标记，升级前备份。配置校验允许缺少秘密的暂停／导入任务；实际适配器在联网前检查凭据。用户名和密码不写入任务库、清单、日志或辅助进程参数。标记约束凭据可用性，不强迫服务器选择某种认证方法。媒体认证代理在私密输入通道验收前明确失败。

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

## 媒体隔离与恢复边界

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

Weekly queue windows use explicit IANA time zones and a pure local-minute predicate. The scheduling actor obtains one UTC instant through an application port; the QtCore adapter converts that instant to civil time, so DST gaps have no instants and repeated minutes follow the same predicate twice. Overnight and equal-endpoint 24-hour windows belong to their starting weekday. Missing zones hold closed without a local/UTC fallback. A per-UTC-minute cache is invalidated on forward or backward clock changes and policy updates. Window closure preserves Run intent, while manual pause and source authorization retain their existing authority. Schema 8 appends window policy to queue CAS records. Desktop global limits are supplied before the coordinator starts; widget construction is not an admission gate. This clock port does not replace the credential/session security clock.

Queue membership is trusted task metadata independent of source owner, context, proxy, grants and recovery identity. The existing coordinator owns a focused queue registry; queues add no worker coordinators or database connections. The shared SQLite backend exposes a queue facade alongside task/context facades. Migration 7 installs the default queue and persists membership and Run/Paused intent without moving task files or changing old state ordinals.

Queue configuration uses revision checks. Moving an inactive selection validates every member and conditionally updates all queue IDs and timestamps in one transaction; any mismatch rolls back the batch. Task owners, route snapshots, credentials and paths remain unchanged. The default queue cannot be removed, and a named queue must contain no task records before removal. Browser drafts and interchange files cannot choose queue policy or membership. Existing scoped submissions and imports use the default queue; trusted desktop controls can subsequently move inactive tasks.

The actor admits work in queue turns, preserving priority within a queue and intersecting global and queue active limits. Persistent manual intent distinguishes a user pause from queue policy waiting. Disabling a queue cooperatively pauses its running work; enabling it reopens only eligible policy-held Run tasks, preserving retry and one-shot gates. Completed, failed, revoked and manually paused work does not start through queue enable. Startup retains safe recovery semantics.

Rates are allocated across global, queue and task caps with unused-cap redistribution. Media helpers retain their actual launch reservation until exit; incompatible reductions return an ordinary pause-first error. Settings await that result before updating saved preferences. Finite exhausted allocations must never become the existing zero/unlimited control value. HTTP reduces live handles when a tiny positive allocation cannot fund one byte per handle; the durable extents remain pending. These are bandwidth controls, separate from the later traffic ledger and payload allowances. Completion notification and power countdown also check unfinished work, including tasks that have never been active.
