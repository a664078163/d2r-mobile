# 技术架构

最后更新：2026-10-27

## 1. 项目定位

D2R Mobile 是面向 iOS 和 macOS 的《Diablo II: Resurrected》兼容层。项目让用户使用自己的游戏文件，在 Apple 平台原生运行游戏程序，并通过宿主层提供系统、图形、音频和输入能力。

## 2. 核心技术路线

| 业务主线 | 当前方案 | 关键约束 |
|---|---|---|
| 游戏运行 | 使用 Apple 平台上的原生 aarch64 程序执行，针对少量平台不兼容指令进行改写 | 不包含或分发游戏内容；资源由用户自行准备 |
| 平台宿主 | 由 C、Objective-C 和 Apple 平台 API 组成宿主层，提供文件、系统调用、音频、输入等能力 | 保持与原程序的接口契约一致 |
| 图形渲染 | 将游戏图形 API 映射到 Metal，着色器离线转换并预编译 | 旧设备需要额外纹理处理；运行时避免现场编译 |
| 移动交互 | 在 iOS 上实现触摸、手柄、面板布局和安全区适配；主攻击键与四个技能键支持长按拖动交换或拖到空白处置空，空位保留可命中的 `➕` 槽位 | 触摸与手柄模式需要保持界面状态一致；布局用归一化坐标保存，不写入游戏存档 |
| 验证与构建 | `dm/` 下的工具、测试和验证脚本支持资源处理、静态检查及设备行为验证；macOS 资源生成通过 `xcrun --sdk macosx clang` 使用完整 Xcode SDK | 具体入口以各子目录 README 和脚本参数为准；免费 Apple ID 签名依赖在线真机 |

## 3. 数据与运行链路

用户提供的游戏文件经过资源准备工具处理后，生成应用可读取的资源；应用启动后由宿主层初始化平台能力，原生游戏程序通过宿主接口进入主菜单和游戏流程。图形请求经图形适配层转换为 Metal 调用，输入事件经触摸或手柄层转换为游戏可消费的输入状态。

## 4. 当前架构边界

- 仓库不携带游戏程序、艺术资源、文本、数据表或着色器原件。
- 项目面向离线单机运行，不提供联机服务。
- macOS 资源生成不直接依赖命令行工具链默认路径，避免 Command Line Tools 与完整 Xcode SDK 不一致导致标准头文件或架构解析失败。
- 算法题或通用示例不属于现有业务主线，新增此类内容前需要明确独立目录或独立项目边界。

## 5. 触摸绑定契约

空技能位由覆盖层显示加号，触摸层依据同一份原生技能快照判断是否进入绑定。主攻击键（`DM_BTN_A`）也遵循这条契约：有技能时保持连续攻击，空位点击才打开技能选择；第二技能组继续附带 `ZL` 修饰。主攻击键的绘制半径和圆形命中半径统一为 `60*k`，安全区布局按该半径计算。

可编辑布局只作用于主攻击键和四个技能键。按住约 0.5 秒并拖动超过阈值后进入编辑态：拖到另一个可编辑键位时交换两者的触摸位置及空/非空状态，拖到空白处则将源槽置空但保留原位置、半径和命中区域；空槽显示 `➕`，短点击进入技能绑定，长按拖动仍可参与交换。松手提交后写入 `NSUserDefaults` 的 `dm_touch_layout_v1`。技能组切换键仍保留为恢复默认布局的入口，布局状态不进入游戏存档。

空槽的短点击由 C 触摸状态机在松手时产出一次性绑定请求，iOS 层只取走并交给宿主打开技能树，不再在 UIKit 中按另一套时长、位移或面板状态重新判定。该请求允许 0.6 秒、14 点以内的自然点按；超时、明显移动、长按拖动和面板态均不产生请求。这样能避免技能刚释放后的事件时序使 `➕` 点击被静默丢弃。

宿主侧的技能命中坐标按绑定事务隔离：开始新一轮绑定、取消绑定或绑定框完成时，都会清掉上一轮的命中坐标、命中控件引用和加点标识。命中坐标不是跨事务的持久状态，避免第二个空槽进入“等待选择技能”后误把上一轮技能当成当前点击并自动确认。这个契约由宿主触摸回归中的跨轮命中测试锁定。

绑定完成后，宿主线程会先发布“已回到游戏态”，再通过主队列异步通知 UIKit 更新导航状态。触摸开始入口会同步读取这份已发布状态；如果主线程回调尚未执行，先校正本地导航态再分配触摸槽位。这样第二个空槽的快速点击不会被旧的面板态当成界面指针而丢弃绑定请求。该同步只修正输入状态，不改变宿主绑定状态机、游戏存档或资源链路。

