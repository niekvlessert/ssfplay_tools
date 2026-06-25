#include "libvgm_ab_slot_callback.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "player/playera.hpp"
#include "player/vgmplayer.hpp"
#include "player/s98player.hpp"
#include "player/droplayer.hpp"
#include "player/gymplayer.hpp"
#include "utils/DataLoader.h"
#include "utils/FileLoader.h"

namespace {

constexpr uint32_t kSampleRate = 44100;
constexpr uint32_t kChannels = 2;
constexpr uint32_t kBits = 16;
constexpr uint32_t kFramesPerBlock = 1024;
constexpr uint32_t kSlotCount = 32;
constexpr uint32_t kMagic = 0x42564741;  // "AGVB" little-endian.
constexpr uint32_t kVersion = 2;

struct StreamHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t sample_rate;
  uint32_t frames_per_block;
  uint32_t slot_count;
  uint32_t channels;
  uint64_t total_frames;
};

struct BlockHeader {
  uint32_t frames;
};

struct SlotCapture {
  std::vector<int16_t> samples;
  std::array<uint32_t, kSlotCount> keyons = {};
  size_t frame = 0;
};

static int16_t clip16(int32_t value) {
  if (value > 32767) return 32767;
  if (value < -32768) return -32768;
  return static_cast<int16_t>(value);
}

static int16_t read_i16le(const uint8_t* p) {
  return static_cast<int16_t>(static_cast<uint16_t>(p[0] | (p[1] << 8)));
}

static void write_all(FILE* file, const void* data, size_t size) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  while (size) {
    const size_t wrote = std::fwrite(p, 1, size, file);
    if (wrote == 0) {
      std::fprintf(stderr, "libvgm_ab_worker: write failed: %s\n", std::strerror(errno));
      std::exit(1);
    }
    p += wrote;
    size -= wrote;
  }
}

static void slot_callback(void* opaque, uint32_t slot, int32_t sample) {
  SlotCapture* capture = static_cast<SlotCapture*>(opaque);
  if (!capture || slot >= kSlotCount || capture->frame >= kFramesPerBlock)
    return;
  capture->samples[capture->frame * kSlotCount + slot] = clip16(sample);
  if (slot + 1 == kSlotCount)
    ++capture->frame;
}

static void keyon_callback(void* opaque, uint32_t slot, uint32_t, uint32_t) {
  SlotCapture* capture = static_cast<SlotCapture*>(opaque);
  if (!capture || slot >= kSlotCount)
    return;
  ++capture->keyons[slot];
}

static void usage(const char* argv0) {
  std::fprintf(stderr, "Usage: %s [--loops n] [--fade seconds] [--mute hexmask] input.vgm|input.vgz\n", argv0);
}

static uint32_t scan_u32(const char* text, uint32_t fallback) {
  char* end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 10);
  return end && *end == '\0' ? static_cast<uint32_t>(value) : fallback;
}

static uint32_t scan_hex_u32(const char* text, uint32_t fallback) {
  char* end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 16);
  return end && *end == '\0' ? static_cast<uint32_t>(value) : fallback;
}

