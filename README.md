# IHG2 中控屏本地桥接快速安装

本项目通过桥接程序把开发商自带的 IHG2 网关作为配件接入 Apple 家庭，从而将网关中已有的服务全部接入到 Apple Home 中。桥接服务直接运行在中控屏上，不依赖额外服务器。

## 小提醒

这是我个人维护的桥接工具，不是开发商官方软件。安装时会在中控屏放置桥接服务和开机脚本；虽然安装器会先检查环境，固件差异仍可能造成问题。请只在自己有权管理的设备上使用，并自行判断和承担安装、使用风险；预检有失败项时先停下来处理，不要硬装。

## 开始前

- 中控屏须为本项目对应的 RK3288 同类机型、32 位 ARMv7、Android API 25 或更新版本，并具有 root 权限。固件自带的 DNS-SD 库、mdnsd 服务和 flash_recovery 启动项由安装器自动检查；安装器只管理本桥自己的启动脚本，不会替用户安装系统组件。它不是适用于任意 Android 设备的通用安装包。
- 厂商的“全屋智控”应用须已完成该网关的本地配置，并把设备清单同步到屏端。桥读取应用留下的本地 SDK 数据，不使用账号或云端登录；应用不必在桥运行期间保持登录。首次检查还要求中控屏的邻居表中已有网关地址，通常让厂商应用连接一次网关即可。
- 使用 Windows 或 Linux 电脑；电脑需要安装 Android Platform Tools（adb）。中控屏须已开启 ADB 调试，并授权这台电脑。首次启用和授权不能由安装程序远程完成。
- 电脑、中控屏和网关须能在局域网中互相通信。

## 在电脑上安装 ADB

从 [Android 官方 Platform Tools 下载页](https://developer.android.com/tools/releases/platform-tools?hl=zh-cn) 下载与你的电脑系统对应的压缩包并解压。Windows 下载包里的 adb.exe 和配套文件要一起保留，不要只复制 adb.exe。

把下面路径换成 platform-tools 文件夹的实际位置。命令只对当前终端窗口生效；安装时请继续使用同一个窗口。

Windows PowerShell（假设解压到了 C:\platform-tools）：

    $env:Path += ";C:\platform-tools"

Windows 命令提示符：

    set "PATH=%PATH%;C:\platform-tools"

Linux Bash：

    export PATH="$PATH:/path/to/platform-tools"

Linux Fish：

    set -gx PATH $PATH /path/to/platform-tools

运行以下命令确认已能找到 ADB：

    adb version

## 安装

### 1. 解压发布包

把压缩包解压到维护电脑上的一个目录，在该目录中运行以下命令。

### 2. 运行安装前检查（建议）

Linux：

    ./preflight.sh 192.0.2.11

Windows PowerShell：

    .\preflight.ps1 192.0.2.11

将示例地址替换成中控屏的 IP。脚本会检查 ADB 连接、root 权限、硬件环境和存储情况；有检查失败时，先按提示处理。

### 3. 安装或升级

Linux：

    ./install.sh 192.0.2.11

Windows 命令提示符或 PowerShell：

    .\install.cmd 192.0.2.11

中控屏的 ADB 端口默认为 1379。已有且仅有一台 ADB 设备在线时，可以省略 IP。首次安装和后续升级使用同一条命令；升级会保留 Apple 家庭配对数据。成功时，安装程序会确认 HomeKit 服务已启动。

### 4. 首次添加到 Apple 家庭

安装后运行诊断命令，取得配对码：

Linux：

    ./install.sh 192.0.2.11 --doctor

Windows：

    .\install.cmd 192.0.2.11 -Doctor

健康检查全部通过后，命令会显示 HomeKit 配对码。在 iPhone 的“家庭”App 中选择“添加配件”，按提示输入配对码。升级已有安装时无需重新配对。

## 遇到问题时

安装和预检的完整输出会保存在发布包目录下的 logs 文件夹中：

- install-*.log：安装、ADB 传输、面板返回信息、失败回滚结果和退出码。
- preflight-*.log：安装前检查结果和退出码。

## 详细资料

- [部署、升级与故障诊断](DEPLOYMENT.md)
- [自动读取的数据与隐私说明](AUTO-ADOPTION.md)
