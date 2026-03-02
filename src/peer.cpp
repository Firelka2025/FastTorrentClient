#include "peer.h"
#include <bits/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <liburing.h>
#include <cstring>
#include <iostream>
#include "message.h"
#include "byte_tools.h"
#include "piece_manager.h"

PeerConnection::PeerConnection(Peer peer, std::array<uint8_t, PEER_MEMORY_SIZE> &mem, PieceManager &pm) :
        peer_(std::move(peer)),
        receive_memory_(std::span<uint8_t>(mem.data(), RECEIVE_MAX_BYTES)),
        send_memory_(std::span<uint8_t>(mem.data() + RECEIVE_MAX_BYTES, SEND_MAX_BYTES)),
        pieceManager_(pm) {
    read_ctx_.peer_connection_ = this;
    write_ctx_.peer_connection_ = this;
    peer_bitfield_.resize(pieceManager_.GetTotalPieces(), false);
}

PeerConnection::~PeerConnection() {
    if (fd_ != -1) close(fd_);
}

void PeerConnection::StartConnection(struct io_uring *ring) {
    if (state_ == CLOSED) {
        throw std::runtime_error("Called StartConnection on closed connection");
    }

    fd_ = socket(AF_INET, SOCK_STREAM, 0);

    //Setting sockaddr
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(peer_.port);
    inet_pton(AF_INET, &peer_.ip[0], &addr.sin_addr);

    write_ctx_.target_ = IoContext::PEER;
    write_ctx_.op_ = IoContext::CONNECT;
    state_ = CONNECTING;

    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) throw std::runtime_error("sqe is empty");

    io_uring_prep_connect(sqe, fd_, (struct sockaddr *) &addr, sizeof(addr));
    io_uring_sqe_set_data(sqe, &write_ctx_);
}

void
PeerConnection::SendAndReceiveHandshake(struct io_uring *ring, const std::array<uint8_t, SEND_MAX_BYTES> &handshake) {
    std::vector<uint8_t> hs_vec(handshake.begin(), handshake.end());
    send_queue_.push(std::move(hs_vec));

    if (!is_sending_) ProcessSendQueue(ring);

    read_ctx_.target_ = IoContext::PEER;
    read_ctx_.op_ = IoContext::READ;
    read_ctx_.expected_bytes = 68;
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_recv(sqe, fd_, receive_memory_.data(), 68, 0);
    io_uring_sqe_set_data(sqe, &read_ctx_);

    state_ = HANDSHAKING;
    io_uring_submit(ring);
}

bool PeerConnection::CheckHandshakeMessage(const std::array<uint8_t, SEND_MAX_BYTES> &handshake, int length,
                                           struct io_uring *ring, int efd) {
    if (length + current_offset_ >= 68) {
        for (int i = 0; i < 20; ++i)
            if (receive_memory_[i] != handshake[i]) {
                std::cerr << "First message is not a Handshake!" << std::endl;
                Close();
                return false;
            }
        for (int i = 28; i < 48; ++i)
            if (receive_memory_[i] != handshake[i]) {
                std::cerr << "First message is not a Handshake!" << std::endl;
                Close();
                return false;
            }
        size_t next_message_length = current_offset_ + length - 68;
        memmove(receive_memory_.data(), receive_memory_.data() + 68, next_message_length);

        current_offset_ = 0;
        state_ = DOWNLOADING;
        ReceiveMessage(ring, next_message_length, efd);
//        std::cout << "Handshake successful" << std::endl;
        return true;
    }

    current_offset_ += length;
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_recv(sqe, fd_, receive_memory_.data() + current_offset_, 68 - current_offset_, 0);
    io_uring_sqe_set_data(sqe, &read_ctx_);
    io_uring_submit(ring);
    return false;
}

