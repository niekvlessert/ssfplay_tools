#include <atomic>
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#define popen _popen
#define pclose _pclose
#elif defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <unistd.h>
#include <sys/wait.h>
#elif defined(SSFPLAY_HAVE_ALSA)
#include <alsa/asoundlib.h>
#include <unistd.h>
#include <sys/wait.h>
#else
#include <chrono>
#include <unistd.h>
#include <sys/wait.h>
#endif

#ifndef LIBVGM_AB_NEW_WORKER
#define LIBVGM_AB_NEW_WORKER "libvgm_ab_worker_new"
#endif
#ifndef LIBVGM_AB_OLD_WORKER
#define LIBVGM_AB_OLD_WORKER "libvgm_ab_worker_old"
#endif

namespace {

constexpr uint32_t kMagic = 0x42564741;
constexpr uint32_t kVersion = 2;
constexpr uint32_t kSampleRate = 44100;
constexpr uint32_t kSlotCount = 32;
constexpr uint32_t kFinalChannels = 2;

enum class Mode : int {
  All = 0,
  Slot = 1,
  Old = 2,
  New = 3,
};

enum class Monitor : int {
  AB = 0,
  OldBoth = 1,
  NewBoth = 2,
};

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

struct Worker {
  FILE* pipe = nullptr;
  FILE* control = nullptr;
  std::mutex control_mutex;
#if !defined(_WIN32)
  pid_t pid = -1;
#endif
  StreamHeader header = {};
  std::vector<int16_t> samples;
  std::array<uint32_t, kSlotCount> keyons = {};
  uint32_t frames = 0;
  bool ended = false;
};

struct Options {
  std::string input;
  std::string dump_wav;
  std::string old_worker = LIBVGM_AB_OLD_WORKER;
  std::string new_worker = LIBVGM_AB_NEW_WORKER;
  uint32_t loops = 1;
  double fade = 0.0;
  bool slot_stats = false;
};

std::atomic<bool> g_running(true);
std::atomic<int> g_mode(static_cast<int>(Mode::All));
std::atomic<int> g_monitor(static_cast<int>(Monitor::AB));
std::atomic<int> g_slot(0);
std::atomic<int> g_old_gain_milli(4000);
std::atomic<int> g_new_gain_milli(1000);
std::atomic<int> g_solo_gain_milli(1000);
Worker* g_control_old_worker = nullptr;
Worker* g_control_new_worker = nullptr;

static int16_t clip16(int32_t value) {
  if (value > 32767) return 32767;
  if (value < -32768) return -32768;
  return static_cast<int16_t>(value);
}

static int32_t apply_gain(int32_t value, int gain_milli) {
  return static_cast<int32_t>((static_cast<int64_t>(value) * gain_milli) / 1000);
}

static bool read_exact(FILE* file, void* data, size_t size) {
  uint8_t* p = static_cast<uint8_t*>(data);
  while (size) {
    const size_t got = std::fread(p, 1, size, file);
    if (got == 0)
      return false;
    p += got;
    size -= got;
  }
  return true;
}

static void put_u16(FILE* f, uint16_t v) {
  const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
  std::fwrite(b, 1, 2, f);
}

static void put_u32(FILE* f, uint32_t v) {
  const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                        static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
  std::fwrite(b, 1, 4, f);
}

static void write_wav_header(FILE* f, uint64_t frames) {
  const uint64_t data_bytes = frames * 2 * sizeof(int16_t);
  const uint32_t riff_size = data_bytes > 0xFFFFFFFFull - 36 ? 0xFFFFFFFFu : static_cast<uint32_t>(36 + data_bytes);
  std::fwrite("RIFF", 1, 4, f);
  put_u32(f, riff_size);
  std::fwrite("WAVEfmt ", 1, 8, f);
  put_u32(f, 16);
  put_u16(f, 1);
  put_u16(f, 2);
  put_u32(f, kSampleRate);
  put_u32(f, kSampleRate * 4);
  put_u16(f, 4);
  put_u16(f, 16);
  std::fwrite("data", 1, 4, f);
  put_u32(f, data_bytes > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(data_bytes));
}

static std::string shell_quote(const std::string& text) {
#ifdef _WIN32
  std::string out = "\"";
  for (char c : text) {
    if (c == '"') out += "\\\"";
    else out += c;
  }
  out += "\"";
  return out;
#else
  std::string out = "'";
  for (char c : text) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  out += "'";
  return out;
#endif
}

static uint32_t keep_only_slot_mask(int slot) {
  return ~static_cast<uint32_t>(1u << static_cast<uint32_t>(slot));
}

static uint32_t mute_mask_for_mode(bool old_worker) {
  const Mode mode = static_cast<Mode>(g_mode.load());
  const int slot = std::max(0, std::min(31, g_slot.load()));
  const uint32_t keep_slot = keep_only_slot_mask(slot);
  if (mode == Mode::All)
    return 0;
  if (mode == Mode::Slot)
    return keep_slot;
  if (mode == Mode::Old)
    return old_worker ? keep_slot : 0xFFFFFFFFu;
  return old_worker ? 0xFFFFFFFFu : keep_slot;
}

static std::string hex_mask(uint32_t mask) {
  char text[16] = {};
  std::snprintf(text, sizeof(text), "%08x", mask);
  return text;
}

static bool start_worker(Worker& worker, const std::string& exe, const Options& options,
                         uint32_t initial_mute_mask) {
#if defined(_WIN32)
  std::ostringstream cmd;
  cmd << shell_quote(exe) << " --loops " << options.loops << " --fade " << options.fade
      << " --mute " << hex_mask(initial_mute_mask)
      << " " << shell_quote(options.input);
  worker.pipe = popen(cmd.str().c_str(), "r");
  if (!worker.pipe) {
    std::fprintf(stderr, "libvgm_ab_player: could not start worker: %s\n", exe.c_str());
    return false;
  }
#else
  int out_pipe[2] = {-1, -1};
  int in_pipe[2] = {-1, -1};
  if (pipe(out_pipe) != 0 || pipe(in_pipe) != 0) {
    std::fprintf(stderr, "libvgm_ab_player: pipe failed: %s\n", std::strerror(errno));
    return false;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    std::fprintf(stderr, "libvgm_ab_player: fork failed: %s\n", std::strerror(errno));
    return false;
  }
  if (pid == 0) {
    dup2(in_pipe[0], STDIN_FILENO);
    dup2(out_pipe[1], STDOUT_FILENO);
    close(in_pipe[0]);
    close(in_pipe[1]);
    close(out_pipe[0]);
    close(out_pipe[1]);
    const std::string loops = std::to_string(options.loops);
    const std::string fade = std::to_string(options.fade);
    const std::string mute = hex_mask(initial_mute_mask);
    execl(exe.c_str(), exe.c_str(), "--loops", loops.c_str(), "--fade", fade.c_str(),
          "--mute", mute.c_str(),
          options.input.c_str(), static_cast<char*>(nullptr));
    std::fprintf(stderr, "libvgm_ab_player: exec failed: %s: %s\n", exe.c_str(), std::strerror(errno));
    _exit(127);
  }
  close(in_pipe[0]);
  close(out_pipe[1]);
  worker.pid = pid;
  worker.control = fdopen(in_pipe[1], "w");
  worker.pipe = fdopen(out_pipe[0], "r");
  if (!worker.control || !worker.pipe) {
    std::fprintf(stderr, "libvgm_ab_player: fdopen failed\n");
    return false;
  }
#endif
  if (!read_exact(worker.pipe, &worker.header, sizeof(worker.header))) {
    std::fprintf(stderr, "libvgm_ab_player: worker did not send a header: %s\n", exe.c_str());
    return false;
  }
  if (worker.header.magic != kMagic || worker.header.version != kVersion ||
      worker.header.sample_rate != kSampleRate || worker.header.slot_count != kSlotCount ||
      worker.header.channels != kFinalChannels) {
    std::fprintf(stderr, "libvgm_ab_player: incompatible worker stream: %s\n", exe.c_str());
    return false;
  }
  worker.samples.resize(worker.header.frames_per_block * (kFinalChannels + kSlotCount));
  return true;
}

static void close_worker(Worker& worker) {
  if (worker.control) {
    std::fclose(worker.control);
    worker.control = nullptr;
  }
  if (worker.pipe) {
#if defined(_WIN32)
    pclose(worker.pipe);
#else
    std::fclose(worker.pipe);
#endif
    worker.pipe = nullptr;
  }
#if !defined(_WIN32)
  if (worker.pid > 0) {
    int status = 0;
    waitpid(worker.pid, &status, 0);
    worker.pid = -1;
  }
#endif
}

static bool send_worker_line(Worker* worker, const char* line) {
  if (!worker || !worker->control)
    return false;
  std::lock_guard<std::mutex> lock(worker->control_mutex);
  if (std::fputs(line, worker->control) < 0)
    return false;
  return std::fflush(worker->control) == 0;
}

static void send_mute_mask(Worker* worker, uint32_t mask) {
  char line[32] = {};
  std::snprintf(line, sizeof(line), "mute %08x\n", mask);
  send_worker_line(worker, line);
}

static void apply_worker_mutes() {
  send_mute_mask(g_control_old_worker, mute_mask_for_mode(true));
  send_mute_mask(g_control_new_worker, mute_mask_for_mode(false));
}

static bool read_block(Worker& worker) {
  if (worker.ended) {
    worker.frames = 0;
    return true;
  }
  if (!send_worker_line(&worker, "render\n")) {
    worker.ended = true;
    worker.frames = 0;
    return true;
  }
  BlockHeader header = {};
  if (!read_exact(worker.pipe, &header, sizeof(header))) {
    worker.ended = true;
    worker.frames = 0;
    return true;
  }
  if (header.frames > worker.header.frames_per_block) {
    std::fprintf(stderr, "libvgm_ab_player: worker block too large\n");
    return false;
  }
  const size_t values = static_cast<size_t>(header.frames) * (kFinalChannels + kSlotCount);
  if (!read_exact(worker.pipe, worker.samples.data(), values * sizeof(int16_t))) {
    std::fprintf(stderr, "libvgm_ab_player: short worker block\n");
    return false;
  }
  if (!read_exact(worker.pipe, worker.keyons.data(), worker.keyons.size() * sizeof(uint32_t))) {
    std::fprintf(stderr, "libvgm_ab_player: short worker key-on block\n");
    return false;
  }
  worker.frames = header.frames;
  return true;
}

static int16_t sample_at(const Worker& worker, uint32_t frame, uint32_t index) {
  if (frame >= worker.frames) return 0;
  return worker.samples[static_cast<size_t>(frame) * (kFinalChannels + kSlotCount) + index];
}

static int32_t final_mix_mono(const Worker& worker, uint32_t frame) {
  return (static_cast<int32_t>(sample_at(worker, frame, 0)) + sample_at(worker, frame, 1)) / 2;
}

static void mix_blocks(const Worker& old_worker, const Worker& new_worker,
                       uint32_t frames, std::vector<int16_t>& out) {
  out.resize(static_cast<size_t>(frames) * 2);
  const Mode mode = static_cast<Mode>(g_mode.load());
  const Monitor monitor = static_cast<Monitor>(g_monitor.load());
  const int slot = std::max(0, std::min(31, g_slot.load()));
  for (uint32_t f = 0; f < frames; ++f) {
    int32_t old_value = 0;
    int32_t new_value = 0;
    if (mode == Mode::All) {
      old_value = apply_gain(final_mix_mono(old_worker, f), g_old_gain_milli.load());
      new_value = apply_gain(final_mix_mono(new_worker, f), g_new_gain_milli.load());
    } else if (mode == Mode::Slot) {
      old_value = apply_gain(final_mix_mono(old_worker, f), g_old_gain_milli.load());
      new_value = apply_gain(final_mix_mono(new_worker, f), g_new_gain_milli.load());
    } else if (mode == Mode::Old) {
      old_value = apply_gain(final_mix_mono(old_worker, f), g_solo_gain_milli.load());
    } else {
      new_value = apply_gain(final_mix_mono(new_worker, f), g_solo_gain_milli.load());
    }

    int32_t left = old_value;
    int32_t right = new_value;
    if (monitor == Monitor::OldBoth || mode == Mode::Old) {
      left = right = old_value;
    } else if (monitor == Monitor::NewBoth || mode == Mode::New) {
      left = right = new_value;
    }
    out[f * 2 + 0] = clip16(left);
    out[f * 2 + 1] = clip16(right);
  }
}

static void print_mode() {
  const Mode mode = static_cast<Mode>(g_mode.load());
  const Monitor monitor = static_cast<Monitor>(g_monitor.load());
  const int slot = g_slot.load();
  const double old_gain = g_old_gain_milli.load() / 1000.0;
  const double new_gain = g_new_gain_milli.load() / 1000.0;
  const double solo_gain = g_solo_gain_milli.load() / 1000.0;
  if (mode == Mode::All)
    std::fprintf(stderr, "\nmode: all (old final mix left %.2fx, new final mix right %.2fx)\n",
                 old_gain, new_gain);
  else if (mode == Mode::Slot)
    std::fprintf(stderr, "\nmode: slot %d (old left %.2fx, new right %.2fx)\n",
                 slot, old_gain, new_gain);
  else if (mode == Mode::Old)
    std::fprintf(stderr, "\nmode: old slot %d centered %.2fx\n", slot, solo_gain);
  else
    std::fprintf(stderr, "\nmode: new slot %d centered %.2fx\n", slot, solo_gain);

  if (monitor == Monitor::AB)
    std::fprintf(stderr, "monitor: A/B (old left, new right)\n");
  else if (monitor == Monitor::OldBoth)
    std::fprintf(stderr, "monitor: old/left source in both ears\n");
  else
    std::fprintf(stderr, "monitor: new/right source in both ears\n");
}

static void control_thread() {
  std::fprintf(stderr, "Commands: all/a, slot N/s N, old N/o N, new N/n N,\n"
                       "          solo N, solo old N, solo new N, next, prev,\n"
                       "          ab, hear old, hear new, left, right,\n"
                       "          oldgain X, newgain X, sologain X, gain OLD NEW, q\n");
  print_mode();
  std::string line;
  while (g_running.load() && std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    for (char& c : cmd) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    int n = g_slot.load();
    if (cmd == "q" || cmd == "quit") {
      g_running = false;
      break;
    } else if (cmd == "all" || cmd == "a") {
      g_mode = static_cast<int>(Mode::All);
    } else if (cmd == "next") {
      g_slot = (g_slot.load() + 1) & 31;
      g_mode = static_cast<int>(Mode::Slot);
    } else if (cmd == "prev") {
      g_slot = (g_slot.load() + 31) & 31;
      g_mode = static_cast<int>(Mode::Slot);
    } else if ((cmd == "slot" || cmd == "s") && (in >> n) && n >= 0 && n < 32) {
      g_slot = n;
      g_mode = static_cast<int>(Mode::Slot);
    } else if ((cmd == "old" || cmd == "o") && (in >> n) && n >= 0 && n < 32) {
      g_slot = n;
      g_mode = static_cast<int>(Mode::Old);
      g_monitor = static_cast<int>(Monitor::OldBoth);
    } else if ((cmd == "new" || cmd == "n") && (in >> n) && n >= 0 && n < 32) {
      g_slot = n;
      g_mode = static_cast<int>(Mode::New);
      g_monitor = static_cast<int>(Monitor::NewBoth);
    } else if (cmd == "solo") {
      std::string which;
      if (!(in >> which)) {
        std::fprintf(stderr, "Use: solo N, solo old N, or solo new N\n");
        continue;
      }
      for (char& c : which) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (which == "old" || which == "o") {
        if (!(in >> n) || n < 0 || n >= 32) {
          std::fprintf(stderr, "Use: solo old N, where N is 0..31\n");
          continue;
        }
        g_slot = n;
        g_mode = static_cast<int>(Mode::Old);
        g_monitor = static_cast<int>(Monitor::OldBoth);
      } else if (which == "new" || which == "n") {
        if (!(in >> n) || n < 0 || n >= 32) {
          std::fprintf(stderr, "Use: solo new N, where N is 0..31\n");
          continue;
        }
        g_slot = n;
        g_mode = static_cast<int>(Mode::New);
        g_monitor = static_cast<int>(Monitor::NewBoth);
      } else {
        char* end = nullptr;
        const long slot = std::strtol(which.c_str(), &end, 10);
        if (!end || *end != '\0' || slot < 0 || slot >= 32) {
          std::fprintf(stderr, "Use: solo N, where N is 0..31\n");
          continue;
        }
        g_slot = static_cast<int>(slot);
        g_mode = static_cast<int>(Mode::New);
        g_monitor = static_cast<int>(Monitor::NewBoth);
      }
    } else if (cmd == "ab" || cmd == "compare") {
      g_monitor = static_cast<int>(Monitor::AB);
    } else if (cmd == "left" || cmd == "oldleft" || cmd == "oldboth") {
      g_monitor = static_cast<int>(Monitor::OldBoth);
    } else if (cmd == "right" || cmd == "newright" || cmd == "newboth") {
      g_monitor = static_cast<int>(Monitor::NewBoth);
    } else if (cmd == "hear") {
      std::string which;
      if (!(in >> which)) {
        std::fprintf(stderr, "Use: hear old, hear left, hear new, or hear right\n");
        continue;
      }
      for (char& c : which) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (which == "old" || which == "left" || which == "l")
        g_monitor = static_cast<int>(Monitor::OldBoth);
      else if (which == "new" || which == "right" || which == "r")
        g_monitor = static_cast<int>(Monitor::NewBoth);
      else {
        std::fprintf(stderr, "Use: hear old, hear left, hear new, or hear right\n");
        continue;
      }
    } else if (cmd == "oldgain") {
      double gain = 1.0;
      if (!(in >> gain) || gain < 0.0 || gain > 32.0) {
        std::fprintf(stderr, "Use: oldgain 0..32\n");
        continue;
      }
      g_old_gain_milli = static_cast<int>(gain * 1000.0 + 0.5);
    } else if (cmd == "newgain") {
      double gain = 1.0;
      if (!(in >> gain) || gain < 0.0 || gain > 32.0) {
        std::fprintf(stderr, "Use: newgain 0..32\n");
        continue;
      }
      g_new_gain_milli = static_cast<int>(gain * 1000.0 + 0.5);
    } else if (cmd == "sologain") {
      double gain = 1.0;
      if (!(in >> gain) || gain < 0.0 || gain > 32.0) {
        std::fprintf(stderr, "Use: sologain 0..32\n");
        continue;
      }
      g_solo_gain_milli = static_cast<int>(gain * 1000.0 + 0.5);
    } else if (cmd == "gain") {
      double old_gain = 1.0;
      double new_gain = 1.0;
      if (!(in >> old_gain >> new_gain) || old_gain < 0.0 || old_gain > 32.0 ||
          new_gain < 0.0 || new_gain > 32.0) {
        std::fprintf(stderr, "Use: gain OLD NEW, with values 0..32\n");
        continue;
      }
      g_old_gain_milli = static_cast<int>(old_gain * 1000.0 + 0.5);
      g_new_gain_milli = static_cast<int>(new_gain * 1000.0 + 0.5);
    } else {
      std::fprintf(stderr, "Unknown command. Use: all, slot N, old N, new N, solo N, solo old N, solo new N, next, prev, ab, hear old, hear new, left, right, oldgain X, newgain X, sologain X, gain OLD NEW, q\n");
      continue;
    }
    apply_worker_mutes();
    print_mode();
  }
}

static void usage(const char* argv0) {
  std::fprintf(stderr,
      "Usage: %s [--dump-wav out.wav] [--mode all|slot|old|new] [--slot n]\n"
      "       [--monitor ab|old|new]\n"
      "       [--slot-stats]\n"
      "       [--old-gain x] [--new-gain x] [--solo-gain x]\n"
      "       [--old-worker path] [--new-worker path]\n"
      "       input.vgm|input.vgz\n",
      argv0);
}

static bool set_mode_name(const char* name) {
  if (!std::strcmp(name, "all") || !std::strcmp(name, "a")) {
    g_mode = static_cast<int>(Mode::All);
    return true;
  }
  if (!std::strcmp(name, "slot") || !std::strcmp(name, "s")) {
    g_mode = static_cast<int>(Mode::Slot);
    return true;
  }
  if (!std::strcmp(name, "old") || !std::strcmp(name, "o")) {
    g_mode = static_cast<int>(Mode::Old);
    return true;
  }
  if (!std::strcmp(name, "new") || !std::strcmp(name, "n")) {
    g_mode = static_cast<int>(Mode::New);
    return true;
  }
  return false;
}

static bool set_monitor_name(const char* name) {
  if (!std::strcmp(name, "ab") || !std::strcmp(name, "compare")) {
    g_monitor = static_cast<int>(Monitor::AB);
    return true;
  }
  if (!std::strcmp(name, "old") || !std::strcmp(name, "left") || !std::strcmp(name, "l")) {
    g_monitor = static_cast<int>(Monitor::OldBoth);
    return true;
  }
  if (!std::strcmp(name, "new") || !std::strcmp(name, "right") || !std::strcmp(name, "r")) {
    g_monitor = static_cast<int>(Monitor::NewBoth);
    return true;
  }
  return false;
}

static bool parse_args(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
      usage(argv[0]);
      return false;
    } else if (!std::strcmp(argv[i], "--dump-wav") && i + 1 < argc) {
      options.dump_wav = argv[++i];
    } else if (!std::strcmp(argv[i], "--old-worker") && i + 1 < argc) {
      options.old_worker = argv[++i];
    } else if (!std::strcmp(argv[i], "--new-worker") && i + 1 < argc) {
      options.new_worker = argv[++i];
    } else if (!std::strcmp(argv[i], "--slot-stats")) {
      options.slot_stats = true;
    } else if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) {
      if (!set_mode_name(argv[++i])) {
        usage(argv[0]);
        return false;
      }
    } else if (!std::strcmp(argv[i], "--monitor") && i + 1 < argc) {
      if (!set_monitor_name(argv[++i])) {
        usage(argv[0]);
        return false;
      }
    } else if (!std::strcmp(argv[i], "--slot") && i + 1 < argc) {
      const long slot = std::strtol(argv[++i], nullptr, 10);
      if (slot < 0 || slot >= 32) {
        usage(argv[0]);
        return false;
      }
      g_slot = static_cast<int>(slot);
    } else if (!std::strcmp(argv[i], "--old-gain") && i + 1 < argc) {
      const double gain = std::strtod(argv[++i], nullptr);
      if (gain < 0.0 || gain > 32.0) {
        usage(argv[0]);
        return false;
      }
      g_old_gain_milli = static_cast<int>(gain * 1000.0 + 0.5);
    } else if (!std::strcmp(argv[i], "--new-gain") && i + 1 < argc) {
      const double gain = std::strtod(argv[++i], nullptr);
      if (gain < 0.0 || gain > 32.0) {
        usage(argv[0]);
        return false;
      }
      g_new_gain_milli = static_cast<int>(gain * 1000.0 + 0.5);
    } else if (!std::strcmp(argv[i], "--solo-gain") && i + 1 < argc) {
      const double gain = std::strtod(argv[++i], nullptr);
      if (gain < 0.0 || gain > 32.0) {
        usage(argv[0]);
        return false;
      }
      g_solo_gain_milli = static_cast<int>(gain * 1000.0 + 0.5);
    } else if (!std::strcmp(argv[i], "--loops") && i + 1 < argc) {
      options.loops = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (!std::strcmp(argv[i], "--fade") && i + 1 < argc) {
      options.fade = std::strtod(argv[++i], nullptr);
    } else if (argv[i][0] == '-') {
      usage(argv[0]);
      return false;
    } else {
      options.input = argv[i];
    }
  }
  if (options.input.empty()) {
    usage(argv[0]);
    return false;
  }
  return true;
}

