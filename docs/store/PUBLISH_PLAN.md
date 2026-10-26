# 发布到 mosaico.ai：调研结论与执行清单

> 目标站点：<https://mosaico.ai>（ESP-Mosaico 的 Ideas 社区商店）。
> 当前状态：**未登录、未上传、未公开发布**。本文件只准备发布材料，等你审阅后再执行上传。

## 1. 发布链路（CLI + 网站）

```
本地构建 .irisfw
   ↓
project package        → 生成不可变的 Ideas 发布 ZIP（离线，自动校验）
   ↓
project validate       → 再次校验 ZIP（可选但建议）
   ↓
project upload         → 在 mosaico.ai 创建/更新【草稿】，返回 draft_url
   ↓
你在浏览器打开 draft_url 审阅 → 提交审核 → 审核通过后公开
```

要点：

- `project upload` **只创建草稿**，不会直接上线；正式发布需在网站提交审核。不要把它说成“已发布”。
- 首次发布用 `--create`；后续版本用 `--update <application_id>`（UUID 以服务端为准，不靠标题推断）。
- 站点账号使用浏览器设备授权：`python mosaico.py account login --open-browser`，需要你本人在浏览器确认；我无法代签。
- 服务器默认为 `https://mosaico.ai`；可用 `--server` 覆盖，token 环境变量为 `MAKER_SPARK_TOKEN`。

## 2. 发布物结构

`project package` 生成的 ZIP 内含：

| 内容 | 说明 |
|---|---|
| `firmware/app.irisfw` | 设备固件包（ESP-Iris system-update bundle，≤32 MiB） |
| `README` / changelog | 网站详情页正文（UTF-8，≤100,000 字符且 ≤400 KB） |
| `covers/` | 网站图册（0–9 张，顺序即展示顺序） |
| `device/zh-CN/`、`device/en/` | 设备端商店展示：名称、描述、字体子集、64×64 图标 |

## 3. 作者配置 `mosaico-ideas.json`（应用根目录，schema `mosaico-ideas/project-release/1`）

必需字段：`schema`、`project`、`website`、`device`。要点：

- `project.title`：1–200 字符；最终标题见 `listing.zh-CN.md`。
- `project.categories`：至少 1 项，可选 `smart-home` / `sensors` / `display` / `audio` / `communication` / `motor`；本项目 `["audio","display"]`。
- `project.boards`：只在需要相机/以太网/电池板时填，本项目不填。
- `project.tags`：≤20 项、每项 ≤50 字符。
- `project.license`：必须真实；≤500 字符。**曲目授权未确认前不能编造**（见风险）。
- `project.external_links`：≤10 项，平台值 `github` / `oshwhub` / `makerworld` / `other`。
- `firmware`：相对路径指向 `.irisfw`；也可用 `--bundle` 覆盖。
- `website.readme` 必需；`changelog`、`covers`、`resources`、`attachments` 可选。
- `device.default_locale`：`zh-CN` 或 `en`；`device.presentations.zh-CN` 与 `.en` **都必须提供**，各自可 `generate`（名称/描述/字体/可选图标）或 `prebuilt`。

## 4. 图片规格

| 项 | 要求 |
|---|---|
| 网站封面/图册 | 0–9 张；JPEG/PNG/GIF/WebP；宽高各 1–4096 px；单张 ≤10 MiB；单帧、非动画；推荐 16:9（不强制） |
| 设备图标 | 固定 **64×64**、8 bit RGBA、非交错、无动画 PNG，≤64 KiB；源图只用于本地生成 |
| 显示资源 | 每语言 display + 字体 + 图标合计 ≤1 MiB（中英两种合计 ≤2 MiB） |

## 5. 执行命令（工作区根目录）

```powershell
# 0) 一次性依赖：FreeType 字体子集 / Pillow / fontTools / markdown-it
.\.venv\Scripts\python.exe -m pip install -r submodule\esp-mosaico-utils\mosaico-tools\requirements-ideas.txt

# 1) 生成发布 ZIP（固件已在 build/ 中，版本 1.0.0）
.\.venv\Scripts\python.exe mosaico.py project package --project projects/OP-Ay1 --output dist\OP-Ay1-1.0.0-ideas.zip

# 2) 本地校验
.\.venv\Scripts\python.exe mosaico.py project validate dist\OP-Ay1-1.0.0-ideas.zip

# 3) 登录（浏览器确认设备码）
.\.venv\Scripts\python.exe mosaico.py account login --open-browser

# 4) 首次上传 → 草稿
.\.venv\Scripts\python.exe mosaico.py project upload dist\OP-Ay1-1.0.0-ideas.zip --create --server https://mosaico.ai --yes --json
```

第 4 步返回 `application_id`、`version`、`bundle_sha256`、`status: draft`、`draft_url`。之后在网站审阅并提交审核。

后续版本：更新 `CMakeLists.txt` 的 `project(OP-Ay1 VERSION 1.0.0)` → 重新构建 → `project upload <新 zip> --update <application_id> --yes --json`。

## 6. 版本规则

- 版本三个来源必须一致：`build/project_description.json` 的 `project_version`、包内 manifest 的 `release`、上传参数；当前均为 **1.0.0**。
- 版本 1–80 字符，以字母/数字开头，仅含字母、数字、`.`、`_`、`+`、`-`。
- 纯网页文字或图标更新可以复用同一固件版本，但会生成新的 release_id。

## 7. 发布前必须确认（风险项）

1. **内置曲目 `aryx.s3m` 的授权（已核查：阻塞项）**：作者为 **Karsten Koch**（1995，ModArchive MD5 与本文件一致）。仅存在 ModArchive「未修改、未打包」的分发许可，明确不覆盖打包进应用/固件；Aminet 归档也不构成授权。证据与处置建议见 [licensing-aryx.md](licensing-aryx.md)。选项：
   - 取得作者书面许可：在 `license` 与 README 中写明（如 “Music: aryx.s3m © 1995 Karsten Koch, used with permission (date)”）；
   - 或替换为 PD/CC0/CC-BY 曲目、自制曲目；
   **未解决前不得上传商店，也不建议继续分发含该曲目的 `v1.0.0` 固件。**
2. **中文设备展示字体**：`device.presentations.zh-CN.font` 需要包含中文字形的字体。建议使用 OFL 授权的 Noto Sans SC（下载到本地生成字形子集，不提交到仓库）；不要用系统微软雅黑等不可再分发的字体做嵌入。
3. **账号登录**：需你本人完成浏览器设备授权。
4. **首版定位**：只宣称“播放器 + 混音/片段监听”，不要写成已具备合成器/采样器/音序器；路线图另行说明。

## 8. 审阅清单

- [ ] 产品名与标题（见 `listing.zh-CN.md`）
- [ ] 分类 `audio` + `display`、标签、GitHub 外链
- [ ] 图册顺序与封面文案（`docs/store/covers/`）
- [ ] README 正文（`docs/store/README.zh-CN.md`）
- [ ] `license` 字段与曲目授权
- [ ] 中/英设备名称与描述、64×64 图标
- [ ] changelog 1.0.0
- [ ] 固件 SHA-256 与版本一致
