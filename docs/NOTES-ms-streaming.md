# 微软嵌入式引擎流式架构排障记 (2026-10-17)

从"整段合成后回读"改为流式的全部实测结论。实验工具: `scripts/ms_stream_probe.c`
(mode 1/2/3/4/5)、`scripts/test_cli.c`。

## 可用的 C API 形态 (与公开 C SDK 的差异)

- `synthesizer_start_speaking_ssml(h, ssml, len, &result)` — 4 参 (多一个长度),
  立即返回, 合成后台进行;
- `synth_result_get_audio_data(res, buf, cap, &copied)` — 4 参, 不支持 NULL 探尺寸;
- completed/canceled 回调 ABI: `cb(arg1=synth句柄, arg2=事件句柄)` —
  **set_callback 的 ctx 不回传**, 按 arg1 查进程内注册表路由;
- 音频只走 AudioConfig 目的地: 空 AudioConfig 下 Synthesizing 事件全部 0 字节
  (result 直取同理) → 流式 = **wav 文件跟随** (SDK 渐进写, 首块 ~0.73s,
  ~1.6× 实时; 文件头 44 字节为零, 实为 raw PCM)。

## 状态机红线 (违反即 0 字节空完成 / ORT 段错误)

1. **同一 synth 上并发两次合成** → ONNX Runtime 推理线程段错误
   (ValidateInputOutputCounts)。两引擎 dylib 各自 dlopen, 但链接同一个
   libonnxruntime → 全局连坐 (一个引擎打坏, 另一个也僵)。
2. **推理进行中调 stop_speaking** → 同上 (ORT 僵死)。
3. 读尽上一句后**紧接** start_speaking → 0 字节空完成 (completed 立即到,
   无音频)。**句间 stop_speaking + ~150ms 复位拍后 start 正常**
   (probe M4/M5 与 CLI 交叉验证; M4 无 stop 也成功 — SDK 行为不完全确定,
   适配器统一采用 stop+150ms 的保守路径)。
4. 陈旧 completed 回调会落到"当前 active 流"上 → reader 在 off==0 时
   **不做**完成判定 (门限: 读到过数据才允许 stable-判定)。

## 并发结构 (ms_embedded_adapter.c)

- 每发音人一会话 (懒建): cfg + set_voice(license) + wav acfg + synth + 回调;
- 流对象: 跟随 fd + off + completed/canceled + 稳定判定; 20s 无进展兜底;
- **墓园**: stream_free 不立即销毁 (在途回调可能仍持指针) — 入 vs->grave,
  下次 stream_start 持锁统一回收;
- 发音人枚举与引擎初始化分离 (mtts_global_scan 毫秒级 / init 秒级后台):
  系统查询 speechVoices 时 AU init 必须秒回, 否则注册超时失败。

## 预热之死

后台空跑一句以消掉首推理 ~0.8s 的尝试全部失败: 预热与真实请求的 launch 竞争
落在红线 1/3 上 (即使加 got_real 让位/在途 stop/墓园回收均无法同时满足全部
时序)。**会话跨句复用 ⇒ 首句冷、之后即热**, 预热收益仅每天第一句, 弃。
ms_warm_thread 保留在代码中备查。

## 发音人物料现状

本地可用 HD 包仅 1 个 (晓晓 HD): naturalvoice 与 onecore 各有一份拷贝。
云希 Natural 包只有 SynthModel.key (被拒, reason=1)。更多发音人需用
fetch-voice-models.sh 从广荣服务器下载 HD 包 (需要账号)。
