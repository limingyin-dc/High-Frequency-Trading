#include "TickPool.h"

void TickPool::WarmUp() {
    for (int i = 0; i < TICK_POOL_MAX_INST; ++i) {
        volatile uint64_t t = m_slots[i].recv_tsc;
        (void)t;
    }
}

void TickPool::Write(const CThostFtdcDepthMarketDataField& p, uint64_t recv_tsc, int8_t inst_idx) {
    if (UNLIKELY(inst_idx < 0 || inst_idx >= TICK_POOL_MAX_INST)) return;

    TickSlot& slot = m_slots[inst_idx];
    SlimTick& s    = slot.tick;

    memcpy(s.instrument, p.InstrumentID, 31);
    s.instrument[31] = '\0';
    s.last_price  = p.LastPrice;
    s.upper_limit = p.UpperLimitPrice;
    s.lower_limit = p.LowerLimitPrice;

    s.bid = p.BidPrice1; 
    s.bid_vol = p.BidVolume1;

    s.ask = p.AskPrice1;
    s.ask_vol = p.AskVolume1;
   
    s.update_ms = p.UpdateMillisec;
    memcpy(s.update_time, p.UpdateTime, 8);
    s.update_time[8] = '\0';
    s.inst_idx = inst_idx;

    slot.recv_tsc = recv_tsc;
    // release 保证上面所有写对策略线程可见
    slot.seq.fetch_add(1, std::memory_order_release);
}
