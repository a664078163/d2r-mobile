# 项目进展

最后更新：2026-10-27

## 1. 当前里程碑

| 日期 | 业务主线/模块 | 完成内容 | 状态 | 问题或下一步 |
|---|---|---|---|---|
| 2026-10-14 | 项目文档 | 初始化架构与进展文档，记录当前项目定位和技术主线 | 已完成 | 后续按实际架构或里程碑变化维护 |
| 2026-10-14 | D2 游戏文件与 iOS 构建 | D2R 3.3.94012 文件校验通过；使用 Xcode 个人团队完成签名、安装和首次启动；已推送 README 要求的 18 个资源档案并进入游戏流程 | 已完成 | 后续可在真机继续验证触摸、存档和长时间运行 |
| 2026-10-15 | iOS 触摸控制与设备部署 | 完成主攻击键放大、右下角键位拖动交换/置空/恢复、空槽技能绑定与绑定成功后的槽位恢复；个人团队 `YF8CHNSGUK` 已完成一次 Release 覆盖安装，未启动 App、未触碰沙盒、资源或存档；空槽绑定可靠性回归已达 51/51（1644/1644） | 已完成代码与完整单测 | 仍需本轮修复的 Release 构建和真机验证 |
| 2026-10-16 | 空槽绑定构建 | 已定位 D2R RomFS 到 `~/dm-work/D2/data/switch/3.3.94012/Update/RomFS`；此前误判 Python 虚拟环境位置，实际环境在 `~/dm-work/D2/build/cache/dm/venv` | 已解除 | 已按最新代码生成 r44 三组改写产物 |
| 2026-10-16 | iOS 真机构建与部署 | 触控回归 51/51（1644/1644）；r44 Release 包使用 Personal Team `YF8CHNSGUK` 签名并已通过 Device Hub 覆盖安装到已核对的 iPhone，Bundle ID 保持 `com.haifliu.d2rmobile`；未启动 App、未触碰沙盒、资源或存档 | 已完成 | 后续由用户在真机验证本轮触控行为 |
| 2026-10-16 | 重复空槽技能绑定 | 进一步定位到绑定完成后宿主已发布游戏态、但 UIKit 导航回调仍在主队列等待的竞态；新增“连续两轮绑定 + 页面切换 + 完成通知”宿主回归（16 项），触摸回归仍为 51/51（1647/1647）；触摸开始前同步宿主导航态；Release 包已完成签名并通过 Device Hub 同 Bundle ID 覆盖安装 | 已完成构建与部署 | 待用户在真机立即连续点击两个空槽验证实际交互；不以安装成功代替行为验证 |
| 2026-10-16 | 空槽判据收敛（本轮根因） | 前三次修复都只覆盖“拖到空白处”这条路径；用户症状是游戏内删空技能槽——绘制读游戏读数画 `➕`，输入却只认 `manual_empty`，于是点下去被当成普通施放。改为绘制与输入共用 `dm_ta_slot_empty`（本地空位 ∪ 游戏报空），前端每拍发布 `empty_targets`，恢复槽位时两处一起清；新增 19 项触摸回归与 8 项宿主回归，iOS 全套测试通过（52/52、1666/1666），两个 Objective-C 源文件通过真机工具链语法检查；Release 包已覆盖安装 | 已完成构建与部署 | 待用户在每个空白槽上点击 `➕` 验证技能页是否打开 |
| 2026-10-20 | ACNH 适配启动链 | 以 D2R 完整链路为基线，完成 ACNH `DT_RELR`、SDK/init-array、`sm:`、TIPC/CMIF、`nvdrv`、NVN API major 55 和 Prepare Thread 的启动期验证；并将 ACNH SVC 分发收束到 profile 分支，D2R 回归可继续走原路径 | 启动期代码链已完成 | 后续转入 iOS 产品入口和真实资源，不再重复启动探针 |
| 2026-10-21 | ACNH 资源与 iOS 壳 | Update `ExeFS`/`RomFS` 已准备，RomFS 约 9.9 GB；新增 `DM_IOS_GAME=acnh` 独立构建入口、`project_acnh.yml`、编译期 ACNH profile、专用 RomFS 修复页和 `DM_NO_SIGN` 无签名验证；Release 无签名构建与免费账号分支均通过，三个模块嵌入成功，D2R overlay/MSL/存档/下载扩展未进入包 | 已得到可编译的 ACNH 壳 | 真机未连接；首个 GPU 帧尚未运行 |
| 2026-10-21 | ACNH 着色器与 NVN 键 | 复用 D2R 的 `dmsh_build.py` 和 Ryujinx 翻译器；解析 Update shader 容器得到 3,481 个唯一 shader（1,383/2,097/1），映射表改为相对路径；单 shader 已完成 SPIR-V→MSL→反射冒烟；运行时增加 ACNH 完整控制段+代码键，并接入 ACNH iOS map、MSL、c1 资源打包 | 解析、共享翻译链、运行时键和打包接线已完成 | 本机缺少 Metal Toolchain，无法生成真实 iOS/macOS metallib；Base title key 仍缺；尚未运行真机首帧 |
| 2026-10-21 | ACNH 无 Metal 工具链产物与 Release 编译 | 沿用 D2R 已验证的 `nometal_run.py`/`DM_SHELL` 路径；过滤 1 个无法翻译的 1 字节表哨兵，生成 3,480 个 iOS 源码库、3,480 个反射 JSON 和 `c1_full.tsv`；ACNH 工程无签名 Release 编译链接通过，链版本为 `deb66c568156` | 已完成 | 尚未签名装机；Base title key 和真机首帧仍待解除 |
| 2026-10-21 | ACNH ticket 解密与 Base 资源 | 修正对 D2R 成功链路的复用：使用现有 `nsz --titlekeys` 从 ACNH 本体 NSP ticket 导出本体 title key，再用现有 `hactool` 解出 Base ExeFS/ RomFS；Base RomFS 约 6.3 GB，`Shader/Park_UBER_All.Nin_NX_NVN.szs` 锚点通过 | 已完成 | 下一步是按 Base + Update 两层完成签名构建并进入真机首帧验证 |
| 2026-10-21 | D2R iOS 个人签名续期 | 使用已验证的 Release 包，通过 Device Hub 无线覆盖安装到 `Haifeng的iPhone`；Bundle ID `com.haifliu.d2rmobile` 保持不变，未卸载、未启动 App、未触碰资源和存档 | 已完成 | 用户可继续进行真机验证 |
| 2026-10-22 | ACNH iOS 签名 Archive | 按 `ios-local-deploy` 流程在 Xcode GUI 选定 `Haifeng的iPhone`，确认 Personal Team 自动签名，生成 Release Archive；Bundle ID `com.haifliu.d2rmobile.acnh`、Team `YF8CHNSGUK`、架构 `arm64`、embedded profile 均核验通过；未安装、未启动，也未触碰 D2R | 已完成签名准备 | 安装前还需即时确认；安装后先导入 Base/Update RomFS，再收集首帧日志 |
| 2026-10-22 | ACNH iPhone 安装 | 首次安装因免费开发者描述文件已达到 3 个 App 上限而失败；用户释放一个名额后，按已核对的 Bundle ID 重新安装成功。Device Hub 同时显示 `Animal Crossing Mobile`（`com.haifliu.d2rmobile.acnh`）和 D2R（`com.haifliu.d2rmobile`）；未启动任何 App | 已安装 | 尚未导入 Base/Update RomFS，尚未运行真机首帧 |
| 2026-10-22 | ACNH 资源导入链路诊断 | Finder 传输 2.34 GiB 单文件长时间固定在 5%，设备端大小仍为 0；关闭 Device Hub 后 `xcrun devicectl list devices` 仍因 CoreDeviceService 超时失败。已启用仅限认证用户、只读、仅共享 `/Users/haifliu/dm-work/ACNH` 的 SMB 通道；D2R 未触碰 | 传输通道已就绪 | 在 iPhone“文件”App 连接 `smb://192.168.8.172`，复制 Base/Update RomFS 到 ACNH Documents |
| 2026-10-22 | ACNH Base/Update 资源导入 | 沿 Finder DeviceFS 通道分层导入并逐文件比对：Base 66,790 个文件、6,640,340,166 字节，Update 103,400 个文件、10,415,907,860 字节；两层逐文件路径和大小差异均为 0。Base Shader 锚点 14,269,581 字节、Update Shader 锚点 14,512,785 字节均存在；D2R 未触碰 | 已完成资源层校验 | 已解除资源导入阻塞，尚未启动真机首帧 |
| 2026-10-22 | ACNH 真实容器导入纠错与断点保存 | 用户启动 ACNH 后，iPhone 镜像和真实 `Documents/boot.txt` 均显示 RomFS 校验失败。复核发现前一条“Finder DeviceFS 逐文件一致”只覆盖了 Mac 侧缓存视图；通过 `devicectl` 读取真实容器后，Update 实际只有 39,766 个文件，离 103,400 个来源文件还差 63,636 个，另有 204 个大小不一致；Base 也尚未完成真实容器导入。已把 Update 真实容器清单、缺失/大小异常清单和按顶层目录统计保存到 `run/acnh-transfer-20261022/`，并改为直接按真实容器清单差集补传 | 导入未完成，断点已保存 | 下次连接同一 iPhone 后从真实清单继续；Finder 缓存视图不能作为导入完成证据 |
| 2026-10-22 | D3 profile 与 iOS 壳 | 新增 `DM_GAME_D3`、D3 loader/HLE 过滤、`project_d3.yml`、D3 RomFS Base/Update 探测与 overlay、通用 Metal + 物理手柄前端、`d3_product.sh` 和 D3 native NSO 规则域；D3 运行时默认 `main sdk`，构建可用 `DM_MODULES` 覆盖；D3 壳以现有生成资源完成无签名 Xcode Release 编译，宿主构建与 D3 免费账号工程描述校验通过；补齐 graphics tests 因 profile 符号引入的链接依赖 | 代码链完成，资源链未接通 | D3 NSZ 尚未全量导出；未确认真实模块名、NVN 版本、shader map、服务缺口或首帧 |
| 2026-10-22 | D3 真实 ExeFS 与签名包 | 从 D3 Update NSZ 的 rights-encrypted NCA 中通过现有 `nsz`/ticket 链路提取真实 `main`（约 9.7 MiB）与 `sdk`（约 5.1 MiB）；补丁器新增 D3 `mov sp, x18` 动态栈 trampoline，生成真实 `main`/`sdk` patched binaries；`DmD3` Release arm64 使用 Team `YF8CHNSGUK` 签名，Bundle ID `com.haifliu.d2rmobile.d3`，`codesign --verify --deep --strict` 通过 | D3 可安装包已准备 | Device Hub 当前显示 `Haifeng的iPhone` 为 `unavailable`；需用户让设备靠近或插线、解锁并信任后即时确认安装。Base/Update RomFS、D3 shader map 和首帧仍未验证 |
| 2026-10-22 | D3 收口审计与构建闸门 | rights/title-key 链路确认 Base/Update 均有票据；复核发现现有 6,927 个 B-route metallib 与 D2R map/cache 相同，不能作为 D3 图形产物。`dm/ios/run.sh` 已改为要求显式 `DM_D3_SHADER_MAP`、`DM_D3_MSL_IOS`，并拒绝复用 D2R 资源；缺少 D3 图形资源时构建会明确失败。Update 容器仅完成 3.3 GiB 解压索引准备，未全量导出 RomFS（本机余量约 2.2 GiB） | 已完成审计，D3 候选包未放行 | 需独立临时空间导出 Base/Update RomFS、解析真实 shader 容器并生成 D3 map/MSL；设备仍为 `unavailable`，不可安装 |
| 2026-10-23 | Xcode 缓存清理 | 清理未使用的 iOS 26.1 模拟器数据、非当前 `iOS DeviceSupport`、Xcode `DerivedData` 和对应废纸篓；保留当前 iOS 27.0 真机支持、Archives、签名身份、设备配对和现有 App。系统盘可用空间由约 4.4 GiB 增至约 17 GiB | 已完成 | 后续生成本地大包前先检查可用空间，不删除当前真机支持或项目产物 |
| 2026-10-23 | ACNH 资源包导入 | 新增 `DMACNHPK` v1 资源包、Mac 分批生成/检查/提取工具、iOS staging 导入与原子发布；修复条目头 52 字节契约和 Apple `compression_decode_buffer` 返回值判定，加入 Python raw DEFLATE 到 C 解压的交叉测试。导入器改为先完成 Update，再自动处理 Base，并对包路径去重 | 已完成代码与聚焦测试 | 待安装新包并在真机真实容器完成端到端小包导入验证 |
| 2026-10-23 | ACNH 签名构建复核 | `libcompression` 链接通过；ACNH Release arm64 使用 Team `YF8CHNSGUK`、Bundle ID `com.haifliu.d2rmobile.acnh` 签名，`codesign --verify --deep --strict` 通过；包内 3,480 个 shader library、3,480 个反射 JSON 和 3,482 行 shader map 保留 | 已得到签名可安装候选包 | 尚未执行本轮安装、资源导入和真机首帧；D2R 未触碰 |
| 2026-10-23 | ACNH 资源包导入崩溃修复 | 首次真机点击“导入资源包”时 App 因 `Thread stack size exceeded` 退出；崩溃栈指向 `dm_pack_extract` 入口，根因是 `copy_stored()` 在 512 KiB GCD 工作线程上分配了 1 MiB 栈缓冲区。新增固定 512 KiB 线程栈、2 MiB 原样条目的回归测试，旧实现稳定 SIGBUS、修复后通过；1 MiB I/O 缓冲区改为显式堆分配并在所有返回路径释放。ACNH Release arm64 已按既有 Bundle ID 覆盖安装，覆盖后数据容器变为 `2457BA98-C98C-49BF-A902-D6AF56D0F98D`，`Documents/update-000.dmp` 仍存在且大小一致，D2R 未触碰 | 已修复、重建并安装，未启动 | 等用户确认后启动一次并点击导入，再用真实容器完成标记和 5,000 个文件核验 |
| 2026-10-23 | ACNH 资源导入收口与完整 Update 包 | 修正层导入契约：私有 staging 每次从空目录开始，只有在整层全部包完成路径/大小/SHA-256 校验后才整层替换正式目录；移除了会与 staging 合并的未使用增量发布路径。App 改为优先扫描 `Library/Caches/acnh-packs`，并回退 `Documents`；重新签名覆盖安装后旧 App 数据仍在新容器 `95533CE0-6701-4A49-8F75-D3F0B5C01A6E`。完整 Update 已在重启后保留的 `/Users/haifliu/dm-work/acnh-packs/update` 重新生成并通过清单、大小和 SHA-256 校验：103,400 个文件、10,415,907,860 原始字节、单包 10,211,702,225 字节；前一次 Mac 于 11:35 重启导致 `/private/tmp` 包丢失，未完成设备传输 | 导入代码和完整包已完成 | iPhone 当前 `unavailable`；重新上线后传单包、核对设备端 SHA，再启动 App 执行整层导入 |
| 2026-10-23 | ACNH Update 分段包与扫描优先级 | 完整 Update 已改为 10 个约 1 GiB 的可重试批次，写入持久目录 `/Users/haifliu/dm-work/acnh-packs/update-chunks`：103,400 个文件、10,415,907,860 原始字节、10 包合计 10,211,702,513 存储字节；`pairIndex` 为 1–10、`pairCount` 均为 10，逐包大小和 SHA-256 全部通过。App 扫描改为每层只从优先级最高的根目录选取，避免 `Library/Caches/acnh-packs` 的不完整批次与 `Documents` 旧 5,000 文件测试包混用。ACNH 的 `main/sdk/subsdk0` 也已从现有解密 ExeFS 重建到持久目录 `/Users/haifliu/dm-work/acnh-product`。导入器、ACNH shader 15 项和跨语言 selftest 均通过 | 本地资源与代码已准备 | 最新 App 尚未重建安装；iPhone 仍 `unavailable`，需重新上线后安装并传 10 个批次 |
| 2026-10-23 | ACNH 资源包契约收敛 | Python/C 单条目上限统一为 64 MiB；`--esize` 现在同时约束打包和 `verify-set` 验证；DMACNHSET v2 统一为 `base.set.json`/`update.set.json`、`base`/`update` 层名及 `fileCount`/`packBytes` 字段。新增 Python 64 MiB、esize 限制、清单字段 focused tests，C 回归覆盖 64 MiB 原样条目和超限拒绝；未生成或传输完整资源 | 代码与 focused tests 已完成 | 尚未覆盖真实大于 2 GiB 条目、64 MiB raw DEFLATE 真机解压、设备端读取 DMACNHSET 清单及完整资源端到端导入 |
| 2026-10-26 | D2R iOS 个人签名续期 | Xcode 恢复既有 Apple Account 后，取得 `com.haifliu.d2rmobile` 的新个人开发者描述文件（有效至 2026-11-02）。由于当前工作区的 D2R 翻译链缓存已不再与上次可玩包一致，未混用 D3/ACNH 着色器重建；改为 APFS 克隆上次完整 D2R Release 包，核对 Team、Bundle ID、App Group 与新描述文件一致后重签名，`codesign --verify --deep --strict` 通过，并由 Device Hub 对 `Haifeng的iPhone` 同 Bundle ID 覆盖安装 | 已安装，未启动 | 未卸载、未 Remove、未启动 App，未清理或写入 App 沙盒、游戏资源、存档和偏好设置；待用户自行启动验证 |
| 2026-10-26 | ACNH iOS 签名安装 | 受影响测试通过；使用同一 `com.haifliu.d2rmobile.acnh`、Team `YF8CHNSGUK`、arm64 Release 包覆盖安装。安装后核对 D2R 与 ACNH 两个 Bundle ID 均存在，ACNH 原有 `.dmp`、Base/Update 完成目录、存档和日志仍保留 | 已安装，未启动 | 不卸载、不清理、不自动启动；下一步由用户明确授权后启动 ACNH，先收集首帧和资源导入状态 |
| 2026-10-27 | ACNH 黑屏修复包安装复核 | USB 连接、解锁并信任 Mac 后，即时核对 `Haifeng的iPhone`、UDID `00008150-001231962232401C`、Bundle ID；Device Hub 覆盖安装成功，CoreDevice Apps 查询仍有 ACNH 与 D2R，未启动、卸载或清理任何 App | 已安装，未启动 | 安装已解除；下一步需用户明确确认后只启动 ACNH，采集真实设备首帧日志 |
| 2026-10-27 | ACNH 纹理 view 黑屏根因 | 真实调用点只分配 16 字节临时 `NVNtextureView`，完整宿主 view 状态写回客体会覆盖紧邻的 `NVNcopyRegion`；改为 16 字节客体 cookie、宿主 `view_box_t` 状态表和 mutex 保护，新增相邻内存回归 | 代码修复、宿主验证完成 | `t_nvn` 128/128、ACNH 17/17、shader key 15/15、动态 coverage 198/198；需重新构建签名并只覆盖安装 ACNH |
| 2026-10-27 | ACNH 宿主 smoke 收口 | 修复后 Update RomFS smoke 生成 4 个有效 shader key，audren:u 生命周期和精确事件句柄请求通过，无 `TextureWriteTexels`、NVN 未实现调用、Break 或 crash，SIGTERM 后优雅退出；无窗口 smoke 不产生真实 QueueSubmit/QueuePresent | 宿主初始化验证完成 | 不能据此宣称 Mac/iPhone 首帧；下一步必须用新包启动 ACNH，收集设备端 PSO、提交、呈现和首帧日志 |

