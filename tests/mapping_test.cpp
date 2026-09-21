#include <vision/frame_transport/mapping.hpp>
#include <vision/runtime/buffer_broker.hpp>
#include <vision/serialization/json_codec.hpp>
#include <QCoreApplication>
#include <QProcess>
#include <QUuid>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <iostream>
#include <numeric>
#include <cstring>
#include <thread>
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
using namespace vision::contracts;
using namespace vision::runtime;
using namespace vision::frame_transport;
namespace {
void check(bool ok,const char* reason) {if(!ok)throw std::runtime_error(reason);}
std::string uid() {return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();}
FrameDescriptor descriptor(const PoolSpec& spec,std::uint64_t generation,std::uint64_t lease,std::uint32_t slot,
    std::uint32_t width,std::uint32_t height,FrameOwner owner) {
    return {{spec.run,spec.pool,std::move(owner),slot,generation,lease},FrameId("frame-"+std::to_string(generation)),
        {width,height,width,0,static_cast<std::uint64_t>(width)*height,PixelFormat::Mono8}};
}
void respond(std::string text) {std::cout<<text<<std::endl;}
int child(const QStringList& args) {
    PoolSpec spec{RunId(args[3].toStdString()),PoolId(args[4].toStdString()),args[5].toUInt(),args[6].toULongLong()};
    const auto mode=args[2];
    if(mode=="abandon"||mode=="slow") {
        const auto name=Mapping::object_name(spec)+L"-slot-0";
        HANDLE mutex=OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,name.c_str());
        check(mutex&&WaitForSingleObject(mutex,1000)==WAIT_OBJECT_0,"child lock");
        if(mode=="abandon") {
            auto mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,Mapping::object_name(spec).c_str());
            auto data=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,0);
            check(data!=nullptr,"partial write map");
            auto slot=reinterpret_cast<SlotHeader*>(static_cast<std::byte*>(data)+sizeof(PoolHeader));
            slot->state=1;
            std::memset(reinterpret_cast<std::byte*>(slot)+sizeof(SlotHeader),0xa5,16);
        }
        respond("LOCKED");
        std::this_thread::sleep_for(std::chrono::seconds(8));return 9;
    }
    auto map=Mapping::open(spec,mode=="write"?Access::Writer:Access::Reader);
    respond("READY");
    std::string line;
    while(std::getline(std::cin,line)) {
        if(line=="quit")return 0;
        auto d=vision::serialization::decode_frame(line,spec.slot_bytes);
        const auto value=static_cast<unsigned char>(d.permit.generation%251);
        MemoryStatus status;
        if(mode=="write") {
            std::vector<std::byte> bytes(static_cast<std::size_t>(d.layout.length),static_cast<std::byte>(value));
            status=map->write(d,bytes);
        } else {
            bool valid=false;
            status=map->read(d,100,[&](auto bytes) {
                valid=std::all_of(bytes.begin(),bytes.end(),[&](std::byte b){return b==static_cast<std::byte>(value);});
            });
            if(status==MemoryStatus::Ok&&!valid) {respond("CORRUPT");continue;}
        }
        respond(std::to_string(static_cast<int>(status)));
    }
    return 0;
}
struct Child {
    QProcess process;
    QByteArray buffered;
    Child(const PoolSpec& spec,const QString& mode) {
        process.start(QCoreApplication::applicationFilePath(),{"--child",mode,QString::fromStdString(spec.run.value()),
            QString::fromStdString(spec.pool.value()),QString::number(spec.slot_count),QString::number(spec.slot_bytes)});
        check(process.waitForStarted(3000),"child start");
        check(line()==(mode=="abandon"||mode=="slow"?"LOCKED":"READY"),"child ready");
    }
    ~Child() {
        if(process.state()!=QProcess::NotRunning) {
            process.write("quit\n");process.waitForBytesWritten(100);
            if(!process.waitForFinished(500)) {process.kill();process.waitForFinished(1000);}
        }
    }
    QByteArray line() {
        QElapsedTimer timer;timer.start();
        while(!buffered.contains('\n')&&timer.elapsed()<3000) {
            buffered+=process.readAllStandardOutput();
            if(buffered.contains('\n'))break;
            process.waitForReadyRead(50);
            if(process.state()==QProcess::NotRunning)break;
        }
        const auto pos=buffered.indexOf('\n');
        if(pos<0)throw std::runtime_error("child response timeout: "+process.readAllStandardError().toStdString());
        auto result=buffered.left(pos).trimmed();buffered.remove(0,pos+1);return result;
    }
    QByteArray invoke(const FrameDescriptor& d) {
        process.write(QByteArray::fromStdString(vision::serialization::encode_frame(d,d.layout.offset+d.layout.length))+"\n");
        process.waitForBytesWritten(1000);return line();
    }
    void kill() {process.kill();check(process.waitForFinished(2000),"child exit not confirmed");}
};
std::uint64_t cpu_ticks(HANDLE process) {
    FILETIME created,exit,kernel,user;
    check(GetProcessTimes(process,&created,&exit,&kernel,&user)!=0,"cpu query");
    auto value=[](FILETIME t){return (static_cast<std::uint64_t>(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
    return value(kernel)+value(user);
}
std::uint64_t rss(HANDLE process) {
    PROCESS_MEMORY_COUNTERS counters{};counters.cb=sizeof(counters);
    check(GetProcessMemoryInfo(process,&counters,sizeof(counters))!=0,"rss query");
    return counters.PeakWorkingSetSize;
}
int run(const QString& scenario) {
    const bool bandwidth=scenario=="bandwidth",functional=scenario=="functional";
    const std::uint32_t width=bandwidth?2448:(functional?1024:16),height=bandwidth?2048:(functional?768:8);
    const std::uint64_t size=static_cast<std::uint64_t>(width)*height;
    auto clock=std::make_shared<FakeClock>();
    const FrameOwner writer{WorkerId("camera"),1},reader{WorkerId("reader"),1},storage{WorkerId("storage"),1};
    PoolSpec spec{RunId(uid()),PoolId("camera-1"),bandwidth||functional?16U:1U,size};
    auto owner=Mapping::create(spec);
    BufferBroker broker(spec.run,spec.pool,spec.slot_count,size,2,clock);
    const auto grant=broker.acquire(writer,1000);check(grant.lease.has_value(),"write grant");
    check(owner->arm(*grant.lease)==MemoryStatus::Ok,"arm");
    auto d=descriptor(spec,grant.lease->generation,grant.lease->lease,grant.lease->slot,width,height,writer);
    if(scenario=="slow") {
        Child producer(spec,"write");check(producer.invoke(d)=="0","slow reader setup write");
        auto published=broker.publish(*grant.lease,d.frame,d.layout,{reader},10);
        check(published.readers.size()==1,"slow reader setup publish");
        auto reader_view=Mapping::open(spec,Access::Reader);
        Child blocked(spec,"slow"); // Holds the published slot's read-side mutex until killed.
        check(reader_view->read(published.readers[0],10,[](auto){})==MemoryStatus::Busy,"reader wait was not bounded");
        clock->advance(10);broker.tick();
        check(broker.snapshot().retiring==1&&broker.snapshot().leases==1,"slow reader lease vanished");
        check(broker.acquire(writer,100).status==LeaseStatus::Full,"slow reader bytes reused");
        broker.worker_exited({reader.worker,2});check(broker.snapshot().leases==1,"wrong epoch reclaimed");
        blocked.kill();broker.worker_exited(reader);
        auto next=broker.acquire(writer,100);check(next.lease.has_value(),"dead reader not reclaimed");
        check(owner->arm(*next.lease)==MemoryStatus::Abandoned,"dead reader mutex ignored");
        return 0;
    }
    if(scenario=="abandon") {
        Child blocked(spec,scenario);
        check(owner->arm(*grant.lease)==MemoryStatus::Busy,"owner blocked instead of Busy");
        clock->advance(1000);broker.tick();
        check(broker.acquire(writer,10).status==LeaseStatus::Full,"timed out writer reused");
        blocked.kill();
        check(owner->arm(*grant.lease)==MemoryStatus::Abandoned,"abandoned mutex accepted");
        broker.worker_exited(writer);check(broker.snapshot().free==1,"exit cleanup");
        auto next=broker.acquire(writer,100);check(next.lease.has_value(),"next grant");
        check(owner->arm(*next.lease)==MemoryStatus::Abandoned,"poisoned mapping reused");
        PoolSpec fresh{RunId(uid()),spec.pool,1,size};auto replacement=Mapping::create(fresh);
        auto fresh_reader=Mapping::open(fresh,Access::Reader);
        check(fresh_reader->read(d,0,[](auto){})==MemoryStatus::Invalid,"old run accepted");
        return 0;
    }
    Child producer(spec,"write"),consumer(spec,"read");
    check(producer.invoke(d)=="0","write failed");
    const auto published=broker.publish(*grant.lease,d.frame,d.layout,{reader,storage},1000);
    check(published.readers.size()==2,"publish");
    check(consumer.invoke(published.readers[0])=="0","read checksum");
    if(scenario=="roundtrip") {
        Child second(spec,"read");check(second.invoke(published.readers[1])=="0","second reader");
        auto read_map=Mapping::open(spec,Access::Reader);
        bool protected_readonly=false;
        check(read_map->read(published.readers[0],0,[&](auto bytes) {
            MEMORY_BASIC_INFORMATION info{};
            check(VirtualQuery(bytes.data(),&info,sizeof(info))==sizeof(info),"view protection query");
            protected_readonly=(info.Protect&0xff)==PAGE_READONLY;
        })==MemoryStatus::Ok&&protected_readonly,"reader mapping is not OS read-only");
        check(read_map->write(d,std::vector<std::byte>(size))==MemoryStatus::Invalid,"reader wrote");
        auto changed=published.readers[0];changed.layout.width--;
        check(read_map->read(changed,0,[](auto){})==MemoryStatus::Stale,"bad descriptor");
        check(broker.release(published.readers[0].permit),"first release");
        check(broker.acquire(writer,100).status==LeaseStatus::Full,"reader still owns");
        clock->advance(1000);broker.tick();check(broker.snapshot().retiring==1,"reader timeout");
        check(broker.acquire(writer,100).status==LeaseStatus::Full,"timed out reader reused");
        second.kill();broker.worker_exited(storage);
        auto next=broker.acquire(writer,100);check(next.lease.has_value(),"slot not reclaimed");
        check(owner->arm(*next.lease)==MemoryStatus::Ok,"new generation arm");
        check(consumer.invoke(published.readers[0])==QByteArray::number(static_cast<int>(MemoryStatus::Stale)),"old generation read");
        check(producer.invoke(d)==QByteArray::number(static_cast<int>(MemoryStatus::Stale)),"old generation write");
        check(!broker.release(published.readers[1].permit),"old release accepted");
        LatestPreview preview(16);std::vector<std::byte> bytes(16,std::byte{4});
        check(preview.replace(bytes),"preview");bytes[0]=std::byte{9};
        check(preview.bytes()[0]==std::byte{4},"preview retains formal bytes");
        check(!preview.replace(std::vector<std::byte>(17)),"preview exceeded budget");
        owner.reset(); // Old workers still map the pool after its coordinator handle closes.
        bool duplicate_rejected=false;try {auto duplicate=Mapping::create(spec);}catch(...) {duplicate_rejected=true;}
        check(duplicate_rejected,"mapping overwritten");
        // RGB/BGR layouts include row padding and a nonzero pixel offset.
        for(auto format:{PixelFormat::RGB8,PixelFormat::BGR8}) {
            PoolSpec color{RunId(uid()),PoolId("color"),1,64};
            auto color_owner=Mapping::create(color),color_writer=Mapping::open(color,Access::Writer),
                color_reader=Mapping::open(color,Access::Reader);
            auto c=descriptor(color,1,1,0,3,4,writer);
            c.layout={3,4,12,8,48,format};
            check(color_owner->arm(c.permit)==MemoryStatus::Ok,"color arm");
            std::vector<std::byte> color_bytes(48,std::byte{73});
            check(color_writer->write(c,color_bytes)==MemoryStatus::Ok,"color stride write");
            check(color_reader->read(c,0,[&](auto value){check(std::equal(value.begin(),value.end(),color_bytes.begin()),"color bytes");})==MemoryStatus::Ok,"color read");
        }
        return 0;
    }
    broker.release(published.readers[0].permit);broker.release(published.readers[1].permit);
    // Two independent camera pools, each with actual 16-slot allocation.
    PoolSpec other{spec.run,PoolId("camera-2"),16,size};auto owner2=Mapping::create(other);
    BufferBroker broker2(other.run,other.pool,16,size,2,clock);
    Child producer2(other,"write"),consumer2(other,"read");
    std::vector<FrameLease> held;
    for(int n=0;n<16;++n) {
        auto g=broker.acquire(writer,1000);check(g.lease.has_value(),"capacity grant");held.push_back(*g.lease);
    }
    check(broker.acquire(writer,1000).status==LeaseStatus::Full,"capacity exceeded");
    auto independent=broker2.acquire(writer,1000);
    check(independent.lease.has_value(),"camera pool not isolated");
    broker2.release(*independent.lease);
    for(const auto& permit:held)broker.release(permit);
    std::vector<HANDLE> handles;
    for(auto child:{&producer,&consumer,&producer2,&consumer2})
        handles.push_back(OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,static_cast<DWORD>(child->process.processId())));
    for(auto h:handles)check(h!=nullptr,"benchmark process handle");
    const auto parent_before=cpu_ticks(GetCurrentProcess());
    std::uint64_t child_before=0;for(auto h:handles)child_before+=cpu_ticks(h);
    QElapsedTimer timer;timer.start();std::vector<double> latencies;
    const int frames_per_camera=bandwidth?60:30;
    const int period_ms=bandwidth?50:100;
    for(int frame=0;frame<frames_per_camera;++frame) {
      for(int camera=0;camera<2;++camera) {
        auto& b=camera?broker2:broker;auto& map=camera?owner2:owner;auto& s=camera?other:spec;
        auto& p=camera?producer2:producer;auto& c=camera?consumer2:consumer;
        QElapsedTimer latency;latency.start();
        auto g=b.acquire(writer,1000);check(g.lease.has_value(),"benchmark full");
        check(map->arm(*g.lease)==MemoryStatus::Ok,"benchmark arm");
        auto frame_desc=descriptor(s,g.lease->generation,g.lease->lease,g.lease->slot,width,height,writer);
        check(p.invoke(frame_desc)=="0","benchmark write");
        auto readers=b.publish(*g.lease,frame_desc.frame,frame_desc.layout,{reader},1000);
        check(readers.readers.size()==1&&c.invoke(readers.readers[0])=="0","benchmark checksum");
        b.release(readers.readers[0].permit);
        latencies.push_back(static_cast<double>(latency.nsecsElapsed())/1000000.0);
      }
      const auto wait=(frame+1)*period_ms-timer.elapsed();
      if(wait>0)std::this_thread::sleep_for(std::chrono::milliseconds(wait));
    }
    const double seconds=timer.nsecsElapsed()/1e9;
    std::sort(latencies.begin(),latencies.end());
    std::uint64_t child_after=0,child_rss=0;
    for(auto h:handles) {child_after+=cpu_ticks(h);child_rss+=rss(h);CloseHandle(h);}
    QJsonObject report{{"scenario",scenario},{"frames",frames_per_camera*2},{"bytes_per_frame",QString::number(size)},
        {"seconds",seconds},{"frames_per_second",frames_per_camera*2/seconds},{"MiB_per_second",size*frames_per_camera*2/seconds/1048576.0},
        {"latency_p50_ms",latencies[latencies.size()/2]},{"latency_p95_ms",latencies[latencies.size()*95/100]},
        {"latency_max_ms",latencies.back()},{"coordinator_cpu_ms",(cpu_ticks(GetCurrentProcess())-parent_before)/10000.0},
        {"children_cpu_ms",(child_after-child_before)/10000.0},{"coordinator_peak_rss",QString::number(rss(GetCurrentProcess()))},
        {"children_peak_rss_sum",QString::number(child_rss)},{"allocated_pixel_bytes",QString::number(size*32)},
        {"slot_high_water_camera1",static_cast<int>(broker.snapshot().high_water)},{"slot_high_water_camera2",static_cast<int>(broker2.snapshot().high_water)}};
    respond(QJsonDocument(report).toJson(QJsonDocument::Compact).toStdString());
    return 0;
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        const auto args=app.arguments();
        if(args.size()==7&&args[1]=="--child")return child(args);
        check(args.size()==2,"scenario required");
        const int result=run(args[1]);respond(args[1].toStdString()+" passed");return result;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}

