# rkGateway
多协议接入框架源码：https://github.com/LRs0ng/miniGateway.git
## 1. 核心功能演示
### OTA升级
特点：
- 多次启动失败自动回退
- 断电防变砖
#### 正常OTA升级过程
[![点击观看演示视频](docs/covers/OTA-cover.png)](https://github.com/user-attachments/assets/bfbf4a4b-a0ff-4474-a4d5-6a27be2e48a2)
#### 在镜像写入过程中断电（防变砖）
[![点击观看演示视频](docs/covers/OTA-cover2.png)](https://github.com/user-attachments/assets/3c2c0bfe-f0c9-4015-bd36-164b259025a2)
#### 多次启动失败回退原系统
[![点击观看演示视频](docs/covers/OTA-cover3.png)](https://github.com/user-attachments/assets/189d7e62-c5de-434b-aa2d-a0a6c1bb6d5b)
### 多协议接入
特点：
- 任何符合接口定义的插件都能接入系统
- 可扩展性强
- 轻量
#### 温度采集+mqtt上报
[![点击观看演示视频](docs/covers/framework-cover1.png)](https://github.com/user-attachments/assets/b71753f6-4bb0-465f-9368-12bd3c498753)
#### 温度采集+ADC电压采集+mqtt上报
[![点击观看演示视频](docs/covers/framework-cover2.png)](https://github.com/user-attachments/assets/da838a71-377c-43ea-8b61-3e69e228e0d0)