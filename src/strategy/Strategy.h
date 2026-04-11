#pragma once
#include "TdEngine.h"
#include "TickPool.h"
#include "Tsc.h"
#include "Logger.h"
#include <cstdint>
#include <array>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>

#ifndef LIKELY
  #define LIKELY(x)   __builtin_expect(!!(x), 1)
  #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif


namespace PriceUtil {
    // 放大倍数，假设我们把价格放大 10000 倍处理（精度到 0.0001）
    constexpr double SCALE = 10000.0;

    // double 转 整数（带四舍五入，防止精度丢失）
    inline int64_t ToInt(double price) {
        return static_cast<int64_t>(std::round(price * SCALE));
    }

    // 整数转 double
    inline double ToDouble(int64_t price_int) {
        return static_cast<double>(price_int) / SCALE;
    }

    // 增加 Tick 的逻辑
    // mid_int: 当前价格整数, num_ticks: 偏移跳数, tick_size_int: 0.2 对应的整数值
    inline int64_t AddTick(int64_t mid_int, int num_ticks, int64_t tick_size_int) {
        return mid_int + (static_cast<int64_t>(num_ticks) * tick_size_int);
    }
}

// 固定窗口分位数统计器
template<int N = 1000>
class LatencyStats {
public:
    const char* name;
    explicit LatencyStats(const char* n) : name(n) {}
    void Add(int64_t ns) {
        m_buf[m_count++] = ns;
        if (m_count == N) Flush();
    }
private:
    std::array<int64_t, N> m_buf{};
    int m_count = 0;
    void Flush() {
        std::sort(m_buf.begin(), m_buf.end());
        LOG_INFO("[Latency][%s] n=%d  p50=%lldns  p99=%lldns  p999=%lldns",
                 name, N,
                 (long long)m_buf[N * 50  / 100],
                 (long long)m_buf[N * 99  / 100],
                 (long long)m_buf[N * 999 / 1000]);
        m_count = 0;
    }
};

// ============================================================
// 做市策略
//
// 每个合约维护一对挂单（bid + ask），挂在买一/卖一各偏移
// spread_ticks 档的位置。
//
// 每次收到新 tick：
//   1. 若盘口价格变动，撤掉旧的双边挂单
//   2. 检查净持仓是否允许继续双边报价
//   3. 重新挂 bid / ask
//
// 风控：
//   - 净多头 >= max_net_pos 时不挂 bid（不再买）
//   - 净多头 <= -max_net_pos 时不挂 ask（不再卖）
//   - 涨跌停时不报价
// ============================================================
class Strategy {
public:
    Strategy(TdEngine& td, int spread_ticks, int max_net_pos,
             const std::vector<std::string>& instruments);
    void Start(int cpu_core = -1);

private:
    static constexpr int MAX_INST = 16;

    TdEngine& m_td;
    int       m_spread_ticks; // 单边偏移档数，总价差 = spread_ticks * 2 * tick_size
    int       m_max_net_pos;  // 单合约最大净持仓（多/空方向各自上限）

    int  m_inst_count = 0;
    char m_inst_names[MAX_INST][32]{};

    // 每个合约的做市状态
    struct MmState {
        // 当前挂单的 OMS 槽位下标，-1 表示无挂单
        int bid_slot = -1;
        int ask_slot = -1;
        // 上次报价的中间价（整数），用于判断是否需要撤单重报
        int64_t last_mid = 0;
    };
    std::array<MmState, MAX_INST> m_state{};
    // 每个合约上次处理的 seq，用于检测新 tick
    std::array<uint64_t, MAX_INST> m_last_seq{};

    // 撤掉某合约的 bid 或 ask 挂单（通过槽位下标，O(1)）
    void CancelIfActive(int slot_idx);

    // 核心做市逻辑，每个新 tick 调用一次
    void OnTick(int idx, const SlimTick& tick, uint64_t t1_tsc);

    void Run();

    LatencyStats<1000> m_lat_tick2order{"tick→报单(T1→T3)"};
};
