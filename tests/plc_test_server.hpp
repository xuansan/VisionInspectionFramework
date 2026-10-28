#pragma once
#include <array>
#include <atomic>
#include <cstring>
#include <thread>
#include <string>
#ifdef TEST_S7
#include <snap7.h>
#endif
struct Server {
    std::string fault;
    std::array<unsigned char,48> command{};
    std::atomic<unsigned> writes{};
    void reply(void* buffer) {
        auto* out=static_cast<unsigned char*>(buffer);
        std::memset(out,0,50);std::memcpy(out,command.data(),24);
        out[25]=7;out[33]=1;std::memcpy(out+34,command.data()+26,16);
        if(fault=="old_session")out[0]^=1;
        if(fault=="wrong_sequence")out[23]^=1;
        if(fault=="old_ack"&&command[43])out[34]^=1;
        if(fault=="unsafe_ready")out[25]=1;
    }
};
#ifdef TEST_S7
int S7API exchange(void* context,int,int operation,PS7Tag tag,void* data){
    auto& s=*static_cast<Server*>(context);
    if(s.fault=="timeout")std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if(s.fault=="reject")return 1;
    if(tag->DBNumber!=1)return 1;
    if(operation==OperationWrite&&tag->Start==100&&tag->Size==48){
        std::memcpy(s.command.data(),data,48);++s.writes;return 0;
    }
    if(operation==OperationRead&&tag->Start==200&&tag->Size==50){s.reply(data);return 0;}
    return 1;
}
#endif
