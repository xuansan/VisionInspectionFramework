#define CPPHTTPLIB_REQUEST_URI_MAX_LENGTH 2048
#define CPPHTTPLIB_HEADER_MAX_LENGTH 8192
#define CPPHTTPLIB_HEADER_MAX_COUNT 32
#include <httplib.h>
#include "../output-common/async.hpp"
#include <vision/storage/store.hpp>
#include <vision/inference/engine.hpp>
#include <deque>
#include <cstdlib>
#include <charconv>
#include <random>
namespace {
using J=nlohmann::json;
std::uint64_t number(const std::string& text) {
    if(text.empty())return 0;
    return vision::contracts::parse_u64(text);
}
class Output final:public vision::outputs::AsyncOutput {
public:
    ~Output(){finish();server_.stop();if(listener_.joinable())listener_.join();}
    void request_stop() noexcept override {vision::outputs::AsyncOutput::request_stop();server_.stop();}
private:
    static void error(httplib::Response& response,int status,const char* code) {
        response.status=status;response.set_content(J{{"code",code},{"message",code},{"request_id",response.get_header_value("X-Request-ID")}}.dump(),"application/json");
    }
    void configure(const J& j) override {
        if(j.size()!=3||!j.contains("root")||!j.contains("port")||!j.contains("token_env"))throw std::invalid_argument("HTTP parameters");
        root_=vision::inference::utf8_path(j.at("root").get<std::string>());
        std::random_device random;
        epoch_=std::to_string(random())+std::to_string(random());
        const auto variable=j.at("token_env").get<std::string>();
        if(variable.empty()||variable.size()>128)throw std::invalid_argument("Secret reference");
        const auto* secret=std::getenv(variable.c_str());if(!secret||std::strlen(secret)<32||std::strlen(secret)>128)throw std::invalid_argument("HTTP token");
        token_="Bearer "+std::string(secret);const auto& p=j.at("port");
        if(!p.is_number_integer()||p.get<std::int64_t>()<1024||p.get<std::uint64_t>()>65535)throw std::invalid_argument("HTTP port");
        server_.new_task_queue=[] {return new httplib::ThreadPool(4,4,8);};
        server_.set_read_timeout(0,300000);server_.set_write_timeout(0,300000);
        server_.set_keep_alive_max_count(2);server_.set_payload_max_length(1024);
        server_.set_pre_routing_handler([this](const auto& request,auto& response) {
            response.set_header("X-Request-ID",epoch_+"-"+std::to_string(++request_sequence_));
            const auto supplied=request.get_header_value("Authorization");
            unsigned different=static_cast<unsigned>(supplied.size()!=token_.size());
            for(std::size_t i=0;i<token_.size();++i)different|=static_cast<unsigned char>(token_[i])^(i<supplied.size()?static_cast<unsigned char>(supplied[i]):0);
            if(different){error(response,401,"AUTH.REQUIRED");return httplib::Server::HandlerResponse::Handled;}
            if(request.method!="GET"){error(response,405,"METHOD.NOT_ALLOWED");return httplib::Server::HandlerResponse::Handled;}
            {std::lock_guard lock(mutex_);const auto now=std::chrono::steady_clock::now();
                if(now-rate_start_>=std::chrono::seconds(1)){rate_start_=now;requests_=0;}
                if(++requests_>64){error(response,429,"RATE.LIMIT");return httplib::Server::HandlerResponse::Handled;}}
            response.set_header("Cache-Control","no-store");response.set_header("X-Content-Type-Options","nosniff");
            return httplib::Server::HandlerResponse::Unhandled;
        });
        server_.Get("/api/v1/health",[this](const auto&,auto& response) {
            bool storage=true;try{(void)vision::storage::history(root_,0,1);}catch(...){storage=false;}
            std::lock_guard lock(mutex_);
            response.set_content(J{{"alive",true},{"production_ready",false},{"storage_available",storage},
                {"live_window",events_.size()},{"fresh",last_!=std::chrono::steady_clock::time_point{}&&std::chrono::steady_clock::now()-last_<std::chrono::seconds(2)}}.dump(),"application/json");
        });
        server_.Get("/api/v1/inspections",[this](const auto& request,auto& response) {
            try {
                for(const auto& [key,value]:request.params){(void)value;if(key!="after"&&key!="limit")throw std::invalid_argument("Query");}
                const auto after=number(request.get_param_value("after"));const auto limit=request.has_param("limit")?number(request.get_param_value("limit")):50;
                if(!limit||limit>200)throw std::invalid_argument("Limit");
                J items=J::array();std::uint64_t next=after;
                for(const auto& row:vision::storage::history(root_,after,static_cast<unsigned>(limit))){items.push_back(J::parse(row.body));next=row.cursor;}
                response.set_content(J{{"items",items},{"next_cursor",std::to_string(next)}}.dump(),"application/json");
            }catch(const std::invalid_argument&){error(response,400,"QUERY.INVALID");}catch(...){error(response,503,"STORAGE.UNAVAILABLE");}
        });
        server_.Get(R"(/api/v1/inspections/([A-Za-z0-9_.:-]{1,128}))",[this](const auto& request,auto& response) {
            try{const auto body=vision::storage::find_result(root_,request.matches[1].str());if(body.empty())error(response,404,"RESULT.NOT_FOUND");else response.set_content(body,"application/json");}
            catch(...){error(response,503,"STORAGE.UNAVAILABLE");}
        });
        server_.Get("/api/v1/events",[this](const auto& request,auto& response) {
            try {
                const auto cursor=request.get_header_value("Last-Event-ID");
                std::uint64_t after=0;
                if(!cursor.empty()){
                    const auto separator=cursor.find('/');
                    if(separator==std::string::npos||cursor.substr(0,separator)!=epoch_){error(response,409,"CURSOR.EXPIRED");return;}
                    after=number(cursor.substr(separator+1));
                }
                std::string stream="retry: 1000\n\n";
                {std::lock_guard lock(mutex_);
                    if(after>sequence_||!events_.empty()&&after&&after<events_.front().first-1){error(response,409,"CURSOR.EXPIRED");return;}
                    for(const auto& [id,body]:events_)if(id>after)stream+="id: "+epoch_+"/"+std::to_string(id)+"\nevent: inspection.finalized\ndata: "+body+"\n\n";
                }
                // Finite SSE batches, automatic EventSource reconnect every second; no unbounded held queues.
                response.set_content(stream+": heartbeat\n\n","text/event-stream");
            }catch(...){error(response,400,"CURSOR.INVALID");}
        });
        server_.Get(R"(/api/v1/images/([A-Za-z0-9_.:-]{1,128})/access)",[this](const auto& request,auto& response) {
            try{
                const auto id=request.matches[1].str();const auto info=vision::storage::find_image(root_,id);
                if(info.state=="Missing"||info.state=="Deleted"){error(response,404,"IMAGE.NOT_FOUND");return;}
                if(info.state!="Available"){error(response,409,"IMAGE.NOT_READY");return;}
                const auto expiry=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count())+60;
                response.set_content(J{{"url","/api/v1/images/"+id+"/content?expires="+std::to_string(expiry)},{"expires_unix",std::to_string(expiry)},{"authorization_required",true}}.dump(),"application/json");
            }catch(...){error(response,503,"STORAGE.UNAVAILABLE");}
        });
        server_.Get(R"(/api/v1/images/([A-Za-z0-9_.:-]{1,128})/content)",[this](const auto& request,auto& response) {
            try {
                const auto expiry=number(request.get_param_value("expires"));
                const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
                if(expiry<now||expiry>now+60){error(response,403,"ACCESS.EXPIRED");return;}
                const auto id=request.matches[1].str();const auto info=vision::storage::find_image(root_,id);
                if(info.state=="Missing"||info.state=="Deleted"){error(response,404,"IMAGE.NOT_FOUND");return;}
                if(info.state!="Available"){error(response,409,"IMAGE.NOT_READY");return;}
                if(std::filesystem::is_symlink(root_/(id+".stage"))){error(response,503,"IMAGE.INTEGRITY");return;}
                const auto bytes=vision::inference::read_file(root_/(id+".stage"),16*1024*1024);
                if(bytes.size()!=info.bytes||vision::inference::sha256(bytes)!=info.hash){error(response,503,"IMAGE.INTEGRITY");return;}
                response.set_content(std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size()),"application/octet-stream");
            }catch(...){error(response,503,"IMAGE.UNAVAILABLE");}
        });
        if(!server_.bind_to_port("127.0.0.1",p.get<int>()))throw std::runtime_error("HTTP bind");
        listener_=std::thread([this]{server_.listen_after_bind();});
    }
    void write(const J& request) override {
        const auto text=request.at("event").dump();if(text.size()>32768)throw std::invalid_argument("HTTP result bound");
        std::lock_guard lock(mutex_);
        const auto id=request["event"]["event_id"].get<std::string>();
        for(const auto& [sequence,body]:events_){(void)sequence;const auto old=J::parse(body);if(old["event_id"]==id){if(body!=text)throw std::runtime_error("Event conflict");return;}}
        if(sequence_==UINT64_MAX)throw std::runtime_error("SSE cursor exhausted");
        events_.emplace_back(++sequence_,text);if(events_.size()>64)events_.pop_front();last_=std::chrono::steady_clock::now();
    }
    httplib::Server server_;std::thread listener_;std::filesystem::path root_;std::string token_,epoch_;
    std::mutex mutex_;std::deque<std::pair<std::uint64_t,std::string>> events_;std::uint64_t sequence_{};unsigned requests_{};
    std::atomic<std::uint64_t> request_sequence_{};
    std::chrono::steady_clock::time_point rate_start_=std::chrono::steady_clock::now(),last_{};
};
}
#define VISION_OUTPUT_ID "vision.http-output"
#define VISION_OUTPUT_BUILD "http-output-v1"
#include "../output-common/exports.hpp"
