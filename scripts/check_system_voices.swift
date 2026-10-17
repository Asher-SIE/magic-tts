#!/usr/bin/env swift
// 系统语音注册状态一页看 (2026-10-17 事故复盘产物, 用法见 docs/NOTES-voice-catalog.md):
//   swift scripts/check_system_voices.swift
// 输出: ① 本扩展在语音目录的注册条目 ② AU 组件注册状态 ③ appex 探针摘要。
import AVFoundation
import Foundation

let appexId = "com.magictts.voices.synth"

// ① 语音目录
let mine = AVSpeechSynthesisVoice.speechVoices().filter { $0.identifier.contains("com.magictts") }
print("== 语音目录: \(mine.count) 条 ==")
for v in mine { print("  \(v.identifier) | \(v.name) | \(v.language)") }
if mine.isEmpty { print("  (无 — 若 AU 组件在, 见 docs/NOTES-voice-catalog.md 症状一)") }

// ② AU 组件 (ausp = speech synthesizer 扩展点)
let desc = AudioComponentDescription(
    componentType: OSType(0x61757370), componentSubType: 0,
    componentManufacturer: 0, componentFlags: 0, componentFlagsMask: 0)
let comps = AVAudioUnitComponentManager.shared().components(matching: desc)
    .filter { $0.name.contains("Magic") || $0.manufacturerName.contains("Magic") }
print("== AU 组件: \(comps.count) 个 ==")
for c in comps {
    let d = c.audioComponentDescription
    print("  \(c.name) | manuf=\(c.manufacturerName ?? "?") | subtype=\(String(format: "%C", d.componentSubType)) | \(c.componentURL?.path ?? "-")")
}
if comps.isEmpty { print("  (无 — 重装 app 触发目录重扫: rm -rf + cp -R + open)") }

// ③ 探针 (宿主进程无沙盒, 按真实家目录拼容器绝对路径)
let debug = FileManager.default.homeDirectoryForCurrentUser
    .appendingPathComponent("Library/Containers/\(appexId)/Data/Engines/.debug")
print("== 探针 \(debug.path) ==")
for name in ["entry_trace.txt", "voices_queried.txt"] {
    let p = debug.appendingPathComponent(name)
    if let s = try? String(contentsOf: p, encoding: .utf8) {
        let lines = s.split(separator: "\n")
        print("  \(name): \(lines.count) 行, 最近: \(lines.last ?? "")")
    } else {
        print("  \(name): 不存在")
    }
}
let wavs = (try? FileManager.default.contentsOfDirectory(
    at: debug, includingPropertiesForKeys: [.contentModificationDateKey]))?
    .filter { $0.lastPathComponent.hasPrefix("last_") }
    .sorted { (try? $0.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
        > (try? $1.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast } ?? []
print("  last_*.wav: \(wavs.count) 个\(wavs.first.map { ", 最近 \(($0.lastPathComponent as NSString).lastPathComponent)" } ?? "")")
