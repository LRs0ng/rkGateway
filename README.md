# rkGateway DS18B20

本项目包含：

- `drivers/ds18b20`：使用 Linux GPIO descriptor 子系统模拟 1-Wire 的 DS18B20 平台字符设备驱动；
- `plugins/ds18b20`：面向 `third_party/miniGateway` 的 Poll `IProtocolDriver` 动态插件；
- `configs/ds18b20_linux.json`：开发板上的 miniGateway 示例配置。

## 硬件与设备树

`custom_dt.dts` 已提供驱动匹配的节点：

```dts
 ds18b20 {
     compatible = "ds-temperature-collector";
     pinctrl-names = "default";
     pinctrl-0 = <&ds18b20_pin>;
     ds18b20-gpios = <&gpio4 RK_PC6 GPIO_ACTIVE_HIGH>;
     status = "okay";
 };
```

该驱动假设：

1. 总线上只有一个 DS18B20，因此使用 `SKIP ROM (0xcc)`；
2. DS18B20 使用外部供电，数据线上有符合 1-Wire 要求的外部上拉；
3. GPIO 释放总线时切换为输入，拉低总线时切换为输出低电平。

## 字符设备 ABI

驱动加载后创建 `/dev/ds18b20`。每次从偏移 0 读取至少 `sizeof(double)`（8）字节时，驱动会：

1. 发送 `CONVERT T`；
2. 等待最长 12-bit 转换时间 750 ms；
3. 读取 9 字节 scratchpad 并校验 CRC8；
4. 将当前温度以一个**二进制 IEEE-754 double** 返回。

这是二进制 ABI，不是 ASCII 文本。例如用户态需要使用：

```c
 double temperature;
 ssize_t n = read(fd, &temperature, sizeof(temperature));
```

内核代码没有使用浮点指令；它通过整数运算构造 IEEE-754 表示后复制到用户空间。

## 交叉编译

环境要求：

- `aarch64-linux-gnu-gcc/g++`；
- 已准备好的 `temp/linux-headers-6.1.99-rk356x`；
- 已初始化 `third_party/miniGateway/third_party/json` 子模块。

配置和构建：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DKERNEL_HEADERS=/path/to/linux-headers-6.1.99-rk356x \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++
cmake --build build --parallel
```

所有生成文件位于 `build/`，主要产物为：

- `build/kernel/ds18b20/ds18b20.ko`；
- `build/miniGateway/libgateway_ds18b20.so`；
- `build/miniGateway/miniGateway`；
- `build/miniGateway/config.json`。

构建系统将 Kbuild 的外部模块工作目录暂存到 `build/kernel/ds18b20/src`，不会在 `drivers/ds18b20` 中留下编译产物。

## miniGateway 配置

插件默认打开 `/dev/ds18b20`，也可以在驱动配置中指定路径：

```json
"driver_config": {
  "path": "/dev/ds18b20"
}
```

设备的采集点必须是 `double`；Poll 组中的每个点都会收到同一条 DS18B20 温度读数。驱动错误会映射为 `Disconnected`、`DecodeError` 或 `Bad` 质量状态，超时会映射为 `Timeout`。

本环境仅执行交叉编译和静态检查，没有加载内核模块、访问 GPIO 或运行 aarch64 程序。

## 开发板快速验证工具

新增工具源码：

```text
temp/tools/ds18b20_test.c
```

交叉编译后产物为：

```text
build/tools/ds18b20_test
```

将 `build/tools/ds18b20_test` 拷贝到开发板后，可以执行：

```bash
# 读取一次
./ds18b20_test

# 连续读取 10 次，每次间隔 1 秒
./ds18b20_test -n 10 -i 1000

# 持续读取，按 Ctrl+C 停止
./ds18b20_test -n 0 -i 1000

