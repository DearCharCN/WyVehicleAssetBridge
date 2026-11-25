---
name: wy-vehicle-publish
description: 使用 WyVehicleAssetBridgeManager.exe 检查载具资源桥接工具的环境，编译或发布本仓库的 UE5 和 3ds Max 插件。适用于首次安装、指定单端发布、按源码差异增量发布和排查发布失败。
---

# 发布载具资源桥接插件

在仓库根目录运行 `WyVehicleAssetBridgeManager.exe`。它读取旁边的 `build_and_deploy.ini`，也支持用户指定 `-Config <INI路径>`。UE 工程与 Max 安装位置以该 INI 为准。

| 用户要做的事 | 传给 EXE 的参数 |
| --- | --- |
| 检查环境配置页的七项路径 | `check-env` |
| 检查某端发布所需的配置与目标目录 | `publish -Target UE -Check` 或 `publish -Target Max -Check` |
| 发布一端，包括首次安装 | `publish -Target UE` 或 `publish -Target Max` |
| 全部发布 | `publish` |
| 预览哪些已安装的插件有源码改动 | `publish -ChangedOnly -Check` |
| 仅发布有源码改动的插件 | `publish -ChangedOnly`，可加 `-Target UE` 或 `-Target Max` |
| 仅编译到本地 `build/latest` | `publish -BuildOnly`，可加 `-Target UE` 或 `-Target Max` |

在 PowerShell 中把输出接到 `Out-Host`，等待这个 Windows 图形程序结束后立即读取 `$LASTEXITCODE`。例如，用户要求增量发布时运行：

```powershell
& .\WyVehicleAssetBridgeManager.exe publish -ChangedOnly | Out-Host
exit $LASTEXITCODE
```

增量发布按安装目录中记录的源码指纹筛选。用户只要求增量时，如实报告被跳过的未安装或无指纹插件。用户要求首次安装或更新无指纹旧包时，用对应目标的普通发布建立基准。仅编译会更新本地产物，安装版本仍以最近一次成功部署为准。

发布前需要关闭所选目标的 UE 编辑器或 3ds Max。退出码 `0` 表示命令成功；根据输出区分实际更新和「没有需要增量发布的插件」。退出码 `1` 时，报告失败项或目标，编译错误可从输出给出的 `ue_plugins/build/logs/` 或 `3ds_plugins/build/logs/` 日志定位。
