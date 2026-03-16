#pragma once

#include <cstdint>
#include "message.h"
#include "torrent_file.h"
#include "thread_pool.h"


struct HashResult;

class ThreadPool;

struct Block {
    uint32_t index_ = uint32_t(-1);
    uint32_t begin_ = uint32_t(-1);
    uint32_t length_ = uint32_t(-1);

    [[nodiscard]] Message GetRequestMessage() const;
};

struct Piece {
    enum State {
        MISSING,
        DOWNLOADING,
        DOWNLOADED,
        HASHING,
        SAVED
    };

    Piece(size_t id, size_t this_length);

    void StartDownload();

    void ResetBlock(size_t offset);

    void FillDownloaded(size_t offset, std::span<uint8_t> data);

    [[nodiscard]] bool IsDownloaded() const;

    void ResetPiece();

    [[nodiscard]] bool CheckDownload(std::string_view check_hash) const;

    Block GetNextBlock();


    size_t total_size_, id_, blocks_downloaded = 0;
    State state_ = MISSING;
    std::vector<size_t> not_downloaded_begins_;
    std::vector<bool> downloaded_;
    std::vector<uint8_t> piece_data_;
};

class PieceManager {
public:
    [[nodiscard]] size_t GetTotalPieces() const { return pieces_.size(); }

    explicit PieceManager(const TorrentFile &tf, std::mutex &mutex, std::queue<HashResult> &mem, ThreadPool &tp);

    Block GetNextBlockToDownload(const std::vector<bool> &peer_bitfield, uint32_t &preferred_piece,
                                 const std::vector<Block> &active_requests);

    Piece &GetPieceById(size_t id);

    [[nodiscard]] bool FinishedDownloading() const;

    void SetSaved(size_t id);

    void CheckDownload(size_t id, int efd);

    void ResetPiece(size_t id);

    void SetDownloaded(size_t id);

    [[nodiscard]] size_t SavedCount() const;

    [[nodiscard]] size_t TotalPiecesToDownload() const;

private:
    std::vector<uint32_t> missing_pieces_;
    std::vector<uint32_t> downloading_pieces_;
    size_t downloaded_pieces = 0;
    const TorrentFile &tf_;
    std::vector<Piece> pieces_;
    std::mutex &out_mutex;
    std::queue<HashResult> &hash_results;
    ThreadPool &threadPool;
};

