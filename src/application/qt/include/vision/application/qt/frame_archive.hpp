#pragma once
#include <vision/capture/qt/session.hpp>
#include <array>
namespace vision::application::qt {
struct FrameArchiveConfig {QString program,root;std::string recipe_hash,recipe_body;};
// Sequential, bounded, one workpiece. Lease return requires OS-confirmed process exit.
class FrameArchive {
public:
    FrameArchive(std::shared_ptr<runtime::qt::StationGuard>,FrameArchiveConfig,
        std::array<capture::qt::Session*,2>);
    ~FrameArchive();
    void submit(std::array<contracts::FrameDescriptor,2>,std::string inspection,std::uint64_t budget);
    void pulse();
    void stop();
    std::optional<std::array<contracts::FrameDescriptor,2>> take();
    bool alive() const {return process_.state()!=QProcess::NotRunning;}
    bool busy() const {return bool(frames_[0])||bool(frames_[1])||alive();}
    const std::string& error() const {return error_;}
    std::uint64_t staged() const {return staged_;}
private:
    void consume();
    void finish(int,QProcess::ExitStatus);
    void fail(std::string);
    void release();
    std::shared_ptr<runtime::qt::StationGuard> guard_;
    FrameArchiveConfig config_;
    std::array<capture::qt::Session*,2> sources_;
    std::array<std::optional<contracts::FrameDescriptor>,2> frames_;
    std::optional<std::array<contracts::FrameDescriptor,2>> completed_;
    QProcess process_;
    QByteArray output_,request_;
    runtime::SteadyClock clock_;
    std::uint64_t due_{},sequence_{},staged_{};
    std::string inspection_,error_;
    bool stopped_{};
};
}
