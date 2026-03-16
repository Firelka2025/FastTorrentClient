#include "../include/torrent_file.h"
#include "../include/torrent_tracker.h"
#include "thread_pool.h"
#include "byte_tools.h"
#include <fcntl.h>
#include "piece_manager.h"
#include <sys/eventfd.h>
#include "config.h"
#include <liburing.h>
#include <iostream>
#include <random>
#include <list>
#include <fstream>

// ------------------------generating our id-----------------------------------------
std::string RandomString(size_t length) {
    std::random_device rd;
    std::mt19937 gen(rd());
    constexpr char all_vals[37] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::uniform_int_distribution<> dis(0, 36);

    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; ++i) result.push_back(all_vals[dis(gen)]);

    return result;
}

const std::string PeerId = "-PTK001-" + RandomString(12);

std::array<uint8_t, SEND_MAX_BYTES> GetHandshakeMessage(const TorrentFile &tf, std::string_view peerId) {
    std::array<uint8_t, SEND_MAX_BYTES> ans{};
    ans[0] = 19;
    for (int i = 1; i < 28; ++i) ans[i] = "BitTorrent protocol\0\0\0\0\0\0\0\0"[i - 1];
    for (int i = 28; i < 48; ++i) ans[i] = tf.infoHash[i - 28];
    for (int i = 48; i < 68; ++i) ans[i] = peerId[i - 48];
    return ans;
}

//---------------------------check hash----------------------------------------------

void CheckDownloadedFile(const std::string &filepath, const TorrentFile &tf) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open file for integrity check: " + filepath << std::endl;
        return;
    }

    size_t total_bytes_to_download = tf.length;
    size_t pieces_to_check = (total_bytes_to_download + tf.pieceLength - 1) / tf.pieceLength;
    if (pieces_to_check > tf.pieceHashes.size()) pieces_to_check = tf.pieceHashes.size();

    bool allValid = true;
    std::vector<uint8_t> buffer(tf.pieceLength);

    for (size_t i = 0; i < pieces_to_check; ++i) {
        int pieceSize = static_cast<int>(std::min(tf.length - i * tf.pieceLength, tf.pieceLength));

        file.read(reinterpret_cast<char *>(buffer.data()), pieceSize);
        if (!file) {
            std::cerr << "Failed to read piece " + std::to_string(i) + " from file" << std::endl;
            return;
        }

        std::vector<uint8_t> pieceData(buffer.begin(), buffer.begin() + pieceSize);
        auto hash = CalculateSHA1(pieceData);

        bool pieceOk = true;
        for (int j = 0; j < 20; ++j)
            if (hash[j] != static_cast<uint8_t>(tf.pieceHashes[i][j])) {
                pieceOk = false;
                break;
            }

        if (!pieceOk) {
            allValid = false;
            std::cerr << "Piece " << i << " is corrupted!" << std::endl;
        }
    }

    file.close();
    if (allValid) std::cout << "All downloaded pieces are valid." << std::endl;
    else std::cerr << "Some pieces are corrupted." << std::endl;
}

//-------------------starting and downloading----------------------------------------

std::deque<std::array<uint8_t, PEER_MEMORY_SIZE>> peersMemory;
std::deque<PeerConnection> peersHandlers;

//void FreeClosedConnection() {
//    auto handlers_it = peersHandlers.begin();
//    auto memory_it = peersMemory.begin();
//    while (handlers_it != peersHandlers.end()) {
//        if (handlers_it->IsClosed()) {
//            peersHandlers.erase(handlers_it);
//            peersMemory.erase(memory_it);
//            return;
//        }
//        ++handlers_it;
//        ++memory_it;
//    }
//}


void DownloadTorrentFile(const TorrentFile &torrentFile, const std::string &downloadDir) {
    //Prepare eventfd_ctx
    const int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) throw std::runtime_error("Failed to create eventfd");

    IoContext eventfd_ctx;
    eventfd_ctx.peer_connection_ = nullptr;
    eventfd_ctx.target_ = IoContext::THREADS;
    eventfd_ctx.op_ = IoContext::READ;
    uint64_t eventfd_buffer = 0;

    //Init io_uring;
    io_uring_queue_init(1024, &main_ring, 0);

    //Init thread pool
    std::mutex out_mutex;
    std::queue<HashResult> hash_results;
    ThreadPool threadPool(15);
    PieceManager pieceManager(torrentFile, out_mutex, hash_results, threadPool);

    //Update Peers
    TorrentTracker tracker(torrentFile.announce);
    tracker.UpdatePeers(torrentFile, PeerId, 6881);
