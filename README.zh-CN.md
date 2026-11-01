# stellaris-perf

[English](README.md) | [简体中文](README.zh-CN.md)

**Stellaris 4.5.2**（Windows x64，`-dx11` 版本）的性能插件：一个注入到 `stellaris.exe` 的小 DLL，把引擎里几处较慢的代码路径换成等价但更省的实现。不需要 mod，不修改存档。它是 **Stellaris 启动器**的一个插件（插件规范 v2）：启动器负责安装它、提供设置文件的编辑、检查它是否适用于已安装的游戏版本，并在启动游戏时加载它。仓库里同时放着用来发现和验证这些优化的基准测试与压力测试工具。

| 发布文件 | 内容 |
|---|---|
| `stellaris-perf-<版本>.zip` | **插件文件夹本身**：`stl-plugin.json`、`stellaris_perf.dll`、`defaults\stellaris_perf.ini`、本说明 |
| `stellaris-perf-bench-<版本>.zip` | `stellaris_bench.dll`、基准测试脚本和压力 mod 生成器（见 [`bench/`](bench/README.zh-CN.md)）；不是插件 |

## 兼容性

- 只适用于**一个确定的游戏版本**：对应 `stellaris.exe` 的 PE 时间戳写在清单里（`game.exe_timestamps`）和发布说明里。启动器不会把插件加载进其他版本；DLL 还会再和 SDK 比对一次，不一致时什么都不安装。游戏更新之后需要重新生成 SDK 子集并重新编译（见[编译](#编译)）。
- 引擎地址没有写死：全部来自从已安装的可执行文件生成的 SDK（`sdk/stellaris_sdk.hpp`，由 `tools/extract_sdk.py` 写出的子集）。
- 和 mod 兼容：插件改变的是引擎怎么计算，不是脚本和数据写了什么。
- 联机和玩游戏 mod 一样：每个玩家都要装这个插件，并且设置相同（见[多人游戏](#多人游戏)）。

## 安装和使用

1. 在 [Releases 页面](https://github.com/Yidhar/stellaris-perf/releases)下载 `stellaris-perf-<版本>.zip`（旁边的 `.sha256` 文件是校验和）。
2. 把它放到启动器存放插件的地方：把 zip 解压到 `Documents\Paradox Interactive\Stellaris\plugins\stellaris-perf\`（zip 里没有最外层文件夹，`stl-plugin.json` 会直接落在这个文件夹里）。
   也可以解压到任意位置，再用启动器安装这个文件夹：`stl plugin install <文件夹>`，或者启动器里的**插件 > 安装**。
3. 在你的 playset 里启用插件（`stl plugin enable stellaris-perf`，或者插件页面）。
4. **用 Stellaris 启动器启动游戏**（`stl launch`，或它的开始游戏按钮）。启动器会等游戏窗口出现，再加载插件。从 Steam 或 Paradox 启动器启动游戏时**不会**加载插件；这是插件的工作方式，没有替身 DLL，也没有别的加载器。

更新：清单里写了本仓库（`update.github`），所以启动器的插件页面（或 `stl plugin update stellaris-perf`）会提示有新版本，校验 SHA-256 后覆盖安装，并保留 `config\`。更新时游戏必须是关闭的，因为 DLL 正在使用。

插件文件夹是插件唯一的存放位置：

| | |
|---|---|
| `config\stellaris_perf.ini` | 设置。启动器根据 `defaults\stellaris_perf.ini` 生成它，插件页面可以直接编辑。插件每 2 秒检查一次文件的修改时间，变了就重新读取，所以游戏运行中就能改设置。文件缺失时使用内置默认值。 |
| `logs\stellaris_perf.log` | 已安装的钩子、生效的设置，以及每 30 秒一次和每次变化时的计数 |
| `logs\stellaris_perf_rules.csv` | 仅在 `rule_profile=1` 时 |

不会往游戏文件夹里写任何东西。游戏文件夹里如果有旧的 `stellaris_perf.ini`（插件有自己的文件夹之前的设置位置），而插件启动时 `config\` 里还没有文件，就会把它复制到 `config\` 一次，之后忽略游戏文件夹里的那份。启动器已经根据默认值生成了 `config\stellaris_perf.ini` 时，需要手动把旧设置复制过去。

## 做了哪些优化

每项优化都是一个钩子（或一处 5 字节的代码补丁），由 `config\stellaris_perf.ini` 里的一个键控制。关闭的钩子直接透传给引擎。

### 默认开启：答案和引擎自己的一致

- **`has_*_flag` 扫描**（`flag_simd`，默认 `1`）。引擎每次循环比较一个 flag id，对象是一个无序数组。`1` 用 SSE2 扫描回答同一个问题，每次比较 8 个 id，不会读到数组末尾及以后。`2` 还会同时运行引擎自己的扫描，统计任何不一致。动态的 `flag@scope` 名字和读不了的容器交给引擎。
- **每日 flag 过期**（`flag_expiry_skip`）。过期处理只动带计时器的 flag；没有计时 flag 的容器直接跳过，因为引擎在那里本来也什么都不改。
- **舰队更新：每支舰队单独一个工作块**（`fleet_parallel_grain1`）。引擎每个微 tick 的舰队和舰船更新，把舰队按 `舰队数 / 任务数 / 3` 的块大小分给工作线程。只有少数几支超大舰队时，一个块里装着大舰队，其他线程就空等。对块大小上限的一处 5 字节补丁，让每支舰队单独成块。这个阶段禁用了随机数，舰队之间互相独立，所以哪个线程更新哪支舰队不会改变任何结果（引擎自己的分块本来就随线程数变化）。该键关闭时会还原原始字节。
- **舰队管理器窗口：军事实力缓存**（`fleet_manager_cache`）。模板列表每帧为了显示一支舰队的实力，都要重新计算每艘船的完整属性。现在只在 `CFleetManagerView::Update` 内部缓存（AI 和模拟调用同一个引擎函数，不受影响），最长一秒真实时间，且不跨游戏日，并且把舰队的舰船数组和数量也作为键，合并或拆分会立刻体现。
- **舰队管理器窗口：补充兵力重算节流**（`fleet_manager_reinforce_ms`，默认 1000）。窗口打开时，引擎每 80 ms 里有 10 ms 会重跑一次"哪些舰船需要补充"，不管有没有变化；插件让它每个间隔最多跑一次。只影响显示。
- **大块回收缓存**（`alloc_cache`、`alloc_cache_mb`）。大规模战斗中，引擎每个战斗 tick 都为每支舰队构建并释放好几个几百 KB 的数组（索敌表），每次都是一对普通的 `operator new` / `free`，所以每个 tick 都从堆里拿冷内存、再还回去。插件挂钩 `operator new` 和 `free`，把每个线程最近释放的、128 KB 及以上的几个块留着（每线程最多 `alloc_cache_mb`，默认 8 MB）；该线程下一次申请同样大小时就拿回同一个块，它还热在这个核心的缓存里。这些块是普通的堆块，没有额外的头，所以引擎随时可以释放其中一个，插件卸载之后也一样；块里的内容从不假定为零（`operator new` 本来也不保证这一点）。只有当堆报告该块仍然够大时才会复用。把该键关掉会摘掉钩子，并把缓存的块还给堆。
- **战斗参与者共享而不是拷贝**（`combat_no_copy`）。引擎的并行舰船更新会在战斗参与者表里查每艘船所属舰队的条目，对有条目的舰队，为**每艘船**深拷贝一份条目（七个数组，长度都等于星系内的舰船数），只是为了把它作为只读参数传给 `CShip::MicroUpdateParallel`，随后再释放。插件把这次拷贝补掉（11 字节的代码补丁，靠字节特征定位，必须恰好匹配一处）：被调函数拿到表里自己的那份对象，销毁拷贝的代码被跳过。舰船更新期间这张表不会被修改，所以每艘船看到的数据和以前一样。补丁只在 tick 之间、工作线程空闲时写入和还原。
- **殖民地月更串行段里少做修正器工作**（`colony_monthly_batch`，默认 `2`）。殖民地的月更串行段（`CColony::MonthlyUpdateSerial`：迁移、增长、组装、衰减，量是之前并行人口统计段算好的）逐个人口组改变数量。每次变动都把殖民地的修正节点置失效，下一个读取者按需重建它：变动触发的事件的 trigger、引擎在每次变动后强制做的刷新（节点，然后是缓存的舒适度/犯罪/稳定度，变了再置失效一次）、新建人口组的刷新——晚期存档每个月初约 1,300 次重建，加上串行段里修正器管理器的几次 flush。`1`：强制刷新在殖民地自己的串行段期间跳过、结束时按引擎顺序做一次，串行段期间管理器的 flush 被按住（脏节点由串行段之后的下一次 flush 重建）；读取者遇到脏节点会按需重建，所以脚本和引擎读到的东西都不变。`2`：此外殖民地自己的节点在它的串行段期间冻结：串行段内部对它的每次读取（`on_pop_growth` 之类事件的 trigger、串行段自己的代码）拿到的是串行段开始时的节点；结束时重建一次并再次置失效，用冻结节点算出的东西随后都会重建。确定性的，两个客户端都开着就一致。两种模式与引擎的共同差别：殖民地自己的串行段里舒适度或住房档位发生变化时，它拿到的稳定度对应串行段之前的档位（引擎：最后一次变动之前的档位）；读取缓存值的 `on_pop_growth` / `on_pop_purged` 事件看到的是串行段之前的值。晚期存档实测，月初的月更（36 个和 72 个月初的两次运行）：`0` 370 / 397 ms，`1` 358 / 387 ms，`2` 345 / 349 ms（见 [`bench/RESULTS.md`](bench/RESULTS.md)）。

- **tick 中间的渲染帧率上限**（`tick_render_fps`，默认 10）。开着帧平滑时，只要引擎按刷新率算出的计时器说"该画一帧了"，它就会在模拟中间画一帧；高刷新率显示器上最多能到显示器的刷新率，而每一帧都要在模拟线程上遍历并绘制整棵 GUI 窗口树。插件挂钩引擎的 `RenderFrame`（35 个调用点，靠字节特征定位，必须都指向同一个函数），把 tick 内部、间隔小于 1 / `tick_render_fps` 秒的调用丢掉。`0` 表示用游戏自己的频率。只影响显示。

- **tick 之间的帧用同一个上限**（`render_cap_between_ticks`，默认开）。引擎的主循环在 tick 之间也会调用 `RenderFrame`，这部分约占 `RenderFrame` 总耗时的一半。开启后它们遵守同样的上限，但只在模拟运行时生效（最近 250 ms 内有 tick 结束）；暂停、菜单、加载时不限。（改成丢掉每次主循环迭代末尾的绘制步反而更慢：引擎自己的帧逻辑会因此更频繁地重做 GUI 更新。）

### 可选

- **帧平滑**（`frame_smoothing`）。设置引擎自己的 `g_bFrameSmoothing`（`smooth` 控制台命令）：开启后游戏在一个 tick 中间渲染帧（界面更流畅，模拟更慢）。`-1` 不改游戏的设置。

### 可选，近似：按游戏日缓存（默认关闭）

这些优化把一次引擎计算在一个游戏日内缓存起来。如果相关状态在当天内发生了变化，就会返回过时的值，所以不是引擎的精确行为，默认关闭。

- **好感度缓存**（`opinion_cache`）：不带原因字符串的 `CCountry::CalcOurOpinionOfOther`。
- **游戏规则缓存**（`rule_cache`）：每条脚本游戏规则（`CScriptedRule::Evaluate`），以规则和作用域的 this / from / root / prev 对象为键。要求返回原因的调用，以及带参数的作用域，会绕过缓存。
  - `1`：缓存所有规则。
  - `2`：**自适应**。每隔约 30 秒开一个很短的学习窗口，让所有规则都走缓存；之后按实测数据评判每条规则：用缓存的期望成本（命中率 × 命中成本 + 未命中率 × 未命中成本）对比引擎自己求值的成本，只对缓存明显更省的规则启用（低于求值成本的 85% 时打开，高于 95% 时关闭）。
  - 缓存表的大小取单个核心 L2 缓存的四分之一，在加载时从 CPU 读取，所以在任何处理器上都能留在缓存里，而不是只针对某一台机器调参。

### 实验性（默认关闭）

- **修正器图刷新快速路径**（`modifier_flush`、`modifier_flush_max`）。引擎在每个效果块和命令之后，都会用一个约 100 个任务的并行作业刷新修正器节点图，即使只有一个节点是脏的。这一项在主线程上串行重建很小、而且完全已知的脏集合，遵循引擎的类别顺序，模式 `2` 会把结果逐节点和引擎自己的刷新对比。不建议用于实际游玩。

### 诊断

- `profile` 对已挂钩且缓存关闭的函数，按主线程和其他线程分开，统计每个游戏日里每次调用的耗时（写入日志）。
- `rule_profile` 每 30 秒左右把每条游戏规则、每条路径（引擎、绕过、命中、未命中）的调用数和周期数写入 `logs\stellaris_perf_rules.csv`。
- `scope_profile` 统计事件目标解析（`CEventTarget::GetScope`、动态的 `name@target` flag、作用域拷贝）的耗时。
- `phase_profile` 在引擎各阶段的入口用 rdtsc 计时，按游戏日记录：tick 及其各部分（微更新、日更、月更、年更、含等待工作线程的 AI 更新、自动存档）、tick 内和 tick 之间画的帧、帧函数自己的 GUI 更新和绘制，每个阶段记的是独占时间。每个游戏日一条记录写到 `logs\stellaris_perf_phases.json`；`python bench\scripts\phase_report.py` 按日子分组（普通日、月初、1 月 1 日、7 月 1 日）打印每天的毫秒数和占墙钟的比例，月初和自动存档的尖峰不再被平均掉。`phase_extra`（`0xRVA[=name], ...`）可以在游戏运行中把可执行文件里的任意函数加进计时（只接受函数起点），用来逐层拆开某个阶段，不用重新编译；`phase_callers`（某个已计时函数的 `0xRVA`）按调用点记录它的次数和耗时，普通日和月初分开。
- 当前设置和计数还会发布到一块只读的共享内存（`Local\stellaris_perf_stats_<pid>`，结构见 `perf/include/perf_shared.hpp`），基准脚本从这里读取。

### 研究过，没有发布

用压力测试工具测量后，认为不值得而没有做：事件目标和动态名字查找（统计功能保留为 `scope_profile`）、月初的人口岗位分配和迁移、修正器克隆的记忆化。

## 设置参考

| 键 | 默认值 | 含义 |
|---|---|---|
| `frame_smoothing` | `-1` | `-1` 不改游戏设置，`0` 关，`1` 开 |
| `fleet_parallel_grain1` | `1` | 每支舰队单独一个并行工作块 |
| `alloc_cache` | `1` | 引擎 `operator new` / `free` 的大块回收缓存 |
| `alloc_cache_mb` | `8` | 每个线程最多缓存多少内存（MB，`0` = 关） |
| `combat_no_copy` | `1` | 共享战斗参与者表里的对象，不再为每艘船拷贝一份 |
| `colony_monthly_batch` | `2` | 殖民地月更串行段：`1` 强制刷新合并到末尾、按住 flush；`2` 再把殖民地节点在串行段期间冻结 |
| `tick_render_fps` | `10` | tick 中间每秒最多画多少帧（`0` = 游戏自己的频率） |
| `render_cap_between_ticks` | `1` | 同样的上限用于 tick 之间的帧，只在模拟运行时生效 |
| `fleet_manager_cache` | `1` | 缓存舰队管理器窗口里的舰队军事实力 |
| `fleet_manager_reinforce_ms` | `1000` | 两次补充兵力重算之间的最小毫秒数（`0` = 游戏自己的节流） |
| `flag_simd` | `1` | `0` 引擎扫描，`1` SSE2 扫描，`2` 校验 |
| `flag_expiry_skip` | `1` | 跳过没有计时 flag 的容器的每日过期处理 |
| `opinion_cache` | `0` | 按游戏日缓存好感度 |
| `rule_cache` | `0` | `0` 关，`1` 所有规则，`2` 自适应 |
| `modifier_flush` | `0` | `0` 引擎，`1` 串行快速路径，`2` 校验，`3` 对照 |
| `modifier_flush_max` | `32` | 走串行路径的最大脏集合 |
| `profile`、`rule_profile`、`scope_profile`、`phase_profile` | `0` | 诊断 |

## 设计上怎么保证安全

- 内联钩子用 [MinHook](https://github.com/TsudaKageyu/minhook)；引擎代码始终只由游戏自己的线程执行。没有管道，没有控制台访问，也没有跨线程调用引擎。
- 原始内存读取都有 SEH 保护，坏指针会被跳过（并计数），不会让游戏崩溃。
- 缓存表是每线程一份，计数在线程里累计、分批发布，统计本身不会变成它想去掉的开销。
- 卸载由 DLL 自己完成：先摘掉钩子，等进行中的调用结束，再释放自己。游戏线程可能还在某个钩子函数里，所以绝不会从外部调用 `FreeLibrary`。

## 多人游戏

Stellaris 的多人游戏是锁步的：每个客户端模拟同一局游戏，并比对校验和。这个插件的用法和玩法 mod 一样：**游戏里的每个玩家都运行它，并且设置相同**。插件里没有任何针对多人游戏的代码，只有一部分玩家装了插件的局不在支持范围内。

插件要保证的是：两个都运行它的客户端算出相同的结果。默认开启的每项优化，返回的都是引擎自己的答案（或者不依赖由哪个线程、按什么顺序算出的答案），所以模拟保持一致。按天的缓存（`opinion_cache`、`rule_cache`）和修正器快速路径（`modifier_flush`）默认关闭，因为它们的结果可能取决于哪个工作线程先填了缓存；在重新设计之前不会打开。各玩家的设置文件如果不同，插件在一个客户端上的行为就不同，所以请保持一致。

## 编译

需要 Visual Studio 2022（MSVC，x64）和 CMake 3.20+。MinHook 由 CMake 自动获取。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release        # build\Release\stellaris_perf.dll、stellaris_bench.dll
```

构建还会组装出插件文件夹 `build\plugin\stellaris-perf`（清单、DLL、`defaults\`）。用 `stl plugin install build\plugin\stellaris-perf` 安装它，或者用 `stl plugin install --link build\plugin\stellaris-perf` 原地开发（这时 `config\` 和 `logs\` 会建在构建文件夹里）。`python tools\check_plugin.py [--dir build\plugin\stellaris-perf]` 会对照插件规范和 SDK 检查清单。

开发时，`stl inject build\plugin\stellaris-perf\stellaris_perf.dll` 可以把一个构建手动加载进正在运行的游戏（这时 DLL 在那个构建文件夹里读取 `config\`、写入 `logs\`），`python bench\scripts\dllctl.py unload perf` 让 DLL 自己卸载（摘掉钩子、等进行中的调用结束、再释放自己），这样不用重启游戏就能换新构建。玩家两者都不需要：插件由启动器加载。

游戏更新之后，先在 [Stellaris MCP 仓库](https://github.com/Yidhar/stellaris-mcp)里重新生成 SDK（`python tools/sdk_dumper/dump.py`），然后：

```powershell
python tools\extract_sdk.py <完整的 stellaris_sdk.hpp 路径>   # 重写 sdk\stellaris_sdk.hpp
```

再重新编译。每个发布版本对应的游戏版本写在它的发布说明里。

## 发布和 CI

发布页：<https://github.com/Yidhar/stellaris-perf/releases>。最新版本由 CI 根据标签编译并发布，附带两个 zip 和对应的 SHA-256 文件。

`.github/workflows/build-release.yml` 在每次 push 和 pull request 时编译两个 DLL，用 `tools/check_plugin.py` 检查插件文件夹（清单是 schema 2、游戏版本和 SDK 一致、没有多余文件），并把打包好的 zip 作为工作流产物保存。推送标签 `v<版本>`（版本就是 `plugin/stl-plugin.json` 里的版本，例如 `git tag v0.2.0 && git push origin v0.2.0`；标签对不上会让检查失败），同一个工作流就会发布 GitHub Release，附带两个 zip、SHA-256 文件，以及写明 DLL 对应游戏版本的发布说明。标签里带 `-` 的（例如 `v0.2.0-rc1`）会作为预发布版本。

## 基准测试和压力测试

[`bench/`](bench/README.zh-CN.md) 里有自动化的 A/B 基准测试（在一次游戏运行里按 ABBA 顺序对比两组设置，可以自动读档）、一个按热门 mod 中出现的负载形态生成的压力测试 mod（修正器风暴、大规模舰队战、大量使用事件目标的触发器、额外的 AI 人口），以及分析辅助工具。

## 许可证

MIT，见 [LICENSE](LICENSE)。