## 2. 当前阻塞与风险

### 2026-10-24 收口结论

- ACNH 宿主 allocator 崩溃已解除：针对真实构建标识的 `main+0x37c7c40` 保留原生 `x18` 指令；清理临时 `[DEBUG-acnh-*]` 探针后，宿主重新编译并重复 smoke test 均到达受控 `Break#1`。
- 资源导入前置契约已收敛：现有 Update 10 包与 Base 7 包全部通过 inspect；C/Python 单条目上限统一为 64 MiB；Caches 不完整批次会回退 Documents；完成标记会重新统计正式目录文件数和字节数。
- ACNH 无签名 Release 再构建通过，Bundle ID 为 `com.haifliu.d2rmobile.acnh`，包含 3,480 个 shader metallib 和 3,480 个 reflection JSON。当前设备状态为 `unavailable`，因此尚未签名安装、导入完整资源或启动首帧。

### 2026-10-26 D2R 启动阻塞

- [已解除，2026-10-26] D2R 未被 ACNH 覆盖：设备上两个 Bundle ID 和数据容器均独立，D2R 容器保留历史正常运行与存档日志。
- [已解除，2026-10-26] D2R 旧个人开发者描述文件于 2026-10-22 过期，设备因此拒绝启动。新描述文件保持原 Team 与 Bundle ID，有效至 2026-11-02；已使用同 Bundle ID 覆盖安装，原沙盒未清理。安装成功不等于已启动或真机行为验证完成。

