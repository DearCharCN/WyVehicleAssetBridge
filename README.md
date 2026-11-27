# WyVehicleAssetBridge

在 3ds Max 2025 与 UE5 编辑器之间双向同步载具网格、参考骨架和车身碰撞。Max 原生 GUP 通过本机 TCP 连接 UE Editor 插件，双方读取内存中的场景与资产。接收和撤销均在接收端完成，由使用者决定何时保存文件。

## 使用载具面板

1. 打开启用了插件的望月 UE 工程。在 Max 选择 **望月载具工具 → 载具资源桥接**。面板会在后台连接，顶部显示 **正在连接**，成功后显示工程路径。点击 **重新连接** 可再次发起后台连接。默认端口为 `127.0.0.1:51976`。同一台机器运行多个编辑器时，只能有一个编辑器监听同一个端口。
2. 选择或输入载具类型，填写载具名和变体名，点击 **应用并比对**。类型列表会隐藏共用资源目录 `CommonVH`。变体默认为 `Base`。这三个字段随当前 Max 场景保存。目标目录为 `Content/Vehicle/{载具类型}/{载具名}/{载具名}_{变体名}/`。
3. 目标目录为空，或仅预放了 `Texture/`、`MaterialInstance/` 下的资源时，可以 **首次创建资源**。日常编辑后点击 **重新比对**。资源大纲分为**骨骼、骨骼网格体、静态网格体、碰撞体**。在类别、部位或单个 LOD 行点击 **Max → UE** 或 **UE → Max**，也可以多选后用右键一次同步。双击行展开或折叠。点击 **在大纲中选中** 列的 **选中**，Max 场景资源管理器会展开对应的层级并滚到该骨骼、LOD 网格或碰撞子节点。
4. 在 **碰撞 → 构建 BodyCollider** 选择 **整体凸包** 或 **上下凸包**。上下凸包可输入或左右拖动切面高度，在 Max 视口中检查半透明白色切面，然后点击 **生成**。要更新 UE 物理资产，请在大纲的 **BodyCollider** 父行点击 **Max → UE**。

| 大纲中的操作对象 | UE 资产（相对车辆目录） | 一次同步的范围 |
| --- | --- | --- |
| 骨骼 | Base 为 `Skeleton/SKEL_{载具名}`，其他变体为 `Skeleton/SKEL_{载具名}_{变体名}` | 整棵参考骨架的层级和参考姿势。旧车继续使用已有的 `_Base` 骨骼，同目录两份并存时按车身绑定选择 |
| `LODx_Tire_####`、`Hub_####`、`BrakeF`、`BrakeR`、`BrakeB` | `StaticMesh/SM_{部件名}`，例如 `SM_Tire_0000` 或 `SM_BrakeF` | 单个部件的一个 LOD，旧车可继续同步到原有无前缀资产 |
| `LODx_Body`、`LODx_DoorModelFL` 等蒙皮部件 | `SkeletalMesh/SK_{载具名}` | 共用骨骼网格中当前部件的一个 LOD |
| `BodyCollider` | `PhysicsAsset/PHYS_{载具名}_Body` | 挂在 Body 骨骼的整组凸包 |

首次创建时，车身骨骼网格的 **Physics Asset（物理资产）** 指向本车的 `PHYS_`，物理资产的 **Preview Mesh（预览网格体）** 指回本车的 `SK_`。旧物理资产若打开时提示使用 Cube，遇到删除缺失骨骼刚体的询问请选择 **取消**。在物理资产编辑器的 **预览网格体** 中选择本车 `SK_`，点击 **应用到资产**，再保存物理资产。

**首次创建**同时由 UE 生成两张可编辑的蓝图。以 `SportsCar106` 为例，Base 变体生成 `Blueprint/BP_SportsCar106` 和 `AnimBlueprint/ABP_SportsCar106`，Sport 变体则生成 `BP_SportsCar106_Sport` 和 `ABP_SportsCar106_Sport`。普通网格同步会保留蓝图中的手工设置。

车辆蓝图继承望月工程的 `WYAutomobileCS`。默认 FGear 骨骼网格组件使用本车的 `SK_` 资产和动画蓝图。四个轮骨 `D_WheelFL`、`D_WheelFR`、`D_WheelBL`、`D_WheelBR` 各挂一个轮毂、胎皮和刹车。新建轮组引用 `SM_Hub_0000`、`SM_Tire_0000`、前刹 `SM_BrakeF` 和后刹 `SM_BrakeR`。Max 使用旧后刹名 `BrakeB` 时生成 `SM_BrakeB`。前后轮的 FGear 半径和宽度取自胎皮的 LOD0 源顶点，以米存储。动画蓝图读取 FGear 前后轴的左右轮位移和旋转，在组件空间驱动相应轮骨。生成完成后可在 UE 一次撤销，资产由使用者手动保存。

网格通过 Max SDK 和 UE 编辑器源网格直传，包括有向三角形、位置、逐面角 UV、颜色、法线、切线及蒙皮权重。已匹配部件的归属保存在 UE 的逐 LOD 编辑器源数据中。遇到缺少可靠归属的旧资产，面板显示“待匹配”。几何修改请先在对应编辑器的工具中接受或提交到资产，再刷新比对。关卡实例上的涂色不参与同步。

