// Linux / C++20 / Boost 1.83+ demonstration.
// Compile: g++ -std=c++20 -O2 -Wall -Wextra -pedantic l3_orderbook_hugepage.cpp -o l3_orderbook
// Run without configured HugeTLB: ./l3_orderbook --normal
// Run with preconfigured 2 MiB HugeTLB: ./l3_orderbook
#include <boost/intrusive/list.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <cerrno>
#include <sys/mman.h>
#include <linux/mman.h>

namespace bi = boost::intrusive;
constexpr std::size_t HugeSize = 2U * 1024U * 1024U;

class MmapRegion {
    void* data_ = nullptr;
    std::size_t bytes_ = 0;
    bool huge_ = false;

public:
    MmapRegion(std::size_t bytes, bool use_huge) : bytes_(bytes), huge_(use_huge) {
        if (bytes == 0 || (use_huge && bytes % HugeSize != 0))
            throw std::invalid_argument("region must be nonempty and HugeTLB size must be 2 MiB multiple");
        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
        if (use_huge) flags |= MAP_HUGETLB | MAP_HUGE_2MB;
        data_ = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
        if (data_ == MAP_FAILED) {
            data_ = nullptr;
            throw std::runtime_error(use_huge
                ? "MAP_HUGETLB failed; configure nr_hugepages or use --normal (explicit test mode)"
                : "normal mmap failed");
        }
        // Prefault/touch all pages during setup, before timing the hot path.
        std::memset(data_, 0, bytes_);
    }

    MmapRegion(const MmapRegion&) = delete;
    MmapRegion& operator=(const MmapRegion&) = delete;
    ~MmapRegion() { if (data_) ::munmap(data_, bytes_); }

    void* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return bytes_; }
    bool huge() const noexcept { return huge_; }
    bool contains(const void* ptr) const noexcept {
        auto p = reinterpret_cast<std::uintptr_t>(ptr);
        auto b = reinterpret_cast<std::uintptr_t>(data_);
        return p >= b && p - b < bytes_; // Linux flat-address implementation
    }
};

// A bounded arena with an exact-size reusable free list for container reallocations.
// Single-threaded; not a general-purpose production allocator (no splitting/coalescing).
class BookMemory {
    struct FreeBlock {
        FreeBlock* next;
        std::size_t bytes;
        std::size_t alignment;
    };

    MmapRegion& region_;
    std::size_t bump_ = 0;
    FreeBlock* free_ = nullptr;
    std::size_t reused_ = 0;

    static std::size_t normalized_size(std::size_t bytes) noexcept {
        return std::max(bytes, sizeof(FreeBlock));
    }
    static std::size_t normalized_alignment(std::size_t align) noexcept {
        return std::max(align, alignof(FreeBlock));
    }

public:
    explicit BookMemory(MmapRegion& region) noexcept : region_(region) {}
    BookMemory(const BookMemory&) = delete;
    BookMemory& operator=(const BookMemory&) = delete;

    void* allocate(std::size_t bytes, std::size_t alignment) {
        const auto n = normalized_size(bytes);
        const auto a = normalized_alignment(alignment);
        // alignof(T) and alignof(FreeBlock) are valid power-of-two alignments.
        FreeBlock** link = &free_;
        while (*link) {
            FreeBlock* p = *link;
            if (p->bytes == n && p->alignment == a) {
                *link = p->next;
                std::destroy_at(p);
                ++reused_;
                return p;
            }
            link = &p->next;
        }
        std::byte* origin = static_cast<std::byte*>(region_.data());
        void* candidate = origin + bump_;
        std::size_t remaining = region_.size() - bump_;
        void* aligned = std::align(a, n, candidate, remaining);
        if (!aligned) throw std::bad_alloc{};
        bump_ = static_cast<std::size_t>(static_cast<std::byte*>(aligned) - origin) + n;
        return aligned;
    }

    void deallocate(void* ptr, std::size_t bytes, std::size_t alignment) noexcept {
        if (!ptr) return;
        assert(region_.contains(ptr));
        // Caller destroyed its objects; storage is now reusable as FreeBlock.
        free_ = ::new (ptr) FreeBlock{free_, normalized_size(bytes), normalized_alignment(alignment)};
    }

    std::size_t committed_from_region() const noexcept { return bump_; }
    std::size_t reused_blocks() const noexcept { return reused_; }
};

template<class T>
class BookAllocator {
    template<class> friend class BookAllocator;
    BookMemory* mem_;
public:
    using value_type = T;
    using is_always_equal = std::false_type;
    using propagate_on_container_copy_assignment = std::false_type;
    using propagate_on_container_move_assignment = std::false_type;
    using propagate_on_container_swap = std::false_type;