- [已解除，2026-10-21] ACNH 本体密钥阻塞：本体 NSP 程序 NCA 的 `Rights ID` 为 `01006F8002326000000000000000000A`；最初仅用 `prod.keys` 时为 `Titlekey: Unknown`，随后按 D2R 既有链路使用本体 NSP ticket 导出 title key，并成功解出 Base RomFS。title key 仅在本地进程中使用，不写入项目。

- 之前的 `QQQ83D4FW6` 是证书标识误判；Xcode 当前 Apple Account 的 Personal Team 是 `YF8CHNSGUK`，本机配置已改用该团队并完成安装。
- iOS 首次启动的个人开发者信任已完成，App 已成功进入游戏流程。
- [已解除，2026-10-15] 本次构建前 `ioreg` 能看到 USB iPhone，但 `idevicepair`、`xcdevice` 和 `devicectl` 均未返回目标 UDID；改用 Xcode Release Archive 和 Device Hub 后已完成覆盖安装。
- 免费 Apple ID 的描述文件约 7 天过期，且不能使用 iCloud 存档同步、持续执行和扩展虚拟寻址能力；这是账号限制，不是构建故障。
- 本轮修复包已完成同 Bundle ID 覆盖安装，但尚未完成真机交互验证；解除条件是用户在游戏内完成两个空槽连续绑定路径并确认第二个 `➕` 可正常打开技能页。
- 本轮（第 4 次）修复把空位判据统一为一份读数，并已在“游戏内删空槽位”的复现场景上完成红色 → 绿色验证；仍未经过真机确认，解除条件是用户点击每个空白槽的 `➕` 都能打开技能页。
- [已解除，2026-10-21] ACNH 资源准备阻塞：Base RomFS 已导出到 `/Users/haifliu/dm-work/ACNH/Base/RomFS`，约 6.3 GB；Update RomFS 已准备，约 9.9 GB。两层均通过 `Shader/Park_UBER_All.Nin_NX_NVN.szs` 锚点检查。
- [已解除，2026-10-22] ACNH 安装前阻塞：无 Metal 编译器路径已经完成 3,480/3,480 产物，Release Archive 已签名；按 `ios-local-deploy` 核对设备、Bundle ID 和免费开发者名额后，ACNH 已安装到 iPhone，D2R 未被覆盖。
- [已解除，2026-10-22] ACNH 签名阻塞：命令行曾因未读取 GUI 账号返回 `No Accounts`，改用 Xcode GUI 选定真实 iPhone 后已生成 Personal Team 的 Xcode Managed Profile 和 Release Archive；不改用 D2R Bundle ID。
- [已解除，2026-10-22] ACNH 免费开发者 App 数量阻塞：首次安装触发设备最多 3 个免费开发者 App 的限制；用户释放一个非 D2R App 名额后，ACNH 已成功安装，D2R 保持原 Bundle ID 和安装状态。
- ACNH 资源传输阻塞：前一条“ Finder/DeviceFS 两层已完整”结论已作废，原因是它验证的是 Mac 侧缓存视图，App 真实容器不可见。真实容器当前 Update 为 39,766/103,400 个文件，仍有 63,636 个缺失和 204 个大小异常；Base 未完成。解除条件是直接读取 `devicectl` 真实容器清单，使 Update 的 103,400 个文件全部存在且大小一致，并把 Base 的 66,790 个文件导入同一真实容器后再次核验。
- [已解除，2026-10-23] ACNH 资源包小包端到端验证：5,000 个 Update 文件的 `.dmp` 已通过 `devicectl` 传到真实 App 容器，App 导入成功；真实容器核对为 5,000 个资源文件加 1 个完成标记，总字节与来源一致。
- ACNH 完整资源导入：单层导入契约和本地测试已经完成；解除条件是把 10 个 Update 分段包传到 `Library/Caches/acnh-packs`，App 确认全部批次齐全并整层发布后，真实容器出现 103,400 个 Update 文件和 Shader 锚点；之后同样生成并导入 Base。
- [已解除，2026-10-27] ACNH 设备连接：USB 连接后 `device info details` 确认 `Transport Type: wired`、`Device State: connected`，UDID 与设备名一致；Device Hub 覆盖安装已完成。
- [已解除，2026-10-27] ACNH 图形适配安装阻塞：包含 16 字节 guest-view 修复的 `dd-acnh-view-cookie` Release 包已通过签名校验并由 Device Hub 覆盖安装；设备保持 `connected`，Apps 列表仍有 `com.haifliu.d2rmobile.acnh` 与 `com.haifliu.d2rmobile`。D2R 未启动、未卸载、未清理。
- ACNH Metal Toolchain：本机 `xcrun metal`/`metallib` 仍不可用，但项目已有 `nometal_run.py` + `DM_SHELL` 设备端编译路径；这不再是当前适配阻塞。离线 Metal 编译验证仍未进行，设备首次启动会承担编译时间。
- D3 资源阻塞：Update `main`/`sdk` ExeFS 已解出并改写；Base/Update RomFS 仍未导出，磁盘余量不足以安全展开完整 15 GiB 内容。解除条件是准备独立临时空间后导出并核对 RomFS 锚点、NVN 版本和 shader 容器。
- D3 安装阻塞：已生成并严格校验签名包，但 Device Hub 当前目标 `Haifeng的iPhone` 状态为 `unavailable`。解除条件是用户让设备靠近或插线、解锁并信任 Mac；安装前再次核对完整 Bundle ID `com.haifliu.d2rmobile.d3` 并即时确认，安装后不自动启动。
- D3 首帧阻塞：真实 patched `main`/`sdk` 已嵌入签名包，但尚未在真实 D3 二进制和资源上启动宿主。解除条件是完成安装后单独记录首帧、资源校验和正常游玩结果，不以签名或安装成功代替运行结果。
- 图形全量回归仍有环境缺口：`make -C dm/gfx/tests t_defer_chain` 已通过（59 项全绿），但全量 `make -C dm/gfx/tests run` 在 `t_refl` 因本机缺少两个既有 `b-deb66c568156` 反射样本而停止；这不是 D3 编译错误，补齐样本后再跑完整套件。
- “三数之和”尚未映射到仓库中的业务模块、实现入口或测试入口，需要明确是算法题解、独立示例，还是项目内功能需求。