static bool dump_wav(Worker& old_worker, Worker& new_worker, const std::string& path) {
  FILE* out = std::fopen(path.c_str(), "wb");
  if (!out) {
    std::fprintf(stderr, "libvgm_ab_player: could not open %s\n", path.c_str());
    return false;
  }
  write_wav_header(out, 0);
  std::vector<int16_t> mixed;
  uint64_t written = 0;
  while (g_running.load()) {
    if (!read_block(old_worker) || !read_block(new_worker)) {
      std::fclose(out);
      return false;
    }
    const uint32_t frames = std::max(old_worker.frames, new_worker.frames);
    if (!frames) break;
    mix_blocks(old_worker, new_worker, frames, mixed);
    std::fwrite(mixed.data(), sizeof(int16_t), mixed.size(), out);
    written += frames;
  }
  std::fseek(out, 0, SEEK_SET);
  write_wav_header(out, written);
  std::fclose(out);
  return true;
}

struct SlotStats {
  uint64_t active_frames = 0;
  uint64_t keyons = 0;
  uint64_t energy = 0;
  int peak = 0;
};

static void accumulate_slot_stats(const Worker& worker, std::array<SlotStats, kSlotCount>& stats) {
  for (uint32_t slot = 0; slot < kSlotCount; ++slot)
    stats[slot].keyons += worker.keyons[slot];

  for (uint32_t f = 0; f < worker.frames; ++f) {
    for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
      const int sample = sample_at(worker, f, kFinalChannels + slot);
      const int abs_sample = sample < 0 ? -sample : sample;
      if (abs_sample > 8)
        ++stats[slot].active_frames;
      stats[slot].energy += static_cast<uint64_t>(abs_sample) * static_cast<uint64_t>(abs_sample);
      if (abs_sample > stats[slot].peak)
        stats[slot].peak = abs_sample;
    }
  }
}

