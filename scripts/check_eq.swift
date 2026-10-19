/* 三段 EQ 频响断言: 正弦稳态测幅, 跳过起始暂态 (只验 Eq3Band 语义)。
 *
 * 编译运行:
 *   xcrun swiftc -swift-version 5 scripts/check_eq.swift \
 *     Shared/EqFilter.swift -o /tmp/check_eq \
 *     && /tmp/check_eq
 */
import Foundation

var failures = 0
func expect(_ name: String, _ cond: Bool) {
    if cond { print("ok   \(name)") } else { failures += 1; print("FAIL \(name)") }
}

/// 正弦稳态峰值幅度 (跳过前 skip 样本的滤波暂态)。
func sineAmp(_ freq: Double, rate: Double, eq: Eq3Band, samples: Int = 48000) -> Double {
    var peak: Double = 0
    let skip = 8000
    for i in 0..<samples {
        let x = sin(2 * .pi * freq * Double(i) / rate)
        let y = Double(eq.process(Float(x)))
        if i >= skip { peak = max(peak, abs(y)) }
    }
    return peak
}

/// 某频点相对全 0 (透传) 的增益 dB。
func gainDb(_ freq: Double, rate: Double, low: Int, mid: Int, high: Int) -> Double {
    let neutral = sineAmp(freq, rate: rate, eq: Eq3Band(sampleRate: rate, low: 0, mid: 0, high: 0))
    let boosted = sineAmp(freq, rate: rate, eq: Eq3Band(sampleRate: rate, low: low, mid: mid, high: high))
    return 20 * log10(boosted / neutral)
}

func near(_ name: String, _ got: Double, _ target: Double, tol: Double = 1.5) {
    expect("\(name) (期望≈\(target)dB, 实测\(String(format: "%.1f", got))dB)",
           abs(got - target) <= tol)
}

@main
enum CheckEqMain {
    static func main() {
        let rate = 24000.0

        // 全 0 = 透传, 逐样本精确相等
        let passthrough = Eq3Band(sampleRate: rate, low: 0, mid: 0, high: 0)
        var exact = true
        for i in 0..<1000 {
            let x = Float(i % 97) / 97.0
            if passthrough.process(x) != x { exact = false; break }
        }
        expect("全 0 dB 逐样本透传", exact)
        expect("isNeutral 判定", Eq3Band.isNeutral(low: 0, mid: 0, high: 0)
               && !Eq3Band.isNeutral(low: 0, mid: 1, high: 0))

        // 各段只动自己频段, 远端 ~0 dB
        near("低架 +12dB @ 60Hz (架内)", gainDb(60, rate: rate, low: 12, mid: 0, high: 0), 12)
        near("低架 +12dB @ 8kHz (架外)", gainDb(8000, rate: rate, low: 12, mid: 0, high: 0), 0)
        near("峰值 +12dB @ 1.5kHz (中心)", gainDb(1500, rate: rate, low: 0, mid: 12, high: 0), 12, tol: 1.0)
        near("峰值 +12dB @ 60Hz (带外)", gainDb(60, rate: rate, low: 0, mid: 12, high: 0), 0)
        near("峰值 +12dB @ 8kHz (带外)", gainDb(8000, rate: rate, low: 0, mid: 12, high: 0), 0)
        near("高架 +12dB @ 8kHz (架内)", gainDb(8000, rate: rate, low: 0, mid: 0, high: 12), 12)
        near("高架 +12dB @ 60Hz (架外)", gainDb(60, rate: rate, low: 0, mid: 0, high: 12), 0)

        // 衰减与叠加
        near("低架 -12dB @ 60Hz", gainDb(60, rate: rate, low: -12, mid: 0, high: 0), -12)
        near("三段各 +6dB @ 1.5kHz (中段主导)", gainDb(1500, rate: rate, low: 6, mid: 6, high: 6), 6, tol: 2.0)
        near("三段各 +6dB @ 60Hz (低段主导)", gainDb(60, rate: rate, low: 6, mid: 6, high: 6), 6, tol: 2.0)
        near("三段各 +6dB @ 8kHz (高段主导)", gainDb(8000, rate: rate, low: 6, mid: 6, high: 6), 6, tol: 2.0)

        // 16k 采样率下同样成立 (永德引擎段经重采样后统一 24k, 此处仅验系数生成)
        near("低架 +12dB @ 60Hz (16k)", gainDb(60, rate: 16000, low: 12, mid: 0, high: 0), 12)

        print(failures == 0 ? "全部通过" : "\(failures) 项失败")
        exit(failures == 0 ? 0 : 1)
    }
}
