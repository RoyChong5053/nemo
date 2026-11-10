# Nemo rclone fork — 计划总纲 (plan.md)

> 本文件从之前多个 opencode session 恢复并整理而来，作为 fork 的单一计划来源。
> 目标效果（用户原话）：侧边栏 rclone 页面显示 rclone config 里的连接，
> 点击就自动 mount，然后像浏览本地文件一样浏览文件；连接逻辑自然略有不同，
> **尤其是图片缩略图的生成逻辑要适配网盘连线问题**。

---

## 0. 从旧 session 恢复的历史（context）

三个相关 session（已从 `~/.local/share/opencode/opencode.db` 恢复）：

| Session | 主题 | 关键产出 |
|---|---|---|
| `ses_e44fd21…` | Nemo fork 集成 rclone 与 yazi 预读取 | 产品需求收敛、M0–M4 分阶段计划、v1 实现（远程策略 + favorites + 传输窗口） |
| `ses_e44c1ff…` | nemo 图片缩略图实现与缓存 | 缩略图机制深挖、**按需远程缩略图**设计、内存缓存/后台解码方案 |
| `ses_e4352f2…` | rclone 位置报错（翻车修复） | 当前这个 session；修复 `Nemo cannot handle "rclone" locations`，实现按需挂载 |

### M0–M4 总体路线图（来自旧 session）
- **M0** 远程文件系统策略：识别 `fuse.rclone/sshfs/cifs/nfs`、精简属性集、跳过子目录计数 → **已完成**（`nemo-remote-policy.[ch]`）
- **M1** rclone 记忆层：remotes、fav link、A→B 传输、右键菜单、侧边栏 → **已完成**（`src/rclone/`）
- **M1.5** 按需挂载（mount-on-demand）：点 favorite 自动 `rclone mount` 并打开 → **已完成（本次/上一 session）**
- **M2** 写回进度、`vfs/queue` 后台上传监控、systemd `rclone-mount@.service`
- **M3** 并发/超时/视口预取
- **M4** Hyprland 打磨 + `ricing.css`
- **★ 新增聚焦** 远程/网盘图片缩略图的按需生成（本 objective 的核心诉求）

---

## 1. 当前状态（已落地，v1 + 按需挂载）

### 已实现的功能
1. **远程策略**（`libnemo-private/nemo-remote-policy.[ch]`）
   - 通过 `/proc/self/mountinfo` 识别慢速远程
   - 慢速远程用精简属性集（去掉 `access/owner/selinux/metadata/trash`）
   - 慢速远程默认不为每个子目录枚举项目数（`nemo_remote_policy_should_skip_item_count`）
2. **Rclone 服务**（`src/rclone/nemo-rclone-service.[ch]`）
   - `rclone listremotes`（回退解析 `rclone.conf`）
   - favorites 存 `~/.config/nemo/rclone-favorites.json`
   - `resolve_mountpoint` / `mount_favorite` / `unmount_favorite` / `remote_to_local` / `favorite_is_mounted` / `slugify` / `format_uri`
   - 按需挂载：`fork`+`setsid` detach，`g_child_watch_add` 回收，`WNOHANG` 快速失败检查
   - 挂点默认 `~/.local/share/nemo-rclone/mounts/`（`/mnt` 可写时用 `/mnt/nemo-rclone/`），`NEMO_RCLONE_MOUNT_BASE` 可覆盖
   - 日志 `~/.cache/nemo-rclone/<mountpoint>.log`，失败显示日志尾部
3. **Rclone 窗口**（`src/rclone/nemo-rclone-window.[ch]`）
   - 传输源/目标选择、目录浏览（`rclone lsf`）、`copy/copyto/move/sync`、真实进度（`--use-json-log --stats 1s`）、取消、日志
   - Favorites 标签页：**Mount / Unmount / Open** 按钮 + 实时挂载状态列
   - `nemo_rclone_open_uri()` 挂载后打开本地挂点
