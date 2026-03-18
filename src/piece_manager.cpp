#include "piece_manager.h"

#include <utility>
#include <iostream>
#include "byte_tools.h"


Message Block::GetRequestMessage() const {
    std::vector<uint8_t> ans(12);
    ans[0] = uint8_t((index_ >> 24) & 0xFF);
    ans[1] = uint8_t((index_ >> 16) & 0xFF);
    ans[2] = uint8_t((index_ >> 8) & 0xFF);
    ans[3] = uint8_t(index_ & 0xFF);

    ans[4] = uint8_t((begin_ >> 24) & 0xFF);
    ans[5] = uint8_t((begin_ >> 16) & 0xFF);
    ans[6] = uint8_t((begin_ >> 8) & 0xFF);
    ans[7] = uint8_t(begin_ & 0xFF);

    ans[8] = uint8_t((length_ >> 24) & 0xFF);
    ans[9] = uint8_t((length_ >> 16) & 0xFF);
    ans[10] = uint8_t((length_ >> 8) & 0xFF);
    ans[11] = uint8_t(length_ & 0xFF);

    return {Message::Request, std::move(ans)};
}

PieceManager::PieceManager(const TorrentFile &tf, std::mutex &mutex, std::queue<HashResult> &mem, ThreadPool &tp) :
        tf_(tf), out_mutex(mutex), hash_results(mem), threadPool(tp) {
    size_t total_pieces = tf.length / tf.pieceLength;
    size_t last_piece_size = tf.length - total_pieces * tf.pieceLength;

    pieces_.reserve(total_pieces + (last_piece_size > 0));
    for (size_t i = 0; i < total_pieces; ++i)
        pieces_.emplace_back(i, tf.pieceLength, downloading_pieces_);
    if (last_piece_size) pieces_.emplace_back(total_pieces, last_piece_size, downloading_pieces_);

    downloading_pieces_.Init(pieces_.size());
    missing_pieces_.Init(pieces_.size());
    for (size_t i = 0; i < pieces_.size(); ++i) missing_pieces_.Set(i);
}

Block PieceManager::GetNextBlockToDownload(const dynamic_bitset &peer_bitfield, uint32_t &preferred_piece,
                                           const std::vector<Block> &active_requests) {
    if (preferred_piece != uint32_t(-1) && preferred_piece < pieces_.size()) {
        if (peer_bitfield.Peek(preferred_piece) &&
            pieces_[preferred_piece].state_ == Piece::DOWNLOADING &&
            !pieces_[preferred_piece].not_downloaded_begins_.empty()) {
            return pieces_[preferred_piece].GetNextBlock();
        }
    }

    auto pos = downloading_pieces_.GetAnyOne(peer_bitfield);
    if (pos != downloading_pieces_.Size()) {
        preferred_piece = pos;
        return pieces_[pos].GetNextBlock();
    }

    pos = missing_pieces_.GetAnyOne(peer_bitfield);
    if (pos != missing_pieces_.Size()) {
        downloading_pieces_.Set(pos);
        missing_pieces_.Reset(pos);
        preferred_piece = pos;
        return pieces_[pos].GetNextBlock();
    }

    for (const Piece &piece: pieces_) {
        if (piece.state_ == Piece::DOWNLOADING && piece.not_downloaded_begins_.empty() &&
            peer_bitfield.Peek(piece.id_)) {

            for (size_t b = 0; b < piece.downloaded_.size(); ++b) {
                if (!piece.downloaded_[b]) {
                    uint32_t begin = b << 14;

                    bool already_requested = false;
                    for (const auto &req: active_requests) {
                        if (req.index_ == piece.id_ && req.begin_ == begin) {
                            already_requested = true;
                            break;
                        }
                    }

                    if (!already_requested) {
                        Block ans;
                        ans.index_ = piece.id_;
                        ans.begin_ = begin;
                        ans.length_ = std::min(piece.total_size_ - ans.begin_, static_cast<size_t>(1 << 14));
                        return ans;
                    }
                }
            }
        }
    }

//    std::cerr << "No data for this peer" << std::endl;
    preferred_piece = uint32_t(-1);
    return {};
}

Piece &PieceManager::GetPieceById(size_t id) { return pieces_.at(id); }

void PieceManager::CheckDownload(size_t id, int efd) {
    if (id > pieces_.size() || pieces_[id].state_ == Piece::HASHING) {
        std::cerr << "Wrong INDEX for Piece OR already Hashing" << std::endl;
        return;
    }
    pieces_[id].state_ = Piece::HASHING;
    threadPool.Enqueue([efd, id, this]() {
        bool ans = this->pieces_[id].CheckDownload(this->tf_.pieceHashes[id]);
        {
            std::lock_guard<std::mutex> lock(out_mutex);
            hash_results.emplace(id, ans);
        }
        uint64_t msg = 1;
        uint8_t cnt_er = 0;
        do {
            if (write(efd, &msg, sizeof(msg)) != -1) break;
        } while (++cnt_er < 5);
    });
}