## 6. 多游戏 profile 与 ACNH 适配

游戏 profile 是在进程启动时选择的一组游戏差异配置；未设置时保持 D2R 默认行为。ACNH 适配沿用 D2R 的宿主主链，只在确有二进制证据的服务契约处分流。

| 链路 | D2R 基线 | ACNH 当前方案 | 当前结论 |
|---|---|---|---|
| 装载与启动 | ELF/NSO 映射、动态重定位、SDK 初始化、`DT_INIT/init_array` | 复用同一 loader；补充 `DT_RELR` 解析和 profile 入口 | ACNH 已通过启动阶段 |
| 文件与服务 | 宿主 HLE 提供 RomFS、线程、HID、`sm:` 等能力 | 复用通用 HLE；ACNH 的 TIPC/CMIF 和 `nvdrv` 会话使用 profile 内 opaque handle | Base + Update RomFS 均已准备；真实资源访问尚未进入首个 GPU 帧，后续服务按调用证据扩展 |
| iOS 产品入口 | `Sources` 全量编译，Bundle ID 不参与游戏选择，资源页和 shader prep 属于 D2R 产品路径 | 独立 `project_acnh.yml` 与 `DM_IOS_GAME=acnh` 构建分支；编译宏 `DM_IOS_GAME_ACNH=1` 在编译期选择 profile，ACNH 使用专用 RomFS 修复页 | 无签名和免费账号分支均已编译链接通过；Xcode GUI 已用 Personal Team 生成 Release Archive 并安装到 iPhone；ACNH 工程不携带 D2R overlay、MSL、save_ref 或下载扩展，Bundle ID 与 D2R 保持隔离 |
| NVN 设备契约 | Tegra X1 设备表，API major 53 | 共享其余设备能力，只把 `NVN_DEVICE_INFO_API_MAJOR_VERSION` 返回为 55 | 这是已验证的 ACNH 差异；不伪造 GPU 对象 |
| 图形执行 | `nvn_mtl.m` 从系统取得 Metal 设备并回放 NVN 命令 | 复用 `front_run → CAMetalLayer → mtl_init → NVN replay`；ACNH 前端只保留 Metal 输出和物理手柄 | 宿主 smoke 已通过 shader 初始化且无 NVN 红线；最新 guest-view 修复需重新生成 iOS 包，首个真实 `nvnProgramSetShaders`、提交和首帧仍待真机验证 |
| 着色器输入 | D2R 使用包内 `BTCDBTCD` 记录、控制段键和 PC 反射作答表 | `dm/tools/acnh_shader.py` 解析 Yaz0 + SARC/BAHS + BFSHA/BNSH，输出与共享 `dmsh_build.py` 相同的 BTCD 记录契约；不可翻译的 1 字节表哨兵写入跳过清单 | 3,480 个可翻译 shader（顶点 1,382、像素 2,097、计算 1），另有 1 个哨兵；同键冲突为 0；来源列使用相对路径 |
| 离线翻译与回放键 | 共享 Ryujinx → SPIR-V → spirv-cross → MSL → metallib；D2R 键为控制段哈希 | ACNH 复用同一翻译器和构建器，键为 `sha256(完整控制段 + 完整代码区)[:16]`；运行时 `prog_t` 保存每阶段前 8 字节；没有 Metal 编译器时沿用 `nometal_run.py`，把 MSL 放入库文件并由 `DM_SHELL` 在 iPhone 首次编译 | 3,480/3,480 完成 SPIR-V、MSL、反射和 iOS 源码库；链版本 `deb66c568156`、`c1_full.tsv` 一致；本机 Metal Toolchain 缺失不再阻塞该路径，但设备首帧尚未验证 |

关键原则是：profile 只承载游戏协议差异，窗口、Metal 设备、NVN 回放、资源生命周期和线程模型仍由 D2R 的既有实现负责。临时 BRK/SIGTRAP 探针已删除，不进入正式架构。

ACNH 资源采用“Base 优先、Update 覆写”的两层契约：App 的 `Application Support/acnh/romfs` 是本体层，`Application Support/acnh/update` 是升级层；启动时以 `DM_ROMFS_OVERLAY=<update>:<base>` 把升级层放在前面。任意一层存在且通过 `Shader/Park_UBER_All.Nin_NX_NVN.szs` 锚点检查即可进入启动链；完整游玩仍要求两层都可用。当前来源基准为 Base 66,790 个文件、6,640,340,166 字节，Update 103,400 个文件、10,415,907,860 字节。

