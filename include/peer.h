#pragma once

#include <string>
#include <utility>
#include <netinet/in.h>
#include "message.h"
#include "span"
#include "piece_manager.h"

constexpr uint RECEIVE_MAX_BYTES = 1 << 20;
constexpr uint SEND_MAX_BYTES = 68;
constexpr uint PEER_MEMORY_SIZE = RECEIVE_MAX_BYTES + SEND_MAX_BYTES;

struct PeerConnection;

struct Peer {
    std::string ip;
    int port;

    inline bool operator==(const Peer &b) const { return ip == b.ip && port == b.port; }
};

struct IoContext {
    enum Target {
        PEER,
        THREADS,
        DISC
    };
    enum Operation {
        CONNECT,
        READ,
        WRITE,
        TIMEOUT
    };

    PeerConnection *peer_connection_;
    Operation op_;
    Target target_;
    size_t expected_bytes = 0;
    uint32_t piece_index = 0;
};

struct PeerConnection {
    enum State {
        CONNECTING,
        HANDSHAKING,
        DOWNLOADING,
        CLOSED
    };

    PeerConnection(Peer peer, std::array<uint8_t, PEER_MEMORY_SIZE> &mem, PieceManager &pm);

    ~PeerConnection();

    void StartConnection(struct io_uring *ring);

    void SendAndReceiveHandshake(struct io_uring *ring, const std::array<uint8_t, SEND_MAX_BYTES> &handshake);

    bool
    CheckHandshakeMessage(const std::array<uint8_t, SEND_MAX_BYTES> &handshake, int length, struct io_uring *ring,
                          int efd);

    void ReceiveMessage(struct io_uring *ring, size_t length, int efd);

    bool SendMessage(struct io_uring *ring, const Message &message);

    [[nodiscard]] std::string_view GetIp() const;

    [[nodiscard]] int GetPort() const;

    [[nodiscard]] bool IsClosed() const;

    [[nodiscard]] State GetState() const;

    void OnSendCompleted(struct io_uring *ring, int res);

    void Close();

private:
    void ProcessSendQueue(struct io_uring *ring);

    void RequestMoreBlocks(struct io_uring *ring);

    static constexpr size_t MAX_PIPELINE = 150;
    size_t send_offset_ = 0;
    uint32_t preferred_piece_ = uint32_t(-1);
    std::queue<std::vector<uint8_t>> send_queue_;
    bool is_sending_ = false;
    std::vector<Block> active_requests_;
    int fd_ = -1;
    bool is_choked_ = true;
    bool sent_interest = false;
    std::vector<bool> peer_bitfield_;
    State state_ = CONNECTING;
    Peer peer_;
    size_t current_offset_ = 0;
    std::span<uint8_t> receive_memory_;
    std::span<uint8_t> send_memory_;
    struct sockaddr_in addr{};
    IoContext read_ctx_;
    IoContext write_ctx_;
    PieceManager &pieceManager_;
};
