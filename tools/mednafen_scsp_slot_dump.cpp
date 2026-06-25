#include "../src/ssfplay_capture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <zlib.h>

namespace {

struct Options {
  std::filesystem::path input;
  std::filesystem::path output_dir;
  uint64_t limit_samples = 0;
  bool skip_silent = false;
};

struct SlotStats {
  uint64_t frames = 0;
  uint64_t nonzero = 0;
  int peak = 0;
  long double sum_squares = 0;
};

struct TraceState {
  std::array<std::ofstream, 32> slot_files;
  std::array<SlotStats, 32> stats;
};

static uint16_t read_u16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

static uint32_t read_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) |
                               (p[3] << 24));
}

static void put_u16(std::ostream& out, uint16_t value) {
  const char bytes[2] = {static_cast<char>(value),
                         static_cast<char>(value >> 8)};
  out.write(bytes, 2);
}

static void put_u32(std::ostream& out, uint32_t value) {
  const char bytes[4] = {static_cast<char>(value),
                         static_cast<char>(value >> 8),
                         static_cast<char>(value >> 16),
                         static_cast<char>(value >> 24)};
  out.write(bytes, 4);
}

static void write_wav_header(std::ostream& out, uint16_t channels,
                             uint64_t frames) {
  const uint64_t data_bytes = frames * channels * sizeof(int16_t);
  if (data_bytes > std::numeric_limits<uint32_t>::max() - 36)
    throw std::runtime_error("WAV exceeds RIFF size limit");
  out.write("RIFF", 4);
  put_u32(out, static_cast<uint32_t>(36 + data_bytes));
  out.write("WAVEfmt ", 8);
  put_u32(out, 16);
  put_u16(out, 1);
  put_u16(out, channels);
  put_u32(out, 44100);
  put_u32(out, 44100 * channels * 2);
  put_u16(out, channels * 2);
  put_u16(out, 16);
  out.write("data", 4);
  put_u32(out, static_cast<uint32_t>(data_bytes));
}

static void write_sample(std::ostream& out, int16_t sample) {
  put_u16(out, static_cast<uint16_t>(sample));
}

static bool ends_with_ci(std::string_view text, std::string_view suffix) {
  if (text.size() < suffix.size()) return false;
  text.remove_prefix(text.size() - suffix.size());
  for (size_t i = 0; i < text.size(); ++i) {
    const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
    const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
    if (a != b) return false;
  }
  return true;
}

