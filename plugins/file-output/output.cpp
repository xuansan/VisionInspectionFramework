#include "../output-common/async.hpp"
#include <vision/inference/engine.hpp>
#include <fstream>
#include <algorithm>
#define NOMINMAX
#include <windows.h>
namespace {
std::string csv(std::string text) {
    if(!text.empty()&&(text[0]=='='||text[0]=='+'||text[0]=='-'||text[0]=='@'||text[0]=='\t'||text[0]=='\r'))text="'"+text;
    std::string out="\"";for(char c:text){if(c=='"')out+='"';out+=c;}return out+"\"";
}
class Output final:public vision::outputs::AsyncOutput {
public:
    ~Output(){finish();}
private:
    void configure(const nlohmann::json& j) override {
        if(j.size()!=3||!j.contains("root")||!j.contains("format")||!j.contains("max_files"))throw std::invalid_argument("File parameters");
        root_=vision::inference::utf8_path(j.at("root").get<std::string>());format_=j.at("format");
        const auto& count=j.at("max_files");
        if(!count.is_number_integer()||count.get<std::int64_t>()<1||count.get<std::uint64_t>()>128)throw std::invalid_argument("Retention");
        max_=count.get<unsigned>();
        if(!std::filesystem::is_directory(root_)||std::filesystem::is_symlink(root_)||(format_!="jsonl"&&format_!="csv"))throw std::invalid_argument("File root/format");
    }
    void write(const nlohmann::json& request) override {
        const auto event=vision::serialization::decode_result(request.at("event").dump());
        const auto canonical=vision::serialization::encode_result(event);
        std::string record;
        if(format_=="jsonl")record=canonical+"\n";
        else record="schema_version,event_id,run_id,inspection_id,workpiece_id,quality,result_sha256\r\n1,"+csv(event.event_id.value())+","+csv(event.result.run_id.value())+
            ","+csv(event.result.inspection_id.value())+","+csv(event.result.workpiece_id.value())+","+csv(std::string(vision::contracts::name(event.result.quality)))+
            ","+csv(vision::inference::sha256(std::as_bytes(std::span(canonical.data(),canonical.size()))))+"\r\n";
        if(record.size()>65536)throw std::runtime_error("File record limit");
        const auto id=event.event_id.value();const auto hash=vision::inference::sha256(std::as_bytes(std::span(id.data(),id.size()))).substr(7);
        const auto destination=root_/(hash+"."+format_);
        struct Handle{HANDLE h;~Handle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}};
        Handle lock{CreateFileW((root_/L".output-lock").c_str(),GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr)};
        if(lock.h==INVALID_HANDLE_VALUE)throw std::runtime_error("Output directory busy");
        if(std::filesystem::exists(destination)) {
            if(std::filesystem::is_symlink(destination))throw std::runtime_error("Output symlink");
            const auto old=vision::inference::read_file(destination,65536);
            if(std::string(reinterpret_cast<const char*>(old.data()),old.size())!=record)throw std::runtime_error("Event conflict");
            return;
        }
        std::vector<std::filesystem::directory_entry> files;unsigned entries=0;
        for(const auto& e:std::filesystem::directory_iterator(root_)) {
            if(++entries>256||!e.is_regular_file()||e.is_symlink())throw std::runtime_error("Output directory bounds");
            if(e.path().extension()=="."+format_){if(e.file_size()>65536)throw std::runtime_error("Output file limit");files.push_back(e);}
        }
        std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.last_write_time()<b.last_write_time();});
        const auto temporary=root_/(hash+".partial");
        Handle file{CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr)};
        if(file.h==INVALID_HANDLE_VALUE)throw std::runtime_error("File create");
        DWORD written=0;
        if(!WriteFile(file.h,record.data(),static_cast<DWORD>(record.size()),&written,nullptr)||written!=record.size()||!FlushFileBuffers(file.h))
            throw std::runtime_error("File write/flush");
        CloseHandle(file.h);file.h=INVALID_HANDLE_VALUE;
        if(!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH))throw std::runtime_error("File publish");
        while(files.size()>=max_){std::filesystem::remove(files.front().path());files.erase(files.begin());}
    }
    std::filesystem::path root_;std::string format_;unsigned max_{};
};
}
#define VISION_OUTPUT_ID "vision.file-output"
#define VISION_OUTPUT_BUILD "file-output-v1"
#include "../output-common/exports.hpp"
