/*
 * codec2_dec — Codec2 1600 to WAV decoder
 *
 * Usage: codec2_dec <in.c2> <out.wav>
 *
 * Input:  raw codec2 bitstream, 8 bytes per 40ms frame (1600 bps)
 * Output: 16-bit PCM mono WAV at 8kHz
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "codec2_mod.h"

/* ---- minimal WAV writer ---- */

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
    fmt.fmt = 1;       /* PCM */
    fmt.nch = 1;       /* mono */
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
        fprintf(stderr, "Usage: %s <in.c2> <out.wav>\n", argv[0]);
        return 1;
    }

    FILE *fin = fopen(argv[1], "rb");
    if (!fin)
    {
        perror(argv[1]);
        return 1;
    }

    /* count frames */
    fseek(fin, 0, SEEK_END);
    long file_size = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    if (file_size % CODEC2_BYTES_PER_FRAME != 0)
    {
        fprintf(stderr, "Warning: file size %ld not a multiple of %d bytes, truncating\n",
                file_size, CODEC2_BYTES_PER_FRAME);
    }

    uint32_t frames = (uint32_t)(file_size / CODEC2_BYTES_PER_FRAME);
    uint32_t total_samples = frames * CODEC2_SAMPLES_PER_FRAME;

    if (frames == 0)
    {
        fprintf(stderr, "Error: empty input\n");
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

    /* write WAV header (will seek back to fix size later if needed) */
    write_wav_header(fout, total_samples);

    /* allocate codec state and buffers */
    codec2_t *c2 = aligned_alloc(64, sizeof(codec2_t));
    if (!c2)
    {
        fprintf(stderr, "Error: out of memory\n");
        fclose(fin);
        fclose(fout);
        return 1;
    }
    codec2_init(c2);

    int16_t speech[CODEC2_SAMPLES_PER_FRAME];
    uint8_t bits[CODEC2_BYTES_PER_FRAME];

    fprintf(stderr, "Decoding %u frames (%.2fs)\n",
            frames, (double)total_samples / 8000.0);

    uint32_t actual_frames = 0;
    for (uint32_t i = 0; i < frames; i++)
    {
        if (fread(bits, 1, CODEC2_BYTES_PER_FRAME, fin) != CODEC2_BYTES_PER_FRAME)
            break;

        codec2_decode(c2, speech, bits);
        fwrite(speech, sizeof(int16_t), CODEC2_SAMPLES_PER_FRAME, fout);
        actual_frames++;
    }

    /* fix WAV header if we decoded fewer frames than expected */
    if (actual_frames != frames)
    {
        uint32_t actual_samples = actual_frames * CODEC2_SAMPLES_PER_FRAME;
        fseek(fout, 0, SEEK_SET);
        write_wav_header(fout, actual_samples);
    }

    fprintf(stderr, "Done. %u frames decoded.\n", actual_frames);
    free(c2);
    fclose(fin);
    fclose(fout);
    return 0;
}
