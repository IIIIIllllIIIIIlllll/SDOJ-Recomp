# SDOJ-Recomp 无伤模式（No-Damage）— 交接文档

> 本文档用于把"在重编译版东方非想天则中实现 Arrange 模式无伤"这项任务的**全部进展、环境、已确认事实、走过的弯路、当前状态与下一步**完整移交给下一个 AI / 开发者。
> 生成时间：2026-09-06。所有路径、地址、函数号均为**实际核对过**的真实值。

---

## 1. 项目与任务概述

- **项目**：`SDOJ-Recomp` — 用 **ReXGlue SDK** 把 Xbox 360 版《东方非想天则 / DoDonPachi Saidaioujou》（TU1）重编译为原生 C++ 项目。
- **ReXGlue 工作方式**：不是重写逻辑，而是**逐条 PowerPC 指令级翻译**成 C++。每个原函数变成一个 `DEFINE_REX_FUNC(sub_XXXX)`，内部用 `ctx.rN`（模拟 32 个 PowerPC 通用寄存器，64 位宽）、`ctx.lr`、`ctx.xer`、`ctx.crN`（条件寄存器）、`ctx.ctr`，以及 `REX_LOAD_U8/U16/U32/U64` / `REX_STORE_U8/U16/U32/U64` 宏来读写**模拟的 Xbox 内存**（挂在 `base` 指针上）。
- **任务**：实现一个**无伤模式（cheat）**。用户玩的是 **Arrange 模式（CA022110 模块）**，规则：
  - 命数 = 1；
  - 有 5 个炸弹（UI 上显示 4 个一排 + 1 个单独的，疑似"最后一个"的视觉警示）；
  - **碰到弹幕会自动消耗炸弹**（清弹 + 短暂无敌），炸弹耗尽后再被击 → **正式死亡 + 倒计时**。
  - 目标：让玩家**实际上死不了**。
- **性质**：单人游戏本地作弊，个人 / 逆向研究用途，无安全约束。

---

## 2. 环境信息