static bool print_slot_stats(Worker& old_worker, Worker& new_worker) {
  std::array<SlotStats, kSlotCount> old_stats = {};
  std::array<SlotStats, kSlotCount> new_stats = {};
  uint64_t total_frames = 0;

  while (g_running.load()) {
    if (!read_block(old_worker) || !read_block(new_worker))
      return false;
    const uint32_t frames = std::max(old_worker.frames, new_worker.frames);
    if (!frames)
      break;
    total_frames += frames;
    accumulate_slot_stats(old_worker, old_stats);
    accumulate_slot_stats(new_worker, new_stats);
  }

  std::printf("slot,total_s,old_keyons,new_keyons,old_active_pct,new_active_pct,old_peak,new_peak\n");
  const double total = total_frames ? static_cast<double>(total_frames) : 1.0;
  for (uint32_t slot = 0; slot < kSlotCount; ++slot) {
    std::printf("%u,%.3f,%llu,%llu,%.2f,%.2f,%d,%d\n",
                slot,
                total_frames / static_cast<double>(kSampleRate),
                static_cast<unsigned long long>(old_stats[slot].keyons),
                static_cast<unsigned long long>(new_stats[slot].keyons),
                100.0 * old_stats[slot].active_frames / total,
                100.0 * new_stats[slot].active_frames / total,
                old_stats[slot].peak,
                new_stats[slot].peak);
  }
  return true;
}

