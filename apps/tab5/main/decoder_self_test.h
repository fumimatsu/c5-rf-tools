#pragma once
#include "c5_decoder.h"
#include <cassert>
inline void c5_decoder_self_test() {
    struct Seen { int count=0; } seen;
    auto callback=+[](uint8_t kind,const uint8_t* p,size_t n,void* ctx) {
        c5::Record record{};
        assert(kind=='R' && c5::read_record(p,n,record) && c5::is_raceband(record));
        for(unsigned i=0;i<8;i++) {
            const auto& r=record.channels[i];
            assert(r.raw==-123-int(i) && r.filtered==250+int(i));
            assert(r.source_us==0x12345678u+i && r.gain==30+i);
            assert(r.flags==(i==3?1:0));
        }
        ++static_cast<Seen*>(ctx)->count;
    };
    uint8_t frame[131]={0xa5,0x5a,'R',124,0}; frame[7]=8;
    for(unsigned i=0;i<8;i++) {
        uint8_t* q=frame+17+14*i;
        uint32_t time=0x12345678u+i; for(int j=0;j<4;j++) q[j]=time>>(8*j);
        uint16_t raw=uint16_t(-123-int(i)), filtered=250+i, mhz=c5::raceband[i];
        q[4]=raw; q[5]=raw>>8; q[6]=filtered; q[7]=filtered>>8;
        q[8]=mhz; q[9]=mhz>>8; q[10]=30+i; q[12]=(i==3?1:0);
    }
    auto checksum=c5::crc(frame+2,127); frame[129]=checksum; frame[130]=checksum>>8;
    static c5::Decoder decoder; decoder.reset();
    for(auto byte:frame) decoder.feed(&byte,1,callback,&seen);
    assert(seen.count==1);
    frame[23]^=1; decoder.feed(frame,sizeof(frame),callback,&seen);
    assert(seen.count==1 && decoder.crc_errors==1);
    frame[23]^=1;
    uint8_t bad_header[]={0xff,0xa5,0xa5,0x5a,'R',0xff,0xff};
    decoder.feed(bad_header,sizeof(bad_header),callback,&seen);
    decoder.feed(frame,sizeof(frame),callback,&seen);
    assert(seen.count==2 && decoder.bad_lengths==1);
    c5::Record record{};
    assert(!c5::read_record(frame+5,123,record));
    frame[7]=9; assert(!c5::read_record(frame+5,124,record));
    frame[7]=8; frame[25]=0;
    assert(c5::read_record(frame+5,124,record) && !c5::is_raceband(record));
    uint8_t single[26]{}; single[2]=1;
    assert(c5::read_record(single,sizeof(single),record) && !c5::is_raceband(record));
    // CRC failure must invalidate the window before a later valid frame in the
    // same USB batch can finish it. Continue parsing; do not discard that frame.
    struct Order { unsigned n=0; uint8_t event[4]{}; } order;
    auto accept=+[](uint8_t,const uint8_t*,size_t,void* ctx) {
        auto &o=*static_cast<Order*>(ctx); assert(o.n<4); o.event[o.n++]=1;
    };
    auto reject=+[](c5::FrameError e,void* ctx) {
        auto &o=*static_cast<Order*>(ctx); assert(o.n<4 && e==c5::FrameError::Crc); o.event[o.n++]=2;
    };
    frame[25]=uint8_t(c5::raceband[0]);
    checksum=c5::crc(frame+2,127); frame[129]=checksum; frame[130]=checksum>>8;
    uint8_t batch[393];
    std::memcpy(batch,frame,131); std::memcpy(batch+131,frame,131); std::memcpy(batch+262,frame,131);
    batch[131+23]^=1;
    decoder.reset(); decoder.feed(batch,sizeof(batch),accept,&order,reject);
    assert(order.n==3 && order.event[0]==1 && order.event[1]==2 && order.event[2]==1);
    c5::StreamTracker stream;
    record.seq=65535; record.dropped=0xfffffffeu;
    auto delta=stream.observe(record); assert(!delta.reset && !delta.gaps && !delta.drops);
    record.seq=0; record.dropped=1; delta=stream.observe(record);
    assert(!delta.reset && !delta.gaps && delta.drops==3);
    record.seq=3; record.dropped=3; delta=stream.observe(record);
    assert(!delta.reset && delta.gaps==2 && delta.drops==2);
    delta=stream.observe(record); assert(delta.duplicate && !delta.reset && !delta.gaps);
    record.seq=0; record.dropped=0; delta=stream.observe(record);
    assert(delta.reset && !delta.gaps && !delta.drops);
    stream.reset(); record.seq=30000;
    delta=stream.observe(record); assert(!delta.reset && !delta.gaps && !delta.drops);
}