### 2.1 关键路径
| 用途 | 路径 |
|---|---|
| 源码根目录（工作目录） | `C:\Users\Mark\Workspace\CProject\SDOJ-Recomp-main` |
| 游戏 ISO | `C:\baidunetdiskdownload\DoDonPachi Saidaioujou (Japan).iso` |
| **运行时目录**（游戏实际跑这里） | `C:\baidunetdiskdownload\SDOJ-Recomp\` |
| 编译输出目录 | `C:\Users\Mark\Workspace\CProject\SDOJ-Recomp-main\out\build\win-amd64-release\` |
| 死亡探测日志 | `C:\baidunetdiskdownload\SDOJ-Recomp\sdoj_death.log`（每次运行以 `"w"` 模式覆盖） |

### 2.2 构建命令（Git Bash）
```bash
export PATH="/c/Program Files/LLVM/bin:/c/baidunetdiskdownload:$PATH"
cd "C:\Users\Mark\Workspace\CProject\SDOJ-Recomp-main"
cmake --build --preset win-amd64-release --parallel
```
- 工具链：LLVM/Clang（`C:\Program Files\LLVM\bin`）+ MSVC 头/库 + ninja（`C:\baidunetdiskdownload`）。
- **重要**：`generated/*.cpp` / `init.h` **不会在构建时重新生成**——CMake 里的 codegen 只给"重编译器工具"嵌模板，项目里的 `generated/` 是**直接编译的已提交源码**。所以**直接编辑 `generated/` 下的文件，改动会保留在后续 `cmake --build` 中**（这正是本任务打补丁的方式）。
- 构建产物（部署这 3 个即可）：
  - `saidaioujou_recomp_tu1.exe`（主程序）
  - `rexruntime.dll`
  - `saidaioujou_recomp_tu1_CA022110.dll`（**Arrange 模式 DLL**）
  - `saidaioujou_recomp_tu1_CA022100.dll`（**Normal 模式 DLL —— 也已打补丁，见 §8.5**）

### 2.3 部署
把上面 3 个产物拷到 `C:\baidunetdiskdownload\SDOJ-Recomp\`，并删掉旧的 `sdoj_death.log`（下次运行会自动重建）：
```bash
SRC="C:/Users/Mark/Workspace/CProject/SDOJ-Recomp-main/out/build/win-amd64-release"
DST="C:/baidunetdiskdownload/SDOJ-Recomp"
cp -f "$SRC/saidaioujou_recomp_tu1.exe" "$DST/"
cp -f "$SRC/rexruntime.dll" "$DST/"
cp -f "$SRC/saidaioujou_recomp_tu1_CA022110.dll" "$DST/"
cp -f "$SRC/saidaioujou_recomp_tu1_CA022100.dll" "$DST/"
rm -f "$DST/sdoj_death.log"
```

### 2.4 启动脚本（在运行时目录）
- `launch.bat`：普通启动（`--input_backend=xinput --vsync=false --fullscreen=true --xex_apply_patches=true --render_patch=true --input_patch=true`，`game_data`/`user_data` 用相对目录）。
- `invincible.bat`：同上 + **`--sdoj_invincible=true`**（无伤开关）。
- `trace.bat`：探测相关。
- 三者都会先跑 `prepare_game.ps1 -PrepareOnly` 校验/准备 game_data。
- 平台 win32，shell = Git Bash，**不是 git 仓库**。

---

## 3. 模块结构（关键）

重编译代码按原 XEX 的模块拆成 3 套 `generated/`：
- `generated/default/` → 主 exe（`saidaioujou_recomp_tu1.exe`）
- `generated/CA022100/` → Normal 模式共享 DLL
- `generated/CA022110/` → **Arrange 模式共享 DLL（cheat 目标）**

每个 CA DLL 是独立 TU 集合，**各自的静态变量独立**（所以探测逻辑只放进 CA022110，避免污染 Normal/主 exe）。Arrange 模块共 **12 个** `saidaioujou_recomp_tu1_recomp.*.cpp`（recomp.0 … recomp.11），**每个都 `#include "../../src/sdoj_trace.h"`**（第 2 行）。

**cvar 系统**（定义在主 exe，跨模块读取）：
- 定义：`src/main.cpp` 里 `REXCVAR_DEFINE_BOOL(name, default, "SDOJ", desc)`：
  - `input_patch`(默认 true)、`render_patch`(true)、`sdoj_trace`(false)、`sdoj_invincible`(false)。
  - 生命周期 `kRequiresRestart`（改了要重启）。
- 读取：`src/sdoj_patch_flags.h` 里 `REXCVAR_QUERY(bool, name)`，封装成 `sdoj_patch_flags::invincible_enabled()` / `input_enabled()` / `render_enabled()` / `trace_enabled()`。
- **`sdoj_invincible` 就是无伤总开关**（`--sdoj_invincible=true` 打开）。补丁必须用它门控，关掉时行为完全还原。

---

## 4. ReXGlue 重编译代码约定（打补丁时要懂）

- 每个函数形如：
  ```cpp
  DEFINE_REX_FUNC(sub_880D2B18) {
      REX_FUNC_PROLOGUE();
      // lwz r11,5308(r31)
      ctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 5308);
      // stw r11,5308(r31)
      REX_STORE_U32(ctx.r31.u32 + 5308, ctx.r11.u32);
      ...
  }
  ```
- `ctx.rN`：64 位寄存器。常用视图：`.u64/.s64`、`.u32/.s32`、`.u16/.s16`、`.u8/.s8`。赋值给 `.u64` 会整体覆盖（低 32 位放 32 位值）。
- `ctx.crN`：条件寄存器，`.eq/.lt/.gt/...` 是布尔；比较用 `ctx.crN.compare<T>(a, b, ctx.xer)`（T = `int32_t` 或 `uint32_t`，**有符号/无符号不同**）。
- `ctx.lr`（返回地址）、`ctx.xer`（CA/OW 等）、`ctx.ctr`。
- `REX_LOAD_Ux(addr)` / `REX_STORE_Ux(addr, val)`：`addr` 传**模拟地址**（形如 `ctx.rN.u32 + offset`），宏内部自动加 `base` + 大端转换（`__builtin_bswapX`）。`base`（`uint8_t*`）**在函数内处处可见**。
- `REX_PHYS_HOST_OFFSET(x)`：`x >= 0xE0000000 ? 0x1000 : 0`（模拟内存到 host 的偏移）。
- **`__func__` 在每个打补丁点可见**，值为函数名（如 `"__imp__sub_880D2B18"`），可用来识别"是哪个函数写的"。
- **补丁门控范式**：`if (sdoj_patch_flags::invincible_enabled()) { ... }`。
- **注意**：`REX_STORE_*` 现在被 `sdoj_trace.h` 重新定义为 `do{ store; hook; }while(0)`，所以 `if(...) REX_STORE_U32(...);` 语法仍然成立。

---

## 5. 关键内存地址 / 函数速查表（Arrange, CA022110）

> 模拟（guest）地址。以下地址都在 **host 内存 `base + addr`** 处（无 `0xE0000000` 偏移）。

### 5.1 已确认的对象 / 数据
| guest 地址 | 含义 | 依据 |
|---|---|---|
| `0x88617D80` | 某实体对象基址（`sub_880D2B18` 里的 `r31`，按 ID 从链表查得）；`[r31+192]=0x8805B8E0`（常量指针，疑似类型/vtable） | LogHit 探针 |
| `0x8861923C` | `[r31+5308]`，一个 **one-shot 计数器**（0→−1，之后停）。**不是炸弹数** | LogHit 探针 |
| `0x88619238` | `[r31+5304]`，死亡分支里被清 0 的字段，但**实测恒为 0**，不是存活标志 | LogHit 探针 |
| `0x88617E0C` | 每帧被 `sub_880CA6A8`（文本渲染）写入的浮点/打包数据。**不是炸弹** | WRT 钩 |
| `0x88614947` | "被击"标志，每次被击 1→0，整局触发 6 次（= 5 炸弹 + 1 致命） | 字节差 |
| `0x8887D900`–`0x8887DCFF` | **通用定时器回调队列**（死亡倒计时经它调度；头部 `0x8887D978/980/984`，16 槽×40B @ `0x8887D988`，槽位 +0=标志 +32=回调 +36=参数） | 静态分析 `sub_880358C0/88035978` 实锤，见 §8 |

### 5.2 死亡瞬间（一次实测会话，采样号 S≈1958–1991）同时 1→0 的字节
`0x88619247`、`0x88619C54`、`0x88619C5B`、`0x8861B117`、`0x8861B123`（另见 `0x88619CEF` 16→15 复位型计时器等）。**尚未确定哪个是"死亡触发点"**——它们可能是副作用。

### 5.3 相关函数（`generated/CA022110/saidaioujou_recomp_tu1_recomp.*.cpp`）
| 函数 | 位置 | 说明 |
|---|---|---|
| `sub_880D2B18` | recomp.5.cpp:4（到 1733） | 按 ID 查实体、递减 `[r31+5308]`（one-shot）、变负清 `[r31+5304]`。**不是炸弹/死亡主逻辑** |
| `sub_880D36F0` | recomp.5.cpp:1734 | `sub_880D2B18` 的下一个函数 |
| `sub_880D3B00` | recomp.5.cpp:2337 | 被 `sub_880D2B18` 调用 |
| `sub_880CA6A8` | recomp.4.cpp:53297 | **文本/字形渲染器**：把每字符属性 `[r3+28]`（0–5）映射成标志（0x800000/0x8000/0x80），与炸弹无关 |
| 死亡块 14 个写入者 | 多个 TU | `sub_880358C0, sub_88035978, sub_88036900, sub_88036B38, sub_88036C10, sub_8808F728, sub_8808F3E0, sub_8808F970, sub_8807E720, sub_8804A860, sub_880E7700, sub_880D6F08, sub_88034188, ArrangeGameWorker` |

---

## 6. 已完成的分析历程（走了多少、怎么走的）

目标始终是找到"**炸弹计数**"或"**炸弹 vs 死亡的判定分支**"，从而让炸弹永不清零 / 死亡永不触发。

1. **误判 1（已回退）**：以为 `sub_880D2B18` 是"被击处理函数"、`[r31+5308]`（一度记成 `0x88617E0C`）是炸弹数，在其递减处打了"地板"补丁。结果无效，且 `0x88617E0C` 实为每帧浮点数据（负数），地板会把它破坏。
2. **误判 2（已回退）**：把补丁条件写成 `(ctx.r31.u32 + 5308) == 0x88617E0C`——实际 `[r31+5308]` 落在 `0x8861923C`，条件永不成立，补丁是空操作。
3. **加探针后确认**：在 `sub_880D2B18` 递减处加 `LogHit`，读到 `[r31+5308]=0x8861923C` 是 **one-shot（0→−1）**，`[r31+5304]` 恒为 0。**证明 `sub_880D2B18` 不是玩家炸弹/死亡逻辑**（它更像一个按 ID 处理的实体/子弹处理）。
4. **字节差（byte-diff）发现法**：逐帧对一段模拟内存做差，记录"小计数(0..16)恰好减 1"（带 12 帧冷却抑制逐帧计时器）。在 `0x88610000–0x88620000` 里**没有找到干净的 5→0 单向炸弹倒计**——最频繁的都是复位型（16→15、2→1、1→0 循环）。**结论：炸弹数要么不是简单的 1 字节递减、要么是位掩码、要么不在这个窗口。**
5. **锁定死亡区域**：死亡/倒计时状态在 `0x8887D900–0x8887DCFF`，**一直在之前监视窗口之外**——这是之前一直没抓到的根因。

### 分析规模
- 精读/核对了 Arrange 模块中多个大函数（`sub_880D2B18` ~1730 行、`sub_880CA6A8` 文本渲染、以及 recomp.5/4.cpp 大段）；
- 跑了 **多次"编译→部署→用户实机死亡→读日志"循环**（游戏会真实运行、真实死亡），累积了 store 钩（WRT）与字节差（DEC）两份日志；
- 重写了探测头 `sdoj_trace.h` 两版（计数器窗口版 → 死亡区域版）。

---

## 7. 已排除的错误假设（别再踩）

- ❌ `0x88617E0C` 是炸弹数 → **它是 `sub_880CA6A8` 的每帧浮点/文本数据**，且不在 `sub_880D2B18` 的 `[r31+5308]` 上。
- ❌ `sub_880D2B18` 是玩家"被击/炸弹"处理 → 它按 ID 查实体、递减的是一个 **one-shot** 计数器，`[r31+5304]` 恒 0，**不是死亡标志**。
- ❌ 在 `0x88610000–0x88620000` 用"字节恰好减 1"找炸弹 → 找不到（炸弹很可能不是这种编码，或不在该窗口）。
- ❌ 把 `[r31+5304]=0` 当"清除存活标志=死亡" → 实测它本来就是 0。

---

## 8. 最终状态（2026-09-07：**任务完成，无敌模式已实测生效**）

**结果**：方案 B（咽喉点拦截）一次成功。用户用 `invincible.bat` 实测：耗光炸弹后再被击不死、无倒计时、无卡死/闪烁/音效异常。

**最终补丁**（`generated/CA022110/...recomp.4.cpp`，`sub_880B9FA0` 入口）：
```cpp
if (sdoj_patch_flags::invincible_enabled()) { ctx.r3.s64 = 1; return; }
```
- 用 cvar `sdoj_invincible` 门控：`invincible.bat`（`--sdoj_invincible=true`）开启，`launch.bat` 为原生行为对照组。
- 4 个死亡调用点（recomp.4.cpp 三条路径 + `sub_880BA8F8`）均不使用返回值，拦截安全。

**死亡调用链（实锤）**：
```
被击判定 → sub_880BB920（玩家主逻辑, recomp.4.cpp:19503）4 条路径：
  ① 标志位路径: [0x8860CD0C]（状态块基址 0x8860C828+1252）==1 → 清标志 → 死亡
  ② 直接路径（实测死亡走的这条, recomp.4.cpp:20932）: 查表 (v & 0xF0)==16 → 死亡
  ③ 状态路径（recomp.4.cpp:21226）
  ④ sub_880BA8F8（recomp.4.cpp:17213）
  → 全部汇聚 sub_880B9FA0（recomp.4.cpp:15643，死亡计数 [r10+1124]+1）← 补丁点
    → sub_880B8638（recomp.4.cpp:12112，死亡流程：状态字节 [r31+116]=3/[r31+117]=2、
                    180 帧倒计时（写 0x886191B0=180）、音效 3/4/5）
      → sub_880B6EA8（recomp.4.cpp:8904，清存活标志 0x88619247=0）
```
其他实锤：
- 存活标志 `0x88619247`：`sub_880FB098` 开局/重生置 1，死亡时 `sub_880B6EA8` 清 0。
- 被击后无敌帧：命中置 `0x886149F3`=1（+两个 double），42 帧后清除。
- 死亡后每帧更新器：`sub_880B8818`（倒计时期间反复写 `0x88619C54`）。
- `0x88614947` = 对象销毁队列计数低字节（非被击标志）；`0x8861B123` = 每帧翻转动画位（非死亡信号）。
- `0x8887D900–0x8887DCFF` = 通用定时器回调队列（tick=`sub_880358C0`，调度器=`sub_88035978`，16 槽×40B @ `0x8887D988`，槽位 +32=回调/+36=参数）；`0x8887D000–0x8887E000` 整块每帧高频率写入，store 钩不可行。
- **炸弹计数始终未定位**（不在 `0x8860C000–0x88620000`；可能位掩码/对象列表/更高区域编码）——不影响方案 B 成功。

---

## 8.5 Normal 模式（CA022100）无敌补丁（2026-09-07 实测通过）

**结论：Arrange 的分析方法完全适用于 Normal 模式**（同一游戏的两套玩法模块，逻辑同构、地址不同），但**补丁点不能照搬**——返回值语义不同。

### 函数映射（Arrange ↔ Normal）
| 角色 | Arrange (CA022110) | Normal (CA022100) |
|---|---|---|
| 被击登记 / 死亡咽喉点 | `sub_880B9FA0`（recomp.4.cpp:15643，返回值**不被使用**） | `sub_88079500`（recomp.2.cpp:15861，返回值**被使用**） |
| 死亡流程（180 帧倒计时启动） | `sub_880B8638`（recomp.4.cpp:12112，实体基址 `0x88617D80`） | `sub_880B71F8`（recomp.4.cpp:8976，实体基址 `0x88610000+208`） |
| 击杀 / Game Over 视觉 | （无独立对应物） | `sub_880B8B10`（recomp.4.cpp:12463，击杀计数 `[r10+1128]++`，调 `sub_880B71F8`，返回 1） |
| 玩家主逻辑 | `sub_880BB920`（recomp.4.cpp:19503） | `sub_880B90A8`（recomp.4.cpp:13275）/ `sub_880BA4C0`（recomp.4.cpp:16356） |

### 关键差异（坑）
- Normal 的 `sub_88079500`：**返回 1 = 登记成功**（掉命 `[r31+16]-1`、被击计数 `[r31+1124]+1`、调用方放被击特效 + 启动 180 帧死亡倒计时）；**返回 0 = 忽略**（命数已 0 或校验失败）。若照抄 Arrange 的 `return 1`，反而会**走进死亡路径**。
- Normal 的命数耗尽时，玩家主逻辑走"返回 0 路径"调 `sub_880B8B10` 触发 **Game Over 击杀**——必须同时封掉，否则不掉命也会 Game Over。

### Normal 补丁（双门控，均 `invincible_enabled()` 开关）
```cpp
// generated/CA022100/...recomp.2.cpp, sub_88079500 入口
if (sdoj_patch_flags::invincible_enabled()) { ctx.r3.s64 = 0; return; }  // 不掉命、不计被击数

// generated/CA022100/...recomp.4.cpp, sub_880B8B10 入口
if (sdoj_patch_flags::invincible_enabled()) { ctx.r3.s64 = 1; return; }  // 抑制 Game Over 击杀
```
- `sub_88079500` 共 8 处调用点（全在 `sub_880B90A8` 内）；`sub_880B8B10` 共 4 处（1 处在 `sub_880B90A8`，3 处在 `sub_880BA4C0`），已逐一核对全是被击/Game Over 路径。
- **注意**：CA022100 的 recomp.2.cpp / recomp.4.cpp 原本**没有** include `sdoj_patch_flags.h`（只有 recomp.0/1 有），打补丁时已补上（第 2 行）。
- 用户实测（2026-09-07）：Normal 模式撞弹不掉命、不死、无 Game Over；Arrange 模式回归正常。

**探测钩子现状（`src/sdoj_trace.h`）**：全部逻辑已被 `sdoj_patch_flags::trace_enabled()`（cvar `sdoj_trace`，**默认关**）门控，默认运行零日志、零字节差扫描；store 宏快路径只剩每次 store 4 次整数比较。以后若要再探测（如找炸弹计数做无限炸弹），加 `--sdoj_trace=true` 即可复活 DIE/HIT/CHG 全套钩子。

## 9. 后续可选工作（非必需）

- **方案 A（无限炸弹，UX 更完整）**：让每次被击照常触发自动炸弹（清弹+无敌帧）但炸弹数永不清零。难点是炸弹计数未定位：可 `--sdoj_trace=true` 把 CHG 区域扩到 `0x88620000–0x88640000` 再跑一次死亡局，或静态确认直接路径②的查表语义（recomp.4.cpp:20868–20932，`(v & 0xF0)==16`，表基址 `0x88715BFC/0x88715C18`——高半字节可能是炸弹相关计数）。
- **被击反馈**：当前炸弹耗尽后再撞弹无任何反馈（无清弹无无敌帧）。如需可让拦截点同时写被击无敌字段（`0x886149F3`=1 + 两个 double 计时）模拟无敌窗口。
- **通用注意**：补丁一律 `invincible_enabled()` 门控；Arrange 改 `generated/CA022110/`、Normal 改 `generated/CA022100/`（两套模块地址不同、返回值语义不同，见 §8.5）；store 钩必须"快路径内联、命中才回调"。

---

## 10. 相关文件清单

| 文件 | 作用 |
|---|---|
| `src/sdoj_trace.h` | **探测/补丁核心头**（当前：死亡区域 store 钩 + 字节差；被 12 个 Arrange TU include） |
| `src/sdoj_patch_flags.h` | cvar 访问器：`invincible_enabled()` / `input_enabled()` / `render_enabled()` / `trace_enabled()` |
| `src/main.cpp` | 主 exe；定义 cvar `input_patch/render_patch/sdoj_trace/sdoj_invincible` |
| `generated/CA022110/saidaioujou_recomp_tu1_recomp.{0..11}.cpp` | Arrange 重编译 TU（cheat 目标；recomp.1=worker，recomp.5 含 `sub_880D2B18` 等） |
| `generated/CA022100/...` | Normal 模式 DLL（已打双门控无敌补丁，见 §8.5） |
| `generated/default/...` | 主 exe 重编译 |
| `C:\baidunetdiskdownload\SDOJ-Recomp\{launch,invincible,trace}.bat` + `prepare_game.ps1` + `game_data/` + `user_data/` | 运行时目录 |
| `C:\baidunetdiskdownload\SDOJ-Recomp\sdoj_death.log` | 探测日志（WRT/CHG 行） |

---

### 一句话总结
> **任务完成（2026-09-07）**：无敌模式通过咽喉点拦截实现——Arrange 拦截 `sub_880B9FA0`（return 1，返回值无调用方使用），Normal 双门控拦截 `sub_88079500`（return 0，不掉命）+ `sub_880B8B10`（return 1，抑制 Game Over），均按 `sdoj_invincible` 开关控制；两种模式均已实测通过。探测钩子已由 `sdoj_trace`（默认关）门控。炸弹计数未定位，无限炸弹（方案 A）留作可选后续。
