#pragma once

namespace va {
namespace audio {
/**
 * @brief The AudioFormat enum
 */
enum AudioFormat { UNKNOWN, BYTE, SHORT, INT, FLOAT };

/**
 * @brief The AudioConfig class
 */
struct AudioConfig {
    /**
   * @brief the number of samples per second of audio
   */
    int sampleRate;

    /**
   * @brief the internal sample format
   */
    AudioFormat format;

    /**
   * @brief the number of channels
   */
    int channels;

    /**
     * @brief returns the byte size for each sample: sizeof(format) * channels
     * @return
     */
    int getSampleSizeInBytes() const
    {
        int size = 0;
        switch (format) {
        case AudioFormat::BYTE:
            size = sizeof(char);
            break;
        case AudioFormat::SHORT:
            size = sizeof(short);
            break;
        case AudioFormat::INT:
            size = sizeof(int);
            break;
        case AudioFormat::FLOAT:
            size = sizeof(float);
            break;
        default:
            return 0;
        }

        return size * channels;
    };
};
} // namespace audio
} // namespace va