资源传输不再逐文件复制。Mac 端的 `dm/tools/acnh_pack.py` 按排序后的源文件生成 `DMACNHPK` v1 `.dmp` 包；包内文件可原样存储，也可使用 raw RFC 1951 DEFLATE，并记录原大小和 SHA-256。完整 Update 当前分为 10 个约 1 GiB 的批次，Base 分为 7 个批次，批次头记录 `pairIndex/pairCount`。设备优先扫描 `Library/Caches/acnh-packs`，兼容旧的 `Documents` 路径；每个根目录必须先通过包头检查得到完整连续批次，不完整的 Caches 不得遮蔽 Documents 中的完整集合。每次导入仍视为“一层完整包集合”：从空的 `acnh/{romfs,update}.dm-pack-staging` 开始，逐个校验路径、长度和摘要；全部成功后才写完成标记，并以目录 rename 整层替换正式目录。启动时完成标记还会重新统计正式目录的普通文件数和字节数，删除或截断资源会被拒绝。失败或取消会清理 staging，不合并或删除已有正式层。导入工作运行在 GCD 后台线程，因此 C 导入器不持有大型栈缓冲区；流式复制使用显式堆分配并释放，回归测试固定以 512 KiB 线程栈导入超过 1 MiB 的原样条目。

资源包的单条目原始大小上限由 Python/C 共同固定为 64 MiB；`--esize` 可在 1–64 MiB 之间收紧限制，打包和 `verify-set` 都按该值检查，不能只把它当作清单注释。`DMACNHSET` v2 的清单文件统一命名为 `base.set.json` / `update.set.json`，层值统一为 `base` / `update`；汇总使用 `fileCount`、`originalBytes`，包项使用 `fileCount`、`packBytes`、`pairIndex`、`pairCount` 和 `sha256`。设备运行时目录仍保留 `romfs` 这一历史路径名，但不再作为 v2 清单层名。

[已废弃，2026-10-23] 分批导入时把新包增量合并到已有正式层：该模式让“整层校验后原子替换”不再成立，且 staging 失败时可能影响已发布资源。完整小包端到端验证通过后改为单层完整包整层替换。

[已废弃，2026-10-23] 大层通过单个约 10 GB 的 `.dmp` 做整文件传输：首次设备传输因 CoreDevice 连接中断留下 0 字节占位，无法可靠续传，也不适合作为长期重试单位。替代为同一完整层的多个约 1 GiB 批次；设备端仍等全部批次齐全、路径/大小/摘要校验完成后才发布，保持整层原子替换语义。

[已废弃，2026-10-22] 用 Finder `DeviceFS` 的逐文件比对判断资源已完成：它可能显示 Mac 侧缓存，真实 ACNH 容器的 Update 当时只有 39,766/103,400 个文件。资源核验统一以 `devicectl` 查询的真实 `appDataContainer` 清单为依据；小包端到端验证已经把 5,000 个资源文件加完成标记正确写入该容器。Base title key 由本体 NSP ticket 按 D2R 既有链路导出后使用，不写入项目。

### 6.1 ACNH shader key 契约

ACNH `NVNshaderData` 的控制段声明长度不是公开 NVN 的固定 `0x880`；真实运行时观测到 `0xb31`、`0xf68` 和 `0x9c8`。宿主按有界 `0x2000` 窗口读取 `control_length`，并与完整 code 区一起计算 shader key；key parser 回归测试覆盖扩展长度和上界拒绝。此前错误上限会让 `ProgramSetShaders` 留下空 key，导致设备首帧黑屏。

### 6.2 ACNH 音频 renderer 宿主边界

ACNH 的 `audren:u` 启动链复用通用 CMIF/HIPC 解析：work-size 使用扩展参数计算，AArch64 receive-buffer descriptor 按 3 words 解码，command 6 返回 renderer system event。Mac smoke 使用 ACNH profile 的无声 renderer 后端接管 `Open/RequestUpdate/Start/Execute/Stop/CloseAudioRenderer` 生命周期，保持 `AudioRendererHandle` 不透明；D2R 不进入该分支。该边界已通过 Update RomFS smoke，未出现音频 Break 或未实现 SendSync。

### 6.3 ACNH 宿主改写例外