//    int64_t peersUpdateInterval = tracker.UpdatePeers(torrentFile, PeerId, 6881);

    for (const Peer &peer: tracker.GetPeers()) {
        //Create "Connections handlers"
        peersMemory.emplace_back();
        peersHandlers.emplace_back(peer, peersMemory.back(), pieceManager);

        //Try to connect with each handler
        try {
            peersHandlers.back().StartConnection();
        } catch (std::runtime_error &e) {
            peersMemory.pop_back();
            peersHandlers.pop_back();
            std::cout << e.what() << std::endl;
        }
    }

    prepare_io_uring_pack();

    const std::array<uint8_t, SEND_MAX_BYTES> handshake_message(GetHandshakeMessage(torrentFile, PeerId));
    std::cout << "Initial peers: " << peersHandlers.size() << std::endl;

    //Send eventfd to main_ring
    {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&main_ring);
        io_uring_prep_read(sqe, efd, &eventfd_buffer, sizeof(eventfd_buffer), 0);
        io_uring_sqe_set_data(sqe, &eventfd_ctx);
        prepare_io_uring_pack();
    }

    //Prepare file
    int file_fd = open(downloadDir.data(), O_WRONLY | O_CREAT, 0666);
    if (file_fd < 0) throw std::runtime_error("Failed to open file for writing! Check permissions.");
    posix_fallocate(file_fd, 0, static_cast<long>(torrentFile.length));

    //Listening cycle
    while (!pieceManager.FinishedDownloading()) {
        struct io_uring_cqe *cqe;
        struct __kernel_timespec ts = {.tv_sec = 0, .tv_nsec = 5'000'000};
        int ret = io_uring_wait_cqe_timeout(&main_ring, &cqe, &ts);
        if (ret < 0) {
            prepare_io_uring_pack(true);
            continue;
        }

        int connect_result = cqe->res;
        auto *data = static_cast<IoContext *>(io_uring_cqe_get_data(cqe));

        if (data->target_ == IoContext::DISC) {
            if (pieceManager.SavedCount() % 100 == 0)
                std::cout << "Total saved: " << pieceManager.SavedCount() << " / "
                          << pieceManager.TotalPiecesToDownload()
                          << std::endl;

            pieceManager.SetSaved(data->piece_index);
            delete data;
            io_uring_cqe_seen(&main_ring, cqe);
            continue;
        }

        if (data->target_ == IoContext::THREADS) {
            std::queue<HashResult> ready;
            {
                std::lock_guard<std::mutex> lock(out_mutex);
                std::swap(ready, hash_results);
            }
            while (!ready.empty()) {
//                std::cout << "Piece: " << ready.front().piece_index_ << " "
//                          << (ready.front().is_valid_ ? "valid" : "INVALID") << std::endl;
                if (ready.front().is_valid_) {
                    pieceManager.SetDownloaded(ready.front().piece_index_);
                    uint64_t file_offset = ready.front().piece_index_ * torrentFile.pieceLength;
                    struct io_uring_sqe *sqe = io_uring_get_sqe(&main_ring);
                    const std::vector<uint8_t> &buffer = pieceManager.GetPieceById(
                            ready.front().piece_index_).piece_data_;

                    auto *ctx = new IoContext();
                    ctx->target_ = IoContext::DISC;
                    ctx->op_ = IoContext::WRITE;
                    ctx->piece_index = ready.front().piece_index_;

                    io_uring_prep_write(sqe, file_fd, buffer.data(), buffer.size(), file_offset);
                    io_uring_sqe_set_data(sqe, ctx);
                    prepare_io_uring_pack();
                } else pieceManager.ResetPiece(ready.front().piece_index_);
                ready.pop();
            }
            struct io_uring_sqe *sqe = io_uring_get_sqe(&main_ring);
            io_uring_prep_read(sqe, efd, &eventfd_buffer, sizeof(eventfd_buffer), 0);
            io_uring_sqe_set_data(sqe, &eventfd_ctx);
            prepare_io_uring_pack();

            io_uring_cqe_seen(&main_ring, cqe);
            continue;
        }

        if (data->peer_connection_->IsClosed()) {
            std::cerr << "Operation on closed handler!" << std::endl;
            io_uring_cqe_seen(&main_ring, cqe);
            continue;
        }

        switch (data->op_) {
            case IoContext::CONNECT: {
                if (connect_result == 0) {
                    std::cout << "Successfully connected to " << data->peer_connection_->GetIp() << ":"
                              << data->peer_connection_->GetPort() << std::endl;
                    //Send Handshake
                    data->peer_connection_->SendAndReceiveHandshake(handshake_message);
                } else {
                    std::cerr << "Connection failed: " << connect_result << std::endl;
                    //Erase closed connection
                    data->peer_connection_->Close();
                }
                break;
            }
            case IoContext::READ : {
                if (connect_result <= 0) {
                    std::cerr << "Connection closed / connection error" << std::endl;
                    data->peer_connection_->Close();
                    break;
                }

                if (data->peer_connection_->GetState() == PeerConnection::HANDSHAKING)
                    data->peer_connection_->CheckHandshakeMessage(handshake_message, connect_result, efd);
                else
                    data->peer_connection_->ReceiveMessage(connect_result, efd);
                break;
            }
            case IoContext::WRITE: {
                if (connect_result <= 0) {
                    data->peer_connection_->Close();
                } else {
                    data->peer_connection_->OnSendCompleted(connect_result);
                }
                break;
            }

            default:
                break;
        }

        io_uring_cqe_seen(&main_ring, cqe);
    }

    for (auto &i: peersHandlers) i.Close();
    close(file_fd);

    io_uring_queue_exit(&main_ring);
}


//-----------------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    std::string download_dir = ".";
    std::string torrent_file_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-d" && i + 1 < argc) {
            download_dir = argv[++i];
        } else {
            torrent_file_path = arg;
        }
    }

    if (torrent_file_path.empty()) {
        std::cerr << "-d <download_dir> <torrent_file>" << std::endl;
        return 1;
    }

    try {
        TorrentFile torrentFile;

        torrentFile = LoadTorrentFile(torrent_file_path);
        std::cout << "Loaded torrent file " << torrentFile.name << ". " << torrentFile.comment << std::endl;

        std::string downloadDir = download_dir + "/" + torrentFile.name;

        DownloadTorrentFile(torrentFile, downloadDir);
        CheckDownloadedFile(downloadDir, torrentFile);

    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}