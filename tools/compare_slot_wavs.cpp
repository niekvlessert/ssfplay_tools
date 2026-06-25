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
#include <vector>

namespace {

struct WavInfo {
  uint32_t channels = 0;
  uint32_t sample_rate = 0;
  uint32_t bits = 0;
  uint32_t data_offset = 0;
  uint32_t data_size = 0;
};

struct Stats {
  uint64_t frames = 0;
  uint64_t nonzero = 0;
  uint64_t different = 0;
  int peak = 0;
  long double sum_sq = 0;
  uint64_t first = UINT64_MAX;
  uint64_t last = 0;
};

struct PairStats {
  Stats a;
  Stats b;
  uint64_t different = 0;
  long double diff_sq = 0;
  long double dot = 0;
  int max_abs_diff = 0;
};

static uint16_t read_u16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

static uint32_t read_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) |
                               (p[3] << 24));
}

static WavInfo read_wav_info(std::ifstream& in) {
  std::array<uint8_t, 12> riff{};
  in.read(reinterpret_cast<char*>(riff.data()), riff.size());
  if (!in || std::memcmp(riff.data(), "RIFF", 4) ||
      std::memcmp(riff.data() + 8, "WAVE", 4)) {
    throw std::runtime_error("not a RIFF/WAVE file");
  }
  WavInfo info;
  for (;;) {
    std::array<uint8_t, 8> hdr{};
    in.read(reinterpret_cast<char*>(hdr.data()), hdr.size());
    if (!in) throw std::runtime_error("missing WAVE data chunk");
    const uint32_t size = read_u32(hdr.data() + 4);
    if (!std::memcmp(hdr.data(), "fmt ", 4)) {
      std::vector<uint8_t> fmt(size);
      in.read(reinterpret_cast<char*>(fmt.data()), fmt.size());
      if (!in || fmt.size() < 16) throw std::runtime_error("bad fmt chunk");
      if (read_u16(fmt.data()) != 1) throw std::runtime_error("not PCM");
      info.channels = read_u16(fmt.data() + 2);
      info.sample_rate = read_u32(fmt.data() + 4);
      info.bits = read_u16(fmt.data() + 14);
    } else if (!std::memcmp(hdr.data(), "data", 4)) {
      info.data_offset = static_cast<uint32_t>(in.tellg());
      info.data_size = size;
      in.seekg(size + (size & 1), std::ios::cur);
    } else {
      in.seekg(size + (size & 1), std::ios::cur);
    }
    if (info.channels && info.data_offset) break;
  }
  if (info.channels != 1 || info.sample_rate != 44100 || info.bits != 16) {
    throw std::runtime_error("expected mono 44.1 kHz 16-bit PCM");
  }
  return info;
}

static void update_stats(Stats& s, int16_t value, uint64_t index) {
  ++s.frames;
  const int mag = std::abs(static_cast<int>(value));
  s.peak = std::max(s.peak, mag);
  s.sum_sq += static_cast<long double>(value) * value;
  if (value) {
    ++s.nonzero;
    s.first = std::min(s.first, index);
    s.last = index;
  }
}

static PairStats compare_wavs(const std::filesystem::path& a_path,
                              const std::filesystem::path& b_path) {
  std::ifstream a(a_path, std::ios::binary);
  std::ifstream b(b_path, std::ios::binary);
  if (!a || !b) throw std::runtime_error("could not open WAV pair");
  const WavInfo ai = read_wav_info(a);
  const WavInfo bi = read_wav_info(b);
  if (ai.data_size != bi.data_size) throw std::runtime_error("WAV length mismatch");
  a.seekg(ai.data_offset);
  b.seekg(bi.data_offset);

  PairStats out;
  std::vector<uint8_t> abuf(1 << 20);
  std::vector<uint8_t> bbuf(1 << 20);
  uint64_t index = 0;
  uint32_t remaining = ai.data_size;
  while (remaining) {
    const uint32_t bytes = std::min<uint32_t>(remaining, static_cast<uint32_t>(abuf.size()));
    a.read(reinterpret_cast<char*>(abuf.data()), bytes);
    b.read(reinterpret_cast<char*>(bbuf.data()), bytes);
    if (!a || !b) throw std::runtime_error("short WAV read");
    for (uint32_t pos = 0; pos < bytes; pos += 2, ++index) {
      const int16_t av = static_cast<int16_t>(read_u16(abuf.data() + pos));
      const int16_t bv = static_cast<int16_t>(read_u16(bbuf.data() + pos));
      update_stats(out.a, av, index);
      update_stats(out.b, bv, index);
      const int diff = static_cast<int>(bv) - static_cast<int>(av);
      out.different += diff != 0;
      out.diff_sq += static_cast<long double>(diff) * diff;
      out.dot += static_cast<long double>(av) * bv;
      out.max_abs_diff = std::max(out.max_abs_diff, std::abs(diff));
    }
    remaining -= bytes;
  }
  return out;
}

