// Lock-free SPSC ring buffer tests.

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "nanobook/spsc_queue.hpp"
#include "test_framework.hpp"

using namespace nanobook;

NB_TEST(spsc_layout_is_cache_line_aligned) {
    NB_CHECK_EQ(alignof(SPSCQueue<int>), kCacheLineSize);
    // head, tail and the read-only fields each get their own line.
    NB_CHECK(sizeof(SPSCQueue<int>) >= 3 * kCacheLineSize);
}

NB_TEST(spsc_capacity_rounds_up_to_power_of_two) {
    NB_CHECK_EQ(SPSCQueue<int>(0).capacity(), 2u);
    NB_CHECK_EQ(SPSCQueue<int>(5).capacity(), 8u);
    NB_CHECK_EQ(SPSCQueue<int>(1024).capacity(), 1024u);
}

NB_TEST(spsc_empty_queue_pops_nothing) {
    SPSCQueue<int> q(8);
    int            out = -1;
    NB_CHECK(!q.try_pop(out));
    NB_CHECK_EQ(out, -1);
    NB_CHECK(q.front() == nullptr);
    NB_CHECK(q.empty_approx());
    NB_CHECK_EQ(q.size_approx(), 0u);
}

NB_TEST(spsc_full_queue_rejects_push) {
    SPSCQueue<int> q(4);
    for (int i = 0; i < 4; ++i) NB_CHECK(q.try_push(i));
    NB_CHECK_EQ(q.size_approx(), 4u);
    NB_CHECK(!q.try_push(99));

    int out = 0;
    NB_CHECK(q.try_pop(out));
    NB_CHECK_EQ(out, 0);
    NB_CHECK(q.try_push(4));  // one slot freed
    NB_CHECK(!q.try_push(5));

    for (int expected = 1; expected <= 4; ++expected) {
        NB_CHECK(q.try_pop(out));
        NB_CHECK_EQ(out, expected);
    }
    NB_CHECK(!q.try_pop(out));
}

NB_TEST(spsc_fifo_order_survives_wraparound) {
    SPSCQueue<std::uint64_t> q(4);
    std::uint64_t            next_in  = 0;
    std::uint64_t            next_out = 0;
    bool                     ordered  = true;

    // Uneven batch sizes walk the indices around the ring thousands of times.
    for (int round = 0; round < 5'000; ++round) {
        const int batch = 1 + round % 4;
        for (int i = 0; i < batch; ++i) NB_CHECK(q.try_push(next_in++));
        for (int i = 0; i < batch; ++i) {
            std::uint64_t v = 0;
            NB_CHECK(q.try_pop(v));
            ordered = ordered && v == next_out++;
        }
    }
    NB_CHECK(ordered);
    NB_CHECK_EQ(next_out, next_in);
    NB_CHECK(next_in > 4 * q.capacity());
    NB_CHECK(q.empty_approx());
}

NB_TEST(spsc_front_and_pop_give_zero_copy_access) {
    SPSCQueue<int> q(2);
    NB_CHECK(q.try_push(7));
    NB_CHECK(q.try_push(8));
    int* head = q.front();
    NB_CHECK(head != nullptr);
    NB_CHECK_EQ(*head, 7);
    q.pop();
    NB_CHECK_EQ(*q.front(), 8);
    q.pop();
    NB_CHECK(q.front() == nullptr);
}

NB_TEST(spsc_supports_move_only_types) {
    SPSCQueue<std::unique_ptr<int>> q(4);
    NB_CHECK(q.try_push(std::make_unique<int>(42)));
    NB_CHECK(q.try_emplace(new int(43)));

    std::unique_ptr<int> out;
    NB_CHECK(q.try_pop(out));
    NB_CHECK(out != nullptr && *out == 42);
    NB_CHECK(q.try_pop(out));
    NB_CHECK(out != nullptr && *out == 43);
}

NB_TEST(spsc_two_thread_stress_preserves_order_and_payload) {
    // Two-word payload: a torn or stale read would break the checksum relation.
    struct Message {
        std::uint64_t seq   = 0;
        std::uint64_t check = 0;
    };
    constexpr std::uint64_t kCount = 2'000'000;
    constexpr auto          mix    = [](std::uint64_t v) { return v * 0x9E3779B97F4A7C15ull ^ 0xA5A5A5A5ull; };

    SPSCQueue<Message> q(1024);  // small ring: forces constant full/empty transitions
    std::uint64_t      received  = 0;
    std::uint64_t      sum       = 0;
    bool               in_order  = true;
    bool               intact    = true;

    std::thread consumer([&] {
        Message m;
        while (received < kCount) {
            if (!q.try_pop(m)) continue;
            in_order = in_order && m.seq == received;
            intact   = intact && m.check == mix(m.seq);
            sum += m.seq;
            ++received;
        }
    });

    for (std::uint64_t i = 0; i < kCount; ++i) {
        const Message m{i, mix(i)};
        while (!q.try_push(m)) {
        }
    }
    consumer.join();

    NB_CHECK_EQ(received, kCount);
    NB_CHECK(in_order);
    NB_CHECK(intact);
    NB_CHECK_EQ(sum, kCount * (kCount - 1) / 2);
    NB_CHECK(q.empty_approx());
}
