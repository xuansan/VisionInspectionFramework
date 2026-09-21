#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <vision/runtime/supervisor.hpp>
using namespace vision::runtime;
namespace {
SupervisorPolicy policy() {return {100,50,40,10,5,20,2};}
void ready(Supervisor& s,std::uint64_t now=0) {
    REQUIRE(s.start(now));REQUIRE(s.poll(now)==ProcessAction::Launch);
    auto e=s.snapshot().epoch;
    s.spawned(e,now);s.authenticated(e,now);s.initialized(e,now);
    REQUIRE(s.snapshot().phase==WorkerPhase::Ready);
}
}
TEST_CASE("ready requires launch handshake initialization and current epoch") {
    Supervisor s(policy());
    REQUIRE(s.start(0));CHECK_FALSE(s.start(0));
    REQUIRE(s.poll(0)==ProcessAction::Launch);
    s.initialized(1,0);CHECK(s.snapshot().phase==WorkerPhase::Starting);
    s.spawned(2,0);CHECK_FALSE(s.snapshot().process_alive);
    s.spawned(1,0);s.authenticated(2,0);
    CHECK(s.snapshot().phase==WorkerPhase::Handshaking);
    s.authenticated(1,0);s.initialized(1,0);
    CHECK(s.snapshot().phase==WorkerPhase::Ready);
}
TEST_CASE("heartbeat cannot hide work stall or revive expired liveness") {
    Supervisor s(policy());ready(s);
    REQUIRE(s.set_busy(1,true,0));
    s.heartbeat(1,1,20);
    CHECK_FALSE(s.poll(39));
    CHECK(s.poll(40)==ProcessAction::Kill);
    CHECK(s.snapshot().reason=="ProgressTimeout");
    CHECK_FALSE(s.poll(1000)); // No new instance until actual exit.
    CHECK(s.snapshot().epoch==1);
    Supervisor late(policy());ready(late);
    late.heartbeat(1,1,50);
    CHECK(late.poll(50)==ProcessAction::Kill);
    CHECK(late.snapshot().reason=="HeartbeatTimeout");
}
TEST_CASE("finite recovery counts across ready and ignores old exits") {
    Supervisor s(policy());ready(s);
    for(std::uint64_t e=1;e<=3;++e) {
        const auto now=e*100;
        s.fault(e,"Crash",now);
        REQUIRE(s.poll(now)==ProcessAction::Kill);
        s.exited(e,now);
        if(e==3)break;
        REQUIRE(s.poll(now+20)==ProcessAction::Launch);
        CHECK(s.snapshot().epoch==e+1);
        s.exited(e,now+20); // stale completion from previous QProcess
        CHECK(s.snapshot().phase==WorkerPhase::Starting);
        s.spawned(e+1,now+20);s.authenticated(e+1,now+20);s.initialized(e+1,now+20);
    }
    CHECK(s.snapshot().phase==WorkerPhase::ManualIntervention);
    CHECK(s.snapshot().restarts==2);
    CHECK_FALSE(s.poll(10000));
    CHECK(s.start(10000));CHECK(s.snapshot().epoch==4);
}
TEST_CASE("stop cancels backoff and graceful timeout kills only once") {
    Supervisor s(policy());ready(s);
    s.stop(1);CHECK(s.poll(1)==ProcessAction::Stop);
    CHECK_FALSE(s.poll(10));CHECK(s.poll(11)==ProcessAction::Kill);
    CHECK_FALSE(s.poll(100));
    s.exited(1,100);CHECK(s.snapshot().phase==WorkerPhase::Stopped);
    Supervisor b(policy());ready(b);b.fault(1,"fault",1);b.poll(1);b.exited(1,1);
    b.stop(2);CHECK_FALSE(b.poll(100));CHECK(b.snapshot().phase==WorkerPhase::Stopped);
}
TEST_CASE("idle rotation preserves fault budget and rejects busy rotation") {
    Supervisor s(policy());ready(s);
    s.set_busy(1,true,0);CHECK_FALSE(s.rotate(1));
    s.set_busy(1,false,1);REQUIRE(s.rotate(1));
    REQUIRE(s.poll(1)==ProcessAction::Stop);s.exited(1,2);
    REQUIRE(s.poll(7)==ProcessAction::Launch);
    CHECK(s.snapshot().restarts==0);CHECK(s.snapshot().epoch==2);
}
TEST_CASE("lease checks identity sequence and permanently revokes at deadline") {
    ControlLease lease("run",7,10);
    CHECK_FALSE(lease.valid(0));
    CHECK_FALSE(lease.renew("old",7,1,0));
    CHECK_FALSE(lease.renew("run",6,1,0));
    REQUIRE(lease.renew("run",7,1,0));
    CHECK_FALSE(lease.renew("run",7,1,9));
    CHECK(lease.valid(9));
    CHECK_FALSE(lease.renew("run",7,2,10));
    CHECK_FALSE(lease.valid(10));
    CHECK_FALSE(lease.renew("run",7,3,11));
}
TEST_CASE("failed rotation consumes recovery budget and fatal launch does not retry") {
    Supervisor s(policy());ready(s);
    REQUIRE(s.rotate(1));REQUIRE(s.poll(1)==ProcessAction::Stop);
    REQUIRE(s.poll(11)==ProcessAction::Kill);
    s.exited(1,12,false);
    CHECK(s.snapshot().restarts==1);
    CHECK(s.snapshot().reason=="RotationStopTimeout");
    Supervisor fatal(policy());REQUIRE(fatal.start(0));fatal.poll(0);
    fatal.fault(1,"ConfigurationInvalid",0,false);fatal.poll(0);fatal.exited(1,0);
    CHECK(fatal.snapshot().phase==WorkerPhase::ManualIntervention);
    CHECK(fatal.snapshot().restarts==0);
}
