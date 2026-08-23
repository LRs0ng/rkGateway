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
