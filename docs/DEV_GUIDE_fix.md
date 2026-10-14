# 开发者指南 / Developer Guide（fix 分支交接文档）

> 本文档面向接手 `fix` 分支的开发者：先讲清项目框架与构建方式，再逐条说明本分支
> 相对上游 `dev` 的改动内容、动机与验证方法。所有改动均在 vivo PD2302B
>（天玑 9200，Mali-G715-Immortalis MC11，GLES 驱动 r38p1，Vulkan 1.3.219）上实测。

---

## 1. 项目框架（30 秒版）

MobileGL 是一个**桌面 OpenGL 4.x → 移动端 GLES/Vulkan 转译层**，让 Minecraft Java
+ Iris/OptiFine 光影在 Android 设备上运行。三个组成部分：

```
┌──────────────────────────────────────────────────────────────┐
│ android-plugin/   Android APK：FCL 启动器渲染插件 + trace 工具 │
│   app/src/main/   plugin flavor：打包 libMobileGL + ANGLE 库  │
│   app/src/trace/  trace flavor：apitrace 重放/基准测试工具     │
├──────────────────────────────────────────────────────────────┤
│ MobileGL/         核心库 libMobileGL.so（C++，本目录主体）     │
│   MG_Impl/…       入口：EGL/GL 上下文、GetProcAddress 覆盖    │
│   MG_State/       桌面 GL 状态机 + 程序链接（ProgramLinkTask） │
│   MG_Util/        着色器转译管线（glslang→SPIR-V→SPIRV-Cross）│
│     ShaderTranspiler/  含 SpirvPasses/（自定义 SPIR-V 优化遍）│
│     BackendLoaders/    ANGLE/系统库加载（Loader.cpp）          │
│   MG_Backend/     双后端：                                    │
│     DirectGLES/   ── 直连原生 GLES 驱动（Mali 上最优路径）     │
│     (DirectVulkan/"Magma") ── Vulkan 后端，实验性             │
├──────────────────────────────────────────────────────────────┤
│ 3rdparty/         vendored glslang + spirv-tools（含本地补丁） │
│ tools/trace_replay/  重放 fixture 与基准工具                  │
└──────────────────────────────────────────────────────────────┘
```

**调用链**：Minecraft(LWJGL) → FCL 加载本插件 APK 提供的渲染库 → libMobileGL
把桌面 GL 调用转译到两条通道之一：
- **DirectGLES**：状态机翻译 → ESSL 3.2 → 原生驱动。ANGLE 可选叠加（GLES→Vulkan）。
- **DirectVulkan**：Magma 后端，SPIR-V 直上 Vulkan（Mali r38p1 需要兼容开关，见 §4）。

## 2. 构建与测试

```powershell
# plugin APK（交付物）
$env:MOBILEGL_ABIS="arm64-v8a"
gradle.bat --no-daemon -p G:\MobileGL\android-plugin :app:assemblePluginRelease `
  "-Pmobilegl.apkSuffix=你的后缀"
# 需要 env：SIGNING_STORE_PASSWORD / SIGNING_KEY_ALIAS / SIGNING_KEY_PASSWORD

