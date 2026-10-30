// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
namespace QcomButton {
// Consumer Power, Volume Increment, Volume Decrement; three independent bits
// allow chords and simultaneous releases without synthesizing key repeats.
constexpr uint8_t ReportDescriptor[] = {
    0x05,0x0c, 0x09,0x01, 0xa1,0x01,
    0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x04,
    0x09,0x30, 0x09,0xe9, 0x09,0xea, 0x0a,0x23,0x02, 0x81,0x02,
    0x75,0x04, 0x95,0x01, 0x81,0x03, 0xc0
};
// AC Home sits in bit 3. No PMIC key feeds it: volume up plus volume down is
// reported as Home, because the body has no Home key and the lock screen waits
// for one.
constexpr uint8_t HomeBit = 1u << 3;
inline unsigned reportBit(unsigned usage) { return usage==0x30 ? 0 : usage==0xe9 ? 1 : 2; }
class State {
    struct Key { bool raw=false, stable=false; uint64_t since=0, debounce=15000000; unsigned bit=0; } keys[3];
    bool chord = false;
public:
    void configure(unsigned i,unsigned usage,unsigned milliseconds) {
        keys[i].bit=reportBit(usage); keys[i].debounce=uint64_t(milliseconds)*1000000;
    }
    uint8_t sample(uint32_t raw,uint64_t now,bool &pending) {
        pending=false; uint8_t report=0;
        for (unsigned i=0;i<3;++i) {
            auto &k=keys[i]; bool down=raw&(1U<<i);
            if (down!=k.raw || now<k.since) { k.raw=down; k.since=now; }
            if (k.raw!=k.stable) {
                if (now-k.since>=k.debounce) k.stable=k.raw;
                else pending=true;
            }
            if (k.stable) report|=1U<<k.bit;
        }
        // Volume up and volume down together become Home. The chord is latched
        // until both keys are released, otherwise letting go of one first would
        // report the other as a lone volume key press.
        const uint8_t power=report&1U;
        const bool anyVolume=(report&0x06)!=0;
        const bool bothVolume=(report&0x06)==0x06;
        if (bothVolume) chord=true;
        if (chord) {
            report=uint8_t(power|(bothVolume?HomeBit:0));
            if (!anyVolume) chord=false;
        }
        return report;
    }
    void reset() { for (auto &k:keys) { k.raw=k.stable=false; k.since=0; } chord=false; }
};
}