ACNH 的真实 `main` 在构建标识 `ff1d1c05670db6021c85b624a710b963` 的 `main+0x37c7c40` 使用原生 `sub x18, x23, x18` 参与 allocator 地址计算。通用 `x18` 线程槽改写若覆盖该点会破坏 free-node 链并触发 `main+0x37c7ab0` 空指针崩溃，因此 `patch_nso.py` 对该单点保留原指令。[已解除，2026-10-27] 早期 smoke 到达受控 `Break#1` 的阶段已被 audren:u 和宿主无声 renderer 边界修复取代；当前同一 Update RomFS smoke 无 Break、无 crash，并能优雅退出。

ACNH iOS 壳的产品边界由编译期宏 `DM_IOS_GAME_ACNH=1` 决定，而不是靠 Bundle ID 猜测。ACNH 构建只嵌入 `main`、`sdk`、`subsdk0` 三个改写模块；`run.sh` 复制 ACNH 专用 shader map、当前链版本的 `b-<V>/` 和 `c1_full.tsv`，并把 `Gen/msl` 纳入 Xcode 资源。D2R 的触控 PNG、RomFS overlay、标准存档和资源下载扩展不进入 ACNH 安装包。[已废弃] 生成空 shader 共享表的过渡分支已在 2026-10-21 删除，原因是它会让真实 ACNH shader 库即使存在也无法被运行时索引。

### 6.4 ACNH 纹理 view 的客体内存边界

ACNH 在真实调用点只为临时 `NVNtextureView` 分配 16 字节，并把紧邻的内存用于 `NVNcopyRegion`；D2R 风格的完整宿主 `view_t` 不能直接写回客体地址。旧方案把约 36 字节状态写入客体 view，覆盖了后面的 region，使纹理写入收到宽高为零的区域并表现为 `TextureWriteTexels` 参数非法。当前方案是“客体 16 字节 cookie + 宿主 `view_box_t` 完整状态”：所有 setter/getter、比较、view offset、纹理读写/flush、descriptor pool 注册和 command snapshot 都从宿主状态读取，并用 mutex 保护状态更新。

| 方案演进 | 调整原因 | 当前结论 |
|---|---|---|
| 完整 `view_t` 直接写客体 → 16 字节 cookie + 宿主状态表 | ACNH 的临时 view 与 `NVNcopyRegion` 相邻，完整写入会破坏 region 的宽高和层数 | `t_nvn` 的相邻内存回归 128 项全绿；ACNH smoke 不再出现 `TextureWriteTexels` 红线 |

## 7. Diablo III profile 与资源链

D3 profile 是面向《Diablo III: Eternal Collection》的独立运行配置；它复用已验证的 D2R 宿主能力，但不把 D2R 的固定地址业务补丁、触摸绑定、云存档和 RomFS overlay 误注入 D3。

| 链路 | D2R 基线 | D3 当前方案 | 适用边界 |
|---|---|---|---|
| 游戏选择 | 默认 `d2r`，宿主按 argv/环境装载 | `DM_GAME_PROFILE=d3`，支持 `d3/diablo3/diablo-3/diablo_3`；iOS 用 `DM_IOS_GAME_D3=1` 固定壳能力集 | 未设置时仍是 D2R；Bundle ID 只用于产品隔离，不作为运行时判断 |
| NSO 改写 | 通用 x18、TPIDR、SVC、计时器 + D2R 构建号 CUTS/OBS | 强制 `native` ruleset，保留平台改写，跳过 D2R CUTS/OBS；D3 的 `mov sp, x18` 动态栈切换使用无栈 trampoline 保留原栈语义 | D3 Update ExeFS 已确认包含 `main`/`sdk` 并生成真实 patched binaries；固定地址业务补丁仍须按 D3 自身证据新增，不能复制 D2R 偏移 |
| 模块与启动 | `main sdk`，按 D2R 产品清单启动 | 默认 `main sdk`，`DM_D3_MODULES`/`DM_MODULES` 可覆盖；iOS `project_d3.yml` 独立生成 | 实际模块名以 D3 ExeFS 为准；缺模块时产品脚本应失败，不静默降级 |
| 文件与资源 | `DM_ROMFS` + D2R overlay、存档和下载扩展 | `Application Support/d3/{romfs,update}` 或 `Documents/d3/{romfs,update}`；两层时 `update:base` | 至少一层用于启动页检查；完整游玩需要与真实 D3 文件契约一致的 Base/Update |
| 前端与输入 | D2R 触控层、云存档和生命周期 | 通用 `CAMetalLayer` 输出 + 物理手柄；跳过 D2R 触控订阅、云存档和资源自检 | D3 专用 UI/存档能力必须在获得游戏内调用证据后再增加 |
| 图形与 shader | D2R 控制段哈希映射和既有 iOS metallib | D3 必须显式提供独立 `DM_D3_SHADER_MAP` 与 `DM_D3_MSL_IOS`；构建脚本拒绝回退到 D2R 映射或缓存，待真实 RomFS shader 容器解析后再生成 D3 map/key 规则 | 当前旧包中的 B-route 仍是临时 D2R 缓存，已标记为不可放行；未完成 D3 map、资源锚点和首帧验证前不构建可安装候选包 |

