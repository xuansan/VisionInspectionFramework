#include <vision/modbus/client.hpp>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdexcept>
#include <array>
#include <chrono>
namespace vision::modbus {
namespace {
void put(std::vector<std::uint8_t>& b,std::uint16_t n){b.push_back(static_cast<std::uint8_t>(n>>8));b.push_back(static_cast<std::uint8_t>(n));}
std::uint16_t get(std::span<const std::uint8_t> b,std::size_t i){return static_cast<std::uint16_t>((b[i]<<8)|b[i+1]);}
}
struct Client::Impl {
    SOCKET socket=INVALID_SOCKET;std::uint16_t transaction{};std::uint8_t unit;unsigned timeout;
    bool initialized{};
    ~Impl(){if(socket!=INVALID_SOCKET)closesocket(socket);if(initialized)WSACleanup();}
    void wait(bool writing,std::chrono::steady_clock::time_point deadline) {
        const auto remaining=std::chrono::duration_cast<std::chrono::microseconds>(deadline-std::chrono::steady_clock::now()).count();
        if(remaining<=0)throw std::runtime_error("Modbus deadline");
        fd_set set;FD_ZERO(&set);FD_SET(socket,&set);timeval t{static_cast<long>(remaining/1000000),static_cast<long>(remaining%1000000)};
        if(select(0,writing?nullptr:&set,writing?&set:nullptr,nullptr,&t)!=1)throw std::runtime_error("Modbus timeout");
    }
    std::vector<std::uint8_t> exchange(std::vector<std::uint8_t> pdu) {
        if(transaction==UINT16_MAX)throw std::runtime_error("Modbus transaction exhausted");
        const auto id=++transaction;std::vector<std::uint8_t> request;put(request,id);put(request,0);
        put(request,static_cast<std::uint16_t>(pdu.size()+1));request.push_back(unit);request.insert(request.end(),pdu.begin(),pdu.end());
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);
        auto transfer=[&](char* bytes,std::size_t size,bool sending) {
            while(size){wait(sending,deadline);const int count=sending?send(socket,bytes,static_cast<int>(size),0):recv(socket,bytes,static_cast<int>(size),0);
                if(count==SOCKET_ERROR&&WSAGetLastError()==WSAEWOULDBLOCK)continue;
                if(count<=0)throw std::runtime_error("Modbus disconnected");bytes+=count;size-=static_cast<std::size_t>(count);}
        };
        transfer(reinterpret_cast<char*>(request.data()),request.size(),true);
        std::array<std::uint8_t,7> header{};transfer(reinterpret_cast<char*>(header.data()),7,false);
        const auto length=get(header,4);
        if(get(header,0)!=id||get(header,2)!=0||header[6]!=unit||length<2||length>254)throw std::runtime_error("Modbus MBAP");
        std::vector<std::uint8_t> response(length-1);transfer(reinterpret_cast<char*>(response.data()),response.size(),false);
        if(response[0]!=pdu[0])throw std::runtime_error("Modbus function/exception");
        return response;
    }
};
Client::Client(std::string ip,std::uint16_t port,std::uint8_t unit,unsigned timeout):impl_(std::make_unique<Impl>()) {
    if(!port||!timeout||timeout>1000)throw std::invalid_argument("Modbus endpoint");
    WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))throw std::runtime_error("Winsock");impl_->initialized=true;impl_->unit=unit;impl_->timeout=timeout;
    impl_->socket=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(impl_->socket==INVALID_SOCKET)throw std::runtime_error("Socket");
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port);
    if(InetPtonA(AF_INET,ip.c_str(),&address.sin_addr)!=1)throw std::invalid_argument("Numeric IPv4 required");
    u_long mode=1;if(ioctlsocket(impl_->socket,FIONBIO,&mode))throw std::runtime_error("Socket mode");
    if(connect(impl_->socket,reinterpret_cast<sockaddr*>(&address),sizeof(address))==SOCKET_ERROR) {
        if(WSAGetLastError()!=WSAEWOULDBLOCK)throw std::runtime_error("Connect");
        impl_->wait(true,std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout));
        int error=0,len=sizeof(error);if(getsockopt(impl_->socket,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&len)||error)throw std::runtime_error("Connect");
    }
}
Client::~Client()=default;
void Client::write(std::uint16_t address,std::span<const std::uint16_t> values) {
    if(values.empty()||values.size()>123||values.size()>65536U-address)throw std::invalid_argument("Register range");
    std::vector<std::uint8_t> pdu{16};put(pdu,address);put(pdu,static_cast<std::uint16_t>(values.size()));pdu.push_back(static_cast<std::uint8_t>(values.size()*2));
    for(auto value:values)put(pdu,value);
    const auto response=impl_->exchange(pdu);
    if(response.size()!=5||get(response,1)!=address||get(response,3)!=values.size())throw std::runtime_error("Write acknowledgment");
}
std::vector<std::uint16_t> Client::read(std::uint16_t address,std::uint16_t count) {
    if(!count||count>125||count>65536U-address)throw std::invalid_argument("Register range");
    std::vector<std::uint8_t> pdu{3};put(pdu,address);put(pdu,count);const auto response=impl_->exchange(pdu);
    if(response.size()!=2+count*2U||response[1]!=count*2U)throw std::runtime_error("Read count");
    std::vector<std::uint16_t> values;for(std::size_t i=2;i<response.size();i+=2)values.push_back(get(response,i));return values;
}
}
