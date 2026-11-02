// C++20, single-writer fixed-index History + conservative seq_cst EBR.
// g++ -std=c++20 -O2 -pthread -Wall -Wextra -pedantic ebr_history_seq.cpp -o ebr_history_seq
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

template <std::size_t Capacity, std::size_t Readers>
class EpochHistory {
    static_assert(Capacity > 0 && Readers > 0);

public:
    struct Entry {
        const std::uint64_t seq;
        const std::uint64_t value;
    };

private:
    struct alignas(64) ReaderRecord {
        // 0: inactive. Other values: global epoch observed on entry.
        std::atomic<std::uint64_t> epoch{0};
    };
    struct Retired {
        const Entry* ptr;
        std::uint64_t threshold;
    };

    std::array<std::atomic<const Entry*>, Capacity> slots_{};
    std::array<ReaderRecord, Readers> readers_{};
    std::atomic<std::uint64_t> globalEpoch_{1};
    std::atomic<std::uint64_t> latest_{0};
    std::vector<Retired> retired_; // accessed by writer only
    std::uint64_t writerSeq_ = 0;  // accessed by writer only

public:
    class Guard {
        EpochHistory& history_;
        const std::size_t id_;

    public:
        explicit Guard(EpochHistory& history, std::size_t readerId)
            : history_(history), id_(readerId) {
            assert(id_ < Readers);
            // One distinct readerId per concurrent thread/Guard.
            assert(history_.readers_[id_].epoch.load(
                       std::memory_order_seq_cst) == 0);
            const auto e = history_.globalEpoch_.load(
                std::memory_order_seq_cst);
            // Must be visible before protected pointer loads.
            history_.readers_[id_].epoch.store(
                e, std::memory_order_seq_cst);
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&&) = delete;
        Guard& operator=(Guard&&) = delete;
        ~Guard() {
            // Only leave after the LAST dereference of an Entry.
            history_.readers_[id_].epoch.store(
                0, std::memory_order_seq_cst);
        }

        const Entry* lookup(std::uint64_t wantedSeq) const noexcept {
            if (wantedSeq == 0) return nullptr;
            const Entry* ptr = history_.slots_[wantedSeq % Capacity].load(
                std::memory_order_seq_cst);
            // EBR protects lifetime, but not the identity of a reused index.
            return ptr && ptr->seq == wantedSeq ? ptr : nullptr;
        }
    };

    EpochHistory() {
        for (auto& slot : slots_)
            slot.store(nullptr, std::memory_order_relaxed);
    }
    EpochHistory(const EpochHistory&) = delete;
    EpochHistory& operator=(const EpochHistory&) = delete;
    ~EpochHistory() {
        // All reader threads must be joined and all guards destroyed.
        for (const auto& r : readers_)
            assert(r.epoch.load(std::memory_order_seq_cst) == 0);
        for (auto& slot : slots_)
            delete slot.load(std::memory_order_relaxed);
        for (const auto& item : retired_) delete item.ptr;
    }

    Guard pin(std::size_t readerId) { return Guard(*this, readerId); }

    // Single writer only. Each Entry is allocated separately, then immutable.
    void publish(std::uint64_t seq, std::uint64_t value) {
        assert(seq == writerSeq_ + 1);
        const Entry* fresh = new Entry{seq, value};
        auto& slot = slots_[seq % Capacity];
        const Entry* old = slot.exchange(fresh, std::memory_order_seq_cst);

        // Advance *after* unlink, using this value as reclamation threshold.
        const auto threshold = globalEpoch_.fetch_add(
                                   1, std::memory_order_seq_cst) + 1;
        if (old) retired_.push_back({old, threshold});
        writerSeq_ = seq;
        latest_.store(seq, std::memory_order_release);

        if ((seq & 63) == 0) collect();
    }

    std::uint64_t latest() const noexcept {
        return latest_.load(std::memory_order_acquire);
    }

    // Single-writer collection; reader records are stable for whole lifetime.
    void collect() {
        std::array<std::uint64_t, Readers> snapshot{};
        for (std::size_t i = 0; i < Readers; ++i)
            snapshot[i] = readers_[i].epoch.load(
                std::memory_order_seq_cst);

        std::size_t keep = 0;
        for (const Retired& r : retired_) {
            const bool blocked = std::any_of(
                snapshot.begin(), snapshot.end(), [&](std::uint64_t e) {
                    return e != 0 && e < r.threshold;
                });
            if (blocked) retired_[keep++] = r;
            else delete r.ptr;
        }
        retired_.resize(keep);
    }

    // Diagnostics, writer thread only.
    std::size_t retiredCount() const noexcept { return retired_.size(); }
    std::uint64_t currentEpoch() const noexcept {
        return globalEpoch_.load(std::memory_order_seq_cst);
    }
};

int main() {
    // Test A: EBR protects a whole interval, not one specific pointer.
    {
        EpochHistory<8, 2> history;
        for (std::uint64_t s = 1; s <= 8; ++s)
            history.publish(s, s * 10);
        {
            auto guard = history.pin(0);
            const auto* old = guard.lookup(3);
            assert(old && old->value == 30);

            // The writer unlinks seq 1,2,3 in successive publications.
            for (std::uint64_t s = 9; s <= 11; ++s)
                history.publish(s, s * 10);
            history.collect();
            // Even though we only read seq 3, the old epoch protects all 3.
            assert(history.retiredCount() == 3);
            assert(guard.lookup(3) == nullptr); // removed from the index
            assert(old->seq == 3 && old->value == 30); // old ptr still alive
        } // guard exits -> reader epoch = inactive
        history.collect();
        assert(history.retiredCount() == 0);
        std::cout << "epoch window / old pointer: PASS\n";
    }

    // Test B: multiple reader threads and a writer, history lookups in batches.
    {
        EpochHistory<64, 4> history;
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> hits{0}, misses{0};
        std::vector<std::thread> threads;
        for (std::size_t id = 0; id < 4; ++id) {
            threads.emplace_back([&, id] {
                std::uint64_t rnd = id + 17;
                while (!stop.load(std::memory_order_acquire)) {
                    // Pin once and read multiple Entries in one critical section.
                    auto guard = history.pin(id);
                    const auto top = history.latest();
                    if (top == 0) continue;
                    for (int k = 0; k < 8; ++k) {
                        rnd = rnd * 6364136223846793005ULL + 1;
                        const auto back = (rnd >> 32) % 96;
                        const auto wanted = top > back ? top - back : 1;
                        const auto* p = guard.lookup(wanted);
                        if (p) {
                            assert(p->seq == wanted);
                            assert(p->value == wanted * 10);
                            hits.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            misses.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            });
        }
        for (std::uint64_t s = 1; s <= 50000; ++s)
            history.publish(s, s * 10);
        stop.store(true, std::memory_order_release);
        for (auto& t : threads) t.join();
        history.collect();
        assert(history.retiredCount() == 0);
        {
            auto guard = history.pin(0);
            const auto* last = guard.lookup(50000);
            assert(last && last->value == 500000);
        }
        std::cout << "concurrent replay lookup: PASS; hits=" << hits
                  << ", misses=" << misses << '\n';
    }
}
