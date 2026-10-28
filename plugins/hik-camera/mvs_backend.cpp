#include "backend.hpp"
#include <MvCameraControl.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <cstring>
namespace vision::hik {
namespace {
void ok(int code){if(code!=MV_OK)throw std::runtime_error("MVS error "+std::to_string(static_cast<unsigned>(code)));}
std::string serial(const MV_CC_DEVICE_INFO& d) {
    const unsigned char* s=d.nTLayerType==MV_GIGE_DEVICE?d.SpecialInfo.stGigEInfo.chSerialNumber:d.SpecialInfo.stUsb3VInfo.chSerialNumber;
    auto n=strnlen(reinterpret_cast<const char*>(s),64);if(n==64)throw std::runtime_error("Serial unterminated");
    return {reinterpret_cast<const char*>(s),n};
}
class Mvs final:public Backend {
public:
    Mvs(){ok(MV_CC_Initialize());}
    ~Mvs(){cleanup();MV_CC_Finalize();}
    std::string enumerate() override {
        MV_CC_DEVICE_INFO_LIST list{};ok(MV_CC_EnumDevices(MV_GIGE_DEVICE|MV_USB_DEVICE,&list));
        nlohmann::json devices=nlohmann::json::array();
        for(unsigned i=0;i<list.nDeviceNum;++i)devices.push_back({{"id",serial(*list.pDeviceInfo[i])},{"simulated",false}});
        return nlohmann::json{{"devices",devices},
            {"sdk_version",std::to_string(MV_CC_GetSDKVersion())}}.dump();
    }
    void open(const Config& c) override {
        if(handle_)throw std::runtime_error("Already open");config_=c;
        try {
            MV_CC_DEVICE_INFO_LIST list{};ok(MV_CC_EnumDevices(MV_GIGE_DEVICE|MV_USB_DEVICE,&list));
            MV_CC_DEVICE_INFO* found=nullptr;
            for(unsigned i=0;i<list.nDeviceNum;++i)if(serial(*list.pDeviceInfo[i])==c.serial){if(found)throw std::runtime_error("Ambiguous serial");found=list.pDeviceInfo[i];}
            if(!found)throw std::runtime_error("Camera serial not found");
            ok(MV_CC_CreateHandle(&handle_,found));ok(MV_CC_OpenDevice(handle_));opened_=true;
            ok(MV_CC_SetEnumValueByString(handle_,"AcquisitionMode","Continuous"));
            ok(MV_CC_SetEnumValueByString(handle_,"TriggerMode","On"));ok(MV_CC_SetEnumValueByString(handle_,"TriggerSource","Software"));
            ok(MV_CC_SetIntValueEx(handle_,"OffsetX",0));ok(MV_CC_SetIntValueEx(handle_,"OffsetY",0));
            ok(MV_CC_SetIntValueEx(handle_,"Width",c.width));ok(MV_CC_SetIntValueEx(handle_,"Height",c.height));
            ok(MV_CC_SetEnumValueByString(handle_,"ExposureAuto","Off"));ok(MV_CC_SetFloatValue(handle_,"ExposureTime",c.exposure));
            ok(MV_CC_SetEnumValueByString(handle_,"GainAuto","Off"));ok(MV_CC_SetFloatValue(handle_,"Gain",c.gain));
            MVCC_INTVALUE_EX payload{};ok(MV_CC_GetIntValueEx(handle_,"PayloadSize",&payload));
            if(payload.nCurValue<=0||payload.nCurValue>32*1024*1024)throw std::runtime_error("Payload bound");
            ok(MV_CC_SetImageNodeNum(handle_,2));ok(MV_CC_StartGrabbing(handle_));grabbing_=true;
        }catch(...){cleanup();throw;}
    }
    void trigger() override {
        // Exactly one trigger is in flight. Drain stale SDK buffers before issuing a new trigger.
        bool drained=false;
        for(int i=0;i<3;++i){MV_FRAME_OUT f{};const int s=MV_CC_GetImageBuffer(handle_,&f,0);if(s==MV_E_NODATA){drained=true;break;}ok(s);ok(MV_CC_FreeImageBuffer(handle_,&f));}
        if(!drained)throw std::runtime_error("Unexpected image stream");
        ok(MV_CC_SetCommandValue(handle_,"TriggerSoftware"));
    }
    bool read(std::vector<std::byte>& pixels) override {
        MV_FRAME_OUT frame{};const auto s=MV_CC_GetImageBuffer(handle_,&frame,5);
        if(s==MV_E_NODATA)return false;ok(s);
        struct Release{void* h;MV_FRAME_OUT* f;~Release(){if(f)MV_CC_FreeImageBuffer(h,f);}} release{handle_,&frame};
        const auto& info=frame.stFrameInfo;
        if(info.nWidth!=config_.width||info.nHeight!=config_.height||!frame.pBufAddr||!info.nFrameLen||info.nFrameLen>32*1024*1024)
            throw std::runtime_error("Unexpected image layout");
        const unsigned channels=config_.format=="Mono8"?1:3;
        pixels.resize(static_cast<std::size_t>(config_.width)*config_.height*channels);
        MV_CC_PIXEL_CONVERT_PARAM_EX convert{};convert.nWidth=info.nWidth;convert.nHeight=info.nHeight;
        convert.enSrcPixelType=info.enPixelType;convert.pSrcData=frame.pBufAddr;convert.nSrcDataLen=info.nFrameLen;
        convert.enDstPixelType=channels==1?PixelType_Gvsp_Mono8:config_.format=="RGB8"?PixelType_Gvsp_RGB8_Packed:PixelType_Gvsp_BGR8_Packed;
        convert.pDstBuffer=reinterpret_cast<unsigned char*>(pixels.data());convert.nDstBufferSize=static_cast<unsigned>(pixels.size());
        ok(MV_CC_ConvertPixelTypeEx(handle_,&convert));
        if(convert.nDstLen!=pixels.size())throw std::runtime_error("Converted length mismatch");
        const auto released=MV_CC_FreeImageBuffer(handle_,&frame);release.f=nullptr;ok(released);return true;
    }
private:
    void cleanup() noexcept {if(handle_){if(grabbing_)MV_CC_StopGrabbing(handle_);if(opened_)MV_CC_CloseDevice(handle_);MV_CC_DestroyHandle(handle_);}handle_=nullptr;opened_=false;grabbing_=false;}
    void* handle_{};bool opened_{},grabbing_{};Config config_;
};
}
std::unique_ptr<Backend> make_backend(){return std::make_unique<Mvs>();}
}
