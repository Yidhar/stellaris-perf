# bench：基准测试和压力测试

[English](README.md) | [简体中文](README.zh-CN.md)

用来测量 `stellaris_perf.dll` 各项优化的自动化性能测试。使用插件本身不需要这里的任何东西。

## 组成

| 部分 | 作用 |
|---|---|
| `stellaris_bench.dll` | 修改 DXGI 交换链虚表里 `IDXGISwapChain::Present` 的槽位，用来计帧，并在下一帧于主线程上执行命令。自己的管道是 `\\.\pipe\stellaris_bench`，命令：`status`、`pause 0/1`、`speed 0..5`。引擎地址全部来自生成的 SDK（`sdk::fn::CInGameIdler_SetPaused/SetGameSpeed`、`sdk::rt::CInGameIdler_paused/speed`、`sdk::rt::CGameState_date_hours`）。 |
| `stellaris_perf.dll` 的统计共享内存 | `Local\stellaris_perf_stats_<pid>`（`perf/include/perf_shared.hpp`），只读。内容包括当前生效的设置、`CFleetManagerView::Update` 的调用次数、缓存和节流计数。优化 DLL 本身没有任何控制通道，设置仍然只从 `stellaris_perf.ini` 读取。 |
| `scripts/dllctl.py` | 加载或卸载两个 DLL。加载是远程 `LoadLibraryW`；卸载通过设置 DLL 自己的事件，由 DLL 摘掉钩子、等进行中的调用都结束后自行卸载，从不在外部调用 `FreeLibrary`。 |
| `scripts/bench_fm.py` | 在一次连续运行里做 A/B，不需要读档。 |
| `scripts/game_session.py` | 自动读档：用指定存档重启游戏，加载 DLL，暂停，打开舰队管理器。 |
| `scripts/rule_report.py` | 从游戏内存里为 `stellaris_perf_rules.csv`（`rule_profile=1`）里的规则命名。 |
| `stress_mod/gen_stress_mod.py` | 生成压力测试 mod（见下）。 |
| `tools/` | 硬件计数器（缓存未命中）采样用的 WPR 配置和报告脚本。 |
| `results/` | 每次运行的 JSON 结果（首次运行时创建，不入库）。 |

为什么不用 inline hook：另一个工具可能已经用 inline hook 挂了 `Present`。两个 inline hook 叠在同一个函数上，如果按错误的顺序卸载，一方会把一条指向已卸载 DLL 的跳转写回去，游戏就会崩溃。改虚表槽位和 inline hook 互不干扰。

## 用法

先编译两个 DLL（见主 README），然后在游戏运行并已读档时：

```powershell
python bench\scripts\dllctl.py load all      # 在游戏中加载 perf 和 bench
python bench\scripts\bench_fm.py             # 默认 4 个 ABBA 组，每段 60 天
python bench\scripts\dllctl.py unload all
```

`bench_fm.py` 的流程：

- 修改 `stellaris_perf.ini`，等统计里出现新设置；
- 先让 `settle` 天过去，再计时 `days` 天；
- 按 ABBA 顺序排段，游戏逐渐变慢这类稳定漂移会在配对差里抵消；
- 事件弹窗暂停游戏时自动取消暂停，天与天之间超过 2 秒的间隔不计入；
- 舰队管理器被弹窗隐藏时（每帧更新次数低于 0.9），先暂停游戏并提示重新打开窗口，窗口恢复后重测这一段。

输出每个配置的均值、标准差和帧率，以及按 ABBA 组计算的配对差（均值和 95% 置信区间）。

自定义配置：

```powershell
python bench_fm.py --config base:fleet_manager_cache=0 --config fix:fleet_manager_cache=1 --blocks 6 --days 60
python bench_fm.py --no-fm-check --config off:opinion_cache=0 --config on:opinion_cache=1
```

## 自动读档（`scripts/game_session.py`）

```powershell
python bench\scripts\game_session.py load arena_base --fleet-manager   # 关闭游戏，改写 continue_game.json，用 --continuelastsave 启动，
                                                                  # 加载两个 DLL，等读档完成，暂停，按 F9 打开舰队管理器
python bench\scripts\game_session.py restore                           # 恢复原来的 continue_game.json
python bench\scripts\bench_fm.py --reload arena_base --warmup 5 --blocks 10 --days 3 --settle 1 --config ... --config ...
```

- `--continuelastsave` 要写两个连字符：引擎检查的选项名是 `-continuelastsave`，命令行解析会去掉一个前导 `-`。它读取 `continue_game.json` 里的 `title`，设置 `g_QuickStartSave` 和 `g_bQuickStart`。
- `--reload` 让每个 ABBA 组都从同一个存档时刻开始，组间交替使用 ABBA / BAAB 顺序。读档后前几十天的漂移是非线性的，所以结果除了普通配对差，还会给出"消除顺序效应"的估计。

## 压力测试 mod

`stress_mod/gen_stress_mod.py` 会写出一个 mod（"zz perf stress"），在原版内容上复现大型 mod 里出现的负载形态，不改变游戏玩法（修正器极小，触发器永远不匹配，游戏规则保持原版结果）：

- 舰船、星球、国家修正器风暴（每次变化都重建修正器，分别测批量和不批量）；
- 舰队进入星系事件的扇出、大量全局 flag、更重的游戏规则、触发式好感度修正、mean_time_to_happen 事件；
- 竞技场：在一个星系里持续进行的超大规模舰队战，带战斗日循环、每次命中事件和舰船被毁事件；
- 额外的 AI 人口，配有岗位，同时保留一些失业和空缺，让人口迁移持续运转；
- 大量使用事件目标的触发器（几百个全局目标、动态的 `name@this` 目标、每艘船的局部目标）。

```powershell
python bench\stress_mod\gen_stress_mod.py                       # 写出 mod 文件夹并注册 mod\zz_perf_stress.mod
python bench\stress_mod\gen_stress_mod.py --enable              # 加入 dlc_load.json（会保留备份）
python bench\stress_mod\gen_stress_mod.py --disable             # 再移除
python bench\stress_mod\gen_stress_mod.py --help                # 所有规模参数
```

用完记得运行 `--disable`：启用这个 mod 时游戏比正常慢得多。游戏不在默认的 Steam 目录时，设置环境变量 `STELLARIS_DIR`。


## 已知限制

- 舰队管理器由 `--reload` 自动按 F9 打开。不读档时，窗口需要手动打开一次；被事件弹窗隐藏后，也需要手动重新打开，脚本会等待。
- 测的是真实时间，游戏窗口要保持在前台、不能最小化；最小化时不渲染帧，命令会超时。
