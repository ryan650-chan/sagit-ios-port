// SPDX-License-Identifier: GPL-2.0-only
// sagit QUP5 producer: no firmware download, panel reset or regulator writes.
#include <Uefi.h>
#include <Protocol/Cpu.h>
extern "C" {
#include "bridge.h"
void *memcpy(void *,const void *,size_t);
}
#include "../../drivers/clock/QcomGCC/GCCController.h"
#include "../../drivers/bus/QcomGPIO/TLMMPins.h"
#include "../../drivers/bus/QcomI2C/QUPI2C.h"
#include "../../drivers/input/SynapticsTouch/RMI4.h"
namespace {
uint32_t rd(uint64_t a) {
    uint32_t v=*reinterpret_cast<volatile uint32_t *>(a);
    __asm__ volatile("uxtw %x0,%w0\n\tdmb oshld":"+r"(v)::"memory");return v;
}
void wr(uint64_t a,uint32_t v) {
    __asm__ volatile("dmb oshst":::"memory");
    *reinterpret_cast<volatile uint32_t *>(a)=v;__asm__ volatile("dsb sy":::"memory");
}
uint64_t nowUS() { uint64_t t,h;__asm__ volatile("mrs %0,cntpct_el0":"=r"(t));__asm__ volatile("mrs %0,cntfrq_el0":"=r"(h));return (t/h)*1000000+(t%h)*1000000/h; }
void waitUS(unsigned us) {const uint64_t end=nowUS()+us;while(nowUS()<end)__asm__ volatile("yield");}
uint64_t pin(unsigned p) { return 0x03400000ULL+QcomTLMM::PinBase[p]; }
[[maybe_unused]] uint32_t gccRead(void *,uint32_t r) {return rd(0x00100000+r);}
[[maybe_unused]] void gccWrite(void *,uint32_t r,uint32_t v) {wr(0x00100000+r,v);}
[[maybe_unused]] void gccWait(void *,unsigned us) {waitUS(us);}
struct Registers {
    uint32_t lastState=0,lastStatus=0,lastOp=0,lastError=0;
    uint32_t read32(uint32_t r) {return rd(0x0c179000+r);}
    void write32(uint32_t r,uint32_t v) {
        if(r==QUPI2C::Reset) {
            lastState=read32(QUPI2C::State);lastStatus=read32(QUPI2C::Status);
            lastOp=read32(QUPI2C::Operational);lastError=read32(QUPI2C::Errors);
        }
        wr(0x0c179000+r,v);
    }
    void delayUS(unsigned us) {waitUS(us);}
    uint64_t nowUS() {return ::nowUS();}
};
void *busVersion=nullptr,*touchVersion=nullptr;
uint32_t busHz=0;
bool prepared=false;
struct Transport {
    QUPI2C::Engine<Registers> &engine;
    Registers &registers;
    bool transfer(I2C::Message *messages,unsigned count) {
        I2C::Transaction transaction{0x20,messages,count,20000};
        auto result=engine.transfer(transaction);
        if(result!=I2C::Result::Success) {
            bridge_puts("[Touch I2C] error=");bridge_hex(unsigned(result));
            bridge_puts(" status=");bridge_hex(registers.lastStatus);
            bridge_puts(" state=");bridge_hex(registers.lastState);bridge_puts(" op=");bridge_hex(registers.lastOp);
            bridge_puts(" core-error=");bridge_hex(registers.lastError);bridge_puts("\n");
            bridge_puts("[Touch I2C] first-byte=");bridge_hex(messages[0].bytes[0]);
            bridge_puts(" first-length=");bridge_hex(messages[0].length);bridge_puts(" messages=");bridge_hex(count);bridge_puts("\n");
        }
        return result==I2C::Result::Success;
    }
    bool select(uint16_t a) {uint8_t p[]={255,uint8_t(a>>8)};I2C::Message m{I2C::Direction::Write,p,2};return transfer(&m,1);}
    bool read(uint16_t a,uint8_t *p,size_t n) {
        if(!select(a))return false;
        uint8_t r=a;I2C::Message m[]={{I2C::Direction::Write,&r,1},{I2C::Direction::Read,p,n}};return transfer(m,2);
    }
    bool write(uint16_t a,const uint8_t *p,size_t n) {
        if(!n || n>254 || !select(a))return false;
        uint8_t b[255];b[0]=a;memcpy(b+1,p,n);I2C::Message m{I2C::Direction::Write,b,n+1};return transfer(&m,1);
    }
    void delayMS(unsigned ms) {waitUS(ms*1000);}
};
}
static void touch_settle_rails(void)
{
    // blsp_i2c5 function 3 on GPIO87/88, 2 mA, no internal pull: the board
    // carries the external pulls.
    bridge_puts("[Touch rails] I2C pin mux\n");
    const unsigned busPins[]={87,88};for(unsigned p:busPins)wr(pin(p),3U<<2);
    // Board fixed avdd enable. Never lower it and never touch the panel rails.
    bridge_puts("[Touch rails] sensor power\n");
    wr(pin(50)+4,rd(pin(50)+4)|2);wr(pin(50),0x200);
    // GPIO125 stays an input with pull-up; QcomGPIO owns its interrupt.
    wr(pin(125)+8,rd(pin(125)+8)&~1U);wr(pin(125),0x1c3);
    // The touch-only reset is already low, so this releases it rather than
    // pulsing it; the shared panel reset on GPIO8 is left alone.
    bridge_puts("[Touch rails] reset release\n");
    wr(pin(89)+4,rd(pin(89)+4)|2);wr(pin(89),0x3c3);
    waitUS(200000);
    bridge_puts("[Touch rails] settled\n");
}

extern "C" int bridge_touch_prepare(EFI_BOOT_SERVICES *bs,void *tree) {
    uint32_t bytes=0,h[4];
    busVersion=dt_property(tree,"i2c@c179000","qcom,i2c-handoff",&bytes);
    if(!busVersion || bytes!=sizeof(h))return -1;
    memcpy(h,busVersion,sizeof(h));
    if(h[0] || h[1]!=7 || h[2]!=19200000 || (h[3]!=100000 && h[3]!=400000))return -2;
    busHz=h[3];
    touchVersion=dt_property(tree,"touchscreen@20","synaptics,touch-handoff",&bytes);
    if(!touchVersion || bytes!=12)return -3;
    // This firmware does not instantiate a touch client. A running leaf means
    // another producer might own QUP5; refuse to reset an active controller.
    if((rd(0x00121008)&0x80000001U)!=0x80000000U)return -4;
    EFI_GUID guid=EFI_CPU_ARCH_PROTOCOL_GUID;EFI_CPU_ARCH_PROTOCOL *cpu=nullptr;
    if(EFI_ERROR(bs->LocateProtocol(&guid,nullptr,reinterpret_cast<void **>(&cpu))) || !cpu || !cpu->SetMemoryAttributes)return -5;
    if(EFI_ERROR(cpu->SetMemoryAttributes(cpu,0x0c179000,4096,EFI_MEMORY_UC|EFI_MEMORY_XP)))return -6;
    prepared=true;
    touch_settle_rails();
    // The kernel driver decides whether the sensor really answers, so the
    // handoff is published as enabled once the rails are up.
    uint32_t version=1;memcpy(touchVersion,&version,4);memcpy(busVersion,&version,4);
    bridge_puts("[Touch handoff] rails settled; bus, clocks and RMI stay with the kernel\n");
    return 0;
}
extern "C" int bridge_touch_finalize(void) {
    // Nothing is left for EFI: the clocks and the QUP engine belong to
    // QcomI2C, which starts from the kernel and can log its own failures.
    return 0;
}
