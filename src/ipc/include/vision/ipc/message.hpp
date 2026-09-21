#pragma once
#include <vision/ipc/session.hpp>
#include <vision/contracts/types.hpp>

namespace vision::ipc {
struct Message {
    Header header;
    std::optional<contracts::Correlation> correlation;
    std::string payload; // Validated compact JSON object. No JSON library in the public API.
};
class ProtocolError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
Message decode_message(std::string_view json);
std::string encode_message(const Message& message);
bool control_type(std::string_view type) noexcept;
struct HelloPayload { std::string token; std::set<std::string> required; };
HelloPayload read_hello(const Message& message);
std::string hello_payload(std::string_view token);
} // namespace vision::ipc