#if defined(_WIN32)
static bool play_audio(Worker& old_worker, Worker& new_worker) {
  WAVEFORMATEX fmt = {};
  fmt.wFormatTag = WAVE_FORMAT_PCM;
  fmt.nChannels = 2;
  fmt.nSamplesPerSec = kSampleRate;
  fmt.wBitsPerSample = 16;
  fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
  fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

  HWAVEOUT wave = nullptr;
  if (waveOutOpen(&wave, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
    std::fprintf(stderr, "libvgm_ab_player: waveOutOpen failed\n");
    return false;
  }

  std::vector<int16_t> mixed;
  while (g_running.load()) {
    if (!read_block(old_worker) || !read_block(new_worker)) break;
    const uint32_t frames = std::max(old_worker.frames, new_worker.frames);
    if (!frames) break;
    mix_blocks(old_worker, new_worker, frames, mixed);

    WAVEHDR header = {};
    header.lpData = reinterpret_cast<LPSTR>(mixed.data());
    header.dwBufferLength = static_cast<DWORD>(mixed.size() * sizeof(int16_t));
    if (waveOutPrepareHeader(wave, &header, sizeof(header)) != MMSYSERR_NOERROR)
      break;
    if (waveOutWrite(wave, &header, sizeof(header)) != MMSYSERR_NOERROR) {
      waveOutUnprepareHeader(wave, &header, sizeof(header));
      break;
    }
    while (g_running.load() && !(header.dwFlags & WHDR_DONE))
      Sleep(1);
    waveOutUnprepareHeader(wave, &header, sizeof(header));
  }

  waveOutReset(wave);
  waveOutClose(wave);
  return true;
}
#elif defined(__APPLE__)
AudioQueueRef g_audio_queue = nullptr;
Worker* g_audio_old_worker = nullptr;
Worker* g_audio_new_worker = nullptr;

static bool fill_audioqueue_buffer(AudioQueueBufferRef buffer) {
  if (!g_audio_old_worker || !g_audio_new_worker)
    return false;
  if (!read_block(*g_audio_old_worker) || !read_block(*g_audio_new_worker))
    return false;
  const uint32_t frames = std::max(g_audio_old_worker->frames, g_audio_new_worker->frames);
  if (!frames)
    return false;
  static thread_local std::vector<int16_t> mixed;
  mix_blocks(*g_audio_old_worker, *g_audio_new_worker, frames, mixed);
  const UInt32 bytes = static_cast<UInt32>(mixed.size() * sizeof(int16_t));
  std::memcpy(buffer->mAudioData, mixed.data(), bytes);
  buffer->mAudioDataByteSize = bytes;
  return true;
}

static void audio_queue_output_callback(void*, AudioQueueRef, AudioQueueBufferRef buffer) {
  if (!g_running.load())
    return;
  if (fill_audioqueue_buffer(buffer))
    AudioQueueEnqueueBuffer(g_audio_queue, buffer, 0, nullptr);
  else
    g_running = false;
}

static bool play_audio(Worker& old_worker, Worker& new_worker) {
  constexpr uint32_t kQueueBuffers = 3;
  AudioStreamBasicDescription fmt = {};
  fmt.mSampleRate = kSampleRate;
  fmt.mFormatID = kAudioFormatLinearPCM;
  fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
  fmt.mChannelsPerFrame = 2;
  fmt.mBitsPerChannel = 16;
  fmt.mBytesPerFrame = 4;
  fmt.mBytesPerPacket = 4;
  fmt.mFramesPerPacket = 1;
  OSStatus err = AudioQueueNewOutput(&fmt, audio_queue_output_callback, nullptr, nullptr, nullptr, 0, &g_audio_queue);
  if (err != noErr) {
    std::fprintf(stderr, "libvgm_ab_player: AudioQueueNewOutput failed: %d\n", static_cast<int>(err));
    return false;
  }

  const UInt32 buffer_bytes = static_cast<UInt32>(old_worker.header.frames_per_block * 2 * sizeof(int16_t));
  g_audio_old_worker = &old_worker;
  g_audio_new_worker = &new_worker;
  AudioQueueBufferRef buffers[kQueueBuffers] = {};
  uint32_t primed = 0;
  for (uint32_t i = 0; i < kQueueBuffers; ++i) {
    err = AudioQueueAllocateBuffer(g_audio_queue, buffer_bytes, &buffers[i]);
    if (err != noErr) {
      std::fprintf(stderr, "libvgm_ab_player: AudioQueueAllocateBuffer failed: %d\n", static_cast<int>(err));
      AudioQueueDispose(g_audio_queue, true);
      g_audio_queue = nullptr;
      return false;
    }
    if (!fill_audioqueue_buffer(buffers[i]))
      break;
    err = AudioQueueEnqueueBuffer(g_audio_queue, buffers[i], 0, nullptr);
    if (err != noErr) {
      std::fprintf(stderr, "libvgm_ab_player: AudioQueueEnqueueBuffer failed: %d\n", static_cast<int>(err));
      break;
    }
    ++primed;
  }
  if (primed == 0) {
    std::fprintf(stderr, "libvgm_ab_player: no audio buffers were primed\n");
    AudioQueueDispose(g_audio_queue, true);
    g_audio_queue = nullptr;
    g_audio_old_worker = nullptr;
    g_audio_new_worker = nullptr;
    return false;
  }

  err = AudioQueueStart(g_audio_queue, nullptr);
  if (err != noErr) {
    std::fprintf(stderr, "libvgm_ab_player: AudioQueueStart failed: %d\n", static_cast<int>(err));
    AudioQueueDispose(g_audio_queue, true);
    g_audio_queue = nullptr;
    return false;
  }

  while (g_running.load()) usleep(10000);
  AudioQueueStop(g_audio_queue, true);
  AudioQueueDispose(g_audio_queue, true);
  g_audio_queue = nullptr;
  g_audio_old_worker = nullptr;
  g_audio_new_worker = nullptr;
  return true;
}
#elif defined(SSFPLAY_HAVE_ALSA)
static bool play_audio(Worker& old_worker, Worker& new_worker) {
  snd_pcm_t* pcm = nullptr;
  if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0)
    return false;
  if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                         2, kSampleRate, 1, 100000) < 0) {
    snd_pcm_close(pcm);
    return false;
  }
  std::vector<int16_t> mixed;
  while (g_running.load()) {
    if (!read_block(old_worker) || !read_block(new_worker)) break;
    const uint32_t frames = std::max(old_worker.frames, new_worker.frames);
    if (!frames) break;
    mix_blocks(old_worker, new_worker, frames, mixed);
    size_t offset = 0;
    while (offset < frames && g_running.load()) {
      snd_pcm_sframes_t wrote = snd_pcm_writei(pcm, mixed.data() + offset * 2, frames - offset);
      if (wrote < 0) wrote = snd_pcm_recover(pcm, static_cast<int>(wrote), 0);
      if (wrote < 0) break;
      offset += static_cast<size_t>(wrote);
    }
  }
  snd_pcm_drain(pcm);
  snd_pcm_close(pcm);
  return true;
}
#else
static bool play_audio(Worker& old_worker, Worker& new_worker) {
  std::fprintf(stderr, "libvgm_ab_player: no audio backend, use --dump-wav\n");
  std::vector<int16_t> mixed;
  while (g_running.load()) {
    if (!read_block(old_worker) || !read_block(new_worker)) break;
    const uint32_t frames = std::max(old_worker.frames, new_worker.frames);
    if (!frames) break;
    mix_blocks(old_worker, new_worker, frames, mixed);
    std::this_thread::sleep_for(std::chrono::milliseconds(frames * 1000 / kSampleRate));
  }
  return true;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_args(argc, argv, options))
    return 2;

  Worker old_worker;
  Worker new_worker;
  if (!start_worker(old_worker, options.old_worker, options, mute_mask_for_mode(true)) ||
      !start_worker(new_worker, options.new_worker, options, mute_mask_for_mode(false)))
    return 1;
  g_control_old_worker = &old_worker;
  g_control_new_worker = &new_worker;
  apply_worker_mutes();

  bool ok = false;
  if (options.slot_stats) {
    ok = print_slot_stats(old_worker, new_worker);
  } else if (!options.dump_wav.empty()) {
    ok = dump_wav(old_worker, new_worker, options.dump_wav);
  } else {
    std::thread controls(control_thread);
    ok = play_audio(old_worker, new_worker);
    g_running = false;
    if (controls.joinable()) controls.detach();
  }

  close_worker(old_worker);
  close_worker(new_worker);
  return ok ? 0 : 1;
}
