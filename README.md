# bilive

bilive 是原生 OBS 插件，把 B 站扫码登录、直播间管理和开关播放进 OBS 停靠窗口，并提供 PK、视频连线、直播姬场景转换和 HTTP 自动化。

## 安装

使用与 OBS 版本匹配的插件包。退出 OBS 后解压并安装。

| 平台 | 安装位置 |
| --- | --- |
| macOS | 将 `bilive.plugin` 放入 `~/Library/Application Support/obs-studio/plugins/` |
| Windows | 将 `bilive` 文件夹放入 `%ProgramData%\obs-studio\plugins\` |
| Linux | 将 `bilive` 文件夹放入 `${XDG_DATA_HOME:-$HOME/.local/share}/obs-studio/plugins/` |

启动 OBS，打开「停靠窗口 → bilive · B 站开播」，扫码登录后即可管理房间并将推流设置导入 OBS。使用 PK 或视频连线前，先开始 OBS 推流。Linux 保存账号需要桌面 Secret Service 和 `secret-tool`。

## 构建

面向 OBS 33.0.1，使用 Python 3.13、CMake、Ninja 和 C++20。Windows 在 VS x64 开发终端运行。Ubuntu 26.04 需要 `qt6-base-dev`、`qt6-base-dev-tools`、`ninja-build` 和 `libobs-dev`。

```sh
python3 tools/platform-build.py .sdk
```

构建包位于 `dist/`。可在 [GitHub Actions](https://github.com/shiftz300/bilive/actions) 查看各平台的构建状态。

基于 [StartLive 1.4.0](https://github.com/Radekyspec/StartLive/tree/1.4.0)，采用 [GPL-3.0-only](LICENSE)。
