# Linux 构建与验收

## 资格范围

原生 Linux 版本与 Windows 共用 C++20／Qt 6 任务管理、传输和持久化层。当前实测环境为 Ubuntu 24.04 x64 宿主上的独立 Ubuntu 26.04 x64 容器，宿主内核 `7.0.0-43-generic`；不代表 Ubuntu 24.04 原生用户态二进制兼容。

完整辅助进程保护要求 Linux **6.9+** 的 pidfd 进程组信号能力。SDK 实测 GCC 15.2、CMake 4.2.3、Qt 6.10.2、curl 8.18.0、SQLite 3.46.1、libxml2 2.15.2、libsecret 0.21.7、OpenSSL 3.5.5；独立 BT 库使用固定的 2.1.2。最低源码契约为 Qt 6.8、libxml2 2.14 及支持所需协议的 curl，不能假设旧发行版开发包满足要求。

## 构建与完整测试

在匹配用户态中准备 `.github/workflows/linux.yml` 列出的开发包。不要为构建升级已有业务宿主。以下源码命令在仓库根目录执行，运行包不包含构建脚本。以普通用户运行：

```sh
python3 -m venv out/python-linux
out/python-linux/bin/python -m pip install -r tests/requirements.txt yt-dlp==2026.10.26
export PATH="$PWD/out/python-linux/bin:$PATH"
export PYTHON="$PWD/out/python-linux/bin/python"
sh scripts/verify-linux.sh
```

脚本构建 Release，执行扩展测试、完整 CTest 及真实媒体专项。`scripts/linux_test_session.py` 使用临时 HOME、会话总线和加密测试钥匙串，不复用用户的凭据库；它拒绝 root。容器内用 `tini -s --` 包裹命令以回收脱离的测试后代。

可指定 `MASKPILOT_BUILD_DIR` 选择新构建目录。离线 BT 源码通过 `sh scripts/verify-linux.sh -DFETCHCONTENT_SOURCE_DIR_MASKPILOT_TORRENT=/absolute/source` 提供；源码必须对应已核对的固定依赖。已安装兼容 BT 库时，打包还需设置 `MASKPILOT_TORRENT_NOTICES_DIR`，包含匹配的完整 `LICENSE` 与 `COPYING`。

## 本地验收包

```sh
python scripts/package_linux.py --build-directory out/build/linux-gcc-release \
  --output-directory out/package/MaskPilot-0.3.0-linux-x64-local
```

包采用相邻 `lib/` 下的 BT 运行库和系统 Qt／传输依赖，保留完整依赖声明。它不是跨发行版通用包、安装器或正式签名发行版。暂存目录、归档或摘要已存在即拒绝，失败现场保留。清单记录实际文件／内部链接、源码摘要和可确认的 Git 状态；Git-less 快照的声明提交不等于已核验提交，摘要也不证明发布者身份。

在匹配系统运行 `./maskpilot`；CLI 与 `media_guard/` 保持相邻。无需媒体组件即可使用原生文件下载、BT 和有界直播；可选点播解析／合并组件由系统安装或用户明确选择，Linux 不下载 Windows 组件。记住代理凭据需要已解锁的 Secret Service；服务不可用时明确失败，没有明文回退。

Ubuntu 26.04 对应的系统运行包为：

```sh
sudo apt-get install libqt6widgets6 libqt6network6 libqt6svg6 qt6-qpa-plugins \
  libxcb-cursor0 libcurl4t64 libsqlite3-0 libxml2-16 libsecret-1-0 libssl3t64 fonts-noto-cjk
```

这组包名不适用于旧发行版；系统管理的依赖及其许可证／源码由发行版渠道提供。Secret Service 的用户会话和已解锁密钥环需由桌面环境提供。关机动作使用系统会话授权，本轮不执行真实关机。

## 浏览器手动配置

