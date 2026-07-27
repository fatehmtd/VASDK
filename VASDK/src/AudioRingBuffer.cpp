#include "AudioRingBuffer.h"
#include <cstddef>
#include <stdexcept>
#include <cstring>
#include <spdlog/spdlog.h>

using namespace va;

AudioRingBuffer::AudioRingBuffer(long capacity, int sampleSize)
    : _capacity(capacity)
    , _offset(0)
    , _sampleSize(sampleSize)
{
    if (capacity <= 0 || sampleSize <= 0) {
        throw std::invalid_argument("Invalid capacity or element size");
    }
    _buffer = std::make_unique<char[]>(static_cast<size_t>(_capacity) * sampleSize);
}

AudioRingBuffer::~AudioRingBuffer() = default;

long AudioRingBuffer::write(const char* data, int srcSampleOffset, int numSamples)
{
    if (srcSampleOffset < 0) {
        return -1; // Invalid offset
    }

    size_t totalBytesToWrite = static_cast<size_t>(numSamples) * _sampleSize;
    const char* sourcePtr = data + static_cast<size_t>(srcSampleOffset) * _sampleSize;

    size_t currentBufferOffsetBytes = _offset * _sampleSize;
    size_t capacityBytes = _capacity * _sampleSize;

    // Bytes available in the first segment before the buffer wraps
    size_t bytesUntilEnd = capacityBytes - currentBufferOffsetBytes;

    if (totalBytesToWrite <= bytesUntilEnd) {
        // Data fits in one contiguous block
        std::memcpy(_buffer.get() + currentBufferOffsetBytes, sourcePtr, totalBytesToWrite);
    }
    else {
        // Data wraps around
        std::memcpy(_buffer.get() + currentBufferOffsetBytes, sourcePtr, bytesUntilEnd);
        std::memcpy(_buffer.get(), sourcePtr + bytesUntilEnd, totalBytesToWrite - bytesUntilEnd);
    }

    _offset = (_offset + numSamples) % _capacity;

    // Track total samples written for stream offset mapping
    _totalSamplesWritten += numSamples;

    return _offset;
}

void AudioRingBuffer::read(long relativeOffset,
    char* data,
    int dstSampleOffset,
    int numSamples) const
{
    if (data == nullptr || numSamples <= 0 || dstSampleOffset < 0) {
        return; // Invalid parameters
    }

    // Wrap relativeOffset (which may be negative) into a valid [0, capacity) read offset
    long signedOffset = static_cast<long>(_offset) + relativeOffset;
    size_t actualReadOffset = (signedOffset % static_cast<long>(_capacity) + static_cast<long>(_capacity)) % static_cast<long>(_capacity);

    size_t bytesToRead = static_cast<size_t>(numSamples) * _sampleSize;
    char* destinationPtr = data + static_cast<size_t>(dstSampleOffset) * _sampleSize;

    size_t currentBufferReadOffsetBytes = actualReadOffset * _sampleSize;
    size_t capacityBytes = _capacity * _sampleSize;

    if (currentBufferReadOffsetBytes + bytesToRead <= capacityBytes) {
        // Read in one go if the data doesn't wrap around
        std::memcpy(destinationPtr, _buffer.get() + currentBufferReadOffsetBytes, bytesToRead);
    }
    else {
        // Wrap around required
        size_t bytesUntilEnd = capacityBytes - currentBufferReadOffsetBytes;
        std::memcpy(destinationPtr, _buffer.get() + currentBufferReadOffsetBytes, bytesUntilEnd);
        std::memcpy(destinationPtr + bytesUntilEnd, _buffer.get(), bytesToRead - bytesUntilEnd);
    }
}

int va::AudioRingBuffer::readAbsolute(long absoluteOffset, char* data, int dstSampleOffset, int numSamples) const
{
    if (data == nullptr || numSamples <= 0 || dstSampleOffset < 0) {
        return -1; // Invalid parameters
    }
    if (absoluteOffset < 0 || absoluteOffset >= _capacity) {
        return -1; // Invalid absolute offset
    }
    size_t bytesToRead = static_cast<size_t>(numSamples) * _sampleSize;
    char* destinationPtr = data + static_cast<size_t>(dstSampleOffset) * _sampleSize;

    size_t currentBufferReadOffsetBytes = absoluteOffset * _sampleSize;
    size_t capacityBytes = _capacity * _sampleSize;

    if (currentBufferReadOffsetBytes + bytesToRead <= capacityBytes) {
        // Read in one go if the data doesn't wrap around
        std::memcpy(destinationPtr, _buffer.get() + currentBufferReadOffsetBytes, bytesToRead);
    }
    else {
        // Wrap around required
        size_t bytesUntilEnd = capacityBytes - currentBufferReadOffsetBytes;
        std::memcpy(destinationPtr, _buffer.get() + currentBufferReadOffsetBytes, bytesUntilEnd);
        std::memcpy(destinationPtr + bytesUntilEnd, _buffer.get(), bytesToRead - bytesUntilEnd);
    }
    return numSamples;
}

long va::AudioRingBuffer::computeOffset(long relativeOffset) const
{
    long effectiveOffset = (_offset + relativeOffset) % _capacity;
    if (effectiveOffset < 0) {
        effectiveOffset += _capacity; // Ensure positive offset
    }
    return effectiveOffset;
}

long va::AudioRingBuffer::normalizeOffset(long offset) const
{
    return (offset % _capacity + _capacity) % _capacity; // Normalize to [0, capacity)
}

long va::AudioRingBuffer::computeDeltaOffset(long fromOffset, long toOffset) const
{
    return (toOffset - fromOffset + _capacity) % _capacity;
}

bool va::AudioRingBuffer::overwrite(long absoluteOffset, const char* data, int numSamples)
{
    if (data == nullptr || numSamples <= 0 || absoluteOffset < 0 || absoluteOffset >= _capacity) {
        return false;
    }

    const size_t totalBytes    = static_cast<size_t>(numSamples) * _sampleSize;
    const size_t startByte     = static_cast<size_t>(absoluteOffset) * _sampleSize;
    const size_t capacityBytes = static_cast<size_t>(_capacity) * _sampleSize;
    const size_t bytesUntilEnd = capacityBytes - startByte;

    if (totalBytes <= bytesUntilEnd) {
        std::memcpy(_buffer.get() + startByte, data, totalBytes);
    }
    else {
        std::memcpy(_buffer.get() + startByte, data, bytesUntilEnd);
        std::memcpy(_buffer.get(), data + bytesUntilEnd, totalBytes - bytesUntilEnd);
    }
    return true;
}