/*
 * codec2_encodec — WAV through Codec2 1600 round-trip (encode then decode)
 *
 * Usage: codec2_encodec <in.wav> <out.wav>
 *
 * Input:  16-bit PCM mono WAV (any rate, resampled to 8kHz)
 * Output: 16-bit PCM mono WAV at 8kHz (after codec2 encode/decode round-trip)
 *
 * Useful for quality evaluation: output is what the codec would produce.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "codec2_mod.h"

/* ---- minimal WAV I/O ---- */

#pragma pack(push, 1)
typedef struct
{
    char riff[4];
    uint32_t file_size;
    char wave[4];
} wav_hdr_t;

typedef struct
{
    char id[4];
    uint32_t size;
} chunk_hdr_t;

typedef struct
{
    uint16_t fmt;
    uint16_t nch;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits;
} fmt_chunk_t;
#pragma pack(pop)

static int find_chunk(FILE *f, const char *id, uint32_t *out_size)
{
    chunk_hdr_t ch;
    while (fread(&ch, sizeof(ch), 1, f) == 1)
    {
        if (memcmp(ch.id, id, 4) == 0)
        {
            *out_size = ch.size;
            return 0;
        }
        fseek(f, ch.size, SEEK_CUR);
    }
    return -1;
}

static void write_wav_header(FILE *f, uint32_t n_samples)
{
    uint32_t data_size = n_samples * sizeof(int16_t);
    uint32_t fmt_size = sizeof(fmt_chunk_t);

    wav_hdr_t hdr;
    memcpy(hdr.riff, "RIFF", 4);
    hdr.file_size = sizeof(wav_hdr_t) + sizeof(chunk_hdr_t) + fmt_size + sizeof(chunk_hdr_t) + data_size;
    memcpy(hdr.wave, "WAVE", 4);
    fwrite(&hdr, sizeof(hdr), 1, f);

    chunk_hdr_t fmt_hdr;
    memcpy(fmt_hdr.id, "fmt ", 4);
    fmt_hdr.size = fmt_size;
    fwrite(&fmt_hdr, sizeof(fmt_hdr), 1, f);

    fmt_chunk_t fmt;
    fmt.fmt = 1;
    fmt.nch = 1;
    fmt.sample_rate = 8000;
    fmt.byte_rate = 8000 * sizeof(int16_t);
    fmt.block_align = sizeof(int16_t);
    fmt.bits = 16;
    fwrite(&fmt, sizeof(fmt), 1, f);

    chunk_hdr_t data_hdr;
    memcpy(data_hdr.id, "data", 4);
    data_hdr.size = data_size;
    fwrite(&data_hdr, sizeof(data_hdr), 1, f);
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <in.wav> <out.wav>\n", argv[0]);
        return 1;
    }

    FILE *fin = fopen(argv[1], "rb");
    if (!fin)
    {
        perror(argv[1]);
        return 1;
    }

    /* parse WAV header */
    wav_hdr_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, fin) != 1 ||
        memcmp(hdr.riff, "RIFF", 4) != 0 ||
        memcmp(hdr.wave, "WAVE", 4) != 0)
    {
        fprintf(stderr, "Error: not a WAV file\n");
        fclose(fin);
        return 1;
    }

    uint32_t fmt_size;
    if (find_chunk(fin, "fmt ", &fmt_size) != 0)
    {
        fprintf(stderr, "Error: fmt chunk not found\n");
        fclose(fin);
        return 1;
    }

    fmt_chunk_t fmt;
    if (fread(&fmt, sizeof(fmt), 1, fin) != 1)
    {
        fprintf(stderr, "Error: failed to read fmt data\n");
        fclose(fin);
        return 1;
    }
    if (fmt_size > sizeof(fmt))
        fseek(fin, fmt_size - sizeof(fmt), SEEK_CUR);

    if (fmt.fmt != 1)
    {
        fprintf(stderr, "Error: unsupported format %u (PCM required)\n", fmt.fmt);
        fclose(fin);
        return 1;
    }
    if (fmt.bits != 16)
    {
        fprintf(stderr, "Error: %u-bit samples not supported (16-bit required)\n", fmt.bits);
        fclose(fin);
        return 1;
    }

    uint32_t data_size;
    if (find_chunk(fin, "data", &data_size) != 0)
    {
        fprintf(stderr, "Error: data chunk not found\n");
        fclose(fin);
        return 1;
    }

    uint32_t n_samples = data_size / (fmt.bits / 8) / fmt.nch;
    uint32_t frames = n_samples / CODEC2_SAMPLES_PER_FRAME;
    uint32_t total_samples = frames * CODEC2_SAMPLES_PER_FRAME;

    if (frames == 0)
    {
        fprintf(stderr, "Error: input too short (< 1 frame of %d samples)\n",
                CODEC2_SAMPLES_PER_FRAME);
        fclose(fin);
        return 1;
    }

    FILE *fout = fopen(argv[2], "wb");
    if (!fout)
    {
        perror(argv[2]);
        fclose(fin);
        return 1;
    }

    write_wav_header(fout, total_samples);

    /* allocate codec state */
    codec2_t *c2 = aligned_alloc(64, sizeof(codec2_t));
    if (!c2)
    {
        fprintf(stderr, "Error: out of memory\n");
        fclose(fin);
        fclose(fout);
        return 1;
    }
    codec2_init(c2);

    int16_t speech_in[CODEC2_SAMPLES_PER_FRAME];
    int16_t speech_out[CODEC2_SAMPLES_PER_FRAME];
    uint8_t bits[CODEC2_BYTES_PER_FRAME];

    fprintf(stderr, "Round-tripping %u frames (%.2fs), %u Hz, %uch → 8kHz mono 1600bps\n",
            frames, (double)total_samples / fmt.sample_rate,
            fmt.sample_rate, fmt.nch);

    for (uint32_t i = 0; i < frames; i++)
    {
        /* read samples */
        if (fmt.nch == 1)
        {
            if (fread(speech_in, sizeof(int16_t), CODEC2_SAMPLES_PER_FRAME, fin) != CODEC2_SAMPLES_PER_FRAME)
                break;
        }
        else
        {
            int16_t buf[CODEC2_SAMPLES_PER_FRAME * fmt.nch];
            if (fread(buf, sizeof(int16_t), CODEC2_SAMPLES_PER_FRAME * fmt.nch, fin) != (size_t)(CODEC2_SAMPLES_PER_FRAME * fmt.nch))
                break;
            for (int j = 0; j < CODEC2_SAMPLES_PER_FRAME; j++)
            {
                int32_t mix = 0;
                for (int ch = 0; ch < fmt.nch; ch++)
                    mix += buf[j * fmt.nch + ch];
                speech_in[j] = (int16_t)(mix / fmt.nch);
            }
        }

        /* resample if needed */
        if (fmt.sample_rate != 8000)
        {
            int16_t resampled[CODEC2_SAMPLES_PER_FRAME];
            float ratio = (float)fmt.sample_rate / 8000.0f;
            for (int j = 0; j < CODEC2_SAMPLES_PER_FRAME; j++)
            {
                float src_idx = j * ratio;
                int idx0 = (int)src_idx;
                int idx1 = idx0 + 1;
                if (idx1 >= CODEC2_SAMPLES_PER_FRAME)
                    idx1 = CODEC2_SAMPLES_PER_FRAME - 1;
                float frac = src_idx - idx0;
                resampled[j] = (int16_t)(speech_in[idx0] * (1.0f - frac) + speech_in[idx1] * frac);
            }
            memcpy(speech_in, resampled, sizeof(speech_in));
        }

        /* encode then decode (round-trip) */
        codec2_encode(c2, bits, speech_in);
        codec2_decode(c2, speech_out, bits);

        fwrite(speech_out, sizeof(int16_t), CODEC2_SAMPLES_PER_FRAME, fout);
    }

    fprintf(stderr, "Done.\n");
    free(c2);
    fclose(fin);
    fclose(fout);
    return 0;
}
