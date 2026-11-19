#include <stddef.h>
#include <stdint.h>

enum { FIR_TAPS = 128, FIR_SAMPLES = 16384 };

static int16_t fir_quantize(int64_t sum)
{
    int64_t magnitude = sum < 0 ? -sum : sum;
    int64_t rounded = (magnitude + 16384) >> 15;
    if (sum < 0)
        rounded = -rounded;
    if (rounded > INT16_MAX)
        return INT16_MAX;
    if (rounded < INT16_MIN)
        return INT16_MIN;
    return (int16_t)rounded;
}

static void fir_reference(const int16_t *input, int16_t *output, size_t count)
{
    static const int16_t coefficients[FIR_TAPS] = {
#include "coeff.inc"
    };
    int16_t history[FIR_TAPS] = {0};
    unsigned head = 0;

    for (size_t i = 0; i < count; ++i) {
        history[head] = input[i];
        int64_t sum = 0;
        for (unsigned tap = 0; tap < FIR_TAPS; ++tap)
            sum += (int32_t)history[(head - tap) & (FIR_TAPS - 1)] * coefficients[tap];
        output[i] = fir_quantize(sum);
        head = (head + 1) & (FIR_TAPS - 1);
    }
}