    explicit BookAllocator(BookMemory& mem) noexcept : mem_(&mem) {}
    template<class U>
    BookAllocator(const BookAllocator<U>& other) noexcept : mem_(other.mem_) {}

    [[nodiscard]] T* allocate(std::size_t n) {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::bad_array_new_length{};
        return static_cast<T*>(mem_->allocate(n * sizeof(T), alignof(T)));
    }
    void deallocate(T* p, std::size_t n) noexcept {
        mem_->deallocate(p, n * sizeof(T), alignof(T));
    }
    template<class U>
    bool operator==(const BookAllocator<U>& other) const noexcept {
        return mem_ == other.mem_;
    }
};

// Metadata and in-place object storage remain inside a hugepage-backed vector.
// The vector is resized only once; objects are individually placement-constructed.
template<class T>
class FixedPool {
    struct Slot {
        alignas(T) std::byte storage[sizeof(T)];
        T* live = nullptr;
        std::size_t next = 0;
    };
    using SlotAlloc = BookAllocator<Slot>;
    std::vector<Slot, SlotAlloc> slots_;
    std::size_t head_;
    std::size_t count_ = 0;

public:
    explicit FixedPool(BookMemory& mem, std::size_t capacity)
        : slots_(SlotAlloc{mem}), head_(0) {
        if (capacity == 0) throw std::invalid_argument("pool capacity must be positive");
        slots_.resize(capacity);
        for (std::size_t i = 0; i < capacity; ++i)
            slots_[i].next = i + 1;
    }
    FixedPool(const FixedPool&) = delete;
    FixedPool& operator=(const FixedPool&) = delete;
    ~FixedPool() { assert(count_ == 0); }

    template<class... Args>
    T* create(Args&&... args) {
        if (head_ == slots_.size()) throw std::bad_alloc{};
        const auto i = head_;
        Slot& s = slots_[i];
        assert(!s.live);
        // Strong exception safety: only consume slot after successful construction.
        T* obj = ::new (static_cast<void*>(s.storage)) T(i, std::forward<Args>(args)...);
        head_ = s.next;
        s.live = obj;
        ++count_;
        return obj;
    }

    void destroy(T* obj) noexcept {
        assert(obj);
        const auto i = obj->slotIndex;
        assert(i < slots_.size() && slots_[i].live == obj);
        std::destroy_at(obj);
        Slot& s = slots_[i];
        s.live = nullptr;
        s.next = head_;
        head_ = i;
        --count_;
    }
    std::size_t size() const noexcept { return count_; }
    std::size_t capacity() const noexcept { return slots_.size(); }
};

using OrderId = std::uint64_t;
using Price = std::int64_t; // integer tick price, not double
using Qty = std::uint64_t;
enum class Side : std::uint8_t { Bid, Ask };

using OrderHook = bi::list_member_hook<bi::link_mode<bi::safe_link>>;
struct PriceLevel;

struct Order {
    std::size_t slotIndex;
    OrderId id;
    Price price;
    Qty qty;
    Side side;
    PriceLevel* level;
    OrderHook hook;

    Order(std::size_t slot, OrderId i, Price p, Qty q, Side s, PriceLevel* l) noexcept
        : slotIndex(slot), id(i), price(p), qty(q), side(s), level(l) {}
    Order(const Order&) = delete;
    Order& operator=(const Order&) = delete;
};

using FIFO = bi::list<Order,
    bi::member_hook<Order, OrderHook, &Order::hook>,
    bi::constant_time_size<false>>;

struct PriceLevel {
    std::size_t slotIndex;
    Side side;
    Price price;
    Qty total = 0;
    FIFO orders;

    PriceLevel(std::size_t idx, Side s, Price p) noexcept
        : slotIndex(idx), side(s), price(p) {}
};

using OrderKV = std::pair<const OrderId, Order*>;
using LevelKV = std::pair<const Price, PriceLevel*>;
using OrderMap = boost::unordered_flat_map<OrderId, Order*,
    boost::hash<OrderId>, std::equal_to<OrderId>, BookAllocator<OrderKV>>;
using LevelMap = boost::unordered_flat_map<Price, PriceLevel*,
    boost::hash<Price>, std::equal_to<Price>, BookAllocator<LevelKV>>;
using LevelVector = std::vector<PriceLevel*, BookAllocator<PriceLevel*>>;