4. **`rclone://` scheme 的中央拦截**（`src/nemo-window-manage-views.c` + `src/nemo-places-sidebar.c`）
   - 不是 GVFS backend，由 Nemo 自己处理，无 `G_IO_ERROR_NOT_SUPPORTED`
   - `rclone://transfer` → 传输窗口
   - `rclone://remote/path` → 匹配 favorite 则按需挂载并打开；未知路径 → 传输窗口预填源
5. **右键菜单 Rclone**：Open Transfer / Mark as Favorite / Copy Path / Use as Source
6. **FORK.md** 已更新（mount 段、已知限制）

### 挂载参数模板（固定）
```
--vfs-cache-mode full --vfs-read-ahead 128M --vfs-read-chunk-streams 8
--dir-cache-time 10m --attr-timeout 1m --poll-interval 0
--cache-dir <XDG_CACHE>/rclone/vfs --allow-other --allow-non-empty
```

### 已知遗留
- 无 systemd 单元，crash 不重启、登出不自动 umount
- 目录浏览仍是同步（大/慢目录会短暂卡）
- 复制回写进度仍走 FUSE（M2）
- 每文件夹缩放/排序仍靠 GVFS metadata（无 path-keyed view-state）

---

## 2. 本次 objective 的核心诉求：网盘图片缩略图

### 2.1 现状（已查代码确认）
- 缩略图走 GNOME 标准缓存（`~/.cache/thumbnails/large|normal|fail/`），`nemo-thumbnails.c` + 线程池。
- `nemo_file_should_show_thumbnail()`（`nemo-file.c:4489`）判定顺序：
  1. 先查父目录 `NEMO_METADATA_KEY_SHOW_THUMNAILS` 元数据（`true`/`false` 可覆盖全局）→ **这就是"每文件夹开关"的实现点**
  2. 再回落全局偏好：`IF_LOCAL`（默认）→ `nemo_file_is_local()` → **远程返回 FALSE**
- **默认结论**：rclone/FUSE 网盘图片**默认不生成缩略图**（显示通用图标），这正是"优先不卡"想要的。
- 那 657 个缓存全部来自本地 `/mnt/data`（ext4），**没有一个是网盘的**。

### 2.2 用户想要的效果
1. 浏览网盘图片文件夹：**优先不卡，先 load 出文件名**（默认 `local-only` 已满足）
2. 可**一张一张打开**预览
3. 觉得不爽时，**手动按一次"为本文件夹生成缩略图"**
4. 生成后**绑定该路径**，下次浏览直接 cache 出缩略图（"绑定路径"）
5. 这符合 fork "remote-first、用户决定何时值得花网络成本"的定位

### 2.3 关键技术风险：元数据持久化
- 现有"每文件夹开关"靠 GVFS `metadata::` xattr 持久化。
- **rclone FUSE（尤其 SFTP/m64）对 xattr 支持因后端而异**，很可能不持久 → "绑定路径"不成立。
- **这是 fork 真正要补的地基**，必须先验证再决定。

### 2.4 设计方案（plan 阶段已收敛，待实现）
- **默认**：`local-only`，远程只显示文件名，丝滑。
- **手动入口**：远程目录里点 "Show Thumbnails"（现有工具栏按钮/右键菜单已有，可能只需默认可见）。
- **持久化开关（fork 白名单，B 方案为主，GVFS 回落 A）**：
  - 新增 fork 自己的"已启用缩略图路径白名单"（JSON，按 mountpoint 前缀匹配，如
    `~/.local/share/nemo-rclone/mounts/m64_home`）。
  - `nemo_file_should_show_thumbnail()` 读取时**先查白名单，再回落 GVFS 元数据**。
  - 远程目录开关因此 100% 可靠，不依赖 FUSE xattr。
- **生成**：触发后该目录图片落本地 `~/.cache/thumbnails/`；用**后台解码**（否则点下去卡主线程）。
  - 首次生成需下载全分辨率原图过网络（一次性网络成本，无法避免；格式优化救不了这个）。
