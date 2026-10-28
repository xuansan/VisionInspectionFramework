#include "engine.hpp"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <mutex>
#include <set>
#include <stdexcept>

namespace vision::opencv_plugin {
using Json = nlohmann::json;
namespace {
void fields(const Json& j, std::initializer_list<const char*> allowed) {
    if(!j.is_object()) throw std::invalid_argument("Expected object");
    for(auto it=j.begin();it!=j.end();++it) {
        bool found=false;for(const auto* key:allowed)if(it.key()==key)found=true;
        if(!found)throw std::invalid_argument("Unknown config field: "+it.key());
    }
}
int integer(const Json& j,const char* key,int fallback,int low,int high) {
    if(!j.contains(key))return fallback;
    if(!j[key].is_number_integer())throw std::invalid_argument(key);
    const auto v=j[key].get<std::int64_t>();
    if(v<low||v>high)throw std::invalid_argument(key);
    return static_cast<int>(v);
}
double number(const Json& j,const char* key,double fallback,double low,double high) {
    const auto v=j.value(key,fallback);
    if(!std::isfinite(v)||v<low||v>high)throw std::invalid_argument(key);
    return v;
}
}
std::filesystem::path utf8_path(const std::string& value) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()),value.size()));
}
struct Engine::Impl {
    std::string mode;
    int threshold{}, kernel{}, min_area{}, max_area{}, min_count{}, max_count{};
    bool invert{};
    std::string morphology;
    std::optional<cv::Rect> roi;
    double min_width{},max_width{},min_height{},max_height{},score{};
    cv::Mat pattern;
};
Engine::~Engine()=default;
Engine::Engine(const Json& j,const std::filesystem::path& base):impl_(std::make_unique<Impl>()) {
    static std::once_flag configured;
    std::call_once(configured,[]{cv::setNumThreads(1);});
    fields(j,{"mode","roi","threshold","invert","morphology","kernel","min_area","max_area",
        "min_count","max_count","min_width","max_width","min_height","max_height","template","min_score"});
    auto& c=*impl_;c.mode=j.at("mode").get<std::string>();
    if(c.mode!="threshold"&&c.mode!="components"&&c.mode!="measure"&&c.mode!="template")
        throw std::invalid_argument("Unknown algorithm mode");
    c.threshold=integer(j,"threshold",128,0,255);c.invert=j.value("invert",false);
    c.kernel=integer(j,"kernel",3,1,31);if(c.kernel%2==0)throw std::invalid_argument("Odd kernel required");
    c.morphology=j.value("morphology",std::string("none"));
    if(c.morphology!="none"&&c.morphology!="open"&&c.morphology!="close")throw std::invalid_argument("Morphology");
    c.min_area=integer(j,"min_area",1,1,16777216);c.max_area=integer(j,"max_area",16777216,1,16777216);
    c.min_count=integer(j,"min_count",1,0,64);c.max_count=integer(j,"max_count",1,0,64);
    if(c.min_area>c.max_area||c.min_count>c.max_count)throw std::invalid_argument("Inverted limits");
    c.min_width=number(j,"min_width",0,0,4096);c.max_width=number(j,"max_width",4096,0,4096);
    c.min_height=number(j,"min_height",0,0,4096);c.max_height=number(j,"max_height",4096,0,4096);
    if(c.min_width>c.max_width||c.min_height>c.max_height)throw std::invalid_argument("Size limits");
    c.score=number(j,"min_score",0.8,0,1);
    if(j.contains("roi")) {
        const auto& r=j["roi"];fields(r,{"x","y","width","height"});
        c.roi=cv::Rect(integer(r,"x",-1,0,4095),integer(r,"y",-1,0,4095),
            integer(r,"width",-1,1,4096),integer(r,"height",-1,1,4096));
        if(c.roi->x<0||c.roi->y<0||c.roi->width<1||c.roi->height<1)throw std::invalid_argument("Incomplete ROI");
    }
    if(c.mode=="template") {
        auto file=utf8_path(j.at("template").get<std::string>());if(file.is_relative())file=base/file;
        const auto size=std::filesystem::file_size(file);
        if(!size||size>16*1024*1024)throw std::invalid_argument("Template file size");
        std::ifstream input(file,std::ios::binary);std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
        if(!input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())))throw std::invalid_argument("Template read");
        // Inspect supported header sizes before decode to bound decompression memory.
        if(bytes.size()<11||bytes[0]!='P'||bytes[1]!='5')throw std::invalid_argument("Template must be binary PGM");
        std::string text(reinterpret_cast<const char*>(bytes.data()),std::min<std::size_t>(bytes.size(),128));
        std::istringstream header(text);std::string magic;int w=0,h=0,max=0;header>>magic>>w>>h>>max;
        if(w<2||h<2||w>1024||h>1024||max!=255)throw std::invalid_argument("Template dimensions");
        c.pattern=cv::imdecode(bytes,cv::IMREAD_GRAYSCALE);
        if(c.pattern.empty()||c.pattern.cols!=w||c.pattern.rows!=h)throw std::invalid_argument("Template decode");
        cv::Scalar mean,dev;cv::meanStdDev(c.pattern,mean,dev);
        if(dev[0]<1e-6)throw std::invalid_argument("Constant template");
    }
}
Json Engine::run(std::span<const std::byte> pixels,const contracts::ImageLayout& layout,
                 const std::atomic<bool>& stopped,std::uint64_t budget_ns) const {
    const auto start=std::chrono::steady_clock::now();
    const auto checkpoint=[&] {
        if(stopped.load())throw std::runtime_error("Cancelled");
        if(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-start).count())>=budget_ns)throw std::runtime_error("Deadline");
    };
    checkpoint();
    contracts::validate_layout(layout,contracts::checked_add(layout.offset,layout.length));
    if(layout.width>4096||layout.height>4096||static_cast<std::uint64_t>(layout.width)*layout.height>4194304||
        layout.length>64*1024*1024||pixels.size()!=layout.length)
        throw std::invalid_argument("Image resource limit");
    const auto& c=*impl_;
    cv::Rect roi=c.roi.value_or(cv::Rect(0,0,static_cast<int>(layout.width),static_cast<int>(layout.height)));
    if(roi.x+roi.width>static_cast<int>(layout.width)||roi.y+roi.height>static_cast<int>(layout.height))
        throw std::invalid_argument("ROI outside image");
    const int channels=layout.format==contracts::PixelFormat::Mono8?1:3;
    // Mapping::read already slices at layout.offset. Clone Mono8 to keep shared memory immutable.
    const cv::Mat view(static_cast<int>(layout.height),static_cast<int>(layout.width),CV_MAKETYPE(CV_8U,channels),
        const_cast<std::byte*>(pixels.data()),static_cast<std::size_t>(layout.stride));
    cv::Mat gray;
    if(channels==1)gray=view(roi).clone();
    else cv::cvtColor(view(roi),gray,layout.format==contracts::PixelFormat::RGB8?cv::COLOR_RGB2GRAY:cv::COLOR_BGR2GRAY);
    checkpoint();
    Json result{{"execution_state","Succeeded"},{"quality","OK"},{"defects",Json::array()},
        {"measurements",Json::array()},{"elapsed_ns","0"}};
    auto measurement=[&](std::string name,double value,const char* unit) {
        result["measurements"].push_back({{"name",std::move(name)},{"value",value},{"unit",unit}});
    };
    auto defect=[&](const char* label,cv::Rect box,double score) {
        result["defects"].push_back({{"class_id",0},{"label",label},{"score",score},
            {"box_xyxy",{box.x,box.y,box.x+box.width,box.y+box.height}},{"coordinate_space","original_pixels"}});
    };
    if(c.mode=="template") {
        if(c.pattern.cols>gray.cols||c.pattern.rows>gray.rows)throw std::invalid_argument("Template exceeds ROI");
        const auto positions=static_cast<std::uint64_t>(gray.cols-c.pattern.cols+1)*(gray.rows-c.pattern.rows+1);
        if(positions*static_cast<std::uint64_t>(c.pattern.total())>500000000ULL)
            throw std::invalid_argument("Template work limit");
        cv::Mat scores;cv::matchTemplate(gray,c.pattern,scores,cv::TM_CCOEFF_NORMED);checkpoint();
        double score;cv::Point location;cv::minMaxLoc(scores,nullptr,&score,nullptr,&location);
        if(!std::isfinite(score))throw std::runtime_error("Invalid template score");
        measurement("match_score",score,"ratio");
        const bool found=score>=c.score;
        if(found) {measurement("match_x",roi.x+location.x,"px");measurement("match_y",roi.y+location.y,"px");}
        else {result["quality"]="NG";defect("template_not_found",roi,1.0);}
    } else {
        cv::Mat binary;cv::threshold(gray,binary,c.threshold,255,c.invert?cv::THRESH_BINARY_INV:cv::THRESH_BINARY);
        if(c.morphology!="none")cv::morphologyEx(binary,binary,c.morphology=="open"?cv::MORPH_OPEN:cv::MORPH_CLOSE,
            cv::getStructuringElement(cv::MORPH_RECT,{c.kernel,c.kernel}));
        checkpoint();
        measurement("foreground_pixels",cv::countNonZero(binary),"px2");
        cv::Mat labels,stats,centers;const int count=cv::connectedComponentsWithStats(binary,labels,stats,centers,8,CV_32S);
        checkpoint();
        std::vector<int> selected;
        for(int i=1;i<count;++i)if(stats.at<int>(i,cv::CC_STAT_AREA)>=c.min_area&&stats.at<int>(i,cv::CC_STAT_AREA)<=c.max_area) {
            if(selected.size()==50)throw std::runtime_error("Component evidence limit");
            selected.push_back(i);
        }
        measurement("component_count",static_cast<double>(selected.size()),"count");
        bool ok=selected.size()>=static_cast<std::size_t>(c.min_count)&&selected.size()<=static_cast<std::size_t>(c.max_count);
        for(std::size_t index=0;index<selected.size();++index) {
            const auto i=selected[index];
            cv::Rect box(roi.x+stats.at<int>(i,cv::CC_STAT_LEFT),roi.y+stats.at<int>(i,cv::CC_STAT_TOP),
                stats.at<int>(i,cv::CC_STAT_WIDTH),stats.at<int>(i,cv::CC_STAT_HEIGHT));
            const auto prefix="component_"+std::to_string(index)+"_";
            measurement(prefix+"area",stats.at<int>(i,cv::CC_STAT_AREA),"px2");
            measurement(prefix+"x",roi.x+centers.at<double>(i,0),"px");
            measurement(prefix+"y",roi.y+centers.at<double>(i,1),"px");
            measurement(prefix+"width",box.width,"px");measurement(prefix+"height",box.height,"px");
            if(c.mode=="measure"&&(box.width<c.min_width||box.width>c.max_width||box.height<c.min_height||box.height>c.max_height)) {
                ok=false;defect("size_out_of_range",box,1.0);
            }
        }
        if(!ok) {result["quality"]="NG";if(result["defects"].empty())defect("component_count_out_of_range",roi,1.0);}
    }
    checkpoint();
    result["elapsed_ns"]=std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-start).count());
    return result;
}
}
