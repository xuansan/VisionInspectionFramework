#pragma once
#include <vision/plugin_sdk/api.hpp>
#include <nlohmann/json.hpp>
#include <charconv>
#include <set>
#include <string_view>
#include <vector>
namespace vision::plc {
inline nlohmann::json parse(plugin_sdk::Bytes b){
    if(!b.data||!b.size||b.size>16384)throw std::invalid_argument("PLC JSON size");
    std::vector<std::set<std::string>> keys;
    auto j=nlohmann::json::parse(b.data,b.data+b.size,[&](int depth,nlohmann::json::parse_event_t event,nlohmann::json& value){
        if(depth>6)throw std::invalid_argument("PLC JSON depth");
        if(event==nlohmann::json::parse_event_t::object_start)keys.emplace_back();
        if(event==nlohmann::json::parse_event_t::key&&!keys.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("Duplicate PLC key");
        if(event==nlohmann::json::parse_event_t::object_end)keys.pop_back();
        return true;
    });
    if(!j.is_object())throw std::invalid_argument("PLC object required");
    return j;
}
inline bool ipv4(std::string_view text){
    for(int part=0;part<4;++part){
        auto end=text.find('.');
        auto segment=text.substr(0,end);
        unsigned value{};
        auto [ptr,error]=std::from_chars(segment.data(),segment.data()+segment.size(),value);
        if(segment.empty()||segment.size()>3||error!=std::errc{}||ptr!=segment.data()+segment.size()||value>255)return false;
        if(part==3)return end==std::string_view::npos;
        if(end==std::string_view::npos)return false;
        text.remove_prefix(end+1);
    }
    return false;
}
}