### 2026-10-27 ACNH Mac 宿主 smoke

- [已解除] `audren:u` 启动阻塞：确认 ACNH `AudioRendererParameterInternal` 的 work-size 请求使用现有参数计算；修正 AArch64 HIPC receive-buffer descriptor 为 3 words；补齐 `audren:u` command 6 `QuerySystemEvent` 和精确的事件句柄请求。
- [已解除] SDK 在 `OpenAudioRenderer`/renderer 生命周期中执行平台特定虚调用并触发 `Break#1`；ACNH profile 现在使用明确的宿主无声 renderer 后端，句柄保持不透明，D2R 路径不变。
- [已验证] Update RomFS Mac smoke 在 25 秒窗口内无 `Break#1`、无 `CRASH sig`、无未实现 `SendSyncRequest`，超时 SIGTERM 后完成优雅退出；`dmhost` 强制构建通过，x18 测试 7/7（96 子测试）通过，`git diff --check` 通过。


### 2026-10-27 ACNH iOS 黑屏诊断

- [已解除] 黑屏根因：ACNH 运行时控制段声明长度可达 `0xf68`，原 `0x880` 上限使 shader key parser 返回状态 2，首帧没有进入 shader/PSO 路径；现改为有界 `0x2000` 契约，key 单测 15 项全绿，Mac smoke 已生成有效 ACNH shader key。
- [已解除，2026-10-27] 修复后的 ACNH Release 包已重新签名并通过 Bundle ID、Team、arm64 和 codesign 校验；改用 USB 后 Device Hub 覆盖安装成功，CoreDevice Apps 查询确认 `com.haifliu.d2rmobile.acnh` 与 `com.haifliu.d2rmobile` 均存在。ACNH/D2R 均未启动，未清理任何 App。
- [新增诊断结论，2026-10-27] 真机开启 `DM_NVN_LOG=1` 后已确认进入 `DeviceGetInteger`、`QueueInitialize`、3 张 `1920x1080` 交换链纹理和 `WindowInitialize`；随后两次 `SIGABRT` 的设备 crash report 均落在 `h_fs_OpenFile → abort`，根因是诊断参数把 `DM_FSLOG` 指向设备不可写的绝对 `/tmp` 路径，首次文件打开触发项目既有 `die()`，不是产品黑屏根因。改用 Documents 工作目录的相对路径后待设备重新解锁继续采集。

