#include <vision/plugin_sdk/api.hpp>
#include <vision/modbus/client.hpp>
#include <vision/contracts/types.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstring>
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
using namespace vision;
using namespace plugin_sdk;
namespace {
using J=nlohmann::json;
std::vector<std::uint16_t> token(const std::string& text) {
    unsigned char digest[32]{};
    if(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),
        static_cast<ULONG>(text.size()),digest,32)<0)throw std::runtime_error("Token");
    std::vector<std::uint16_t> words;for(unsigned i=0;i<16;i+=2)words.push_back(static_cast<std::uint16_t>((digest[i]<<8)|digest[i+1]));return words;
}
void append(std::vector<std::uint16_t>& words,std::uint64_t value){for(int shift=48;shift>=0;shift-=16)words.push_back(static_cast<std::uint16_t>(value>>shift));}
std::uint64_t number(const std::vector<std::uint16_t>& words,std::size_t offset){std::uint64_t n=0;for(unsigned i=0;i<4;++i)n=(n<<16)|words[offset+i];return n;}
class Device final:public Communication {
public:
    Status initialize(Bytes bytes) noexcept override {
        try {
            const auto j=J::parse(bytes.data,bytes.data+bytes.size);
            for(auto it=j.begin();it!=j.end();++it)if(it.key()!="address"&&it.key()!="port"&&it.key()!="unit"&&it.key()!="write_base"&&it.key()!="read_base")return Status::Invalid;
            auto integer=[&](const char* key,unsigned limit){const auto& v=j.at(key);if(!v.is_number_integer()||v.get<std::int64_t>()<0||v.get<std::uint64_t>()>limit)throw std::invalid_argument("Register configuration");return v.get<unsigned>();};
            address_=j.at("address");port_=static_cast<std::uint16_t>(integer("port",65535));unit_=static_cast<std::uint8_t>(integer("unit",255));
            write_=static_cast<std::uint16_t>(integer("write_base",65512));read_=static_cast<std::uint16_t>(integer("read_base",65511));
            if(!port_||!(write_+24<=read_||read_+25<=write_))return Status::Invalid;
            return Status::Ok;
        }catch(...){return Status::Invalid;}
    }
    void request_stop() noexcept override{stopped_=true;}
    Status connect(Bytes bytes) noexcept override {
        try {
            const auto id=J::parse(bytes.data,bytes.data+bytes.size).at("session_id").get<std::string>();(void)contracts::TaskId(id);
            if(!session_.empty())return session_==id&&!stopped_?Status::Ok:Status::Invalid;
            client_=std::make_unique<modbus::Client>(address_,port_,unit_);session_=id;return Status::Ok;
        }catch(...){return Status::Failed;}
    }
    Status try_command(Bytes bytes) noexcept override {
        try {
            if(stopped_||!client_||!report_.empty())return Status::Busy;
            const auto j=J::parse(bytes.data,bytes.data+bytes.size);
            const auto sequence=contracts::parse_u64(j.at("command_sequence").get<std::string>());
            if(j.at("session_id")!=session_||sequence<=sequence_)return Status::Invalid;sequence_=sequence;
            const auto identity=token(session_);auto words=identity;append(words,sequence);
            words.push_back(j.at("ready").get<bool>()?1:0);
            const bool result=!j.at("result_id").is_null();
            const auto result_id=result?j.at("result_id").get<std::string>():std::string{};
            const auto result_token=result?token(result_id):std::vector<std::uint16_t>(8);
            words.insert(words.end(),result_token.begin(),result_token.end());words.push_back(result?1:0);
            words.push_back(200);words.push_back(j.at("quality")=="OK"?1:j.at("quality")=="NG"?2:0);
            client_->write(write_,words);const auto reply=client_->read(read_,25);
            if(!std::equal(identity.begin(),identity.end(),reply.begin())||number(reply,8)!=sequence)
                return Status::Failed;
            const bool ready=reply[12]&1,position=reply[12]&2,safety=reply[12]&4;
            if(reply[12]>7||ready&&(!position||!safety)||!std::equal(result_token.begin(),result_token.end(),reply.begin()+17))
                return Status::Failed;
            report_=J{{"session_id",session_},{"command_sequence",std::to_string(sequence)},{"ready",ready},
                {"position",position},{"safety_ok",safety},{"arrival_sequence",std::to_string(number(reply,13))},
                {"ack_id",result?J(result_id):J(nullptr)}}.dump();return Status::Ok;
        }catch(...){return Status::Failed;}
    }
    Status poll_input(Buffer& out) noexcept override {
        if(stopped_)return Status::Cancelled;if(report_.empty())return Status::Busy;
        if(report_.size()>out.capacity)return Status::Invalid;
        std::memcpy(out.data,report_.data(),report_.size());out.size=static_cast<std::uint32_t>(report_.size());report_.clear();return Status::Ok;
    }
private:
    std::unique_ptr<modbus::Client> client_;std::atomic<bool> stopped_{};
    std::string address_,session_,report_;std::uint16_t port_{},write_{},read_{};std::uint8_t unit_{};std::uint64_t sequence_{};
};
template<std::size_t N>void copy(char(&out)[N],const char* text){const auto n=std::strlen(text);if(n<N)std::memcpy(out,text,n+1);}
}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_describe(Descriptor* out,std::uint32_t size) noexcept {
    if(!out||size!=sizeof(Descriptor))return Status::Invalid;*out={};out->struct_size=size;out->version=api_version;
    out->kind=Kind::Communication;out->threading=Threading::Serialized;out->max_instances=1;
    copy(out->plugin_id,"vision.modbus-device");copy(out->plugin_version,"0.1.0");copy(out->build_id,"modbus-device-v1");
    copy(out->platform,"windows");copy(out->architecture,"x64");copy(out->compiler_abi,"msvc-v143-md");
#ifdef _DEBUG
    copy(out->runtime_variant,"Debug");
#else
    copy(out->runtime_variant,"Release");
#endif
    return Status::Ok;
}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_create(Plugin** out) noexcept {
    if(!out)return Status::Invalid;try{*out=new Device;return Status::Ok;}catch(...){*out=nullptr;return Status::Failed;}
}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_destroy(Plugin* p) noexcept {
    auto* device=dynamic_cast<Device*>(p);if(!device)return Status::Invalid;delete device;return Status::Ok;
}
