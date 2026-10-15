# Binary Market Data Wire Layout: `pack(1)`, `alignas`, Big-Endian Fields and Safe Decoding

> Target: HFT / market data / order-entry binary protocols, C++20/23, x86-64 Linux.  
> Main recommendation: **`BigEndian<T>` + wire struct + length validation + `memcpy` + on-demand field access**.

## 1. Executive summary

- `#pragma pack(push, 1)` is a **non-standard, compiler-supported layout directive**. It caps member alignment and commonly removes inter-field/trailing padding. Restore with `#pragma pack(pop)`.
- `alignas(1)` is **not packing**: standard C++ cannot use `alignas` to weaken a type's required alignment. `alignas(64)` can increase alignment, useful for cache lines.
- A packed struct is useful as a **short-lived wire representation** when its offsets exactly match a binary protocol. It does **not** handle endian, field validity, bounds, versioning or object lifetime automatically.
- `reinterpret_cast<const WireMessage*>(network_buffer)` alone **does not create a WireMessage object**. C++20 implicit-lifetime rules are nuanced; do not treat an arbitrary socket buffer cast as universally portable.
- The simplest safe design is: validate length, construct a real trivially-copyable wire object, `std::memcpy` bytes into it, decode only fields needed, then update naturally aligned internal state.
- On little-endian x86-64, big-endian multi-byte integer fields need conversion. `std::byteswap` (C++23) or GCC/Clang `__builtin_bswap{16,32,64}` can help.
- A byte-array-backed `BigEndian<T>` encapsulates endian conversion at the field boundary; **alignment = 1 without relying on `#pragma pack`**.

## 2. `#pragma pack(1)` vs `alignas(1)`

```cpp
#include <cstddef>
#include <cstdint>

#pragma pack(push, 1)
struct Packed {
    std::uint8_t a;   // offset 0
    std::uint32_t b;  // offset 1
    std::uint64_t c;  // offset 5
};
#pragma pack(pop)

struct alignas(1) NotPacked {
    std::uint8_t a;
    std::uint32_t b;
    std::uint64_t c;
};
```

On a typical x86-64 ABI:

| Property | Packed | NotPacked |
| --- | ---: | ---: |
| `offsetof(b)` | 1 | 4 |
| `offsetof(c)` | 5 | 8 |
| `sizeof` | 13 | 16 |
| `alignof` | 1 | 8 |

`alignas(1)` cannot lower the required natural alignment of `NotPacked`. Standard C++ does not specify `#pragma pack` behavior; verify compiler/ABI and build flags. Packing an outer struct does **not** recursively rewrite the internal layout of nested subobjects: check every wire type's offsets.

Good practice:

```cpp
static_assert(sizeof(Packed) == 13);
static_assert(alignof(Packed) == 1);
static_assert(offsetof(Packed, b) == 1);
static_assert(offsetof(Packed, c) == 5);
```

## 3. What packing solves — and does not solve

| Concern | `pack(1)` alone |
| --- | --- |
| Member offsets / padding | Often yes; test with `offsetof` and `sizeof` |
| Required alignment of packed wire type | Often lowered to 1 |
| CPU byte order | **No** |
| TCP framing / partial `recv` | **No** |
| UDP truncation / packet validation | **No** |
| Network-buffer object lifetime | **No** |
| Strict aliasing / legal typed access | **No** |
| Invalid field representation | **No** |
| Schema compatibility / versioning | **No** |

Use fixed-width integer fields and raw byte arrays for protocol strings, flags and unusual widths. Avoid raw pointers, STL containers, virtual functions, native `bool`, and arbitrary enums in wire layouts.

### Packed member access

```cpp
#pragma pack(push, 1)
struct Wire {
    std::uint8_t kind;
    std::uint64_t id;  // offset 1, potentially unaligned
};
#pragma pack(pop)

Wire wire{};
auto id = wire.id;           // compiler knows this is a packed member
// auto* p = &wire.id;       // avoid: may yield misaligned uint64_t*
```

On x86-64, unaligned scalar access is normally supported; crossing cache-line or page boundaries can be more expensive. Do not leak references or pointers to under-aligned packed integer members.

## 4. Basic approach: packed wire object + memcpy + endian conversion

Assume a **hypothetical** 17-byte message. This is not an actual ITCH/CME schema.

