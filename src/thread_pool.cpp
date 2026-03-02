#include "thread_pool.h"


// Создаём пул с указанным количеством рабочих потоков
ThreadPool::ThreadPool(size_t numThreads) : stop(false), activeTasks(0) {
    for (size_t i = 0; i < numThreads; ++i) {
        workers.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(queueMutex);
                    condition.wait(lock, [this] { return stop || !tasks.empty(); });
                    if (stop && tasks.empty()) return;
                    task = std::move(tasks.front());
                    tasks.pop();
                }
                task();
                {
                    std::lock_guard<std::mutex> lock(countMutex);
                    --activeTasks;
                    if (activeTasks == 0) cvDone.notify_one();
                }
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stop = true;
    }
    condition.notify_all();
    for (std::thread &worker: workers)
        if (worker.joinable()) worker.join();
}

void ThreadPool::Enqueue(const std::function<void()> &f) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (stop) throw std::runtime_error("Enqueue on stopped ThreadPool");
        tasks.emplace(f);
    }
    {
        std::lock_guard<std::mutex> lock(countMutex);
        ++activeTasks;
    }
    condition.notify_one();
}

