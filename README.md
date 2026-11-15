# bilive

bilive 把 B 站开播和 StartLive 的工作流程放到 OBS 停靠窗口中。界面由 OBS 自带的 Qt C++ 绘制。推流设置、开始和停止输出通过 OBS 前端 API 操作。封装的后台进程提供 StartLive 的 PK 会话管理、RTC、人脸验证编解码和场景转换。

打开入口为「停靠窗口 → bilive · B 站开播」。

| 功能 | 0.2.0 的入口 |
| --- | --- |
| 扫码与账号 | 扫码、保存到系统凭据库、切换账号及退出当前账号 |
| 直播间 | 房间状态、标题历史、分区搜索、标题与公告、封面裁剪上传、B 站推流延迟 |
| 开关播 | RTMP / RTMPS / SRT、自动导入 OBS、可选自动推流、人脸验证二维码与状态轮询 |
| PK / 连线 | 经典 PK、视频 PK、视频连线、搜索主播、邀请、连战、静音、编码检测与裁剪设置 |
| 场景转换 | 发现直播姬场景、预览转换结果和不兼容项、导出 OBS 场景集合 |
| 设置 | 代理、推流偏好、停靠窗口字体、状态记录以及 HTTP 自动化 |

PK 前先开始 OBS 推流。窗口可显示本地和对端预览。需要把对端音视频交给 OBS 时可选「添加对端源」，再用 OBS 调整场景、混音与监听。完整的 B 站连线、硬件编码器以及混音回路仍需登录后的实测。

账号凭据进入 macOS 钥匙串或 Windows 凭据管理器。Linux 使用桌面的 Secret Service，通常由 `libsecret-tools` 提供 `secret-tool`。凭据服务不可用时账号保留到本次退出。自动推流和 HTTP 监听首次都关闭。独立版的托盘、背景和更新界面由 OBS 的界面与插件安装流程承接。

可选 HTTP 自动化保留 `POST /api/startLive` 和 `POST /api/stopLive`。启用后从设置复制令牌，请求带上 `Authorization: Bearer <令牌>`。默认只接受本机连接。返回 `202` 表示插件已接收操作，实际结果在停靠窗口显示。绑定到局域网地址时，HTTP 内容由网络传输。

## 安装和验证范围

使用与目标 OBS 版本匹配的插件包。退出 OBS 后安装，重新启动再打开停靠窗口。

| 平台 | OBS 33 安装位置 |
| --- | --- |
| macOS | 将 `bilive.plugin` 放到 `~/Library/Application Support/obs-studio/plugins/` |
| Windows | 将压缩包内的 `bilive` 文件夹放到 `%ProgramData%\obs-studio\plugins\` |
| Linux | 将压缩包内的 `bilive` 文件夹放到 `${XDG_DATA_HOME:-$HOME/.local/share}/obs-studio/plugins/` |

本次生成的包适用于 Apple Silicon、macOS 14+、OBS 33.0.1 使用的 Qt 6.11.1。已通过编译、签名检查和离线模拟测试。Windows x64 和 Linux x64 有对应的 SDK 与打包流程，尚无运行结果。实际扫码开播、PK 连线及 UI 运行也尚未实测。

## 构建

需要 CMake、Ninja、C++20 和匹配目标 OBS 的 Qt SDK。只运行协议测试时使用普通 Qt SDK。

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/qt
cmake --build build
ctest --test-dir build --output-on-failure
```

OBS 33.0.1 的打包入口在三平台共用。Windows 在 Visual Studio x64 Developer PowerShell 运行。Ubuntu 26.04 安装 `qt6-base-dev`、`qt6-base-dev-tools`、`ninja-build` 和 `libobs-dev`。macOS 可用 `--obs-app /Applications/OBS.app` 指定已安装的同版本 OBS。脚本会下载相应 SDK 和后台运行时。

```sh
python3 tools/platform-build.py .sdk
```

已有 OBS 和 Qt CMake SDK 的环境可以用 `-DBILIVE_OBS_PLUGIN=ON` 手动配置。用 `tools/helper-runtime.py` 准备 Python 3.13 与依赖，将输出的 `BILIVE_HELPER_RUNTIME` 和 `BILIVE_HELPER_SITE` 传给 CMake，然后 `cmake --install` 打包。如采用源码头文件方式构建，参照 `tools/platform-build.py` 中对应平台的参数。

[StartLive 1.4.0](https://github.com/Radekyspec/StartLive/tree/1.4.0) · [OBS 前端 API](https://docs.obsproject.com/reference-frontend-api) · [OBS 33.0.1 发布](https://github.com/obsproject/obs-studio/releases/tag/33.0.1)
