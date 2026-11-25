# WyVehicleAssetBridge

用于 3ds Max 与 UE5 之间双向同步载具资源的基础工程。Max 端使用原生 C++ GUP 插件，UE 端使用 C++ Editor 插件；当前两端只提供加载入口和日志。TCP 通信、网格差异同步与可逆导入规则将在后续实现。

## 项目布局

```text
3ds_plugins/
  WyVehicleAssetBridge/         Max 插件源码、CMake 与包描述
  build/
    latest/WyVehicleAssetBridge/  最新成功构建的 Max 插件包
    dependencies/            本机 SDK 缓存
    cmake/                   增量编译缓存
    logs/                    每次构建日志
ue_plugins/
  WyVehicleAssetBridge/         UE 插件源码与 .uplugin 描述
  build/
    latest/WyVehicleAssetBridge/  最新成功构建的 UE 插件包
    logs/                    每次构建日志
.agents/skills/wy-vehicle-publish/  项目级 AI 插件发布指南
src/WyVehicleAssetBridgeManager/   Windows 图形界面、命令行与插件发布源码
tests/                       构建部署的离线验证
WyVehicleAssetBridgeManager.exe    双击进入环境配置与发布插件界面
build_and_deploy.ini          配置程序在本机生成，Git 忽略
```

两侧任意层级的 `build/` 均被 Git 忽略。源码目录中的 UE `Binaries/`、`Intermediate/` 等生成目录也被忽略。构建产生的 `staging/` 在成功后清理，失败时保留用于诊断。

## 环境和配置

双击根目录的 **WyVehicleAssetBridgeManager.exe**。INI 中某项路径为空，或者本机还没有 INI 时，程序会尝试定位 UE5 和 3ds Max 2025，并查找下面四项依赖。望月工程使用“选择路径”指定 `.uproject`。UE5 和 Max 的自动检测结果也可以用“选择路径”更改。

已有配置值时只检查该值；如果路径失效，界面保留原值并显示原因。要重新自动查找某一项，可以清空 INI 中该项的值后重新打开配置程序。

程序会检测 CMake、3ds Max 2025 SDK、Windows SDK 10.0.22621.0 以及 Visual Studio 2022 的 MSVC v143 x64 工具链。检测范围包括常用安装目录、注册表、环境变量、Visual Studio 实例和本项目已有的 Max SDK 缓存。缺少某项时，点击对应的“自动安装”，按 Windows 的提示授权。安装结束后程序会再次检测。

七项通过后，程序会在 EXE 旁自动创建第一份 `build_and_deploy.ini`。修改已有配置时，点击“保存配置”生效。该文件仅保存在本机。桌面工具的源码位于 `src/WyVehicleAssetBridgeManager/`，开发者可运行该目录的 `build.ps1` 重新生成 EXE。

