#include <ssfplay/ssfplay.h>

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int make_unsafe_ssf(const char* input, const char* output) {
  FILE* in = fopen(input, "rb");
  if (!in) return 1;
  fseek(in, 0, SEEK_END);
  long size = ftell(in);
  rewind(in);
  char* data = (char*)malloc((size_t)size + 1);
  if (!data || fread(data, 1, (size_t)size, in) != (size_t)size) return 1;
  data[size] = 0;
  fclose(in);
  char* lib = 0;
  for (long i = 0; i + 5 < size; ++i)
    if (!memcmp(data + i, "_lib=", 5)) { lib = data + i; break; }
  if (!lib) return 1;
  lib += 5;
  char* end = lib;
  while (end < data + size && *end != '\n') ++end;
  if (end == data + size || end - lib < 7) return 1;
  memset(lib, 'x', (size_t)(end - lib));
  memcpy(lib, "../evil", 7);
  FILE* out = fopen(output, "wb");
  if (!out || fwrite(data, 1, (size_t)size, out) != (size_t)size) return 1;
  fclose(out);
  free(data);
  return 0;
}

static int test_failures(const char* music_dir) {
  char valid[4096];
  snprintf(valid, sizeof(valid), "%s/%s", music_dir,
           "25 Twin Seeds - Growing Wings.ssf");

  ssfplay_decoder* first = 0;
  ssfplay_decoder* second = 0;
  if (ssfplay_open(valid, 0, &first) != SSFPLAY_OK) { fprintf(stderr, "valid open failed\n"); return 1; }
  if (ssfplay_open(valid, 0, &second) != SSFPLAY_ERROR_BUSY) { fprintf(stderr, "busy test failed\n"); return 1; }
  ssfplay_close(first);

  if (ssfplay_open("/definitely/not/an/ssf", 0, &second) != SSFPLAY_ERROR_IO) { fprintf(stderr, "missing test failed\n"); return 1; }
  if (!ssfplay_last_error()[0]) { fprintf(stderr, "last error test failed\n"); return 1; }

  const char* unsafe = "/tmp/ssfplay-unsafe.ssf";
  if (make_unsafe_ssf(valid, unsafe)) { fprintf(stderr, "unsafe fixture failed\n"); return 1; }
  if (ssfplay_open(unsafe, 0, &second) != SSFPLAY_ERROR_IO) { fprintf(stderr, "unsafe open failed\n"); return 1; }
  if (!strstr(ssfplay_last_error(), "unsafe")) { fprintf(stderr, "unsafe error failed: %s\n", ssfplay_last_error()); return 1; }
  remove(unsafe);

  const char* malformed = "/tmp/ssfplay-malformed.ssf";
  FILE* bad_file = fopen(malformed, "wb");
  if (!bad_file) return 1;
  fwrite("not an ssf", 1, 10, bad_file);
  fclose(bad_file);
  if (ssfplay_open(malformed, 0, &second) != SSFPLAY_ERROR_FORMAT) { fprintf(stderr, "malformed test failed\n"); return 1; }
  remove(malformed);

  ssfplay_config bad;
  ssfplay_config_init(&bad);
  bad.sample_rate = 1;
  if (ssfplay_open(valid, &bad, &second) != SSFPLAY_ERROR_INVALID_ARGUMENT) { fprintf(stderr, "bad config failed\n"); return 1; }

  ssfplay_config resampled;
  ssfplay_config_init(&resampled);
  resampled.sample_rate = 48000;
  if (ssfplay_open(valid, &resampled, &first) != SSFPLAY_OK) { fprintf(stderr, "resampled open failed\n"); return 1; }
  int16_t before[2048], after[2048];
  size_t before_count = 0, after_count = 0;
  if (ssfplay_render(first, before, 1024, &before_count) != SSFPLAY_OK) { fprintf(stderr, "resampled render failed\n"); return 1; }
  if (ssfplay_reset(first) != SSFPLAY_OK) { fprintf(stderr, "resampled reset failed\n"); return 1; }
  if (ssfplay_render(first, after, 1024, &after_count) != SSFPLAY_OK) { fprintf(stderr, "resampled rerender failed\n"); return 1; }
  if (before_count != after_count || memcmp(before, after, before_count * 4)) { fprintf(stderr, "resampled determinism failed: %zu %zu\n", before_count, after_count); return 1; }
  ssfplay_close(first);

  ssfplay_config short_track;
  ssfplay_config_init(&short_track);
  short_track.length_ms = 10;
  short_track.fade_ms = 10;
  if (ssfplay_open(valid, &short_track, &first) != SSFPLAY_OK) return 1;
  int16_t short_pcm[1024 * 2];
  size_t short_count = 0;
  if (ssfplay_render(first, short_pcm, 1024, &short_count) != SSFPLAY_OK) return 1;
  if (short_count != 882) return 1;
  if (ssfplay_render(first, short_pcm, 1024, &short_count) != SSFPLAY_EOF || short_count != 0) return 1;
  ssfplay_close(first);
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 1) {
    ssfplay_config config;
    ssfplay_config_init(&config);
    if (config.sample_rate != 44100 || config.resampler_quality != 10 ||
        config.length_ms != -1 || config.fade_ms != -1) {
      fprintf(stderr, "default config mismatch\n");
      return 1;
    }
    ssfplay_decoder* d = 0;
    if (ssfplay_open("/definitely/not/an/ssf", 0, &d) != SSFPLAY_ERROR_IO) {
      fprintf(stderr, "missing file did not fail as expected\n");
      return 1;
    }
    if (!ssfplay_last_error()[0]) {
      fprintf(stderr, "missing file did not set last error\n");
      return 1;
    }
    printf("fixture-free API smoke test passed\n");
    return 0;
  }
  if (argc != 2) return 2;
  if (test_failures(argv[1])) return 1;
  DIR* dir = opendir(argv[1]);
  if (!dir) return 2;
  struct dirent* ent;
  int opened = 0;
  while ((ent = readdir(dir))) {
    size_t n = strlen(ent->d_name);
    if (n < 4 || strcmp(ent->d_name + n - 4, ".ssf")) continue;
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", argv[1], ent->d_name);
    ssfplay_decoder* d = 0;
    ssfplay_result r = ssfplay_open(path, 0, &d);
    if (r != SSFPLAY_OK) { fprintf(stderr, "open failed: %s: %s\n", path, ssfplay_result_string(r)); return 1; }
    int16_t first[2048], second[2048];
    size_t got1 = 0, got2 = 0;
    if (ssfplay_render(d, first, 1024, &got1) != SSFPLAY_OK || got1 != 1024) return 1;
    if (ssfplay_reset(d) != SSFPLAY_OK) return 1;
    if (ssfplay_render(d, second, 1024, &got2) != SSFPLAY_OK || got2 != got1 || memcmp(first, second, sizeof(first))) return 1;
    if (!ssfplay_metadata(d, SSFPLAY_METADATA_TITLE)[0] || ssfplay_length_ms(d) <= 0) return 1;
    ssfplay_close(d);
    ++opened;
  }
  closedir(dir);
  printf("opened and rendered %d SSF files\n", opened);
  return opened == 30 ? 0 : 1;
}