static long double rms(const Stats& s) {
  return s.frames ? std::sqrt(s.sum_sq / s.frames) : 0.0L;
}

static long double db_ratio(long double value, long double ref) {
  return value > 0 && ref > 0 ? 20.0L * std::log10(value / ref) : -999.0L;
}

static void usage(const char* argv0) {
  std::fprintf(stderr, "Usage: %s ymir-slot-dir libvgm-slot-dir output.csv\n", argv0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    usage(argv[0]);
    return 2;
  }
  try {
    const std::filesystem::path a_dir = argv[1];
    const std::filesystem::path b_dir = argv[2];
    std::ofstream csv(argv[3]);
    csv << "slot,frames,ymir_rms,libvgm_rms,rms_db_lib_minus_ymir,"
           "ymir_peak,libvgm_peak,ymir_active_pct,libvgm_active_pct,"
           "diff_pct,diff_rms,max_abs_diff,sample_corr,"
           "ymir_first_s,libvgm_first_s,ymir_last_s,libvgm_last_s\n";
    std::printf("slot  ymir_rms lib_rms  dB(lib-y) y_peak l_peak diff_rms maxdiff corr y_act lib_act\n");
    for (int slot = 0; slot < 32; ++slot) {
      char name[32];
      std::snprintf(name, sizeof(name), "slot_%02d.wav", slot);
      const PairStats s = compare_wavs(a_dir / name, b_dir / name);
      const long double ar = rms(s.a);
      const long double br = rms(s.b);
      const long double dr = s.a.frames ? std::sqrt(s.diff_sq / s.a.frames) : 0.0L;
      const long double corr =
          (s.a.sum_sq > 0 && s.b.sum_sq > 0) ? s.dot / std::sqrt(s.a.sum_sq * s.b.sum_sq) : 0.0L;
      const long double adb = db_ratio(br, ar);
      const long double a_active = s.a.frames ? 100.0L * s.a.nonzero / s.a.frames : 0.0L;
      const long double b_active = s.b.frames ? 100.0L * s.b.nonzero / s.b.frames : 0.0L;
      const long double diff_pct = s.a.frames ? 100.0L * s.different / s.a.frames : 0.0L;
      const long double afirst = s.a.first == UINT64_MAX ? -1.0L : static_cast<long double>(s.a.first) / 44100.0L;
      const long double bfirst = s.b.first == UINT64_MAX ? -1.0L : static_cast<long double>(s.b.first) / 44100.0L;
      const long double alast = static_cast<long double>(s.a.last) / 44100.0L;
      const long double blast = static_cast<long double>(s.b.last) / 44100.0L;
      csv << slot << "," << s.a.frames << "," << static_cast<double>(ar) << ","
          << static_cast<double>(br) << "," << static_cast<double>(adb) << ","
          << s.a.peak << "," << s.b.peak << "," << static_cast<double>(a_active) << ","
          << static_cast<double>(b_active) << "," << static_cast<double>(diff_pct) << ","
          << static_cast<double>(dr) << "," << s.max_abs_diff << ","
          << static_cast<double>(corr) << "," << static_cast<double>(afirst) << ","
          << static_cast<double>(bfirst) << "," << static_cast<double>(alast) << ","
          << static_cast<double>(blast) << "\n";
      std::printf("%02d %8.1Lf %8.1Lf %8.2Lf %6d %6d %8.1Lf %7d %6.3Lf %6.2Lf %6.2Lf\n",
                  slot, ar, br, adb, s.a.peak, s.b.peak, dr, s.max_abs_diff, corr,
                  a_active, b_active);
    }
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "compare_slot_wavs: %s\n", ex.what());
    return 1;
  }
}