static std::vector<uint8_t> read_file(const std::filesystem::path& path) {
  const std::string name = path.string();
  if (ends_with_ci(name, ".vgz")) {
    gzFile file = gzopen(name.c_str(), "rb");
    if (!file) throw std::runtime_error("could not open VGZ");
    std::vector<uint8_t> data;
    std::array<uint8_t, 65536> chunk{};
    for (;;) {
      const int got = gzread(file, chunk.data(), static_cast<unsigned>(chunk.size()));
      if (got < 0) {
        int err = Z_OK;
        const char* msg = gzerror(file, &err);
        gzclose(file);
        throw std::runtime_error(msg ? msg : "VGZ read failed");
      }
      if (got == 0) break;
      data.insert(data.end(), chunk.begin(), chunk.begin() + got);
    }
    if (gzclose(file) != Z_OK) throw std::runtime_error("VGZ close failed");
    return data;
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("could not open input file");
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

static void parse_vgm(const std::vector<uint8_t>& data, std::vector<uint8_t>& ram,
                      std::vector<ssfplay_capture_event>& events,
                      uint64_t& total_samples) {
  if (data.size() < 0x100 || std::memcmp(data.data(), "Vgm ", 4))
    throw std::runtime_error("not a VGM file");
  total_samples = read_u32(data.data() + 0x18);
  size_t pos = 0x34 + read_u32(data.data() + 0x34);
  uint64_t sample = 0;
  bool initial_phase = true;
  ram.assign(0x80000, 0);
  while (pos < data.size()) {
    const uint8_t cmd = data[pos++];
    if (cmd == 0x66) break;
    if (cmd == 0x61) {
      if (pos + 2 > data.size()) throw std::runtime_error("truncated wait");
      sample += read_u16(data.data() + pos);
      pos += 2;
    } else if (cmd == 0x62) {
      sample += 735;
    } else if (cmd == 0x63) {
      sample += 882;
    } else if ((cmd & 0xF0) == 0x70) {
      sample += (cmd & 0x0F) + 1;
    } else if (cmd == 0xC5) {
      initial_phase = false;
      if (pos + 3 > data.size()) throw std::runtime_error("truncated SCSP write");
      ssfplay_capture_event event = {};
      event.sample = sample;
      event.type = SSFPLAY_CAPTURE_REGISTER_WRITE;
      event.address = static_cast<uint32_t>((data[pos] << 8) | data[pos + 1]);
      event.value = data[pos + 2];
      events.push_back(event);
      pos += 3;
    } else if (cmd == 0x67) {
      if (pos + 6 > data.size() || data[pos++] != 0x66)
        throw std::runtime_error("invalid data block");
      const uint8_t type = data[pos++];
      const uint32_t size = read_u32(data.data() + pos);
      pos += 4;
      if (size < 4 || pos + size > data.size())
        throw std::runtime_error("truncated data block");
      const uint32_t address = read_u32(data.data() + pos);
      pos += 4;
      const uint32_t bytes = size - 4;
      if (type == 0xE0) {
        if (initial_phase && sample == 0 && address < ram.size() &&
            bytes <= ram.size() - address) {
          std::copy(data.begin() + pos, data.begin() + pos + bytes,
                    ram.begin() + address);
        } else {
          initial_phase = false;
          for (uint32_t i = 0; i < bytes; ++i) {
            ssfplay_capture_event event = {};
            event.sample = sample;
            event.type = SSFPLAY_CAPTURE_RAM_WRITE;
            event.address = address + i;
            event.value = data[pos + i];
            events.push_back(event);
          }
        }
      }
      pos += bytes;
    } else {
      throw std::runtime_error("unsupported VGM command");
    }
  }
}

static void slot_callback(void* opaque, uint32_t slot, int16_t sample) {
  auto& state = *static_cast<TraceState*>(opaque);
  if (slot >= state.slot_files.size()) return;
  write_sample(state.slot_files[slot], sample);
  auto& stats = state.stats[slot];
  ++stats.frames;
  const int mag = std::abs(static_cast<int>(sample));
  stats.peak = std::max(stats.peak, mag);
  stats.nonzero += sample != 0;
  stats.sum_squares += static_cast<long double>(sample) * sample;
}

static void write_manifest(const std::filesystem::path& path,
                           const TraceState& state) {
  std::ofstream out(path);
  if (!out) throw std::runtime_error("could not write manifest");
  out << "slot,wav,frames,nonzero_samples,peak,rms,key_on_count\n";
  for (size_t i = 0; i < state.stats.size(); ++i) {
    const auto& stats = state.stats[i];
    const long double rms = stats.frames ? std::sqrt(stats.sum_squares / stats.frames) : 0.0L;
    char wav[32];
    std::snprintf(wav, sizeof(wav), "slot_%02zu.wav", i);
    out << i << ',' << wav << ',' << stats.frames << ',' << stats.nonzero
        << ',' << stats.peak << ',' << static_cast<double>(rms) << ",0\n";
  }
}

static void write_final_mix(const std::filesystem::path& path,
                            const std::vector<int16_t>& pcm) {
  std::ofstream out(path, std::ios::binary);
  if (!out) throw std::runtime_error("could not write final mix");
  write_wav_header(out, 2, pcm.size() / 2);
  for (int16_t sample : pcm) write_sample(out, sample);
}

static Options parse_args(int argc, char** argv) {
  Options options;
  std::vector<const char*> positional;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--skip-silent")) {
      options.skip_silent = true;
    } else if (!std::strcmp(argv[i], "--limit-ms") && i + 1 < argc) {
      const long long ms = std::strtoll(argv[++i], nullptr, 10);
      if (ms < 0) throw std::runtime_error("negative --limit-ms");
      options.limit_samples = static_cast<uint64_t>(ms) * 44100 / 1000;
    } else {
      positional.push_back(argv[i]);
    }
  }
  if (positional.size() != 2)
    throw std::runtime_error("expected input and output directory");
  options.input = positional[0];
  options.output_dir = positional[1];
  return options;
}

static void usage(const char* argv0) {
  std::fprintf(stderr,
               "Usage: %s [--skip-silent] [--limit-ms N] input.vgm|input.vgz output-dir\n",
               argv0);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_args(argc, argv);
    std::vector<uint8_t> ram;
    std::vector<ssfplay_capture_event> events;
    uint64_t samples = 0;
    parse_vgm(read_file(options.input), ram, events, samples);
    if (options.limit_samples)
      samples = std::min(samples, options.limit_samples);

    std::filesystem::create_directories(options.output_dir);
    TraceState state;
    for (size_t i = 0; i < state.slot_files.size(); ++i) {
      char name[32];
      std::snprintf(name, sizeof(name), "slot_%02zu.wav", i);
      state.slot_files[i].open(options.output_dir / name, std::ios::binary);
      if (!state.slot_files[i]) throw std::runtime_error("could not write slot WAV");
      write_wav_header(state.slot_files[i], 1, samples);
    }

    std::vector<int16_t> mix(static_cast<size_t>(samples) * 2);
    const ssfplay_result result = ssfplay_capture_replay_slot_trace(
        ram.data(), ram.size(), events.data(), events.size(), samples,
        slot_callback, &state, mix.data());
    if (result != SSFPLAY_OK)
      throw std::runtime_error(ssfplay_result_string(result));

    for (auto& file : state.slot_files) file.close();
    if (options.skip_silent) {
      for (size_t i = 0; i < state.stats.size(); ++i) {
        if (state.stats[i].peak != 0) continue;
        char name[32];
        std::snprintf(name, sizeof(name), "slot_%02zu.wav", i);
        std::filesystem::remove(options.output_dir / name);
      }
    }
    write_final_mix(options.output_dir / "final_mix.wav", mix);
    write_manifest(options.output_dir / "manifest.csv", state);
    std::fprintf(stderr, "wrote %llu samples to %s\n",
                 static_cast<unsigned long long>(samples),
                 options.output_dir.string().c_str());
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "mednafen_scsp_slot_dump: %s\n", ex.what());
    usage(argv[0]);
    return 1;
  }
}
