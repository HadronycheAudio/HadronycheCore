#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Hadronyche {
namespace Lv2 {

// Whole messages between one writing thread and one reading thread, without locks or allocation after construction,
// so either end can be the audio thread. Each record is its size and then its bytes.
class ByteQueue
{
public:
    explicit ByteQueue (size_t capacity) : _buffer (capacity) {}

    // False, leaving the queue as it was, if the record doesn't fit.
    bool push (const void* data, uint32_t size)
    {
        const size_t write = _write.load (std::memory_order_relaxed);
        const size_t read = _read.load (std::memory_order_acquire);
        if (_buffer.size () - (write - read) < sizeof (size) + size)
            return false;

        copyIn (write, &size, sizeof (size));
        copyIn (write + sizeof (size), data, size);
        _write.store (write + sizeof (size) + size, std::memory_order_release);
        return true;
    }

    // The next record's size, or false if there's none.
    bool peekSize (uint32_t& size) const
    {
        const size_t read = _read.load (std::memory_order_relaxed);
        if (_write.load (std::memory_order_acquire) == read)
            return false;
        copyOut (read, &size, sizeof (size));
        return true;
    }

    // Copies the next record, which must be no larger than its size from peekSize, and removes it.
    void pop (void* data)
    {
        const size_t read = _read.load (std::memory_order_relaxed);
        uint32_t size = 0;
        copyOut (read, &size, sizeof (size));
        copyOut (read + sizeof (size), data, size);
        _read.store (read + sizeof (size) + size, std::memory_order_release);
    }

    void skip ()
    {
        const size_t read = _read.load (std::memory_order_relaxed);
        uint32_t size = 0;
        copyOut (read, &size, sizeof (size));
        _read.store (read + sizeof (size) + size, std::memory_order_release);
    }

private:
    void copyIn (size_t position, const void* data, size_t size)
    {
        const size_t start = position % _buffer.size ();
        const size_t first = std::min (size, _buffer.size () - start);
        std::memcpy (_buffer.data () + start, data, first);
        std::memcpy (_buffer.data (), static_cast<const uint8_t*> (data) + first, size - first);
    }

    void copyOut (size_t position, void* data, size_t size) const
    {
        const size_t start = position % _buffer.size ();
        const size_t first = std::min (size, _buffer.size () - start);
        std::memcpy (data, _buffer.data () + start, first);
        std::memcpy (static_cast<uint8_t*> (data) + first, _buffer.data (), size - first);
    }

    std::vector<uint8_t> _buffer;
    std::atomic<size_t> _write { 0 }; // Both count every byte ever, so they only meet when the queue is empty.
    std::atomic<size_t> _read { 0 };
};

} // namespace Lv2
} // namespace Hadronyche
