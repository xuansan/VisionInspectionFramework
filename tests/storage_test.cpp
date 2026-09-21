#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <vision/storage/store.hpp>
#include <vision/application/demo.hpp>
#include <fstream>
#include <chrono>
#include <thread>
#include <sqlite3.h>
#include <vision/inference/engine.hpp>
using namespace vision;
namespace {
struct Root {
    std::filesystem::path path=std::filesystem::temp_directory_path()/("vision-store-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Root(){std::filesystem::create_directory(path);}
    ~Root(){std::error_code e;std::filesystem::remove_all(path,e);}
};
}
TEST_CASE("authority result and routing intent commit atomically and survive restart") {
    Root root;auto event=*application::run_demo(application::DemoScenario::Ng).event;
    {
        storage::Store store(root.path);store.begin_inspection(event.result.run_id.value(),event.result.inspection_id.value());
        CHECK(store.commit(event,{"file","http"}));CHECK_FALSE(store.commit(event,{"file","http"}));
        CHECK(store.pending().size()==2);CHECK(store.query(0).size()==1);
        CHECK_THROWS(store.commit(event,{"file"}));
        auto conflict=event;conflict.event_id=contracts::EventId("another-event");
        CHECK_THROWS(store.commit(conflict,{"new-output"}));CHECK(store.pending().size()==2);
        store.begin_inspection("interrupted-run","piece-2");
    }
    {
        storage::Store store(root.path);CHECK(store.query(0).size()==1);CHECK(store.recover_interrupted()==1);CHECK(store.recover_interrupted()==0);
        store.observe(event.event_id.value(),"file",1,"BusinessAcked");
        CHECK(store.pending().size()==1);CHECK_THROWS(store.observe(event.event_id.value(),"file",2,"Pending"));
        CHECK_THROWS(store.query(0,201));CHECK_THROWS(store.commit(event,{"plc-action"}));
        bool rejected=false;std::thread other([&]{try{(void)store.query(0);}catch(...){rejected=true;}});other.join();CHECK(rejected);
        store.checkpoint();
    }
}
TEST_CASE("staging integrity quota missing files and fail closed corruption") {
    Root root;storage::Store store(root.path,2*1024*1024);
    std::vector<std::byte> pixels(64,std::byte{42});
    auto hash=store.stage_image("image-1",pixels);CHECK(hash.starts_with("sha256:"));CHECK(store.stage_image("image-1",pixels)==hash);
    CHECK(store.image("image-1")==pixels);CHECK(store.image_state("image-1")=="Pending");
    pixels[0]=std::byte{43};CHECK_THROWS(store.stage_image("image-1",pixels));
    CHECK_THROWS(store.stage_image("oversize",std::vector<std::byte>(2*1024*1024)));
    store.available("image-1");CHECK(store.image_state("image-1")=="Available");store.deleted("image-1");
    CHECK_THROWS(store.image("image-1"));CHECK(store.image_state("image-1")=="Deleted");
    store.stage_image("image-2",pixels);std::filesystem::remove(root.path/"image-2.stage");CHECK_THROWS(store.image("image-2"));
    Root corrupt;{std::ofstream f(corrupt.path/"results.sqlite");f<<"not a database";}
    CHECK_THROWS(storage::Store{corrupt.path});
    CHECK_THROWS(store.stage_image("file:stream",pixels));
}
TEST_CASE("durable claims survive restart and reject stale ACK and exhausted retry") {
    Root root;auto event=*application::run_demo(application::DemoScenario::Ok).event;
    const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    std::optional<storage::ClaimedIntent> first;
    {
        storage::Store store(root.path);store.commit(event,{"file","http"});
        first=store.claim("file","run-1",now);REQUIRE(first);CHECK(first->attempt==1);
        CHECK_FALSE(store.claim("file","run-2",now+1));
    }
    {
        storage::Store store(root.path);
        auto second=store.claim("file","run-2",now+3001);REQUIRE(second);CHECK(second->attempt==2);
        CHECK_THROWS(store.settle(*first,"BusinessAcked",now+3002));
        CHECK_THROWS(store.observe(event.event_id.value(),"file",3,"BusinessAcked"));
        store.settle(*second,"BusinessAcked",now+3002);
        CHECK_FALSE(store.claim("file","run-3",now+6002));
        for(unsigned i=0;i<3;++i){auto claim=store.claim("http","run-"+std::to_string(i),now+3001*i);REQUIRE(claim);CHECK(claim->attempt==i+1);}
        CHECK_FALSE(store.claim("http","run-4",now+10000));CHECK(store.pending().empty());
    }
    Root expired;storage::Store store(expired.path);store.commit(event,{"file"});
    CHECK_FALSE(store.claim("file","new-run",now+120000));CHECK(store.pending().empty());
}
TEST_CASE("database write lock and actual SQLite FULL fail without partial intents") {
    Root root;storage::Store store(root.path,1024*1024);
    sqlite3* external=nullptr;const auto path=(root.path/"results.sqlite").u8string();
    REQUIRE(sqlite3_open(reinterpret_cast<const char*>(path.c_str()),&external)==SQLITE_OK);
    REQUIRE(sqlite3_exec(external,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)==SQLITE_OK);
    auto event=*application::run_demo(application::DemoScenario::Ok).event;
    CHECK_THROWS(store.commit(event,{"file"}));CHECK(store.query(0).empty());
    sqlite3_exec(external,"ROLLBACK",nullptr,nullptr,nullptr);sqlite3_close(external);
    bool full=false;unsigned committed=0;
    for(unsigned i=0;i<2000;++i){
        event=*application::run_demo(application::DemoScenario::Ok).event;
        try{store.commit(event,{"file"});++committed;store.checkpoint();}
        catch(const std::exception& e){INFO(e.what());full=std::string(e.what()).find("code=13")!=std::string::npos;CHECK(full);break;}
    }
    CHECK(full);CHECK(committed>0);
    // The first failed event was rolled back, not half committed.
    CHECK(storage::find_result(root.path,event.event_id.value()).empty());
}
TEST_CASE("upload destinations survive restart and cannot alias another image") {
    Root root;std::vector<std::byte> pixels(64,std::byte{1});
    {
        storage::Store store(root.path);store.stage_image("one",pixels);store.stage_image("two",pixels);
        store.bind_object({"one","http://127.0.0.1:9000","images","one.pgm",0});
        store.bind_object({"one","http://127.0.0.1:9000","images","one.pgm",0});
        CHECK_THROWS(store.bind_object({"two","http://127.0.0.1:9000","images","one.pgm",0}));
        CHECK_THROWS(store.bind_object({"one","http://127.0.0.1:9000","images","changed.pgm",0}));
        CHECK(store.start_upload("one")==1);
    }
    storage::Store store(root.path);const auto jobs=store.pending_uploads();REQUIRE(jobs.size()==1);
    CHECK(jobs[0].attempt==1);CHECK(jobs[0].key=="one.pgm");
    CHECK(store.start_upload("one")==2);CHECK(store.start_upload("one")==3);
    CHECK_THROWS(store.start_upload("one"));store.fail_image("one");CHECK(store.pending_uploads().empty());
}

TEST_CASE("delivery totals distinguish no pending from business acknowledgement") {
    Root root;storage::Store store(root.path);
    auto acknowledged=*application::run_demo(application::DemoScenario::Ok).event;
    auto failed=*application::run_demo(application::DemoScenario::Ng).event;
    auto expired=*application::run_demo(application::DemoScenario::Ok).event;
    store.commit(acknowledged,{"file","http"});store.commit(failed,{"file"});store.commit(expired,{"file"});
    store.observe(acknowledged.event_id.value(),"file",1,"BusinessAcked");
    store.observe(failed.event_id.value(),"file",1,"Failed");store.observe(expired.event_id.value(),"file",1,"Expired");
    const auto totals=store.delivery_totals("file");
    CHECK(totals.total==3);CHECK(totals.acknowledged==1);CHECK(totals.pending==0);CHECK(totals.unconfirmed==2);
    CHECK(store.delivery_totals("http").pending==1);CHECK(store.delivery_totals("absent").total==0);
}

TEST_CASE("archived pixels preserve layout recipe and inspection identity across restart") {
    Root root;const std::string recipe="effective-recipe";
    const auto hash=inference::sha256(std::as_bytes(std::span(recipe.data(),recipe.size())));
    contracts::FrameDescriptor frame{{contracts::RunId("archive-run"),contracts::PoolId("pool"),
        {contracts::WorkerId("frame-archive"),1},0,1,2},contracts::FrameId("frame"),{8,4,8,0,32,contracts::PixelFormat::Mono8}};
    std::vector<std::byte> pixels(32,std::byte{42});std::string id;
    {
        storage::Store store(root.path);
        id=store.archive_frame(frame,32,"piece","camera-a",hash,recipe,pixels);
        CHECK(store.archive_frame(frame,32,"piece","camera-a",hash,recipe,pixels)==id);
        CHECK_THROWS(store.archive_frame(frame,32,"piece","camera-a",hash,"wrong",pixels));
        auto changed=frame;changed.layout={4,8,4,0,32,contracts::PixelFormat::Mono8};
        CHECK_THROWS(store.archive_frame(changed,32,"piece","camera-a",hash,recipe,pixels));
        auto event=*application::run_demo(application::DemoScenario::Ok).event;
        event.result.run_id=contracts::RunId("archive-run");event.result.inspection_id=contracts::InspectionId("piece");
        for(auto& check:event.result.checks){check.correlation.run_id=event.result.run_id;check.correlation.inspection_id=event.result.inspection_id;}
        CHECK_THROWS_WITH(store.commit(event,{"file"}),"Result archive recipe mismatch");CHECK(store.pending().empty());
        event.result.recipe_hash=hash;CHECK(store.commit(event,{"file"}));
        CHECK_FALSE(store.commit(event,{"file"}));
        const std::string other="different-recipe";
        const auto other_hash=inference::sha256(std::as_bytes(std::span(other.data(),other.size())));
        CHECK_THROWS_WITH(store.archive_frame(frame,32,"piece","camera-b",other_hash,other,pixels),"Archive result recipe mismatch");
        CHECK(store.frames("archive-run","piece").size()==1);
    }
    {
        storage::Store store(root.path);const auto rows=store.frames("archive-run","piece");
        REQUIRE(rows.size()==1);CHECK(rows[0].image==id);CHECK(rows[0].recipe_body==recipe);
        CHECK(rows[0].recipe_hash==hash);CHECK(store.image(id)==pixels);
        CHECK(store.image_state(id)=="Pending");CHECK(store.frames("other-run","piece").empty());
    }
}

TEST_CASE("version three metadata migrates without losing committed results") {
    Root root;auto event=*application::run_demo(application::DemoScenario::Ok).event;
    {storage::Store store(root.path);store.commit(event,{"file"});}
    sqlite3* db=nullptr;const auto path=(root.path/"results.sqlite").u8string();
    REQUIRE(sqlite3_open(reinterpret_cast<const char*>(path.c_str()),&db)==SQLITE_OK);
    const auto status=sqlite3_exec(db,"DROP TABLE archived_frames;PRAGMA user_version=3",nullptr,nullptr,nullptr);
    sqlite3_close(db);REQUIRE(status==SQLITE_OK);
    storage::Store store(root.path);CHECK(store.query(0).size()==1);CHECK(store.pending().size()==1);
    CHECK(store.frames(event.result.run_id.value(),event.result.inspection_id.value()).empty());
    CHECK_FALSE(store.commit(event,{"file"}));
}
