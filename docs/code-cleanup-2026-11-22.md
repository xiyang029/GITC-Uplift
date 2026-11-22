# 冗余与死代码清理落地记录

本轮以 2026-11-22 开始实施前的磁盘工作树为基线，包含当时已有的暂存与未暂存改动。以下仅记录本轮新增清理；已有 XMX 上游改动与原暂存区保持原样。

## 按执行顺序完成

1. **接通 UI 步长。** `overlay.cpp` 的整数输入读取 `SettingDescriptor::step/fast_step`。DynamicMFGTargetFps 与 ReflexFpsCap 恢复 1/32 步长，其他行沿用其 schema 默认值。
2. **删除已停用的 Swin 诊断簇。** 移除恒关闭的 POST 探针、LaunchTrace、NR_THIN_ABBA benchmark、专属资源与参数缓存，以及始终为空的逐评估 set 覆盖容器。`UseSet` 直接记录传入值。保留扩展 POST 的识别与 Thin4 不兼容检查，以及正常 Thin4、Pre/PostPair4、Level4、barrier 与权重生命周期。
3. **删除无消费者的生成表。** 删除 778 行 `NrPassBorrowTables.h` 和唯一 include；该表没有运行时审计、注册或初始化副作用。
4. **删除 10 项只写字段。** 同步删除声明和赋值，真实操作保留，明细见下表。
5. **合并 13 组重复逻辑。** 共同判定、参数布局和调用骨架改为共享 helper；后端保留资源所有权、状态、退休实现与同步调用位置，明细见下表。
6. **移除过时翻译和功能占位。** 删除旧桥接状态的 3 个格式模板；删除 `PedestalRemoval` 枚举、Settings 字段、隐藏 schema 行及其标签、tooltip、独占的 Always 选项翻译。旧配置中的这个键按既有未知键规则忽略，其他有效设置继续加载；新序列化不再写出它。配置版本与 IPC 布局不变。
7. **退休没有输出端的 ViT 探针。** 删除 `NR_VIT_POOL_PROBE` 查询、capture kernel、诊断缓冲、readback 和销毁分支。正常 pooling/unpooling 与传给它们的参数布局保留，pool 的可选诊断输出指针维持默认 0。该环境变量不再触发诊断 GPU 工作。

生产源码相对实施前基线净减少 **960 行**，不含新增回归工具与本文档。这是源码规模变化，不是运行性能或显存收益测量。

## 删除的普通字段

| 所属类型 | 字段 | 保留的实际行为 |
| --- | --- | --- |
| BeforeFgDecision | outer_motion_candidate | BeforeFgOuterMotionCandidate 仍用于正常帧前判定 |
| ContextStatus | dlss_latch_tripped | 私有 latch、LatchTripped 与设备保护逻辑 |
| SessionStatus | passes_live | LiveFeatureCount 与 feature 生命周期 |
| CreateCall | override_rejected | override 失败重试和参数恢复 |
| HookedModule | vulkan_shutdown、d3d12_shutdown、d3d11_shutdown、d3d11_shutdown_all | shutdown detour、original 指针与 hook 日志 |
| SetupView | chain_split | 每行 runs/starts_group 与 pass_stages |
| DeviceRecord | storage_extended_enabled | 物理设备支持检测、feature 开启、创建失败回退 |

## 重复逻辑的共同实现

| 原重复实现 | 共享实现 | 后端保留内容 |
| --- | --- | --- |
| SettleLayout | addon/stage_math.hpp: SettleStagePlan | 各自 stage 状态、work_size 与 Session 通知 |
| SetBlockedReason | addon/stage_math.hpp: UpdateBlockedReason | 各自 blocked_reason |
| SetLookConfig | sources/stage_sets.hpp: ApplyLookConfig | 各自配置与所有 stage 历史状态 |
| NoteMeterBasis | sources/auto_exposure.hpp: UpdateMeterBasis | 各自曝光 readback 退休 |
| FacesConstants | color/faces_constants.hpp | 各 API 的 shader 绑定与 dispatch |
| WrapNvapi | nvfp4/Nvfp4Support.h: WrapKernelNvapi | FFN/ViT/Swin 独立 state、mutex、detour；原 3 项 FP8 wrapper 保留 |
| TextureDesc11/TextureDesc | d3d11_texture.hpp: TextureDesc11 | bridge 与 x86 client 分别使用轻量工具层 |
| Transition | d3d12_barrier.hpp: Transition | 调用位置、前后状态与顺序 |
| Stop | bridge/private_device_bridge.hpp: StopCrossApiWaits | Vulkan timeline 约束注释、各自资源 teardown |
| RetireSetsFrom | sources/stage_sets.hpp: RetireStageSets | Main → Look → Faces → Exposure 顺序与后端退休函数 |
| Diagnostic | nr/feature_state.hpp: FeatureDiagnostic | 各 feature 的诊断状态 |
| Release | nr/feature_state.hpp: ReleaseFeatureState | GPU 完成证明由调用者提供；Snippet 路由对应 API；Abandon 独立保留 |
| NoteTimeout | client/timeout.hpp: CountTimeout | 各 transport 的计数清零点、Stop 与资源释放 |

## 验证

使用 MSVC/Ninja，x64 清除全部构建产物后执行 `./build.cmd release`；x86 从空目录执行 `./build.cmd release-x86`。两者 exit 0，分别生成晚于源码修改的 `gitc-uplift.addon64` 与 `gitc-uplift.addon32`。清除产物避免本工具链本地化 `/showIncludes` 导致的头依赖遗漏；shader 也在 x64 全量构建中重新编译。

以下目标重新编译后全部 exit 0：

| 回归目标 | 覆盖内容 |
| --- | --- |
| uplift_cleanup_regression | 旧配置与序列化、FPS 步长、现有 Vulkan 嵌套翻译、选择性历史重置、曝光依据、退休顺序、布局边界、DLSS off 回退、超时阈值、NVAPI 空/未知入口与模型状态隔离 |
| uplift_nr_regression | recording/token 生命周期；256 个 GPU bit-exact 像素案例；16 个紧凑 Look 历史案例；曝光依据、深度 SRV 与 GPU timing |
| uplift_nvfp4_scratch_regression | 缓存淘汰、recording/execution lease、嵌套作用域与失败恢复 |
| uplift_helper_protocol_regression + uplift_helper64_identity_regression | 96 次 FENCED/SHARED_CPU 交接、2 次失效的组合交接、reply 发布与 GPU debug layer |
| uplift_vk_nr_regression | 32 个 Vulkan bit-exact fusion 案例、历史失效、Present staging/direct、双队列退休、失败 signal/wait 与 CPU 回退；同步验证开启 |

GPU 回归使用 NVIDIA GeForce RTX 5070 Ti。新增工具是 `EXCLUDE_FROM_ALL` 手动目标，正常发布构建不附带它。

`git diff --check` 通过。`src/xmx/generator.hpp` 与实施前快照逐字节一致；本轮未编辑 XMX vendor。IPC 的 `busy_skips/helper_pid`、Intel ABI、代理设备 ComPtr、通过数值/schema 使用的枚举及有效诊断入口均保留。现有 vendor 和环境变量 API 警告未修改。

未做真实游戏、真实 NVIDIA NR/XeFG 模型的画面或性能实测，也未自动点击 overlay 控件。专项回归验证 FPS schema 值与加载行为，overlay 调用处已经直接接入对应字段；构建和身份运行时回归不代表真实游戏全路径验证，不据此宣称 FPS 或显存改善。
