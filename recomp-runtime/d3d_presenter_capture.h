#ifndef DOAXBV_RECOMP_D3D_PRESENTER_CAPTURE_H
#define DOAXBV_RECOMP_D3D_PRESENTER_CAPTURE_H

#include "d3d_presenter.h"

#include <cstddef>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

class D3dCapturePacket {
public:
    enum Kind : uint32_t { COMMAND, RELEASE, REPORT, INVALIDATE };
    struct Record { Kind kind; uint32_t base; uint32_t size; };

    D3dCapturePacket() = default;
    D3dCapturePacket(const D3dCapturePacket &) = delete;
    D3dCapturePacket &operator=(const D3dCapturePacket &) = delete;
    // The move empties spans_ and the resident arena, so their counts reset with them.
    D3dCapturePacket(D3dCapturePacket &&other) noexcept
        : entries_(std::move(other.entries_)), commands_(std::move(other.commands_)),
          payload_(std::move(other.payload_)), spans_(std::move(other.spans_)),
          span_count_(std::exchange(other.span_count_, 0u)),
          resident_(std::move(other.resident_)),
          resident_bytes_(std::move(other.resident_bytes_)),
          resident_index_(std::move(other.resident_index_)),
          resident_used_(std::exchange(other.resident_used_, 0u)),
          resident_live_(std::exchange(other.resident_live_, 0u)),
          resident_added_(std::exchange(other.resident_added_, 0u)), sealed_(other.sealed_) {}
    D3dCapturePacket &operator=(D3dCapturePacket &&other) noexcept
    {
        entries_ = std::move(other.entries_);
        commands_ = std::move(other.commands_);
        payload_ = std::move(other.payload_);
        spans_ = std::move(other.spans_);
        span_count_ = std::exchange(other.span_count_, 0u);
        resident_ = std::move(other.resident_);
        resident_bytes_ = std::move(other.resident_bytes_);
        resident_index_ = std::move(other.resident_index_);
        resident_used_ = std::exchange(other.resident_used_, 0u);
        resident_live_ = std::exchange(other.resident_live_, 0u);
        resident_added_ = std::exchange(other.resident_added_, 0u);
        sealed_ = other.sealed_;
        return *this;
    }

    RecompD3dPresenterError add(const RecompD3dPresenterCommand &command);
    RecompD3dPresenterError addRelease(uint32_t base, uint32_t size);
    RecompD3dPresenterError addInvalidate(uint32_t base, uint32_t size);
    /* Drops standing bytes for [base, base + size). The presenter thread calls
       this on every packet, not just the open one: each packet keeps its own
       standing copies and the worker may still read two of them. Only the owner
       thread calls it, and a drop never moves or rewrites queued bytes. */
    void dropResident(uint32_t base, uint32_t size);
    RecompD3dPresenterError addReport();
    void seal();
    size_t count() const;
    const Record &record(size_t index) const;
    const RecompD3dPresenterCommand &command(size_t index) const;
    void clear();
    // Copied commands, records, this fill's payload and newly standing bytes.
    uint64_t bytes() const;

private:
    // Default-initializes instead of zeroing; Debug builds rebind to proxy types.
    template <typename T>
    struct NoInitAllocator : std::allocator<T> {
        using std::allocator<T>::allocator;
        template <typename U>
        struct rebind { using other = NoInitAllocator<U>; };

        template <typename U>
        void construct(U *pointer)
        {
            ::new (static_cast<void *>(pointer)) U;
        }

        template <typename U, typename... Args>
        void construct(U *pointer, Args &&...args)
        {
            ::new (static_cast<void *>(pointer)) U(std::forward<Args>(args)...);
        }
    };

    struct Entry { Record record; size_t command_index; };
    struct CapturedCommand {
        CapturedCommand() {} // Skip zeroing: add() writes every field it uses.
        alignas(8) RecompD3dPresenterCommand value;
        size_t offsets[8];
        // Which offsets name resident bytes rather than this fill's payload.
        uint8_t resident_mask;
    };
    // offset == SIZE_MAX marks an empty slot.
    struct Span { uint64_t key; size_t size; size_t offset; };
    /* Guest texture bytes outlive one present: a write lock, Register or free
       drops them, the events that already invalidate the presenter's textures.
       A raw store that misses every sampled word is the remaining gamble, as
       with the presenter's own fingerprint. Linear surfaces are rewritten in
       place every frame and always go through the per-fill payload. */
    // A dead entry is kept until the next clear so live indices stay stable.
    struct Resident { uint64_t key; uint32_t address; uint32_t size; size_t offset; bool live; };
    // Per packet, so three fills hold at most this much standing texture data.
    static constexpr size_t kResidentByteLimit = 32u * 1024u * 1024u;
    // Bounds the map and the arena; past it a span goes to the per-fill payload.
    static constexpr size_t kResidentSpanLimit = 4096u;

    RecompD3dPresenterError addRecord(Kind kind, uint32_t base, uint32_t size);
    void insertSpan(uint64_t key, size_t size, size_t offset);
    void clearSpans();
    uint64_t residentKey(uint32_t address, const void *bytes, size_t size) const;
    size_t residentReuse(uint64_t key, uint32_t address, const void *bytes, size_t size);
    void clearResident();

    std::vector<Entry> entries_;
    std::vector<CapturedCommand> commands_;
    // Word storage keeps indices and vertex data aligned after relocation.
    // resize() appends uninitialized words; add() overwrites the used bytes.
    std::vector<uint64_t, NoInitAllocator<uint64_t>> payload_;
    // Open-addressing table keyed by source, size and sampled bytes; memcmp still
    // confirms each reuse. Slots persist across clear(), so a frame allocates nothing.
    std::vector<Span> spans_;
    size_t span_count_ = 0u;
    // Texture bytes that survive clear(); offsets are words into resident_bytes_.
    std::vector<Resident> resident_;
    std::vector<uint64_t, NoInitAllocator<uint64_t>> resident_bytes_;
    std::unordered_map<uint64_t, size_t> resident_index_;
    size_t resident_used_ = 0u;
    size_t resident_live_ = 0u;
    size_t resident_added_ = 0u;
    bool sealed_ = false;
};

#endif
