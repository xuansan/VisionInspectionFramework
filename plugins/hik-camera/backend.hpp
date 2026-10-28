#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <cstddef>
namespace vision::hik {
struct Config {std::string serial,format;unsigned width{},height{};float exposure{},gain{};};
struct Backend {
    virtual ~Backend()=default;
    virtual std::string enumerate()=0;
    virtual void open(const Config&)=0;
    virtual void trigger()=0;
    // false means no frame yet; output owns copied pixels, never an SDK pointer.
    virtual bool read(std::vector<std::byte>&)=0;
};
std::unique_ptr<Backend> make_backend();
}
