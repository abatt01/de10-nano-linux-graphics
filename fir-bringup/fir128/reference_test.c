#include <assert.h>
#include <stdio.h>
#include "reference.h"

int main(void)
{
    static int16_t input[FIR_SAMPLES], expected[FIR_SAMPLES], output[FIR_SAMPLES];
    FILE *clip = fopen("mixed.s16le", "rb");
    FILE *oracle = fopen("expected.s16le", "rb");
    assert(clip && oracle);
    assert(fread(input, sizeof(*input), FIR_SAMPLES, clip) == FIR_SAMPLES);
    assert(fread(expected, sizeof(*expected), FIR_SAMPLES, oracle) == FIR_SAMPLES);
    assert(fgetc(clip) == EOF && fgetc(oracle) == EOF);
    fclose(clip);
    fclose(oracle);
    assert(fir_quantize(16384) == 1 && fir_quantize(-16384) == -1);
    assert(fir_quantize(16383) == 0 && fir_quantize(-16383) == 0);
    assert(fir_quantize(32768LL * 32768) == 32767);
    assert(fir_quantize(-32769LL * 32768) == -32768);
    fir_reference(input, output, FIR_SAMPLES);
    for (size_t i = 0; i < FIR_SAMPLES; ++i)
        assert(output[i] == expected[i]);
    puts("C reference agrees with integer oracle on all 16384 samples");
}
