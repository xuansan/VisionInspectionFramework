#include <vision/replay/assets.hpp>
#include <vision/inference/engine.hpp>
#include <vision/capture/source.hpp>
#include <nlohmann/json.hpp>
#include <set>
#define NOMINMAX
#include <windows.h>
namespace vision::replay {
namespace {
using J=nlohmann::json;
auto raw(const std::string& value){return std::as_bytes(std::span(value.data(),value.size()));}
J parse(std::span<const std::byte> bytes) {
    std::vector<std::set<std::string>> keys;
    return J::parse(reinterpret_cast<const char*>(bytes.data()),reinterpret_cast<const char*>(bytes.data()+bytes.size()),
        [&](int depth,J::parse_event_t event,J& value) {
            if(depth>8)throw std::invalid_argument("Snapshot depth");
            if(event==J::parse_event_t::object_start)keys.emplace_back();
            if(event==J::parse_event_t::key&&!keys.back().insert(value.get<std::string>()).second)throw std::invalid_argument("Snapshot duplicate key");
            if(event==J::parse_event_t::object_end)keys.pop_back();return true;
        });
}
void write_new(const std::filesystem::path& path,std::span<const std::byte> bytes) {
    const auto file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Snapshot create");
    DWORD written=0;const auto ok=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr);
    CloseHandle(file);
    if(!ok||written!=bytes.size())throw std::runtime_error("Snapshot partial write");
}
auto verified(const std::filesystem::path& root,const char* name,const J& hash,std::size_t limit) {
    if(std::filesystem::is_symlink(root/name))throw std::invalid_argument("Snapshot symlink");
    auto data=inference::read_file(root/name,limit);
    if(inference::sha256(data)!=hash.get<std::string>())throw std::runtime_error("Snapshot asset hash mismatch");
    return data;
}
}
Snapshot load_snapshot(const std::filesystem::path& manifest,std::string_view expected_hash) {
    if(!contracts::valid_hash(expected_hash)||std::filesystem::is_symlink(manifest)||std::filesystem::is_symlink(manifest.parent_path()))
        throw std::invalid_argument("Snapshot identity");
    const auto bytes=inference::read_file(manifest,65536);
    if(inference::sha256(bytes)!=expected_hash)throw std::runtime_error("Snapshot manifest hash mismatch");
    const auto m=parse(bytes);
    const std::set<std::string> required{"schema_version","mode","runtime","preprocess","model_hash","config_hash","image_hash","pixel_hash","width","height","channels"};
    if(!m.is_object()||m.size()!=required.size())throw std::invalid_argument("Snapshot fields");
    for(auto it=m.begin();it!=m.end();++it)if(!required.contains(it.key()))throw std::invalid_argument("Snapshot field");
    if(m["schema_version"]!=1||m["mode"]!="Replay"||m["runtime"]!="ort-cpu-1.30.0"||m["preprocess"]!="nearest-letterbox-rgb-nchw-v1")
        throw std::invalid_argument("Snapshot version");
    const auto root=manifest.parent_path();
    (void)verified(root,"model.onnx",m["model_hash"],256*1024*1024);
    const auto config=verified(root,"config.json",m["config_hash"],65536);
    const auto j=parse(config);
    if(j.at("model")!="model.onnx"||j.at("model_hash")!=m["model_hash"])throw std::invalid_argument("Snapshot model reference");
    (void)inference::read_config(root/"config.json");
    const auto image=verified(root,"image.pnm",m["image_hash"],16*1024*1024+4096);
    const auto decoded=capture::decode_pnm(image);
    for(const char* field:{"width","height","channels"})
        if(!m[field].is_number_unsigned())throw std::invalid_argument("Snapshot dimension type");
    if(inference::sha256(decoded.pixels)!=m["pixel_hash"].get<std::string>()||decoded.layout.width!=m["width"].get<std::uint64_t>()||
       decoded.layout.height!=m["height"].get<std::uint64_t>()||static_cast<unsigned>(decoded.layout.format==contracts::PixelFormat::Mono8?1:3)!=m["channels"].get<std::uint64_t>())
        throw std::invalid_argument("Snapshot image identity");
    return {manifest,root/"config.json",root/"image.pnm",std::string(expected_hash),m["pixel_hash"].get<std::string>(),decoded.layout};
}
Snapshot create_snapshot(const std::filesystem::path& config_path,const std::filesystem::path& image_path,const std::filesystem::path& root) {
    if(!std::filesystem::is_directory(root)||std::filesystem::is_symlink(root))throw std::invalid_argument("Snapshot root");
    const auto config=inference::read_config(config_path);
    auto document=parse(inference::read_file(config_path,65536));
    const auto model=inference::read_file(config.model,256*1024*1024);
    if(inference::sha256(model)!=config.model_hash)throw std::invalid_argument("Snapshot model hash");
    document["model"]="model.onnx";const auto config_bytes=document.dump();
    const auto image=inference::read_file(image_path,16*1024*1024+4096);const auto decoded=capture::decode_pnm(image);
    const J m{{"schema_version",1},{"mode","Replay"},{"runtime","ort-cpu-1.30.0"},{"preprocess","nearest-letterbox-rgb-nchw-v1"},
        {"model_hash",config.model_hash},{"config_hash",inference::sha256(raw(config_bytes))},{"image_hash",inference::sha256(image)},
        {"pixel_hash",inference::sha256(decoded.pixels)},{"width",decoded.layout.width},{"height",decoded.layout.height},
        {"channels",decoded.layout.format==contracts::PixelFormat::Mono8?1:3}};
    const auto bytes=m.dump(),hash=inference::sha256(raw(bytes));const auto directory=root/hash.substr(7);
    if(std::filesystem::exists(directory))return load_snapshot(directory/"manifest.json",hash);
    if(!std::filesystem::create_directory(directory))throw std::runtime_error("Snapshot directory create");
    write_new(directory/"model.onnx",model);write_new(directory/"config.json",raw(config_bytes));
    write_new(directory/"image.pnm",image);write_new(directory/"manifest.json",raw(bytes)); // Commit marker last.
    return load_snapshot(directory/"manifest.json",hash);
}
}