方案演进记录：`D2R 全量业务路径 → profile 边界 + D3 真实 ExeFS 启动壳`。Update NCA 通过现有 NSZ/ticket 链路在本地进程中读取，提取 `main`/`sdk` 后立即删除临时 NCA；title key、ticket 和临时解包文件不进入仓库。当前结论是先保证真实模块装载、资源分层、Metal 输出和手柄路径可复用，再根据 D3 首个真实调用补充 HLE；Base/Update RomFS 和 D3 shader map 仍是独立资源链。

空技能位只有一个判据，绘制与输入共用：覆盖层画不画 `➕`、手势层点下去进不进绑定，都读 `dm_ta_slot_empty`。它把两个来源取并集——玩家拖到空白处留下的本地空位，和游戏自己报的空槽（槽里技能号为 `UINT32_MAX`）。两条来源分别只覆盖一半场景，任一条说空，画面就是 `➕`，那一下就必须进绑定。手势层每拍由前端把这份读数发布进 `empty_targets`（`dm_tc_set_empty_targets`），不在点击那一刻另读一份快照。快照只是旧了（游戏停拍、切后台、长卡顿）时退回最后一次取到的内容，因为绑定没变、只是暂时没人刷新；面板态不参与空位判定，那五个位置上是虚拟手柄圆钮。绑定恢复槽位时同时清掉 `manual_empty` 与 `empty_targets` 对应位，否则下一次正常施放会被吞成一次绑定。

| 方案演进 | 调整原因 | 当前结论 |
|---|---|---|
| [已废弃] 用 Bundle ID 识别 ACNH → 编译期游戏 profile | 改包名或签名描述文件会让进程误走 D2R 资源页、D2R 着色器预编译和 D2R 触控业务 | `DM_IOS_GAME_ACNH=1` 在编译期固定能力集；启动阶段只设置 `DM_GAME_PROFILE=acnh` 供宿主共用 |
| [已废弃] ACNH 复用 D2R 资源下载页 → ACNH 专用 RomFS 修复页 | D2R 页面查找托管资源包和下载地址，ACNH 当前资源来自用户自行放置的两层本地目录 | ACNH 只说明 Base/Update 目录并重试；`DM_ASSETS_TEST`、`DM_TEST_BA`、ASTC、iCloud 和分享日志分支不进入 ACNH 启动路径 |
| [已废弃] 只允许 `F1-F4` 空位绑定 → 主攻击键漏出到普通手势层 | 主键显示加号时点击无效，无法恢复攻击技能 | 主攻击键与四个技能键共享绑定命中判定；绑定入口仍由 `dm_ui_bind_begin` 驱动 |
| 原固定触摸位置 → 可编辑位置状态机 | 用户需要修复误操作后的按键布局，同时不能改变技能绑定和存档 | 主键与四个技能键使用归一化位置、长按拖动交换/置空、空位保持可见可命中、NSUserDefaults 持久化；2026-10-15 生效 |
| 上一轮技能命中状态跨绑定复用 → 按绑定事务清理 | 第二个空槽可能把上一轮技能当成当前选择，表现为技能页不显示或 `➕` 无响应 | `dm_ui_bind_begin`、取消和完成路径清理命中状态；2026-10-16 生效 |
| [已废弃] 仅依赖 UIKit 异步 `navigationMode:` 回调 → 触摸开始前同步宿主发布态 | 绑定完成后立即点击第二个空槽时，主线程可能仍保留面板态，第二次点击被当作面板指针吞掉 | `touchesBegan:` 先按 `g_touch_navigation` 校正本地状态，异步回调保持幂等；2026-10-16 生效 |
| [已废弃] 绘制认游戏读数、输入只认 `manual_empty` → 两侧共用 `dm_ta_slot_empty` | 游戏里删掉的技能只改游戏读数：槽位画着 `➕`，点下去却被当成普通施放，表现为“点加号没反应”；`manual_empty` 只覆盖“拖到空白处”这一半场景 | 空位判据收敛为一份读数，前端每拍发布 `empty_targets`；恢复槽位时两处一起清；2026-10-16 生效 |
