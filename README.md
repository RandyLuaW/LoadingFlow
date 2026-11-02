# LoadingFlow

**空间进出加载体验改善 · Smoother loading for doors and interiors**

[![License](https://img.shields.io/badge/license-All%20Rights%20Reserved-red)](./LICENSE)
[![Source](https://img.shields.io/badge/source-available%20for%20audit%20only-black)](./LICENSE)
![Platform](https://img.shields.io/badge/Skyrim%20SE-1.5.97-blue)
![Runtime](https://img.shields.io/badge/SKSE-2.0.20-blue)

> 本仓库公开源码**只供阅读与安全审计**，不是开源项目：**不授予修改、衍生、再分发权利**。
> This repository is source-available **for review only** — modification and redistribution are
> **not** granted. See [LICENSE](./LICENSE). English text prevails.

---

## 这是什么

一个 Skyrim SE 的 SKSE 插件。它**不复制场景、不改关卡、不拦截传送**：所有门仍然走引擎自己的
真 cell 切换，因此寻路、NPC、战斗、任务事件全部保持原版行为。插件只做四件事：

| 做的事 | 怎么做 |
|---|---|
| 不显示读条界面 | Hook `LoadingMenu::ProcessMessage`，对 `kShow` / `kReshow` 返回 `kIgnore`，菜单根本不创建 |
| 记录各段耗时 | 在"按 E → 引擎请求菜单 → cell 切换完成"三个点打时间戳，输出 `N` 与前后段拆分 |
| 压掉引擎的过渡计时器 | 运行时修正 `fFadeToBlackFadeSeconds`、`fNormalDoorFadeSecs`、`fMinSecondsForLoadFadeIn`、`fPostLoadUpdateTimeMS` 等 |
| 门前模型预读（可选） | 走近门时用 `BSModelDB::Demand` 把目标 cell 的模型读进内存，8ms/片预算 |

**它不修改任何一段引擎机器码**：全部改动是交换两个 vtable 条目加改写已注册的 INI 设置值。

## 它明确不做什么

诚实说明，避免期待错位：

- **不消除加载本身**。在这台测试机（约 3800 个插件）上，真实加载仍需 **1.0~2.6 秒**，这是该
  加载顺序的物理成本，插件层无法压缩。
- **不消除切换瞬间引擎把新场景"旋转+缩放展开"的过渡帧**。那是引擎行为，只能靠黑屏遮住，而黑屏
  与"到达后立刻见景"互斥（本项目默认选择后者）。
- **不处理非门触发的传送**（`ACTI` + 脚本 `MoveTo`、触发器体积）。默认只接管 `TESObjectDOOR`
  的激活；可选开关 `bGlobalMenuSuppression` 可放宽，但快速旅行与读档永远豁免。
- 快速旅行、读档的原版读条界面**原样保留**。

## 实测数据（同一对门，每组前完全重启）

| 阶段 | 耗时 | 性质 |
|---|---|---|
| 按 E → 引擎请求 LoadingMenu | 694~734 ms | 计时器，方差 <5%，已压到下限 |
| 引擎请求 → cell 切换完成 | 1019~2577 ms | 负载驱动的工作量，是唯一剩下的瓶颈 |

对照组：未应用计时器修正时，前段为 **1002~1022 ms**（`fFadeToBlackFadeSeconds` 的出厂值 1.0）；
本机 `fPostLoadUpdateTimeMS` 出厂值为 **2000 ms**，应用后为 100 ms。

> 注意：**同一组内反复进同一对门，后段必然逐次下降**（文件进入系统缓存）。比较实现效果时只能
> 比"组内第一次"或"两组同一位次"，不能拿组内递减曲线当优化成果。

## 构建

前置：Windows + Visual Studio 2022 + CMake ≥ 3.21 + [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG)
（含 spdlog）。

```bat
cmake -S . -B build -DCommonLibSSE_DIR=<path to CommonLibSSEConfig.cmake>
cmake --build build --config Release
```

产物为 `build/Release/LoadingFlow.dll`。

- 可选项 `-DWITH_MENU=ON` 需要自身从 [SKSE-Menu-Framework-3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3)
  取得 `SKSEMenuFramework.h` 并放进 `include/`——**该第三方头文件未随本仓库分发**。
- `-DCLIBSSE_SRC=<dir>` 可把本地 CommonLibSSE 源码树也映射掉，避免绝对路径进入二进制。

## 已知边界（有证据的结论，不必重试）

| 方向 | 结论 |
|---|---|
| 引擎自带 linked-interior 预载（`bPreloadLinkedInteriors`） | **开启会导致读档永久卡死**，在两个不同预载上限下各复现一次 → 已从代码中移除 |
| 调大 `iPreloadSizeLimit` | 它只是**容量上限不是开关**，单开调大对门加载无可测效果 |
| 双客户端 / 截图式加载画面 / 加载前厅 / 克隆场景 | 分别因世界状态不一致、盖不住人物与视角差异、空间逻辑穿帮、引擎级联释放必崩而否决 |
| 用 image-space 黑屏盖住 UI | 不成立：`FadeToBlackImod` 只作用于世界渲染，UI/Scaleform 在其之上合成 |

## 许可

**源码公开仅供审计，保留所有权利**：不授予修改、衍生、再分发、商用权利，也不允许建立公开
分叉或重新上传。详见 [LICENSE](./LICENSE)。第三方组件许可见
[THIRD-PARTY-NOTICES.txt](./THIRD-PARTY-NOTICES.txt)。

## 免责

本软件按"现状"提供，不附带任何担保；使用导致的存档损坏、崩溃或其他损失由使用者自行承担。
请自行备份存档。与 Bethesda Softworks 及 SKSE 团队无任何隶属关系。
