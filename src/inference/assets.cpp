#include <vision/inference/engine.hpp>
#include <fstream>
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <set>
namespace vision::inference {
std::filesystem::path utf8_path(std::string_view text) {
    if(text.empty()||text.size()>32768||text.find('\0')!=std::string_view::npos)throw std::invalid_argument("Path");
    const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(!count)throw std::invalid_argument("UTF8 path");
    std::wstring wide(static_cast<std::size_t>(count),L'\0');
    if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),wide.data(),count)!=count)throw std::invalid_argument("UTF8 path");
    return std::filesystem::path(wide);
}
std::string sha256(std::span<const std::byte> bytes) {
    if(bytes.size()>ULONG_MAX)throw std::invalid_argument("Hash input");
    std::uint8_t digest[32]{};
    if(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
        static_cast<ULONG>(bytes.size()),digest,32)<0)throw std::runtime_error("SHA256");
    constexpr char hex[]="0123456789abcdef";std::string result="sha256:";
    for(auto value:digest){result+=hex[value>>4];result+=hex[value&15];}return result;
}
std::vector<std::byte> read_file(const std::filesystem::path& path,std::size_t limit) {
    const auto size=std::filesystem::file_size(path);
    if(!size||size>limit)throw std::invalid_argument("Asset file limit");
    std::ifstream file(path,std::ios::binary);std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if(!file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size))||file.peek()!=EOF)
        throw std::runtime_error("Asset changed/unreadable");
    return bytes;
}
Config read_config(const std::filesystem::path& path) {
    using J=nlohmann::json;
    const auto bytes=read_file(path,65536);std::vector<std::set<std::string>> keys;
    const auto p=J::parse(reinterpret_cast<const char*>(bytes.data()),reinterpret_cast<const char*>(bytes.data()+bytes.size()),
        [&](int depth,J::parse_event_t event,J& value) {
            if(depth>8)throw std::invalid_argument("Model config depth");
            if(event==J::parse_event_t::object_start)keys.emplace_back();
            if(event==J::parse_event_t::key&&!keys.back().insert(value.get<std::string>()).second)throw std::invalid_argument("Duplicate model config");
            if(event==J::parse_event_t::object_end)keys.pop_back();return true;
        });
    const std::set<std::string> allowed{"schema_version","model","model_hash","type","input","output","prototypes","labels","width","height",
        "max_results","score","iou","roi","scores","ok_label"};
    if(!p.is_object())throw std::invalid_argument("Model config");
    for(auto it=p.begin();it!=p.end();++it)if(!allowed.contains(it.key()))throw std::invalid_argument("Model config field");
    if(p.at("schema_version")!=1)throw std::invalid_argument("Model config version");
    Config c;c.model=path.parent_path()/utf8_path(p.at("model").get<std::string>());
    c.model_hash=p.at("model_hash");c.type=p.at("type");c.labels=p.at("labels").get<std::vector<std::string>>();
    auto integer=[](const J& v,unsigned max) {
        if(!v.is_number_integer()||v.get<std::int64_t>()<0||v.get<std::uint64_t>()>max)throw std::invalid_argument("Config integer");
        return v.get<unsigned>();
    };
    c.width=integer(p.at("width"),2048);c.height=integer(p.at("height"),2048);
    c.max_results=integer(p.at("max_results"),64);
    c.score=p.at("score").get<double>();c.iou=p.at("iou").get<double>();
    c.input=p.at("input");c.output=p.at("output");
    if(p.contains("prototypes"))c.prototypes=p.at("prototypes");
    if(p.contains("scores"))c.scores=p.at("scores");
    if(p.contains("ok_label"))c.ok_label=p.at("ok_label");
    if(p.contains("roi")) {
        if(!p["roi"].is_array()||p["roi"].size()!=4)throw std::invalid_argument("ROI");
        c.roi_x=integer(p["roi"][0],16384);c.roi_y=integer(p["roi"][1],16384);
        c.roi_width=integer(p["roi"][2],16384);c.roi_height=integer(p["roi"][3],16384);
    }
    return c;
}
std::vector<std::byte> load_mask(const std::filesystem::path& root,const contracts::MaskReference& ref) {
    const auto size=static_cast<std::uint64_t>(ref.width)*ref.height;
    if(!contracts::valid_hash(ref.hash)||!size||size>1024*1024)throw std::invalid_argument("Mask reference");
    const auto path=root/(ref.hash.substr(7)+".mask");
    if(std::filesystem::is_symlink(path))throw std::invalid_argument("Mask symlink");
    auto bytes=read_file(path,1024*1024);
    if(bytes.size()!=size||sha256(bytes)!=ref.hash)throw std::runtime_error("Mask integrity");
    return bytes;
}
std::vector<contracts::MaskReference> store_masks(const std::filesystem::path& root,const std::vector<Mask>& masks) {
    if(masks.size()>64||root.empty()||!std::filesystem::is_directory(root)||std::filesystem::is_symlink(root))
        throw std::invalid_argument("Mask store");
    struct Handle {HANDLE h;~Handle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}};
    // Exclusive, nonblocking cross-process ownership of quota accounting and writes.
    Handle lock{CreateFileW((root/L".mask-lock").c_str(),GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr)};
    if(lock.h==INVALID_HANDLE_VALUE)throw std::runtime_error("Mask store busy");
    std::uint64_t used=0,total=0;unsigned files=0;
    for(const auto& entry:std::filesystem::directory_iterator(root)) {
        if(++files>4096||entry.is_symlink()||!entry.is_regular_file())throw std::runtime_error("Mask store entries");
        used+=entry.file_size();if(used>64*1024*1024)throw std::runtime_error("Mask store capacity");
    }
    std::vector<contracts::MaskReference> refs;
    for(const auto& mask:masks) {
        const auto size=static_cast<std::uint64_t>(mask.width)*mask.height;total+=size;
        if(!size||size>1024*1024||total>16*1024*1024||mask.pixels.size()!=size)throw std::invalid_argument("Mask bytes");
        const auto bytes=std::as_bytes(std::span(mask.pixels));const auto hash=sha256(bytes);
        contracts::MaskReference ref{hash,mask.width,mask.height,static_cast<unsigned>(refs.size())};
        const auto path=root/(hash.substr(7)+".mask");
        if(std::filesystem::exists(path)) {(void)load_mask(root,ref);refs.push_back(ref);continue;}
        if(size>64*1024*1024-used||++files>4096)throw std::runtime_error("Mask store capacity");
        Handle file{CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};
        if(file.h==INVALID_HANDLE_VALUE)throw std::runtime_error("Mask create");
        DWORD written=0;
        if(!WriteFile(file.h,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)||written!=bytes.size()) {
            CloseHandle(file.h);file.h=INVALID_HANDLE_VALUE;DeleteFileW(path.c_str());throw std::runtime_error("Mask write");
        }
        used+=size;refs.push_back(ref);
    }
    return refs;
}
}
