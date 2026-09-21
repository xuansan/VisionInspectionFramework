#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
namespace vision::modbus {
class Client {
public:
    Client(std::string ipv4,std::uint16_t port,std::uint8_t unit,unsigned timeout_ms=50);
    ~Client();
    void write(std::uint16_t address,std::span<const std::uint16_t>);
    std::vector<std::uint16_t> read(std::uint16_t address,std::uint16_t count);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
