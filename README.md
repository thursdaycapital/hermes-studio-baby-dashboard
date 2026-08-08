# Quote/0 Baby Dashboard

[中文介绍与使用教程](docs/README.zh-CN.md) | English

[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-5b7f63.svg)](LICENSE)
[![Release](https://img.shields.io/github/v/release/thursdaycapital/hermes-studio-baby-dashboard)](https://github.com/thursdaycapital/hermes-studio-baby-dashboard/releases)

一个开源、本地优先的 Quote/0 墨水屏宝宝照护看板。手机通过局域网记录
喂奶、睡眠和尿布，设备同步显示状态、趋势与提醒，不依赖官方云服务。

![Quote/0 宝宝看板实机运行效果](docs/quote0-baby-dashboard.jpg)

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
HTTP API.

The hardware has no button, so recovery is automatic. If saved Wi-Fi
credentials cannot connect, the firmware retries about 11 times and then opens
the same `Quote0-Baby-XXXX` recovery hotspot. Connect to it and manually open
`http://192.168.4.1` to replace the Wi-Fi settings. ESP32-C3 supports 2.4 GHz
Wi-Fi only. USB remains an additional recovery path.
