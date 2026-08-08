# Quote/0 Baby Dashboard

[中文介绍与使用教程](docs/README.zh-CN.md) | English

[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-5b7f63.svg)](LICENSE)
[![Release](https://img.shields.io/github/v/release/thursdaycapital/hermes-studio-baby-dashboard)](https://github.com/thursdaycapital/hermes-studio-baby-dashboard/releases)

一个开源、本地优先的 Quote/0 墨水屏宝宝照护看板。手机通过局域网记录
喂奶、睡眠和尿布，设备同步显示状态、趋势与提醒，不依赖官方云服务。

## 项目状态：公开 Beta / Project status: Public Beta

> 这是社区开发的独立开源固件，不是 MindReset 官方产品，也不是医疗器械。

当前版本适合少量愿意测试、能够自行刷机或由熟悉 ESP32 的朋友协助安装的
用户，尚不建议在缺少技术支持的情况下大规模分发给普通家庭。使用前请了解：

- 原厂设备第一次安装需要通过 USB 执行完整的 `idf.py flash`；Release 中的
  `quote0_baby.bin` 只适合已安装本项目后的 OTA 升级。
- 改刷前应自行备份原厂固件；本仓库不分发原厂镜像，也不保证可以恢复原厂服务。
- 当前恢复热点使用公开默认密码 `baby1234`，局域网令牌由设备 MAC 推导，
  因此只能在可信的家庭网络中使用，不应暴露到公网或访客网络。
- 所有设备默认使用 `quote0-baby.local`。同一局域网内运行多台设备可能出现
  主机名冲突，Hermes 自动发现也可能选错设备。
- 设备端仅保存最近 32 条历史记录；请定期导出 JSON，并妥善保管宝宝数据。
- 当前主要在一台 Quote/0 真机上验证，尚未覆盖所有硬件批次、路由器和长期运行场景。
- 看板仅用于日常照护记录，不能替代儿科医生、专业喂养建议或紧急医疗服务。

English summary: this is independent public-beta firmware for technical early
adopters. First installation requires a full USB flash, security assumes a
trusted private LAN, one device per LAN is recommended, only 32 device records
are retained, and hardware/router coverage is still limited. Back up the
factory firmware and baby data before use. Do not use the dashboard for medical
diagnosis or emergency decisions.

![Quote/0 宝宝看板实机运行效果](docs/quote0-baby-dashboard.jpg)

### 局域网网页看板 / Local web dashboard

手机或电脑连接同一 Wi-Fi 后，可以记录喂奶、睡眠和尿布，查看历史记录与
多日趋势，设置喂奶及疫苗提醒，并导出或恢复本地数据备份。

![Hermes Studio 宝宝看板与数据备份界面](docs/web-dashboard.webp)

Local-first custom firmware for the MindReset Quote/0:

- ESP32-C3, 4 MB flash
- 152x296 monochrome e-paper
- UC8251D display controller, selected from the original NVS setting
- Official MindReset GPIO mapping and Apache-2.0 display driver

The current build renders a four-row newborn dashboard and stores its state in
NVS. It accepts local USB commands for feeding, sleep, diapers, reminders, and
the baby's day number. `hermes_bridge.py` exposes those commands as an MCP
server for Hermes Studio; no official cloud service is used. The day number is
anchored when `DAY` is set and advances automatically at midnight in Shanghai
time after the device synchronizes its clock over Wi-Fi.

The original factory image and all personal baby data are intentionally excluded
from this repository.

## Hermes history and summaries

Hermes MCP writes successful feeding, diaper, and sleep records to the local
`baby_history.sqlite3` database beside `hermes_bridge.py`. Feeding records also
set the screen's next feeding time automatically. The default interval is three
hours and can be changed from a Hermes conversation.

Additional MCP tools provide today's totals, recent history, configurable feed
intervals, and undo for the most recent local record. The database stays on the
Mac and is not uploaded to an official or third-party cloud.

Date-specific vaccination reminders are supported with:

```text
REMIND 08-08 09:00 SHOT
```

Hermes accepts a full `YYYY-MM-DD` date and converts it to the compact date
shown on the e-paper display.

## USB commands

```text
FEED 20:33 80
DIAPER 21:10 W
SLEEP 21:30 ON
NEXT 23:30 FEED
DAY 9
STATUS
SHOW
```

State-changing commands refresh the display and persist the latest values in
NVS. The bridge can also be used directly:

```bash
python3 hermes_bridge.py STATUS
python3 hermes_bridge.py FEED 20:33 80
```

Hermes can be configured with the `quote0-baby` MCP server and the bundled
skill. Start a new Hermes Studio conversation after installation so the tools
are included in the session.

Before using Wi-Fi commands, store the device token locally. This file is
ignored by Git:

```bash
printf '%s\n' 'YOUR_DEVICE_TOKEN' > .quote0-token
```

Alternatively, set `QUOTE0_TOKEN` in the environment.

## Build

Clone the display-driver dependency and build with ESP-IDF:

```bash
git clone --recurse-submodules https://github.com/thursdaycapital/hermes-studio-baby-dashboard.git
cd hermes-studio-baby-dashboard
idf.py build
```

The project targets ESP32-C3 with a 4 MB dual-OTA partition table.
The `quote0_baby.bin` asset from GitHub Releases is an OTA application image;
use a full `idf.py flash` for the first installation on an original device.

After this community firmware is installed, open `http://quote0-baby.local/`
and expand **宝宝社区固件更新** to upload a future `quote0_baby.bin` release
directly from a phone or computer. The page shows upload progress and reboots
the device after validation. Factory firmware still requires the first full
USB flash.

The same panel performs a one-tap online update. The device downloads the
latest `quote0_baby.bin` release asset from this GitHub repository over HTTPS,
validates it, writes the inactive OTA partition, and reboots.

## Open source

This project is open source under the [Apache License 2.0](LICENSE). Issues,
improvements, device adaptations, and documentation contributions are welcome.

项目使用 [Apache License 2.0](LICENSE) 开源，欢迎提交问题、改进方案、硬件适配和文档贡献。

## Local Wi-Fi

When no network is configured, connect a phone to:

```text
SSID: Quote0-Baby-XXXX
Password: baby1234
Setup page: http://192.168.4.1
```

After saving the home Wi-Fi credentials, the device reboots onto the LAN. The
Hermes bridge discovers it with a UDP broadcast and sends commands to the local
HTTP API. On networks that support multicast DNS, the dashboard is also
available at `http://quote0-baby.local/` even if the router changes its IP.

The web dashboard checks for changes from other caregivers every four seconds
while visible. It includes an installable web-app manifest and a versioned JSON
backup/restore flow for the current state, feeding interval, and the latest 32
device records. Backup files are validated completely before NVS is replaced.
Because browsers require HTTPS for Service Worker storage, offline caching is
not guaranteed when the dashboard is served over plain LAN HTTP.

The hardware has no button, so recovery is automatic. If saved Wi-Fi
credentials cannot connect, the firmware retries about 11 times and then opens
the same `Quote0-Baby-XXXX` recovery hotspot. Connect to it and manually open
`http://192.168.4.1` to replace the Wi-Fi settings. ESP32-C3 supports 2.4 GHz
Wi-Fi only. USB remains an additional recovery path.