```cpp
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>

#pragma pack(push, 1)
struct WireAddOrder {
    std::uint8_t type;           // 0
    std::uint64_t order_id_be;   // 1..8
    std::uint32_t price_be;      // 9..12 (signed price bit pattern)
    std::uint32_t qty_be;        // 13..16
};
#pragma pack(pop)

static_assert(sizeof(WireAddOrder) == 17);
static_assert(alignof(WireAddOrder) == 1);
static_assert(offsetof(WireAddOrder, order_id_be) == 1);
static_assert(std::is_trivially_copyable_v<WireAddOrder>);

struct AddOrder {
    std::uint64_t order_id;
    std::int32_t price;
    std::uint32_t qty;
};

std::optional<AddOrder> decode(const std::byte* buf, std::size_t len) {
    if (!buf || len < sizeof(WireAddOrder))
        return std::nullopt;

    WireAddOrder wire{};  // actual C++ object exists
    std::memcpy(&wire, buf, sizeof(wire));

    const auto from_be32 = [](std::uint32_t v) {
        if constexpr (std::endian::native == std::endian::little)
            return std::byteswap(v); // C++23
        else
            return v;                // assume little/big native endian
    };
    const auto from_be64 = [](std::uint64_t v) {
        if constexpr (std::endian::native == std::endian::little)
            return std::byteswap(v);
        else
            return v;
    };

    return AddOrder{
        from_be64(wire.order_id_be),
        std::bit_cast<std::int32_t>(from_be32(wire.price_be)),
        from_be32(wire.qty_be)
    };
}
```

The unsigned field representing signed price is intentionally bit-cast **after** byte-order correction. An encoding with scaling, decimal price, or sentinel values needs additional protocol-specific validation.

### Why memcpy instead of casting a socket buffer?

`WireAddOrder wire;` has an established object lifetime, correct type and alignment. `std::memcpy` copies its representation. By contrast, `reinterpret_cast<const WireAddOrder*>(buf)` is only a pointer conversion; it does not by itself establish an object at `buf`. C++20 has implicit-object-creation rules for certain operations, but do not generalize them to every `recv` / vector-of-bytes / byte-span scenario.

For trivially copyable, fixed-width integer-only wire types, memcpy is a practical conservative boundary. Always verify length **before** copying. For TCP, accumulate a complete framed message first; for UDP, validate actual datagram length.

## 5. Preferred design: field-level `BigEndian<T>`

Store raw bytes (alignment 1) rather than native integer subobjects. Convert lazily when the field is read.

```cpp
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

template<class T>
struct BigEndian {
    static_assert(std::is_integral_v<T>);
    static_assert(!std::is_same_v<T, bool>);
    static_assert(sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);

    using U = std::make_unsigned_t<T>;
    std::array<std::byte, sizeof(T)> raw{};

    [[nodiscard]] T value() const noexcept {
        U u;
        std::memcpy(&u, raw.data(), sizeof(u));

        if constexpr (std::endian::native == std::endian::little) {
            u = std::byteswap(u);  // C++23
        } else {
            static_assert(std::endian::native == std::endian::big,
                          "Mixed-endian platforms are unsupported");
        }

        if constexpr (std::is_signed_v<T>)
            return std::bit_cast<T>(u);
        else
            return u;
    }

    [[nodiscard]] operator T() const noexcept { return value(); }
};

static_assert(alignof(BigEndian<std::uint64_t>) == 1);
static_assert(sizeof(BigEndian<std::uint64_t>) == 8);
static_assert(std::is_trivially_copyable_v<BigEndian<std::uint64_t>>);
```

