#include <vision/ipc/qt/local_connection.hpp>
#include <QThread>
#include <algorithm>

namespace vision::ipc::qt {
LocalConnection::LocalConnection(QLocalSocket* socket,std::string run,std::string worker,std::uint64_t epoch,
    std::string token,bool initiator,Limits limits,TransportLimits transport,QObject* parent)
    : QObject(parent),socket_(socket),channel_(std::move(run),std::move(worker),epoch,std::move(token),initiator,limits,0),
      limits_(transport) {
    if(!socket_ || socket_->thread()!=thread() || limits_.read_buffer<=0 || limits_.read_buffer>1048576 ||
       limits_.write_buffer<=0 || limits_.write_buffer>1048576 || limits_.io_chunk<=0 || limits_.io_chunk>65536 ||
       !limits_.write_stall_ns)throw std::invalid_argument("Invalid local transport configuration");
    socket_->setParent(this);
    socket_->setReadBufferSize(limits_.read_buffer);
    elapsed_.start();
    QObject::connect(socket_,&QLocalSocket::connected,this,[this]{pump();});
    QObject::connect(socket_,&QLocalSocket::readyRead,this,[this]{readable();});
    QObject::connect(socket_,&QLocalSocket::bytesWritten,this,[this](qint64 bytes) {
        if(bytes>0)write_progress_=now();
        pump();
    });
    QObject::connect(socket_,&QLocalSocket::disconnected,this,[this]{close("DisconnectedUnknown");});
    QObject::connect(socket_,&QLocalSocket::errorOccurred,this,[this](QLocalSocket::LocalSocketError) {close("SocketError");});
    timer_.setInterval(10);
    timer_.setTimerType(Qt::PreciseTimer);
    QObject::connect(&timer_,&QTimer::timeout,this,[this]{check();});
    timer_.start();
    QTimer::singleShot(0,this,[this]{check();});
}
void LocalConnection::require_thread() const {
    if(QThread::currentThread()!=thread())throw std::logic_error("LocalConnection requires its owning event thread");
}
std::uint64_t LocalConnection::now() const {return static_cast<std::uint64_t>(elapsed_.nsecsElapsed());}
void LocalConnection::connect_to(const QString& endpoint) {
    require_thread();
    if(socket_->state()!=QLocalSocket::UnconnectedState || channel_.snapshot().closed)
        throw std::logic_error("Cannot reuse a local connection");
    socket_->connectToServer(endpoint);
}
SendResult LocalConnection::send(std::string type,std::string payload,std::optional<contracts::Correlation> correlation,
                                 std::uint64_t timeout_ns) {
    require_thread();
    auto result=channel_.send(std::move(type),std::move(payload),std::move(correlation),now(),timeout_ns);
    pump();deliver();
    return result;
}
SendStatus LocalConnection::reply(const Message& request,std::string type,std::string payload) {
    require_thread();
    auto result=channel_.reply(request,std::move(type),std::move(payload),now());
    pump();deliver();
    return result;
}
TransportSnapshot LocalConnection::snapshot() const {
    require_thread();
    return {channel_.snapshot(),active_.size()-offset_,socket_->bytesToWrite()};
}
void LocalConnection::close(std::string reason) {
    require_thread();
    channel_.disconnect(std::move(reason));
    finish_close();
}
void LocalConnection::finish_close() {
    if(!channel_.snapshot().closed)return;
    timer_.stop();
    active_.clear();offset_=0;
    // Set notification guard before abort(), which may synchronously emit signals.
    if(closed_notified_)return;
    closed_notified_=true;
    socket_->abort();
    auto events=channel_.take_events();
    for(const auto& e:events) {
        try {if(on_request_event)on_request_event(e);}
        catch(...) { /* One consumer must not suppress the other final notifications. */ }
    }
    try {if(on_closed)on_closed(channel_.snapshot().failure);}
    catch(...) { /* Connection already closed; callback cannot revive it. */ }
}
void LocalConnection::deliver() {
    if(delivering_)return;
    delivering_=true;
    try {
        if(channel_.snapshot().authenticated&&!ready_notified_) {
            ready_notified_=true;
            if(on_ready)on_ready();
        }
        auto events=channel_.take_events();
        bool callback_failed=false;
        for(const auto& e:events) {
            try {if(on_request_event)on_request_event(e);}
            catch(...) {callback_failed=true;}
        }
        if(callback_failed)channel_.disconnect("CallbackFailure");
        while(!channel_.snapshot().closed) {
            auto message=channel_.pop();
            if(!message)break;
            if(on_message)on_message(*message);
        }
    } catch(...) {channel_.disconnect("CallbackFailure");}
    delivering_=false;
    finish_close();
}
void LocalConnection::readable() {
    require_thread();
    if(channel_.snapshot().closed)return;
    // Bound work per event-loop turn, not just buffer capacity.
    qint64 budget=65536;
    while(budget>0 && socket_->bytesAvailable()>0 && !channel_.snapshot().closed) {
        const auto count=std::min({budget,limits_.io_chunk,socket_->bytesAvailable()});
        const auto bytes=socket_->read(count);
        if(bytes.isEmpty())break;
        budget-=bytes.size();
        channel_.feed(std::string_view(bytes.constData(),static_cast<std::size_t>(bytes.size())),now());
        deliver();
    }
    if(socket_->bytesAvailable()>0 && !channel_.snapshot().closed)
        QTimer::singleShot(0,this,[this]{readable();});
    pump();finish_close();
}
void LocalConnection::pump() {
    require_thread();
    if(pumping_ || channel_.snapshot().closed || socket_->state()!=QLocalSocket::ConnectedState)return;
    pumping_=true;
    try {
        qint64 budget=65536;
        while(budget>0 && !channel_.snapshot().closed) {
            if(active_.empty()) {
                auto wire=channel_.next_wire(now());
                if(!wire)break;
                active_=std::move(*wire);offset_=0;
            }
            const auto room=limits_.write_buffer-socket_->bytesToWrite();
            if(room<=0)break;
            const auto amount=std::min({room,budget,limits_.io_chunk,static_cast<qint64>(active_.size()-offset_)});
            const auto written=socket_->write(active_.data()+offset_,amount);
            if(written<0) {channel_.disconnect("WriteError");break;}
            if(written==0)break;
            budget-=written;offset_+=static_cast<std::size_t>(written);
            if(offset_==active_.size()) {active_.clear();offset_=0;}
        }
    } catch(...) {channel_.disconnect("TransportFailure");}
    pumping_=false;
    const bool busy=!active_.empty() || socket_->bytesToWrite()>0 || channel_.snapshot().queued_count;
    if(busy&&!write_progress_)write_progress_=now();
    if(!busy)write_progress_.reset();
    finish_close();
}
void LocalConnection::check() {
    require_thread();
    channel_.tick(now());
    if(write_progress_ && now()-*write_progress_>=limits_.write_stall_ns)channel_.disconnect("WriteStallTimeout");
    if(channel_.snapshot().closed) {finish_close();return;}
    readable();pump();deliver();
}
} // namespace vision::ipc::qt
