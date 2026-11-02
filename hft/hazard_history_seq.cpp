// hazard_history_seq.cpp
// g++ -std=c++20 -O2 -pthread -Wall -Wextra -pedantic hazard_history_seq.cpp -o hazard_history_seq
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

// 1 writer, fixed reader slots, at most 1 Pinned per reader ID.
// Sequence does not wrap or repeat; stop all readers before destruction.
template <std::size_t Capacity, std::size_t ReaderCount>
class SequenceHistory {
    static_assert(Capacity > 0 && ReaderCount > 0);
public:
    struct Entry {
        const std::uint64_t seq;
        const std::uint64_t value;
    };
private:
    struct alignas(64) HazardSlot {
        std::atomic<const Entry*> ptr{nullptr};
    };
    std::array<std::atomic<const Entry*>, Capacity> index_{};
    std::array<HazardSlot, ReaderCount> hazards_{};
    std::atomic<std::uint64_t> latest_{0};
    std::vector<const Entry*> retired_;  // writer-only
    std::uint64_t lastPublished_ = 0;     // writer-only
public:
    class Pinned {
        friend class SequenceHistory;
        SequenceHistory* owner_ = nullptr;
        std::size_t readerId_ = 0;
        const Entry* ptr_ = nullptr;
        Pinned(SequenceHistory* owner, std::size_t id, const Entry* p)
            : owner_(owner), readerId_(id), ptr_(p) {}
    public:
        Pinned() = default;
        Pinned(const Pinned&) = delete;
        Pinned& operator=(const Pinned&) = delete;
        Pinned(Pinned&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              readerId_(other.readerId_),
              ptr_(std::exchange(other.ptr_, nullptr)) {}
        Pinned& operator=(Pinned&& other) noexcept {
            if (this != &other) {
                reset();
                owner_ = std::exchange(other.owner_, nullptr);
                readerId_ = other.readerId_;
                ptr_ = std::exchange(other.ptr_, nullptr);
            }
            return *this;
        }
        ~Pinned() { reset(); }
        void reset() noexcept {
            if (owner_) {
                // Last dereference must occur before this store.
                owner_->hazards_[readerId_].ptr.store(
                    nullptr, std::memory_order_seq_cst);
                owner_ = nullptr;
                ptr_ = nullptr;
            }
        }
        explicit operator bool() const noexcept { return ptr_ != nullptr; }
        const Entry& operator*() const { assert(ptr_); return *ptr_; }
        const Entry* operator->() const { assert(ptr_); return ptr_; }
    };

    SequenceHistory() = default;
    SequenceHistory(const SequenceHistory&) = delete;
    SequenceHistory& operator=(const SequenceHistory&) = delete;
    ~SequenceHistory() {
        // Join all threads and destroy outstanding Pinned first.
        for (const auto& h : hazards_)
            assert(h.ptr.load(std::memory_order_seq_cst) == nullptr);
        for (auto& loc : index_)
            delete loc.load(std::memory_order_seq_cst);
        for (const Entry* p : retired_) delete p;
    }

    void publish(std::uint64_t seq, std::uint64_t value) {
        assert(seq == lastPublished_ + 1);
        const Entry* fresh = new Entry{seq, value};
        auto& location = index_[seq % Capacity];
        const Entry* old = location.exchange(
            fresh, std::memory_order_seq_cst);
        if (old) retired_.push_back(old); // NEVER delete old immediately
        lastPublished_ = seq;
        latest_.store(seq, std::memory_order_release);
        if ((seq & 31) == 0) collect();
    }
    std::uint64_t latest() const {
        return latest_.load(std::memory_order_acquire);
    }

    Pinned lookup(std::uint64_t seq, std::size_t readerId) {
        assert(readerId < ReaderCount);
        assert(hazards_[readerId].ptr.load(std::memory_order_seq_cst)
               == nullptr && "one Pinned per reader ID");
        if (seq == 0) return {};
        auto& location = index_[seq % Capacity];
        const Entry* candidate;
        do {
            // DO NOT dereference before successful validation.
            candidate = location.load(std::memory_order_seq_cst);
            hazards_[readerId].ptr.store(
                candidate, std::memory_order_seq_cst);
        } while (candidate !=
                 location.load(std::memory_order_seq_cst));

        if (!candidate || candidate->seq != seq) {
            hazards_[readerId].ptr.store(
                nullptr, std::memory_order_seq_cst);
            return {}; // evicted or not published
        }
        return Pinned{this, readerId, candidate};
    }

    // Single writer takes a full hazard snapshot and compacts retired_.
    void collect() {
        std::array<const Entry*, ReaderCount> snapshot{};
        for (std::size_t i = 0; i < ReaderCount; ++i)
            snapshot[i] = hazards_[i].ptr.load(
                std::memory_order_seq_cst);
        std::size_t keep = 0;
        for (const Entry* old : retired_) {
            if (std::find(snapshot.begin(), snapshot.end(), old)
                  != snapshot.end())
                retired_[keep++] = old;
            else delete old;
        }
        retired_.resize(keep);
    }
    // Writer thread only.
    std::size_t retiredCount() const { return retired_.size(); }
};

int main() {
    {
        SequenceHistory<8, 2> history;
        for (std::uint64_t s = 1; s <= 8; ++s)
            history.publish(s, s * 10);
        auto old = history.lookup(3, 0);
        assert(old && old->value == 30);
        for (std::uint64_t s = 9; s <= 11; ++s)
            history.publish(s, s * 10);
        history.collect();
        assert(history.retiredCount() == 1);
        auto missing = history.lookup(3, 1);
        assert(!missing);
        assert(old->seq == 3 && old->value == 30);
        old.reset();
        history.collect();
        assert(history.retiredCount() == 0);
        std::cout << "old-version pin & eviction: PASS\n";
    }
    {
        SequenceHistory<64, 4> history;
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> hits{0}, misses{0};
        std::vector<std::thread> readers;
        for (std::size_t id = 0; id < 4; ++id)
            readers.emplace_back([&, id] {
                std::uint64_t rnd = id + 17;
                while (!stop.load(std::memory_order_acquire)) {
                    auto top = history.latest();
                    if (top == 0) continue;
                    rnd = rnd * 6364136223846793005ULL + 1;
                    auto back = (rnd >> 32) % 96;
                    auto wanted = top > back ? top - back : 1;
                    auto pin = history.lookup(wanted, id);
                    if (pin) {
                        assert(pin->seq == wanted);
                        assert(pin->value == wanted * 10);
                        hits.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        misses.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        for (std::uint64_t s = 1; s <= 50000; ++s)
            history.publish(s, s * 10);
        stop.store(true, std::memory_order_release);
        for (auto& t : readers) t.join();
        history.collect();
        assert(history.retiredCount() == 0);
        auto latest = history.lookup(50000, 0);
        assert(latest && latest->value == 500000);
        latest.reset();
        std::cout << "concurrent lookup: PASS; hits=" << hits
                  << ", misses=" << misses << '\n';
    }
}
