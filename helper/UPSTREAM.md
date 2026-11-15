# Upstream

The headless business and media layers under `startlive/` reuse StartLive 1.4.0 under GPL-3.0-only. The core worker layer is used without the standalone GUI. A small part of the source tree retains the upstream `PySide` namespace for PK controller imports. It contains plain Python and has no PySide runtime dependency. bilive uses its own settings and native system credential store.

The media capture sources in `third_party/startlive-capture` come from the same release with the OBS module entry points integrated into bilive. The Windows named pipe and POSIX socket names are local to bilive. The original StartLive artwork is absent from the plugin.

[StartLive 1.4.0 source](https://github.com/Radekyspec/StartLive/tree/1.4.0)
