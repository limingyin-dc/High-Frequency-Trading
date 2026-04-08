#pragma once
#include "ThostFtdcMdApi.h"
#include "future_struct.h"
#include <atomic>
#include <array>
#include <cstring>

// 最大订阅合约数，与 MdEngine::MAX_INST 保持一致
constexpr int TICK_POOL_MAX_INST = 16;

// 每个合约独占一个 TickSlot，固定位置存储
// 行情线程按 inst_idx 直接覆盖写，策略线程按 inst_idx 直接读
// 无环形索引，无队列，O(1) 定位
struct alignas(64) TickSlot {
    SlimTick tick;
    uint64_t recv_tsc{0};
    // 每个槽位独立的写入版本号，策略侧用于检测该合约是否有新 tick
    alignas(64) std::atomic<uint64_t> seq{0};
};

class TickPool {
public:
    TickPool() = default;

    // 启动前预热，触发物理页分配，避免开盘 Page Fault
    void WarmUp();

    // 行情线程：按合约下标写入固定槽位
    // inst_idx 由 MdEngine::FindInstIdx() 提供，范围 [0, TICK_POOL_MAX_INST)
    void Write(const CThostFtdcDepthMarketDataField& p, uint64_t recv_tsc, int8_t inst_idx);

    // 策略线程：按合约下标读取最新 tick 槽位
    const TickSlot& SlotByInst(int8_t inst_idx) const {
        return m_slots[inst_idx];
    }

    // 策略线程：读取某合约当前写入序号，用于检测是否有新 tick
    uint64_t SeqByInst(int8_t inst_idx) const {
        return m_slots[inst_idx].seq.load(std::memory_order_acquire);
    }

private:
    std::array<TickSlot, TICK_POOL_MAX_INST> m_slots{};
};

inline TickPool g_tick_pool;
