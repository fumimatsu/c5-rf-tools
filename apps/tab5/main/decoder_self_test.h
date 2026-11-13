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
}