### 2026-10-27 ACNH 纹理 view 复核

- [已解除] 第二处黑屏根因：ACNH 真实调用点把 16 字节临时 `NVNtextureView` 紧邻放在 `NVNcopyRegion` 前；旧实现写入完整 view 状态后覆盖 region，导致纹理写入区域宽高被破坏并出现 `TextureWriteTexels` 参数非法。当前改为客体 cookie 与宿主完整状态表，setter 在 mutex 内更新。
- [已验证] `t_nvn` 128/128、ACNH NVN 17/17、shader key 15/15、名字表 3/3、动态 ACNH coverage 198/198；修复后 Mac smoke 无 `TextureWriteTexels`、NVN 未实现调用、Break 或 crash，SIGTERM 后优雅退出。
- [已解除，2026-10-27] 当前设备已安装包含 guest-view 修复的 ACNH 包，不再使用旧包作为本轮依据。
- [当前阻塞] ACNH 真机首帧仍未证明：无窗口 Mac smoke 没有 `QueueSubmitCommands`/`QueuePresentTexture`；下一步需用户明确授权后只启动 ACNH，收集设备端 shader/PSO、`QueueSubmitCommands`、`QueuePresentTexture` 与服务缺口日志，不能把安装成功当作运行成功。
- [当前设备阻塞] 设备仍显示 `connected`、两个 Bundle ID 均存在，但 SpringBoard 对 ACNH 启动返回 `RequestDenied / Locked`；需在 iPhone 上重新点亮并实际输入密码后再继续，不能反复重试启动命令绕过锁屏。

