#include <vision/storage/store.hpp>
#include <vision/inference/engine.hpp>
#include <vision/serialization/json_codec.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <cstdlib>
#define NOMINMAX
#include <windows.h>
using namespace vision;
struct UploadLock {
    HANDLE handle=INVALID_HANDLE_VALUE;
    ~UploadLock(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
};
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc!=3)return 2;
        const auto bytes=inference::read_file(std::filesystem::path(argv[2]),65536);
        const auto j=nlohmann::json::parse(reinterpret_cast<const char*>(bytes.data()),reinterpret_cast<const char*>(bytes.data()+bytes.size()));
        storage::Store store(inference::utf8_path(j.at("root").get<std::string>()));
        const std::wstring operation=argv[1];
        UploadLock upload_lock;
        if(operation==L"upload"||operation==L"resume-uploads"){
            const auto path=inference::utf8_path(j.at("root").get<std::string>())/L".upload-lock";
            upload_lock.handle=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(upload_lock.handle==INVALID_HANDLE_VALUE)throw std::runtime_error("Uploader busy");
        }
        if(operation==L"commit") {
            const auto event=serialization::decode_result(j.at("event").dump());
            std::cout<<nlohmann::json{{"inserted",store.commit(event,j.at("outputs").get<std::vector<std::string>>())},{"persistence","Durable"}}.dump()<<'\n';
        } else if(operation==L"query") {
            std::cout<<"[";bool first=true;for(const auto& row:store.query(j.value("after",0ULL),j.value("limit",50U))){if(!first)std::cout<<",";first=false;std::cout<<row;}std::cout<<"]\n";
        } else if(operation==L"trace") {
            const auto body=storage::find_result(inference::utf8_path(j.at("root").get<std::string>()),j.at("event_id").get<std::string>());
            if(body.empty())throw std::runtime_error("Result not found");
            const auto event=serialization::decode_result(body);auto images=nlohmann::json::array();
            for(const auto& frame:store.frames(event.result.run_id.value(),event.result.inspection_id.value())){
                bool intact=true;try{(void)store.image(frame.image);}catch(...){intact=false;}
                const auto image=storage::find_image(inference::utf8_path(j.at("root").get<std::string>()),frame.image);
                images.push_back({{"image_id",frame.image},{"channel",frame.channel},{"descriptor",nlohmann::json::parse(frame.descriptor)},
                    {"recipe_hash",frame.recipe_hash},{"recipe_body",frame.recipe_body},{"object_state",image.state},
                    {"hash",image.hash},{"bytes",image.bytes},{"local_integrity",intact}});
            }
            std::cout<<nlohmann::json{{"result",nlohmann::json::parse(body)},{"images",images},{"production_ready",false}}.dump()<<'\n';
        } else if(operation==L"upload") {
            const char* access=std::getenv("VISION_S3_ACCESS"),*secret=std::getenv("VISION_S3_SECRET");
            if(!access||!secret)throw std::runtime_error("Missing S3 secret reference");
            storage::S3 s3(j.at("endpoint"),j.at("bucket"),access,secret);
            const auto id=j.at("image_id").get<std::string>(),key=j.at("key").get<std::string>();
            const auto image=inference::read_file(inference::utf8_path(j.at("file").get<std::string>()),16*1024*1024);
            const auto hash=store.stage_image(id,image);
            store.bind_object({id,j.at("endpoint"),j.at("bucket"),key,0});
            if(store.image_state(id)=="Available"){
                if(inference::sha256(s3.get(key))!=hash)throw std::runtime_error("Remote image hash");
                std::cout<<nlohmann::json{{"image_id",id},{"state","Available"},{"hash",hash}}.dump()<<'\n';return 0;
            }
            const auto attempt=store.start_upload(id);
            try {
                s3.put(key,image);
                if(inference::sha256(s3.get(key))!=hash)throw std::runtime_error("Remote image hash");
            }catch(...){if(attempt>=3)store.fail_image(id);throw;}
            if(store.image_state(id)=="Pending")store.available(id);
            std::cout<<nlohmann::json{{"image_id",id},{"state",store.image_state(id)},{"hash",hash}}.dump()<<'\n';
        } else if(operation==L"resume-uploads"){
            const char* access=std::getenv("VISION_S3_ACCESS"),*secret=std::getenv("VISION_S3_SECRET");
            if(!access||!secret)throw std::runtime_error("Missing S3 secret reference");
            unsigned completed=0,failed=0;
            for(const auto& job:store.pending_uploads()){
                if(job.attempt>=3){store.fail_image(job.image);++failed;continue;}
                std::vector<std::byte> image;
                try{image=store.image(job.image);}catch(...){store.fail_image(job.image);++failed;continue;}
                store.start_upload(job.image);
                try{
                    storage::S3 s3(job.endpoint,job.bucket,access,secret);s3.put(job.key,image);
                    if(inference::sha256(s3.get(job.key))!=inference::sha256(image))throw std::runtime_error("Remote hash");
                    store.available(job.image);++completed;
                }catch(...){if(job.attempt+1>=3)store.fail_image(job.image);++failed;}
            }
            std::cout<<nlohmann::json{{"completed",completed},{"failed",failed}}.dump()<<'\n';
            return failed?1:0;
        } else if(operation==L"recover")std::cout<<store.recover_interrupted()<<'\n';
        else return 2;
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
