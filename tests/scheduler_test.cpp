#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <vision/runtime/scheduler.hpp>
using namespace vision::runtime;
using namespace vision::contracts;
namespace {
struct Fixture {
    std::shared_ptr<FakeClock> clock=std::make_shared<FakeClock>();
    ResourceBudget budget{{{"slots",2},{"bytes",20}},4,clock};
    TaskScheduler scheduler{RunId("run"),2,budget,clock};
    Admission enqueue(std::uint64_t epoch=1) {
        return scheduler.enqueue(WorkerId("worker"),epoch,InspectionId("inspection"),CheckId("check"),
            {{"slots",1},{"bytes",10}},100);
    }
};
}
TEST_CASE("capacity includes undelivered terminals and dispatch is FIFO per worker") {
    Fixture f;const auto a=f.enqueue(),b=f.enqueue();
    REQUIRE(a.correlation);REQUIRE(b.correlation);
    CHECK(f.enqueue().status==AdmissionStatus::Full);
    auto d=f.scheduler.dispatch(WorkerId("worker"),1);REQUIRE(d);
    CHECK(d->correlation==*a.correlation);CHECK(d->remaining_ns==100);
    CHECK_FALSE(f.scheduler.dispatch(WorkerId("worker"),1));
    REQUIRE(f.scheduler.finish(*a.correlation,TaskState::Succeeded));
    CHECK_FALSE(f.scheduler.finish(*a.correlation,TaskState::Succeeded));
    CHECK(f.enqueue().status==AdmissionStatus::Full);
    REQUIRE(f.scheduler.take_terminal());
    d=f.scheduler.dispatch(WorkerId("worker"),1);REQUIRE(d);CHECK(d->correlation==*b.correlation);
    CHECK(f.enqueue().status==AdmissionStatus::Accepted);
}
TEST_CASE("running timeout holds resource until actual stop and late success cannot replace timeout") {
    Fixture f;const auto a=f.enqueue();REQUIRE(a.correlation);
    REQUIRE(f.scheduler.dispatch(WorkerId("worker"),1));
    f.clock->advance(100);f.scheduler.tick();
    CHECK(f.budget.snapshot().used.at("slots")==1);
    auto terminal=f.scheduler.take_terminal();REQUIRE(terminal);
    CHECK(terminal->state==TaskState::TimedOut);
    REQUIRE(f.scheduler.take_cancel());CHECK_FALSE(f.scheduler.take_cancel());
    CHECK(f.scheduler.snapshot().awaiting_stop==1);
    auto stale=*a.correlation;stale.worker_epoch=2;
    CHECK_FALSE(f.scheduler.finish(stale,TaskState::Succeeded));
    CHECK(f.budget.snapshot().used.at("slots")==1);
    CHECK_FALSE(f.scheduler.finish(*a.correlation,TaskState::Succeeded));
    CHECK(f.budget.snapshot().used.at("slots")==0);
    CHECK(f.scheduler.snapshot().retained==0);
    CHECK_FALSE(f.scheduler.take_terminal());
}
TEST_CASE("queued expiry releases resources and cancellation is idempotent") {
    Fixture f;const auto a=f.enqueue(),b=f.enqueue();
    REQUIRE(a.correlation);REQUIRE(b.correlation);
    REQUIRE(f.scheduler.cancel(*a.correlation));
    CHECK_FALSE(f.scheduler.cancel(*a.correlation));
    CHECK(f.budget.snapshot().used.at("slots")==1);
    f.clock->advance(100);f.scheduler.tick();
    CHECK(f.budget.snapshot().used.at("slots")==0);
    auto t=f.scheduler.take_terminal();REQUIRE(t);CHECK(t->state==TaskState::Cancelled);
    t=f.scheduler.take_terminal();REQUIRE(t);CHECK(t->state==TaskState::TimedOut);
    CHECK_FALSE(f.scheduler.dispatch(WorkerId("worker"),1));
    CHECK_FALSE(f.scheduler.take_cancel());
}
TEST_CASE("exit scopes cleanup by worker epoch and all correlation fields matter") {
    Fixture f;const auto a=f.enqueue(1),b=f.enqueue(2);
    REQUIRE(a.correlation);REQUIRE(b.correlation);
    REQUIRE(f.scheduler.dispatch(WorkerId("worker"),1));
    CHECK_FALSE(f.scheduler.dispatch(WorkerId("worker"),2));
    auto wrong=*b.correlation;wrong.attempt=2;
    CHECK_FALSE(f.scheduler.finish(wrong,TaskState::Succeeded));
    wrong=*b.correlation;wrong.run_id=RunId("old");
    CHECK_FALSE(f.scheduler.finish(wrong,TaskState::Succeeded));
    f.scheduler.worker_exited(WorkerId("worker"),1);
    CHECK(f.budget.snapshot().used.at("slots")==1);
    REQUIRE(f.scheduler.dispatch(WorkerId("worker"),2));
    f.scheduler.worker_exited(WorkerId("worker"),1); // stale exit cannot reclaim new epoch
    CHECK(f.budget.snapshot().used.at("slots")==1);
    REQUIRE(f.scheduler.finish(*b.correlation,TaskState::Succeeded));
    CHECK(f.budget.snapshot().used.at("slots")==0);
}
TEST_CASE("late queued tasks never dispatch and failed dispatch releases without retry") {
    Fixture f;auto a=f.enqueue();REQUIRE(a.correlation);
    REQUIRE(f.scheduler.dispatch(WorkerId("worker"),1));
    REQUIRE(f.scheduler.dispatch_rejected(*a.correlation));
    CHECK(f.budget.snapshot().used.at("slots")==0);
    auto t=f.scheduler.take_terminal();REQUIRE(t);CHECK(t->state==TaskState::Failed);
    a=f.enqueue();REQUIRE(a.correlation);CHECK(a.correlation->task_id.value()=="task-2");
    f.clock->advance(101);
    CHECK_FALSE(f.scheduler.dispatch(WorkerId("worker"),1));
}
TEST_CASE("resource rejection does not retain task or partially reserve") {
    Fixture f;
    const auto a=f.scheduler.enqueue(WorkerId("worker"),1,InspectionId("inspection"),CheckId("check"),
        {{"slots",1},{"bytes",21}},100);
    CHECK(a.status==AdmissionStatus::NoResources);
    CHECK(f.scheduler.snapshot().retained==0);
    CHECK(f.budget.snapshot().used.at("slots")==0);
    CHECK(f.scheduler.enqueue(WorkerId("worker"),0,InspectionId("inspection"),CheckId("check"),
        {{"slots",1}},100).status==AdmissionStatus::Invalid);
}
TEST_CASE("diagnostic history stays bounded without discarding terminal delivery") {
    Fixture f;
    for(int i=0;i<100;++i) {
        const auto admitted=f.enqueue();REQUIRE(admitted.correlation);
        REQUIRE(f.scheduler.dispatch(WorkerId("worker"),1));
        REQUIRE(f.scheduler.finish(*admitted.correlation,TaskState::Succeeded));
        REQUIRE(f.scheduler.take_terminal());
    }
    CHECK(f.scheduler.snapshot().retained==0);
    CHECK(f.scheduler.dropped_events()==44);
    const auto events=f.scheduler.take_events();REQUIRE(events.size()==256);
    CHECK(events.back().correlation.task_id.value()=="task-100");
    CHECK(events.back().reason=="Terminal");
    CHECK(f.scheduler.take_events().empty());
}
TEST_CASE("expired unsent dispatch keeps timeout and releases resources exactly once") {
    Fixture f;const auto admitted=f.enqueue();REQUIRE(admitted.correlation);
    REQUIRE(f.scheduler.dispatch(WorkerId("worker"),1));
    f.clock->advance(100);
    REQUIRE(f.scheduler.dispatch_rejected(*admitted.correlation));
    CHECK(f.budget.snapshot().used.at("slots")==0);
    CHECK_FALSE(f.scheduler.take_cancel());
    auto event=f.scheduler.take_terminal();REQUIRE(event);
    CHECK(event->state==TaskState::TimedOut);
    CHECK_FALSE(f.scheduler.dispatch_rejected(*admitted.correlation));
}
