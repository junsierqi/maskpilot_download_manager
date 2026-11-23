# macOS 构建与验收

## 当前状态

本轮交付原生构建配置、Apple 平台适配和可执行 CI 验收入口；当前开发环境没有 macOS，**真实编译及运行结果待 GitHub 首次执行**。Windows 回归或 Python 模拟用例均不能替代 macOS 资格。暂不承诺 macOS 正式发行、签名、在线更新或完整媒体／浏览器兼容。

## 推送后检查

提交到 GitHub 后，push／pull request 自动触发 `.github/workflows/macos.yml`；也可在 Actions → macOS → Run workflow 手动运行。工作流固定 `macos-15`（arm64）和 `macos-15-intel`（x86_64），启动时校验实际架构，任一失败都需处理。无需 Apple 开发者账号或签名秘密。

每个任务安装 Qt 6.11.3、原生开发依赖和 Python／Node 测试工具，构建全部 CMake 目标。首次拉取固定摘要的 BT 依赖会增加耗时。系统及 Homebrew 依赖的实际版本记录在 `macos-sdk.txt`；这些环境并非正式发行的锁定工具链。

## 自动检查范围

- 原生 C++ GoogleTest：领域、调度、持久化、Qt 控件、BT、摘要、媒体策略及安全边界。
- 本地 HTTP、FTP／FTPS、SFTP、POST、额度、扫描、站点发布、本机消息、浏览器恢复和动态分段的产品路径。
- macOS 专属：Unicode 凭据重开读取、所有者／路由／写入随机量绑定、重复写拒绝及准确删除；锁定钥匙串明确失败；本地种子读取与链接拒绝；目录无覆盖发布；正常停止和强制结束所有者后的子孙进程清理，以及私密输入 EOF。
- 浏览器扩展的 Node 单元测试及临时钥匙串脚本的可移植故障恢复用例。

钥匙串测试只写入自有随机命名空间的假凭据。CI 在全新私有目录创建临时钥匙串，临时替换搜索列表／默认项，结束或异常时恢复并核验原状态，仅删除该临时钥匙串。执行器限制为 GitHub 托管 macOS 环境；不要在个人机器伪造环境变量执行它。任务遭强制取消时由托管虚拟机销毁回收，不能保证进程内 finally 执行。

## 日志与失败处理

在每个任务下载 `macos-<架构>-qualification` artifact，查看 `macos-ci.log`、SDK／链接库记录、CTest `LastTest.log` 和 CMake 配置日志。配置失败先检查 SDK／依赖发现；编译链接失败查看首条诊断；运行失败查看具体用例和动态库路径。将失败步骤或完整日志交回维护者，修复后两种架构均须重跑。

通过后的 `*-unpackaged.tar.gz` 只是供检查的未打包程序，仍依赖构建机 Qt／原生库；不是可直接分发的 `.app`、DMG 或独立安装包。失败早于构建时可能只有部分诊断文件。

## 原生机器手动构建

安装 Xcode 命令行工具、Qt 6.11.3 和 `cmake ninja pkgconf boost curl libxml2 sqlite googletest openssl@3` 开发包；`QT_ROOT_DIR` 指向与本机架构兼容的 Qt SDK：

```bash
python3 -m venv out/python-macos
out/python-macos/bin/python -m pip install -r tests/requirements.txt
export CMAKE_PREFIX_PATH="$QT_ROOT_DIR;$(brew --prefix curl);$(brew --prefix libxml2);$(brew --prefix sqlite);$(brew --prefix googletest);$(brew --prefix boost)"
export OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --preset macos-clang-release -DPython3_EXECUTABLE="$PWD/out/python-macos/bin/python"
cmake --build --preset macos-clang-release
QT_QPA_PLATFORM=offscreen ctest --preset macos-clang-release -R '^(Checksums\.|macos_ci_fixture$)' --output-on-failure
```

完整 CI 检查由 `scripts/verify-macos.sh` 编排，并包含仅限托管 CI 的钥匙串会话。手动命令只运行不写钥匙串的子集；不要在个人钥匙串中直接运行完整凭据测试。

## 后续资格

两架构 CI 全绿后仍须独立验收原生 UI／字体／DPI／托盘、真实浏览器安装与权限、可选解析器／封装器、真实媒体录制、长时间负载、睡眠唤醒及干净系统部署。当前工作流不安装可选媒体组件，也不执行依赖它们的完整媒体产品用例；原生单元覆盖不代表这些外部组件已经在 macOS 上验收。正式打包、签名和公证延后。
