#pragma once
#include "vasdk_export.h"
#include <cstdint>
#include <memory>

namespace va {
/**
 * @brief Circular buffer for audio samples.
 */
class VASDK_EXPORT AudioRingBuffer
{
public:
    // Disable copy construction and assignment
    AudioRingBuffer &operator=(const AudioRingBuffer &) = delete;
    AudioRingBuffer(const AudioRingBuffer &) = delete;
    /**
     * @brief AudioRingBuffer
     * Default constructor is deleted to prevent instantiation without parameters.
     */
    AudioRingBuffer() = delete;
    /**
     * @brief AudioRingBuffer
     * @param capacity
     * @param sampleSize
     */
    AudioRingBuffer(long capacity, int sampleSize);
    ~AudioRingBuffer();

    /**
     * @brief Writes data at the current offset and advances it by the samples written, wrapping around circularly.
     * @param data Pointer to the source data.
     * @param srcSampleOffset Offset in the source data to start writing from.
     * @param numSamples Number of samples to write.
     * @return New offset after writing, or -1 on failure.
     */
    long write(const char *data, int srcSampleOffset, int numSamples);

    /**
     * @brief Reads data at a relative offset from the current write position, wrapping around circularly.
     * @param ringbufferOffset Offset relative to the current write position; 0 = most recent data, negative = past.
     * @param data Pointer to the destination buffer.
     * @param dstSampleOffset Offset in the destination buffer to start writing at.
     * @param numSamples Number of samples to read.
     */
    void read(long ringbufferOffset, char *data, int dstSampleOffset, int numSamples) const;

    /**
     * @brief Reads data at an absolute buffer position (0 = buffer start), wrapping around circularly.
     * @param absoluteOffset Absolute position in the buffer.
     * @param data Pointer to the destination buffer.
     * @param dstSampleOffset Offset in the destination buffer to start writing at.
     * @param numSamples Number of samples to read.
     * @return Number of samples read, or -1 on failure (e.g. out-of-bounds offset).
     */
    int readAbsolute(long absoluteOffset, char *data, int dstSampleOffset, int numSamples) const;

    /**
     * @brief get the current write offset
     * @return the write offset
     */
    long getOffset() const { return _offset; }

    /**
     * @brief returns the capacity in number of samples
     * @return capacity in samples
     */
    long getCapacity() const { return _capacity; }

    /**
     * @brief Capacity in bytes (samples × sample size).
     */
    long getCapacityInBytes() const { return _capacity * _sampleSize; }

    /**
     * @brief computeOffset
     * Computes the effective offset in the buffer given a relative offset.
     */
    long computeOffset(long ringbufferOffset) const;

    /**
     * @brief normalizeOffset
     * Normalizes any offset to be within the bounds of the buffer capacity.
     */
    long normalizeOffset(long offset) const;

    /**
     * @brief computeDeltaOffset
     * Computes the delta offset between two absolute offsets in the buffer.
     * The result is always non-negative and represents the distance from fromOffset to toOffset in a circular manner.
     */
    long computeDeltaOffset(long fromOffset, long toOffset) const;

    /**
     * @brief Get total samples written since buffer creation
     * @return Total number of samples ever written (continuous counter, not circular)
     */
    uint64_t getTotalSamplesWritten() const { return _totalSamplesWritten; }

    /**
     * @brief Overwrite data at a specific absolute offset without advancing the write pointer.
     *
     * Used by in-line DSP processors (e.g. noise suppression) to modify audio after the
     * transport writes it but before downstream processors (VAD, transcription) read it.
     *
     * @param absoluteOffset  Start position in the ring buffer (in samples).
     * @param data            Pointer to the replacement data.
     * @param numSamples      Number of samples to write.
     * @return true on success, false if parameters are invalid.
     */
    bool overwrite(long absoluteOffset, const char* data, int numSamples);

private:
    long _capacity = 0;
    int _sampleSize = 0;
    long _offset = 0;
    uint64_t _totalSamplesWritten = 0;
    std::unique_ptr<char[]> _buffer;
};

}
