#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <vision/ipc/channel.hpp>
#include <nlohmann/json.hpp>
#include <vision/application/demo.hpp>
#include <vision/serialization/json_codec.hpp>
using namespace vision::ipc;
using J=nlohmann::json;
namespace {
const std::string token(48,'t');
const std::string hash="sha256:"+std::string(64,'a');
const std::string config=J{{"recipe_hash",hash},{"config_revision","1"}}.dump();
const std::string task=R"({"operation":"Inspect","input_ref":"frame-1","budget_ns":"1000"})";
vision::contracts::Correlation correlation() {
    return {vision::contracts::RunId("run"),vision::contracts::WorkerId("worker"),7,
        vision::contracts::InspectionId("inspection"),vision::contracts::CheckId("check"),vision::contracts::TaskId("task"),1};
}
Message message(std::string type,std::string payload) {
    return {{1,0,std::move(type),"req","run","worker",7,1},{},std::move(payload)};
}
void transfer(Channel& from,Channel& to,std::uint64_t now=0) {
    while(auto wire=from.next_wire(now))to.feed(*wire,now);
}
struct Pair {
    Limits limits;
    Channel client,server;
    explicit Pair(Limits l={}):limits(l),client("run","worker",7,token,true,l,0),server("run","worker",7,token,false,l,0) {
        transfer(client,server);transfer(server,client);
        REQUIRE(client.snapshot().authenticated);
        REQUIRE(server.snapshot().authenticated);
    }
};
}
TEST_CASE("all control payloads and full header roundtrip") {
    std::vector<Message> messages;
    messages.push_back(message("Hello",hello_payload(token)));
    messages.push_back(message("Configure",config));
    messages.push_back(message("Ready",config));
    messages.push_back(message("SubmitTask",task));
    messages.push_back(message("TaskAccepted","{}"));
    messages.push_back(message("TaskProgress",R"({"progress_sequence":"18446744073709551615"})"));
    messages.push_back(message("TaskFinished",R"({"execution_state":"Succeeded","quality":"NG","result_ref":"result-1","error_code":null})"));
    messages.push_back(message("Cancel",R"({"reason":"cancel"})"));
    messages.push_back(message("Heartbeat",R"({"progress_sequence":"0"})"));
    messages.push_back(message("Fault",R"({"code":"DEVICE.FAILED","category":"Device","message":"failure","retryability":"AfterRecovery"})"));
    messages.push_back(message("Drain",R"({"budget_ns":"20"})"));
    messages.push_back(message("Stop",R"({"reason":"stop"})"));
    messages.push_back(message("LeaseRelease",R"({"pool_id":"pool","lease_id":"lease","slot_id":"0","slot_generation":"9"})"));
    auto event=*vision::application::run_demo(vision::application::DemoScenario::Ng).event;
    auto result=message("ResultEvent",J{{"event",J::parse(vision::serialization::encode_result(event))}}.dump());
    result.header.run_id=event.result.run_id.value();messages.push_back(result);
    messages.push_back(message("DeliveryReport",R"({"event_id":"event","output_instance_id":"http","state":"BusinessAcked","attempt":"1","error_code":null})"));
    for(auto m:messages) {
        if(m.header.type.starts_with("Task")||m.header.type=="SubmitTask"||m.header.type=="Cancel")m.correlation=correlation();
        m.header.sequence=UINT64_MAX;
        auto parsed=decode_message(encode_message(m));
        CHECK(parsed.header.type==m.header.type);
        CHECK(parsed.header.sequence==UINT64_MAX);
        CHECK(parsed.correlation==m.correlation);
    }
}
TEST_CASE("inference evidence cannot substitute another task or carry oversized masks") {
    auto event=*vision::application::run_demo(vision::application::DemoScenario::Ng).event;
    auto check=event.result.checks.front();check.correlation=correlation();
    auto response=message("CheckFinished",J{{"check",J::parse(vision::serialization::encode_check(check))}}.dump());
    response.correlation=correlation();
    CHECK(decode_message(encode_message(response)).header.type=="CheckFinished");
    auto bad=J::parse(response.payload);bad["check"]["correlation"]["task_id"]="other-task";
    response.payload=bad.dump();CHECK_THROWS(encode_message(response));
    bad["check"]["correlation"]["task_id"]="task";
    bad["check"]["masks"]=J::array({J{{"hash",hash},{"width",1048576},{"height",2},{"instance",0},{"storage","local_ephemeral"}}});
    response.payload=bad.dump();CHECK_THROWS(encode_message(response));
}
TEST_CASE("malformed headers, duplicate keys, types, UTF8, payloads and identity reject") {
    const auto valid=J::parse(encode_message(message("Configure",config)));
    for(int n=0;n<12;++n) {
        auto j=valid;
        switch(n) {
        case 0:j["extra"]=1;break;
        case 1:j["worker_epoch"]=7;break;
        case 2:j["sequence"]="01";break;
        case 3:j["sequence"]="18446744073709551616";break;
        case 4:j["protocol_major"]=2;break;
        case 5:j["protocol_minor"]=1;break;
        case 6:j["message_type"]="Undefined";break;
        case 7:j["payload"]["recipe_hash"]="bad";break;
        case 8:j["payload"]["extra"]=1;break;
        case 9:j["payload"]["config_revision"]="0";break;
        case 10:j.erase("request_id");break;
        case 11:j["payload"]=nullptr;break;
        }
        CHECK_THROWS_AS(decode_message(j.dump()),ProtocolError);
    }
    CHECK_THROWS_AS(decode_message(R"({"a":1,"a":2})"),ProtocolError);
    CHECK_THROWS_AS(decode_message(std::string(1048577,' ')),ProtocolError);
    CHECK_THROWS_AS(decode_message(std::string(34,'[')+std::string(34,']')),ProtocolError);
    auto invalid_utf=valid.dump();invalid_utf.insert(2,1,static_cast<char>(0xff));
    CHECK_THROWS_AS(decode_message(invalid_utf),ProtocolError);
    auto m=message("SubmitTask",task);
    CHECK_THROWS_AS(encode_message(m),ProtocolError);
    m.correlation=correlation();m.correlation->worker_epoch=6;
    CHECK_THROWS_AS(encode_message(m),ProtocolError);
    m=message("TaskFinished",R"({"execution_state":"Failed","quality":"OK","result_ref":null,"error_code":"FAIL"})");
    m.correlation=correlation();
    CHECK_THROWS_AS(encode_message(m),ProtocolError);
}
TEST_CASE("request matching distinguishes accepted progress terminal and configuration mismatch") {
    Pair p;
    auto sent=p.client.send("Configure",config,{},1);
    REQUIRE(sent.status==SendStatus::Accepted);
    transfer(p.client,p.server,1);
    auto request=p.server.pop();REQUIRE(request);
    CHECK(p.server.reply(*request,"Ready",config,1)==SendStatus::Accepted);
    transfer(p.server,p.client,1);
    REQUIRE(p.client.pop());
    CHECK(p.client.snapshot().pending==0);
    sent=p.client.send("SubmitTask",task,correlation(),2);
    REQUIRE(sent.status==SendStatus::Accepted);
    transfer(p.client,p.server,2);request=p.server.pop();REQUIRE(request);
    CHECK(p.server.reply(*request,"TaskAccepted","{}",2)==SendStatus::Accepted);
    transfer(p.server,p.client,2);
    CHECK(p.client.snapshot().pending==1);
    REQUIRE(p.client.pop());
    CHECK(p.server.reply(*request,"TaskProgress",R"({"progress_sequence":"1"})",2)==SendStatus::Accepted);
    CHECK(p.server.reply(*request,"TaskFinished",R"({"execution_state":"Failed","quality":"Unknown","result_ref":null,"error_code":"MODEL.FAIL"})",2)==SendStatus::Accepted);
    transfer(p.server,p.client,2);
    REQUIRE(p.client.pop());REQUIRE(p.client.pop());
    CHECK(p.client.snapshot().pending==0);
}
TEST_CASE("deadlines, queued expiry, late response, disconnect and trickle frame have finite outcomes") {
    Pair p;
    auto sent=p.client.send("Configure",config,{},1,10);
    REQUIRE(sent.status==SendStatus::Accepted);
    transfer(p.client,p.server,1);auto request=p.server.pop();REQUIRE(request);
    p.client.tick(11);
    auto events=p.client.take_events();REQUIRE(events.size()==1);CHECK(events[0].reason=="Timeout");
    CHECK(p.server.reply(*request,"Ready",config,11)==SendStatus::Accepted);
    transfer(p.server,p.client,11);
    CHECK_FALSE(p.client.pop());
    events=p.client.take_events();REQUIRE(events.size()==1);CHECK(events[0].reason=="LateResponse");
    REQUIRE(p.client.send("Configure",config,{},12,1).status==SendStatus::Accepted);
    CHECK_FALSE(p.client.next_wire(13)); // Expired before send: not emitted.
    p.client.take_events();
    REQUIRE(p.client.send("Configure",config,{},14).status==SendStatus::Accepted);
    p.client.disconnect();
    events=p.client.take_events();REQUIRE(events.size()==1);CHECK(events[0].reason=="DisconnectedUnknown");

    Limits l;l.handshake_ns=100;l.partial_ns=10;
    Channel s("run","worker",7,token,false,l,0);
    const auto wire=frame(encode_message(message("Hello",hello_payload(token))));
    s.feed(std::string_view(wire).substr(0,1),1);
    s.feed(std::string_view(wire).substr(1,1),9);
    s.tick(11);
    CHECK(s.snapshot().failure=="PartialFrameTimeout"); // No resetting deadline for trickle.
    Channel idle("run","worker",7,token,false,l,0);idle.tick(100);
    CHECK(idle.snapshot().failure=="HandshakeTimeout");
}
TEST_CASE("control lanes survive overload, sequence assigned at send, receive and request limits hold") {
    Limits l;l.normal_count=1;l.pending_count=1;l.receive_count=1;
    Pair p(l);
    REQUIRE(p.client.send("Configure",config,{},1).status==SendStatus::Accepted);
    CHECK(p.client.send("Configure",config,{},1).status==SendStatus::Full);
    CHECK(p.client.send("Stop",R"({"reason":"urgent"})",{},1).status==SendStatus::Accepted);
    auto wire=p.client.next_wire(1);REQUIRE(wire);
    p.server.feed(*wire,1);
    auto first=p.server.pop();REQUIRE(first);CHECK(first->header.type=="Stop");CHECK(first->header.sequence==2);
    transfer(p.client,p.server,1);
    auto second=p.server.pop();REQUIRE(second);CHECK(second->header.sequence==3);
    CHECK_FALSE(p.server.snapshot().closed);
    CHECK(p.client.send("Heartbeat",R"({"progress_sequence":"1"})",{},1).status==SendStatus::Accepted);
    CHECK(p.client.send("Heartbeat",R"({"progress_sequence":"2"})",{},1).status==SendStatus::Accepted);
    transfer(p.client,p.server,1);
    CHECK(p.server.snapshot().failure=="ReceiveQueueFull");
}
TEST_CASE("wrong ready revision and correlated response close rather than completing pending") {
    Pair p;
    const auto sent=p.client.send("Configure",config,{},1);
    transfer(p.client,p.server,1);auto request=p.server.pop();REQUIRE(request);
    const auto wrong=J{{"recipe_hash",hash},{"config_revision","2"}}.dump();
    CHECK(p.server.reply(*request,"Ready",wrong,1)==SendStatus::Invalid);
    auto malicious=message("Ready",wrong);
    malicious.header.request_id=sent.request_id;malicious.header.sequence=2;
    p.client.feed(frame(encode_message(malicious)),1);
    CHECK(p.client.snapshot().failure=="ResponseMismatch");
    REQUIRE(p.client.take_events().size()==1);
}
TEST_CASE("session request history limit does not disable stop or heartbeats") {
    Limits l;l.session_requests=1;
    Pair p(l);
    REQUIRE(p.client.send("Configure",config,{},1).status==SendStatus::Accepted);
    transfer(p.client,p.server,1);auto request=p.server.pop();REQUIRE(request);
    REQUIRE(p.server.reply(*request,"Ready",config,1)==SendStatus::Accepted);
    transfer(p.server,p.client,1);REQUIRE(p.client.pop());
    CHECK(p.client.send("Configure",config,{},2).status==SendStatus::Full);
    CHECK(p.client.send("Heartbeat",R"({"progress_sequence":"1"})",{},2).status==SendStatus::Accepted);
    CHECK(p.client.send("Stop",R"({"reason":"renew connection"})",{},2).status==SendStatus::Accepted);
    transfer(p.client,p.server,2);
    REQUIRE(p.server.pop());REQUIRE(p.server.pop());
    CHECK_FALSE(p.server.snapshot().closed);
}
TEST_CASE("diagnostic overflow closes finitely and byte budgets differ from count budgets") {
    Limits l;l.pending_count=1;l.normal_bytes=1;l.control_count=1;l.control_bytes=1024;
    Pair p(l);
    CHECK(p.client.send("Configure",config,{},1).status==SendStatus::Full);
    CHECK(p.client.snapshot().pending==0);
    CHECK(p.client.send("Stop",R"({"reason":"stop"})",{},1).status==SendStatus::Accepted);
    CHECK(p.client.send("Stop",R"({"reason":"stop"})",{},1).status==SendStatus::Full);
    CHECK(p.client.send("Undefined","{}",{},1).status==SendStatus::Invalid);
    Limits small;small.pending_count=1;
    Pair q(small);
    for(std::uint64_t n=0;n<3;++n) {
        REQUIRE(q.client.send("Configure",config,{},n*2,1).status==SendStatus::Accepted);
        q.client.tick(n*2+1);
    }
    CHECK(q.client.snapshot().closed);
    CHECK(q.client.snapshot().failure=="DiagnosticQueueFull");
    const auto final_events=q.client.take_events();
    REQUIRE(final_events.size()==3);
    CHECK(final_events.back().request_id=="c-3");
    CHECK(final_events.back().reason=="DisconnectedUnknown");
}
TEST_CASE("terminal ownership survives queue overflow and disconnect before delivery") {
    for(bool task_request:{false,true}) {
    for(bool overflow:{false,true}) {
        Limits l;l.receive_count=1;
        Pair p(l);
        const auto sent=task_request?p.client.send("SubmitTask",task,correlation(),1,10):
            p.client.send("Configure",config,{},1,10);
        REQUIRE(sent.status==SendStatus::Accepted);
        transfer(p.client,p.server,1);auto request=p.server.pop();REQUIRE(request);
        if(overflow) {
            REQUIRE(p.server.send("Heartbeat",R"({"progress_sequence":"1"})",{},1).status==SendStatus::Accepted);
            transfer(p.server,p.client,1);
        }
        const auto reply=task_request?p.server.reply(*request,"TaskFinished",
            R"({"execution_state":"Succeeded","quality":"OK","result_ref":"result","error_code":null})",1):
            p.server.reply(*request,"Ready",config,1);
        REQUIRE(reply==SendStatus::Accepted);
        transfer(p.server,p.client,1);
        if(!overflow) {
            p.client.tick(11);
            CHECK(p.client.take_events().empty()); // Arrived on time; waiting for application delivery.
            CHECK(p.client.snapshot().pending==1);
            p.client.disconnect();
        }
        CHECK(p.client.snapshot().closed);
        auto events=p.client.take_events();REQUIRE(events.size()==1);
        CHECK(events[0].request_id==sent.request_id);
        CHECK(events[0].reason=="DisconnectedUnknown");
        CHECK_FALSE(p.client.pop());
    }
    }
}
TEST_CASE("delivered terminal settles once and duplicate cannot cause timeout") {
    Pair p;
    REQUIRE(p.client.send("Configure",config,{},1,10).status==SendStatus::Accepted);
    transfer(p.client,p.server,1);auto request=p.server.pop();REQUIRE(request);
    REQUIRE(p.server.reply(*request,"Ready",config,1)==SendStatus::Accepted);
    REQUIRE(p.server.reply(*request,"Ready",config,1)==SendStatus::Accepted);
    transfer(p.server,p.client,1);
    p.client.tick(11);
    auto events=p.client.take_events();REQUIRE(events.size()==1);
    CHECK(events[0].reason=="LateResponse");
    REQUIRE(p.client.pop());CHECK_FALSE(p.client.pop());
    p.client.disconnect();
    CHECK(p.client.take_events().empty());
}
TEST_CASE("capture contract rejects identity layout fields and fabricated quality") {
    using namespace vision::contracts;
    const FrameDescriptor frame{{RunId("run"),PoolId("pool"),{WorkerId("worker"),7},0,1,1},
        FrameId("frame"),{8,4,8,0,32,PixelFormat::Mono8}};
    const auto descriptor=J::parse(vision::serialization::encode_frame(frame,32));
    auto request=message("CaptureTask",J{{"slot_count",2},{"slot_bytes","32"},{"write_frame",descriptor},{"budget_ns","1000"}}.dump());
    request.correlation=correlation();
    const auto valid=J::parse(encode_message(request));
    for(int n=0;n<10;++n) {
        auto invalid=valid;
        switch(n) {
        case 0:invalid["payload"]["slot_count"]=0;break;
        case 1:invalid["payload"]["slot_count"]=4097;break;
        case 2:invalid["payload"]["slot_bytes"]="31";break;
        case 3:invalid["payload"]["write_frame"]["slot_id"]="2";break;
        case 4:invalid["payload"]["write_frame"]["worker_epoch"]="8";break;
        case 5:invalid["payload"]["write_frame"]["run_id"]="other";break;
        case 6:invalid["payload"]["write_frame"]["length"]="18446744073709551616";break;
        case 7:invalid["payload"]["budget_ns"]="0";break;
        case 8:invalid["payload"]["unexpected"]=true;break;
        case 9:invalid["correlation"]=nullptr;break;
        }
        CHECK_THROWS_AS(decode_message(invalid.dump()),ProtocolError);
    }
    auto terminal=message("CaptureFinished",J{{"execution_state","Succeeded"},{"frame",descriptor},{"error_code",nullptr}}.dump());
    terminal.correlation=correlation();
    CHECK_NOTHROW(encode_message(terminal));
    auto bad=J::parse(encode_message(terminal));bad["payload"]["quality"]="OK";
    CHECK_THROWS_AS(decode_message(bad.dump()),ProtocolError);
    bad=J::parse(encode_message(terminal));bad["payload"]["execution_state"]="Failed";
    CHECK_THROWS_AS(decode_message(bad.dump()),ProtocolError);
    Pair p;
    CHECK(p.client.send("CaptureTask",request.payload,correlation(),1,100).status==SendStatus::Accepted);
    transfer(p.client,p.server,1);auto r=p.server.pop();REQUIRE(r);
    CHECK(p.server.reply(*r,"TaskAccepted","{}",1)==SendStatus::Accepted);
    auto wrong=J::parse(terminal.payload);wrong["frame"]["frame_id"]="old-frame";
    CHECK(p.server.reply(*r,"CaptureFinished",wrong.dump(),1)==SendStatus::Invalid);
    CHECK(p.server.reply(*r,"TaskFinished",R"({"execution_state":"Succeeded","quality":"OK","result_ref":"r","error_code":null})",1)==SendStatus::Invalid);
    CHECK(p.server.reply(*r,"CaptureFinished",terminal.payload,1)==SendStatus::Accepted);
    CHECK(p.server.reply(*r,"CaptureFinished",terminal.payload,1)==SendStatus::Accepted);
    transfer(p.server,p.client,1);
    REQUIRE(p.client.pop()); // acceptance
    REQUIRE(p.client.pop()); // sole terminal
    CHECK_FALSE(p.client.pop());CHECK(p.client.snapshot().pending==0);
    const auto events=p.client.take_events();REQUIRE(events.size()==1);CHECK(events[0].reason=="LateResponse");
}
TEST_CASE("capture terminal pending ownership survives timeout boundary") {
    using namespace vision::contracts;
    FrameDescriptor frame{{RunId("run"),PoolId("pool"),{WorkerId("worker"),7},0,1,1},
        FrameId("frame"),{8,4,8,0,32,PixelFormat::Mono8}};
    auto payload=J{{"slot_count",2},{"slot_bytes","32"},
        {"write_frame",J::parse(vision::serialization::encode_frame(frame,32))},{"budget_ns","10"}}.dump();
    Pair p;REQUIRE(p.client.send("CaptureTask",payload,correlation(),1,10).status==SendStatus::Accepted);
    transfer(p.client,p.server,1);auto r=p.server.pop();REQUIRE(r);
    const auto failure=R"({"execution_state":"Cancelled","frame":null,"error_code":"CAMERA.CANCELLED"})";
    REQUIRE(p.server.reply(*r,"CaptureFinished",failure,5)==SendStatus::Accepted);
    transfer(p.server,p.client,5);p.client.tick(11);
    CHECK(p.client.take_events().empty());CHECK(p.client.snapshot().pending==1);
    p.client.disconnect();auto events=p.client.take_events();REQUIRE(events.size()==1);
    CHECK(events[0].reason=="DisconnectedUnknown");
}
TEST_CASE("inspection request validates consumer identity and matches only algorithm terminals") {
    using namespace vision::contracts;
    FrameDescriptor frame{{RunId("run"),PoolId("pool"),{WorkerId("worker"),7},0,1,2},
        FrameId("frame"),{8,4,8,0,32,PixelFormat::Mono8}};
    auto payload=J{{"slot_count",2},{"slot_bytes","32"},
        {"read_frame",J::parse(vision::serialization::encode_frame(frame,32))},{"budget_ns","10"}};
    auto request=message("InspectTask",payload.dump());request.correlation=correlation();
    CHECK_NOTHROW(encode_message(request));
    for(const auto* field:{"run_id","worker_id","worker_epoch"}) {
        auto bad=payload;bad["read_frame"][field]=field==std::string("worker_epoch")?"8":"other";
        request.payload=bad.dump();CHECK_THROWS_AS(encode_message(request),ProtocolError);
    }
    Pair p;REQUIRE(p.client.send("InspectTask",payload.dump(),correlation(),1,10).status==SendStatus::Accepted);
    transfer(p.client,p.server,1);auto r=p.server.pop();REQUIRE(r);
    CHECK(p.server.reply(*r,"CaptureFinished",R"({"execution_state":"Failed","frame":null,"error_code":"NO"})",2)==SendStatus::Invalid);
    const auto ok=R"({"execution_state":"Succeeded","quality":"OK","result_ref":"frame","error_code":null})";
    CHECK(p.server.reply(*r,"TaskFinished",ok,2)==SendStatus::Accepted);
    CHECK(p.server.reply(*r,"TaskFinished",ok,2)==SendStatus::Accepted);
    transfer(p.server,p.client,2);REQUIRE(p.client.pop());CHECK_FALSE(p.client.pop());
    CHECK(p.client.snapshot().pending==0);
    const auto events=p.client.take_events();REQUIRE(events.size()==1);CHECK(events[0].reason=="LateResponse");
}
TEST_CASE("device replies bind session sequence and reject inconsistent physical result") {
    auto payload=J{{"session_id","session"},{"command_sequence","1"},{"ready",true},{"arrival",false},
        {"position",true},{"safety_ok",true},{"result_id",nullptr},{"quality","Unknown"}};
    auto m=message("DeviceTask",payload.dump());m.correlation=correlation();
    CHECK_NOTHROW(encode_message(m));
    auto bad=payload;bad["quality"]="OK";m.payload=bad.dump();CHECK_THROWS_AS(encode_message(m),ProtocolError);
    Pair p;REQUIRE(p.client.send("DeviceTask",payload.dump(),correlation(),1,100).status==SendStatus::Accepted);
    transfer(p.client,p.server,1);auto request=p.server.pop();REQUIRE(request);
    auto reply=J{{"session_id","old"},{"command_sequence","1"},{"ready",true},{"arrival_sequence","1"},
        {"position",true},{"safety_ok",true},{"ack_id",nullptr}};
    CHECK(p.server.reply(*request,"DeviceFinished",reply.dump(),2)==SendStatus::Invalid);
    reply["session_id"]="session";reply["command_sequence"]="2";
    CHECK(p.server.reply(*request,"DeviceFinished",reply.dump(),2)==SendStatus::Invalid);
    reply["command_sequence"]="1";CHECK(p.server.reply(*request,"DeviceFinished",reply.dump(),2)==SendStatus::Accepted);
    transfer(p.server,p.client,2);REQUIRE(p.client.pop());CHECK(p.client.snapshot().pending==0);
}
TEST_CASE("delivery replies bind event output attempt and require correlation") {
    auto event=*vision::application::run_demo(vision::application::DemoScenario::Ok).event;
    const auto payload=J{{"event",J::parse(vision::serialization::encode_result(event))},
        {"output_instance_id","output"},{"attempt","1"},{"budget_ns","100"}};
    Pair p;REQUIRE(p.client.send("DeliveryTask",payload.dump(),correlation(),1,100).status==SendStatus::Accepted);
    transfer(p.client,p.server,1);auto request=p.server.pop();REQUIRE(request);
    auto report=J{{"event_id",event.event_id.value()},{"output_instance_id","output"},{"attempt","1"},
        {"state","BusinessAcked"},{"error_code",nullptr}};
    for(const auto* field:{"event_id","output_instance_id","attempt"}) {
        auto bad=report;bad[field]=field==std::string("attempt")?"2":"other";
        CHECK(p.server.reply(*request,"DeliveryFinished",bad.dump(),2)==SendStatus::Invalid);
    }
    auto no_identity=message("DeliveryFinished",report.dump());
    CHECK_THROWS_AS(encode_message(no_identity),ProtocolError);
    CHECK(p.server.reply(*request,"DeliveryFinished",report.dump(),2)==SendStatus::Accepted);
    CHECK(p.server.reply(*request,"DeliveryFinished",report.dump(),2)==SendStatus::Accepted);
    transfer(p.server,p.client,2);REQUIRE(p.client.pop());CHECK_FALSE(p.client.pop());
    CHECK(p.client.take_events().size()==1);
}
