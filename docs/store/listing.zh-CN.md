# 上架文案（中文）

## 产品名建议

| 方案 | 名称 | 说明 |
|---|---|---|
| **A（推荐）** | **OP-Ay1** | 系列名，和固件里 `project_name` 一致；首版用副标题限定为播放器，后续加合成器/采样器/音序器仍可沿用。 |
| B | ARYX Player | 以首版内置曲目命名，记忆点强；但绑定单曲，换曲目后品牌语义变弱。 |
| C | Pocket Tracker | 品类清晰、英文友好；但通用名容易被同名项目淹没。 |

推荐标题（≤200 字符）：

> **OP-Ay1｜ARYX 模块播放器**

备选：`OP-Ay1 · 口袋模块音乐播放器`、`OP-Ay1：把 ARYX 装进口袋`。

## 站点文案

**一句话简介**

> 一台为 ARYX 模块音乐打造的掌上播放器：谱面、波形、电平、混音、片段切换，一块 480×480 屏幕全给齐。

**短描述（约 90 字）**

> OP-Ay1 把一首内置的 S3M 模块音乐装进口袋：便携 C 引擎 48 kHz 播放，五页 tracker 界面，dBFS 电平与 dB 推子混音台，4×4 Section Pads 按节拍切换 Pattern。首版专注“听”和“调”，合成器、采样器、音序器在路线图中。

**长描述**：见 `README.zh-CN.md`（作为 `website.readme` 提交，站点详情页会渲染它）。

**分类**：`audio`、`display`

**标签**：`S3M`、`tracker`、`模块音乐`、`播放器`、`混音台`、`ESP-Mosaico`、`自制固件`

**外链**：`GitHub` → <https://github.com/AyalaKaguya/OP-Ay1>

**License 字段**（≤500 字符，确认曲目授权后再上传）：

> 代码：Apache-2.0（见仓库 LICENSE）。内置曲目 aryx.s3m © 1995 Karsten Koch，
> 版权归作者所有；打包分发需作者书面许可（当前待确认）。

**Changelog v1.0.0**

> 首个公开版本：内置 ARYX 模块播放（48 kHz 立体声）、WAVES / PATTERN / PLAYBACK /
> MIX / SECTION PADS 五页界面、dBFS 电平与 dB 推子、M/S 独奏联动、Section Pads
> 节拍排队与量化停止，以及看门狗与崩溃日志保留。

## 图册顺序（covers 数组顺序 = 展示顺序）

| # | 文件 | 说明 |
|---|---|---|
| 1 | `docs/store/covers/cover-01-hero.png` | 主视觉：OP-Ay1 + 谱面 |
| 2 | `docs/store/covers/cover-02-mix.png` | MIX 混音台 |
| 3 | `docs/store/covers/cover-03-pads.png` | SECTION PADS |
| 4 | `docs/store/gallery/playback.png` | PLAYBACK 走带页实拍 |
| 5 | `docs/store/gallery/waves.png` | WAVES 波形页实拍 |
| 6 | `docs/store/gallery/mix.png` | MIX 实拍（与封面互补） |

## 英文版（device.en 与可选的站点英文摘要）

- Title: **OP-Ay1 — ARYX Module Player**
- Tagline: *A pocket player for the ARYX module.*
- Short: OP-Ay1 plays the built-in ARYX S3M module through a portable C engine at
  48 kHz: tracker views, dBFS meters, a dB fader mixer and a 4×4 section-pad
  launcher. First release focuses on playback and mixing; synth, sampler and
  sequencer are on the roadmap.
- Device name (en): `OP-Ay1 Player`
- Device description (en): `ARYX module player with mixer and section pads`

## 设备端展示（mosaico-ideas.json 的 device.presentations）

| 语言 | name | description |
|---|---|---|
| zh-CN | `OP-Ay1 播放器` | `内置 ARYX 模块音乐播放器，带混音台与片段启动器` |
| en | `OP-Ay1 Player` | `Built-in ARYX module player with mixer and section pads` |

字体：中文需要 OFL 授权的 CJK 字体（建议 Noto Sans SC）；英文可使用仓库内
`components/app_tracker/ui/fonts/DejaVuSans.ttf`。图标使用
`docs/store/icon-64.png`（64×64 RGBA）。