- **下次进来**：白名单命中 + 本地缓存 → 直接出缩略图，"绑定路径"兑现。
- **不加文字通知**（用户已定：图片 fade-in 本身就是反馈）。

### 2.5 本地快场景的进一步优化（M3 关联，次要）
- 对本地/LAN（如 m64 走 LAN）大图库：阶段1 = **LRU 内存缓存 + 后台解码 + 进入目录即预取**（`nemo-thumbnail-memory-cache.[ch]`）。
- 解决：①主线程同步解码卡 GUI ②无跨导航内存缓存（每次重进=重解码）。
- Zstd / 原始像素 framebuffer 只对本地的**冷启动**有意义；**网盘场景跳过格式讨论**（缓存结果在本地、瓶颈是网络延迟）。

---

## 3. 下一步实现优先级

### P0（本次 objective 主体）—— 远程缩略图按需生成
1. **验证**：rclone FUSE 上 `metadata::` xattr 能否持久化（实测）。
2. **fork 白名单持久化**（`nemo-rclone-thumbs.c` + `~/.config/nemo/rclone-thumbs.json`）：
   - 按目录路径前缀匹配，`nemo_file_should_show_thumbnail()` 开头优先查它，
     命中即 `return TRUE`，再回落 GVFS 元数据 / 全局偏好。
   - 持久化入口：`nemo_window_slot_set_show_thumbnails()`（视图菜单的
     "Show Thumbnails" 开关）——远程目录开关写入白名单，本地目录不改。
   - **已完成 + 编译通过**（模块编进 `libnempo-private`，符号对全量目标可见）。
   - 待做：**端到端验证**（xattr 实测 + 白名单 add/remove/is_enabled 单测 +
     真机浏览 m64 勾选缩略图后下次命中）。
3. **后台解码**：缩略图解码挪到线程池，主线程不栅格化。
4. **入口**：确保远程目录可触发"生成缩略图"，生成即绑定路径。

### P1（v1 完善）—— 写回进度 / 后台上传监控
- 拦截 Nemo copy/move 写 FUSE 路径，区分 `NEMO_COPY / RCLONE_JOB / VFS_UPLOAD`。
- `vfs/queue`、`core/stats` 后台上传监控，状态栏显示。

### P2 — systemd 挂载管理
- `rclone-mount@.service` + 单挂载 `--rc` socket，crash 重启、登出不 umount（可选）。

### P3 — 并发/预取/视觉打磨（M3/M4）
- 视口预取、每目录并发池、超时看门狗、`pricing.css`、每文件夹 view-state。

---

## 4. 验证协议（重要：不打扰用户工作）

- **截图测试必须挪到 workspace 8**：用 `hyprctl` 把 nemo 窗口移到 ws8 后再 `grim` 截图，
  不要把窗口弹在当前 workspace（会挡住用户的正常工作）。
- 挂载验证：`nemo rclone://m64/` 应自动挂载 m64 + 打开窗口；`fusermount3 -u <挂点>` 后再次点击应可重新挂载。
- 挂载检查：`mount | grep nemo-rclone`；日志 `~/.cache/nemo-rclone/*.log`。
- 收尾：`nemo -q` 后重启正常 `nemo`，清理测试残留窗口。

---

## 5. 构建与安装
```sh
cd /home/roychong/app/nemo
ninja -C build
sudo ninja -C build install && sudo ldconfig
nemo -q   # 重启生效
```

---

## 6. v2 计划（2026-11-10）—— 从“能挂载”到“全能文件管理器”

### 6.1 用户当前优先级
1. **Termux / SFTP favorite 挂载失败**（当前最痛，先修）。
2. **进度窗口一次只能跑一个任务**；需要统一的 Job 查询页面 + 美化进度界面。
3. **FUSE 表现**：慢网盘不卡、不假死、进度真实。
4. **跨电脑访问/复制**：任何 rclone remote 都能像本地盘一样用。

