#include <vision/storage/store.hpp>
#include <vision/frame_transport/mapping.hpp>
#include <vision/serialization/json_codec.hpp>
#include <vision/inference/engine.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
using namespace vision;
using J=nlohmann::json;
int main() {
    try {
        std::string input;char ch;
        while(std::cin.get(ch)){if(input.size()>=65536)throw std::runtime_error("Archive input limit");input+=ch;}
        const auto config=J::parse(input);
        if(config.at("frames").size()!=2)throw std::runtime_error("Archive frame count");
        storage::Store store(inference::utf8_path(config.at("root").get<std::string>()));
        J images=J::array();
        for(const auto& item:config.at("frames")){
            const auto slot_bytes=contracts::parse_u64(item.at("slot_bytes").get<std::string>());
            if(slot_bytes>16*1024*1024)throw std::runtime_error("Archive slot limit");
            const auto frame=serialization::decode_frame(item.at("frame").dump(),slot_bytes);
            if(frame.permit.owner.worker.value()!="frame-archive"||frame.permit.run.value()!=config.at("run").get<std::string>())throw std::runtime_error("Archive identity");
            auto mapping=frame_transport::Mapping::open({frame.permit.run,frame.permit.pool,item.at("slot_count").get<unsigned>(),slot_bytes},frame_transport::Access::Reader);
            std::vector<std::byte> pixels;
            if(mapping->read(frame,0,[&](auto bytes){pixels.assign(bytes.begin(),bytes.end());})!=frame_transport::MemoryStatus::Ok)throw std::runtime_error("Archive mapping");
            mapping.reset(); // No filesystem or SQL IO while holding the shared slot read lock.
            const auto id=store.archive_frame(frame,slot_bytes,config.at("inspection").get<std::string>(),item.at("channel").get<std::string>(),config.at("recipe_hash").get<std::string>(),config.at("recipe_body").get<std::string>(),pixels);
            images.push_back(id);
        }
        std::cout<<J{{"run",config.at("run")},{"inspection",config.at("inspection")},{"images",images},{"state","LocallyStaged"}}.dump()<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<e.what();return 1;}
}
