#pragma once
#include <vision/algorithm/qt/session.hpp>
#include <vision/application/production.hpp>
#include <vision/inspection/inspection.hpp>
#include <vision/runtime/resources.hpp>
#include <array>
#include <vision/application/qt/frame_archive.hpp>
namespace vision::application::qt {
struct StationConfig {
    QString host,camera_manifest,algorithm_manifest;
    std::array<std::string,2> camera_parameters{R"({"width":8,"height":4})",R"({"width":8,"height":4})"};
    std::array<std::string,2> algorithm_parameters{"{}","{}"};
    contracts::ImageLayout layout{8,4,8,0,32,contracts::PixelFormat::Mono8};
    std::string recipe_hash{"sha256:"+std::string(64,'a')};
    std::uint64_t budget_ns{1000000000};
    std::optional<FrameArchiveConfig> archive;
};
enum class TriggerStatus {Accepted,NotReady,Duplicate,Full,Invalid};
struct StationSnapshot {
    ProductionState state;
    bool ready,active;
    inspection::Counts counts;
    std::size_t leases,tickets;
    std::uint64_t archived_frames{};
    bool archive_alive{};
    std::string archive_error;
};
// Demo-only serialized station: one workpiece, two required checks, one unread event.
class Station {
public:
    Station(std::shared_ptr<runtime::qt::StationGuard>,StationConfig);
    ~Station();
    bool start(contracts::Mode mode=contracts::Mode::Demo);
    void pulse();
    TriggerStatus trigger(std::uint64_t sequence);
    void pause();
    bool resume();
    void stop();
    void cancel();
    StationSnapshot snapshot() const;
    std::optional<contracts::ResultEnvelope> take_result();
    std::array<runtime::WorkerSnapshot,4> workers() const;
private:
    void inspect_frame(std::size_t,contracts::FrameDescriptor);
    void finish_if_ready();
    void terminate(contracts::InspectionState,std::string);
    bool resources_ready() const;
    bool settled() const;
    StationConfig config_;
    std::string run_;
    std::shared_ptr<runtime::SteadyClock> clock_;
    runtime::ResourceBudget budget_;
    std::optional<runtime::Reservation> ticket_;
    ProductionController production_;
    std::array<std::unique_ptr<capture::qt::Session>,2> cameras_;
    std::array<std::unique_ptr<algorithm::qt::Session>,2> algorithms_;
    std::unique_ptr<FrameArchive> archive_; // Destroy before cameras and their mappings.
    std::array<std::optional<contracts::FrameDescriptor>,2> awaiting_archive_;
    std::array<std::optional<contracts::Correlation>,2> expected_;
    std::unique_ptr<inspection::Inspection> inspection_;
    std::optional<contracts::ResultEnvelope> result_;
    inspection::ResultCounter counter_{4096};
    std::uint64_t last_trigger_{},events_{},due_{};
    bool terminal_{},stopping_{};
};
}
