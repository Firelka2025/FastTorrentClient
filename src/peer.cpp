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
#include <netinet/tcp.h>

PeerConnection::PeerConnection(Peer peer, std::array<uint8_t, PEER_MEMORY_SIZE> &mem, PieceManager &pm) :
        peer_(std::move(peer)),
        receive_memory_(std::span<uint8_t>(mem.data(), RECEIVE_MAX_BYTES)),
        send_memory_(std::span<uint8_t>(mem.data() + RECEIVE_MAX_BYTES, SEND_MAX_BYTES)),
        pieceManager_(pm) {
    read_ctx_.peer_connection_ = this;
    write_ctx_.peer_connection_ = this;
    peer_bitfield_.resize(pieceManager_.GetTotalPieces(), false);
    sending_buffer_.reserve(SEND_BUFFER_RESERVE);
    pending_buffer_.reserve(SEND_BUFFER_RESERVE);
}

PeerConnection::~PeerConnection() {
    if (fd_ != -1) close(fd_);
}

void PeerConnection::StartConnection() {
    if (state_ == CLOSED) {
        throw std::runtime_error("Called StartConnection on closed connection");
    }

    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int flag = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));

    //Setting sockaddr
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(peer_.port);
    inet_pton(AF_INET, &peer_.ip[0], &addr.sin_addr);

    write_ctx_.target_ = IoContext::PEER;
    write_ctx_.op_ = IoContext::CONNECT;
    state_ = CONNECTING;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&main_ring);
    if (!sqe) throw std::runtime_error("sqe is empty");

    io_uring_prep_connect(sqe, fd_, (struct sockaddr *) &addr, sizeof(addr));
    io_uring_sqe_set_data(sqe, &write_ctx_);
}

void PeerConnection::SendAndReceiveHandshake(const std::array<uint8_t, SEND_MAX_BYTES> &handshake) {
    pending_buffer_.insert(pending_buffer_.end(), handshake.begin(), handshake.end());

    if (!is_sending_) ProcessSendQueue();

    read_ctx_.target_ = IoContext::PEER;
    read_ctx_.op_ = IoContext::READ;
    struct io_uring_sqe *sqe = io_uring_get_sqe(&main_ring);
    io_uring_prep_recv(sqe, fd_, receive_memory_.data(), 68, 0);
    io_uring_sqe_set_data(sqe, &read_ctx_);

    state_ = HANDSHAKING;
    prepare_io_uring_pack();
}

bool PeerConnection::CheckHandshakeMessage(const std::array<uint8_t, SEND_MAX_BYTES> &handshake, int length, int efd) {
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
        ReceiveMessage(next_message_length, efd);
//        std::cout << "Handshake successful" << std::endl;
        return true;
    }

    current_offset_ += length;
    struct io_uring_sqe *sqe = io_uring_get_sqe(&main_ring);
    io_uring_prep_recv(sqe, fd_, receive_memory_.data() + current_offset_, 68 - current_offset_, 0);
    io_uring_sqe_set_data(sqe, &read_ctx_);
    prepare_io_uring_pack();
    return false;
}

void PeerConnection::ReceiveMessage(size_t length, int efd) {
    size_t total_bytes = current_offset_ + length;
    size_t parsed_bytes = 0;

    while (total_bytes - parsed_bytes >= 4) {
        uint32_t message_length = BytesToInt(std::span<uint8_t>(receive_memory_.data() + parsed_bytes, 4));

        if (message_length > RECEIVE_MAX_BYTES) {
            std::cerr << "Zlovredniy peer sent big message" << std::endl;
            Close();
            return;
        }
        if (total_bytes - parsed_bytes < 4 + message_length) break;


        if (message_length == 0) {
//            Message message(Message::KeepAlive, std::span<uint8_t>());
//            std::cout << "Got KeepAlive message" << std::endl;
        } else {
            Message message(receive_memory_[parsed_bytes + 4],
                            std::span<uint8_t>(receive_memory_.data() + parsed_bytes + 5, message_length - 1));
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

                    RequestMoreBlocks();
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
                        SendMessage(InterestedMessage);
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
                        SendMessage(InterestedMessage);
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
                    if (it != active_requests_.end()) {
                        std::swap(*it, active_requests_.back());
                        active_requests_.pop_back();
                    }

                    Piece &cur_piece = pieceManager_.GetPieceById(index);
                    if (cur_piece.state_ == Piece::DOWNLOADING) {
                        cur_piece.FillDownloaded(begin, data);
                        if (cur_piece.IsDownloaded()) pieceManager_.CheckDownload(index, efd);
                    }
                    RequestMoreBlocks();
                    break;
                }
            }
        }

        parsed_bytes += 4 + message_length;
    }
    if (state_ == CLOSED) return;

    size_t remaining = total_bytes - parsed_bytes;

    if (remaining > 0 && parsed_bytes > 0)
        memmove(receive_memory_.data(), receive_memory_.data() + parsed_bytes, remaining);


    current_offset_ = remaining;


    read_ctx_.target_ = IoContext::PEER;
    read_ctx_.op_ = IoContext::READ;
    struct io_uring_sqe *sqe = io_uring_get_sqe(&main_ring);
    io_uring_prep_recv(sqe, fd_, receive_memory_.data() + current_offset_, RECEIVE_MAX_BYTES - current_offset_, 0);
    io_uring_sqe_set_data(sqe, &read_ctx_);
    prepare_io_uring_pack();
}

bool PeerConnection::SendMessage(const Message &message) {
    size_t old_size = pending_buffer_.size();

    pending_buffer_.resize(old_size + message.GetMessageLength());
    std::span<uint8_t> span_buf(pending_buffer_.data() + old_size, message.GetMessageLength());
    message.PrepareMemToSend(span_buf);

    if (!is_sending_) ProcessSendQueue();

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

void PeerConnection::OnSendCompleted(int res) {
    if (res <= 0) {
        Close();
        return;
    }

    send_offset_ += res;
    ProcessSendQueue();
}

void PeerConnection::ProcessSendQueue() {
    if (send_offset_ == sending_buffer_.size()) {
        sending_buffer_.clear();
        send_offset_ = 0;
    }

    if (sending_buffer_.empty()) {
        if (pending_buffer_.empty()) {
            is_sending_ = false;
            return;
        }
        std::swap(sending_buffer_, pending_buffer_);
    }

    is_sending_ = true;
    write_ctx_.target_ = IoContext::PEER;
    write_ctx_.op_ = IoContext::WRITE;

    size_t bytes_to_send = sending_buffer_.size() - send_offset_;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&main_ring);
    io_uring_prep_send(sqe, fd_, sending_buffer_.data() + send_offset_, bytes_to_send, 0);
    io_uring_sqe_set_data(sqe, &write_ctx_);
    prepare_io_uring_pack();
}

void PeerConnection::RequestMoreBlocks() {
    if (is_choked_) return;

    while (active_requests_.size() < MAX_PIPELINE) {
        Block next_block = pieceManager_.GetNextBlockToDownload(peer_bitfield_, preferred_piece_, active_requests_);
        if (next_block.index_ == uint32_t(-1)) break;

        active_requests_.push_back(next_block);
        SendMessage(next_block.GetRequestMessage());
    }
}


