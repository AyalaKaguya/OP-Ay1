# 项目工作站实施记录

日期：2026-10-27。实施分支：`codex/project-workstation`；验证分支：`codex/project-build-tests`。
按已确认的[项目原型](project-system-prototype.zh-CN.md)实施，随后按用户的构建测试要求
完成原生回归、GSP 原生交互检查和固定 ESP-IDF 固件构建，未写入设备。
以下分别记录源码设计、已完成检查和真机验证边界。

界面、项目服务、NAND 挂载、采样器和设备／主机接线均已落到该分支源码。
新版固件及 System Update 包已生成，并独立核对包内大小和 SHA-256。

## 模块与所有权

- `app_project` 拥有一个全局项目会话及后台 I/O worker。保存、打开、导入和重型采样操作
  串行执行，界面只发送有界请求和读取快照。所有模块借用同一个实例。
- `app_nand` 在正常应用中挂载既有 `/nand` LittleFS，使用原有 NAND wear-level block device。
  先读取卷几何，不格式化、不增长卷，不更改 NOR 分区表、身份、凭据或 Vibe Mode。
- `app_sampler` 拥有项目样本库、编辑副本、试听和录音 Take；音频总线借用采样器。
- `app_tracker_engine` 保持公开 handle 稳定，预先准备目标 S3M，再提交内部模块替换。
  原 Order／Pattern／效果命令和样本槽编号继续保留。
- `app_tracker` 在既有像素 renderer 上增加主屏、项目库、设置和采样器界面。
  UI 状态与项目 I/O、PCM 所有权分离，继续使用已存在的全局下拉、动画与唤醒入口。
- `main` 只组合音源、采样器、项目服务、设备设置与 UI；主机 adapter 使用真实本地目录。

## NAND 与外部导入

应用根目录为 `/nand/apps/op-ay1`，项目位于 `projects/`，导入文件位于 `inbox/`。
与 Vibe Mode 已有的 `/nand/ideas` 和 `/nand/system-update` 分开。
挂载失败会报告不可用，原会话仍留在内存；没有自动格式化入口。

正常应用在 Iris 启动前注册延后就绪的 `opay_inbox` 文件卷，仅映射应用收件目录。
挂载及目录未就绪时返回 BUSY；文件卷不会把项目版本目录或 NAND 其他应用目录交给外部写入。
使用既有 Iris 文件服务把 S3M／WAV 放入该卷，之后在项目导入界面刷新并选择文件。
该入口沿用 Vendor 管理链路；本轮不继续迭代此前未通过实机验收的 MIDI Host 功能。

保存以项目稳定 ID 和版本为单位。先保存完整音频资源与状态，再校验后提交可见版本。
失败保留旧的完整版本与当前内存修改；不能只保存文件名或 PC 路径。
打开目标时先验证、分配并准备目标内容，成功后才替换会话。
打开当前项目 ID 不重新加载，HOME 与模块切换也不重启项目。

主机预览源码默认使用 `build-sim/project-data` 作为实际文件根目录；
`OP_AY1_PROJECT_ROOT` 可指定另一目录。主机目录不是板载 NAND 的模拟容量证明。

## 演奏与采样

HOME 每页 2×2。第一页为 OPEN PROJECT、SETTINGS、PLAYBACK、MIXER；
第二页为 PADS、SYNTH、SAMPLER、PATTERN；第三页为 WAVES 和三个空位。
项目名、未保存标记和保存入口在模块间保持一致。

项目保存 S3M 原始数据／当前 Order、样本 PCM 和裁剪／循环参数、显式乐器槽映射、
完整 Synth patch、混音与工程控制。电平、按住的音符和播放位置不作为未保存修改。
亮度、自动旋转、降亮度和息屏设置仍是全局设备偏好。

采样器提供 LIBRARY／EDIT／RECORD。WAV 解码与导入、乐器槽替换等大复制在后台完成；
Apply 才提交编辑，Cancel 不改资源，Undo 保留一轮恢复。
现有 PADS 继续是 Pattern launcher，不被静默改成样本鼓垫。

设备录音接 BSP 的 ES8311 麦克风，复用当前 I2S 数据接口和输出采样格式。
录音先进入 Take，停止后可试听、保留或丢弃；Keep 后才成为项目资源。
监听默认关闭。主机没有麦克风采集 adapter 时明确返回不支持，不生成假录音。
采样／录音有数量、时长和内存边界，分配失败应报告错误并保留已有项目。

当前约束：样本库最多 64 个槽，单样本至多 2,880,000 帧／60 秒（取更小值）；
录音为 48 kHz、16-bit，麦克风立体声输入取平均存为单声道，监听默认关闭。
WAV 接受 RIFF PCM 8/16-bit、mono/stereo、8…192 kHz，文件导入逐块转换，
不把整份 WAV 与转换后的 PCM 同时复制一遍。
ARYX 提取保留 9 个乐器槽（8 个有 PCM，1 个空槽），不删除或重新编号空槽。
原乐器音量与可编辑 Gain 分离，C2 基准回放率不当作录音采样率。

项目列表和收件目录快照各最多 32 条，超出时标记截断；最近列表只记录本次运行的
最近项目并核对当前目录。完整项目加载需要原会话与目标暂存资源同时驻留，
内存不足时保留原项目。当前保留旧保存版本，不自动清除用户文件或旧 generation。

