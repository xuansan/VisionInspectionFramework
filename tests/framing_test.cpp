#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <vision/ipc/framing.hpp>
#include <vision/ipc/session.hpp>
using namespace vision::ipc;
TEST_CASE("every split boundary and concatenated frames") {
    const auto encoded=frame("{\"message\":\"hello\"}");
    for(std::size_t cut=0;cut<=encoded.size();++cut) {
        Decoder d;
        d.feed(std::string_view(encoded).substr(0,cut));
        d.feed(std::string_view(encoded).substr(cut));
        REQUIRE(d.available());
        CHECK(d.pop()=="{\"message\":\"hello\"}");
        CHECK_FALSE(d.available());
        CHECK_NOTHROW(d.finish());
    }
    Decoder d;
    d.feed(frame("a")+frame("b"));
    CHECK(d.pop()=="a"); CHECK(d.pop()=="b");
}
TEST_CASE("length limits, queue saturation and sticky failure") {
    Decoder d(8,1);
    CHECK_THROWS_AS(d.feed(frame("a")+frame("b")),FrameError);
    CHECK_FALSE(d.available());
    CHECK_THROWS_AS(d.feed(frame("c")),FrameError);
    Decoder zero;
    CHECK_THROWS_AS(zero.feed(std::string(4,'\0')),FrameError);
    Decoder huge(8);
    CHECK_THROWS_AS(huge.feed(frame("123456789")),FrameError);
    CHECK_THROWS_AS(frame(""),FrameError);
}
TEST_CASE("disconnect at every partial byte is rejected") {
    const auto encoded=frame("payload");
    for(std::size_t cut=1;cut<encoded.size();++cut) {
        Decoder d; d.feed(std::string_view(encoded).substr(0,cut));
        CHECK_THROWS_AS(d.finish(),FrameError);
    }
}
TEST_CASE("session rejects old identity, bad credentials, versions, gaps and premature messages") {
    const std::string token(32,'a');
    Header h{1,0,"Hello","req-1","run-1","worker-1",1,1};
    Session s("run-1","worker-1",1,token,100);
    CHECK(s.hello(h,token,{},0)==SessionStatus::Accepted);
    h.type="Heartbeat";h.sequence=2;
    CHECK(s.receive(h)==SessionStatus::Accepted);
    CHECK(s.receive(h)==SessionStatus::SequenceMismatch);
    CHECK_FALSE(s.authenticated());
    for(int mutation=0;mutation<6;++mutation) {
        Session peer("run-1","worker-1",1,token,100);
        Header hello{1,0,"Hello","req","run-1","worker-1",1,1};
        if(mutation==0) hello.epoch=2;
        if(mutation==1) hello.major=2;
        if(mutation==2) hello.run_id="other";
        const auto result=peer.hello(hello,mutation==3?"wrong":token,
            mutation==4?std::set<std::string>{"unsupported"}:std::set<std::string>{},mutation==5?100:0);
        CHECK(result!=SessionStatus::Accepted);
        CHECK_FALSE(peer.authenticated());
    }
    Session premature("run-1","worker-1",1,token,100);
    CHECK(premature.receive(h)==SessionStatus::NotAuthenticated);
}
TEST_CASE("control reserve survives normal overload and pending requests are bounded") {
    Outbox box(1,8,2,8);
    CHECK(box.try_push({"SubmitTask","12345678"}));
    CHECK_FALSE(box.try_push({"SubmitTask","a"}));
    CHECK(box.try_push({"Stop","stop"}));
    REQUIRE(box.pop()->type=="Stop");
    CHECK(box.pop()->type=="SubmitTask");
    CHECK(box.bytes()==0);
    box.close();CHECK_FALSE(box.try_push({"Stop","stop"}));
    PendingRequests pending(2);
    CHECK(pending.add("one",10,0));CHECK_FALSE(pending.add("one",10,0));
    CHECK(pending.add("two",20,0));CHECK_FALSE(pending.add("three",30,0));
    CHECK(pending.expire(10)==std::vector<std::string>{"one"});
    CHECK(pending.disconnect()==std::vector<std::string>{"two"});
    CHECK_FALSE(pending.add("three",30,0));
}
