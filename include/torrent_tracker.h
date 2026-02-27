#pragma once

#include <string>
#include "torrent_file.h"
#include "peer.h"

class TorrentTracker {
public:
    /*
     * url - адрес трекера, берется из поля announce в .torrent-файле
     */
    explicit TorrentTracker(std::string url);

    /*
     * Получить список пиров у трекера и сохранить его для дальнейшей работы.
     * Запрос пиров происходит посредством HTTP GET запроса, данные передаются в формате bencode.
     * Такой же формат использовался в .torrent файле.
     *
     * Возвращает интервал для следующего запроса в секундах
     *
     * tf: структура с разобранными данными из .torrent файла из предыдущего домашнего задания.
     * peerId: id, под которым представляется наш клиент.
     * port: порт, на котором наш клиент будет слушать входящие соединения (пока что мы не слушаем и на этот порт никто
     *  не сможет подключиться).
     */
    int64_t UpdatePeers(const TorrentFile &tf, const std::string &peerId, int port, size_t downloaded = 0);

    /*
     * Отдает полученный ранее список пиров
     */
    [[nodiscard]] const std::vector<Peer> &GetPeers() const;

private:
    std::string url_;
    std::vector<Peer> peers_;
};
