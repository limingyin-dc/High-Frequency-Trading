#include "Strategy.h"
#include "Logger.h"
#include "Tsc.h"       // TSC 时间戳工具
#include <thread>
#include <pthread.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdint>

// 辅助函数：将内部整数价格对齐到最小变动单位的倍数
// 解决 "价格非最小单位的倍数" (如 .14) 的拒单问题
inline int64_t AlignPrice(int64_t price_int, int64_t tick_size) {
    if (tick_size <= 0) return price_int;
    return (price_int / tick_size) * tick_size;
}

Strategy::Strategy(TdEngine& td, int spread_ticks, int max_net_pos,
                   const std::vector<std::string>& instruments)
    : m_td(td), m_spread_ticks(spread_ticks), m_max_net_pos(max_net_pos) {
    
    m_inst_count = std::min((int)instruments.size(), MAX_INST);
    for (int i = 0; i < m_inst_count; ++i) {
        strncpy(m_inst_names[i], instruments[i].c_str(), 31);
        m_inst_names[i][31] = '\0';
        m_last_seq[i] = 0;
    }
    LOG_INFO("[Strategy] 策略初始化完成，合约数: %d, Spread: %d, MaxPos: %d", 
             m_inst_count, m_spread_ticks, m_max_net_pos);
}

void Strategy::CancelIfActive(int slot_idx) {
    if (slot_idx < 0) return;
    auto& slot = m_td.m_oms.GetSlot(slot_idx);
    int st = slot.state.load(std::memory_order_acquire);
    // 只有在挂单状态或部分成交状态才执行撤单
    if (st == OrderState::Pending || st == OrderState::PartialFilled) {
        m_td.CancelOrder(slot);
    }
}

void Strategy::OnTick(int idx, const SlimTick& tick, uint64_t t1_tsc) {
    MmState& ms = m_state[idx];
    const char* inst = tick.instrument;

    // 1. 基础风控：涨跌停不报价，避免单边无法成交风险
    if (tick.last_price >= tick.upper_limit || tick.last_price <= tick.lower_limit)
        return;

    // 2. 报价基准计算：以买一卖一中点为基准
    double  mid_price = (tick.bid + tick.ask) * 0.5;
    int64_t mid_int   = PriceUtil::ToInt(mid_price);

    // 频率控制：中间价变动不足一个最小单位时不动作，减少不必要的撤单次数
    if (mid_int == ms.last_mid) return;
    ms.last_mid = mid_int;

    // 3. 极速撤单：撤掉该合约之前的双边挂单（OMS 槽位 O(1) 访问）
    CancelIfActive(ms.bid_slot);
    CancelIfActive(ms.ask_slot);
    ms.bid_slot = -1;
    ms.ask_slot = -1;

    // 4. 获取当前仓位：决定是开仓还是平仓
    int net = m_td.GetNetLongByIdx(idx);

    // 5. 计算报价偏移 (Inventory Skewing)
    // 根据库存方向微调 Spread，引导仓位回归 0
    int bid_offset = m_spread_ticks;
    int ask_offset = m_spread_ticks;

    if (net > 0)      ask_offset = std::max(1, ask_offset - 1); // 多头多，卖单向中点靠拢，诱导平多
    else if (net < 0) bid_offset = std::max(1, bid_offset - 1); // 空头多，买单向中点靠拢，诱导平空

    // 股指期货 TICK_SIZE 对应放大后的 200 (假设精度为 1000)
    // 请务必在此根据实际合约修改对应值
    constexpr int64_t TICK_SIZE = 2000; 

    int64_t bid_int = PriceUtil::AddTick(mid_int, -bid_offset, TICK_SIZE);
    int64_t ask_int = PriceUtil::AddTick(mid_int,  ask_offset, TICK_SIZE);

    // 强制价格对齐，解决 CFFEX: 价格非最小单位倍数 错误
    bid_int = AlignPrice(bid_int, TICK_SIZE);
    ask_int = AlignPrice(ask_int, TICK_SIZE);

    // 保护：防止因极端波动或算法计算导致的自成交
    if (bid_int >= ask_int) return;

    // 6. 发送 BID（买入）逻辑
    if (net < m_max_net_pos) {
        // 如果手里有空头，买入即为平仓
        char offset = (net < 0) ? THOST_FTDC_OF_CloseToday : THOST_FTDC_OF_Open;
        double bid_px = PriceUtil::ToDouble(bid_int);
        
        std::string ref = m_td.SendOrder(inst, bid_px, THOST_FTDC_D_Buy, offset, 1, true);
        if (!ref.empty()) {
            ms.bid_slot = m_td.m_oms.DecodeIndexPublic(ref.c_str());
            // 记录打点延迟
            LOG_DEBUG("[Strategy] 发单成功: %s BID %.2f Offset=%c Net=%d Ref=%s Lat=%ld ns",
                      inst, bid_px, offset, net, ref.c_str(),
                      Tsc::ToNs(Tsc::Now() - t1_tsc));
            m_lat_tick2order.Add(Tsc::ToNs(Tsc::Now() - t1_tsc));
        }
    }

    // 7. 发送 ASK（卖出）逻辑
    if (net > -m_max_net_pos) {
        // 如果手里有多头，卖出即为平仓
        char offset = (net > 0) ? THOST_FTDC_OF_CloseToday : THOST_FTDC_OF_Open;
        double ask_px = PriceUtil::ToDouble(ask_int);

        std::string ref = m_td.SendOrder(inst, ask_px, THOST_FTDC_D_Sell, offset, 1, true);
        if (!ref.empty()) {
            ms.ask_slot = m_td.m_oms.DecodeIndexPublic(ref.c_str());
            m_lat_tick2order.Add(Tsc::ToNs(Tsc::Now() - t1_tsc));
        }
    }
}

void Strategy::Start(int cpu_core) {
    std::thread t([this, cpu_core]() {
        // 绑核逻辑，减少上下文切换延迟
        if (cpu_core >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cpu_core, &cpuset);
            pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
            LOG_INFO("[Strategy] Thread pinned to core %d", cpu_core);
        }
        Run();
    });
    t.detach();
}

void Strategy::Run() {
    LOG_INFO("[Strategy] 策略引擎启动...");
    while (true) {
        bool any_new = false;

        for (int idx = 0; idx < m_inst_count; ++idx) {
            // 通过无锁队列/序号轮询最新行情
            uint64_t cur_seq = g_tick_pool.SeqByInst((int8_t)idx);
            if (cur_seq == m_last_seq[idx]) continue;

            m_last_seq[idx] = cur_seq;
            any_new = true;

            // 只有 TdEngine 登录成功并初始化后才开始策略
            if (UNLIKELY(!m_td.isReady)) continue;

            const TickSlot& slot = g_tick_pool.SlotByInst((int8_t)idx);
            const SlimTick& tick = slot.tick;
            
            // 脏数据检查
            if (UNLIKELY(tick.bid <= 0.1 || tick.ask <= 0.1)) continue;

            OnTick(idx, tick, slot.recv_tsc);
        }

        // 如果没有新行情，进入轻量级忙等，减少 CPU 功耗但保持唤醒速度
        if (!any_new) {
            __asm__ __volatile__("pause");
        }
    }
}