#include "../src/ssfplay_capture.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(path) mkdir(path, 0775)
#endif

namespace {

struct CandidateKey {
  uint32_t start;
  uint32_t samples;
  bool pcm8;
  uint8_t loop_mode;
  uint8_t sbctl;
  uint16_t lsa;
  uint16_t lea;

  bool operator<(const CandidateKey& other) const {
    if (start != other.start) return start < other.start;
    if (samples != other.samples) return samples < other.samples;
    if (pcm8 != other.pcm8) return pcm8 < other.pcm8;
    if (loop_mode != other.loop_mode) return loop_mode < other.loop_mode;
    if (sbctl != other.sbctl) return sbctl < other.sbctl;
    if (lsa != other.lsa) return lsa < other.lsa;
    return lea < other.lea;
  }
};

struct Candidate {
  CandidateKey key = {};
  uint64_t first_sample = 0;
  uint32_t slot_mask = 0;
  uint16_t pitch = 0;
  uint16_t input_mix = 0;
  uint16_t output_mix = 0;
  uint8_t total_level = 0;
};

static uint16_t word_be(const uint8_t* data, uint32_t offset) {
  return static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
}

static void put16(FILE* f, uint32_t v) {
  std::fputc(static_cast<int>(v & 0xFF), f);
  std::fputc(static_cast<int>((v >> 8) & 0xFF), f);
}

static void put32(FILE* f, uint32_t v) {
  put16(f, v & 0xFFFF);
  put16(f, v >> 16);
}

static bool write_wav(const std::string& path, const std::vector<int16_t>& pcm,
                      uint32_t sample_rate) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const uint32_t bytes = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
  std::fwrite("RIFF", 1, 4, f);
  put32(f, 36 + bytes);
  std::fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 1);
  put16(f, 1);
  put32(f, sample_rate);
  put32(f, sample_rate * 2);
  put16(f, 2);
  put16(f, 16);
  std::fwrite("data", 1, 4, f);
  put32(f, bytes);
  if (!pcm.empty())
    std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), f);
  const bool ok = !std::ferror(f);
  std::fclose(f);
  return ok;
}

static int16_t decode_sample(const uint8_t* ram, uint32_t address, bool pcm8,
                             uint8_t sbctl) {
  int32_t sample;
  if (pcm8) {
    sample = static_cast<int8_t>(ram[address]) << 8;
  } else {
    sample = static_cast<int16_t>((ram[address] << 8) | ram[address + 1]);
  }
  if (sbctl & 1) sample ^= 0x7FFF;
  if (sbctl & 2) sample = static_cast<int16_t>(sample ^ 0x8000);
  return static_cast<int16_t>(sample);
}

static void add_candidate(const uint8_t* shadow, uint64_t sample,
                          std::map<CandidateKey, Candidate>& candidates) {
  for (uint32_t slot = 0; slot < 32; ++slot) {
    const uint32_t base = slot * 0x20;
    const uint16_t control = word_be(shadow, base);
    const bool key_on = (control & 0x0800) != 0;
    const uint8_t source = static_cast<uint8_t>((control >> 7) & 3);
    if (!key_on || source != 0) continue;

    const bool pcm8 = (control & 0x10) != 0;
    const uint16_t lsa = word_be(shadow, base + 0x04);
    const uint16_t lea = word_be(shadow, base + 0x06);
    if (!lea || lea <= lsa) continue;

    CandidateKey key;
    key.start = ((control & 0xF) << 16) | word_be(shadow, base + 0x02);
    key.samples = lea;
    key.pcm8 = pcm8;
    key.loop_mode = static_cast<uint8_t>((control >> 5) & 3);
    key.sbctl = static_cast<uint8_t>((control >> 9) & 3);
    key.lsa = lsa;
    key.lea = lea;

    Candidate& c = candidates[key];
    if (!c.slot_mask) {
      c.key = key;
      c.first_sample = sample;
      c.pitch = word_be(shadow, base + 0x10);
      c.input_mix = word_be(shadow, base + 0x14);
      c.output_mix = word_be(shadow, base + 0x16);
      c.total_level = static_cast<uint8_t>(word_be(shadow, base + 0x0C) & 0xFF);
    }
    c.slot_mask |= (1u << slot);
  }
}

