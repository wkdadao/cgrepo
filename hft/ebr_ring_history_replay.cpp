// ebr_ring_history_replay.cpp
// g++ -std=c++20 -O2 -pthread -Wall -Wextra -Wpedantic ebr_ring_history_replay.cpp -o ebr_ring_history_replay
// Fixed-slot, in-place history ring with one writer and an epoch-based
// grace period before overwriting a previously published slot.
// No dynamic allocations for data records, no pointer retirement.
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

// Only suitable for the documented single-writer / fixed-reader model.
// All publication, pin and grace-period operations use seq_cst for clarity.
template <std::size_t Capacity, std::size_t ReaderCount>
class EpochReplayRing {
    static_assert(Capacity > 0 && ReaderCount > 0);

public:
    struct Data {
        std::uint64_t seq = 0;
        std::uint64_t value = 0;
        std::uint64_t createdEpoch = 0;
    };

private:
    struct alignas(64) Slot {
        std::atomic<std::uint64_t> publishedSeq{0}; // 0 = unavailable
        Data data{}; // never concurrently read and overwritten
    };

    struct alignas(64) ReaderRecord {
        std::atomic<std::uint64_t> announcedEpoch{0}; // 0 = inactive
    };

    std::array<Slot, Capacity> slots_{};
    std::array<ReaderRecord, ReaderCount> readers_{};
    std::atomic<std::uint64_t> epoch_{1};
    std::atomic<std::uint64_t> latest_{0};
    std::uint64_t lastPublished_ = 0; // single-writer only

    void waitForReadersBefore(std::uint64_t threshold) const noexcept {
        // The writer must finish the grace period BEFORE touching slot.data.
        for (;;) {
            bool safe = true;
            for (const auto& r : readers_) {
                const auto e = r.announcedEpoch.load(
                    std::memory_order_seq_cst);
                if (e != 0 && e < threshold) {
                    safe = false;
                    break;
                }
            }
            if (safe) return;
            std::this_thread::yield(); // demo only: real-time policy is workload-specific
        }
    }

public:
    class Guard {
        EpochReplayRing* owner_;
        std::size_t readerId_;
        std::uint64_t epoch_;

    public:
        Guard(EpochReplayRing& owner, std::size_t readerId)
            : owner_(&owner), readerId_(readerId), epoch_(0) {
            if (readerId >= ReaderCount)
                throw std::out_of_range("readerId");
            // Reader ID must have a single owner, and guards cannot nest.
            assert(owner_->readers_[readerId].announcedEpoch.load(
                       std::memory_order_seq_cst) == 0);
            epoch_ = owner_->epoch_.load(std::memory_order_seq_cst);
            // This announcement must precede every Slot load within guard.
            owner_->readers_[readerId].announcedEpoch.store(
                epoch_, std::memory_order_seq_cst);
        }

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&&) = delete;
        Guard& operator=(Guard&&) = delete;

        ~Guard() {
            // After this store no pointer to Slot::data may be used.
            owner_->readers_[readerId_].announcedEpoch.store(
                0, std::memory_order_seq_cst);
        }

        std::uint64_t epoch() const noexcept { return epoch_; }

        // May read an earlier epoch's historical data as long as still in ring.
        // The returned pointer is valid ONLY within this Guard's lifetime.
        const Data* lookup(std::uint64_t wantedSeq) const noexcept {
            if (wantedSeq == 0) return nullptr;
            const auto& slot = owner_->slots_[wantedSeq % Capacity];
            auto seq = slot.publishedSeq.load(std::memory_order_seq_cst);
            if (seq != wantedSeq) return nullptr;
            // Safe because any overwrite must await a grace period.
            return &slot.data;
        }
    };

    EpochReplayRing() = default;
    EpochReplayRing(const EpochReplayRing&) = delete;
    EpochReplayRing& operator=(const EpochReplayRing&) = delete;

    ~EpochReplayRing() {
        // Caller must join all Reader threads and destroy all Guards.
        for (const auto& r : readers_) {
            assert(r.announcedEpoch.load(std::memory_order_seq_cst) == 0);
        }
    }

    Guard pin(std::size_t readerId) { return Guard{*this, readerId}; }
    std::uint64_t latest() const noexcept {
        return latest_.load(std::memory_order_acquire);
    }
    std::uint64_t globalEpoch() const noexcept {
        return epoch_.load(std::memory_order_seq_cst);
    }

    // Exactly one publishing thread at a time. Sequences must be 1,2,3...
    // Test hook invalidatedSignal is optional and must outlive the call.
    void publish(std::uint64_t seq, std::uint64_t value,
                 std::atomic<bool>* invalidatedSignal = nullptr) {
        if (lastPublished_ == UINT64_MAX || seq != lastPublished_ + 1)
            throw std::logic_error("publish requires contiguous seq");

        auto& slot = slots_[seq % Capacity];
        // First make the previous generation unavailable to new readers.
        const auto oldSeq = slot.publishedSeq.exchange(
            0, std::memory_order_seq_cst);

        if (oldSeq != 0) {
            // Invalidation occurs before epoch advancement.
            const auto threshold = epoch_.fetch_add(
                1, std::memory_order_seq_cst) + 1;
            if (invalidatedSignal)
                invalidatedSignal->store(true, std::memory_order_release);
            waitForReadersBefore(threshold);
        }
        // No reader can currently be accessing the old Slot::data.
        // Data is non-atomic; concurrent access would otherwise be UB.
        slot.data = Data{seq, value, epoch_.load(std::memory_order_seq_cst)};
        slot.publishedSeq.store(seq, std::memory_order_seq_cst);
        lastPublished_ = seq;
        latest_.store(seq, std::memory_order_release);
    }
};