void PeerConnection::ReceiveMessage(struct io_uring *ring, size_t length, int efd) {
    while (true) {
        uint32_t message_length = 0;
        if (current_offset_ + length >= 4)
            message_length = BytesToInt(std::span<uint8_t>(receive_memory_.data(), 4));
        if (message_length > RECEIVE_MAX_BYTES) {
            std::cerr << "Zlovredniy peer sent big message" << std::endl;
            Close();
            return;
        }

        if (current_offset_ + length < 4 || current_offset_ + length - 4 < message_length) {
            read_ctx_.target_ = IoContext::PEER;
            read_ctx_.op_ = IoContext::READ;
            read_ctx_.expected_bytes = -1;
            current_offset_ += length;

            struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
            io_uring_prep_recv(sqe, fd_, receive_memory_.data() + current_offset_, RECEIVE_MAX_BYTES - current_offset_,
                               0);
            io_uring_sqe_set_data(sqe, &read_ctx_);
            io_uring_submit(ring);
            return;
        }


        if (message_length == 0) {
            Message message(Message::KeepAlive, std::span<uint8_t>());
//            std::cout << "Got KeepAlive message" << std::endl;
        } else {
            Message message(receive_memory_[4], std::span<uint8_t>(receive_memory_.data() + 5, message_length - 1));
            switch (message.id_) {
                case (Message::Choke): {
//                    std::cout << "Got CHOCKED" << std::endl;
                    is_choked_ = true;

                    for (const auto &block: active_requests_) {
                        pieceManager_.GetPieceById(block.index_).ResetBlock(block.begin_);
                    }
                    active_requests_.clear();

                    break;
                }
                case (Message::Unchoke): {
//                    std::cout << "We are UNCHOCKED now" << std::endl;
                    is_choked_ = false;

                    RequestMoreBlocks(ring);
                    break;
                }
                case (Message::BitField): {
                    for (size_t i = 0, pos = 0; i < message.payload_.size(); ++i) {
                        for (int j = 0; j < 8; ++j) {
                            if (pos < peer_bitfield_.size()) {
                                peer_bitfield_[pos++] = (message.payload_[i] >> (7 - j)) & 1;
                            }
                        }
                    }
                    if (!sent_interest) {
                        SendMessage(ring, InterestedMessage);
                        sent_interest = true;
                    }
                    break;
                }
                case (Message::Have): {
                    if (message.payload_.size() == 4) {
                        uint32_t piece_index = BytesToInt(std::span<uint8_t>(message.payload_.data(), 4));
                        if (piece_index < peer_bitfield_.size()) {
                            peer_bitfield_[piece_index] = true;
                        }
                    }
                    if (!sent_interest) {
                        SendMessage(ring, InterestedMessage);
                        sent_interest = true;
                    }
                    break;
                }

                case (Message::Piece): {
                    if (message.payload_.size() < 8) {
                        std::cerr << "Invalid Piece message size!" << std::endl;
                        Close();
                        return;
                    }

                    std::span<uint8_t> data(message.payload_.begin() + 8, message.payload_.size() - 8);
                    uint32_t index = BytesToInt(std::span<uint8_t>(message.payload_.data(), 4));
                    uint32_t begin = BytesToInt(std::span<uint8_t>(message.payload_.data() + 4, 4));
//                    std::cout << "Got Peice: index: " << index << " begin: " << begin << " size: " << data.size()
//                              << std::endl;

                    auto it = std::find_if(active_requests_.begin(), active_requests_.end(), [&](const Block &b) {
                        return b.index_ == index && b.begin_ == begin;
                    });
                    if (it != active_requests_.end()) active_requests_.erase(it);

                    Piece &cur_piece = pieceManager_.GetPieceById(index);
                    if (cur_piece.state_ == Piece::DOWNLOADING) {
                        cur_piece.FillDownloaded(begin, data);
                        if (cur_piece.IsDownloaded()) pieceManager_.CheckDownload(index, efd);
                    }
                    RequestMoreBlocks(ring);
                    break;
                }
            }
        }

        memmove(receive_memory_.data(), receive_memory_.data() + message_length + 4,
                current_offset_ + length - 4 - message_length);

        length = current_offset_ + length - 4 - message_length;
        current_offset_ = 0;
    }
}

bool PeerConnection::SendMessage(struct io_uring *ring, const Message &message) {
    std::vector<uint8_t> buffer(message.GetMessageLength());
    std::span<uint8_t> span_buf(buffer);
    message.PrepareMemToSend(span_buf);

    send_queue_.push(std::move(buffer));

    if (!is_sending_) ProcessSendQueue(ring);

    return true;
}


void PeerConnection::Close() {
    if (state_ == CLOSED) return;
    state_ = CLOSED;
    if (fd_ != -1) close(fd_), fd_ = -1;

    for (const auto &block: active_requests_) {
        pieceManager_.GetPieceById(block.index_).ResetBlock(block.begin_);
    }
    active_requests_.clear();
}

int PeerConnection::GetPort() const { return peer_.port; }

std::string_view PeerConnection::GetIp() const { return peer_.ip; }

bool PeerConnection::IsClosed() const { return state_ == CLOSED; }

PeerConnection::State PeerConnection::GetState() const { return state_; }

void PeerConnection::OnSendCompleted(struct io_uring *ring, int res) {
    if (res <= 0) {
        Close();
        return;
    }

    send_offset_ += res;

    if (send_offset_ == send_queue_.front().size()) {
        send_queue_.pop();
        send_offset_ = 0;
    }

    ProcessSendQueue(ring);
}

void PeerConnection::ProcessSendQueue(struct io_uring *ring) {
    if (send_queue_.empty()) {
        is_sending_ = false;
        return;
    }
    is_sending_ = true;

    const auto &buffer = send_queue_.front();
    write_ctx_.target_ = IoContext::PEER;
    write_ctx_.op_ = IoContext::WRITE;
    write_ctx_.expected_bytes = buffer.size();

    size_t bytes_to_send = buffer.size() - send_offset_;
    write_ctx_.expected_bytes = bytes_to_send;

    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_send(sqe, fd_, buffer.data() + send_offset_, bytes_to_send, 0);
    io_uring_sqe_set_data(sqe, &write_ctx_);
    io_uring_submit(ring);
}

void PeerConnection::RequestMoreBlocks(struct io_uring *ring) {
    if (is_choked_) return;

    while (active_requests_.size() < MAX_PIPELINE) {
        Block next_block = pieceManager_.GetNextBlockToDownload(peer_bitfield_, preferred_piece_, active_requests_);
        if (next_block.index_ == uint32_t(-1)) break;

        active_requests_.push_back(next_block);
        SendMessage(ring, next_block.GetRequestMessage());
    }
}