# 指定设备节点
./ds18b20_test -d /dev/ds18b20 -n 5 -i 500
```

工具会输出：

- 读取序号；
- 温度值；
- 原始 IEEE-754 `double` 位模式；
- `OK`、`WARNING_OUT_OF_DS18B20_RANGE` 或 `INVALID_NON_FINITE` 状态。

如果打开设备失败、驱动读取失败或返回长度不是 8 字节，工具会打印 `errno` 并返回非零退出码。

## IMX415 摄像头抓拍工具与 miniGateway Push 插件

本项目还包含一个不需要额外内核驱动的 V4L2 摄像头采集实现：

- `plugins/camera/camera_capture.c/.h`：V4L2 单平面/多平面 MMAP 采集、格式转换公共实现；
- `plugins/camera/camera.cpp/.hpp`：miniGateway `Push` 类型 camera source plugin；
- `tools/camera_test.c`：开发板抓拍验证工具；
- `configs/camera_periodic_linux.json`：周期采集示例；
- `configs/camera_control_linux.json`：控制采集示例。

IMX415 节点在 `temp/kernel-6.1/arch/arm64/boot/dts/rockchip/rk3568-lubancat-csi2-double.dtsi` 中默认是 `disabled`，本仓库的 `custom_dt.dts` 已通过：

```dts
&imx415 {
    status = "okay";
    ...
};
```

启用它。该 DTS 使用 `imx415 -> csi2_dphy1 -> rkisp_vir0` 的链路。实际的 `/dev/videoX` 编号由开发板内核注册顺序决定，不能仅凭 DTS 固定假定。启动前建议在板上执行：

```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/videoX --list-formats-ext
media-ctl -p
```

工具默认使用 `auto` 格式，按 MJPEG、NV12、YUYV、YUV420、RGB24 的顺序选择设备支持的格式。对于 RKISP 输出节点，NV12 或 YUYV 往往比 MJPEG 更常见；原始格式会被转换为无第三方依赖的 PPM (`P6`) 图片，MJPEG 帧则直接保存为 JPEG。工具支持 `V4L2_CAP_VIDEO_CAPTURE` 和 `V4L2_CAP_VIDEO_CAPTURE_MPLANE` 的 MMAP video capture 节点，并会根据设备能力自动选择单平面或多平面接口；不要直接选择只输出 RAW Bayer 的 CIF 节点作为 JPEG/PPM 图片源，应选择 ISP 输出节点。

交叉编译后，工具位于 `build/tools/camera_test`。默认图片会保存到**可执行文件所在目录**：

```bash
# 默认抓拍一张，默认设备 /dev/video0，自动选择格式
sudo ./camera_test

# 指定 V4L2 节点和 ISP 输出格式
sudo ./camera_test -d /dev/videoX -f nv12 -w 1920 -h 1080

# 强制 MJPEG，成功时输出 JPEG
sudo ./camera_test -d /dev/videoX -f mjpeg -o ./imx415.jpg

# 连续抓拍；会生成 capture-0001.ppm、capture-0002.ppm ...
sudo ./camera_test -d /dev/videoX -n 10 -i 1000
```

工具会先丢弃 `-u/--warmup` 指定数量的帧（默认 3 帧），以等待摄像头和 ISP 的初始状态稳定；单帧等待超时由 `-t/--timeout-ms` 控制。当前环境只进行了 AArch64 交叉编译和静态检查，没有访问真实摄像头。

#### 调亮参数

IMX415 没有通用的 `V4L2_CID_BRIGHTNESS` 传感器控件，画面偏暗时应优先调节曝光和模拟增益：

```bash
# 先查看板上的 sensor subdev 名称和控制范围
for s in /dev/v4l-subdev*; do
    echo "===== $s ====="
    cat "/sys/class/video4linux/$(basename "$s")/name"
done
v4l2-ctl -d /dev/v4l-subdevX --list-ctrls

# 使用自动查找 IMX415 subdev；适度增加模拟增益
sudo ./camera_test -d /dev/video0 -f nv12 \
    --control-device auto --analogue-gain 32

