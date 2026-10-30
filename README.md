# 天选5 Pro 风扇调控（fancontrol）

ASUS TUF Gaming A16 FA607PV 在 Arch Linux 下的风扇曲线调控工具。
C++ 本地后端 + 浏览器前端，。

## 构建与运行

```bash
g++ -O2 -std=c++17 -pthread -o fan_server fan_server.cpp
./fan_server
```

然后浏览器访问 <http://127.0.0.1:8931>。

无需 root，无第三方依赖（仅需 g++ 与 asusctl）。

## 打包

`packaging/build-all.sh` 一键产出三种包（输出到 `packaging/out/`）：

```bash
cd packaging && ./build-all.sh
```

| 格式 | 工具 | 产物 |
|---|---|---|
| Arch | makepkg | `fancontrol-1.0.0-1-x86_64.pkg.tar.zst` |
| Debian | dpkg-deb | `fancontrol_1.0.0_amd64.deb` |
| Fedora/RHEL | rpmbuild | `fancontrol-1.0.0-1.x86_64.rpm`（需先 `sudo pacman -S rpm-tools`） |

安装后可用用户级服务常驻：`systemctl --user enable --now fancontrol`。


## 架构

```text
浏览器页面 → fan_server（127.0.0.1:8931）→ asusctl → asusd → 内核／固件
                    │
                    └→ 直接读 sysfs hwmon（RPM／温度／曲线，只读）
```

- 按 `name` 动态查找 hwmon 设备（`asus` / `asus_custom_fan_curve` / `k10temp` / `amdgpu`），不硬编码编号。
- 曲线写入通过 fork/exec 调用 `asusctl`，不经 shell，参数全部校验。
- 仅监听回环地址，外部机器无法访问。

## HTTP 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/status` | RPM、CPU/GPU 温度、双通道 8 点曲线、当前配置档 |
| POST | `/api/curve` | 写曲线：`{"fan":"cpu","profile":"Balanced","points":[[温度℃,PWM%],...共8点]}` |
| POST | `/api/enable` | 启用/停用曲线：`{"enabled":true,"profile":"Balanced"}` |
| POST | `/api/default` | 恢复 BIOS 默认：`{"profile":"Balanced"}` |

后端校验：恰好 8 个点；温度 0–100℃ 单调不减；PWM 0–100% 单调不减；
配置档仅限 `Balanced` / `Quiet` / `Performance`。

## 注意

- `asus_armoury` 驱动的曲线温度实际为**整摄氏度**而非毫摄氏度，读取代码已做自适应（>1000 视为毫摄氏度）。
- 不提供固定 RPM/PWM 功能（固件无此接口）。
- 修改曲线后注意高负载温度，异常时用"恢复默认"按钮或：
  `asusctl fan-curve --mod-profile Balanced --default`