Linux 尚无安装／卸载器。以下是普通用户默认目录的开发配置，未代替真实浏览器／企业策略验收。原生主机路径、Chrome 用户级目录及 Firefox 清单规则依据 [Chrome 原生消息文档](https://developer.chrome.com/docs/extensions/develop/concepts/native-messaging) 和 [Mozilla 清单文档](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/Native_manifests)。自定义浏览器资料目录、沙箱包及策略可能改变查找方式。

在解压后的、当前用户拥有的程序目录中执行；三项文件只要已有一项便先退出，不覆盖旧配置：

```sh
umask 077
python3 - <<'PY'
import json
from pathlib import Path
root = Path.cwd().resolve()
assert (root / 'maskpilot').is_file() and (root / 'maskpilot_native_host').is_file()
home = Path.home()
chrome = home / '.config/google-chrome/NativeMessagingHosts/com.maskpilot.downloads.json'
firefox = home / '.mozilla/native-messaging-hosts/com.maskpilot.downloads.json'
config = root / 'native-bridge.json'
paths = (config, chrome, firefox)
assert not any(path.exists() or path.is_symlink() for path in paths), 'Existing configuration retained'
origin = 'chrome-extension://epdfgbhpbpkpephcfmemmadlblpodnfo/'
gecko = 'downloads@maskpilot.invalid'
host = dict(name='com.maskpilot.downloads', description='MaskPilot downloads',
            type='stdio', path=str(root / 'maskpilot_native_host'))
values = (
    dict(allowedOrigins=[origin], allowedFirefoxExtensions=[gecko],
         firefoxManifestPath=str(firefox), desktopPath=str(root / 'maskpilot'),
         dataDirectory=str(home / '.local/share/maskpilot-downloads')),
    dict(host, allowed_origins=[origin]),
    dict(host, allowed_extensions=[gecko]),
)
for path, value in zip(paths, values):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    with path.open('x', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')
PY
```

Chrome 开发者模式加载 `browser_extension/chromium/`；Firefox 临时加载 `browser_extension/firefox/manifest.json`。扩展编号须与清单一致。启用此例后，手动启动桌面也使用 `./maskpilot --data-dir "$HOME/.local/share/maskpilot-downloads"`，保证浏览器与桌面连接同一个下载区。Firefox 的清单路径／扩展编号／目标程序绑定由原生主机再次检查。只撤销自己建立且内容未改的配置，不删除浏览器资料或下载数据。

## 验收记录

原生功能修复提交为 `a0a5e37`，本轮使用独立构建和测试目录，早期失败／修复日志均保留。

| 检查 | 当前证据 |
|---|---|
| Linux 完整入口 | `sh scripts/verify-linux.sh` 已通过：单次 CTest **309/309**（670.13 秒）、扩展 **48/48**、媒体专项 **14 组**；其中包含实际部署包、原生密钥环和进程生命周期测试 |
| Windows 保留 | 独立全量 **323/323**；此后补修对应测试 **10/10**，原生自动启动与分段夹具分别追加 **1/1** 通过，保留其失败反例 |
| 资产保护 | `AGENTS.md` 与原先 2,460 个受保护文件摘要一致；服务器原先 25 个业务容器身份一致，测试容器仅挂载本轮专属目录、无公开端口 |
| 图形 | Xvfb 中实际启动并检查 1280×860 中文 Qt 界面；不等于实体桌面／GPU 资格 |
| 短时负载 | 已安装程序的 300 秒 HTTP 配置通过：实际负载 304.86 秒、44 轮、4,400 个独立摘要校验文件、106 次暂停恢复、活跃峰值 8；桌面通信 P95 为 4.20 ms，采样私有内存最高 50.90 MiB、RSS 62.03 MiB，进程与测试文件已清理 |
| CI | 新增原生 Linux 定义并核对 YAML／Shell；同一完整验证入口已在服务器运行通过，未触发远端托管 CI |

私有原始日志位于源码工作区 `out/qa/linux-port-20261120/`，核心入口日志为 `linux-final-verification.log`。Linux 包中的 `linux-build-info.json` 和 `manifest.json` 分别记录实际运行库契约与字节／来源摘要。原测试服务器资源上限为 1 CPU、1,536 MiB 内存、单构建／测试作业，业务宿主未升级。

本轮 Windows 工作区与 Linux 快照的原生源码摘要一致：`fd563785f21f39dab9a1b29a9c7f8a7d66e7ebe387cb8e005fc0040f2dad13ab`。源码摘要按相对 POSIX 路径排序，不依赖开发系统的路径大小写排序规则。负载结论仅覆盖本地 HTTP Range，媒体、TLS 和公网环境不据此取得长测资格；最终归档的摘要由相邻 `.sha256` 文件提供。

尚未验收实体桌面 GPU／Wayland、多屏／DPI、生产浏览器策略、其他发行版／架构、macOS 和 8 小时长测。自动更新、正式发行及浏览器工程集成仍是后续范围。