Notes:
- Requires C++23 for `std::byteswap`. For C++20, use a small helper dispatching to `__builtin_bswap16/32/64` (GCC/Clang), or implement byte shifts.
- Targets conventional little-/big-endian platforms with 8-bit bytes and standard fixed-width integer types.
- For signed integers, `std::bit_cast` maps the corrected bit pattern into the signed type (two's-complement on the stated target).
- The object stores bytes, so it needs no manual `pack(1)` for its own alignment.

### Full wire struct

```cpp
struct WireAddOrder2 {
    std::uint8_t type;
    BigEndian<std::uint64_t> order_id;
    BigEndian<std::int32_t> price;
    BigEndian<std::uint32_t> qty;
};

static_assert(sizeof(WireAddOrder2) == 17);
static_assert(alignof(WireAddOrder2) == 1);
static_assert(offsetof(WireAddOrder2, order_id) == 1);
static_assert(offsetof(WireAddOrder2, price) == 9);
static_assert(offsetof(WireAddOrder2, qty) == 13);
static_assert(std::is_trivially_copyable_v<WireAddOrder2>);

std::optional<AddOrder> decode2(const std::byte* buf, std::size_t len) {
    if (!buf || len < sizeof(WireAddOrder2))
        return std::nullopt;

    WireAddOrder2 wire{};
    std::memcpy(&wire, buf, sizeof(wire));

    // Decode only fields actually needed.
    return AddOrder{
        wire.order_id.value(),
        wire.price.value(),
        wire.qty.value()
    };
}
```

This design is **packing-free**, because every field has alignment 1, while preserving schema offsets. A packed directive may still be appropriate for more complex legacy schemas, but it is unnecessary in this example.

You can also skip constructing `AddOrder` and pass the decoded fields directly to the orderbook if the operation immediately consumes the message.

### Optional improvement: 24-bit or 48-bit fields

Protocols such as ITCH can include six-byte timestamps. Do **not** use `BigEndian<uint64_t>` directly in a six-byte wire slot. Provide a separate `BigEndianBytes<N>` decoder that shifts bytes into `uint64_t`, or a dedicated `be48` wrapper, and assert the exact wire size.

## 6. Can `memcpy` disappear in optimized machine code?

Yes, **possibly**, not guaranteed. With LTO/inlining and fixed-size copies, GCC/Clang may perform scalar replacement, dead-field elimination, and direct loads from original message offsets.

For a requested 64-bit big-endian value on little-endian x86-64, plausible optimized code:

```asm
mov   rax, [rdi + 1]  ; unaligned 8-byte load
bswap rax             ; big -> little
```

This is illustrative assembly, **not a promise**. Inspect target compiler output. C++23 `std::byteswap` often maps to `bswap` for 32/64-bit values (16-bit may use `ror` or another sequence).

## 7. Performance trade-offs

| Technique | Advantages | Costs / cautions |
| --- | --- | --- |
| Packed native integer fields + `memcpy` + explicit swap | Simple, exact offsets, little code | Easy to forget swap; packed native members unaligned |
| `BigEndian<T>` byte-backed fields + `memcpy` | Endianness typed, alignment 1, lazy conversion | Template utility and tests; repeated access may repeat conversion |
| Offset decoder + per-field `memcpy` | No packed layout dependency, decode only needed fields | Explicit offsets/schema maintenance |
| Host-native aligned internal struct | Repeated reads and updates efficient | Requires decode/transform step |
| Direct reinterpret-cast view | No visible copy | Lifetime/aliasing/validity proof is harder; not the default recommendation |

**Single-message latency vs throughput**: copying a small wire message may be optimized away. A large message where only two fields are used may favor offset-based reads. Long-lived orderbook state should usually be naturally aligned.

**Unaligned access**: a load within one cache line is often cheap on x86-64; crossing cache lines/pages may cost considerably more. SIMD vectorization and hardware differences also matter.

**Endian conversion**: apply once per use or normalize once if the field is frequently reread. Do not keep converting it on every orderbook update.

## 8. Correctness checklist for production

1. Follow the **official binary protocol spec**: field offsets, widths, endian, scaling, sentinels, optional blocks and version.
2. Frame TCP messages before decoding; validate UDP packet truncation and message count/length.
3. Check `len >= sizeof(Wire)` before each fixed-layout copy, and validate variable-length tails separately.
4. Use `static_assert(sizeof, alignof, offsetof)` for each wire type and nested layout.
5. Keep wire types trivially copyable and free of invalid native representations (especially native `bool`, pointers, unvalidated enums).
6. Prefer fixed-width integers and byte arrays; convert to domain types only after validation.
7. Treat endian conversion as part of the **field interface**, not an implicit side effect of `pack`.
8. Avoid references/pointers to unaligned packed members.
9. Keep wire layouts separated from naturally aligned **internal** state structures.
10. Fuzz truncated, malformed and boundary messages; test known hexadecimal fixtures against expected values.
11. Inspect optimized assembly and measure p50/p99/p99.9 in the target runtime; do not assume `memcpy` materializes.
12. For protocol generation tools (e.g. SBE), prefer tested schema-generated decoders when appropriate.

## 9. HFT interview takeaway

> `#pragma pack(1)` controls wire layout, while `alignas(1)` cannot reduce natural alignment. For a fixed binary market data protocol, I can define a trivially-copyable wire struct, validate the complete message, and memcpy into a real object. For big-endian fields, I prefer a byte-backed `BigEndian<T>` wrapper that converts on demand. This gives alignment-safe field storage, avoids raw-buffer object-lifetime assumptions, keeps endianness explicit in the type system, and usually compiles to efficient loads and byteswaps. I keep the long-lived internal orderbook naturally aligned and verify generated assembly and latency measurements.

## 10. Useful references

- C++ alignment specification: https://en.cppreference.com/w/cpp/language/alignas
- C++ `std::endian`: https://en.cppreference.com/w/cpp/types/endian
- C++23 `std::byteswap`: https://en.cppreference.com/w/cpp/numeric/byteswap
- C++ object lifetime: https://en.cppreference.com/w/cpp/language/lifetime
- GCC structure packing attributes: https://gcc.gnu.org/onlinedocs/gcc/Common-Type-Attributes.html
