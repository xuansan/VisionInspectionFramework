#include "../output-common/async.hpp"
#include <curl/curl.h>
#include <cstdlib>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <set>
#include <vector>
namespace {
using J=nlohmann::json;
class Output final:public vision::outputs::AsyncOutput {
public:
    ~Output(){finish();}
private:
    void configure(const J& j) override {
        if(j.size()!=3||!j.contains("url")||!j.contains("token_env")||!j.contains("timeout_ms"))
            throw std::invalid_argument("Webhook parameters");
        url_=j.at("url").get<std::string>();
        if(url_.size()>1024||url_.find_first_of("@?#\\")!=std::string::npos)
            throw std::invalid_argument("Webhook URL");
        for(unsigned char c:url_)if(c<=32||c>=127)throw std::invalid_argument("Webhook URL character");
        std::unique_ptr<CURLU,decltype(&curl_url_cleanup)> parsed(curl_url(),curl_url_cleanup);
        if(!parsed||curl_url_set(parsed.get(),CURLUPART_URL,url_.c_str(),0)!=CURLUE_OK)
            throw std::invalid_argument("Webhook URL");
        auto part=[&](CURLUPart field){
            char* value=nullptr;
            if(curl_url_get(parsed.get(),field,&value,0)!=CURLUE_OK)throw std::invalid_argument("Webhook URL part");
            std::unique_ptr<char,decltype(&curl_free)> owned(value,curl_free);return std::string(value);
        };
        const auto scheme=part(CURLUPART_SCHEME),host=part(CURLUPART_HOST);
        if(scheme!="https"&&(scheme!="http"||host!="127.0.0.1"))
            throw std::invalid_argument("Webhook requires HTTPS");
        const auto& timeout=j.at("timeout_ms");
        if(!timeout.is_number_integer()||timeout.get<std::int64_t>()<100||timeout.get<std::int64_t>()>5000)
            throw std::invalid_argument("Webhook timeout");
        timeout_=timeout.get<long>();
        const auto variable=j.at("token_env").get<std::string>();
        if(variable.empty()||variable.size()>128)throw std::invalid_argument("Webhook secret reference");
        for(unsigned char c:variable)if(!(std::isalnum(c)||c=='_'))throw std::invalid_argument("Webhook secret reference");
        const auto* secret=std::getenv(variable.c_str());
        if(!secret||std::strlen(secret)<32||std::strlen(secret)>128)throw std::invalid_argument("Webhook secret");
        token_=secret;
        for(unsigned char c:token_)if(c<=32||c>=127)throw std::invalid_argument("Webhook secret character");
        static std::once_flag init;
        std::call_once(init,[]{if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)throw std::runtime_error("Curl init");});
    }
    void write(const J& request) override {
        if(stopped_)throw std::runtime_error("Webhook stopped");
        const auto id=request.at("event").at("event_id").get<std::string>();
        for(unsigned char c:id)if(c<=32||c>=127)throw std::invalid_argument("Webhook identity header");
        const auto body=request.dump();
        std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> handle(curl_easy_init(),curl_easy_cleanup);
        if(!handle)throw std::runtime_error("Curl handle");
        curl_slist* raw=nullptr;
        auto append=[&](const std::string& header){
            auto* next=curl_slist_append(raw,header.c_str());
            if(!next){curl_slist_free_all(raw);raw=nullptr;throw std::bad_alloc();}
            raw=next;
        };
        append("Content-Type: application/json");
        append("Authorization: Bearer "+token_);
        append("Idempotency-Key: "+id);
        append("Expect:");
        std::unique_ptr<curl_slist,decltype(&curl_slist_free_all)> headers(raw,curl_slist_free_all);
        const auto h=handle.get();std::string response;
        curl_easy_setopt(h,CURLOPT_URL,url_.c_str());
        curl_easy_setopt(h,CURLOPT_POST,1L);
        curl_easy_setopt(h,CURLOPT_HTTPHEADER,headers.get());
        curl_easy_setopt(h,CURLOPT_POSTFIELDS,body.data());
        curl_easy_setopt(h,CURLOPT_POSTFIELDSIZE_LARGE,static_cast<curl_off_t>(body.size()));
        curl_easy_setopt(h,CURLOPT_PROTOCOLS_STR,"http,https");
        curl_easy_setopt(h,CURLOPT_FOLLOWLOCATION,0L);
        curl_easy_setopt(h,CURLOPT_CONNECTTIMEOUT_MS,timeout_);
        curl_easy_setopt(h,CURLOPT_TIMEOUT_MS,timeout_);
        curl_easy_setopt(h,CURLOPT_NOSIGNAL,1L);
        curl_easy_setopt(h,CURLOPT_PROXY,"");
        curl_easy_setopt(h,CURLOPT_NOPROGRESS,0L);
        curl_easy_setopt(h,CURLOPT_XFERINFODATA,this);
        curl_easy_setopt(h,CURLOPT_XFERINFOFUNCTION,+[](void* self,curl_off_t,curl_off_t,curl_off_t,curl_off_t)->int{
            return static_cast<Output*>(self)->stopped_?1:0;
        });
        curl_easy_setopt(h,CURLOPT_WRITEDATA,&response);
        curl_easy_setopt(h,CURLOPT_WRITEFUNCTION,+[](char* bytes,std::size_t size,std::size_t count,void* data)->std::size_t {
            auto& text=*static_cast<std::string*>(data);
            if(size&&count>4096/size)return 0;
            const auto n=size*count;if(n>4096-text.size())return 0;
            try{text.append(bytes,n);return n;}catch(...){return 0;}
        });
        const auto status=curl_easy_perform(h);long code=0;curl_easy_getinfo(h,CURLINFO_RESPONSE_CODE,&code);
        if(status!=CURLE_OK||code<200||code>=300)throw std::runtime_error("Webhook transport/status");
        std::set<std::string> keys;bool valid=true;
        const auto ack=J::parse(response,[&](int depth,J::parse_event_t event,J& value){
            if(depth>2)valid=false;
            if(event==J::parse_event_t::key&&!keys.insert(value.get<std::string>()).second)valid=false;
            return true;
        });
        if(!valid||!ack.is_object()||ack.size()!=2||ack.value("event_id",std::string{})!=id||
           ack.value("state",std::string{})!="BusinessAcked")throw std::runtime_error("Webhook business acknowledgment");
    }
    std::string url_,token_;long timeout_{};
};
}
#define VISION_OUTPUT_ID "vision.webhook-output"
#define VISION_OUTPUT_BUILD "webhook-output-v1"
#include "../output-common/exports.hpp"