### 6.2 根因（已用代码 + 实机验证）
- **Termux 挂载失败不是 mount 失败，而是路径语义错误**：
  - favorite 里存 `/` → `format_path()` 生成 `remote:/`。rclone SFTP 把前导 `/` 当**服务器绝对根**（Android `/`，不可读）；
    留空才是 `remote:`，rclone 会落到 **Termux home** `/data/data/com.termux/files/home`。
  - 实测：`rclone lsd gt20:` 成功；`rclone lsd gt20:/` → `permission denied`；
    `rclone mount gt20: …` 能列出 home；`rclone mount gt20:/ …` 挂载点 `ls` 返回 EPERM。
  - 浏览对话框也从 `/` 起步，`path_join()` 永远补前导 `/`，所以“Add favorite”会重复制造同样的坏路径；
    且 Add 对话框当前**拒绝空 path**，用户无法表达“remote home”。
  - `gt20` 还有一条 `/home` favorite，Termux 上不存在，属于脏数据。
- **进度单进程**：`RcloneWindow` 只有一个 `self->process`、一个 `job_progress`、一个 `job_log`；
  第二次 Start 会覆盖第一个，两个回调写同一组控件，`finish_job()` 还可能清掉第二个进程。
- **FUSE 假进度**：copy 进 `--vfs-cache-mode full` 挂载点时，Nemo 看到的是写入 VFS cache 的字节，
  上传还没开始；Nemo 自带 File Operations 其实支持多任务，但这里的“100%”不代表远端完成。
- **FUSE 假死风险**：`nemo-directory-async.c` 的 `g_file_query_info_async` / enumerate 没有超时，
  网络断了会一直转圈；全局 `MAX_ASYNC_JOBS=10`，没有 per-mount 限流。

### 6.3 目标架构（细节已定）
**浏览走 FUSE，传输走 rclone 原生任务，统计走 RC socket，UI 统一到一个 Jobs 页。**

```
Nemo 视图 ── FUSE mount ── rclone VFS ── remote
   │
   ├─ Nemo File Operations (本地/挂载复制) ─┐
   ├─ rclone transfer job (copy/move/sync) ─┼─→ NemoRcloneJobManager ─→ Jobs 页
   └─ mount VFS upload queue (vfs/queue) ───┘        │
                                                     └─ 状态图标 / 通知 / 侧边栏
```

- **每个 transfer job 一个独立 rclone 进程**，带自己的 Unix RC socket：
  ```
  rclone copy SRC DST --rc --rc-addr unix://$XDG_RUNTIME_DIR/nemo-rclone/jobs/<id>.sock \
    --rc-no-auth --stats 1s --log-file ~/.cache/nemo-rclone/jobs/<id>.log ...
  ```
  UI 轮询 `core/stats`（bytes/totalBytes/speed/eta/transferring[]），取消用 `core/quit`。
  这样 job 并行、进度独立、Nemo 关窗不影响；sidecar JSON 记录 src/dst/mode/pid/socket，重启后可重连。
- **每个 mount 也加 `--rc` socket**，轮询 `vfs/queue` + `vfs/stats` + `core/stats`，
  把“上传队列 / 缓存大小 / 上传中文件”显示成 Jobs 页里的第二类任务。
- **Jobs 页同时聚合 Nemo 自己的 `NemoProgressInfo`**：两个本地/挂载复制也会同时显示，
  彻底解决“同时复制两个路径只看到一个”的体验问题。
- 不引入中心 `rclone rcd` 守护进程（先降低风险）；per-process RC 已能提供统一查询。
  以后若需要跨重启的完整 job 历史，再迁移到 systemd `rclone rcd`。

