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

    s.bid[0] = p.BidPrice1; s.bid_vol[0] = p.BidVolume1;
    s.bid[1] = p.BidPrice2; s.bid_vol[1] = p.BidVolume2;
    s.bid[2] = p.BidPrice3; s.bid_vol[2] = p.BidVolume3;
    s.bid[3] = p.BidPrice4; s.bid_vol[3] = p.BidVolume4;
    s.bid[4] = p.BidPrice5; s.bid_vol[4] = p.BidVolume5;

    s.ask[0] = p.AskPrice1; s.ask_vol[0] = p.AskVolume1;
    s.ask[1] = p.AskPrice2; s.ask_vol[1] = p.AskVolume2;
    s.ask[2] = p.AskPrice3; s.ask_vol[2] = p.AskVolume3;
    s.ask[3] = p.AskPrice4; s.ask_vol[3] = p.AskVolume4;
    s.ask[4] = p.AskPrice5; s.ask_vol[4] = p.AskVolume5;

    s.update_ms = p.UpdateMillisec;
    memcpy(s.update_time, p.UpdateTime, 8);
    s.update_time[8] = '\0';
    s.inst_idx = inst_idx;

    slot.recv_tsc = recv_tsc;
    // release 保证上面所有写对策略线程可见
    slot.seq.fetch_add(1, std::memory_order_release);
}
