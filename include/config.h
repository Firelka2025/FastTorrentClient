#pragma once

#include <cstdlib>
#include <liburing.h>
#include "chrono"

constexpr uint RECEIVE_MAX_BYTES{1 << 19};
constexpr uint SEND_MAX_BYTES{68};
constexpr uint PEER_MEMORY_SIZE{RECEIVE_MAX_BYTES + SEND_MAX_BYTES};
constexpr size_t SEND_BUFFER_RESERVE{1 << 16};


constexpr uint io_uring_min_queries_req{32};
constexpr std::chrono::microseconds io_uring_max_prep_time{5000};

extern struct io_uring main_ring;

void prepare_io_uring_pack(bool update = false);