// SPDX-License-Identifier: BSD-2-Clause
// MSM8998 warm UFS handoff. No EFI storage callback is used after ExitBootServices.
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include "bridge.h"
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
struct clock_protocol;
typedef struct clock_protocol Clock;
struct clock_protocol {
    UINT64 version;
    EFI_STATUS (EFIAPI *get_id)(Clock *, const CHAR8 *, UINTN *);
    EFI_STATUS (EFIAPI *enable)(Clock *, UINTN);
    void *disable, *is_enabled;
    EFI_STATUS (EFIAPI *is_on)(Clock *, UINTN, BOOLEAN *);
    void *set_frequency;
    EFI_STATUS (EFIAPI *get_frequency)(Clock *, UINTN, UINT32 *);
};
struct transfer {
    uint32_t header, reserved, ocs, reserved2;
    uint64_t command;
    uint16_t response_length, response_offset, prd_length, prd_offset;
};
_Static_assert(sizeof(struct transfer)==32,"UFSHCI transfer ABI");
static unsigned char *area;
static unsigned lun;
static uint64_t apfs_lba;
static void *tree;
static uint32_t gear[6];
static const uint32_t gates[]={0x75008,0x7500c,0x75010,0x75014,0x76008,0x76040,0x82028,0x88004};
static uint32_t rd(uint64_t p) {
    uint32_t v=*(volatile uint32_t *)(uintptr_t)p;
    __asm__ volatile("dmb oshld" ::: "memory");return v;
}
static void wr(uint64_t p,uint32_t v) {
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)(uintptr_t)p=v;
    __asm__ volatile("dsb sy" ::: "memory");
}
static void delay(unsigned us) {
    uint64_t start,now,hz;
    __asm__ volatile("mrs %0,cntpct_el0":"=r"(start));
    __asm__ volatile("mrs %0,cntfrq_el0":"=r"(hz));
    do { __asm__ volatile("mrs %0,cntpct_el0":"=r"(now)); } while(now-start < (hz*us+999999)/1000000);
}
static int wait_reg(unsigned off,uint32_t mask,uint32_t expected,unsigned us) {
    for(unsigned n=0;n<=us/10;n++) { if((rd(0x1da4000+off)&mask)==expected)return 0;delay(10); }return -1;
}
static int uic(unsigned op,uint32_t a,uint32_t value,uint32_t *out) {
    if(wait_reg(0x30,8,8,500000))return -1;
    delay(1000);wr(0x1da4020,0x400);wr(0x1da4094,a);wr(0x1da4098,0);wr(0x1da409c,value);wr(0x1da4090,op);
    if(wait_reg(0x20,0x400,0x400,500000))return -1;
    unsigned result=rd(0x1da4098)&255;
    if(out)*out=rd(0x1da409c);
    wr(0x1da4020,0x400);return result?-1:0;
}
static void cache(void *data,size_t size,int invalidate) {
    uint64_t ctr;__asm__ volatile("mrs %0,ctr_el0":"=r"(ctr));
    uint64_t line=4ULL<<((ctr>>16)&15),end=(uintptr_t)data+size;
    for(uint64_t p=(uintptr_t)data&~(line-1);p<end;p+=line) {
        if(invalidate)__asm__ volatile("dc civac,%0"::"r"(p):"memory");
        else __asm__ volatile("dc cvac,%0"::"r"(p):"memory");
    }
    __asm__ volatile("dsb sy":::"memory");
}
static int put32(const char *property,uint32_t value) {
    uint32_t len;unsigned char *p=dt_property(tree,"storage@1da4000",property,&len);
    if(!p||len!=4)return -1;
    for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(value>>(i*8));return 0;
}
static void be32(unsigned char *p,uint32_t v) {for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(v>>(24-8*i));}
static int resources(void) {
    // Retain the already-trained firmware link and its rails/NoC resources.
    // No regulator, RPM, interconnect or clock provider is allowed to power it off.
    if(rd(0x17501c)!=0x105 || rd(0x17602c)!=0x107 || !(rd(0x175004)&0x80000000) || rd(0x175000)&1)return -1;
    for(unsigned i=0;i<sizeof(gates)/sizeof(gates[0]);i++)if(!(rd(0x100000+gates[i])&1))return -1;
    if(!(rd(0x1da40dc)&(1u<<26)) || !(rd(0x1da7d60)&1))return -1;
    return 0;
}
int bridge_ufs_prepare(EFI_BOOT_SERVICES *bs,void *apple_tree) {
    tree=apple_tree;
    uint32_t len;void *v=dt_property(tree,"storage@1da4000","qcom,ufs-handoff-version",&len);
    if(!v || len!=4)return -1;
    if(rd(0x1da40e4)!=0x30010000 || resources())return -2;
    EFI_GUID cg={0x241afae6,0x885f,0x4f6c,{0xa7,0xea,0xc2,0x8e,0xab,0x79,0xc3,0xe5}};
    Clock *clock=0;
    if(EFI_ERROR(bs->LocateProtocol(&cg,0,(void **)&clock)) || !clock || clock->version<0x10005)return -3;
    const char *names[]={"gcc_ufs_axi_clk","gcc_aggre1_ufs_axi_clk","gcc_ufs_ahb_clk","gcc_ufs_unipro_core_clk",
        "gcc_ufs_phy_aux_clk","gcc_ufs_clkref_clk","gcc_ufs_tx_symbol_0_clk","gcc_ufs_rx_symbol_0_clk"};
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) {
        UINTN id;UINT32 hz=0;BOOLEAN on=0;
        if(EFI_ERROR(clock->get_id(clock,(const CHAR8 *)names[i],&id)) || EFI_ERROR(clock->enable(clock,id)) ||
           EFI_ERROR(clock->is_on(clock,id,&on)) || !on)return -4;
        if(i==0 || i==3) {
            if(EFI_ERROR(clock->get_frequency(clock,id,&hz)) || hz!=(i?150000000u:200000000u))return -5;
        }
    }
    EFI_PHYSICAL_ADDRESS memory=0;
    if(EFI_ERROR(bs->AllocatePages(AllocateAnyPages,EfiLoaderData,8,&memory)))return -6;
    area=(void *)(uintptr_t)memory;memset(area,0,32768);
    EFI_GUID bg=EFI_BLOCK_IO_PROTOCOL_GUID;EFI_HANDLE *handles=0;UINTN count=0;
    if(EFI_ERROR(bs->LocateHandleBuffer(ByProtocol,&bg,0,&count,&handles)))return -7;
    int found=0;
    for(UINTN i=0;i<count;i++) {
        EFI_BLOCK_IO_PROTOCOL *b=0;
        if(EFI_ERROR(bs->HandleProtocol(handles[i],&bg,(void **)&b)) || !b || !b->Media || b->Media->LogicalPartition)continue;
        if(b->Media->BlockSize!=4096)continue;
        if(EFI_ERROR(b->FlushBlocks(b)) || EFI_ERROR(b->ReadBlocks(b,b->Media->MediaId,1,4096,area+16384)))continue;
        // Generic disk identity: a valid protective GPT header. This port
        // targets any sagit GPT rather than one specific factory disk GUID;
        // the takeover stage re-verifies the APFS NXSB at the chosen LBA.
        if(area[16384+0]!='E' || area[16384+1]!='F' || area[16384+2]!='I' || area[16384+3]!=' ' ||
           area[16384+4]!='P' || area[16384+5]!='A' || area[16384+6]!='R' || area[16384+7]!='T')continue;
        uint64_t table_lba;memcpy(&table_lba,area+16384+72,8);
        if(table_lba!=2 || EFI_ERROR(b->ReadBlocks(b,b->Media->MediaId,table_lba,4096,area+20480)))continue;
        // The APFS container was flashed to the userdata partition; find it
        // by its GPT entry name, then relabel the entry as Apple APFS so the
        // prelinked apfs personality (Content Hint IOPropertyMatch) probes it.
        static const char wanted[]={'u','s','e','r','d','a','t','a'};
        // Apple APFS partition type GUID in EFI mixed-endian (on-disk) form:
        // uuid.UUID('7C3457EF-0000-11AA-AA11-00306543ECAC').bytes_le.
        // Bytes 6/7 are the little-endian Data3 field 0x11AA -> AA 11.
        static const unsigned char apfs_type[]={0xEF,0x57,0x34,0x7C,0x00,0x00,0xAA,0x11,0xAA,0x11,0x00,0x30,0x65,0x43,0xEC,0xAC};
        unsigned entry_count=0, entry_size=0;
        memcpy(&entry_count,area+16384+80,4);
        memcpy(&entry_size,area+16384+84,4);
        if(!entry_count || !entry_size || entry_count*entry_size>16384)continue;
        unsigned array_blocks=(entry_count*entry_size+4095)/4096;
        if(EFI_ERROR(b->ReadBlocks(b,b->Media->MediaId,table_lba,array_blocks*4096,area+0)))continue;
        int matched=-1;
        for(unsigned entry=0;entry<entry_count;entry++) {
            unsigned char *p=area+entry*entry_size;
            int is_name=1;
            for(unsigned j=0;j<sizeof(wanted);j++)
                if(p[56+j*2]!=(unsigned char)wanted[j] || p[57+j*2]!=0){is_name=0;break;}
            if(!is_name)continue;
            uint64_t first_lba;memcpy(&first_lba,p+32,8);
            if(!first_lba)continue;
            memcpy(&apfs_lba,p+32,8);
            matched=(int)entry;
            break;
        }
        if(matched<0)continue;
        unsigned char *p=area+(unsigned)matched*entry_size;
        int type_equal=1;
        for(unsigned j=0;j<16;j++)if(p[j]!=apfs_type[j]){type_equal=0;break;}
        if(!type_equal) {
            for(unsigned j=0;j<16;j++)p[j]=apfs_type[j];
            unsigned array_bytes=entry_count*entry_size;
            uint32_t array_crc=0,header_crc=0,header_size=0;
            int wrote=0;
            memcpy(&header_size,area+16384+12,4);
            if(!EFI_ERROR(bs->CalculateCrc32(area+0,array_bytes,&array_crc))) {
                memcpy(area+16384+88,&array_crc,4);
                memset(area+16384+16,0,4);
                if(!EFI_ERROR(bs->CalculateCrc32(area+16384,header_size,&header_crc))) {
                    memcpy(area+16384+16,&header_crc,4);
                    if(!EFI_ERROR(b->WriteBlocks(b,b->Media->MediaId,table_lba,array_blocks*4096,area+0)) &&
                       !EFI_ERROR(b->WriteBlocks(b,b->Media->MediaId,1,4096,area+16384)))
                        wrote=1;
                }
            }
            bridge_puts(wrote ? "[UFS handoff] userdata GPT entry relabeled Apple APFS\n"
                              : "[UFS handoff] GPT relabel write failed; apfs match may still fail\n");
            if(wrote) {
            // The backup GPT at the end of the disk must carry the same entry
            // and valid CRCs, otherwise a primary/backup consistency check
            // rejects the table and no partition media is published.
            // GPT header layout: +32 AlternateLBA is the backup header LBA.
            // The backup entry array LBA is authoritative only in the backup
            // header itself (+72); the primary header's +72 is its own array.
            uint64_t alternate_lba=0;
            memcpy(&alternate_lba,area+16384+32,8);
            if(alternate_lba>1 &&
               !EFI_ERROR(b->ReadBlocks(b,b->Media->MediaId,alternate_lba,4096,area+20480))) {
                unsigned char *bh=area+20480;
                int sig_ok = bh[0]=='E'&&bh[1]=='F'&&bh[2]=='I'&&bh[3]==' '&&
                             bh[4]=='P'&&bh[5]=='A'&&bh[6]=='R'&&bh[7]=='T';
                uint64_t bh_entries_lba=0;
                memcpy(&bh_entries_lba,bh+72,8);
                uint32_t bh_size=0;
                memcpy(&bh_size,bh+12,4);
                if(sig_ok && bh_entries_lba>1 && bh_size>=92 && bh_size<=4096 &&
                   bh_entries_lba+array_blocks<=alternate_lba) {
                    // Write the same relabeled entry array into the backup array.
                    int backup_wrote=0;
                    if(!EFI_ERROR(b->WriteBlocks(b,b->Media->MediaId,bh_entries_lba,array_blocks*4096,area+0))) {
                        // Refresh the backup header's array CRC and header CRC.
                        uint32_t bh_header_crc=0;
                        memcpy(bh+88,&array_crc,4);
                        memset(bh+16,0,4);
                        if(!EFI_ERROR(bs->CalculateCrc32(bh,bh_size,&bh_header_crc))) {
                            memcpy(bh+16,&bh_header_crc,4);
                            if(!EFI_ERROR(b->WriteBlocks(b,b->Media->MediaId,alternate_lba,4096,bh)))
                                backup_wrote=1;
                        }
                    }
                    bridge_puts(backup_wrote
                        ? "[UFS handoff] backup GPT synchronized to Apple APFS\n"
                        : "[UFS handoff] backup GPT update failed\n");
                } else {
                    bridge_puts("[UFS handoff] backup GPT header not usable; primary only\n");
                }
            } else {
                bridge_puts("[UFS handoff] backup GPT location invalid; primary only\n");
            }
            // Read back the primary entry and prove the type GUID on disk.
            unsigned char verify[16];
            if(!EFI_ERROR(b->ReadBlocks(b,b->Media->MediaId,table_lba+((unsigned)matched*entry_size)/4096,4096,area+24576))) {
                unsigned base=((unsigned)matched*entry_size)%4096;
                memcpy(verify,area+24576+base,16);
                int ok=1;
                for(unsigned j=0;j<16;j++)if(verify[j]!=apfs_type[j]){ok=0;break;}
                bridge_puts(ok ? "[UFS handoff] primary GPT readback: Content Hint 7C3457EF-0000-11AA-AA11-00306543ECAC\n"
                                : "[UFS handoff] primary GPT readback mismatch\n");
            } else {
                bridge_puts("[UFS handoff] primary GPT readback failed\n");
            }
            }
        }
        if(!apfs_lba)continue;
        // UFSDxe leaves its completed UTRD in memory. Recover the actual LU number.
        uint64_t address=rd(0x1da4050)|((uint64_t)rd(0x1da4054)<<32);
        if(address<0x80000000 || address>0x1ffffffffULL)continue;
        struct transfer *t=(void *)(uintptr_t)address;cache(t,1024,1);
        for(unsigned slot=0;slot<32;slot++) {
            uint64_t cmd=t[slot].command;
            if(cmd<0x80000000 || cmd>0x1ffffffffULL)continue;
            unsigned char *p=(void *)(uintptr_t)cmd;cache(p,512,1);
            if(p[0]!=1 || p[2]>7 || (p[16]!=0x28 && p[16]!=0x88))continue;
            lun=p[2];found=1;break;
        }
        break;
    }
    bs->FreePool(handles);
    if(!found)return -8;
    static const uint16_t attrs[]={0x1583,0x1568,0x1580,0x1560,0x156a,0x1571};
    for(unsigned i=0;i<6;i++)if(uic(1,(uint32_t)attrs[i]<<16,0,&gear[i]))return -9;
    if(!gear[0] || gear[0]>3 || !gear[1] || gear[1]>3 || gear[2]!=1 || gear[3]!=1 || gear[4]!=2)return -10;
    if(gear[5]!=0x11 && gear[5]!=0x44)return -10;
    bridge_puts("[UFS handoff] prepared warm link, HCI=200MHz UniPro=150MHz LU=");bridge_hex(lun);
    bridge_puts(" RX gear=");bridge_hex(gear[0]);bridge_puts(" TX gear=");bridge_hex(gear[1]);bridge_puts(" power mode=");bridge_hex(gear[5]);bridge_puts("\n");
    return 0;
}
static int physical_read_once(uint64_t lba) {
    struct transfer *t=(void *)area;unsigned char *cmd=area+4096,*data=area+12288;
    memset(area,0,1024);memset(cmd,0,8192);memset(data,0,4096);
    t->header=(1u<<28)|(1u<<24)|(2u<<25);t->ocs=15;t->command=(uintptr_t)cmd;
    t->response_length=128;t->response_offset=128;t->prd_length=1;t->prd_offset=256;
    if(lba>0xffffffffULL)return -4;
    cmd[0]=1;cmd[1]=0x40;cmd[2]=(unsigned char)lun;be32(cmd+12,4096);cmd[16]=0x28;
    be32(cmd+18,(uint32_t)lba);cmd[24]=1;
    uint64_t p=(uintptr_t)data;memcpy(cmd+1024,&p,8);uint32_t bytes=4095;memcpy(cmd+1036,&bytes,4);
    cache(area,16384,1);wr(0x1da4020,~0u);wr(0x1da4058,1);
    if(wait_reg(0x58,1,0,1000000))return -1;
    cache(area,16384,1);
    unsigned sense_format=cmd[546]&0x7f;
    unsigned sense_key=(sense_format==0x70 || sense_format==0x71)?cmd[548]&15:cmd[547]&15;
    unsigned sense_asc=(sense_format==0x70 || sense_format==0x71)?cmd[558]:cmd[548];
    if(!t->ocs && cmd[512]==0x21 && cmd[519]==2 && sense_key==6 && sense_format>=0x70 && sense_format<=0x73) {
        bridge_puts("[UFS handoff] acknowledged reset unit attention ASC=");bridge_hex(sense_asc);bridge_puts("\n");return 1;
    }
    if(t->ocs || cmd[512]!=0x21 || cmd[514]!=(unsigned char)lun || cmd[518] || cmd[519]) {
        bridge_puts("[UFS handoff] read response OCS=");bridge_hex(t->ocs);bridge_puts(" UPIU=");bridge_hex(cmd[512]);bridge_puts(" status=");bridge_hex(cmd[519]);bridge_puts(" sense format/key/ASC=");bridge_hex(sense_format);bridge_hex(sense_key);bridge_hex(sense_asc);bridge_puts("\n");return -2;
    }
    return 0;
}
static int physical_read(uint64_t lba) {
    // Unit attention after HCE reset is expected. Retry only a completed READ,
    // never a transport failure or any write command.
    for(unsigned retry=0;retry<3;retry++) {int result=physical_read_once(lba);if(result!=1)return result;}
    return -3;
}
static int control(unsigned transaction,unsigned query_opcode,uint32_t *value) {
    struct transfer *t=(void *)area;unsigned char *cmd=area+4096;
    memset(area,0,1024);memset(cmd,0,8192);
    t->header=(1u<<28)|(1u<<24);t->ocs=15;t->command=(uintptr_t)cmd;
    t->response_length=128;t->response_offset=128;
    cmd[0]=(unsigned char)transaction;
    if(transaction==0x16) {cmd[5]=query_opcode==6?0x81:1;cmd[12]=(unsigned char)query_opcode;cmd[13]=1;}
    cache(area,12288,1);wr(0x1da4020,~0u);wr(0x1da4058,1);
    if(wait_reg(0x58,1,0,1000000))return -1;
    cache(area,12288,1);
    if(t->ocs || cmd[512]!=(transaction?0x36:0x20) || cmd[518] || cmd[519])return -2;
    if(value)*value=((uint32_t)cmd[532]<<24)|((uint32_t)cmd[533]<<16)|((uint32_t)cmd[534]<<8)|cmd[535];
    return 0;
}
int bridge_ufs_takeover(void) {
    // All firmware exit callbacks have completed. Mask interrupts and fence DMA
    // before replacing both descriptor lists; never reuse firmware DMA memory.
    if(!area || resources())return -1;
    uint32_t ctlr=rd(0x17a00000);
    bridge_puts("[UFS handoff] GICD_CTLR before=");bridge_hex(ctlr);bridge_puts("\n");
    // Enable the NS bank's affinity-routed Group1A forwarding. EnableGroup1's
    // other alias can be RAZ/WI, so require only the architected 0x12 interface.
    // Change ARE_NS with NS forwarding off; preserve DS and secure ownership.
    if((ctlr&0x12)!=0x12) {
        wr(0x17a00000,ctlr&~3u);
        for(unsigned i=0;i<10000 && (rd(0x17a00000)&0x80000000);i++)delay(10);
        if(rd(0x17a00000)&0x80000000)return -27;
        wr(0x17a00000,(ctlr&~3u)|0x10u);
        for(unsigned i=0;i<10000 && (rd(0x17a00000)&0x80000000);i++)delay(10);
        if(rd(0x17a00000)&0x80000000)return -28;
        wr(0x17a00000,ctlr|0x12u);
    }
    ctlr=rd(0x17a00000);
    bridge_puts("[UFS handoff] GICD_CTLR after=");bridge_hex(ctlr);bridge_puts("\n");
    if(put32("qcom,ufs-polled-completion",(ctlr&0x12)==0x12?0:1))return -26;
    wr(0x1da4024,0);wr(0x1da4060,0);wr(0x1da4080,0);wr(0x1da4034,0);
    if(wait_reg(0x34,1,0,100000))return -2;
    wr(0x1da4034,1);
    if(wait_reg(0x34,1,1,100000) || wait_reg(0x30,8,8,100000))return -3;
    if(!(rd(0x1da4030)&1) && uic(0x16,0,0,0))return -4;
    uint64_t pa=(uintptr_t)area;
    wr(0x1da4050,(uint32_t)pa);wr(0x1da4054,(uint32_t)(pa>>32));
    wr(0x1da4070,(uint32_t)(pa+2048));wr(0x1da4074,(uint32_t)((pa+2048)>>32));
    if(wait_reg(0x30,0xe,0xe,100000))return -5;
    wr(0x1da4060,1);
    if(control(0,0,0) || control(0x16,6,0))return -6;
    uint32_t initializing=1;
    for(unsigned i=0;i<100 && initializing;i++) {
        if(control(0x16,5,&initializing))return -7;
        if(initializing)delay(10000);
    }
    if(initializing)return -8;
    int result=physical_read(1);if(result)return -10+result;
    for(unsigned j=0;j<4096;j++)if(area[12288+j]!=area[16384+j])return -13;
    result=physical_read(apfs_lba);if(result)return -20+result;
    if(rd((uintptr_t)area+12288+32)!=0x4253584e)return -23;
    // XNU gets a stopped controller; it will allocate its own DMA lists.
    wr(0x1da4060,0);wr(0x1da4034,0);if(wait_reg(0x34,1,0,100000))return -24;
    if(put32("qcom,ufs-hs-gear",gear[0]<gear[1]?gear[0]:gear[1]) ||
       put32("qcom,ufs-hs-power-mode",gear[5]) ||
       put32("qcom,ufs-handoff-version",2) || put32("qcom,ufs-handoff-flags",31))return -25;
    bridge_puts("[UFS handoff] post-EFI physical DMA GPT + APFS NXSB verified; firmware ownership released\n");
    return 0;
}
