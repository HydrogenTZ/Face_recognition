# ESP32-P4 人脸录入、识别与测速

当前工程位置：`E:\B-Poject\ESP\Eye\face_recognition`。原生 Eye 对照工程位于 `E:\B-Poject\ESP\Eye\12_esp32-p4-eye`。VS Code 请直接打开人脸识别工程文件夹，以使用本目录下的任务。

移动后，芯片版本配置文件已收进本工程的 `config/`，clangd 使用 `${workspaceFolder}/build`。旧位置生成的构建目录已归档到 `backups/build_before_relocation`，新的 `build/` 在当前位置重新生成。历史实验对应的旧构建/烧录日志也保留在该归档中，移动验证日志为 `build/relocation_build.log`。

本工程在原有相机示例上接入官方 MFN_S8_V1 人脸特征提取模型。使用现有 MSR/MNP 检测框与五个关键点做对齐，提取 512 维归一化特征，以点积（余弦相似度）查询本地特征库。

## 操作

启动进入 AI 页面，默认 Face 模式。屏幕底部按钮：

- Enroll：等待一张清晰人脸，在 15 秒内录入一个新 ID。
- Bench 200：预热 20 次，收集 200 次有效单人脸识别。
- Stats：将统计值输出到串口。

识别功能在 Face 模式生效；实时行人检测仍可通过原界面切换。特征与检测使用同一张 240×240 RGB565 快照，AI 任务独占快照直至识别完成；忙时跳过新帧。检测框为最近一次已完成结果的显示叠加，不能用于表示严格同步的屏幕显示延迟。

特征保存在 face_store 独立 NVS 分区（0xA10000，256 KiB），最多 32 条，每个 ID 当前对应一条特征，重启保留。原有设置分区与 SD 卡不变。模型版本标识用于拒绝误读其他格式数据。

## 串口命令（115200，按回车发送）

```text
help
enroll alice
enroll bob
list
delete 1
clear YES
threshold 0.50
rotate 270
bench 200
stats
dump
stop
facepage
```

姓名使用最多 23 个 ASCII 字符、不含空格。Enroll 每次新增 ID，不会覆盖已有人员。clear YES 显式清空人员库。初始匹配阈值 0.50 仅作为实验起点，需用独立验证样本调整。阈值本次启动有效。多人脸、无脸、过小人脸不会执行录入。

根据连接开发板上取回的实际图像，AI 输入默认顺时针旋转 270°（即逆时针 90°）以转正人脸，检测框与关键点再反向映射到屏幕。若改变开发板/摄像头的放置方向，可用 rotate 0、90、180、270 调整；该设置在本次启动有效。snapshot 可导出一帧十六进制诊断图，tools/snapshot_to_png.py 可将其恢复为 PNG；不要在正式测速时抓取图像。

## 测速口径

- detect_us：MSR/MNP 检测调用，包括检测预处理、推理、后处理。
- feature_us：人脸对齐、识别模型预处理、MFN 推理、归一化，共同计时。
- match_us：遍历所有已录入 512 维特征并完成阈值判定。
- algorithm_us：检测开始到身份判定完成，包含上述阶段及中间开销。
- pipeline_us：240×240 输入快照生成前到身份判定完成，包括快照缩放、复制及调度等待；不包含此前传感器曝光、采集、PPA 显示图缩放和之后的屏幕刷新。
- 输入方向旋转位于检测之前，计入 pipeline_us；feature_us 包含关键点人脸对齐。检测组件自身逐脸打印的内部日志已关闭，避免影响测速。
- fps：有效测量样本数 / 第一条测量样本提交到最后一条结果完成的时间；异步流水线吞吐率。

每次 bench 先预热 20 次有效单人脸处理；随后记录指定条数，最多 1000。无脸、多脸、特征异常/脸太小单独计数，Unknown 样本计入耗时。180 秒未完成时保留已采集样本并输出超时。此统计没有真实身份标签，不能自动计算准确率。需另行记录已知/未知人员与实际输出。

平均值、中位数、P95 均为板上实测。串口与 UI 统计输出在计时区间外。测试期间不要改动数据库和阈值。dump 在 benchmark 运行时发送会延后到结束再导出，避免大批量串口输出干扰测量。

## 构建与烧录

沿用本机 VS Code 配置的 ESP-IDF v6.1、ESP-DL 3.1.3。官方 human_face_recognition 0.2.0 被作为本地组件固定；仅适配了其 DataBase 构造函数与依赖描述。

```powershell
./tools/idf.ps1 build
./tools/idf.ps1 -IdfArgs @('-p','COM3','flash')
./tools/idf.ps1 -IdfArgs @('-p','COM3','monitor')
```

VS Code 的 Terminal / Run Task 中也提供 Face: build、Face: flash COM3、Face: monitor COM3。串口监视器只能由一个进程占用，运行采集脚本前先关闭其他监视器。

当前保持原工程 Debug 编译优化作为功能基线。正式比较性能时固定并记录 CPU、PSRAM 频率、编译优化、依赖/模型版本、阈值、特征库规模、光照、距离和图像尺寸；不要直接将官方其他配置的模型耗时当成本板成绩。

## 保存原始数据

```powershell
& C:/Espressif/tools/python/v6.1/venv/Scripts/python.exe tools/face_serial.py --seconds 120 --command 'bench 200' --command dump --output results/run01.log
& C:/Espressif/tools/python/v6.1/venv/Scripts/python.exe tools/analyze_faces.py results/run01.log --out results/run01
```

得到 samples.csv、summary.json。串口日志另保留 FACE_STATS、FACE_BENCH_BEGIN 信息。每种条件建议重复 3 轮。检测无脸/多人/无效人脸计数覆盖整次运行，包括预热期间；Unknown 计数对应正式记录样本。

## 备份

修改前源码、配置和可烧录固件位于 backups/before_face_recognition。原固件恢复后，新分区中的人员特征仍保留，但原固件不会访问它。恢复时使用备份的引导、分区表和应用配套文件。

自动化脚本 tools/integrate_face.py 是首次接入时使用的一次性脚本，不要重复运行。常规构建仅使用 tools/idf.ps1。
