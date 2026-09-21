#include <vision/inference/engine.hpp>
#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <set>
namespace vision::inference {
namespace {
std::size_t elements(const std::vector<std::int64_t>& shape,std::size_t limit) {
    std::size_t count=1;
    for(auto d:shape) {if(d<=0||static_cast<std::uint64_t>(d)>limit/count)throw std::invalid_argument("Static tensor bounds");count*=static_cast<std::size_t>(d);}
    return count;
}
double overlap(const contracts::Defect& a,const contracts::Defect& b) {
    const auto area=[](const auto& d){return (d.x2-d.x1)*(d.y2-d.y1);};
    const auto intersection=std::max(0.,std::min(a.x2,b.x2)-std::max(a.x1,b.x1))*std::max(0.,std::min(a.y2,b.y2)-std::max(a.y1,b.y1));
    return intersection/(area(a)+area(b)-intersection);
}
}
struct Engine::Impl {
    Config config;
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING,"vision-inference"};
    Ort::SessionOptions options;
    Ort::RunOptions run_options;
    std::unique_ptr<Ort::Session> session;
    std::vector<std::int64_t> shape,out_shape,proto_shape;
    std::atomic<bool> cancelled{};
    explicit Impl(Config c):config(std::move(c)) {
        if(std::string_view(Ort::GetVersionString())!="1.30.0")throw std::invalid_argument("Unverified ORT version");
        if((config.type!="detection"&&config.type!="classification"&&config.type!="instance_segmentation")||!config.width||!config.height||config.width>2048||config.height>2048||
           !config.max_results||config.max_results>64||config.labels.empty()||config.labels.size()>256||
           !contracts::valid_hash(config.model_hash)||!std::isfinite(config.score)||config.score<0||config.score>1||
           !std::isfinite(config.iou)||config.iou<0||config.iou>1)throw std::invalid_argument("Inference config");
        std::set<std::string> labels;
        for(const auto& label:config.labels)if(label.empty()||label.size()>128||!labels.insert(label).second)throw std::invalid_argument("Labels");
        if(config.type=="classification")for(const auto& label:config.labels)
            if(label.size()>122)throw std::invalid_argument("Classification measurement label length");
        if(config.type=="classification"&&((config.scores!="logits"&&config.scores!="probabilities")||
           !labels.contains(config.ok_label)))throw std::invalid_argument("Classification policy");
        auto bytes=read_file(config.model,256*1024*1024);
        if(sha256(bytes)!=config.model_hash)throw std::invalid_argument("Model hash mismatch");
        options.SetIntraOpNumThreads(1);options.SetInterOpNumThreads(1);
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);
        session=std::make_unique<Ort::Session>(env,bytes.data(),bytes.size(),options);
        const bool segmentation=config.type=="instance_segmentation";
        if(session->GetInputCount()!=1||session->GetOutputCount()!=(segmentation?2U:1U))throw std::invalid_argument("Model IO count");
        Ort::AllocatorWithDefaultOptions allocator;
        if(session->GetInputNameAllocated(0,allocator).get()!=config.input||
           session->GetOutputNameAllocated(0,allocator).get()!=config.output)throw std::invalid_argument("Model IO name");
        auto input_type=session->GetInputTypeInfo(0);auto info=input_type.GetTensorTypeAndShapeInfo();
        shape=info.GetShape();
        if(info.GetElementType()!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT||
           shape!=std::vector<std::int64_t>{1,3,config.height,config.width})throw std::invalid_argument("Input NCHW float32 shape");
        auto output_type=session->GetOutputTypeInfo(0);auto output=output_type.GetTensorTypeAndShapeInfo();out_shape=output.GetShape();
        if(segmentation) {
            if(session->GetOutputNameAllocated(1,allocator).get()!=config.prototypes)throw std::invalid_argument("Prototype name");
            auto type=session->GetOutputTypeInfo(1);auto prototype=type.GetTensorTypeAndShapeInfo();proto_shape=prototype.GetShape();
            if(prototype.GetElementType()!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT||proto_shape.size()!=4||
               proto_shape[0]!=1||proto_shape[1]>32||proto_shape[2]>256||proto_shape[3]>256)
                throw std::invalid_argument("Prototype shape");
            elements(proto_shape,32*256*256);
        }
        const bool valid_shape=config.type=="classification"?
            out_shape==std::vector<std::int64_t>{1,static_cast<std::int64_t>(config.labels.size())}:
            (out_shape.size()==3&&out_shape[0]==1&&out_shape[1]==static_cast<std::int64_t>(4+config.labels.size())+(segmentation?proto_shape[1]:0)&&out_shape[2]<=4096);
        if(output.GetElementType()!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT||!valid_shape)
            throw std::invalid_argument("Detection output shape");
        elements(shape,3*2048*2048);elements(out_shape,2*1024*1024);
        std::vector<float> zeros(elements(shape,3*2048*2048),0);
        (void)execute(zeros); // Warmup completes before plugin Ready.
    }
    std::vector<Ort::Value> execute(std::vector<float>& input) {
        if(cancelled)throw std::runtime_error("Cancelled");
        auto memory=Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
        auto tensor=Ort::Value::CreateTensor<float>(memory,input.data(),input.size(),shape.data(),shape.size());
        const char* inputs[]={config.input.c_str()};const char* outputs[]={config.output.c_str(),config.prototypes.c_str()};
        const auto output_count=proto_shape.empty()?1U:2U;
        auto values=session->Run(run_options,inputs,&tensor,1,outputs,output_count);
        if(cancelled||values.size()!=output_count)throw std::runtime_error("Output contract");
        for(std::size_t n=0;n<values.size();++n) {
            const auto& expected=n?proto_shape:out_shape;
            if(values[n].GetTensorTypeAndShapeInfo().GetShape()!=expected)throw std::runtime_error("Output shape");
            const auto count=elements(expected,2*1024*1024);const auto data=values[n].GetTensorData<float>();
            for(std::size_t i=0;i<count;++i)if(!std::isfinite(data[i]))throw std::runtime_error("Nonfinite model output");
        }
        return values;
    }
};
Engine::Engine(Config c):impl_(std::make_unique<Impl>(std::move(c))) {}
Engine::~Engine()=default;
const Config& Engine::config() const {return impl_->config;}
void Engine::cancel() noexcept {impl_->cancelled=true;try{impl_->run_options.SetTerminate();}catch(...) {}}
Output Engine::run(std::span<const std::byte> bytes,const contracts::ImageLayout& layout) {
    const auto& c=impl_->config;contracts::validate_layout(layout,contracts::checked_add(layout.offset,layout.length));
    if(bytes.size()!=layout.length||bytes.size()>16*1024*1024)throw std::invalid_argument("Image buffer");
    const auto rx=c.roi_x,ry=c.roi_y,rw=c.roi_width?c.roi_width:layout.width,rh=c.roi_height?c.roi_height:layout.height;
    if(rx>=layout.width||ry>=layout.height||!rw||!rh||rw>layout.width-rx||rh>layout.height-ry)throw std::invalid_argument("ROI image bounds");
    const auto scale=std::min(static_cast<double>(c.width)/rw,static_cast<double>(c.height)/rh);
    const auto width=std::max(1U,static_cast<unsigned>(std::round(rw*scale))),height=std::max(1U,static_cast<unsigned>(std::round(rh*scale)));
    const auto left=(c.width-width)/2,top=(c.height-height)/2;
    const auto sx=static_cast<double>(width)/rw,sy=static_cast<double>(height)/rh;
    const auto plane=static_cast<std::size_t>(c.width)*c.height;
    std::vector<float> input(3*plane,114.f/255);
    const unsigned channels=layout.format==contracts::PixelFormat::Mono8?1:3;
    for(unsigned y=0;y<height;++y) {
        if(impl_->cancelled)throw std::runtime_error("Cancelled");
        const auto py=ry+std::min(rh-1,static_cast<unsigned>(y/sy));
        for(unsigned x=0;x<width;++x) {
            const auto px=rx+std::min(rw-1,static_cast<unsigned>(x/sx));
            for(unsigned ch=0;ch<3;++ch) {
                const unsigned source=channels==1?0:layout.format==contracts::PixelFormat::BGR8?2-ch:ch;
                input[ch*plane+(y+top)*c.width+x+left]=std::to_integer<unsigned>(bytes[py*layout.stride+px*channels+source])/255.f;
            }
        }
    }
    auto values=impl_->execute(input);const auto data=values[0].GetTensorData<float>();
    if(c.type=="classification") {
        Output result;result.model_hash=c.model_hash;
        std::vector<double> scores(data,data+c.labels.size());
        double sum=0;
        if(c.scores=="logits") {
            const auto largest=*std::max_element(scores.begin(),scores.end());
            for(auto& score:scores){score=std::exp(score-largest);sum+=score;}
            for(auto& score:scores)score/=sum;
        } else {
            for(auto score:scores){if(score<0||score>1)throw std::runtime_error("Classification probability");sum+=score;}
            if(std::abs(sum-1)>0.0001)throw std::runtime_error("Probability sum");
        }
        std::vector<std::size_t> order(scores.size());
        for(std::size_t i=0;i<order.size();++i)order[i]=i;
        std::stable_sort(order.begin(),order.end(),[&](auto a,auto b){return scores[a]>scores[b];});
        if(scores[order.front()]>=c.score)result.classification=c.labels[order.front()];
        for(std::size_t i=0;i<std::min(order.size(),static_cast<std::size_t>(c.max_results));++i)
            result.measurements.push_back({"class."+c.labels[order[i]],scores[order[i]],"probability"});
        return result;
    }
    const auto count=static_cast<std::size_t>(impl_->out_shape[2]);
    struct Candidate {contracts::Defect defect;std::size_t index;};
    std::vector<Candidate> candidates;Output result;result.model_hash=c.model_hash;
    for(std::size_t i=0;i<count;++i) {
        std::size_t best=0;float score=data[4*count+i];
        for(std::size_t label=0;label<c.labels.size();++label) {
            const auto value=data[(4+label)*count+i];if(value<0||value>1)throw std::runtime_error("Class probability");
            if(value>score) {score=value;best=label;}
        }
        if(score<c.score)continue;
        const auto cx=data[i],cy=data[count+i],w=data[2*count+i],h=data[3*count+i];
        if(w<=0||h<=0)throw std::runtime_error("Detection size");
        const auto x1=std::clamp((cx-w/2.-left)/sx,0.,static_cast<double>(rw))+rx;
        const auto y1=std::clamp((cy-h/2.-top)/sy,0.,static_cast<double>(rh))+ry;
        const auto x2=std::clamp((cx+w/2.-left)/sx,0.,static_cast<double>(rw))+rx;
        const auto y2=std::clamp((cy+h/2.-top)/sy,0.,static_cast<double>(rh))+ry;
        if(x2>x1&&y2>y1)candidates.push_back({{static_cast<std::int32_t>(best),c.labels[best],score,x1,y1,x2,y2},i});
    }
    std::stable_sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return a.defect.score>b.defect.score;});
    std::vector<std::size_t> selected_indices;
    for(const auto& candidate:candidates) {
        bool suppressed=false;for(const auto& selected:result.detections)
            if(candidate.defect.class_id==selected.class_id&&overlap(candidate.defect,selected)>c.iou){suppressed=true;break;}
        if(!suppressed){result.detections.push_back(candidate.defect);selected_indices.push_back(candidate.index);}
        if(result.detections.size()==c.max_results)break;
    }
    if(c.type=="instance_segmentation") {
        const auto mask_bytes=static_cast<std::uint64_t>(layout.width)*layout.height;
        if(mask_bytes>1024*1024||mask_bytes*selected_indices.size()>16*1024*1024)throw std::runtime_error("Mask capacity");
        const auto& ps=impl_->proto_shape;const auto pw=static_cast<unsigned>(ps[3]),ph=static_cast<unsigned>(ps[2]);
        const auto proto=values[1].GetTensorData<float>();
        for(std::size_t n=0;n<selected_indices.size();++n) {
            Mask mask{layout.width,layout.height,std::vector<std::uint8_t>(static_cast<std::size_t>(mask_bytes),0)};
            const auto& box=result.detections[n];
            for(unsigned y=0;y<layout.height;++y) {
                if(impl_->cancelled)throw std::runtime_error("Cancelled");
                for(unsigned x=0;x<layout.width;++x) {
                    if(x+.5<box.x1||x+.5>=box.x2||y+.5<box.y1||y+.5>=box.y2)continue;
                    const auto px=std::min(pw-1,static_cast<unsigned>(((x+.5-rx)*sx+left)*pw/c.width));
                    const auto py=std::min(ph-1,static_cast<unsigned>(((y+.5-ry)*sy+top)*ph/c.height));
                    double logit=0;
                    for(std::size_t k=0;k<static_cast<std::size_t>(ps[1]);++k)
                        logit+=static_cast<double>(data[(4+c.labels.size()+k)*count+selected_indices[n]])*proto[k*pw*ph+py*pw+px];
                    // sigmoid(logit) >= 0.5 iff logit >= 0; avoids exponential overflow.
                    mask.pixels[y*layout.width+x]=logit>=0?255:0;
                }
            }
            result.masks.push_back(std::move(mask));
        }
    }
    return result;
}
}
