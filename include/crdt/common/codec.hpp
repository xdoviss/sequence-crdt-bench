// turns numbers into bytes and back (for network messages and snapshots)
//
// numbers are written as "varints" (LEB128) - small numbers take one byte, bigger ones
// take more, 7 useful bits per byte
//
// every algorithm uses the same encoder, so the
// message sizes of different algorithms can be compared directly.
#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include "crdt/common/types.hpp"

namespace crdt {

// can't decode bytes! panic!
struct DecodeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// appends encoded numbers to a byte buffer
class ByteWriter {
public:
    explicit ByteWriter(Bytes& output) : output_(output) {}

    void write_byte(uint8_t value) { output_.push_back(value); }

    void write_varint(uint64_t value) {
        // write 7 bits at a time, the top bit of a byte says more are coming! prepare!
        while (value >= 0x80) {
            output_.push_back(static_cast<uint8_t>(value) | 0x80);
            value >>= 7;
        }

        output_.push_back(static_cast<uint8_t>(value));
    }

    size_t size() const { return output_.size(); }

private:
    Bytes& output_;
};

// reads numbers from a byte buffer (validates if they make sense too)
class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t length) : cursor_(data), end_(data + length) {}
    explicit ByteReader(const Bytes& bytes) : ByteReader(bytes.data(), bytes.size()) {}
    explicit ByteReader(const std::string& text)
        : ByteReader(reinterpret_cast<const uint8_t*>(text.data()), text.size()) {}

    uint8_t read_byte() {
        if (cursor_ >= end_) 
            throw DecodeError("unexpected end of input");

        return *cursor_++;
    }

    uint64_t read_varint() {
        uint64_t result = 0;
        int shift = 0;
        while (true) {
            if (cursor_ >= end_) 
                throw DecodeError("unexpected end of input in varint");

            uint8_t byte = *cursor_++;
            if (shift > 63 || (shift == 63 && (byte & 0x7e)))
                throw DecodeError("varint overflow");

            result |= static_cast<uint64_t>(byte & 0x7f) << shift;
            if (!(byte & 0x80)) 
                return result;

            shift += 7;
        }
    }

    uint32_t read_u32() {
        uint64_t value = read_varint();
        if (value > std::numeric_limits<uint32_t>::max()) 
            throw DecodeError("u32 out of range");

        return static_cast<uint32_t>(value);
    }

    // checks how many items follow
    // if the items can't fit -> dispose, otherwise a huge array will be allocated
    size_t read_count(size_t min_bytes_per_item) {
        uint64_t items = read_varint();
        size_t remaining = static_cast<size_t>(end_ - cursor_);

        if (items > remaining / min_bytes_per_item) 
            throw DecodeError("element count doesn't fit");

        return static_cast<size_t>(items);
    }

    bool done() const {
        return cursor_ == end_;
    }

private:
    const uint8_t* cursor_;
    const uint8_t* end_;
};

inline ElementId bytes_to_id(const Bytes& bytes) { 
    return ElementId(bytes.begin(), bytes.end());
}

}  // namespace crdt
