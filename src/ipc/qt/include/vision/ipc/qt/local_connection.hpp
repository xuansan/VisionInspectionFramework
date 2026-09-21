#pragma once
#include <vision/ipc/channel.hpp>
#include <QObject>
#include <QLocalSocket>
#include <QElapsedTimer>
#include <QTimer>
#include <functional>

namespace vision::ipc::qt {
struct TransportLimits {
    qint64 read_buffer{65536},write_buffer{65536},io_chunk{16384};
    std::uint64_t write_stall_ns{2000000000};
};
struct TransportSnapshot {
    ChannelSnapshot channel;
    std::size_t active_frame_bytes{};
    qint64 socket_write_bytes{};
};
// Owns socket. All methods and callbacks run on this QObject's thread.
// Callbacks must be short and nonblocking; use deleteLater, not inline deletion.
class LocalConnection final : public QObject {
public:
    LocalConnection(QLocalSocket* socket,std::string run,std::string worker,std::uint64_t epoch,
        std::string token,bool initiator,Limits limits={},TransportLimits transport={},QObject* parent=nullptr);
    void connect_to(const QString& endpoint);
    SendResult send(std::string type,std::string payload,std::optional<contracts::Correlation> correlation={},
                    std::uint64_t timeout_ns=0);
    SendStatus reply(const Message& request,std::string type,std::string payload);
    void close(std::string reason="LocalClosed");
    TransportSnapshot snapshot() const;
    std::function<void(const Message&)> on_message;
    std::function<void(const RequestEvent&)> on_request_event;
    std::function<void()> on_ready;
    std::function<void(const std::string&)> on_closed;
private:
    std::uint64_t now() const;
    void require_thread() const;
    void pump();
    void readable();
    void check();
    void deliver();
    void finish_close();
    QLocalSocket* socket_;
    QElapsedTimer elapsed_;
    QTimer timer_;
    Channel channel_;
    TransportLimits limits_;
    std::string active_;
    std::size_t offset_{};
    std::optional<std::uint64_t> write_progress_;
    bool pumping_{},delivering_{},ready_notified_{},closed_notified_{};
};
} // namespace vision::ipc::qt