# trace APK（A/B 重放验证，debuggable 可被 simpleperf 采样）
gradle.bat --no-daemon -p G:\MobileGL\android-plugin :app:assembleTraceDebug
# 重放 intent 模板见 android-plugin/TRACE_REPLAY.md
```

C++ 侧由 gradle 通过 CMake 构建；单测在 `MobileGL/MG_Test/`，本分支新增
`RelocateVertexImagesTest`（验证 VS image 重定位遍）。

## 3. 配置系统（重要：三层优先级）

```
MOBILEGL_CONFIG_FILE 指定的文件  >  标准路径配置文件  >  进程环境变量  >  内置默认
```

- **配置文件**（本分支新增，`ConfigLoader.cpp::InitializeAcceptedConfigFileVariables`）：
  `/sdcard/MobileGL/mobilegl.conf`（主）/ 插件私有 external dir（兜底）。
  key=value 格式，键必须是已接受的 `MOBILEGL_*` 前缀。
  **为什么需要它**：FCL 启动器对"用户切换型"环境变量传参不可靠（静态 boatEnv 能到
  游戏进程，切换项可能丢）。且 Android 11+ 任何应用都**无法读其他应用的
  `/sdcard/Android/data/<包名>/`，即使有 MANAGE_EXTERNAL_STORAGE**（FUSE 层强制，
  实机已验证）——所以文件必须放共享存储。
- **APK 内设置界面**（`SettingsActivity`，入口在 PostActivity 顶部按钮）：
  切换后端（DirectGLES/DirectVulkan）、ANGLE 开关、Magma 兼容开关，写上述配置文件。
  改动需**完全杀掉游戏进程**后生效（配置在 libMobileGL init 时锁存）。
- logcat 过滤 `MobileGL` 可见 `Config: reading config file` / `applied config file
  override` 确认生效。

## 4. 本分支改动清单（按 commit 顺序）

### ① fix(shader)：顶点着色器 image uniform 重定位（Mali r38p1 兼容）
- **背景**：Mali r38p1 驱动不支持顶点阶段的 image uniform
  （`GL_MAX_VERTEX_IMAGE_UNIFORMS=0`），Solas V3.6、iterationT 2.0 等光影包着色器
  直接链接失败。
- **方案**（新增 SPIR-V 遍 `SpirvPasses/RelocateVertexImagesPass`）：把顶点阶段的
  image 读取改写为"普通采样器 texelFetch + 合成 `mg_imglod_` LOD uniform"，
  绑定到保留纹理单元；`DirectGLES` 绘制路径（`Managers.cpp`）在每次 draw 前把
  应用绑定到 image unit 的纹理镜像到保留单元，并写入 Level。
- **联动**：`ProgramLinkTask` 在重定位生效时跳过顶点 image 数量检查；
  `TranslationCache` 缓存键加入"是否重定位"防跨驱动误用缓存。
- **附带性能修复**（`DirectGLES.cpp`）：FragColor 广播计数从**等值比较改为高水位
  比较**——Iris 类光影同一 gbuffers 程序交替画 shadow FBO(1 附件)与 gbuffer FBO
  (多附件)，等值比较导致每次 pass 切换都重新 detach/转译/重链接（profile 确认为
  稳态 draw 热路径顶部）。广播输出只复制到固定 location，超出 draw FBO 槽位的输出
  由驱动经 glDrawBuffers 路由后忽略，大 count 构建可正确服务更小 count。
- **验证**：`RelocateVertexImagesTest`；native 通道 Solas V3.6 实机可进世界。

### ② fix(loader)：eglSwapBuffersWithDamageEXT 改为可选解析
- 该扩展在部分驱动/ANGLE 组合上不存在，原代码硬解析失败即中止进程（trace 重放
  崩溃根因）。改为可选，缺失时回退 `eglSwapBuffers`。

### ③ feat(config)：配置文件覆盖机制 + APK 内设置界面
- 见 §3。涉及 `ConfigLoader.cpp`、`SettingsActivity.java`（新增）、
  `AndroidManifest.xml`（新增 MANAGE_EXTERNAL_STORAGE）、`PostActivity.java`（入口按钮）、
  `android-plugin/app/src/plugin/jniLibs/`（ANGLE 库随 APK 分发）、
  gradle SDK/NDK 版本对齐（compileSdk 35 / NDK 27.1）。

### ④ feat(trace)：重放工具增强
- `use_angle` 改为三态（显式 true/false/未设置），显式 false 时注入
  `MOBILEGL_ESPRYT_USE_ANGLE=0`（ConfigLoader 把"变量不存在"读作 ANGLE 开，
  "0"是回到 native 的唯一方式）。
- 新增 negative-control extras：`disable_unpack_ring` / `disable_upload_ring` /
  `disable_ubo_ring` / `disable_invalidate_flush` 等，用于单变量 A/B 定位渲染分歧。
- fixture 参考图更新（Derivative/BSL 在 Mali 上的实际输出）。

## 5. 已知问题与注意事项

- **Program N "has frontend global UBO storage, but backend has no
  MGL_GLOBAL_UBO block" 警告**：前后端转译不一致，受影响程序的 uniforms 无处绑定，
  可能导致光影渲染错乱（部分光影包全屏黑/天空异常，待修复）。
- **DirectVulkan（Magma）在 Mali r38p1 上**：需同时开启
  `MOBILEGL_MAGMA_DISABLE_SUBGROUP=1` + `MOBILEGL_MAGMA_R11G11B10F_FALLBACK=1`
  （设置界面已内置勾选项）；设备不支持 multiViewport 与
  VK_EXT_vertex_attribute_divisor，有对应 WARN。
- **Mali r38p1 不支持顶点 image uniform**：由 ① 的重定位机制兜底。
- `3rdparty/` 是 vendored 带补丁的：spirv-tools 的 `pass.cpp` 已恢复上游
  assert 门控写法（原仓库把它写在 if 条件里，Release 下每个优化 pass 结束都
  重建 DefUseManager，占优化器自身成本约 55%）。改 3rdparty 时注意保留。
- 原生通道性能基线：不要让 native DirectGLES 路径出现性能倒退；修改热路径前先
  用 trace APK 基准模式（intent extras `benchmark=true` 等）建立 A/B 数据。