class OrderBook {
    // Declaration order is important: storage outlives all containers using it.
    MmapRegion region_;
    BookMemory mem_;
    FixedPool<Order> orderPool_;
    FixedPool<PriceLevel> levelPool_;
    OrderMap byId_;
    LevelMap bidsByPrice_;
    LevelMap asksByPrice_;
    LevelVector bidLevels_; // sorted descending
    LevelVector askLevels_; // sorted ascending

    LevelMap& price_map(Side side) noexcept {
        return side == Side::Bid ? bidsByPrice_ : asksByPrice_;
    }
    LevelVector& levels(Side side) noexcept {
        return side == Side::Bid ? bidLevels_ : askLevels_;
    }
    const LevelVector& levels(Side side) const noexcept {
        return side == Side::Bid ? bidLevels_ : askLevels_;
    }

    PriceLevel* find_level(Side side, Price p) noexcept {
        auto& map = price_map(side);
        auto it = map.find(p);
        return it == map.end() ? nullptr : it->second;
    }

    // Strong exception guarantee (map insertion or vector insertion may fail).
    PriceLevel* make_level(Side side, Price price) {
        PriceLevel* l = levelPool_.create(side, price);
        auto& m = price_map(side);
        auto& v = levels(side);
        try {
            m.emplace(price, l);
            try {
                auto pos = std::lower_bound(v.begin(), v.end(), price,
                    [side](const PriceLevel* level, Price p) {
                        return side == Side::Bid ? level->price > p : level->price < p;
                    });
                v.insert(pos, l);
            } catch (...) {
                m.erase(price);
                throw;
            }
        } catch (...) {
            levelPool_.destroy(l);
            throw;
        }
        return l;
    }

    void erase_empty_level(PriceLevel* l) noexcept {
        assert(l && l->orders.empty() && l->total == 0);
        auto& v = levels(l->side);
        auto it = std::find(v.begin(), v.end(), l);
        assert(it != v.end());
        v.erase(it); // PriceLevel* move is noexcept, pre-reserved storage remains
        auto count = price_map(l->side).erase(l->price);
        assert(count == 1);
        (void)count;
        levelPool_.destroy(l);
    }

public:
    OrderBook(std::size_t regionBytes, bool huge,
              std::size_t maxOrders, std::size_t maxLevels)
        : region_(regionBytes, huge), mem_(region_),
          orderPool_(mem_, maxOrders), levelPool_(mem_, maxLevels),
          byId_(BookAllocator<OrderKV>{mem_}),
          bidsByPrice_(BookAllocator<LevelKV>{mem_}),
          asksByPrice_(BookAllocator<LevelKV>{mem_}),
          bidLevels_(BookAllocator<PriceLevel*>{mem_}),
          askLevels_(BookAllocator<PriceLevel*>{mem_}) {
        byId_.reserve(maxOrders);
        bidsByPrice_.reserve(maxLevels);
        asksByPrice_.reserve(maxLevels);
        bidLevels_.reserve(maxLevels);
        askLevels_.reserve(maxLevels);
    }
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    ~OrderBook() noexcept { clear(); }

    const Order* find(OrderId id) const noexcept {
        auto it = byId_.find(id);
        return it == byId_.end() ? nullptr : it->second;
    }

    bool add(OrderId id, Side side, Price price, Qty qty) {
        if (qty == 0 || byId_.contains(id)) return false;
        PriceLevel* level = find_level(side, price);
        const bool created = (level == nullptr);
        if (created) level = make_level(side, price);
        try {
            if (qty > std::numeric_limits<Qty>::max() - level->total)
                throw std::overflow_error("level quantity overflow");
            Order* o = orderPool_.create(id, price, qty, side, level);
            try {
                auto [it, inserted] = byId_.emplace(id, o);
                (void)it;
                assert(inserted);
            } catch (...) {
                orderPool_.destroy(o);
                throw;
            }
            level->orders.push_back(*o);
            level->total += qty;
        } catch (...) {
            if (created) erase_empty_level(level);
            throw;
        }
        return true;
    }

    bool cancel(OrderId id) noexcept {
        auto it = byId_.find(id);
        if (it == byId_.end()) return false;
        Order* o = it->second;
        PriceLevel* level = o->level;
        level->orders.erase(level->orders.iterator_to(*o));
        level->total -= o->qty;
        byId_.erase(it);
        orderPool_.destroy(o);
        if (level->orders.empty()) erase_empty_level(level);
        return true;
    }

