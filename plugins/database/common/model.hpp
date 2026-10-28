#pragma once
#include "../../output-common/async.hpp"
#include <vision/storage/store.hpp>
#include <vision/inference/engine.hpp>
#include <cstdlib>
#include <chrono>
#include <vector>
namespace vision::database {
using J=nlohmann::json;
struct Error:std::runtime_error {explicit Error(const std::string& code):std::runtime_error(code){}};
struct Config {
    std::string host,database,user,password,tls,ca;
    std::filesystem::path archive;
    unsigned port{},timeout{};
    static Config parse(const J& j) {
        const std::vector<std::string> keys={"host","port","database","username_env","password_env","timeout_ms","tls","ca_file","archive_root"};
        if(!j.is_object()||j.size()!=keys.size())throw Error("DB.CONFIG");
        for(const auto& key:keys)if(!j.contains(key))throw Error("DB.CONFIG");
        auto text=[&](const char* field,std::size_t max,bool empty=false) {
            auto s=j.at(field).get<std::string>();
            if((!empty&&s.empty())||s.size()>max||s.find('\0')!=std::string::npos)throw Error("DB.CONFIG");
            return s;
        };
        auto integer=[&](const char* field,int minimum,int maximum) {
            const auto& v=j.at(field);
            if(!v.is_number_integer()||v.get<std::int64_t>()<minimum||v.get<std::int64_t>()>maximum)throw Error("DB.CONFIG");
            return v.get<unsigned>();
        };
        auto secret=[&](const char* field) {
            const auto key=text(field,128);
            for(unsigned char c:key)if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'))throw Error("DB.CONFIG");
            const auto* p=std::getenv(key.c_str());
            if(!p||!*p||std::strlen(p)>256)throw Error("DB.CREDENTIAL");
            return std::string(p);
        };
        Config c;c.host=text("host",253);c.database=text("database",63);
        for(const auto* value:{&c.host,&c.database})for(unsigned char ch:*value)
            if(!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='_'||ch=='-'||ch=='.'))
                throw Error("DB.CONFIG");
        c.port=integer("port",1,65535);c.timeout=integer("timeout_ms",100,5000);
        c.user=secret("username_env");c.password=secret("password_env");
        c.tls=text("tls",32);c.ca=text("ca_file",4096,true);
        if(c.tls=="disable-loopback") {
            if(c.host!="127.0.0.1"||!c.ca.empty())throw Error("DB.TLS_REQUIRED");
        }else if(c.tls=="verify-full") {
            if(c.ca.empty()||!std::filesystem::is_regular_file(inference::utf8_path(c.ca)))throw Error("DB.CA");
        }else throw Error("DB.CONFIG");
        const auto root=text("archive_root",4096,true);
        if(!root.empty()) {
            c.archive=inference::utf8_path(root);
            if(!std::filesystem::is_directory(c.archive)||std::filesystem::is_symlink(c.archive))throw Error("DB.ARCHIVE");
        }
        return c;
    }
};
class Deadline {
public:
    Deadline(const Config& c,const J& request,const std::atomic<bool>& stopped):stopped_(stopped) {
        const auto budget=contracts::parse_u64(request.at("budget_ns").get<std::string>());
        if(!budget)throw Error("DB.DEADLINE");
        end_=std::chrono::steady_clock::now()+std::chrono::nanoseconds(std::min<std::uint64_t>(budget,c.timeout*1000000ULL));
    }
    void check() const {
        if(stopped_)throw Error("DB.CANCELLED");
        if(std::chrono::steady_clock::now()>=end_)throw Error("DB.TIMEOUT");
    }
private:
    const std::atomic<bool>& stopped_;std::chrono::steady_clock::time_point end_;
};
struct Rows {std::vector<std::vector<std::string>> values;std::uint64_t affected{};};
struct Image {std::string id,channel,hash,local,descriptor;};
inline std::vector<Image> images(const Config& config,const contracts::ResultEnvelope& event) {
    std::vector<Image> out;
    if(config.archive.empty())return out;
    if(storage::find_result(config.archive,event.event_id.value())!=serialization::encode_result(event))throw Error("DB.ARCHIVE_RESULT");
    storage::Store store(config.archive);
    const auto frames=store.frames(event.result.run_id.value(),event.result.inspection_id.value());
    if(frames.size()>4)throw Error("DB.ARCHIVE_COUNT");
    for(const auto& frame:frames) {
        const auto info=storage::find_image(config.archive,frame.image);
        const auto path=(config.archive/(frame.image+".stage")).u8string();
        out.push_back({frame.image,frame.channel,info.hash,std::string(reinterpret_cast<const char*>(path.data()),path.size()),frame.descriptor});
    }
    return out;
}
// Both adapters expose bound parameters and buffered, bounded result rows.
template<class DB> void write_event(DB& db,const J& request,const Config& config) {
    const auto event=serialization::decode_result(request.at("event").dump());
    const auto body=J::parse(serialization::encode_result(event)).dump();
    if(body.size()>32768)throw Error("DB.PAYLOAD_LIMIT");
    const auto hash=inference::sha256(std::as_bytes(std::span(body.data(),body.size())));
    const auto& r=request.at("event").at("result");
    db.query("BEGIN");
    const auto version=db.query("SELECT version FROM vision_schema_version WHERE id=1");
    if(version.values.size()!=1||version.values[0].size()!=1||version.values[0][0]!="1")throw Error("DB.SCHEMA_VERSION");
    const std::string insert="INSERT INTO vision_results(event_id,run_id,inspection_id,station_id,workpiece_id,recipe_hash,quality,execution_state,sequence_no,emitted_at_unix_ns,result_sha256,result_json) VALUES(";
    std::string placeholders;
    for(unsigned n=1;n<=12;++n){if(n>1)placeholders+=",";placeholders+=db.param(n);}
    const std::vector<std::string> params={event.event_id.value(),r.at("run_id"),r.at("inspection_id"),r.at("station_id"),
        r.at("workpiece_id"),r.at("recipe_hash"),r.at("quality"),r.at("execution_state"),request.at("event").at("sequence"),
        request.at("event").at("emitted_at_unix_ns"),hash,body};
    const auto added=db.query(insert+placeholders+")"+db.insert_suffix(),params);
    const auto found=db.query("SELECT result_sha256,result_json FROM vision_results WHERE event_id="+db.param(1)+" FOR UPDATE",{event.event_id.value()});
    if(found.values.size()!=1||found.values[0].size()!=2||found.values[0][0]!=hash||found.values[0][1]!=body)throw Error("DB.EVENT_CONFLICT");
    if(db.inserted(added)) {
        for(const auto& check:r.at("checks")) {
            unsigned index=0;
            for(const auto& defect:check.at("defects")) {
                const auto& box=defect.at("box_xyxy");
                std::vector<std::string> p={event.event_id.value(),check.at("correlation").at("check_id"),
                    std::to_string(index++),defect.at("class_id").dump(),defect.at("label"),defect.at("score").dump(),
                    box[0].dump(),box[1].dump(),box[2].dump(),box[3].dump()};
                std::string sql="INSERT INTO vision_defects(event_id,check_id,defect_index,class_id,label,score,x1,y1,x2,y2) VALUES(";
                for(unsigned n=1;n<=10;++n){if(n>1)sql+=",";sql+=db.param(n);}db.query(sql+")",p);
            }
        }
        for(const auto& image:images(config,event))
            db.query("INSERT INTO vision_images(event_id,image_id,channel,image_sha256,local_reference,descriptor_json) VALUES("+
                db.param(1)+","+db.param(2)+","+db.param(3)+","+db.param(4)+","+db.param(5)+","+db.param(6)+")",
                {event.event_id.value(),image.id,image.channel,image.hash,image.local,image.descriptor});
    }
    db.query("COMMIT");
}
}
