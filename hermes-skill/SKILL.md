---
name: quote0-baby-dashboard
description: 使用连接到 Mac 的 Quote/0 墨水屏记录新生儿喂奶、睡眠、尿布和下一次提醒。
version: 1.0.0
metadata:
  hermes:
    tags: [baby, newborn, quote0, eink, family]
---

# Quote/0 新生儿桌面看板

当用户在 Hermes Studio 会话中提到宝宝的喂奶、睡眠、换尿布、
出生天数或下一项提醒时，调用 `mcp_quote0_baby_*` 对应工具。

- “刚喝了 80ml” → `baby_log_feed`，省略时间以使用本机当前时间。
- “宝宝睡了” → `baby_log_sleep(state="ON")`。
- “宝宝醒了” → `baby_log_sleep(state="OFF")`。
- “刚换尿布，有尿” → `baby_log_diaper(kind="W")`。
- “有便便” → `baby_log_diaper(kind="D")`。
- “尿和便都有” → `baby_log_diaper(kind="WD")`。
- “23:30 提醒喂奶” → `baby_set_next(time="23:30", kind="FEED")`。
- “今天一共喝了多少” → `baby_today_summary`。
- “看看最近的记录” → `baby_recent_history`。
- “以后每 3 小时提醒喂奶” → `baby_set_feed_interval(minutes=180)`。
- “周六上午九点提醒打预防针” → 解析具体日期后调用
  `baby_set_vaccine_reminder(date="YYYY-MM-DD", time="09:00")`。
- “刚才记错了” → `baby_undo_last`。

喂奶记录成功后会按设置的间隔自动更新屏幕上的“下次喂奶”。
每次写入成功后，简短确认记录内容。不要把这块看板当作医疗器械；
涉及新生儿健康异常时应建议联系儿科医生或专业医疗机构。
