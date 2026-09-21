#pragma once
#include <vision/ipc/message.hpp>
#include <functional>

namespace vision::ipc {
struct Limits {
    std::size_t max_payload{65536},normal_count{32},normal_bytes{262144};
    std::size_t control_count{8},control_bytes{16384},receive_count{32},receive_bytes{262144};
    std::size_t pending_count{32},session_requests{4096};
    std::uint64_t handshake_ns{2000000000},partial_ns{1000000000},request_ns{2000000000};
};
enum class SendStatus { Accepted, Full, NotReady, Invalid, Closed };
struct SendResult { SendStatus status; std::string request_id; };
struct RequestEvent { std::string request_id,reason; }; // Timeout/DisconnectedUnknown/LateResponse.
struct ChannelSnapshot {
    bool authenticated{},closed{};
    std::size_t queued_bytes{},queued_count{},received_bytes{},pending{};
    std::string failure;
    bool rotation_required{};
};
// Single event-thread owner, pure C++. now values are local monotonic nanoseconds.
class Channel {
public:
    Channel(std::string run,std::string worker,std::uint64_t epoch,std::string token,bool initiator,
            Limits limits,std::uint64_t now);
    SendResult send(std::string type,std::string payload,std::optional<contracts::Correlation> correlation,
                    std::uint64_t now,std::uint64_t timeout_ns=0);
    SendStatus reply(const Message& request,std::string type,std::string payload,std::uint64_t now);
    void feed(std::string_view bytes,std::uint64_t now);
    void tick(std::uint64_t now);
    void disconnect(std::string reason="DisconnectedUnknown");
    std::optional<std::string> next_wire(std::uint64_t now);
    std::optional<Message> pop();
    std::vector<RequestEvent> take_events();
    ChannelSnapshot snapshot() const;
private:
    struct Queued {Message message;std::size_t cost;bool request;};
    struct Lane {std::deque<Queued> queue;std::size_t bytes{};};
    // A terminal remains owned here until pop() transfers it to the caller.
    struct Pending {Message request;std::uint64_t deadline;bool terminal_received{};};
    SendStatus enqueue(Message message,bool request);
    void receive(Message message,std::size_t cost,std::uint64_t now);
    void fail(std::string reason);
    void event(std::string id,std::string reason);
    std::string local_id(std::uint64_t n) const;
    std::string run_,worker_,token_;
    std::uint64_t epoch_,handshake_deadline_,sent_sequence_{},next_id_{},issued_requests_{};
    bool initiator_,closed_{};
    Limits limits_;
    Decoder decoder_;
    Session session_;
    Lane normal_,control_;
    std::map<std::string,Pending> pending_;
    std::set<std::string> received_requests_;
    std::deque<std::pair<Message,std::size_t>> received_;
    std::size_t received_bytes_{};
    std::vector<RequestEvent> events_;
    std::optional<std::uint64_t> partial_started_;
    std::string failure_;
};
} // namespace vision::ipc