插件编译需要 UE5 对应的 C++ 构建环境、CMake 3.21 以上、Visual Studio 2022 的 v143 C++ 工具链，以及 3ds Max 2025 SDK。Max 插件固定针对 2025，使用 C++20、x64 和 Windows SDK 10.0.22621.0。[Autodesk 工具链要求](https://help.autodesk.com/cloudhelp/2025/ENU/MAXDEV-Developer/files/about_the_3ds_max_sdk/sdk_requirements.html) · [C++ 版本对齐表](https://aps.autodesk.com/developer/overview/me-vfx-platform-alignment)

所有机器相关路径保存在 EXE 旁的 `build_and_deploy.ini`。

| 配置 | 含义 |
| --- | --- |
| `UE.ProjectPath` | `.uproject` 文件，或只含一个 `.uproject` 的工程目录 |
| `UE.EnginePath` | 该工程实际使用的引擎根目录，目录下应有 `Engine/` |
| `Max.InstallPath` | 含 `3dsmax.exe` 的安装目录 |
| `Max.SdkPath` | 含 `include/max.h` 和 `lib/x64/Release/` 的 SDK 根目录 |
| `Build.CMakePath` | `cmake` 命令名或其可执行文件路径 |
| `Build.CMakeGenerator` / `CMakeToolset` | Max 使用的 CMake 生成器和 MSVC 工具集 |
| `Build.WindowsSdkVersion` | Max 使用的 Windows SDK 版本 |
| `Build.WindowsSdkPath` / `Build.VisualStudioPath` | Max 使用的 Windows Kits 根目录与 VS 2022 实例；UE 的编译器由所选引擎的 UnrealBuildTool 决定 |
| `Build.MaxConfiguration` | `Release` 或 `RelWithDebInfo` |

INI 使用 UTF-8，路径带空格无需引号；相对路径以 INI 所在目录为基准，支持 `%环境变量%`。引擎目录显式配置，避免工程关联未注册或多个同版本引擎之间的歧义。

如果已经单独安装 Max SDK，程序会验证其版本和所需的头文件、链接库。也可以从 [Autodesk SDK 下载页](https://aps.autodesk.com/developer/overview/3ds-max-api) 获取安装包。

## 发布插件

双击 `WyVehicleAssetBridgeManager.exe`，选择左侧的「发布插件」。页面显示已保存配置对应的安装目录。关闭要更新的目标应用后，点击「发布 UE 插件」或「发布 Max 插件」，程序会编译该端、核对产物并安装。「全部发布」同时编译两端，等两端都成功后再部署。进度和错误显示在发布日志中，完整构建记录在各端的 `build/logs/`。

每一端的已安装版本显示为源码 SHA-256 指纹的前 12 位。插件源码、构建配置或随包资源的相对路径和内容发生变化时，该端按钮变绿，左侧显示「源码与插件版本不一致」。此时「全部发布」右边会出现绿色的「增量发布」，点击后只编译并部署有差异的插件。把文件改回原样后，提示会自动消失。

首次安装或升级没有源码指纹的旧插件时，先用单端发布或「全部发布」建立基准。后续成功部署会更新已安装版本；仅编译只更新本地 `build/latest`。

## 使用命令

双击或无参数运行 EXE 打开图形界面。`check-env` 和 `publish` 在命令行运行，默认读取 EXE 旁边的 INI，`-Config` 可指定另一份配置。

`check-env` 逐项列出环境配置页的七项路径、验证结果和失败原因，并核对望月工程与引擎版本。INI 已填写的路径只验证原值；未填写的 UE5、3ds Max 和构建依赖按本机环境自动查找，望月工程需要在 INI 中指定。检查为只读，七项全部通过返回 `0`，任一项失败或配置文件读取出错返回 `1`。

```bat
rem 检查本机环境
WyVehicleAssetBridgeManager.exe check-env

rem 检查另一份 INI 对应的环境
WyVehicleAssetBridgeManager.exe check-env -Config build_and_deploy.local.ini

rem 发布前检查构建参数、文件路径与部署目录
WyVehicleAssetBridgeManager.exe publish -Check

rem 默认同时编译两端，全部成功后部署
WyVehicleAssetBridgeManager.exe publish

rem 增量发布，有差异时编译并安装，两端都一致时直接成功退出
WyVehicleAssetBridgeManager.exe publish -ChangedOnly

rem 预览增量发布将处理的插件
WyVehicleAssetBridgeManager.exe publish -ChangedOnly -Check

rem 应用仍打开时，只编译两端
WyVehicleAssetBridgeManager.exe publish -BuildOnly

rem 单独验证某一端
WyVehicleAssetBridgeManager.exe publish -Target UE -BuildOnly
WyVehicleAssetBridgeManager.exe publish -Target Max -BuildOnly

rem 使用另一份配置（此文件名已被 Git 忽略）
WyVehicleAssetBridgeManager.exe publish -Config build_and_deploy.local.ini

rem 单独编译并发布某一端
WyVehicleAssetBridgeManager.exe publish -Target UE
WyVehicleAssetBridgeManager.exe publish -Target Max
```

在交互终端中，Windows 可能让窗口程序在后台继续运行。需要等待发布完成并取得退出码时，PowerShell 可用 `.\WyVehicleAssetBridgeManager.exe publish | Out-Host` 后读取 `$LASTEXITCODE`，CMD 可用 `start "" /b /wait WyVehicleAssetBridgeManager.exe publish` 后读取 `%ERRORLEVEL%`。

UE 调用配置引擎的 `RunUAT BuildPlugin`，构建 Win64 Development Editor；Max 通过 CMake 编译并打包。默认两项构建并行运行，各自写日志。任意一端失败，整体返回非零退出码，保留原来的 `latest/` 和已部署插件。两端均成功后才更新最新产物，再进行部署。

`-Check` 验证配置、必需路径和已有部署目录的归属。编译器、SDK ABI、写权限和 DLL 锁定在实际执行中验证。同一工作区一次只能运行一个发布或构建任务。

## 部署与加载

- UE：`UE.ProjectPath` 对应工程下的 `Plugins/WyVehicleAssetBridge/`。如果 `Plugins` 为目录联接，自动解析实际目录并显示目标路径。
- Max：`Max.InstallPath/ApplicationPlugins/WyVehicleAssetBridge/`。包内的 `.gup` 由 Max 2025 启动时加载。

部署前关闭所选目标对应的 UE 编辑器或 3ds Max。安装目录位于 Program Files 时，运行程序的账号需要对该目录具有写权限。部署完成后重新启动应用加载插件。

程序只替换自身插件目录。两端文件先复制到临时目录，再替换目标。发生替换错误时会尝试恢复先前版本，无法自动恢复时会输出备份位置。已有同名目录若没有本工具写入的管理标记，程序会停止，避免覆盖其他来源的插件。UE 插件默认启用，工程若显式禁用它，需要先启用。

从旧名称升级时，程序会识别同一位置由本工具管理的旧插件目录。构建成功后迁移本地最新产物，发布成功后迁移安装目录。

## 离线验证

```bat
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\build_pipeline.tests.ps1
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File tests\environment_setup.tests.ps1
```

测试使用隔离的临时包和假编译器，覆盖路径配置、源码版本与按钮状态、增量发布、编译失败和部署回滚。
