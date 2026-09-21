#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <vision/runtime/buffer_broker.hpp>
#include <vision/runtime/scheduler.hpp>
#include <vision/serialization/json_codec.hpp>
#include <nlohmann/json.hpp>
using namespace vision::contracts;
using namespace vision::runtime;
namespace {
const FrameOwner writer{WorkerId("camera"),1},reader{WorkerId("algorithm"),1},storage{WorkerId("storage"),2};
const ImageLayout mono{8,4,8,0,32,PixelFormat::Mono8};
struct Fixture {
    std::shared_ptr<FakeClock> clock=std::make_shared<FakeClock>();
    BufferBroker broker{RunId("run"),PoolId("pool"),1,64,2,clock};
    FrameLease acquire() {auto g=broker.acquire(writer,100);REQUIRE(g.lease);return *g.lease;}
};
}
TEST_CASE("layout validates color stride offset and overflow") {
    CHECK_NOTHROW(validate_layout(mono,32));
    for(auto format:{PixelFormat::RGB8,PixelFormat::BGR8})
        CHECK_NOTHROW(validate_layout({3,4,12,8,48,format},56));
    CHECK_THROWS(validate_layout({3,4,8,0,32,PixelFormat::RGB8},64));
    CHECK_THROWS(validate_layout({8,0,8,0,0,PixelFormat::Mono8},64));
    CHECK_THROWS(validate_layout({8,4,8,0,31,PixelFormat::Mono8},64));
    CHECK_THROWS(validate_layout({8,4,8,UINT64_MAX,32,PixelFormat::Mono8},64));
    CHECK_THROWS(validate_layout({8,4,UINT64_MAX,0,32,PixelFormat::Mono8},64));
    CHECK_THROWS(validate_layout({8,4,8,0,32,static_cast<PixelFormat>(99)},64));
}
TEST_CASE("reader transfer is atomic and rejects stale or expired permission") {
    Fixture f;auto w=f.acquire();
    auto first=f.broker.publish(w,FrameId("frame"),mono,{reader},100).readers.at(0);
    auto next=f.broker.transfer(first,storage,50);
    REQUIRE(next);CHECK(f.broker.snapshot().leases==1);
    CHECK_FALSE(f.broker.authorized(first));CHECK_FALSE(f.broker.release(first.permit));
    CHECK(f.broker.authorized(*next));CHECK(next->permit.owner==storage);
    CHECK_FALSE(f.broker.transfer(first,reader,50));
    CHECK_FALSE(f.broker.transfer(*next,{WorkerId("bad"),0},50));
    f.clock->advance(50);
    CHECK_FALSE(f.broker.transfer(*next,reader,50));
    CHECK(f.broker.snapshot().retiring==1);
    f.broker.worker_exited(reader);CHECK(f.broker.snapshot().leases==1);
    f.broker.worker_exited(storage);CHECK(f.broker.snapshot().free==1);
}
TEST_CASE("multi consumer ownership is atomic bounded and generation protected") {
    Fixture f;auto w=f.acquire();
    CHECK(f.broker.acquire(writer,100).status==LeaseStatus::Full);
    auto invalid=f.broker.publish(w,FrameId("frame"),mono,{reader,reader},100);
    CHECK(invalid.status==LeaseStatus::Invalid);
    CHECK(f.broker.snapshot().writing==1);CHECK(f.broker.snapshot().leases==1);
    const auto published=f.broker.publish(w,FrameId("frame"),mono,{reader,storage},100);
    REQUIRE(published.readers.size()==2);
    CHECK_FALSE(f.broker.release(w));
    CHECK(f.broker.authorized(published.readers[0]));
    CHECK(f.broker.release(published.readers[0].permit));
    CHECK_FALSE(f.broker.release(published.readers[0].permit));
    CHECK(f.broker.acquire(writer,100).status==LeaseStatus::Full);
    CHECK(f.broker.release(published.readers[1].permit));
    auto next=f.acquire();CHECK(next.generation==w.generation+1);
    CHECK_FALSE(f.broker.release(published.readers[1].permit));
    CHECK(f.broker.snapshot().leases==1);
    CHECK(f.broker.snapshot().high_water==1);
}
TEST_CASE("timeout isolates until actual reader release or exact OS exit") {
    Fixture f;const auto w=f.acquire();
    auto grant=f.broker.publish(w,FrameId("frame"),mono,{reader,storage},10);REQUIRE(grant.readers.size()==2);
    f.clock->advance(10);f.broker.tick();
    CHECK_FALSE(f.broker.authorized(grant.readers[0]));
    CHECK(f.broker.snapshot().retiring==1);
    CHECK(f.broker.acquire(writer,100).status==LeaseStatus::Full);
    f.broker.worker_exited({reader.worker,2});CHECK(f.broker.snapshot().leases==2);
    f.broker.worker_exited(reader);CHECK(f.broker.snapshot().leases==1);
    CHECK(f.broker.acquire(writer,100).status==LeaseStatus::Full);
    CHECK(f.broker.release(grant.readers[1].permit));CHECK(f.broker.snapshot().free==1);
}
TEST_CASE("writer death invalidates frame and wrong run pool or identity never release") {
    Fixture f;const auto w=f.acquire();
    auto wrong=w;wrong.pool=PoolId("other");CHECK_FALSE(f.broker.release(wrong));
    wrong=w;wrong.run=RunId("other");CHECK_FALSE(f.broker.release(wrong));
    wrong=w;wrong.owner=reader;CHECK_FALSE(f.broker.release(wrong));
    f.clock->advance(100);
    CHECK(f.broker.publish(w,FrameId("frame"),mono,{reader},10).status==LeaseStatus::Expired);
    CHECK(f.broker.snapshot().leases==1);
    f.broker.worker_exited(writer);
    CHECK(f.broker.snapshot().free==1);
    CHECK(f.broker.publish(w,FrameId("frame"),mono,{reader},10).status==LeaseStatus::Stale);
    auto newer=f.acquire();CHECK(newer.lease>w.lease);
}
TEST_CASE("descriptor layout and identity must agree with coordinator ledger") {
    Fixture f;const auto w=f.acquire();
    auto g=f.broker.publish(w,FrameId("frame"),mono,{reader},100);REQUIRE(g.readers.size()==1);
    auto d=g.readers[0];d.layout.width=7;CHECK_FALSE(f.broker.authorized(d));
    d=g.readers[0];d.frame=FrameId("other");CHECK_FALSE(f.broker.authorized(d));
    d=g.readers[0];d.permit.lease++;CHECK_FALSE(f.broker.authorized(d));
    f.broker.quarantine(w);CHECK(f.broker.authorized(g.readers[0])); // old writer no longer owns slot
    f.broker.quarantine(g.readers[0].permit);CHECK_FALSE(f.broker.authorized(g.readers[0]));
}
TEST_CASE("all validation failures leave writer ownership intact") {
    Fixture f;const auto w=f.acquire();
    CHECK(f.broker.publish(w,FrameId("frame"),mono,{},100).status==LeaseStatus::Invalid);
    CHECK(f.broker.publish(w,FrameId("frame"),mono,{reader,storage,writer},100).status==LeaseStatus::Invalid);
    CHECK(f.broker.publish(w,FrameId("frame"),mono,{{reader.worker,0}},100).status==LeaseStatus::Invalid);
    auto bad=mono;bad.length=65;
    CHECK(f.broker.publish(w,FrameId("frame"),bad,{reader},100).status==LeaseStatus::Invalid);
    CHECK(f.broker.snapshot().leases==1);
    CHECK(f.broker.release(w));CHECK(f.broker.snapshot().free==1);
}
TEST_CASE("frame descriptor JSON is strict and uint64 identity never loses precision") {
    Fixture f;auto w=f.acquire();w.generation=UINT64_MAX;w.lease=UINT64_MAX;
    const FrameDescriptor d{w,FrameId("frame"),mono};
    const auto encoded=vision::serialization::encode_frame(d,64);
    CHECK(vision::serialization::decode_frame(encoded,64)==d);
    auto j=nlohmann::json::parse(encoded);
    j["lease_id"]=UINT64_MAX;
    CHECK_THROWS(vision::serialization::decode_frame(j.dump(),64));
    j=nlohmann::json::parse(encoded);j["slot_id"]="4294967296";
    CHECK_THROWS(vision::serialization::decode_frame(j.dump(),64));
    j=nlohmann::json::parse(encoded);j["extra"]=true;
    CHECK_THROWS(vision::serialization::decode_frame(j.dump(),64));
    j=nlohmann::json::parse(encoded);j["stride"]="18446744073709551615";
    CHECK_THROWS(vision::serialization::decode_frame(j.dump(),64));
    CHECK_THROWS(vision::serialization::decode_frame(R"({"schema_version":1,"schema_version":1})",64));
}
TEST_CASE("lost publication acknowledgement or consumer grant cannot free active bytes") {
    Fixture f;auto w=f.acquire();
    auto g=f.broker.publish(w,FrameId("frame"),mono,{reader,storage},10);REQUIRE(g.readers.size()==2);
    CHECK(f.broker.publish(w,FrameId("frame"),mono,{reader,storage},10).status==LeaseStatus::Stale);
    // First consumer received and released; second grant was lost before delivery.
    CHECK(f.broker.release(g.readers[0].permit));
    f.clock->advance(10);f.broker.tick();
    auto slot=f.broker.inspect(0);REQUIRE(slot);
    CHECK(slot->generation==w.generation);CHECK(slot->phase==SlotPhase::Retiring);
    REQUIRE(slot->holders.size()==1);CHECK(slot->holders[0].owner==storage);
    CHECK(f.broker.acquire(writer,10).status==LeaseStatus::Full);
    f.broker.worker_exited(storage);
    CHECK(f.broker.snapshot().free==1);CHECK_FALSE(f.broker.inspect(1));
}
TEST_CASE("scheduler timeout and image retirement wait for the same real exit evidence") {
    Fixture f;auto w=f.acquire();
    auto g=f.broker.publish(w,FrameId("frame"),mono,{reader},10);REQUIRE(g.readers.size()==1);
    ResourceBudget resources({{"tasks",1}},1,f.clock);
    TaskScheduler scheduler(RunId("run"),1,resources,f.clock);
    auto admitted=scheduler.enqueue(reader.worker,reader.epoch,InspectionId("inspection"),CheckId("check"),{{"tasks",1}},10);
    REQUIRE(admitted.correlation);REQUIRE(scheduler.dispatch(reader.worker,reader.epoch));
    f.clock->advance(10);scheduler.tick();f.broker.tick();
    auto terminal=scheduler.take_terminal();REQUIRE(terminal);CHECK(terminal->state==TaskState::TimedOut);
    CHECK(resources.snapshot().used.at("tasks")==1);CHECK(f.broker.snapshot().retiring==1);
    scheduler.worker_exited(reader.worker,reader.epoch+1);f.broker.worker_exited({reader.worker,reader.epoch+1});
    CHECK(resources.snapshot().used.at("tasks")==1);CHECK(f.broker.snapshot().leases==1);
    scheduler.worker_exited(reader.worker,reader.epoch);f.broker.worker_exited(reader);
    CHECK(resources.snapshot().used.at("tasks")==0);CHECK(f.broker.snapshot().free==1);
}
