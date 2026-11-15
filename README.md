# bilive

bilive 是一个在 OBS Studio 中管理哔哩哔哩直播的原生 Qt C++ 停靠窗口。界面使用 OBS 自带的 Qt。推流配置和启停通过 OBS 前端 API 调用。

当前目标是移植 StartLive 1.4.0 的功能。这个版本完成了核心流程，测试使用模拟响应。

| 功能 | 当前状态 |
| --- | --- |
| 扫码登录 | 生成二维码、轮询、取消、过期处理和会话登录 |
| 直播间 | 读房间状态和开播设置，修改标题、分区、公告 |
| 开关播 | B 站接口、RTMP/RTMPS 和 SRT 选择、可选自动推流、复制密钥 |
| 人脸验证 | 识别两种响应。服务端返回二维码时可在手机上验证后重试 |
| 后续移植 | 持久化多账号、新版人脸验证交互、封面上传和裁剪、直播延迟、PK 及音视频连线、配置转换、局域网自动化与其他设置 |

账号会话保存在内存里。重新启动 OBS 后需要重新扫码。启动推流的复选框默认关闭。导入时使用当前 OBS 配置，既有推流期间禁用导入。

## 本地构建

原生插件的目标平台是 macOS、Windows 和 Linux。当前仅用 Apple Silicon 和 OBS Studio 33.0.1 的 Qt 6.11.1 SDK 完成编译。构建插件时应使用与目标 OBS 匹配的 OBS 和 Qt SDK。Windows 和 Linux 的安装布局已在 CMake 中定义，尚待实机构建和加载验证。

协议核心和模拟测试只依赖 Qt Core、Qt Network、CMake 和 C++20。

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/qt
cmake --build build
ctest --test-dir build --output-on-failure
```

有 OBS CMake SDK 的环境可加 `-DBILIVE_OBS_PLUGIN=ON`，并让 CMake 查到 `libobs` 和 `obs-frontend-api`。测试过的 macOS 本地构建组合也可直接使用已安装的 OBS 和同版本仓库头文件。

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=/path/to/obs-qt-sdk \
  -DBILIVE_OBS_PLUGIN=ON \
  -DBILIVE_OBS_SOURCE=/path/to/obs-studio-33.0.1 \
  -DBILIVE_SIMDE_INCLUDE=/path/to/simde-0.8.2
cmake --build build
```

将构建的 `bilive.plugin` 放进 `~/Library/Application Support/obs-studio/plugins/`，重启 OBS 后从「停靠窗口 → bilive · B 站开播」打开。预览包使用本地临时签名，适用于同一个 Qt ABI 的 OBS。实际登录、开播和应用内加载仍待验证。

协议字段和签名实现参考 StartLive 1.4.0。随附的 `resources/livehime.json` 是同一上游标签中的接口配置。它可能随 B 站接口变化而需要更新。第三方 QR 编码由 Project Nayuki 的 MIT 授权 C++ 实现提供。

[StartLive 1.4.0](https://github.com/Radekyspec/StartLive/tree/1.4.0) · [OBS 前端 API](https://docs.obsproject.com/reference-frontend-api) · [QR Code generator 1.8.0](https://github.com/nayuki/QR-Code-generator/tree/v1.8.0)
