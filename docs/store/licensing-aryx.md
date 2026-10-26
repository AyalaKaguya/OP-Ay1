# aryx.s3m 版权核查与处置建议

> 核查对象：`main/assets/aryx.s3m`（20,800 字节）
> 结论：**受版权保护，未发现允许“打包进固件分发”的授权**。
> 在取得作者书面许可或替换曲目之前，**不要把包含该曲目的固件上传到 mosaico.ai / 继续分发**。

## 1. 证据链

| 项 | 值 |
|---|---|
| 本地文件 SHA-256 | `e7b5f1ba69368178c95cce2abff5eb2cf081582b9ef4386a1cc9080d3e2412ed` |
| 本地文件 MD5 | `0f5bcf77f32f9f7299c49050094caeb8` |
| S3M 内部标题 | `aryx` |
| 内部文本 | `k.koch 29.03.95`、`Tune composed using Space Tracker 1.0 BETA`、`Time: 2:21` |
| ModArchive | 模块 ID **191789**，`aryx` (S3M)，MD5 与本文件**完全一致**：<https://modarchive.org/index.php?request=view_by_moduleid&query=191789> |
| 作者（ModArchive 注册艺术家） | **Karsten Koch** |
| Aminet | `mods/s3m/aryx.lha`：*"Aryx" by Karsten Koch (29.3.95)*，第二条官方发布 12.6.95，文件 20,800 字节：<https://aminet.net/package/mods/s3m/aryx> |

## 2. 授权状态

### ModArchive 的“License”

模块页标注为 **Mod Archive Distribution license**（上传协议 v2.2）。协议关键点：

- **§1.3**：作者保留版权（copyrights retained），仅授予 ModArchive 非独占分发权。
- **§1.2**：允许任何人在**保持原始、未修改、未打包**（original, unmodified, unbundled form）的前提下再分发该文件。
- 同名 FAQ 明确写明：*"This does not cover inclusion in a packed/bundled application or game."*，并说明使用他人模块做应用/游戏应**直接联系作者取得许可**，ModArchive 不能代为授权。

→ 因此：把 `aryx.s3m` 嵌入 `.irisfw` 固件随应用分发，**不属于**该分发许可覆盖的范围。

### Aminet 的“freeware”性质

Aminet 只是历史分发镜像（1995-12-08 上传，原始来自 `ftp.cdrom.com`），页面与 readme 里没有 public domain 声明，也没有可再分发的开放许可证。Aminet 的归档分发不等同于作者对“嵌入式/应用打包”的授权。

### 作者情况

- Karsten Koch，德国开发者；ModArchive 艺术家资料与社区讨论都确认他是 Aryx 的作者。
- 2019 年 ModArchive 论坛上，有开发者（Randy Linden）公开寻找 Karsten Koch，希望把 Aryx 用于自己的游戏，ModArchive 版主给出的建议是**通过 Nectarine 资料页联系**，或尝试旧邮箱（该邮箱域名已失效），说明当时并**不存在一个现成的、可直接引用的授权**：
  - <https://modarchive.org/forums/index.php?topic=4415.0>
- 社区里也有关于其作品被他人取样/使用的争议帖，说明作者权利并未被放弃。

## 3. 对我方发布的影响

- 当前 GitHub Release `v1.0.0` 的 `OP-Ay1-1.0.0.irisfw` **已经内含** `aryx.s3m`，属于“打包分发”，与本核查结论冲突，建议在解决前不要再扩散该二进制。
- mosaico.ai 上架包同样会把该曲目打包进固件，**在上传前必须先解决授权**。
- 仓库源码中的 `main/assets/aryx.s3m` 同样公开可下载，建议一并处理（删除、替换或加清晰的“仅本地开发使用”说明——但仅说明并不能替代授权）。

## 4. 处置选项

### A. 取得作者书面许可（推荐先尝试）

联系渠道（勿公开个人邮箱）：

1. Nectarine 资料页（ModArchive 版主给出的线索）：<https://scenestream.net/demovibes/user/karstenko/>
2. ModArchive 论坛 "Wanted / 找人" 版块发帖，@ 相关人员；
3. 通过 modarchive 艺术家页的留言功能（需登录）。

沟通要点：公开说明用途（非商业开源固件、硬件播放器、通过 mosaico.ai 与 GitHub 分发 `.irisfw`）、是否需要在二进制中嵌入、署名方式、可否按 CC BY 4.0 或一次性非独占许可授权、是否要求只允许非商业分发。

可复制的邮件/私信草稿（英文）：

> Subject: Permission request: using "Aryx" (aryx.s3m, 1995) in an open-source ESP32 firmware
>
> Hi Karsten,
>
> I'm building OP-Ay1, an open-source handheld tracker/player for the ESP-Mosaico
> board. It is non-commercial, source is on GitHub, and firmware builds (.irisfw)
> are distributed for free through GitHub Releases and possibly mosaico.ai.
>
> I'd like to embed your module "Aryx" (aryx.s3m, 1995, 12 channels, Space Tracker)
> in the firmware as the built-in demo song, with clear attribution
> ("Music: Aryx by Karsten Koch, 1995") in the app, README and store listing.
>
> Would you be willing to grant a non-exclusive permission for this use (or point
> me to the license terms you prefer)? If not, I'll replace the track before the
> public release.
>
> Thank you!
> [name / GitHub link]

拿到回复后：截图/存档保存；在 `project.license` 与 README 中写明（例如 “Music: aryx.s3m © 1995 Karsten Koch, used with permission (date)”）。

### B. 替换为可再分发的曲目（最稳妥）

- 从 ModArchive 的 **Public Domain** 过滤器中挑选已明确标注 PD 的模块，或使用 CC0/CC-BY 授权的模块（CC-BY 需署名）。
- 或自行创作/委托创作一首 S3M，完全掌握权利。
- 替换后需要：更新 `main/assets/`、重新构建、更新固件哈希、更新 README/商店文案与截图（如曲目名出现在界面/文案中）。

### C. 暂不公开分发

- 仅本地开发使用，不上传 Store、不发布带该曲目的二进制；等 A 或 B 完成后再发布。

## 5. 检查清单

- [ ] 决定走 A（取得许可）还是 B（替换曲目）
- [ ] 若 A：保留作者书面回复/许可截图
- [ ] 若 B：替换资源并重新构建、更新所有哈希与文案
- [ ] 更新 `mosaico-ideas.json` 的 `license` 字段（必须真实）
- [ ] 更新 README / 商店详情的“许可与致谢”
- [ ] 重新打包并重新生成 Release 资产（当前的 v1.0.0 二进制含该曲目）