void PieceManager::SetDownloaded(size_t id) {
    if (id > pieces_.size()) {
        std::cerr << "Wrong INDEX for Piece" << std::endl;
        return;
    }
    pieces_[id].state_ = Piece::DOWNLOADED;

    downloading_pieces_.Reset(id);
}

void PieceManager::ResetPiece(size_t id) {
    if (id > pieces_.size()) {
        std::cerr << "Wrong INDEX for Piece" << std::endl;
        return;
    }
    pieces_[id].ResetPiece();

    downloading_pieces_.Reset(id);
    missing_pieces_.Set(id);
}

void PieceManager::SetSaved(size_t id) {
    ++downloaded_pieces;
    pieces_[id].state_ = Piece::SAVED;
    pieces_[id].piece_data_.clear();
    pieces_[id].piece_data_.shrink_to_fit();
//    while (!pieces_.empty() && pieces_.back().state_ == Piece::SAVED)
//        pieces_.pop_back();
}

bool PieceManager::FinishedDownloading() const {
    return downloaded_pieces == pieces_.size();
}

size_t PieceManager::SavedCount() const {
    return downloaded_pieces;
}

size_t PieceManager::TotalPiecesToDownload() const {
    return pieces_.size();
}


Piece::Piece(size_t id, size_t this_length, dynamic_bitset &down) :
        total_size_(this_length), id_(id), downloading_pieces_(down) {
    size_t offset = 0, total_blocks = total_size_ / (1 << 14);

    if (total_blocks * (1 << 14) < total_size_) ++total_blocks;
    not_downloaded_begins_.reserve(total_blocks);

    while (offset < total_size_) {
        not_downloaded_begins_.emplace_back(offset);
        offset += 1 << 14;
    }

    downloaded_.resize(not_downloaded_begins_.size(), false);
}

void Piece::StartDownload() {
    state_ = DOWNLOADING;
    piece_data_.resize(total_size_);
}

void Piece::FillDownloaded(size_t offset, std::span<uint8_t> data) {
    if (offset % (1 << 14) || (offset >> 14) >= downloaded_.size()) {
        std::cerr << "Fill downloaded wrong offset" << std::endl;
        return;
    }

    if (!downloaded_[offset >> 14]) {
        std::copy(data.begin(), data.end(), piece_data_.begin() + (int) offset);
        ++blocks_downloaded;
        downloaded_[offset >> 14] = true;
    }
}

void Piece::ResetBlock(size_t offset) {
    if (offset % (1 << 14) || (offset >> 14) >= downloaded_.size()) {
        std::cerr << "Fill downloaded wrong offset" << std::endl;
        return;
    }

    if (find(not_downloaded_begins_.begin(), not_downloaded_begins_.end(), offset) == not_downloaded_begins_.end())
        not_downloaded_begins_.emplace_back(offset);

    if (downloaded_[offset >> 14]) {
        downloaded_[offset >> 14] = false;
        --blocks_downloaded;
    }

    downloading_pieces_.Set(id_);
}

void Piece::ResetPiece() {
    piece_data_.clear();
    piece_data_.shrink_to_fit();
    blocks_downloaded = 0;
    std::fill(downloaded_.begin(), downloaded_.end(), 0);
    state_ = MISSING;

    not_downloaded_begins_.resize(downloaded_.size());
    for (size_t i = 0; i < not_downloaded_begins_.size(); ++i)
        not_downloaded_begins_[i] = i << 14;
}

bool Piece::CheckDownload(std::string_view check_hash) const {
    std::array<uint8_t, 20> hash = CalculateSHA1(piece_data_);
    for (int i = 0; i < 20; ++i)
        if (hash[i] != (uint8_t) check_hash[i]) {
            std::cerr << "Corrupted Piece" << std::endl;
            return false;
        }
    return true;
}

Block Piece::GetNextBlock() {
    if (state_ == MISSING) StartDownload();
    if (state_ != DOWNLOADING) {
        std::cerr << "Called next block on downloaded piece" << std::endl;
        return {};
    }
    Block ans;
    ans.index_ = id_;
    ans.begin_ = not_downloaded_begins_.back();
    ans.length_ = std::min(total_size_ - ans.begin_, static_cast<size_t>(1 << 14));
    not_downloaded_begins_.pop_back();

    if (not_downloaded_begins_.empty()) downloading_pieces_.Reset(id_);

    return ans;
}

bool Piece::IsDownloaded() const {
    return state_ == DOWNLOADED || (state_ == DOWNLOADING && blocks_downloaded == downloaded_.size());
}