static std::string path_join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  const char last = a[a.size() - 1];
  if (last == '/' || last == '\\') return a + b;
  return a + "/" + b;
}

static bool ensure_dir(const std::string& path) {
  if (MKDIR(path.c_str()) == 0) return true;
  return errno == EEXIST;
}

static void usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s [--capture-ms n] [--rate hz] [--max-dumps n] "
               "[--pre-ms n] [--post-ms n] input.ssf output_dir\n",
               argv0);
}

}  // namespace

int main(int argc, char** argv) {
  int64_t capture_ms = 8000;
  uint32_t wav_rate = 44100;
  size_t max_dumps = 512;
  uint32_t pre_ms = 500;
  uint32_t post_ms = 500;

  int arg = 1;
  while (arg < argc && argv[arg][0] == '-') {
    if (!std::strcmp(argv[arg], "--capture-ms") && ++arg < argc) {
      capture_ms = std::strtoll(argv[arg], nullptr, 10);
    } else if (!std::strcmp(argv[arg], "--rate") && ++arg < argc) {
      wav_rate = static_cast<uint32_t>(std::strtoul(argv[arg], nullptr, 10));
    } else if (!std::strcmp(argv[arg], "--max-dumps") && ++arg < argc) {
      max_dumps = static_cast<size_t>(std::strtoull(argv[arg], nullptr, 10));
    } else if (!std::strcmp(argv[arg], "--pre-ms") && ++arg < argc) {
      pre_ms = static_cast<uint32_t>(std::strtoul(argv[arg], nullptr, 10));
    } else if (!std::strcmp(argv[arg], "--post-ms") && ++arg < argc) {
      post_ms = static_cast<uint32_t>(std::strtoul(argv[arg], nullptr, 10));
    } else {
      usage(argv[0]);
      return 2;
    }
    ++arg;
  }
  if (argc - arg != 2 || capture_ms <= 0 || wav_rate < 8000 ||
      wav_rate > 192000 || max_dumps == 0) {
    usage(argv[0]);
    return 2;
  }

  ssfplay_config config;
  ssfplay_config_init(&config);
  config.sample_rate = 44100;
  config.resampler_quality = 10;
  config.length_ms = capture_ms;
  config.fade_ms = 0;

  ssfplay_decoder* decoder = nullptr;
  ssfplay_result result = ssfplay_open(argv[arg], &config, &decoder);
  if (result != SSFPLAY_OK) {
    std::fprintf(stderr, "ssf_sample_dump: %s%s%s\n",
                 ssfplay_result_string(result),
                 ssfplay_last_error()[0] ? ": " : "", ssfplay_last_error());
    return 1;
  }

  if (!ensure_dir(argv[arg + 1])) {
    std::perror("ssf_sample_dump: mkdir");
    ssfplay_close(decoder);
    return 1;
  }

  if (ssfplay_capture_begin(decoder) != SSFPLAY_OK) {
    std::fprintf(stderr, "ssf_sample_dump: capture failed: %s\n",
                 ssfplay_error(decoder));
    ssfplay_close(decoder);
    return 1;
  }

  int16_t discard[4096 * 2];
  for (;;) {
    size_t rendered = 0;
    result = ssfplay_render(decoder, discard, 4096, &rendered);
    if (result == SSFPLAY_EOF) break;
    if (result != SSFPLAY_OK) {
      std::fprintf(stderr, "ssf_sample_dump: render failed: %s\n",
                   ssfplay_error(decoder));
      ssfplay_close(decoder);
      return 1;
    }
  }
  ssfplay_capture_end(decoder);

  size_t ram_size = 0;
  const uint8_t* initial_ram = ssfplay_capture_initial_ram(decoder, &ram_size);
  size_t event_count = 0;
  const ssfplay_capture_event* events =
      ssfplay_capture_events(decoder, &event_count);
  if (!initial_ram || !ram_size) {
    std::fprintf(stderr, "ssf_sample_dump: no SCSP RAM captured\n");
    ssfplay_close(decoder);
    return 1;
  }

  std::vector<uint8_t> ram(initial_ram, initial_ram + ram_size);
  uint8_t shadow[0x1000] = {};
  std::map<CandidateKey, Candidate> candidates;
  for (size_t i = 0; i < event_count; ++i) {
    const ssfplay_capture_event& event = events[i];
    if (event.type == SSFPLAY_CAPTURE_RAM_WRITE) {
      if (event.address < ram.size()) ram[event.address] = event.value;
    } else {
      shadow[event.address & 0xFFF] = event.value;
      add_candidate(shadow, event.sample, candidates);
    }
  }

  const std::string manifest_path = path_join(argv[arg + 1], "manifest.csv");
  FILE* manifest = std::fopen(manifest_path.c_str(), "wb");
  if (!manifest) {
    std::perror("ssf_sample_dump: manifest");
    ssfplay_close(decoder);
    return 1;
  }
  std::fprintf(manifest,
               "file,start,length_samples,format,loop_mode,loop_start,"
               "loop_end,sbctl,slots,first_ms,total_level,pitch,input_mix,"
               "output_mix,rms,peak,zero_prefix_samples,pre_ms,post_ms\n");

  const size_t pre_samples =
      static_cast<size_t>(static_cast<uint64_t>(pre_ms) * wav_rate / 1000);
  const size_t post_samples =
      static_cast<size_t>(static_cast<uint64_t>(post_ms) * wav_rate / 1000);

  size_t written = 0;
  for (const auto& item : candidates) {
    if (written >= max_dumps) break;
    const Candidate& c = item.second;
    const uint32_t bytes_per_sample = c.key.pcm8 ? 1 : 2;
    const uint64_t byte_len =
        static_cast<uint64_t>(c.key.samples) * bytes_per_sample;
    if (c.key.start >= ram.size() || byte_len < 16 ||
        byte_len > ram.size() - c.key.start)
      continue;

    std::vector<int16_t> raw_pcm(c.key.samples);
    double energy = 0.0;
    int peak = 0;
    uint32_t zero_prefix = 0;
    bool prefix = true;
    for (uint32_t i = 0; i < c.key.samples; ++i) {
      const uint32_t address = c.key.start + i * bytes_per_sample;
      const int16_t sample =
          decode_sample(ram.data(), address, c.key.pcm8, c.key.sbctl);
      raw_pcm[i] = sample;
      const int abs_sample = std::abs(static_cast<int>(sample));
      peak = std::max(peak, abs_sample);
      energy += static_cast<double>(sample) * sample;
      if (prefix && abs_sample < 16)
        ++zero_prefix;
      else
        prefix = false;
    }
    if (peak == 0) continue;

    std::vector<int16_t> pcm(pre_samples + raw_pcm.size() + post_samples, 0);
    std::copy(raw_pcm.begin(), raw_pcm.end(), pcm.begin() + pre_samples);

    char filename[160];
    std::snprintf(filename, sizeof(filename),
                  "%03zu_%06x_%s_%05u_%08x.wav", written + 1, c.key.start,
                  c.key.pcm8 ? "pcm8" : "pcm16", c.key.samples,
                  c.slot_mask);
    const std::string wav_path = path_join(argv[arg + 1], filename);
    if (!write_wav(wav_path, pcm, wav_rate)) {
      std::perror("ssf_sample_dump: wav");
      std::fclose(manifest);
      ssfplay_close(decoder);
      return 1;
    }

    const double rms = std::sqrt(energy / raw_pcm.size());
    std::fprintf(manifest,
                 "%s,%u,%u,%s,%u,%u,%u,%u,0x%08x,%.3f,%u,0x%04x,"
                 "0x%04x,0x%04x,%.2f,%d,%u,%u,%u\n",
                 filename, c.key.start, c.key.samples,
                 c.key.pcm8 ? "pcm8" : "pcm16", c.key.loop_mode, c.key.lsa,
                 c.key.lea, c.key.sbctl, c.slot_mask,
                 static_cast<double>(c.first_sample) * 1000.0 / 44100.0,
                 c.total_level, c.pitch, c.input_mix, c.output_mix, rms, peak,
                 zero_prefix, pre_ms, post_ms);
    ++written;
  }

  std::fclose(manifest);
  ssfplay_close(decoder);
  std::printf("dumped %zu active SCSP sample(s) to %s\n", written,
              argv[arg + 1]);
  std::printf("manifest: %s\n", manifest_path.c_str());
  return 0;
}
