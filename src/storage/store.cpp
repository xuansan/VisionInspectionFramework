#include <vision/storage/store.hpp>
#include <vision/serialization/json_codec.hpp>
#include <vision/inference/engine.hpp>
#include <sqlite3.h>
#include <set>
#include <thread>
#define NOMINMAX
#include <windows.h>
namespace vision::storage {
namespace {
void image_id(const std::string& id) {
    (void)contracts::FrameId(id);
    if(id.find(':')!=std::string::npos||id=="."||id=="..")throw std::invalid_argument("Image filename");
}
std::uint64_t utc_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
struct Statement {
    sqlite3_stmt* s{};
    Statement(sqlite3* db,const char* sql){if(sqlite3_prepare_v2(db,sql,-1,&s,nullptr)!=SQLITE_OK)throw std::runtime_error("Storage prepare");}
    ~Statement(){sqlite3_finalize(s);}
    void text(int n,const std::string& v){if(sqlite3_bind_text(s,n,v.data(),static_cast<int>(v.size()),SQLITE_TRANSIENT)!=SQLITE_OK)throw std::runtime_error("Storage bind");}
    void number(int n,std::uint64_t v){if(v>INT64_MAX||sqlite3_bind_int64(s,n,static_cast<sqlite3_int64>(v))!=SQLITE_OK)throw std::invalid_argument("Storage integer");}
    bool step(){const auto result=sqlite3_step(s);if(result!=SQLITE_ROW&&result!=SQLITE_DONE)throw std::runtime_error("Storage step code="+std::to_string(result));return result==SQLITE_ROW;}
    std::string value(int n){const auto* text=sqlite3_column_text(s,n);return text?reinterpret_cast<const char*>(text):"";}
};
}
struct Store::Impl {
    sqlite3* db{};std::filesystem::path root;std::uint64_t quota;std::thread::id owner=std::this_thread::get_id();
    ~Impl(){if(db)sqlite3_close_v2(db);}
    void exec(const char* sql){if(sqlite3_exec(db,sql,nullptr,nullptr,nullptr)!=SQLITE_OK)
        throw std::runtime_error("Storage transaction code="+std::to_string(sqlite3_extended_errcode(db)));}
    void check(){if(owner!=std::this_thread::get_id())throw std::runtime_error("Storage single writer");
        std::uint64_t bytes=0;unsigned count=0;
        for(const auto& e:std::filesystem::directory_iterator(root)) {
            if(++count>4096||!e.is_regular_file()||e.is_symlink())throw std::runtime_error("Storage directory bounds");
            bytes+=e.file_size();if(bytes>quota)throw std::runtime_error("Storage quota");
        }
    }
};
Store::Store(std::filesystem::path root,std::uint64_t quota):impl_(std::make_unique<Impl>()) {
    if(quota<1024*1024||quota>1024ULL*1024*1024||!std::filesystem::is_directory(root)||std::filesystem::is_symlink(root))
        throw std::invalid_argument("Storage root/quota");
    impl_->root=std::move(root);impl_->quota=quota;impl_->check();
    const auto path=(impl_->root/"results.sqlite").u8string();
    if(sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()),&impl_->db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_NOMUTEX,nullptr)!=SQLITE_OK)
        throw std::runtime_error("Storage open");
    sqlite3_busy_timeout(impl_->db,50);sqlite3_limit(impl_->db,SQLITE_LIMIT_LENGTH,1024*1024);
    {Statement version(impl_->db,"PRAGMA user_version");if(!version.step()||sqlite3_column_int(version.s,0)>4)throw std::runtime_error("Storage schema version");}
    impl_->exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON; PRAGMA wal_autocheckpoint=32; PRAGMA journal_size_limit=1048576;");
    const auto pages="PRAGMA max_page_count="+std::to_string((quota-128*1024)/4096);impl_->exec(pages.c_str());
    impl_->exec("BEGIN IMMEDIATE;"
        "CREATE TABLE IF NOT EXISTS inspections(cursor INTEGER PRIMARY KEY AUTOINCREMENT,event TEXT UNIQUE NOT NULL,run TEXT NOT NULL,inspection TEXT NOT NULL,body TEXT NOT NULL,routes TEXT NOT NULL,UNIQUE(run,inspection));"
        "CREATE TABLE IF NOT EXISTS intents(event TEXT NOT NULL REFERENCES inspections(event),output TEXT NOT NULL,state TEXT NOT NULL,attempt INTEGER NOT NULL,PRIMARY KEY(event,output));"
        "CREATE TABLE IF NOT EXISTS running(run TEXT NOT NULL,inspection TEXT NOT NULL,state TEXT NOT NULL,PRIMARY KEY(run,inspection));"
        "CREATE TABLE IF NOT EXISTS images(id TEXT PRIMARY KEY,hash TEXT NOT NULL,bytes INTEGER NOT NULL,state TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS delivery_leases(event TEXT NOT NULL,output TEXT NOT NULL,expires INTEGER NOT NULL,lease_until INTEGER NOT NULL,token TEXT NOT NULL,PRIMARY KEY(event,output),FOREIGN KEY(event,output) REFERENCES intents(event,output));"
        // Earlier records have no trustworthy delivery TTL. They are expired, never blindly replayed.
        "INSERT OR IGNORE INTO delivery_leases SELECT event,output,0,0,'' FROM intents;"
        "CREATE TABLE IF NOT EXISTS upload_jobs(image TEXT PRIMARY KEY REFERENCES images(id),endpoint TEXT NOT NULL,bucket TEXT NOT NULL,object_key TEXT NOT NULL,attempt INTEGER NOT NULL,UNIQUE(endpoint,bucket,object_key));"
        "CREATE TABLE IF NOT EXISTS archived_frames(image TEXT PRIMARY KEY REFERENCES images(id),run TEXT NOT NULL,inspection TEXT NOT NULL,channel TEXT NOT NULL,descriptor TEXT NOT NULL,recipe_hash TEXT NOT NULL,recipe_body TEXT NOT NULL,UNIQUE(run,inspection,channel));"
        "PRAGMA user_version=4;COMMIT;");
}
Store::~Store()=default;
std::string Store::archive_frame(const contracts::FrameDescriptor& frame,std::uint64_t slot_bytes,
    std::string inspection,std::string channel,std::string recipe_hash,std::string recipe_body,std::span<const std::byte> pixels) {
    impl_->check();(void)contracts::InspectionId(inspection);(void)contracts::CheckId(channel);
    const auto descriptor=serialization::encode_frame(frame,slot_bytes);
    if(pixels.size()!=frame.layout.length||recipe_body.empty()||recipe_body.size()>32768||
        inference::sha256(std::as_bytes(std::span(recipe_body.data(),recipe_body.size())))!=recipe_hash)
        throw std::invalid_argument("Archive pixels/recipe");
    const auto identity=frame.permit.run.value()+"\n"+inspection+"\n"+channel;
    const auto id="frame-"+inference::sha256(std::as_bytes(std::span(identity.data(),identity.size()))).substr(7);
    // Flush pixels before publishing their association. Failed metadata may leave inspectable staging.
    stage_image(id,pixels);
    impl_->exec("BEGIN IMMEDIATE");
    try {
        Statement authority(impl_->db,"SELECT body FROM inspections WHERE run=? AND inspection=?");
        authority.text(1,frame.permit.run.value());authority.text(2,inspection);
        if(authority.step()&&serialization::decode_result(authority.value(0)).result.recipe_hash!=recipe_hash)
            throw std::runtime_error("Archive result recipe mismatch");
        Statement old(impl_->db,"SELECT descriptor,recipe_hash,recipe_body FROM archived_frames WHERE image=?");old.text(1,id);
        if(old.step()){
            if(old.value(0)!=descriptor||old.value(1)!=recipe_hash||old.value(2)!=recipe_body)throw std::runtime_error("Archive identity conflict");
        }else{
            Statement insert(impl_->db,"INSERT INTO archived_frames VALUES(?,?,?,?,?,?,?)");
            insert.text(1,id);insert.text(2,frame.permit.run.value());insert.text(3,inspection);insert.text(4,channel);
            insert.text(5,descriptor);insert.text(6,recipe_hash);insert.text(7,recipe_body);insert.step();
        }
        impl_->exec("COMMIT");return id;
    }catch(...){sqlite3_exec(impl_->db,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
}
std::vector<ArchivedFrame> Store::frames(std::string run,std::string inspection) {
    impl_->check();(void)contracts::RunId(run);(void)contracts::InspectionId(inspection);
    Statement q(impl_->db,"SELECT image,channel,descriptor,recipe_hash,recipe_body FROM archived_frames WHERE run=? AND inspection=? ORDER BY channel LIMIT 64");
    q.text(1,run);q.text(2,inspection);std::vector<ArchivedFrame> result;
    while(q.step())result.push_back({q.value(0),q.value(1),q.value(2),q.value(3),q.value(4)});return result;
}
bool Store::commit(const contracts::ResultEnvelope& event,const std::vector<std::string>& outputs) {
    impl_->check();const auto body=serialization::encode_result(event);if(body.size()>65536||outputs.size()>8)throw std::invalid_argument("Storage record bound");
    std::set<std::string> unique;std::string routes;
    for(const auto& output:outputs){(void)contracts::OutputId(output);if(output.starts_with("plc")||!unique.insert(output).second)throw std::invalid_argument("Nonphysical unique routes");routes+=output+"\n";}
    impl_->exec("BEGIN IMMEDIATE");
    try {
        Statement recipe(impl_->db,"SELECT 1 FROM archived_frames WHERE run=? AND inspection=? AND recipe_hash<>? LIMIT 1");
        recipe.text(1,event.result.run_id.value());recipe.text(2,event.result.inspection_id.value());recipe.text(3,event.result.recipe_hash);
        if(recipe.step())throw std::runtime_error("Result archive recipe mismatch");
        Statement existing(impl_->db,"SELECT body,routes FROM inspections WHERE event=?");existing.text(1,event.event_id.value());
        if(existing.step()){if(existing.value(0)!=body||existing.value(1)!=routes)throw std::runtime_error("Result identity conflict");impl_->exec("ROLLBACK");return false;}
        Statement row(impl_->db,"INSERT INTO inspections(event,run,inspection,body,routes) VALUES(?,?,?,?,?)");
        row.text(1,event.event_id.value());row.text(2,event.result.run_id.value());row.text(3,event.result.inspection_id.value());row.text(4,body);row.text(5,routes);row.step();
        for(const auto& output:outputs){
            Statement intent(impl_->db,"INSERT INTO intents VALUES(?,?,'Pending',0)");intent.text(1,event.event_id.value());intent.text(2,output);intent.step();
            Statement lease(impl_->db,"INSERT INTO delivery_leases VALUES(?,?,?,0,'')");
            lease.text(1,event.event_id.value());lease.text(2,output);lease.number(3,utc_ms()+60000);lease.step();
        }
        Statement done(impl_->db,"UPDATE running SET state='Completed' WHERE run=? AND inspection=?");done.text(1,event.result.run_id.value());done.text(2,event.result.inspection_id.value());done.step();
        impl_->exec("COMMIT");return true;
    }catch(...){sqlite3_exec(impl_->db,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
}
std::vector<std::string> Store::query(std::uint64_t after,unsigned limit) {
    impl_->check();if(!limit||limit>200)throw std::invalid_argument("Query limit");
    Statement q(impl_->db,"SELECT body FROM inspections WHERE cursor>? ORDER BY cursor LIMIT ?");q.number(1,after);q.number(2,limit);
    std::vector<std::string> values;while(q.step())values.push_back(q.value(0));return values;
}
std::vector<Intent> Store::pending(unsigned limit) {
    impl_->check();if(!limit||limit>200)throw std::invalid_argument("Intent limit");
    Statement q(impl_->db,"SELECT event,output,state,attempt FROM intents WHERE state='Pending' ORDER BY event,output LIMIT ?");q.number(1,limit);
    std::vector<Intent> values;while(q.step())values.push_back({q.value(0),q.value(1),q.value(2),static_cast<unsigned>(sqlite3_column_int(q.s,3))});return values;
}
DeliveryTotals Store::delivery_totals(std::string output) {
    impl_->check();(void)contracts::OutputId(output);
    Statement q(impl_->db,"SELECT COUNT(*),COALESCE(SUM(state='BusinessAcked'),0),COALESCE(SUM(state='Pending'),0) FROM intents WHERE output=?");
    q.text(1,output);if(!q.step())throw std::runtime_error("Delivery summary missing");
    DeliveryTotals result{static_cast<std::uint64_t>(sqlite3_column_int64(q.s,0)),
        static_cast<std::uint64_t>(sqlite3_column_int64(q.s,1)),static_cast<std::uint64_t>(sqlite3_column_int64(q.s,2)),0};
    result.unconfirmed=result.total-result.acknowledged;return result;
}
void Store::observe(std::string event,std::string output,unsigned attempt,std::string state) {
    impl_->check();if(!attempt||attempt>10||(state!="Pending"&&state!="BusinessAcked"&&state!="Failed"&&state!="Expired"))throw std::invalid_argument("Delivery state");
    Statement q(impl_->db,"UPDATE intents SET state=?,attempt=? WHERE event=? AND output=? AND state='Pending' AND attempt<? AND EXISTS(SELECT 1 FROM delivery_leases l WHERE l.event=intents.event AND l.output=intents.output AND l.token='')");
    q.text(1,state);q.number(2,attempt);q.text(3,event);q.text(4,output);q.number(5,attempt);q.step();
    if(sqlite3_changes(impl_->db)!=1)throw std::runtime_error("Stale delivery observation");
}
void Store::begin_inspection(std::string run,std::string inspection) {
    impl_->check();(void)contracts::RunId(run);(void)contracts::InspectionId(inspection);
    Statement q(impl_->db,"INSERT INTO running VALUES(?,?,'Running')");q.text(1,run);q.text(2,inspection);q.step();
}
std::optional<ClaimedIntent> Store::claim(std::string output,std::string token,std::uint64_t now) {
    impl_->check();(void)contracts::OutputId(output);
    if(token.empty()||token.size()>256||now>INT64_MAX-3000)throw std::invalid_argument("Delivery lease");
    impl_->exec("BEGIN IMMEDIATE");
    try {
        Statement expire(impl_->db,"UPDATE intents SET state='Expired' WHERE output=? AND state='Pending' AND EXISTS(SELECT 1 FROM delivery_leases l WHERE l.event=intents.event AND l.output=intents.output AND l.expires<=?)");
        expire.text(1,output);expire.number(2,now);expire.step();
        Statement exhausted(impl_->db,"UPDATE intents SET state='Failed' WHERE output=? AND state='Pending' AND attempt>=3 AND EXISTS(SELECT 1 FROM delivery_leases l WHERE l.event=intents.event AND l.output=intents.output AND l.lease_until<=?)");
        exhausted.text(1,output);exhausted.number(2,now);exhausted.step();
        std::optional<ClaimedIntent> result;
        {
            Statement q(impl_->db,"SELECT i.event,r.body,i.attempt,l.expires FROM intents i JOIN inspections r ON r.event=i.event JOIN delivery_leases l ON l.event=i.event AND l.output=i.output WHERE i.output=? AND i.state='Pending' AND i.attempt<3 AND l.lease_until<=? ORDER BY r.cursor LIMIT 1");
            q.text(1,output);q.number(2,now);
            if(q.step())result=ClaimedIntent{q.value(0),output,q.value(1),token,static_cast<unsigned>(sqlite3_column_int(q.s,2))+1,static_cast<std::uint64_t>(sqlite3_column_int64(q.s,3))};
        }
        if(result){
            Statement lease(impl_->db,"UPDATE delivery_leases SET token=?,lease_until=? WHERE event=? AND output=?");
            lease.text(1,token);lease.number(2,std::min(now+3000,result->expires_ms));lease.text(3,result->event);lease.text(4,output);lease.step();
            Statement attempt(impl_->db,"UPDATE intents SET attempt=? WHERE event=? AND output=?");
            attempt.number(1,result->attempt);attempt.text(2,result->event);attempt.text(3,output);attempt.step();
        }
        impl_->exec("COMMIT");return result;
    }catch(...){sqlite3_exec(impl_->db,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
}
void Store::settle(const ClaimedIntent& intent,std::string state,std::uint64_t now) {
    impl_->check();
    if(state!="BusinessAcked"&&state!="Failed")throw std::invalid_argument("Terminal delivery state");
    Statement q(impl_->db,"UPDATE intents SET state=? WHERE event=? AND output=? AND state='Pending' AND attempt=? AND EXISTS(SELECT 1 FROM delivery_leases l WHERE l.event=intents.event AND l.output=intents.output AND l.token=? AND l.lease_until>? AND l.expires>?)");
    q.text(1,state);q.text(2,intent.event);q.text(3,intent.output);q.number(4,intent.attempt);q.text(5,intent.token);q.number(6,now);q.number(7,now);q.step();
    if(sqlite3_changes(impl_->db)!=1)throw std::runtime_error("Stale delivery receipt");
}
unsigned Store::recover_interrupted() {
    impl_->check();impl_->exec("UPDATE running SET state='Interrupted' WHERE state='Running'");return static_cast<unsigned>(sqlite3_changes(impl_->db));
}
std::string Store::stage_image(std::string id,std::span<const std::byte> bytes) {
    impl_->check();image_id(id);if(bytes.empty()||bytes.size()>16*1024*1024)throw std::invalid_argument("Image size");
    const auto hash=inference::sha256(bytes);const auto path=impl_->root/(id+".stage");
    Statement existing(impl_->db,"SELECT hash FROM images WHERE id=?");existing.text(1,id);
    if(existing.step()){if(existing.value(0)!=hash)throw std::runtime_error("Image conflict");(void)image(id);return hash;}
    std::uint64_t used=0;for(const auto& e:std::filesystem::directory_iterator(impl_->root))used+=e.file_size();
    if(bytes.size()>impl_->quota-used)throw std::runtime_error("Staging quota");
    if(std::filesystem::exists(path)) {
        if(inference::sha256(inference::read_file(path,16*1024*1024))!=hash)throw std::runtime_error("Orphan staging conflict");
    } else {
        const auto file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Stage create");
        DWORD written=0;const bool ok=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(file);
        CloseHandle(file);if(!ok)throw std::runtime_error("Stage flush");
    }
    Statement insert(impl_->db,"INSERT INTO images VALUES(?,?,?,'Pending')");insert.text(1,id);insert.text(2,hash);insert.number(3,bytes.size());insert.step();return hash;
}
std::vector<std::byte> Store::image(std::string id) {
    impl_->check();image_id(id);Statement q(impl_->db,"SELECT hash,bytes,state FROM images WHERE id=?");q.text(1,id);
    if(!q.step()||q.value(2)=="Deleted")throw std::runtime_error("Image unavailable");
    auto bytes=inference::read_file(impl_->root/(id+".stage"),16*1024*1024);
    if(inference::sha256(bytes)!=q.value(0)||bytes.size()!=static_cast<std::uint64_t>(sqlite3_column_int64(q.s,1)))throw std::runtime_error("Image integrity");
    return bytes;
}
void Store::bind_object(const UploadJob& job){
    impl_->check();image_id(job.image);
    if(job.endpoint.empty()||job.endpoint.size()>1024||job.bucket.empty()||job.bucket.size()>128||job.key.empty()||job.key.size()>256)
        throw std::invalid_argument("Object binding");
    Statement existing(impl_->db,"SELECT endpoint,bucket,object_key FROM upload_jobs WHERE image=?");existing.text(1,job.image);
    if(existing.step()){
        if(existing.value(0)!=job.endpoint||existing.value(1)!=job.bucket||existing.value(2)!=job.key)throw std::runtime_error("Object binding conflict");
        return;
    }
    Statement q(impl_->db,"INSERT INTO upload_jobs VALUES(?,?,?,?,0)");
    q.text(1,job.image);q.text(2,job.endpoint);q.text(3,job.bucket);q.text(4,job.key);q.step();
}
std::vector<UploadJob> Store::pending_uploads(unsigned limit){
    impl_->check();if(!limit||limit>20)throw std::invalid_argument("Upload query limit");
    Statement q(impl_->db,"SELECT j.image,j.endpoint,j.bucket,j.object_key,j.attempt FROM upload_jobs j JOIN images i ON i.id=j.image WHERE i.state='Pending' ORDER BY j.image LIMIT ?");q.number(1,limit);
    std::vector<UploadJob> jobs;
    while(q.step())jobs.push_back({q.value(0),q.value(1),q.value(2),q.value(3),static_cast<unsigned>(sqlite3_column_int(q.s,4))});
    return jobs;
}
unsigned Store::start_upload(std::string id){
    impl_->check();Statement q(impl_->db,"UPDATE upload_jobs SET attempt=attempt+1 WHERE image=? AND attempt<3 AND EXISTS(SELECT 1 FROM images i WHERE i.id=upload_jobs.image AND i.state='Pending') RETURNING attempt");
    q.text(1,id);if(!q.step())throw std::runtime_error("Upload attempt exhausted");
    const auto attempt=static_cast<unsigned>(sqlite3_column_int(q.s,0));q.step();return attempt;
}
void Store::fail_image(std::string id){
    impl_->check();Statement q(impl_->db,"UPDATE images SET state='Failed' WHERE id=? AND state='Pending'");q.text(1,id);q.step();
}
void Store::available(std::string id){impl_->check();Statement q(impl_->db,"UPDATE images SET state='Available' WHERE id=? AND state='Pending'");q.text(1,id);q.step();if(sqlite3_changes(impl_->db)!=1)throw std::runtime_error("Image transition");}
void Store::deleted(std::string id) {
    impl_->check();image_id(id);Statement q(impl_->db,"UPDATE images SET state='Deleted' WHERE id=? AND state='Available'");q.text(1,id);q.step();
    if(sqlite3_changes(impl_->db)!=1)throw std::runtime_error("Image delete state");
    std::filesystem::remove(impl_->root/(id+".stage"));
}
std::string Store::image_state(std::string id){impl_->check();Statement q(impl_->db,"SELECT state FROM images WHERE id=?");q.text(1,id);if(!q.step())throw std::runtime_error("Image missing");return q.value(0);}
void Store::checkpoint(){impl_->check();if(sqlite3_wal_checkpoint_v2(impl_->db,nullptr,SQLITE_CHECKPOINT_TRUNCATE,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("Checkpoint busy");}
namespace {
struct Reader {
    sqlite3* db{};
    explicit Reader(const std::filesystem::path& root) {
        if(!std::filesystem::is_directory(root)||std::filesystem::is_symlink(root)||
            std::filesystem::is_symlink(root/"results.sqlite"))throw std::runtime_error("Storage root");
        const auto path=(root/"results.sqlite").u8string();
        if(sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()),&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_NOMUTEX,nullptr)!=SQLITE_OK) {
            if(db)sqlite3_close_v2(db);db=nullptr;throw std::runtime_error("Storage unavailable");
        }
        sqlite3_busy_timeout(db,30);sqlite3_limit(db,SQLITE_LIMIT_LENGTH,65536);
        sqlite3_progress_handler(db,1000,+[](void* p)->int{
            return std::chrono::steady_clock::now()>*static_cast<std::chrono::steady_clock::time_point*>(p);
        },&deadline);
    }
    ~Reader(){if(db)sqlite3_close_v2(db);}
    std::chrono::steady_clock::time_point deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
};
}
std::vector<HistoryRow> history(const std::filesystem::path& root,std::uint64_t after,unsigned limit) {
    if(!limit||limit>200)throw std::invalid_argument("Query limit");Reader reader(root);
    Statement q(reader.db,"SELECT cursor,body FROM inspections WHERE cursor>? ORDER BY cursor LIMIT ?");q.number(1,after);q.number(2,limit);
    std::vector<HistoryRow> rows;while(q.step())rows.push_back({static_cast<std::uint64_t>(sqlite3_column_int64(q.s,0)),q.value(1)});return rows;
}
std::string find_result(const std::filesystem::path& root,std::string event) {
    (void)contracts::EventId(event);Reader reader(root);Statement q(reader.db,"SELECT body FROM inspections WHERE event=?");q.text(1,event);return q.step()?q.value(0):"";
}
ImageInfo find_image(const std::filesystem::path& root,std::string id) {
    image_id(id);Reader reader(root);Statement q(reader.db,"SELECT state,hash,bytes FROM images WHERE id=?");q.text(1,id);
    if(!q.step())return {"Missing","",0};return {q.value(0),q.value(1),static_cast<std::uint64_t>(sqlite3_column_int64(q.s,2))};
}
}