static double scan_double(const char* text, double fallback) {
  char* end = nullptr;
  const double value = std::strtod(text, &end);
  return end && *end == '\0' ? value : fallback;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  uint32_t loops = 1;
  double fade = 0.0;
  uint32_t initial_mute_mask = 0;
  int arg = 1;
  for (; arg < argc; ++arg) {
    if (!std::strcmp(argv[arg], "--help") || !std::strcmp(argv[arg], "-h")) {
      usage(argv[0]);
      return 0;
    }
    if (!std::strcmp(argv[arg], "--loops") && arg + 1 < argc) {
      loops = scan_u32(argv[++arg], loops);
      continue;
    }
    if (!std::strcmp(argv[arg], "--fade") && arg + 1 < argc) {
      fade = scan_double(argv[++arg], fade);
      continue;
    }
    if (!std::strcmp(argv[arg], "--mute") && arg + 1 < argc) {
      initial_mute_mask = scan_hex_u32(argv[++arg], initial_mute_mask);
      continue;
    }
    break;
  }
  if (arg >= argc) {
    usage(argv[0]);
    return 2;
  }

  PlayerA player;
  player.RegisterPlayerEngine(new VGMPlayer);
  player.RegisterPlayerEngine(new S98Player);
  player.RegisterPlayerEngine(new DROPlayer);
  player.RegisterPlayerEngine(new GYMPlayer);
  if (player.SetOutputSettings(kSampleRate, kChannels, kBits, kFramesPerBlock)) {
    std::fprintf(stderr, "libvgm_ab_worker: unsupported output settings\n");
    return 1;
  }

  PlayerA::Config cfg = player.GetConfiguration();
  cfg.masterVol = 0x10000;
  cfg.loopCount = loops;
  cfg.fadeSmpls = static_cast<uint32_t>(kSampleRate * fade);
  cfg.endSilenceSmpls = 0;
  cfg.pbSpeed = 1.0;
  player.SetConfiguration(cfg);

  DATA_LOADER* loader = FileLoader_Init(argv[arg]);
  if (!loader) {
    std::fprintf(stderr, "libvgm_ab_worker: failed to create FileLoader\n");
    return 1;
  }
  DataLoader_SetPreloadBytes(loader, 0x100);
  if (DataLoader_Load(loader)) {
    std::fprintf(stderr, "libvgm_ab_worker: failed to load input\n");
    DataLoader_Deinit(loader);
    return 1;
  }
  if (player.LoadFile(loader)) {
    std::fprintf(stderr, "libvgm_ab_worker: failed to load file\n");
    DataLoader_Deinit(loader);
    return 1;
  }

  PlayerBase* engine = player.GetPlayer();
  if (!engine) {
    std::fprintf(stderr, "libvgm_ab_worker: no player engine\n");
    DataLoader_Deinit(loader);
    return 1;
  }
  if (engine->GetPlayerType() == FCC_VGM) {
    VGMPlayer* vgm = dynamic_cast<VGMPlayer*>(engine);
    if (vgm) player.SetLoopCount(vgm->GetModifiedLoopCount(loops));
  }

  player.Start();
  uint64_t total_frames = engine->Tick2Sample(engine->GetTotalPlayTicks(loops));
  if (engine->GetLoopTicks() > 0)
    total_frames += player.GetFadeSamples();

  StreamHeader header = {};
  header.magic = kMagic;
  header.version = kVersion;
  header.sample_rate = kSampleRate;
  header.frames_per_block = kFramesPerBlock;
  header.slot_count = kSlotCount;
  header.channels = kChannels;
  header.total_frames = total_frames;
  write_all(stdout, &header, sizeof(header));

  SlotCapture capture;
  capture.samples.resize(kFramesPerBlock * kSlotCount);
  std::vector<uint8_t> final_pcm(kFramesPerBlock * kChannels * sizeof(int16_t));
  std::vector<int16_t> block;
  block.resize(kFramesPerBlock * (kChannels + kSlotCount));

  uint32_t mute_mask = initial_mute_mask;
  uint64_t remaining = total_frames;
  std::string command_line;
  while (remaining && std::getline(std::cin, command_line)) {
    char command[32] = {};
    char value[32] = {};
    const int fields = std::sscanf(command_line.c_str(), "%31s %31s", command, value);
    if (fields >= 1 && !std::strcmp(command, "mute")) {
      if (fields == 2)
        mute_mask = scan_hex_u32(value, mute_mask);
      continue;
    }
    if (fields < 1 || std::strcmp(command, "render"))
      continue;

    const uint32_t frames = static_cast<uint32_t>(std::min<uint64_t>(remaining, kFramesPerBlock));
    std::fill(capture.samples.begin(), capture.samples.end(), 0);
    capture.keyons.fill(0);
    std::fill(final_pcm.begin(), final_pcm.end(), 0);
    capture.frame = 0;
    libvgm_scsp_set_debug_output_mute_mask(mute_mask);
    libvgm_scsp_set_slot_sample_callback(slot_callback, &capture);
    libvgm_scsp_set_slot_keyon_callback(keyon_callback, &capture);
    const uint32_t rendered_bytes = player.Render(frames * kChannels * sizeof(int16_t), final_pcm.data());
    libvgm_scsp_set_slot_keyon_callback(nullptr, nullptr);
    libvgm_scsp_set_slot_sample_callback(nullptr, nullptr);
    const uint32_t rendered_frames = rendered_bytes / (kChannels * sizeof(int16_t));
    if (rendered_frames == 0)
      break;

    for (uint32_t f = 0; f < rendered_frames; ++f) {
      block[f * (kChannels + kSlotCount) + 0] = read_i16le(&final_pcm[(f * 2 + 0) * 2]);
      block[f * (kChannels + kSlotCount) + 1] = read_i16le(&final_pcm[(f * 2 + 1) * 2]);
      for (uint32_t slot = 0; slot < kSlotCount; ++slot)
        block[f * (kChannels + kSlotCount) + kChannels + slot] =
            capture.samples[f * kSlotCount + slot];
    }

    BlockHeader block_header = {};
    block_header.frames = rendered_frames;
    write_all(stdout, &block_header, sizeof(block_header));
    write_all(stdout, block.data(), rendered_frames * (kChannels + kSlotCount) * sizeof(int16_t));
    write_all(stdout, capture.keyons.data(), capture.keyons.size() * sizeof(uint32_t));
    remaining -= rendered_frames;
  }

  libvgm_scsp_set_debug_output_mute_mask(0);
  player.Stop();
  player.UnloadFile();
  player.UnregisterAllPlayers();
  DataLoader_Deinit(loader);
  return 0;
}