    // In-place qty update at same price retains priority. Price change moves order to tail.
    bool modify(OrderId id, Price newPrice, Qty newQty) {
        if (newQty == 0) return cancel(id);
        auto it = byId_.find(id);
        if (it == byId_.end()) return false;
        Order* o = it->second;
        PriceLevel* old = o->level;
        if (newPrice == o->price) {
            if (newQty > o->qty && newQty - o->qty >
                                     std::numeric_limits<Qty>::max() - old->total)
                throw std::overflow_error("level quantity overflow");
            old->total = old->total - o->qty + newQty;
            o->qty = newQty;
            return true;
        }
        PriceLevel* dest = find_level(o->side, newPrice);
        const bool created = dest == nullptr;
        if (created) dest = make_level(o->side, newPrice);
        if (newQty > std::numeric_limits<Qty>::max() - dest->total) {
            if (created) erase_empty_level(dest);
            throw std::overflow_error("level quantity overflow");
        }
        old->orders.erase(old->orders.iterator_to(*o));
        old->total -= o->qty;
        o->price = newPrice;
        o->qty = newQty;
        o->level = dest;
        dest->orders.push_back(*o);
        dest->total += newQty;
        if (old->orders.empty()) erase_empty_level(old);
        return true;
    }

    void clear() noexcept {
        while (!byId_.empty()) {
            const OrderId id = byId_.begin()->first;
            const bool ok = cancel(id);
            assert(ok);
            (void)ok;
        }
        assert(levelPool_.size() == 0 && orderPool_.size() == 0);
    }

    const PriceLevel* best_bid() const noexcept {
        return bidLevels_.empty() ? nullptr : bidLevels_.front();
    }
    const PriceLevel* best_ask() const noexcept {
        return askLevels_.empty() ? nullptr : askLevels_.front();
    }
    void print() const {
        for (Side side : {Side::Ask, Side::Bid}) {
            std::cout << (side == Side::Bid ? "BIDS" : "ASKS") << ":\n";
            for (const PriceLevel* l : levels(side)) {
                std::cout << "  price=" << l->price << " qty=" << l->total << " FIFO=";
                for (const Order& o : l->orders) std::cout << o.id << ":" << o.qty << " ";
                std::cout << "\n";
            }
        }
    }
    bool in_region(const void* p) const noexcept { return region_.contains(p); }
    std::size_t memory_bump_bytes() const noexcept { return mem_.committed_from_region(); }
    std::size_t reused_allocations() const noexcept { return mem_.reused_blocks(); }
    bool using_huge() const noexcept { return region_.huge(); }
    std::size_t order_count() const noexcept { return orderPool_.size(); }
};

static void require(bool condition, const char* msg) {
    if (!condition) throw std::logic_error(msg);
}

int main(int argc, char** argv) {
    try {
        bool huge = true;
        if (argc == 2 && std::string_view(argv[1]) == "--normal") huge = false;
        else if (argc != 1) throw std::invalid_argument("usage: l3_orderbook [--normal]");

        OrderBook book(16 * HugeSize, huge, 10000, 256);
        require(book.add(101, Side::Bid, 10000, 10), "check failed: book.add(101, Side::Bid, 10000, 10)");
        require(book.add(102, Side::Bid, 10000, 20), "check failed: book.add(102, Side::Bid, 10000, 20)");
        require(book.add(103, Side::Bid,  9999, 15), "check failed: book.add(103, Side::Bid,  9999, 15)");
        require(book.add(201, Side::Ask, 10002, 8), "check failed: book.add(201, Side::Ask, 10002, 8)");
        const Order* keep = book.find(101);
        require(keep && book.in_region(keep), "check failed: keep && book.in_region(keep)");
        require(keep == book.find(101), "check failed: keep == book.find(101)");
        require(book.modify(102, 10001, 12), "check failed: book.modify(102, 10001, 12)");
        require(book.cancel(103), "check failed: book.cancel(103)");
        require(book.add(104, Side::Bid, 10000, 30), "check failed: book.add(104, Side::Bid, 10000, 30)");
        require(!book.add(104, Side::Bid, 10000, 20), "check failed: !book.add(104, Side::Bid, 10000, 20)");
        require(book.find(101) == keep, "check failed: book.find(101) == keep"); // stable address after index mutations
        require(book.best_bid() && book.best_bid()->price == 10001, "check failed: book.best_bid() && book.best_bid()->price == 10001");
        require(book.best_ask() && book.best_ask()->price == 10002, "check failed: book.best_ask() && book.best_ask()->price == 10002");
        book.print();
        std::cout << "mode=" << (book.using_huge() ? "explicit HugeTLB" : "normal mmap")
                  << " orders=" << book.order_count()
                  << " allocated_high_water=" << book.memory_bump_bytes()
                  << " reused_blocks=" << book.reused_allocations() << '\n';
        book.clear();
        require(book.order_count() == 0, "check failed: book.order_count() == 0");
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
    return 0;
}