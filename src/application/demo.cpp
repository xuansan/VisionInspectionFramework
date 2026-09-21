#include <vision/application/demo.hpp>
#include <vision/inspection/rules.hpp>
#include <vision/runtime/bounded_queue.hpp>
#include <atomic>
#include <chrono>
#include <random>

namespace vision::application {
using namespace contracts;
DemoOutcome run_demo(DemoScenario scenario) {
    if(scenario < DemoScenario::Ok || scenario > DemoScenario::Overload)
        throw std::invalid_argument("Unknown demo scenario");
    static std::atomic<std::uint64_t> serial{0};
    auto value=serial.load();
    do {
        if(value==UINT64_MAX) throw std::overflow_error("Demo identity exhausted");
    } while(!serial.compare_exchange_weak(value,value+1));
    const auto id = std::to_string(value+1);
    // A fresh process gets a fresh demo run namespace; counter never wraps.
    static const RunId run([] {
        std::random_device random;
        return "demo-"+std::to_string(random())+"-"+std::to_string(random())+
            "-"+std::to_string(random())+"-"+std::to_string(random());
    }());
    const InspectionId inspection_id("demo-ins-" + id);
    const auto hash = "sha256:" + std::string(64,'0');
    auto clock = std::make_shared<runtime::FakeClock>();
    runtime::ResourceBudget budget({{"camera.left",1},{"camera.right",1},{"algorithm",1},{"storage.bytes",1024}},8,clock);
    std::optional<runtime::Reservation> occupied;
    if(scenario == DemoScenario::Overload) {
        auto pre = budget.try_reserve({{"camera.right",1}},1000000000);
        occupied = std::move(pre.ticket);
    }
    auto reservation = budget.try_reserve({{"camera.left",1},{"camera.right",1},{"algorithm",1},{"storage.bytes",1024}},1000000000);
    if(reservation.status != runtime::ReserveStatus::Accepted) {
        occupied.reset();
        return {false,"CapacityExceeded: workpiece was not admitted",std::nullopt,{},budget.snapshot()};
    }
    if(!reservation.ticket->commit()) throw std::logic_error("Demo reservation unexpectedly expired");
    Correlation a{run,WorkerId("demo-worker"),1,inspection_id,CheckId("surface"),TaskId("task-surface-" + id),1};
    Correlation b{run,WorkerId("demo-worker"),1,inspection_id,CheckId("width"),TaskId("task-width-" + id),1};
    InspectionResult context{run,inspection_id,WorkpieceId("demo-piece-" + id),StationId("demo-station"),Mode::Demo,hash};
    inspection::Inspection inspection(std::move(context),{{a,true},{b,true}});
    runtime::BoundedQueue<CheckResult> results(2,8192);
    inspection::Observations surface;
    if(scenario == DemoScenario::Ng)
        surface.detections.push_back({0,"scratch",0.95,10,20,30,40});
    const auto surface_decision = inspection::evaluate(inspection::ForbiddenClass{0},surface);
    CheckResult first{a};
    first.state=TaskState::Succeeded; first.quality=surface_decision.quality; first.defects=surface.detections;
    if(results.try_push(std::move(first),4096) != runtime::PushStatus::Accepted)
        throw std::logic_error("Demo queue unexpectedly full");
    if(scenario != DemoScenario::Timeout) {
        CheckResult second{b};
        if(scenario == DemoScenario::Failure) {
            second.error = Error{"MODEL.EXECUTION_FAILED",ErrorCategory::Execution,
                "Synthetic algorithm failure; zero defects is not OK",Retryability::AfterRecovery,"demo",b,{}};
        } else {
            inspection::Observations width;
            width.measurements.push_back({"width",10.0,"mm"});
            const auto d=inspection::evaluate(inspection::MeasurementRange{"width","mm",9,11,true,true},width);
            second.state=TaskState::Succeeded; second.quality=d.quality; second.measurements=width.measurements;
        }
        if(results.try_push(std::move(second),4096) != runtime::PushStatus::Accepted)
            throw std::logic_error("Demo queue unexpectedly full");
    }
    results.close();
    while(auto report=results.try_pop()) {
        const auto status=inspection.accept(*report);
        if(status != inspection::AcceptStatus::Accepted && status != inspection::AcceptStatus::Finalized)
            throw std::logic_error("Demo report rejected");
    }
    if(scenario == DemoScenario::Timeout) {
        const auto deadline = runtime::deadline_after(*clock,1000000000);
        clock->advance(1000000000);
        if(clock->now_ns() >= deadline)
            inspection.terminate(InspectionState::TimedOut,"TASK.DEADLINE_EXCEEDED");
    }
    auto finalized=inspection.take_finalized();
    if(!finalized) throw std::logic_error("Demo did not finalize");
    inspection::ResultCounter counter(8);
    if(counter.add(*finalized) != inspection::CountStatus::Counted ||
       counter.add(*finalized) != inspection::CountStatus::Duplicate ||
       inspection.take_finalized()) throw std::logic_error("Demo idempotency broken");
    // Synthetic work is now finished. A real timed-out SDK reader must first confirm termination.
    reservation.ticket->release();
    const auto utc=std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return {true,"Synthetic checks completed; no camera/PLC/network operations",
        ResultEnvelope{1,EventId(run.value()+"-event-" + id),parse_u64(id),static_cast<std::uint64_t>(utc),std::move(*finalized)},
        counter.snapshot(),budget.snapshot()};
}
} // namespace vision::application
