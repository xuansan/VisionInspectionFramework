#pragma once
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <string>
#include <string_view>

namespace vision::ipc {
class FrameError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
// Single event-loop owner. A failed decoder stays failed until the connection is replaced.
class Decoder {
public:
    explicit Decoder(std::size_t max_payload=1048576, std::size_t max_frames=32)
        : max_(max_payload), slots_(max_frames) {
        if(!max_ || max_>1048576 || !slots_) throw std::invalid_argument("Invalid frame limits");
    }
    void feed(std::string_view bytes) {
        if(failed_) throw FrameError("Connection decoder already failed");
        try {
            for(unsigned char byte:bytes) {
                if(header_bytes_<4) {
                    length_=(length_<<8)|byte;
                    if(++header_bytes_==4 && (!length_ || length_>max_))
                        throw FrameError("Invalid frame length");
                } else {
                    payload_.push_back(static_cast<char>(byte));
                    if(payload_.size()==length_) {
                        if(ready_.size()>=slots_) throw FrameError("Receive queue full");
                        ready_.push_back(std::move(payload_));
                        payload_.clear(); length_=0; header_bytes_=0;
                    }
                }
            }
        } catch(...) { failed_=true; ready_.clear(); payload_.clear(); throw; }
    }
    bool available() const noexcept { return !ready_.empty(); }
    bool partial() const noexcept { return header_bytes_!=0; }
    std::string pop() {
        if(ready_.empty()) throw FrameError("No complete frame");
        auto value=std::move(ready_.front()); ready_.pop_front(); return value;
    }
    void finish() {
        if(failed_ || header_bytes_) { failed_=true; throw FrameError("Truncated or failed connection"); }
    }
private:
    std::size_t max_,slots_;
    std::uint32_t length_{};
    unsigned header_bytes_{};
    bool failed_{};
    std::string payload_;
    std::deque<std::string> ready_;
};
inline std::string frame(std::string_view payload, std::size_t max=1048576) {
    if(payload.empty() || payload.size()>max || payload.size()>1048576) throw FrameError("Invalid frame length");
    const auto n=static_cast<std::uint32_t>(payload.size());
    std::string out;
    out.reserve(4+payload.size());
    for(int shift=24;shift>=0;shift-=8) out.push_back(static_cast<char>((n>>shift)&255));
    out.append(payload);
    return out;
}
} // namespace vision::ipc
