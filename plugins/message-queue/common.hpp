#pragma once
#include "../output-common/async.hpp"
#include <charconv>
#include <set>
#include <vector>
namespace vision::mq {
using J=nlohmann::json;
inline unsigned port(const J& config) {
    const auto& p=config.at("port");
    if(!p.is_number_integer()||p.get<std::int64_t>()<1||p.get<std::int64_t>()>65535)
        throw std::invalid_argument("MQ port");
    return p.get<unsigned>();
}
inline unsigned timeout(const J& config) {
    const auto& p=config.at("timeout_ms");
    if(!p.is_number_integer()||p.get<std::int64_t>()<100||p.get<std::int64_t>()>800)
        throw std::invalid_argument("MQ deadline");
    return p.get<unsigned>();
}
inline std::string name(const J& config,const char* key,bool empty=false) {
    auto s=config.at(key).get<std::string>();
    if((s.empty()&&!empty)||s.size()>128)throw std::invalid_argument("MQ name");
    for(unsigned char c:s)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
        (c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))throw std::invalid_argument("MQ name character");
    return s;
}
inline bool ack(std::string_view text,const J& request) {
    if(text.empty()||text.size()>4096)throw std::runtime_error("MQ ack bounds");
    std::vector<std::set<std::string>> keys;
    const auto value=J::parse(text,[&](int depth,J::parse_event_t event,J& token){
        if(depth>2)throw std::runtime_error("MQ ack depth");
        if(event==J::parse_event_t::object_start)keys.emplace_back();
        if(event==J::parse_event_t::key&&!keys.back().insert(token.get<std::string>()).second)
            throw std::runtime_error("MQ duplicate key");
        if(event==J::parse_event_t::object_end)keys.pop_back();
        return true;
    });
    return value.is_object()&&value.size()==4&&value.at("event_id")==request.at("event").at("event_id")&&
        value.at("output_instance_id")==request.at("output_instance_id")&&
        value.at("attempt")==request.at("attempt")&&value.at("state")=="BusinessAcked";
}
class Deadline {
    std::chrono::steady_clock::time_point end_;
    const std::atomic<bool>& stopped_;
public:
    Deadline(unsigned ms,const std::atomic<bool>& stopped):end_(std::chrono::steady_clock::now()+std::chrono::milliseconds(ms)),stopped_(stopped){}
    int remaining() const {
        auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(end_-std::chrono::steady_clock::now()).count();
        if(stopped_||ms<=0)throw std::runtime_error("MQ stopped or expired");
        return static_cast<int>(ms);
    }
};
}
