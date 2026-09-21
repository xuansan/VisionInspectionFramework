#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <vision/application/demo.hpp>
#include <vision/inspection/rules.hpp>
#include <vision/runtime/bounded_queue.hpp>
#include <vision/serialization/json_codec.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <fstream>
#include <filesystem>
#include <limits>
#include <thread>
#include <type_traits>

using namespace vision;
using namespace vision::contracts;
using J=nlohmann::json;
namespace {
Correlation corr(std::string check="left") {
    return {RunId("run-1"),WorkerId("worker-1"),1,InspectionId("ins-1"),CheckId(check),TaskId("task-"+check),1};
}
CheckResult report(std::string check="left",QualityVerdict q=QualityVerdict::OK) {
    CheckResult r{corr(check)}; r.state=TaskState::Succeeded; r.quality=q; return r;
}
inspection::Inspection fresh() {
    InspectionResult r{RunId("run-1"),InspectionId("ins-1"),WorkpieceId("piece-1"),StationId("station-1"),
        Mode::Demo,"sha256:"+std::string(64,'a')};
    return inspection::Inspection(std::move(r),{{corr("left"),true},{corr("right"),true}});
}
std::string read(std::string name) {
    const auto bytes=std::string(VISION_SOURCE_DIR)+"/examples/contracts/"+name;
    const auto path=std::filesystem::path(std::u8string(bytes.begin(),bytes.end()));
    std::ifstream file(path,std::ios::binary);
    if(!file) throw std::runtime_error("Missing fixture: "+name);
    return {std::istreambuf_iterator<char>(file),{}};
}
}
TEST_CASE("IDs are distinct and uint64 wire values never pass through floating point") {
    static_assert(!std::is_convertible_v<RunId,TaskId>);
    CHECK_THROWS_AS(RunId(""),std::invalid_argument);
    CHECK_THROWS_AS(RunId("with space"),std::invalid_argument);
    CHECK_THROWS_AS(RunId(std::string(129,'x')),std::invalid_argument);
    CHECK(parse_u64("9007199254740993")==9007199254740993ULL);
    CHECK(parse_u64("18446744073709551615")==UINT64_MAX);
    for(const auto* s:{"","00","01","-1","+1","1.0","1e3"," 1","18446744073709551616"})
        CHECK_THROWS_AS(parse_u64(s),std::invalid_argument);
}
TEST_CASE("required checks determine final quality, not arrival order or defect count") {
    auto i=fresh();
    CHECK(i.accept(report("left",QualityVerdict::NG))==inspection::AcceptStatus::Accepted);
    CHECK(i.snapshot().quality==QualityVerdict::Unknown);
    CHECK_FALSE(i.take_finalized());
    CHECK(i.accept(report("right"))==inspection::AcceptStatus::Finalized);
    CHECK(i.snapshot().state==InspectionState::Completed);
    CHECK(i.snapshot().quality==QualityVerdict::NG);
    CHECK(i.take_finalized().has_value());
    CHECK_FALSE(i.take_finalized());
    CHECK(i.accept(report("right"))==inspection::AcceptStatus::AlreadyFinalized);
}
TEST_CASE("all six task identity dimensions and worker identity are checked") {
    for(int dimension=0;dimension<7;++dimension) {
        auto i=fresh(); auto r=report();
        switch(dimension) {
        case 0:r.correlation.run_id=RunId("old");break;
        case 1:r.correlation.worker_epoch=2;break;
        case 2:r.correlation.inspection_id=InspectionId("other");break;
        case 3:r.correlation.check_id=CheckId("unknown");break;
        case 4:r.correlation.task_id=TaskId("other");break;
        case 5:r.correlation.attempt=2;break;
        case 6:r.correlation.worker_id=WorkerId("other");break;
        }
        CHECK(i.accept(r)==(dimension==3 ? inspection::AcceptStatus::UnknownCheck : inspection::AcceptStatus::Stale));
        CHECK(i.snapshot().checks.empty());
    }
}
TEST_CASE("duplicate, missing, failed with zero defects and late success are safe") {
    auto i=fresh();
    CHECK(i.accept(report())==inspection::AcceptStatus::Accepted);
    CHECK(i.accept(report())==inspection::AcceptStatus::Duplicate);
    auto failed=report("right");
    failed.state=TaskState::Failed; failed.quality=QualityVerdict::Unknown;
    CHECK(i.accept(failed)==inspection::AcceptStatus::Invalid); // Failure needs traceable error.
    failed.error=Error{"MODEL.FAILED",ErrorCategory::Execution,"No inference result",Retryability::Never,"test",failed.correlation,{}};
    CHECK(i.accept(failed)==inspection::AcceptStatus::Finalized);
    CHECK(i.snapshot().state==InspectionState::Failed);
    CHECK(i.snapshot().quality==QualityVerdict::Unknown);
    CHECK(i.snapshot().checks[1].defects.empty());
    CHECK_FALSE(i.terminate(InspectionState::TimedOut,"late timeout"));
    CHECK(i.accept(report("right"))==inspection::AcceptStatus::AlreadyFinalized);

    auto timeout=fresh();
    timeout.accept(report("left",QualityVerdict::NG));
    CHECK(timeout.terminate(InspectionState::TimedOut,"deadline"));
    CHECK(timeout.snapshot().quality==QualityVerdict::Unknown);
    REQUIRE(timeout.snapshot().checks.size()==2);
    CHECK(timeout.snapshot().checks[0].quality==QualityVerdict::NG);
    CHECK(timeout.snapshot().checks[1].state==TaskState::TimedOut);
    CHECK(timeout.accept(report("right"))==inspection::AcceptStatus::AlreadyFinalized);
}
TEST_CASE("optional checks never block completion or override required evidence") {
    InspectionResult context{RunId("run-1"),InspectionId("ins-1"),WorkpieceId("piece-1"),StationId("station-1"),
        Mode::Demo,"sha256:"+std::string(64,'a')};
    inspection::Inspection i(std::move(context),{{corr("left"),true},{corr("right"),false}});
    CHECK(i.accept(report("right",QualityVerdict::NG))==inspection::AcceptStatus::Accepted);
    CHECK(i.accept(report("left"))==inspection::AcceptStatus::Finalized);
    CHECK(i.snapshot().quality==QualityVerdict::OK);
}
TEST_CASE("counter does not double count and refuses saturation without forgetting identities") {
    const auto a=application::run_demo(application::DemoScenario::Ng);
    const auto b=application::run_demo(application::DemoScenario::Failure);
    inspection::ResultCounter c(1);
    REQUIRE(a.event); REQUIRE(b.event);
    CHECK(c.add(a.event->result)==inspection::CountStatus::Counted);
    CHECK(c.add(a.event->result)==inspection::CountStatus::Duplicate);
    CHECK(c.add(b.event->result)==inspection::CountStatus::Full);
    CHECK(c.add(a.event->result)==inspection::CountStatus::Duplicate);
    CHECK(c.snapshot().total==1);
    CHECK(c.snapshot().ng==1);
}
TEST_CASE("four rules specify empty input, missing labels, units and endpoints") {
    using namespace inspection;
    Observations o;
    CHECK(evaluate(ForbiddenClass{0},o).quality==QualityVerdict::OK);
    CHECK(evaluate(CountRange{0,1,2},o).quality==QualityVerdict::NG);
    CHECK(evaluate(AllowedClassification{{"good"}},o).error.has_value());
    o.classification="good";
    CHECK(evaluate(AllowedClassification{{"good"}},o).quality==QualityVerdict::OK);
    o.classification="bad";
    CHECK(evaluate(AllowedClassification{{"good"}},o).quality==QualityVerdict::NG);
    const MeasurementRange range{"width","mm",9,11,true,false};
    CHECK(evaluate(range,o).error.has_value());
    o.measurements={{"width",9,"mm"}};
    CHECK(evaluate(range,o).quality==QualityVerdict::OK);
    o.measurements[0].value=11;
    CHECK(evaluate(range,o).quality==QualityVerdict::NG);
    o.measurements[0].unit="cm";
    CHECK(evaluate(range,o).quality==QualityVerdict::Unknown);
    o.measurements[0].unit="mm";
    o.measurements[0].value=std::numeric_limits<double>::quiet_NaN();
    CHECK(evaluate(range,o).error.has_value());
    o.measurements={{"width",10,"mm"},{"width",10,"mm"}};
    CHECK(evaluate(range,o).error.has_value());
    CHECK_THROWS_AS(validate_rule(CountRange{0,3,1}),std::invalid_argument);
    CHECK_THROWS_AS(validate_rule(AllowedClassification{{"same","same"}}),std::invalid_argument);
    o.measurements.clear();
    o.detections={{0,"scratch",0.9,1,2,3,4}};
    CHECK(evaluate(ForbiddenClass{0},o).quality==QualityVerdict::NG);
    CHECK(evaluate(CountRange{0,1,1},o).quality==QualityVerdict::OK);
    o.detections[0].score=2;
    CHECK(evaluate(ForbiddenClass{0},o).error.has_value());
}
TEST_CASE("queue enforces both budgets and preserves rejected values") {
    runtime::BoundedQueue<std::string> q(2,5);
    std::string a="abc",b="de",rejected="f";
    CHECK(q.try_push(std::move(a),3)==runtime::PushStatus::Accepted);
    CHECK(q.try_push(std::move(b),2)==runtime::PushStatus::Accepted);
    CHECK(q.try_push(std::move(rejected),1)==runtime::PushStatus::Full);
    CHECK(rejected=="f");
    CHECK(q.try_pop().value()=="abc");
    CHECK(q.snapshot().bytes==2);
    CHECK(q.try_push(std::string("abcd"),4)==runtime::PushStatus::Full);
    CHECK(q.try_push(std::string("oversize"),6)==runtime::PushStatus::TooLarge);
    CHECK(q.try_push(std::string("zero"),0)==runtime::PushStatus::InvalidCost);
    q.close(); q.close();
    CHECK(q.try_push(std::string("x"),1)==runtime::PushStatus::Closed);
    CHECK(q.try_pop().value()=="de");
    CHECK_FALSE(q.try_pop());
    CHECK(q.snapshot().bytes==0);
}
TEST_CASE("concurrent queue attempts account for every accepted item without retries") {
    runtime::BoundedQueue<std::uint64_t> q(64,512);
    std::atomic<std::uint64_t> accepted{0},consumed{0},sum_in{0},sum_out{0};
    std::atomic<int> done{0};
    auto producer=[&](std::uint64_t base) {
        for(std::uint64_t n=1;n<=2000;++n) {
            auto value=base+n;
            if(q.try_push(std::move(value),8)==runtime::PushStatus::Accepted) {
                ++accepted; sum_in.fetch_add(base+n);
            }
        }
        ++done;
    };
    std::thread p1(producer,0),p2(producer,2000);
    // Test-only polling bounded by the two finite producers; production API never waits.
    std::thread consumer([&] {
        while(done.load()!=2 || q.snapshot().count!=0) {
            if(auto value=q.try_pop()) { ++consumed; sum_out.fetch_add(*value); }
            else std::this_thread::yield();
        }
    });
    p1.join(); p2.join(); consumer.join();
    CHECK(accepted==consumed);
    CHECK(sum_in==sum_out);
    CHECK(q.snapshot().bytes==0);
}
TEST_CASE("composite budget rolls back partial admission and expiry never frees committed use") {
    auto clock=std::make_shared<runtime::FakeClock>();
    runtime::ResourceBudget b({{"left",1},{"right",1},{"bytes",100}},8,clock);
    auto occupied=b.try_reserve({{"right",1}},10);
    REQUIRE(occupied.ticket);
    CHECK(b.try_reserve({{"left",1},{"right",1}},10).status==runtime::ReserveStatus::CapacityExceeded);
    CHECK(b.snapshot().used.at("left")==0);
    clock->advance(10);
    CHECK(occupied.ticket->state()==runtime::ReservationState::Expired);
    CHECK_FALSE(occupied.ticket->commit());
    auto all=b.try_reserve({{"left",1},{"right",1},{"bytes",100}},10);
    REQUIRE(all.ticket);
    CHECK(all.ticket->commit());
    CHECK(all.ticket->commit());
    clock->advance(100);
    b.expire_reserved();
    CHECK(all.ticket->state()==runtime::ReservationState::Committed);
    CHECK(b.snapshot().used.at("bytes")==100);
    all.ticket->release(); all.ticket->release(); occupied.ticket->release();
    CHECK(b.snapshot().active_tickets==0);
    CHECK(b.snapshot().used.at("bytes")==0);
}
TEST_CASE("resource tickets are move-only, bounded and reclaim local ownership on scope exit") {
    static_assert(!std::is_copy_constructible_v<runtime::Reservation>);
    auto clock=std::make_shared<runtime::FakeClock>();
    runtime::ResourceBudget b({{"slots",5}},1,clock);
    CHECK(b.try_reserve({{"missing",1}},1).status==runtime::ReserveStatus::InvalidRequest);
    CHECK(b.try_reserve({{"slots",0}},1).status==runtime::ReserveStatus::InvalidRequest);
    CHECK(b.try_reserve({{"slots",1}},0).status==runtime::ReserveStatus::InvalidRequest);
    {
        auto r=b.try_reserve({{"slots",1}},10);
        REQUIRE(r.ticket);
        auto moved=std::move(*r.ticket);
        CHECK(b.try_reserve({{"slots",1}},10).status==runtime::ReserveStatus::TicketLimit);
        CHECK(moved.commit());
    }
    CHECK(b.snapshot().used.at("slots")==0);
    clock->advance(UINT64_MAX);
    CHECK_THROWS_AS(clock->advance(1),std::overflow_error);
    CHECK_THROWS_AS(runtime::deadline_after(*clock,1),std::overflow_error);
    CHECK(b.try_reserve({{"slots",1}},1).status==runtime::ReserveStatus::InvalidRequest);
}
TEST_CASE("result roundtrip preserves uint64 and validates both serialization directions") {
    auto outcome=application::run_demo(application::DemoScenario::Ng);
    REQUIRE(outcome.event);
    auto e=*outcome.event;
    e.sequence=UINT64_MAX;
    e.emitted_at_unix_ns=9007199254740993ULL;
    e.result.checks[0].elapsed_ns=UINT64_MAX;
    const auto encoded=serialization::encode_result(e);
    const auto decoded=serialization::decode_result(encoded);
    CHECK(decoded.sequence==UINT64_MAX);
    CHECK(decoded.emitted_at_unix_ns==9007199254740993ULL);
    CHECK(decoded.result.checks[0].elapsed_ns==UINT64_MAX);
    CHECK(decoded.result.quality==QualityVerdict::NG);
    CHECK(decoded.result.checks[0].defects[0].label=="scratch");
    e.result.quality=QualityVerdict::OK;
    CHECK_THROWS_AS(serialization::encode_result(e),serialization::ProtocolError);
}
TEST_CASE("malformed, duplicate, oversized, incompatible and semantically forged results fail") {
    const auto demo=application::run_demo(application::DemoScenario::Ok);
    const auto valid=serialization::encode_result(*demo.event);
    const J original=J::parse(valid);
    for(int mutation=0;mutation<10;++mutation) {
        auto j=original;
        switch(mutation) {
        case 0:j["schema_version"]=2;break;
        case 1:j["sequence"]=9007199254740993ULL;break;
        case 2:j["sequence"]="18446744073709551616";break;
        case 3:j["sequence"]="01";break;
        case 4:j["unexpected"]=true;break;
        case 5:j["result"]["checks"].erase(1);break;
        case 6:j["result"]["checks"][0]["correlation"]["run_id"]="old";break;
        case 7:j["result"]["checks"][0]["execution_state"]="Failed";break;
        case 8:j["result"]["checks"][0]["measurements"]={{{"name","w"},{"value",nullptr},{"unit","mm"}}};break;
        case 9:j["result"]["checks"].push_back(j["result"]["checks"][0]);break;
        }
        CHECK_THROWS_AS(serialization::decode_result(j.dump()),serialization::ProtocolError);
    }
    CHECK_THROWS_AS(serialization::decode_result("{\"schema_version\":1,\"schema_version\":1}"),serialization::ProtocolError);
    CHECK_THROWS_AS(serialization::decode_result(std::string(1024*1024+1,' ')),serialization::ProtocolError);
    CHECK_THROWS_AS(serialization::decode_result(std::string(40,'[')+std::string(40,']')),serialization::ProtocolError);
}
TEST_CASE("recipe and manifest enforce configuration meaning and local library boundaries") {
    const auto recipe=read("recipe-v1.json");
    const auto manifest=read("manifest-v1.json");
    CHECK(serialization::decode_recipe(recipe).checks.size()==2);
    CHECK(serialization::decode_manifest(manifest).kind=="Algorithm");
    auto j=J::parse(recipe);
    j["checks"][1]["check_id"]=j["checks"][0]["check_id"];
    CHECK_THROWS_AS(serialization::decode_recipe(j.dump()),serialization::ProtocolError);
    j=J::parse(recipe); j["checks"][1]["rule"]["minimum"]=20;
    CHECK_THROWS_AS(serialization::decode_recipe(j.dump()),serialization::ProtocolError);
    j=J::parse(recipe); j["deadline_ms"]="0";
    CHECK_THROWS_AS(serialization::decode_recipe(j.dump()),serialization::ProtocolError);
    j=J::parse(manifest); j["entry_library"]="../escape.dll";
    CHECK_THROWS_AS(serialization::decode_manifest(j.dump()),serialization::ProtocolError);
    j=J::parse(manifest); j["sdk_api_version"]=2;
    CHECK_THROWS_AS(serialization::decode_manifest(j.dump()),serialization::ProtocolError);
    j=J::parse(manifest); j["max_instances"]=0;
    CHECK_THROWS_AS(serialization::decode_manifest(j.dump()),serialization::ProtocolError);
}
TEST_CASE("application scenarios produce truthful outcomes and release all local budgets") {
    using S=application::DemoScenario;
    for(const auto scenario:{S::Ok,S::Ng,S::Failure,S::Timeout,S::Overload}) {
        const auto d=application::run_demo(scenario);
        CHECK(d.resources.active_tickets==0);
        for(const auto& [key,amount]:d.resources.used) { (void)key; CHECK(amount==0); }
        if(scenario==S::Overload) { CHECK_FALSE(d.admitted); CHECK_FALSE(d.event); CHECK(d.counts.total==0); }
        else {
            REQUIRE(d.event);
            CHECK(d.counts.total==1);
            const auto q=scenario==S::Ok ? QualityVerdict::OK : scenario==S::Ng ? QualityVerdict::NG : QualityVerdict::Unknown;
            CHECK(d.event->result.quality==q);
            CHECK(d.event->result.mode==Mode::Demo);
            CHECK_NOTHROW(serialization::decode_result(serialization::encode_result(*d.event)));
        }
    }
}
TEST_CASE("plugin parameter schema and runtime updates are validated independently of UI") {
    auto j=J::parse(read("manifest-v1.json"));
    j["parameters_schema"]={
        {"type","object"},{"additionalProperties",false},
        {"properties",{
            {"threshold",{{"type","number"},{"minimum",0},{"maximum",1},{"default",0.5},{"mutable_during_run",true}}},
            {"count",{{"type","integer"},{"minimum",1},{"maximum",10},{"mutable_during_run",false}}}}},
        {"required",{"count"}}};
    auto manifest=serialization::decode_manifest(j.dump());
    CHECK_NOTHROW(serialization::validate_parameters(manifest,R"({"count":2,"threshold":0.9})"));
    CHECK_THROWS_AS(serialization::validate_parameters(manifest,"{}"),serialization::ProtocolError);
    CHECK_THROWS_AS(serialization::validate_parameters(manifest,R"({"count":2,"threshold":2})"),serialization::ProtocolError);
    CHECK_THROWS_AS(serialization::validate_parameters(manifest,R"({"count":2.5})"),serialization::ProtocolError);
    CHECK_THROWS_AS(serialization::validate_parameters(manifest,R"({"count":2,"extra":true})"),serialization::ProtocolError);
    CHECK_NOTHROW(serialization::validate_parameters(manifest,R"({"threshold":0.1})",true));
    CHECK_THROWS_AS(serialization::validate_parameters(manifest,R"({"count":2})",true),serialization::ProtocolError);
    j["parameters_schema"]["properties"]["threshold"]["default"]=5;
    CHECK_THROWS_AS(serialization::decode_manifest(j.dump()),serialization::ProtocolError);
    j["parameters_schema"]["properties"]["threshold"]={{"type","array"}};
    CHECK_THROWS_AS(serialization::decode_manifest(j.dump()),serialization::ProtocolError);
}
