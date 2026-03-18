#pragma once

#include <cstdint>
#include <unordered_set>
#include "message.h"
#include "torrent_file.h"
#include "thread_pool.h"

class dynamic_bitset {
private:
    size_t size, r_size;
    uint64_t *arr;

public:
    std::unordered_set<size_t> active_poses{};

    dynamic_bitset() : size(0), r_size(0), arr(nullptr) {}

    explicit dynamic_bitset(size_t Size) noexcept:
            size((Size + 63ull) >> 6ull), r_size(size << 6ull), arr(new uint64_t[size]{}) {
        active_poses.reserve(size);
    }

    dynamic_bitset(const dynamic_bitset &) = delete;

    dynamic_bitset &operator=(const dynamic_bitset &) = delete;

    ~dynamic_bitset() noexcept { delete[] arr; }

    void Init(size_t Size) noexcept {
        size = (Size + 63ull) >> 6ull;
        r_size = size << 6ull;
        delete[] arr;
        arr = new uint64_t[size]{};
        active_poses.clear();
        active_poses.reserve(size);
    }

    [[nodiscard]] inline size_t Size() const noexcept {
        return r_size;
    }

    [[nodiscard]] inline bool Peek(size_t ind) const noexcept {
        return (arr[ind >> 6ull] >> (ind & 63ull)) & 1ull;
    }

    inline void Set(size_t ind) noexcept {
        auto i = ind >> 6ull;
        if (!arr[i]) active_poses.emplace(i);
        arr[i] |= 1ull << (ind & 63ull);
    }

    inline void Reset(size_t ind) noexcept {
        auto i = ind >> 6ull;
        if (arr[i] && !(arr[i] &= ~(1ull << (ind & 63ull)))) active_poses.erase(i);
    }

    [[nodiscard]] inline size_t GetAnyOne(const dynamic_bitset &b) const noexcept {
        if (active_poses.size() < b.active_poses.size()) {
            for (auto i: active_poses)
                if (arr[i] & b.arr[i]) return (i << 6ull) + __builtin_ctzll(arr[i] & b.arr[i]);
        } else {
            for (auto i: b.active_poses)
                if (arr[i] & b.arr[i]) return (i << 6ull) + __builtin_ctzll(arr[i] & b.arr[i]);
        }
        return r_size;
    }
};

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

    Piece(size_t id, size_t this_length, dynamic_bitset &down);

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
    dynamic_bitset &downloading_pieces_;
};

class PieceManager {
public:
    [[nodiscard]] size_t GetTotalPieces() const { return pieces_.size(); }

    explicit PieceManager(const TorrentFile &tf, std::mutex &mutex, std::queue<HashResult> &mem, ThreadPool &tp);

    Block GetNextBlockToDownload(const dynamic_bitset &peer_bitfield, uint32_t &preferred_piece,
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
    dynamic_bitset missing_pieces_;
    dynamic_bitset downloading_pieces_;
    size_t downloaded_pieces = 0;
    const TorrentFile &tf_;
    std::vector<Piece> pieces_;
    std::mutex &out_mutex;
    std::queue<HashResult> &hash_results;
    ThreadPool &threadPool;
};

