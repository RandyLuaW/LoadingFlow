# LoadingFlow

**空间进出加载体验改善 —— 推门不再弹读条界面。**

[English](./README.md) | [简体中文](./README.zh-CN.md)

[![License](https://img.shields.io/badge/license-Dual%20(source--available%20%7C%20GPL--3.0)-red)](./LICENSE)
[![Source](https://img.shields.io/badge/source-audit%20only-black)](./LICENSE)
![Game](https://img.shields.io/badge/Skyrim-SE%201.5.97%20%7C%20AE%201.6.x%20%7C%20AE%201.7.x(experimental)-blue)
![Runtime](https://img.shields.io/badge/SKSE-2.0.20%2B-blue)

一个 Skyrim SE/AE 的 SKSE 插件。它去掉加载门（load door）上的读条界面，并在运行时把引擎自己的几个过渡计时器调短。每扇门走的仍然是引擎自己的**真实 cell 切换**，所以寻路、NPC、战斗、遭遇与任务事件的表现与没装这个插件时完全一致。

> 本仓库源码**仅供安全审计**，不是开源项目：默认选项下不授予修改与再分发权利，详见 [LICENSE](./LICENSE)。另有 GPL-3.0 替代选项，预编译二进制即以该选项分发。

---

## 它做了什么

| 模块 | 底层实现 | 实际效果 |
|---|---|---|
| **屏蔽读条界面** | Hook `LoadingMenu::ProcessMessage`，对引擎发出的 `kShow` / `kReshow` 直接返回 `kIgnore` | 门转场时读条菜单**根本不会被创建**；快速旅行与读档的原版读条界面原样保留 |
| **压缩过渡计时器** | 运行时改写已注册的 INI 设定值：`fFadeToBlackFadeSeconds`、`fNormalDoorFadeSecs`、`fMinSecondsForLoadFadeIn`、`fPostLoadUpdateTimeMS` 等 | 按 E 到引擎请求菜单的前段等待由出厂 **1002~1022 ms** 降到 **694~734 ms**（方差 <5%）；`fPostLoadUpdateTimeMS` 由出厂 **2000 ms** 降到 **100 ms** |
| **门前模型预读**（可选） | 走近加载门时用 `BSModelDB::Demand` 预读目标 cell 的模型，按 8ms/片 预算分片 | 把模型 I/O 分摊到走向门的那段路上；**对总耗时的实测影响：中性** |
| **分段耗时打点** | 在「按下 E → 引擎请求菜单 → cell 切换完成」三个点记时间戳 | 输出总耗时 N 与前后段拆分，让瓶颈可量化而不是靠猜 |

---

## 设计原则

- **保留真实 cell 切换。** 不复制场景、不改关卡数据、不拦截传送——这正是 NPC 寻路、战斗与任务事件维持原版的原因。
- **不篡改任何引擎机器码。** 全部改动仅限于交换两个虚表（vtable）条目，以及运行时改写引擎已注册的 INI 设定值；不写指令补丁，也不使用 SKSE 跳板。
- **读档后会重新应用设置**，因为游戏在读档时会重读 INI，否则会把值还原成出厂默认。
- **基于 CommonLibSSE-NG**（含 spdlog）构建；可选的游戏内设置菜单需要 `-DWITH_MENU=ON` 与 SKSE Menu Framework 3。

---

## 它不做什么（安装前请先读）

- **不会让加载变瞬发。** 在重度模组改造环境下，从引擎请求菜单到 cell 切换完成，真实加载仍在秒级。这是那套加载顺序本身的物理成本，插件压不下去。
- **盖不住切换瞬间的场景装配。** 引擎会把新场景短暂地旋转并缩放到位。只有黑屏能盖住它，而黑屏与「到达后立刻见景」互斥——本插件选择后者。
- **默认不接管所有传送。** 只处理 `TESObjectDOOR` 的激活；用 activator + 脚本 `MoveTo` 或触发器体积实现的传送不在范围内。可选开关 `bGlobalMenuSuppression` 能放宽，但快速旅行与读档永远豁免。
- **不是开放世界无缝化。** 室内仍然是独立 cell。

---

## 实测数字

同一对门，每组前完全重启：

| 阶段 | 改前 | 改后 |
|---|---|---|
| 按 E → 引擎请求读条菜单 | 1002~1022 ms（出厂 `fFadeToBlackFadeSeconds` = 1.0） | **694~734 ms** |
| 引擎请求菜单 → cell 切换完成 | — | 秒级，负载驱动——此处无法压缩 |
| `fPostLoadUpdateTimeMS` | 出厂 **2000 ms** | **100 ms** |

> 比较时**不要**拿同一次运行里连续进出同一扇门的数据——文件进了系统缓存，后段必然逐次下降。只能比「一次运行内的第一次」或「两次运行的同一序位」。

---

## 构建

前置：Windows、Visual Studio 2022、CMake ≥ 3.21、[CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG)（含 spdlog）。

```bat
cmake -S . -B build -DCommonLibSSE_DIR=<指向 CommonLibSSEConfig.cmake 的目录>
cmake --build build --config Release
```

产物：`build/Release/LoadingFlow.dll`。

- `-DWITH_MENU=ON` 构建游戏内设置菜单，需要把 [SKSE Menu Framework 3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3) 的 `SKSEMenuFramework.h` 放进 `include/` —— **该第三方头文件未随本仓库分发**。
- `-DCLIBSSE_SRC=<dir>` 可把本地 CommonLibSSE 源码树也映射掉，避免绝对路径进入二进制。

---

## 安装

用发布渠道的 FOMOD 安装。它会问两件事：你的游戏运行时（SE 1.5.97 + AE 1.6.x，或 AE 1.7.x 实验版），以及是否需要中文语言包。

**安装前请先删除本插件的旧副本**——两个 DLL 会把同一个引擎函数 hook 两遍。

---

## 已知边界（都查过了，不必重试）

| 方向 | 结论 |
|---|---|
| 引擎自带 linked-interior 预载（`bPreloadLinkedInteriors`） | **开启会导致读档永久卡死**，在两个不同预载上限下各复现一次 → 插件已彻底不再引用它 |
| 调大 `iPreloadSizeLimit` | 它只是容量上限、不是开关；单独调大无可测收益 |
| 双客户端 / 截图式加载画面 / 加载前厅 / 克隆场景 | 均否决：世界状态必然不一致、盖不住人物与视角差异、凭空造玄关破坏空间逻辑、引擎级联释放资源导致崩溃 |
| 用 image-space 黑屏盖住切换 | 不成立：`FadeToBlackImod` 只作用于世界渲染，UI/Scaleform 在其之上合成 |

---

## 许可

**双许可：**

1. **默认 —— 源码公开、保留所有权利。** 源码公开是为了让人阅读与审计；不授予修改、衍生、再分发与商用权利。
2. **替代 —— GNU GPL v3.0。** 你可以选择按 GPL-3.0 使用本作品（[全文](./LICENSE-GPL-3.0)）。

**为什么需要第二个选项：** 预编译插件包含可选的游戏内菜单，而该菜单链接了 SKSE Menu Framework 3（GPL-3.0）。因此该二进制只能依 GPL-3.0 分发，**预编译二进制仅按 GPL-3.0 选项提供**。以 `-DWITH_MENU=OFF` 构建的版本不含 GPL-3.0 代码，只受默认条款约束。

本仓库同时也是上述二进制的 GPL Corresponding Source。

第三方组件及其许可：[THIRD-PARTY-NOTICES.txt](./THIRD-PARTY-NOTICES.txt)。

---

## 致谢

- [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) — MIT
- [spdlog](https://github.com/gabime/spdlog) — MIT
- [SKSE Menu Framework 3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3) — GPL-3.0（游戏内菜单）
- [Faster Loadscreens](https://www.nexusmods.com/skyrimspecialedition/mods/182852)（1001Bits）— 公开了这些引擎设置的出厂值
- [Keizaal Fast Cell Transition](https://www.nexusmods.com/skyrimspecialedition/mods/196720) — 记录了加载后淡入的实际行为
- SKSE 团队与 Address Library 维护者

按「现状」提供，不附带担保。请先备份存档。与 Bethesda Softworks 及 SKSE 团队无隶属关系。
