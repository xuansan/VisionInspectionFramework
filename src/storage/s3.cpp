#include <vision/storage/store.hpp>
#include <curl/curl.h>
#include <mutex>
#include <stdexcept>
namespace vision::storage {
namespace {
void key(const std::string& value) {
    if(value.empty()||value.size()>256||value.find("..")!=std::string::npos||value.front()=='/')throw std::invalid_argument("Object key");
    for(const unsigned char c:value)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='/'))throw std::invalid_argument("Object key");
}
}
S3::S3(std::string endpoint,std::string bucket,std::string access,std::string secret)
    :endpoint_(std::move(endpoint)),bucket_(std::move(bucket)),access_(std::move(access)),secret_(std::move(secret)) {
    if((!endpoint_.starts_with("https://")&&!endpoint_.starts_with("http://127.0.0.1:"))||endpoint_.size()>1024||
       endpoint_.find('@')!=std::string::npos||endpoint_.find('?')!=std::string::npos||endpoint_.find('#')!=std::string::npos||
       bucket_.find('/')!=std::string::npos||access_.empty()||secret_.empty())throw std::invalid_argument("S3 configuration");
    key(bucket_);static std::once_flag initialized;std::call_once(initialized,[]{if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)throw std::runtime_error("Curl init");});
}
std::vector<std::byte> S3::request(const char* method,std::string object,std::span<const std::byte> body) {
    key(object);if(body.size()>16*1024*1024)throw std::invalid_argument("S3 payload");
    struct Handle{CURL* p=curl_easy_init();~Handle(){if(p)curl_easy_cleanup(p);}} h;if(!h.p)throw std::runtime_error("Curl");
    const auto url=endpoint_+"/"+bucket_+"/"+object;std::vector<std::byte> response;
    curl_easy_setopt(h.p,CURLOPT_URL,url.c_str());curl_easy_setopt(h.p,CURLOPT_CUSTOMREQUEST,method);
    curl_easy_setopt(h.p,CURLOPT_USERNAME,access_.c_str());curl_easy_setopt(h.p,CURLOPT_PASSWORD,secret_.c_str());
    curl_easy_setopt(h.p,CURLOPT_AWS_SIGV4,"aws:amz:us-east-1:s3");
    curl_easy_setopt(h.p,CURLOPT_PROTOCOLS_STR,"http,https");curl_easy_setopt(h.p,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(h.p,CURLOPT_CONNECTTIMEOUT_MS,500L);curl_easy_setopt(h.p,CURLOPT_TIMEOUT_MS,2000L);
    curl_easy_setopt(h.p,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(h.p,CURLOPT_PROXY,"");
    if(std::string_view(method)=="PUT"){curl_easy_setopt(h.p,CURLOPT_POSTFIELDS,body.empty()?"":reinterpret_cast<const char*>(body.data()));curl_easy_setopt(h.p,CURLOPT_POSTFIELDSIZE_LARGE,static_cast<curl_off_t>(body.size()));}
    curl_easy_setopt(h.p,CURLOPT_WRITEDATA,&response);
    curl_easy_setopt(h.p,CURLOPT_WRITEFUNCTION,+[](char* bytes,std::size_t size,std::size_t count,void* data)->std::size_t {
        auto& buffer=*static_cast<std::vector<std::byte>*>(data);
        if(size&&count>16*1024*1024/size)return 0;const auto n=size*count;if(n>16*1024*1024-buffer.size())return 0;
        try{buffer.insert(buffer.end(),reinterpret_cast<std::byte*>(bytes),reinterpret_cast<std::byte*>(bytes)+n);return n;}catch(...){return 0;}
    });
    const auto status=curl_easy_perform(h.p);long code=0;curl_easy_getinfo(h.p,CURLINFO_RESPONSE_CODE,&code);
    if(status!=CURLE_OK||code<200||code>=300)throw std::runtime_error("S3 transport or status failure");
    return response;
}
void S3::put(std::string key,std::span<const std::byte> bytes){(void)request("PUT",std::move(key),bytes);}
std::vector<std::byte> S3::get(std::string key){return request("GET",std::move(key),{});}
void S3::remove(std::string key){(void)request("DELETE",std::move(key),{});}
}
