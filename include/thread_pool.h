#pragma once

#include <cstddef>
#include <vector>
#include <thread>
#include <functional>
#include <condition_variable>
#include <queue>
#include "piece_manager.h"


struct HashResult {
    size_t piece_index_ = size_t(-1);
    bool is_valid_ = false;

    HashResult(size_t id, bool val) : piece_index_(id), is_valid_(val) {}
};

class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads);

    ~ThreadPool();

    void Enqueue(const std::function<void()> &f);


private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;

    std::mutex queueMutex;
    std::condition_variable condition;
    bool stop;

    std::mutex countMutex;
    std::condition_variable cvDone;
    size_t activeTasks;
};