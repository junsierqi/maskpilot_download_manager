# 浏览器与本机协议

本文描述当前开发代码。已冻结的 0.2.0 安装包仍使用原协议，不据此推断旧包具备新能力。浏览器扩展默认关闭自动接管与 Cookie 携带；当前扩展仍是 v1 独立下载区客户端。

## 帧与身份

原生消息和本机管道均为 4 字节小端无符号长度，随后为 UTF-8 JSON。单帧最大 1 MiB，一个连接处理一个请求。`requestId`、`taskId`、上下文编号和会话编号使用 32 位小写十六进制；字段有类型、长度和命令白名单检查。

桌面管道仅允许当前用户访问。每次启动生成不同的 `ipc.token` 与 `ipc-admin.token`：前者只服务 v1 独立下载区，后者只授权可信本机管理端。普通扩展 host 不读取管理 token。v2 普通客户端使用管理端为一个上下文签发的 `sessionToken`，验证失败直接拒绝，不转用独立下载区或管理权限。扩展来源注册只证明扩展身份，不证明浏览器配置归属。

没有公开本地 HTTP 控制端口。管理 token、会话和授权 Cookie 不写入任务清单或诊断；Windows 系统凭据库与管道的边界是当前登录用户，不隔离以同一用户运行的恶意进程。

## v1：独立下载区

请求和响应示例：

```json
{"version":1,"requestId":"0123456789abcdef0123456789abcdef","command":"prepare","url":"https://example.test/file.zip","filename":"file.zip"}
{"version":1,"requestId":"0123456789abcdef0123456789abcdef","ok":true,"taskId":"0123456789abcdef0123456789abcdef"}
```

`ping` 检查连接；`list` 返回最多 1000 条独立下载区摘要；`add` 新建任务；`prepare` 持久化暂停任务；`resume`、`pause`、`cancel`、`remove` 使用 `taskId`。不能查看或控制其他上下文。移除只清理历史与专属缓存，保留最终文件；活动任务须先暂停或取消。

新增字段为 `filename`、`referrer`、`cookie`、`userAgent`、`media` 和 `audioOnly`，只接受 HTTP/HTTPS。目的目录、路由和媒体组件来自可信桌面设置，浏览器不能指定任意路径、代理或可执行文件。媒体目前拒绝 Cookie、Authorization 和自定义请求头，公开点播恢复时重新获取。

## v2：普通上下文客户端

通过原生 host 调用时，`sessionToken` 仅用于传输认证；host 将它转换为桌面管道认证信息，不会代入管理 token。示例中的值均为占位编号：

```json
{"version":2,"requestId":"0123456789abcdef0123456789abcdef","sessionToken":"11111111111111111111111111111111","command":"prepare","params":{"url":"https://example.test/file.zip","filename":"file.zip"}}
```

| 命令 | `params` | 结果 |
|---|---|---|
| `ping` | 空对象 | 当前会话有效 |
| `prepare` | `url`、`filename`、可选 `originGrantId` | 暂停任务的 `taskId` |
| `add` | 同上，可选 `paused` | `taskId` |
| `list` | 可选 `limit`（1–1000）、`cursor` | `tasks`、`nextCursor` |
| `resume`／`pause`／`cancel`／`remove` | `taskId` | 接受控制后的任务编号 |

来源目前为 HTTP/HTTPS 或磁力链接；更丰富的媒体选项另行扩展。客户端不能传入上下文编号、Cookie、代理、凭据引用、目的目录或工具路径。归属由会话决定。列表先过滤归属，再分页，只包含编号、文件名、状态与字节数。外部任务编号和不存在的编号使用相同拒绝结果。

相同上下文、相同请求编号的重试返回已持久化任务；不同上下文可独立使用相同请求编号。重试不替换原任务的路由或授权；刷新使用显式管理操作。

## v2：可信本机管理端

可信 Browser Bridge 可启动 `maskpilot_native_host.exe --broker --data-dir <绝对应用数据目录>`，经标准输入／输出传递上述二进制帧。不是直接传文本 JSON。凭据放在帧内，不放进命令行参数。普通浏览器来源调用或 JSON 字段不能切换为此模式。

| 命令 | 参数与结果 |
|---|---|
| `context.configure` | `externalKey`、`label`、`downloadRoot`；可选 `proxyEndpoint`、`proxyAuthenticationRequired`、`originAuthenticationRequired`、`proxyCredentials:{username,password}`、`rememberProxy`。更新另需 `id` 与 `expectedRevision`；恢复撤销上下文须显式 `reactivate`。返回安全上下文信息 |
| `context.list` | 返回上下文信息，保存状态用 `hasStoredCredentials` 表示，不返回系统凭据引用／所有权 nonce |
| `context.issue` | `contextId` → `sessionToken` |
| `origin.grant` | `contextId`、精确 `url`、`cookie`／`authorization`、`expiresAt` → `originGrantId`；有效期最长五分钟 |
| `context.revoke` | `contextId`；禁用持久化后停止所属工作，退出并清理凭据后才确认 |
| `task.rebind` | `taskId`、可选 `originGrantId`；仅重新绑定非活动、未结束任务，保留目标文件位置 |

配置修改使旧客户端会话失效，新任务使用新配置；已接受任务保留原路由，直到显式重新绑定。会话本身不跨进程重启存活。撤销不删除最终文件，清理失败仍保持禁用并报告未完成。普通会话过期不等于撤销已接受的后台下载。

源站授权必须由可信 Browser Storage／Bridge 按 URL、Domain、Path、Secure 与分区规则筛选。下载器只检查授权的上下文、精确 URL 和期限，不读取浏览器私有数据库。携带精确 URL 授权的任务遇到改变地址的重定向时暂停并要求新授权；重启后缺少授权不会自动匿名下载。v1 独立任务收到 Cookie／认证头时也保留源站认证需求标记，并拒绝任何改变 URL 的重定向（包括同源）。实际浏览器端签发仍须其独立工程验收，不能用本地夹具代替。

## 接管与恢复

1. 暂停浏览器下载。
2. `prepare` 验证并持久化暂停任务后才确认。
3. 取消浏览器原下载，再发送 `resume`。
4. 准备失败则恢复浏览器；取消原下载失败则取消原生暂停任务并恢复浏览器。原生恢复失败时保留持久化任务并明确提示。

控制确认表示协调线程已接受状态变更，传输终态通过 `list` 查询；撤销管理命令另有工作退出与清理屏障。没有运行的 UI、生产浏览器策略、商店安装或实际指纹浏览器集成不能声明已验收。