### 2026-10-26 ACNH 真机黑屏复核

- 真机启动诊断时按 Bundle ID 映射确认 ACNH 与 D2R 进程分别属于独立容器；只终止了 ACNH 进程，D2R 进程未触碰。既有诊断日志仍稳定显示 `QueueSubmit=0`、`QueuePresent=0`、FPS `0.0`，但本轮重启后的设备日志端口因 CoreDevice/USB 通道中断没有形成新证据。
- 当前新增阻塞：`xcdevice` 从 `network/available` 变为 `unavailable`，`idevice_id -l` 无 USB UDID，`devicectl` 报开发者磁盘映像无法挂载；Device Hub 原生界面也因 Mac 锁定无法读取。解除条件是手动解锁 Mac、重新插拔并信任 iPhone，使目标 UDID 恢复 `connected` 后，再只启动 ACNH 收集首帧日志。

## 3. 下一步方向

- 真机逐一点击空白槽的 `➕`；若仍无反应，按 `dm_ta_slot_empty` 的返回值与 `empty_targets` 位逐段定位，不再只看拖拽清空这条路径。
- 明确“三数之和”的目标语言、输入输出契约和代码归属。
- 确认后只在对应模块增加实现和 focused test，并同步记录实际进展。
- 资源层已完成；首次启动前再次核对目标设备 `Haifeng的iPhone`、Bundle ID `com.haifliu.d2rmobile.acnh` 和 D2R 的独立 Bundle ID `com.haifliu.d2rmobile`。
- 下次连接同一 ACNH 容器后，先读取真实容器清单，再按 `run/acnh-transfer-20261022/missing-or-mismatch.list` 做差集续传；不要依据 `DeviceFS` 缓存视图判断完成。
- 用户确认后启动一次 ACNH，收集首个真实 `nvnProgramSetShaders`、metallib 设备端编译和服务缺口日志，再决定下一处 profile 分支；不得自动启动。
- 保留 D2R 安装、沙盒、资源和存档不变；ACNH Update 按约 1 GiB 分段传完后再由 App 整层发布，传后以设备端大小、SHA-256 和 App 导入后的真实容器清单共同验证。
- 大包只写重启后保留的 `/Users/haifliu/dm-work/acnh-packs`，不写 `/private/tmp`；每次先传输并确认设备端完整，再删除本地包，随后才能生成下一层，避免磁盘与重启风险。
- 为 D3 准备独立临时空间后运行 `DM_D3_EXEFS=<解密 ExeFS> dm/tools/d3_product.sh <输出目录>`，再用 `DM_IOS_GAME=d3 DM_OUT=<输出目录> DM_NO_SIGN=1 dm/ios/run.sh build` 做首次工程集成验证；拿到真实日志后再增加 D3 专用 HLE 或 shader 规则。
