#include <vision/application/qt/dispatcher.hpp>
#include <vision/serialization/json_codec.hpp>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <chrono>
#include <set>
#include <nlohmann/json.hpp>
namespace vision::application::qt {
namespace {
std::uint64_t utc() {return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());}
bool terminal(const std::string& state) {return state=="BusinessAcked"||state=="Failed"||state=="Unknown"||state=="Expired";}
runtime::SupervisorPolicy policy() {runtime::SupervisorPolicy p;p.restart_limit=0;return p;}
std::string json(QJsonObject p) {return QJsonDocument(p).toJson(QJsonDocument::Compact).toStdString();}
}
Dispatcher::Dispatcher(std::shared_ptr<runtime::qt::StationGuard> guard,QString host,std::vector<OutputConfig> configs) {
    if(!guard||configs.empty()||configs.size()>8)throw std::invalid_argument("Output count");
    std::set<std::string> ids;
    for(auto& config:configs) {
        (void)contracts::OutputId(config.id);
        if(config.id.size()>100||!ids.insert(config.id).second||!config.capacity||config.capacity>128)
            throw std::invalid_argument("Output configuration");
        Lane lane;lane.config=std::move(config);
        lane.host=std::make_unique<runtime::qt::ProcessHost>(guard,host,QStringList{lane.config.manifest,QString::fromStdString(lane.config.parameters)},
            "output-"+lane.config.id,"{\"recipe_hash\":\"sha256:"+std::string(64,'a')+"\",\"config_revision\":\"1\"}",policy());
        lane.host->on_task_message=[this,index=lanes_.size()](const ipc::Message& m){message(index,m);};
        lanes_.push_back(std::move(lane));
    }
}
Dispatcher::~Dispatcher() {stop();for(auto& lane:lanes_)lane.host->on_task_message={};}
bool Dispatcher::start() {
    if(started_||stopped_)return false;
    started_=true;bool ok=true;for(auto& lane:lanes_)ok=lane.host->start()&&ok;return ok;
}
bool Dispatcher::ready() const {
    if(!started_||stopped_)return false;
    for(const auto& lane:lanes_)if(!lane.failed&&lane.host->ready_for_task())return true;
    return false;
}
bool Dispatcher::submit(const contracts::ResultEnvelope& event,std::uint64_t ttl) {
    if(!started_||stopped_||!ttl||ttl>10000000000ULL||records_.size()>=128||event.result.mode!=contracts::Mode::Demo||
       records_.contains(event.event_id.value()))return false;
    try {if(serialization::encode_result(event).size()>32768)return false;}catch(...) {return false;}
    Record record{event,{}};
    for(std::size_t i=0;i<lanes_.size();++i) {
        std::size_t pending=0;
        for(const auto& [id,r]:records_) {(void)id;if(!terminal(r.intents[i].delivery.state))++pending;}
        const auto full=pending>=lanes_[i].config.capacity||lanes_[i].failed;
        record.intents.push_back({{event.event_id.value(),lanes_[i].config.id,full?"Failed":"Pending",
            full?"OUTPUT.CAPACITY_OR_UNAVAILABLE":"",0},contracts::checked_add(utc(),ttl),runtime::deadline_after(clock_,ttl)});
    }
    records_.emplace(event.event_id.value(),std::move(record));return true;
}
void Dispatcher::failed(std::size_t i) {
    auto& lane=lanes_[i];lane.failed=true;
    if(lane.active) {
        auto& d=records_.at(lane.event).intents[i].delivery;
        d.state="Unknown";d.error="OUTPUT.OUTCOME_UNKNOWN";lane.active.reset();
    }
    lane.host->stop();
    for(auto& [id,r]:records_) {(void)id;auto& d=r.intents[i].delivery;
        if(d.state=="Pending") {d.state="Failed";d.error="OUTPUT.UNAVAILABLE";}}
}
void Dispatcher::pulse() {
    const auto now=clock_.now_ns();
    for(std::size_t i=0;i<lanes_.size();++i) {
        auto& lane=lanes_[i];lane.host->pulse_control();
        if(stopped_||lane.failed)continue;
        if((lane.active&&now>=lane.due)||lane.host->snapshot().phase==runtime::WorkerPhase::ManualIntervention) {failed(i);continue;}
        for(auto& [id,record]:records_) {
            auto& intent=record.intents[i];auto& d=intent.delivery;
            if(d.state!="Pending")continue;
            if(now>=intent.due) {d.state="Expired";d.error="OUTPUT.EXPIRED";continue;}
            if(lane.active||!lane.host->ready_for_task())break;
            if(sequence_==UINT64_MAX||d.attempt==UINT64_MAX) {failed(i);break;}
            const auto budget=std::min<std::uint64_t>(intent.due-now,1000000000);
            contracts::Correlation identity{contracts::RunId(lane.host->run_id()),contracts::WorkerId("output-"+lane.config.id),
                lane.host->snapshot().epoch,record.event.result.inspection_id,contracts::CheckId("delivery"),
                contracts::TaskId("delivery-"+std::to_string(++sequence_)),1};
            const auto attempt=d.attempt+1;
            const auto payload=json({{"event",QJsonDocument::fromJson(QByteArray::fromStdString(serialization::encode_result(record.event))).object()},
                {"output_instance_id",QString::fromStdString(lane.config.id)},{"attempt",QString::number(attempt)},{"budget_ns",QString::number(budget)}});
            lane.due=runtime::deadline_after(clock_,budget);
            if(lane.host->deliver(identity,payload,budget)!=ipc::SendStatus::Accepted) {failed(i);break;}
            lane.active=std::move(identity);lane.event=id;d.attempt=attempt;d.state="Accepted";break;
        }
    }
}
void Dispatcher::message(std::size_t i,const ipc::Message& m) {
    auto& lane=lanes_[i];
    if(m.header.type!="DeliveryFinished"||!lane.active||m.correlation!=lane.active||lane.failed||stopped_)return;
    if(clock_.now_ns()>=lane.due) {failed(i);return;}
    const auto p=QJsonDocument::fromJson(QByteArray::fromStdString(m.payload)).object();
    const auto state=p["state"].toString().toStdString();
    if(!terminal(state)) {failed(i);return;}
    auto& d=records_.at(lane.event).intents[i].delivery;
    d.state=state;d.error=p["error_code"].toString().toStdString();lane.active.reset();
}
void Dispatcher::stop() {
    if(stopped_)return;
    stopped_=true;for(std::size_t i=0;i<lanes_.size();++i)failed(i);
}
bool Dispatcher::idle() const {
    for(const auto& [id,r]:records_) {(void)id;for(const auto& intent:r.intents)if(!terminal(intent.delivery.state))return false;}
    return true;
}
std::vector<Delivery> Dispatcher::deliveries() const {
    std::vector<Delivery> result;for(const auto& [id,r]:records_) {(void)id;for(const auto& intent:r.intents)result.push_back(intent.delivery);}
    return result;
}
std::vector<runtime::WorkerSnapshot> Dispatcher::workers() const {
    std::vector<runtime::WorkerSnapshot> result;for(const auto& lane:lanes_)result.push_back(lane.host->snapshot());return result;
}
std::string Dispatcher::checkpoint() const {
    QJsonArray rows;
    for(const auto& [id,r]:records_) {
        (void)id;QJsonArray intents;
        for(const auto& intent:r.intents) {
            const auto& d=intent.delivery;
            intents.append(QJsonObject{{"output",QString::fromStdString(d.output_id)},{"state",QString::fromStdString(d.state)},
                {"attempt",QString::number(d.attempt)},{"expires_utc",QString::number(intent.expires_utc)},{"error",QString::fromStdString(d.error)}});
        }
        rows.append(QJsonObject{{"event",QJsonDocument::fromJson(QByteArray::fromStdString(serialization::encode_result(r.event))).object()},{"intents",intents}});
    }
    return json({{"version",1},{"durability","Volatile"},{"rows",rows}});
}
bool Dispatcher::restore(std::string_view text) {
    if(started_||stopped_||!records_.empty()||text.size()>8*1024*1024)return false;
    try {
        std::vector<std::set<std::string>> keys;
        const auto strict=nlohmann::json::parse(text.begin(),text.end(),[&](int depth,nlohmann::json::parse_event_t event,nlohmann::json& value) {
            if(depth>32)throw std::invalid_argument("Checkpoint depth");
            if(event==nlohmann::json::parse_event_t::object_start)keys.emplace_back();
            if(event==nlohmann::json::parse_event_t::key&&!keys.back().insert(value.get<std::string>()).second)
                throw std::invalid_argument("Duplicate checkpoint key");
            if(event==nlohmann::json::parse_event_t::object_end)keys.pop_back();
            return true;
        });
        if(!strict.is_object())return false;
        const auto root=QJsonDocument::fromJson(QByteArray(text.data(),static_cast<int>(text.size()))).object();
        if(root.size()!=3||root["version"]!=1||root["durability"]!="Volatile"||!root["rows"].isArray()||root["rows"].toArray().size()>128)return false;
        std::map<std::string,Record> pending;
        for(const auto& value:root["rows"].toArray()) {
            const auto row=value.toObject();
            if(row.size()!=2||!row["intents"].isArray()||row["intents"].toArray().size()!=static_cast<qsizetype>(lanes_.size()))return false;
            Record record{serialization::decode_result(json(row["event"].toObject())),{}};
            if(record.event.result.mode!=contracts::Mode::Demo||serialization::encode_result(record.event).size()>32768)return false;
            for(std::size_t i=0;i<lanes_.size();++i) {
                const auto item=row["intents"].toArray()[static_cast<qsizetype>(i)].toObject();
                if(item.size()!=5||item["output"].toString().toStdString()!=lanes_[i].config.id)return false;
                auto state=item["state"].toString().toStdString();
                if(!terminal(state)&&state!="Pending"&&state!="Accepted")return false;
                const auto attempt=contracts::parse_u64(item["attempt"].toString().toStdString());
                const auto expiry=contracts::parse_u64(item["expires_utc"].toString().toStdString());
                if(attempt==UINT64_MAX||!item["error"].isString()||item["error"].toString().size()>128||
                   ((state=="Accepted"||state=="BusinessAcked"||state=="Unknown")&&!attempt))return false;
                const auto wall=utc();
                if(expiry>wall&&expiry-wall>10000000000ULL)return false;
                // Only an explicit restore replays uncertain nonphysical deliveries; same key, new attempt.
                if(state=="Accepted"||state=="Unknown")state="Pending";
                record.intents.push_back({{record.event.event_id.value(),lanes_[i].config.id,state,item["error"].toString().toStdString(),attempt},
                    expiry,runtime::deadline_after(clock_,expiry>wall?expiry-wall:0)});
            }
            const auto id=record.event.event_id.value();
            if(!pending.emplace(id,std::move(record)).second)return false;
        }
        for(std::size_t i=0;i<lanes_.size();++i) {
            std::size_t count=0;for(const auto& [id,r]:pending) {(void)id;if(!terminal(r.intents[i].delivery.state))++count;}
            if(count>lanes_[i].config.capacity)return false;
        }
        records_=std::move(pending);return true;
    }catch(...) {return false;}
}
}
