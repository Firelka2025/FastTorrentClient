#pragma once

#include <string>
#include <utility>
#include <netinet/in.h>
#include "message.h"
#include "span"
#include "piece_manager.h"
#include "config.h"

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

    void StartConnection();

    void SendAndReceiveHandshake(const std::array<uint8_t, SEND_MAX_BYTES> &handshake);

    bool CheckHandshakeMessage(const std::array<uint8_t, SEND_MAX_BYTES> &handshake, int length, int efd);

    void ReceiveMessage(size_t length, int efd);

    bool SendMessage(const Message &message);

    [[nodiscard]] std::string_view GetIp() const;

    [[nodiscard]] int GetPort() const;

    [[nodiscard]] bool IsClosed() const;

    [[nodiscard]] State GetState() const;

    void OnSendCompleted(int res);

    void Close();

private:
    void ProcessSendQueue();

    void RequestMoreBlocks();

    static constexpr size_t MAX_PIPELINE = 150;
    size_t send_offset_ = 0;
    uint32_t preferred_piece_ = uint32_t(-1);
    std::vector<uint8_t> sending_buffer_;
    std::vector<uint8_t> pending_buffer_;
    bool is_sending_ = false;
    std::vector<Block> active_requests_;
    int fd_ = -1;
    bool is_choked_ = true;
    bool sent_interest = false;
    State state_ = CONNECTING;
    Peer peer_;
    size_t current_offset_ = 0;
    std::span<uint8_t> receive_memory_;
    std::span<uint8_t> send_memory_;
    struct sockaddr_in addr{};
    IoContext read_ctx_;
    IoContext write_ctx_;
    PieceManager &pieceManager_;
    dynamic_bitset peer_bitfield_;
};
