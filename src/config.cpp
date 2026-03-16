#include "config.h"

struct io_uring main_ring = {};

void prepare_io_uring_pack(bool update) {
    using namespace std::chrono;
    static auto prev_time = steady_clock::now();
    static uint counted_commands = 0;

    counted_commands -= static_cast<uint>(update);
    if (++counted_commands == io_uring_min_queries_req || steady_clock::now() - prev_time > io_uring_max_prep_time) {
        counted_commands = 0;
        io_uring_submit(&main_ring);
        prev_time = steady_clock::now();
    }
}