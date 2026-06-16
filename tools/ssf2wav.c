#include <ssfplay/ssfplay.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put16(FILE* f, unsigned v) {
  fputc(v & 255, f); fputc((v >> 8) & 255, f);
}
static void put32(FILE* f, unsigned v) {
  put16(f, v & 65535); put16(f, v >> 16);
}
static void wav_header(FILE* f, unsigned rate, unsigned bytes) {
  fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16); put16(f, 1); put16(f, 2); put32(f, rate); put32(f, rate * 4);
  put16(f, 4); put16(f, 16); fwrite("data", 1, 4, f); put32(f, bytes);
}

int main(int argc, char** argv) {
  ssfplay_config config;
  ssfplay_config_init(&config);
  int arg = 1;
  while (arg < argc && argv[arg][0] == '-') {
    if (!strcmp(argv[arg], "--rate") && ++arg < argc) config.sample_rate = (unsigned)strtoul(argv[arg], 0, 10);
    else if (!strcmp(argv[arg], "--quality") && ++arg < argc) config.resampler_quality = (unsigned)strtoul(argv[arg], 0, 10);
    else if (!strcmp(argv[arg], "--length-ms") && ++arg < argc) config.length_ms = strtoll(argv[arg], 0, 10);
    else if (!strcmp(argv[arg], "--fade-ms") && ++arg < argc) config.fade_ms = strtoll(argv[arg], 0, 10);
    else { fprintf(stderr, "usage: ssf2wav [--rate hz] [--quality 0-10] [--length-ms n] [--fade-ms n] input.ssf output.wav\n"); return 2; }
    ++arg;
  }
  if (argc - arg != 2) { fprintf(stderr, "usage: ssf2wav [options] input.ssf output.wav\n"); return 2; }
  ssfplay_decoder* d = 0;
  ssfplay_result r = ssfplay_open(argv[arg], &config, &d);
  if (r != SSFPLAY_OK) {
    fprintf(stderr, "ssf2wav: %s%s%s\n", ssfplay_result_string(r),
            ssfplay_last_error()[0] ? ": " : "", ssfplay_last_error());
    return 1;
  }
  unsigned long long frames = (unsigned long long)(ssfplay_length_ms(d) + ssfplay_fade_ms(d)) * ssfplay_sample_rate(d) / 1000;
  if (!frames || frames > (0xFFFFFFFFULL - 36) / 4) { fprintf(stderr, "ssf2wav: missing duration or RIFF output too large\n"); ssfplay_close(d); return 1; }
  FILE* f = fopen(argv[arg + 1], "wb");
  if (!f) { perror("ssf2wav"); ssfplay_close(d); return 1; }
  wav_header(f, ssfplay_sample_rate(d), (unsigned)frames * 4);
  int16_t pcm[4096 * 2];
  for (;;) {
    size_t got = 0;
    r = ssfplay_render(d, pcm, 4096, &got);
    if (got && fwrite(pcm, 4, got, f) != got) { perror("ssf2wav"); r = SSFPLAY_ERROR_IO; break; }
    if (r == SSFPLAY_EOF) break;
    if (r != SSFPLAY_OK) break;
  }
  fclose(f);
  ssfplay_close(d);
  return r == SSFPLAY_EOF ? 0 : 1;
}
