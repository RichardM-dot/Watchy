# Watchy（墨水屏手表）学习导读

> 目录：`04-项目实战/Watchy-墨水屏手表`
> 来源：https://github.com/sqfmi/Watchy （约 2.9k star）

## 这是什么

Watchy 是 SQFMI 出的**全开源 ESP32 墨水屏手表**：200x200 电子纸屏、三轴加速度计（BMA423）、RTC（PCF8563）、BLE、Wi-Fi，固件就是**一个 Arduino 库**（`src/` 里的 Watchy 类），表盘就是继承 `Watchy` 类重写 `drawWatchFace()`——**十几行代码就能画出自己的表盘**，是"用最舒适的方式玩墨水屏+低功耗"的项目。官方文档 watchy.sqfmi.com 有从刷机到自制表盘的完整教程。

## 目录导读

| 目录/文件 | 内容 |
| --- | --- |
| `src/Watchy.cpp` / `Watchy.h` | 主类：初始化、表盘管理、按钮、深睡 |
| `src/Display.cpp` / `Display.h` | GxEPD2 墨水屏封装（200x200） |
| `src/BLE.cpp` / `bma.h`（BMA423） / `WatchyRTC.cpp`（PCF8563） | 蓝牙、加速度计、RTC 三大外设 |
| `src/DSEG7_Classic_Bold_53.h` | 数字表盘用的七段字体 |
| `examples/` | **官方示例表盘（7-seg、basic、DOS、Pokemon...），学习入口** |
| `extras/` | 硬件原理图等资源 |

## 学习路径（建议）

1. 按 watchy.sqfmi.com/docs/getting-started 装好 Arduino 环境（ESP32 板支持包 + Watchy 库）。
2. 烧录 `examples/WatchFaces/` 里的示例表盘；然后**自己写一个表盘**：继承 Watchy 类、重写 `drawWatchFace`、用 `display.fillCircle/drawString` 画一个时钟。
3. 读 `Watchy.cpp` 的主循环：显示 → **进入深睡** → RTC/加速度计中断唤醒 → 再显示——墨水屏手表的省电闭环。
4. 玩加速度计：BMA423 的计步/翻腕中断如何唤醒（`bma.h`）。
5. 进阶：BLE 配套 App 通信、Wi-Fi NTP 对时（示例里有）、自制 PCB（extras 里有原理图）。

## 学习要点

1. **墨水屏 + 深睡的绝配**：屏幕断电保持画面，MCU 深睡几 μA，靠 RTC 每分钟唤醒刷新——全表电池数周。
2. **Arduino 库化固件**：把固件做成"库 + 用户继承"，用户体验极佳——学 API 设计。
3. **RTC 优先**：PCF8563 独立 RTC（32K 时钟域） vs ESP32 内部 RTC 的取舍。
4. **表盘开发模式**：`drawWatchFace()` 里随便画，BMA 手势在回调里处理——事件驱动 UI 的最小样本。

## 建议购买/准备（三档）

- **0 元档**：官方模拟器/读代码 + 学表盘 API（文档有在线表盘设计器）。
- **正牌 Watchy**：官方店/Mouser/Crowd Supply 约 $60-80（国内代购 ¥500-700），开机即玩。
- **平替 DIY 档（¥100-200，推荐给动手党）**：
  - ESP32 DevKit（¥20-40）
  - 1.54 寸 200x200 墨水屏模块（微雪/GDEH154Z90 等，¥60-120——**注意要 200x200 分辨率型号**）
  - BMA423 或 ADXL345 加速度计模块（¥5-15）、DS3231/PCF8563 RTC 模块（¥5-10）
  - 18650/聚合物锂电 + TP4056（¥15）
- 表带/外壳 3D 打印（官方开源）。

## 学习目标（自测）

- [ ] 烧录示例表盘并完成 Wi-Fi NTP 对时
- [ ] 写一个自定义表盘（含日期+天气/电量任一数据）
- [ ] 说出一次"整点刷新"从 RTC 中断到屏幕显示的流程
- [ ] 实测一次深睡电流并估算电池寿命