NAND 挂载使用固定 `joltwallet/littlefs ==1.23.1`，已核对其
[官方公共头文件](https://raw.githubusercontent.com/joltwallet/esp_littlefs/v1.23.1/include/esp_littlefs.h)
中的 blockdev、卸载和容量接口；这仅确认源码接口，不代表实际介质已挂载。

保留原工作站状态 RPC，新增只读 `0x2101/4` 查看 HOME／模块、当前项目名称、
打开／未保存／busy 状态及会话 generation；不会通过该接口直接改写工程。

## 构建与测试结果

2026-10-27 构建测试完成，中断后已恢复现场并补齐最终检查。
固件构建时源码 HEAD 为 `2b7713f`；随后仅整理测试脚本格式、可执行标记及验证文档，
没有再修改固件生产代码。

| 检查 | 结果与覆盖 |
|---|---|
| 采样器 | 6645 条断言与并发检查通过；ARYX 9 槽、WAV 转换／拒绝畸形输入、Cancel／Apply／Undo、槽映射、PCM 往返及 100 次项目切换 |
| 项目 worker | 最终源码补跑 4 组通过；完整状态／PCM 往返、另存、移除导入源文件后重开、空间不足、损坏版本回退、失败保存保护及重启恢复 |
| 音频与 Synth | 总线和初始化故障注入通过；音高、分块等价、包络、预设电平、数值边界、并发事件和快照通过 |
| MIDI 原生回归 | 路由／描述符及 production lifecycle 两套通过；不代表 USB MIDI Host 真机验收 |
| UI 生命周期 | 帧分配失败／回滚、快速重定向、共享下拉快照、唤醒手势所有权通过 |
| 新项目界面 | 官方 GSP 1.5.1 模拟器通过 12 项真实指针输入流程；独立读取保存文件验证 CRC、PCM、原始 S3M 和槽映射 |
| 既有六页界面 | 13 项流程通过；播放、混音、Pads、Synth、设置、动画及闲置唤醒；输出 4,645,888 帧 PCM，模拟器与 backend 均正常退出 |
| 质量检查 | 相对 `f5e5ec5` 的 99 个变更文件检查通过；恢复后修正新增脚本可执行标记并复查 |

核心测试完成后的 sampler／audio／Synth／tracker engine／note router／USB MIDI
相关改动已按 C token 或 Python AST 复核，只有格式差异；存储路径修复后的 worker
测试则已重新运行。模拟器运行的是共享 C backend，检查没有通过 RPC 直接修改项目状态。
主机 REC 返回不支持，不把主机流程通过当作真实录音通过。

本轮修复包括：采样器 UI 元数据初始化、原生测试的原子支持编译选项、WAV RIFF 长度
校验、存储路径重叠拼接、项目列表文字缓冲区长度和 NAND 驱动参数类型。
组件仓库下载问题仅在构建进程内绕过已验证的 CDN 代理通路，没有更改系统代理。

固定 SDK 实际 checkout 已重新验证为
`7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`，doctor 确认 Python 3.11.15、
`esp32s31` 和声明的 IDF 约束均满足。低噪声构建返回 `IDF LOW-NOISE BUILD: OK`。

- 构建记录：`20261027-133812-build-48300`，用时 149.688 秒。
- 应用：`build-pinned/OP-Ay1.bin`，1,767,152 字节。
- ELF：`build-pinned/OP-Ay1.elf`。
- 更新包：`build-pinned/OP-Ay1-system-update.irisfw`。
- 应用 SHA-256：`653abc262e315e1a68a8ad89cd051a3e4c86697f1c09ffb96bc40a393331f238`。
- 更新包 SHA-256：`d7797fc1abb14f9c40a56dc28d2fd99d6cd372ff1ee2249b2de106e721ca3e4e`。

更新包 ZIP 校验及各组件大小／SHA-256 校验通过；BIN、ELF、更新包哈希与中断前
记录一致。包内为分区表、`ota_0` 应用和 `ui_apps` 数据，不包含 Vibe Mode 镜像。
这确认本地产物一致性，不代表已与连接设备的现有布局完成兼容性核对或安装验收。

构建日志有 3 条警告记录：1 条未加载非信任目录的 IDF 命令扩展，另 2 条为重复的
最小应用分区余量提示。SDK 通用大小检查取所有 app 分区的最小值，因此用到了
保留的 `vibe_mode`（`0x1c0000`）并报告约 4% 余量；更新包实际应用目标为
`main_app/ota_0`（偏移 `0x210000`、容量 `0xcf0000`），不是该最小分区。
没有为消除此提示改动分区布局。

本机完整证据保留在应用目录的 `.codex-runs/`：

- `idf-low-noise-build/20261027-133812-build-48300/raw.log`、`clean.log`、`result.json`。
- `project-validation/core-results.json`、各核心测试日志、`project-resumed.log`、
  `ui-owner.log`、`quality-resumed.log`、`artifacts.json`。
- `project-ui-sim/result.json`、`project-legacy-ui-sim/result.json` 及同目录状态／截图。

## 当前验证边界

板载 NAND 的真实挂载／容量、物理掉电时持久性、麦克风输入／监听、实体触控／USB、
扬声器声音一致性、显示与满负载资源表现仍需真机验收。
本轮没有刷写、格式化或修改设备数据；此前未通过的 USB MIDI Host 真机验收状态不变。
