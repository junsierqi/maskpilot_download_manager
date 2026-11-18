# 验证场景

常规回归运行 `scripts/verify.ps1 -Configuration Release` 和 `npm.cmd --prefix browser_extension test`。CTest 中的调度器控制桩测试为 fixture-only；本地 HTTP、原生主机、真实组件和浏览器测试调用实际产品二进制。

| 场景 | 命令／证据 |
|---|---|
| 原生文件故障矩阵 | CTest `local_http_product_path`：Range、降级、空／未知长度、身份变化、重定向清除凭据、代理、登录、重试、重名和 SHA 失败 |
| FTP/FTPS | CTest `ftp_product_path`，或 `python tests/support/ftp_fixture.py --exercise <cli.exe>`：实际匿名／认证、主动／被动、显式／隐式 TLS、CA／主机名拒绝、摘要、远端变化、REST 回退及强制退出恢复；只对受控本地服务器作保证 |
| 原生代理 | CTest `proxy_product_path`，或 `python tests/integration/proxy_test.py --exercise <cli.exe>`：记录实际 HTTP CONNECT／SOCKS5 转发与认证，验证远端 DNS、环境绕过污染、错误／缺失凭据、代理不可用不直连、FTPS 两种 TLS 模式及强制退出后的凭据标记 |
| BT 代理隔离 | CTest `TorrentProxy.*`：实际元数据与分片通过代理，直连／不同端点／同端点不同凭据并发隔离；坏节点与坏 Tracker 不妨碍有效备用源和同路由其他任务 |
| BT／磁力 | `ctest --test-dir <build-dir> -R TorrentEngine --output-on-failure`：本地真实种源、v1/v2/混合种子、磁力、纯 Tracker、选择、恢复、并发、做种、限速、发布冲突、实际 CLI 与 SQLite。禁用公开发现，不访问公共种源 |
| 强制退出恢复 | 同一实际 CLI 进程被终止，读取持久化 offset，再续传并比对完整文件；不是仅测分段计算 |
| 大文件 | `python tests/support/http_fixture.py --exercise out/build/windows-msvc-release/maskpilot_cli.exe --large`，实际下载 4 GiB+257 字节并验证 SHA-256 |
| 原生消息 | CTest `native_bridge_product_path`：来源／大小限制、持久化后 ACK、恢复、记录移除保留最终文件 |
| 媒体 | `python tests/integration/native_host_test.py <host.exe> <desktop.exe> --media-parser <parser.exe> --media-muxer <ffmpeg.exe>`；实际 MP4、HLS、DASH、音频提取及登录头拒绝 |
| 浏览器实装 | Node ≥22：`node tests/system/browser_test.mjs --browser <test-chromium.exe> --build-directory <build-or-package-dir>`；独立 profile，备份并恢复专属原生主机注册项 |
| 独立部署 | `scripts/smoke.ps1 -PackageDirectory <dir>` 移除开发 PATH，运行桌面／CLI／原生消息；仍不等于一台全新虚拟机 |
| 稳定性 | `python benchmarks/run.py --help` 查看固定预算；短运行不代替 8 小时门槛 |
| 压力报告可靠性 | CTest `stress_report_publication_failure`：实际 Windows 读取占用下的原子报告发布，以及真实桌面负载结束时注入报告失败；错误必须保留失败状态和诊断材料。用例位于 `tests/system/stress_report_test.py` |

某些自动化测试浏览器环境无法启用沙箱，只有明确传入 `--allow-unsandboxed-fixture` 才关闭测试实例沙箱；该结果不证明生产沙箱兼容，产品启动不使用此参数。

测试隔离应用数据、下载目录和本地服务。正常测试不修改实际用户下载或浏览器 profile。禁止实际触发关机来验证倒计时。截图须使用原生 Windows 渲染；offscreen 平台适合控制测试，不能证明字体／DPI 视觉正确。

FTP TLS 夹具每次生成短期证书，不修改系统证书库。测试环境需提供 Python `cryptography` 或 PATH 中的 OpenSSL 命令；缺少两者时测试失败，不静默跳过 TLS。它们仅用于测试，产品运行不需要 Python 或 OpenSSL 命令行程序。

BT 种源正常校验全部测试数据，不使用跳过校验的种源模式。回环端口同时探测 TCP／UDP，避免 Windows 两种协议保留区不同；绑定失败只在原准备时限内重试，不延长传输断言。桌面文件对话框测试操作实际文件名输入框，检查选择路径，并保留阻塞诊断时限；不依赖有焦点时可能被忽略的 `selectFile()`。

混合压力测试先在 `out/qa/media-fixtures/` 准备自有小型 `source.mp4`，或用以下命令生成合成样例。默认 300 秒只是短检查，完整计时传入 `--duration 28800`：

```powershell
New-Item -ItemType Directory -Force out/qa/media-fixtures
ffmpeg -y -f lavfi -i testsrc2=size=160x90:rate=10 -f lavfi -i sine=frequency=440 -t 3 -c:v libx264 -c:a aac out/qa/media-fixtures/source.mp4
python benchmarks/run.py --desktop out/package/MaskPilot-0.1.0-windows-x64/maskpilot.exe --native-host out/package/MaskPilot-0.1.0-windows-x64/maskpilot_native_host.exe --duration 28800 --media-parser out/tools/yt-dlp.exe --media-muxer 'C:/Tools/ffmpeg/bin/ffmpeg.exe'
```

预算在运行前固定：桌面进程采样私有／工作集 <512 MiB、首尾季度私有内存漂移 <64 MiB、GUI IPC p95 <200 ms，观察间隔不能超过 30 秒。输出为增量 JSON，包含二进制哈希和复现命令。外部组件／夹具进程内存不在桌面预算内，结果不宣称外部站点、TLS 或所有可见 UI 操作的压力表现。脚本默认 offscreen；原生视觉验收单独执行。

原生界面可用 `maskpilot.exe --data-dir <isolated-dir> --window-size 850x560 --screenshot <file.png>` 捕获；截图使用 Windows 平台，弹出对话框时捕获当前模态窗口。示例任务只用于布局检查，不作为传输成功证据。

包内协议复测可设置 `MASKPILOT_TEST_CLEAN_CLI_ENV=1`，让 FTP 夹具及 BT 实际 CLI 用例仅清理产品子进程的 PATH／Qt 环境；BT 用例另以 `MASKPILOT_TEST_CLI` 指向包内客户端。父进程的 TLS／种源环境保持完整。

做种上传夹具使用两个回环地址区分原种源与接收端，避免把同一 IP 下的多个本地进程误当成独立远端节点。保留原有 60 秒期限、上传限速与完整数据校验；失败时输出监听及连接诊断。浏览器凭据审计包含所有 SQLite 列和二进制字段，并直接检查数据库及其日志文件。

发布前额外人工／环境验收：Windows 10/11 干净系统、不同 DPI、多显示器、休眠唤醒、受限磁盘与不可写目录、真实 Chrome/Edge 安全策略。没有运行的项目必须留在验收记录中。

## 上下文原生消息验证

`ctest --test-dir <build> -R scoped_native_bridge_product_path --output-on-failure --no-tests=error` 调用实际桌面与 host。夹具验证独立管理权限、两个会话、请求去重、跨上下文拒绝、旧客户端隔离、撤销／重新签发／重启及真实代理和 Cookie 路由。大历史数据只在夹具所属桌面关闭时写入；运行时不另起数据库写入器。此夹具仅使用进程内代理凭据，安全保存的实际控件验收另列，不能据此推断 UI 或浏览器侧签发已完成。