### 6.4 里程碑（按顺序推进，每步 commit + push）
- **P0 Termux / 路径语义修复 — ✅ 已完成（2026-11-10）**
  - `format_path`：空 path → `remote:`；相对 path → `remote:rel`；绝对 path → `remote:/abs`。
  - 浏览对话框从 home（空）起步；`path_join` / parent 保留相对/绝对语义；UI 用 `~` 显示 home。
  - Add Favorite 允许空 path，加 **Home** 按钮与提示；启动时自动修复 Termux 脏 favorite
    （`/` 或 `/home` → home），保留 `m64:/home/roychong` 这类真实绝对路径。
  - 新增 `nemo-rclone-path.[ch]` 统一路径语义 + `test/rclone-path-test.c`（`meson test` 可跑）。
  - 修复 rclone.conf 解析 bug：真实配置是 `key = value`（带空格），旧代码按 `key=` 匹配，
    导致 Termux 检测恒为 false、自动修复从未生效。
  - 同一 remote+path 的重复 favorite 自动去重。
  - 实测（lgv35，Termux/SFTP 8022）：favorite 自动修复为 `""`，`rclone mount` 成功挂到
    `~/.local/share/nemo-rclone/mounts/lgv35`，`ls` 列出 Termux home，unmount 干净退出。
- **P1 并行 Job + 统一 Jobs 页**
  - 新 `src/rclone/nemo-rclone-job.[ch]`、`nemo-rclone-job-manager.[ch]`、`nemo-rclone-rc.[ch]`。
  - 多 job 并行，互不覆盖；每个 job 独立进度/日志/取消/重试。
  - Jobs 页：顶部总览（N 个任务 / 总速度 / 剩余）、任务列表（状态、进度条、速度、ETA）、
    详情（当前文件列表、日志、取消/重试、打开目标）。
  - 聚合 Nemo 自带 File Operations；关窗后台继续；完成/失败发 `GNotification`；侧边栏 Transfers 显示计数。
  - 进度界面美化：CSS class、状态色、图标、平滑更新（≤4Hz）、不再闪。
- **P2 RC 统计 / FUSE 上传可见**
  - mount argv 加 `--rc` socket；JobManager 轮询 `vfs/queue`、`vfs/stats`。
  - copy 进挂载点后，Nemo 进度到 100% 时 Jobs 页继续显示 “Uploading to <remote>” 直到 queue 清空。
  - 后续可加“直接传输”开关：检测到目标是 rclone mount 时，copy/paste 直接起 rclone job（真进度、避免双缓存）。
- **P3 FUSE 性能 / 稳定性**
  - mount profile（balanced / browse-fast / upload-heavy / low-power-SFTP）存 favorite JSON；
    增加 `--vfs-cache-max-size`、`--vfs-cache-max-age`、`--vfs-write-back`、`--buffer-size`、
    `--timeout`、`--contimeout`、`--low-level-retries`；Termux 手机降 `--sftp-concurrency` / `--transfers`。
  - `nemo-directory-async.c`：慢远程 IO 超时 watchdog（取消并显示重试）、per-mount 并发上限；
    thumbnail 路径避免主线程同步 `g_file_query_info`。
  - path-keyed view-state（zoom/sort）存 JSON，修 remount 后缩放重置。
- **P4 跨电脑 / 深度集成**
  - 侧边栏列出全部 remotes（不只 favorite），点开自动挂 home/默认路径。
  - 右键 **Send to…** / **Copy to remote…**：选目标 remote/path，起 job；多选按 batch 显示。
  - `--server-side-across-configs` 可选；跨机器复制走 rclone 任务，不再依赖 Nemo 的 FUSE copy。
  - systemd user mount 单元（Restart=on-failure、登出清理、per-mount rc socket）作为 P3/P4 收尾。

### 6.5 验证清单
- `rclone lsd gt20:` 成功；`nemo rclone://gt20/` 打开的是 Termux home，日志无 `permission denied`。
- 同时启动两个 transfer（gt20→m64、m64→d3180），Jobs 页两行独立进度；取消一个不影响另一个。
- 复制 1GB 到挂载点：Nemo 到 100% 后 Jobs 页仍显示 upload queue，queue 归零后任务完成。
- 断网/iptables DROP 后打开远程目录，30s 内出现错误/重试，不无限转圈。
- 关掉 Nemo 再打开，Jobs 页能重连仍在运行的 job/mount socket（至少显示活动任务）。

