#pragma once
#include <snap7.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace vision::s7 {
// SDK ownership stays on the plugin thread. Cancellation never destroys a handle concurrently.
class Client {
public:
    Client(const std::string& address, std::uint16_t port, unsigned rack, unsigned slot, unsigned db):db_(db) {
        handle_=Cli_Create();
        try {
            if(!handle_)throw std::runtime_error("Snap7 create");
            int timeout=50;
            check(Cli_SetParam(handle_,p_u16_RemotePort,&port));
            check(Cli_SetParam(handle_,p_i32_PingTimeout,&timeout));
            check(Cli_SetParam(handle_,p_i32_SendTimeout,&timeout));
            check(Cli_SetParam(handle_,p_i32_RecvTimeout,&timeout));
            check(Cli_ConnectTo(handle_,address.c_str(),rack,slot));
        } catch(...) {Cli_Destroy(&handle_);throw;}
    }
    ~Client(){if(handle_){Cli_Disconnect(handle_);Cli_Destroy(&handle_);}}
    Client(const Client&)=delete;
    Client& operator=(const Client&)=delete;
    void write(std::uint16_t offset,const std::vector<std::uint16_t>& words) {
        std::vector<std::uint8_t> data;
        for(auto word:words){data.push_back(static_cast<std::uint8_t>(word>>8));data.push_back(static_cast<std::uint8_t>(word));}
        check(Cli_DBWrite(handle_,db_,offset,static_cast<int>(data.size()),data.data()));
    }
    std::vector<std::uint16_t> read(std::uint16_t offset,std::uint16_t count) {
        std::vector<std::uint8_t> data(count*2);
        check(Cli_DBRead(handle_,db_,offset,static_cast<int>(data.size()),data.data()));
        std::vector<std::uint16_t> words;
        for(std::size_t i=0;i<data.size();i+=2)words.push_back(static_cast<std::uint16_t>((data[i]<<8)|data[i+1]));
        return words;
    }
private:
    static void check(int result){if(result)throw std::runtime_error("Snap7 error "+std::to_string(result));}
    S7Object handle_{};
    unsigned db_;
};
}