**网格材质槽**随同一部件的 LOD 一起同步。Max 使用 `MI_{用途}`，UE 使用当前变体目录下的 `MaterialInstance/MI_{车名}_{用途}`。例如，`SportsCar106` 的 Max 材质 `MI_Seat_Front` 对应 UE 资源 `MI_SportsCar106_Seat_Front`，部位名中的下划线完整保留。旧场景中不带 `MI_` 前缀的材质继续按原名匹配。

首次创建实例时，插件按用途从上到下匹配父材质，关键字不区分大小写。父材质均位于 `Content/Material/Vehicle/`。目标路径已有材质时直接复用。以后的双向同步只处理网格引用、材质槽和逐面分配。

| Max 材质名包含 | 新实例的父材质 |
| --- | --- |
| `CarPaint` | `M_Vehicle_CarPaintWY_Master` |
| `Glass` | `M_Vehicle_Glass_Master` |
| `Carbon` | `M_Vehicle_CarbonFiber_Master` |
| 其余名称 | `M_Vehicle_PBR_BaseMat` |

普通 PBR 的参数与贴图路径规则统一定义在 UE 插件中。Max 首次创建时发送网格及其材质槽名，由 UE 创建实例并按下表从当前变体的 `Texture/` 查找 Texture2D。UE 的贴图赋值页签也使用这套规则。

| 实例参数 | 对应贴图 |
| --- | --- |
| `Color_Tex` | `T_{车名}_{用途}_D` |
| `M_Tex` | `T_{车名}_{用途}_MRO` |
| `Normal_Tex` | `T_{车名}_{用途}_N` |

创建时缺少哪一张，就让对应参数继续继承父材质默认值，并将该参数及预期的 UE 对象路径保存到实例的编辑器附加数据中。之后可在 UE 中按需补上贴图。

**在 UE 中给材质赋贴图**。在内容浏览器打开具体载具变体的目录或其子目录，选择 **工具 → 望月载具工具 → 贴图赋值到材质**。面板检查当前车辆的 `MaterialInstance/`，列出直接使用 `M_Vehicle_PBR_BaseMat` 的实例中已有对应贴图、但当前引用与上表不同的字段。每行可预览推荐贴图。点击行内的 **应用**，或多选后右键 **应用**，即可更新选择的字段。成功的行会显示 **已应用**，点击 **刷新** 后只列出仍待处理的字段。批量赋值支持一次撤销，保存时机由使用者决定。

UE → Max 使用 UE 网格当前的槽名和面分配。已有 ID 对应的材质改名时保留其着色参数。新引用的 ID 在目标网格上得到可编辑的占位材质。其他节点共用原材质时，原材质继续供这些节点使用。材质槽改名、换绑和逐面分配变化都参与“有差异”判断。

在 UE 新增的槽即使还没有指定材质，只要已有面使用它，回传时也会在 Max 建立占位。若槽还未命名，桥接会生成 `Material_<编号>`。Max 的 Multi/Sub-Object 可编辑槽编号上限为 60000；超出范围的原始面 ID 仍可随网格传输。如果 Max 无法用现有槽表达该面的分配，本次回传会提示原因并回滚。

UE 5.8.4 自带的骨骼网格刷色工具提交笔划时，会把整个 LOD 的 8 位颜色写回源网格。如果邻近部件原有浮点颜色无法用 8 位精确表示，它也可能显示“有差异”。桥接按资产实际保存到内存的颜色逐面角比较。

骨骼网格在 Max 中以第 0 帧参考姿势提取，需能从 Skin 明确对应到基网格。遇到额外的蒙皮修改器栈、无法表达的剪切，或删除仍有权重引用的骨骼，会在面板提示原因。**构建 BodyCollider** 单独读取当前帧的 LOD0 几何，允许未蒙皮的网格。Max 原有的凹形碰撞传到 UE 后会变为凸包，大纲仍会标记差异；接收 UE 的结果后，Max 可用一次撤销找回原网格。

逐部件身份及 UV 通道数随编辑器源资产保存。Windows Cook 实测打包后可正常加载骨骼网格及其 LOD，成品中已剥离这部分编辑器源数据。当前支持 LOD0 至 LOD7 的显式源网格。整车比对会读取完整源数据，大车型在此期间会占用编辑器主线程，每个 TCP 消息的上限为 256 MiB。

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

真实 UE 桥接回归需先编译 UE 插件（`publish -Target UE -BuildOnly`），再运行：

```bat
python tests\ue_bridge_integration.py
rem 可选：使用另一份环境配置
python tests\ue_bridge_integration.py --config build_and_deploy.local.ini
```

该测试读取 INI 中的引擎路径与工程配置，复制 `ue_plugins/build/latest` 插件到临时工程，以随机端口启动独立 UE 编辑器。验证部件与 LOD 隔离、顶点和三角形增删、逐面角的两个 UV 通道及法线、单独静态网格写入、原生撤销重做和碰撞生成；不写入配置的用户工程、不保存测试资产，结束时仅关闭自身启动的编辑器。日志、协议记录和 JSON 报告保留在 `ue_plugins/build/integration-tests/`。
