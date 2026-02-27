#include "../include/torrent_file.h"
#include <iostream>
#include <filesystem>
#include <random>
#include <thread>
#include <set>

namespace fs = std::filesystem;

std::mutex cerrMutex, coutMutex, peersMutex;

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

//---------------------------check hash----------------------------------------------

void CheckDownloadedPiecesIntegrity(const std::filesystem::path &outputFilename, const TorrentFile &tf,
                                    PieceStorage &pieces) {
    pieces.CloseOutputFile();

    if (pieces.GetPiecesSavedToDiscIndices().size() != pieces.PiecesSavedToDiscCount()) {
        throw std::runtime_error("Cannot determine real amount of saved pieces");
    }

    std::vector<size_t> pieceIndices = pieces.GetPiecesSavedToDiscIndices();
    std::sort(pieceIndices.begin(), pieceIndices.end());

    std::ifstream file(outputFilename, std::ios_base::binary);
    for (size_t pieceIndex: pieceIndices) {
        const std::streamoff positionInFile = pieceIndex * tf.pieceLength;
        file.seekg(positionInFile);
        if (!file.good()) {
            throw std::runtime_error("Cannot read from file");
        }
        std::string pieceDataFromFile(tf.pieceLength, '\0');
        file.read(pieceDataFromFile.data(), tf.pieceLength);
        const size_t readBytesCount = file.gcount();
        pieceDataFromFile.resize(readBytesCount);
        const std::string realHash = CalculateSHA1(pieceDataFromFile);

        if (realHash != tf.pieceHashes[pieceIndex]) {
            std::cerr << "File piece with index " << pieceIndex << " started at position " << positionInFile <<
                      " with length " << pieceDataFromFile.length() << " has wrong hash " << HexEncode(realHash) <<
                      ". Expected hash is " << HexEncode(tf.pieceHashes[pieceIndex]) << std::endl;
            throw std::runtime_error("Wrong piece hash");
        }
    }
}

//-------------------starting and downloading----------------------------------------

std::vector<std::thread> peerThreads;
std::vector<std::shared_ptr<PeerConnect>> peerConnections;
std::set<std::string> connectedPeers;

void StartNewPeerThreads(PieceStorage &pieces, const TorrentFile &torrentFile,
                         const std::string &ourId, const TorrentTracker &tracker) {
    const std::vector<Peer> &peers = tracker.GetPeers();

    std::lock_guard<std::mutex> lock(peersMutex);
    for (const auto &peer: peers) {
        std::string peerKey = peer.ip + ":" + std::to_string(peer.port);
        if (!connectedPeers.contains(peerKey)) {
            std::cout << "New peer found: " << peerKey << std::endl;
            const auto peerConnectPtr = std::make_shared<PeerConnect>(peer, torrentFile, ourId, pieces);
            peerConnections.emplace_back(peerConnectPtr);
            peerThreads.emplace_back([peerConnectPtr, peerKey]() {
                int attempts = 0;
                while (attempts < 3 && !peerConnectPtr->IsTerminated()) {
                    try {
                        ++attempts;
                        peerConnectPtr->Run();
                    } catch (const std::runtime_error &e) {
                        std::lock_guard<std::mutex> cerrLock(cerrMutex);
                        std::cerr << "Runtime error: " << e.what() << std::endl;
                    } catch (const std::exception &e) {
                        std::lock_guard<std::mutex> cerrLock(cerrMutex);
                        std::cerr << "Exception: " << e.what() << std::endl;
                    } catch (...) {
                        std::lock_guard<std::mutex> cerrLock(cerrMutex);
                        std::cerr << "Unknown error" << std::endl;
                    }
                    if (peerConnectPtr->Failed()) break;
                }

                std::lock_guard<std::mutex> lock(peersMutex);
                connectedPeers.erase(peerKey);
            });
            connectedPeers.emplace(peerKey);
        }
    }
}


void DownloadTorrentFile(const TorrentFile &torrentFile, PieceStorage &pieces, const std::string &ourId) {
    using namespace std::chrono_literals;
    std::cout << "Connecting to tracker " << torrentFile.announce << std::endl;
    TorrentTracker tracker(torrentFile.announce);
    const size_t piecesToDownload = pieces.TotalPiecesCount();

    auto prev_time_updated_peers = std::chrono::steady_clock::now();
    int64_t interval_for_update = 0;

    while (pieces.PiecesSavedToDiscCount() < piecesToDownload) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - prev_time_updated_peers).count();

        if (pieces.PiecesInProgressCount() == 0 || elapsed >= interval_for_update) {
            try {
                std::cout << "Updating peers from tracker..." << std::endl;

                size_t downloaded_bytes =
                        static_cast<size_t>(pieces.PiecesSavedToDiscCount()) * torrentFile.pieceLength;
                downloaded_bytes = std::min(downloaded_bytes, torrentFile.length);
                interval_for_update = tracker.UpdatePeers(torrentFile, ourId, 6881, downloaded_bytes);
                std::cout << "New interval: " << interval_for_update << std::endl;

                prev_time_updated_peers = std::chrono::steady_clock::now();

                StartNewPeerThreads(pieces, torrentFile, ourId, tracker);
            } catch (const std::exception &e) {
                std::cerr << "Tracker update failed: " << e.what() << std::endl;
                if (interval_for_update == 0) {
                    std::cout << "Trying 1 more time in 30s" << std::endl;
                    interval_for_update = 30;
                    std::this_thread::sleep_for(10s);
                } else {
                    std::cerr << "No peers found. Cannot download a file" << std::endl;
                    throw std::runtime_error("No peers available");
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(coutMutex);
            std::cout << "Progress: " << pieces.PiecesSavedToDiscCount() << "/" << piecesToDownload
                      << " pieces. Active threads: " << peerThreads.size() << std::endl;
        }

        std::this_thread::sleep_for(1s);
    }

    std::cout << "Download finished. Terminating threads..." << std::endl;
    for (auto &conn: peerConnections) conn->Terminate();
    for (auto &t: peerThreads) if (t.joinable()) t.join();
}


int main(int argc, char *argv[]) {
    std::string download_dir = ".";
    int percent = 100;
    std::string torrent_file_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-d" && i + 1 < argc) {
            download_dir = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            percent = std::stoi(argv[++i]);
            if (percent < 1 || percent > 100) {
                std::cerr << "Invalid percentage" << std::endl;
                return 1;
            }
        } else {
            torrent_file_path = arg;
        }
    }

    if (torrent_file_path.empty()) {
        std::cerr << "-d <download_dir> -p <percent> <torrent_file>" << std::endl;
        return 1;
    }


    try {
        TorrentFile torrentFile;

        torrentFile = LoadTorrentFile(torrent_file_path);
        std::cout << "Loaded torrent file " << torrentFile.name << ". " << torrentFile.comment << std::endl;

        PieceStorage pieces(torrentFile, download_dir, percent);

        DownloadTorrentFile(torrentFile, pieces, PeerId);

        fs::path outputFile = fs::path(download_dir) / torrentFile.name;
        CheckDownloadedPiecesIntegrity(outputFile, torrentFile, pieces);

        std::cout << "Successfully downloaded " << pieces.PiecesSavedToDiscCount()
                  << " pieces and verified integrity" << std::endl;

    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}