int main() {
    // A: new Epoch Reader queries data published in a much older epoch.
    // B: Writer cannot overwrite that historic Slot while the Guard is live.
    {
        EpochReplayRing<8, 2> ring;
        for (std::uint64_t seq = 1; seq <= 12; ++seq)
            ring.publish(seq, seq * 10);

        std::atomic<bool> invalidated{false};
        std::atomic<bool> publishFinished{false};
        std::thread writer;

        {
            auto guard = ring.pin(0);
            const auto* historical = guard.lookup(5);  // latest=12, oldest=5
            assert(historical && historical->seq == 5);
            assert(historical->value == 50);
            // This Reader's epoch is NEWER than the data's creation epoch.
            assert(guard.epoch() > historical->createdEpoch);
            assert(guard.lookup(4) == nullptr); // already overwritten
            assert(guard.lookup(12)->value == 120);

            writer = std::thread([&] {
                ring.publish(13, 130, &invalidated); // reuse seq 5 slot
                publishFinished.store(true, std::memory_order_release);
            });

            while (!invalidated.load(std::memory_order_acquire))
                std::this_thread::yield();

            // Slot was invalidated; writer must wait for guard to finish.
            assert(!publishFinished.load(std::memory_order_acquire));
            assert(guard.lookup(5) == nullptr);  // no NEW acquisition
            assert(historical->seq == 5);        // old pointer still safe
            assert(historical->value == 50);
        } // Guard exits; grace period can complete.

        writer.join();
        assert(publishFinished.load(std::memory_order_acquire));
        {
            auto guard = ring.pin(0);
            assert(guard.lookup(5) == nullptr);
            const auto* current = guard.lookup(13);
            assert(current && current->value == 130);
        }
        std::cout << "historical lookup and overwrite grace period: PASS\n";
    }

    // C: concurrent random lookups for the currently retained history window.
    {
        EpochReplayRing<64, 4> ring;
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> hits{0}, misses{0};
        std::vector<std::thread> readerThreads;
        for (std::size_t id = 0; id < 4; ++id) {
            readerThreads.emplace_back([&, id] {
                std::uint64_t rng = id + 17;
                while (!stop.load(std::memory_order_acquire)) {
                    auto guard = ring.pin(id);
                    const auto top = ring.latest();
                    if (!top) continue;
                    // A single Epoch Guard protects multiple Slot reads.
                    for (int k = 0; k < 4; ++k) {
                        rng = rng * 6364136223846793005ULL + 1;
                        auto back = (rng >> 32) % 96;
                        auto wanted = top > back ? top - back : 1;
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
        for (std::uint64_t seq = 1; seq <= 20000; ++seq)
            ring.publish(seq, seq * 10);
        stop.store(true, std::memory_order_release);
        for (auto& t : readerThreads) t.join();
        {
            auto guard = ring.pin(0);
            assert(guard.lookup(19936) == nullptr);
            assert(guard.lookup(19937)->value == 199370);
            assert(guard.lookup(20000)->value == 200000);
        }
        std::cout << "concurrent history: PASS; hits=" << hits.load()
                  << ", misses=" << misses.load() << '\n';
    }
}