# 同时增加曝光（单位是曝光行数，不是毫秒）
sudo ./camera_test -d /dev/video0 -f nv12 \
    --control-device auto --exposure 1000 --analogue-gain 32

# 自动查找不到时，手工指定实际的 /dev/v4l-subdevX
sudo ./camera_test -d /dev/video0 -f nv12 \
    --control-device /dev/v4l-subdevX --exposure 1000 --analogue-gain 32
```

`analogue_gain` 的 IMX415 驱动范围通常为 `0..240`，`exposure` 的最大值会随当前分辨率、帧率和 VBLANK 动态变化，实际范围以 `v4l2-ctl --list-ctrls` 为准。`--brightness` 仍然保留为兼容 V4L2 输出节点的可选参数；如果节点不提供该控件，程序会报不支持，此时不代表摄像头故障。

RKISP 的 AE/3A 运行时可能再次覆盖手动曝光和增益，因此这些参数是启动时的手动设置，不一定能锁定整段运行期间的亮度。若仍然偏暗，应检查镜头/补光、曝光上限、ISP IQ/AE 配置，或关闭自动曝光后再使用手动值。

### camera Push plugin 配置

插件库为 `build/miniGateway/libgateway_camera.so`，设备配置的点必须使用 `bytes` 或 `byte_array` 类型，例如：

```json
{
  "id": "camera-1",
  "driver": "camera",
  "library": "./libgateway_camera.so",
  "driver_config": {
    "device": "/dev/videoX",
    "width": 1920,
    "height": 1080,
    "pixfmt": "auto",
    "mode": "periodic",
    "interval_ms": 10000,
    "warmup_frames": 3,
    "capture_timeout_ms": 3000,
    "control_command": "capture",
    "control_device": "auto",
    "exposure": 1000,
    "analogue_gain": 32
  },
  "connection": {},
  "points": [
    {"name": "image", "type": "bytes", "unit": "", "address": {},
     "scale": 1.0, "offset": 0.0}
  ]
}
```

`mode` 有两种值：

- `periodic`：driver 启动后立即抓拍一帧，之后每隔 `interval_ms` 抓拍并通过 `SampleSink` 推送；
- `control`：driver 启动并打开摄像头，但不主动抓拍；收到命令名为 `control_command`（默认 `capture`）的 `DeviceControlRequest` 后抓拍一次并推送一条 `RawBatch`。

图片内容以 `gateway::ByteArray` 放在 point 的 `RawSample.value` 中，而不是仅传送文件路径。MJPEG 点中的字节是 JPEG 文件内容；NV12/YUYV/YUV420/RGB24 点中的字节是 PPM 文件内容。事件发布器需要支持 `ByteArray` 才能把图片转发到外部系统。

processor plugin 发送拍照指令时使用 miniGateway 的处理上下文，例如：

```cpp
context.submit_control(gateway::DeviceControlRequest{
    .request_id = "capture-001",
    .device_id = "camera-1",
    .command = "capture",
    .arguments = {},
    .deadline = gateway::ControlClock::now() +
                std::chrono::seconds(5),
});
```

`configs/camera_control_linux.json` 使用第三方自带的 `periodic_control` source 作为测试控制入口；实际部署时可以将它替换为自己的 processor 或外部控制 source。该示例每 10 秒向 `camera-1` 发送一次 `capture`，并非 camera plugin 自己的周期模式。

选择摄像头配置构建时，例如：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DKERNEL_HEADERS="$PWD/temp/linux-headers-6.1.99-rk356x" \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
  -DGATEWAY_CONFIG="$PWD/configs/camera_periodic_linux.json"
cmake --build build --parallel
```

构建后主要摄像头产物为：

- `build/tools/camera_test`；
- `build/miniGateway/libgateway_camera.so`；
- `build/camera_periodic_config.json`；
- `build/camera_control_config.json`。
