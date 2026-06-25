#include <ssfplay/ssfplay.h>

#include <algorithm>
#include <atomic>
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
#include <sys/wait.h>
#include <unistd.h>
#elif defined(SSFPLAY_HAVE_ALSA)
#include <alsa/asoundlib.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <chrono>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef LIBVGM_AB_OLD_WORKER
#define LIBVGM_AB_OLD_WORKER "libvgm_ab_worker_old"
#endif

namespace {

constexpr uint32_t kMagic = 0x42564741;
constexpr uint32_t kVersion = 2;
constexpr uint32_t kSampleRate = 44100;
constexpr uint32_t kFinalChannels = 2;
constexpr uint32_t kSlotCount = 32;
constexpr uint32_t kFramesPerBlock = 1024;

enum class Monitor : int {
  AB = 0,
  SsfBoth = 1,
  VgmBoth = 2,
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
  std::vector<uint32_t> keyons;
  uint32_t frames = 0;
  bool ended = false;
};

struct SsfSource {
  ssfplay_decoder* decoder = nullptr;
  std::vector<int16_t> samples;
  uint32_t frames = 0;
  bool ended = false;
};

struct Options {
  std::string ssf_input;
  std::string vgm_input;
  std::string dump_wav;
  std::string old_worker = LIBVGM_AB_OLD_WORKER;
  uint32_t loops = 1;
  double fade = 0.0;
  double seconds = 0.0;
  bool vgm_dsp = false;
};

std::atomic<bool> g_running(true);
std::atomic<int> g_monitor(static_cast<int>(Monitor::AB));
std::atomic<int> g_ssf_gain_milli(1000);
std::atomic<int> g_vgm_gain_milli(4000);

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
  const uint32_t riff_size =
      data_bytes > 0xFFFFFFFFull - 36 ? 0xFFFFFFFFu : static_cast<uint32_t>(36 + data_bytes);
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

static bool send_worker_line(Worker* worker, const char* line) {
  if (!worker || !worker->control)
    return false;
  std::lock_guard<std::mutex> lock(worker->control_mutex);
  if (std::fputs(line, worker->control) < 0)
    return false;
  return std::fflush(worker->control) == 0;
}

static bool start_worker(Worker& worker, const Options& options) {
#if defined(_WIN32)
  std::fprintf(stderr, "ssf_vgm_ab_player: live worker control is not implemented on Windows yet\n");
  (void)worker;
  (void)options;
  return false;
#else
  int out_pipe[2] = {-1, -1};
  int in_pipe[2] = {-1, -1};
  if (pipe(out_pipe) != 0 || pipe(in_pipe) != 0) {
    std::fprintf(stderr, "ssf_vgm_ab_player: pipe failed: %s\n", std::strerror(errno));
    return false;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    std::fprintf(stderr, "ssf_vgm_ab_player: fork failed: %s\n", std::strerror(errno));
    return false;
  }

  if (pid == 0) {
    dup2(in_pipe[0], STDIN_FILENO);
    dup2(out_pipe[1], STDOUT_FILENO);
    close(in_pipe[0]);
    close(in_pipe[1]);
    close(out_pipe[0]);
    close(out_pipe[1]);
    if (options.vgm_dsp)
      setenv("LIBVGM_SCSP_FORCE_DSP", "1", 1);
    const std::string loops = std::to_string(options.loops);
    const std::string fade = std::to_string(options.fade);
    execl(options.old_worker.c_str(), options.old_worker.c_str(),
          "--loops", loops.c_str(), "--fade", fade.c_str(),
          "--mute", "00000000", options.vgm_input.c_str(), static_cast<char*>(nullptr));
    std::fprintf(stderr, "ssf_vgm_ab_player: exec failed: %s: %s\n",
                 options.old_worker.c_str(), std::strerror(errno));
    _exit(127);
  }

  close(in_pipe[0]);
  close(out_pipe[1]);
  worker.pid = pid;
  worker.control = fdopen(in_pipe[1], "w");
  worker.pipe = fdopen(out_pipe[0], "r");
  if (!worker.control || !worker.pipe) {
    std::fprintf(stderr, "ssf_vgm_ab_player: fdopen failed\n");
    return false;
  }
#endif

  if (!read_exact(worker.pipe, &worker.header, sizeof(worker.header))) {
    std::fprintf(stderr, "ssf_vgm_ab_player: libvgm worker did not send a header\n");
    return false;
  }
  if (worker.header.magic != kMagic || worker.header.version != kVersion ||
      worker.header.sample_rate != kSampleRate || worker.header.channels != kFinalChannels ||
      worker.header.slot_count != kSlotCount) {
    std::fprintf(stderr, "ssf_vgm_ab_player: incompatible libvgm worker stream\n");
    return false;
  }
  worker.samples.resize(worker.header.frames_per_block * (kFinalChannels + kSlotCount));
  worker.keyons.resize(kSlotCount);
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

static bool read_vgm_block(Worker& worker) {
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
    std::fprintf(stderr, "ssf_vgm_ab_player: libvgm worker block too large\n");
    return false;
  }
  const size_t values = static_cast<size_t>(header.frames) * (kFinalChannels + kSlotCount);
  if (!read_exact(worker.pipe, worker.samples.data(), values * sizeof(int16_t))) {
    std::fprintf(stderr, "ssf_vgm_ab_player: short libvgm worker block\n");
    return false;
  }
  if (!read_exact(worker.pipe, worker.keyons.data(), worker.keyons.size() * sizeof(uint32_t))) {
    std::fprintf(stderr, "ssf_vgm_ab_player: short libvgm key-on block\n");
    return false;
  }
  worker.frames = header.frames;
  return true;
}

static bool read_ssf_block(SsfSource& source, uint32_t frames) {
  if (source.ended) {
    source.frames = 0;
    return true;
  }
  source.samples.assign(static_cast<size_t>(frames) * 2, 0);
  size_t rendered = 0;
  const ssfplay_result result =
      ssfplay_render(source.decoder, source.samples.data(), frames, &rendered);
  source.frames = static_cast<uint32_t>(rendered);
  if (result == SSFPLAY_EOF || rendered == 0) {
    source.ended = true;
    return true;
  }
  if (result != SSFPLAY_OK) {
    std::fprintf(stderr, "ssf_vgm_ab_player: ssf render failed: %s\n",
                 ssfplay_error(source.decoder));
    return false;
  }
  return true;
}

static int32_t ssf_mono(const SsfSource& source, uint32_t frame) {
  if (frame >= source.frames)
    return 0;
  return (static_cast<int32_t>(source.samples[frame * 2 + 0]) +
          source.samples[frame * 2 + 1]) / 2;
}

static int16_t worker_sample_at(const Worker& worker, uint32_t frame, uint32_t index) {
  if (frame >= worker.frames)
    return 0;
  return worker.samples[static_cast<size_t>(frame) * (kFinalChannels + kSlotCount) + index];
}

static int32_t vgm_mono(const Worker& worker, uint32_t frame) {
  return (static_cast<int32_t>(worker_sample_at(worker, frame, 0)) +
          worker_sample_at(worker, frame, 1)) / 2;
}

static void mix_blocks(const SsfSource& ssf, const Worker& vgm,
                       uint32_t frames, std::vector<int16_t>& out) {
  out.resize(static_cast<size_t>(frames) * 2);
  const Monitor monitor = static_cast<Monitor>(g_monitor.load());
  for (uint32_t f = 0; f < frames; ++f) {
    const int32_t ssf_value = apply_gain(ssf_mono(ssf, f), g_ssf_gain_milli.load());
    const int32_t vgm_value = apply_gain(vgm_mono(vgm, f), g_vgm_gain_milli.load());
    int32_t left = ssf_value;
    int32_t right = vgm_value;
    if (monitor == Monitor::SsfBoth)
      left = right = ssf_value;
    else if (monitor == Monitor::VgmBoth)
      left = right = vgm_value;
    out[f * 2 + 0] = clip16(left);
    out[f * 2 + 1] = clip16(right);
  }
}

static bool read_pair(SsfSource& ssf, Worker& vgm, std::vector<int16_t>& mixed) {
  if (!read_vgm_block(vgm))
    return false;
  if (!read_ssf_block(ssf, vgm.header.frames_per_block))
    return false;
  const uint32_t frames = std::max(ssf.frames, vgm.frames);
  if (!frames)
    return true;
  mix_blocks(ssf, vgm, frames, mixed);
  return true;
}

static void print_mode() {
  const double ssf_gain = g_ssf_gain_milli.load() / 1000.0;
  const double vgm_gain = g_vgm_gain_milli.load() / 1000.0;
  const Monitor monitor = static_cast<Monitor>(g_monitor.load());
  std::fprintf(stderr, "\nmode: SSF/Mednafen left %.2fx, original libvgm right %.2fx\n",
               ssf_gain, vgm_gain);
  if (monitor == Monitor::AB)
    std::fprintf(stderr, "monitor: A/B (SSF left, VGM right)\n");
  else if (monitor == Monitor::SsfBoth)
    std::fprintf(stderr, "monitor: SSF/Mednafen in both ears\n");
  else
    std::fprintf(stderr, "monitor: original libvgm in both ears\n");
}

static void control_thread() {
  std::fprintf(stderr,
               "Commands: ab, ssf, vgm, left, right, ssfgain X, vgmgain X, gain SSF VGM, q\n");
  print_mode();
  std::string line;
  while (g_running.load() && std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    for (char& c : cmd)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (cmd == "q" || cmd == "quit") {
      g_running = false;
      break;
    } else if (cmd == "ab" || cmd == "compare") {
      g_monitor = static_cast<int>(Monitor::AB);
    } else if (cmd == "ssf" || cmd == "left") {
      g_monitor = static_cast<int>(Monitor::SsfBoth);
    } else if (cmd == "vgm" || cmd == "right") {
      g_monitor = static_cast<int>(Monitor::VgmBoth);
    } else if (cmd == "ssfgain") {
      double gain = 1.0;
      if (!(in >> gain) || gain < 0.0 || gain > 32.0) {
        std::fprintf(stderr, "Use: ssfgain 0..32\n");
        continue;
      }
      g_ssf_gain_milli = static_cast<int>(gain * 1000.0 + 0.5);
    } else if (cmd == "vgmgain") {
      double gain = 1.0;
      if (!(in >> gain) || gain < 0.0 || gain > 32.0) {
        std::fprintf(stderr, "Use: vgmgain 0..32\n");
        continue;
      }
      g_vgm_gain_milli = static_cast<int>(gain * 1000.0 + 0.5);
    } else if (cmd == "gain") {
      double ssf_gain = 1.0;
      double vgm_gain = 1.0;
      if (!(in >> ssf_gain >> vgm_gain) || ssf_gain < 0.0 || ssf_gain > 32.0 ||
          vgm_gain < 0.0 || vgm_gain > 32.0) {
        std::fprintf(stderr, "Use: gain SSF VGM, with values 0..32\n");
        continue;
      }
      g_ssf_gain_milli = static_cast<int>(ssf_gain * 1000.0 + 0.5);
      g_vgm_gain_milli = static_cast<int>(vgm_gain * 1000.0 + 0.5);
    } else {
      std::fprintf(stderr,
                   "Unknown command. Use: ab, ssf, vgm, left, right, ssfgain X, vgmgain X, gain SSF VGM, q\n");
      continue;
    }
    print_mode();
  }
}

static void usage(const char* argv0) {
  std::fprintf(stderr,
      "Usage: %s [options] input.ssf input.vgm|input.vgz\n"
      "Options:\n"
      "  --dump-wav out.wav       Render comparison to WAV instead of audio device\n"
      "  --ssf-gain x             Left/SSF gain, default 1.0\n"
      "  --vgm-gain x             Right/original-libvgm gain, default 4.0\n"
      "  --monitor ab|ssf|vgm     Initial monitor mode\n"
      "  --old-worker path        Override original libvgm worker path\n"
      "  --vgm-dsp                Enable SCSP DSP in the original-libvgm side\n"
      "  --loops n                libvgm loop count, default 1\n"
      "  --fade seconds           libvgm fade seconds, default 0\n"
      "  --seconds seconds        Stop after this many seconds, useful for quick checks\n",
      argv0);
}

static bool set_monitor_name(const char* name) {
  if (!std::strcmp(name, "ab") || !std::strcmp(name, "compare")) {
    g_monitor = static_cast<int>(Monitor::AB);
    return true;
  }
  if (!std::strcmp(name, "ssf") || !std::strcmp(name, "left")) {
    g_monitor = static_cast<int>(Monitor::SsfBoth);
    return true;
  }
  if (!std::strcmp(name, "vgm") || !std::strcmp(name, "right")) {
    g_monitor = static_cast<int>(Monitor::VgmBoth);
    return true;
  }
  return false;
}

static bool parse_gain(const char* text, std::atomic<int>& gain_milli) {
  const double gain = std::strtod(text, nullptr);
  if (gain < 0.0 || gain > 32.0)
    return false;
  gain_milli = static_cast<int>(gain * 1000.0 + 0.5);
  return true;
}

static bool parse_args(int argc, char** argv, Options& options) {
  std::vector<std::string> positional;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
      usage(argv[0]);
      return false;
    } else if (!std::strcmp(argv[i], "--dump-wav") && i + 1 < argc) {
      options.dump_wav = argv[++i];
    } else if (!std::strcmp(argv[i], "--old-worker") && i + 1 < argc) {
      options.old_worker = argv[++i];
    } else if (!std::strcmp(argv[i], "--vgm-dsp")) {
      options.vgm_dsp = true;
    } else if (!std::strcmp(argv[i], "--ssf-gain") && i + 1 < argc) {
      if (!parse_gain(argv[++i], g_ssf_gain_milli)) {
        usage(argv[0]);
        return false;
      }
    } else if (!std::strcmp(argv[i], "--vgm-gain") && i + 1 < argc) {
      if (!parse_gain(argv[++i], g_vgm_gain_milli)) {
        usage(argv[0]);
        return false;
      }
    } else if (!std::strcmp(argv[i], "--monitor") && i + 1 < argc) {
      if (!set_monitor_name(argv[++i])) {
        usage(argv[0]);
        return false;
      }
    } else if (!std::strcmp(argv[i], "--loops") && i + 1 < argc) {
      options.loops = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (!std::strcmp(argv[i], "--fade") && i + 1 < argc) {
      options.fade = std::strtod(argv[++i], nullptr);
    } else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) {
      options.seconds = std::strtod(argv[++i], nullptr);
      if (options.seconds < 0.0) {
        usage(argv[0]);
        return false;
      }
    } else if (argv[i][0] == '-') {
      usage(argv[0]);
      return false;
    } else {
      positional.emplace_back(argv[i]);
    }
  }
  if (positional.size() != 2) {
    usage(argv[0]);
    return false;
  }
  options.ssf_input = positional[0];
  options.vgm_input = positional[1];
  return true;
}

static bool dump_wav(SsfSource& ssf, Worker& vgm, const Options& options) {
  const std::string& path = options.dump_wav;
  FILE* out = std::fopen(path.c_str(), "wb");
  if (!out) {
    std::fprintf(stderr, "ssf_vgm_ab_player: could not open %s\n", path.c_str());
    return false;
  }
  write_wav_header(out, 0);
  uint64_t written = 0;
  const uint64_t max_frames = options.seconds > 0.0
      ? static_cast<uint64_t>(options.seconds * kSampleRate + 0.5)
      : 0;
  std::vector<int16_t> mixed;
  while (g_running.load()) {
    if (!read_pair(ssf, vgm, mixed)) {
      std::fclose(out);
      return false;
    }
    const uint32_t frames = static_cast<uint32_t>(mixed.size() / 2);
    if (!frames)
      break;
    uint32_t frames_to_write = frames;
    if (max_frames && written + frames_to_write > max_frames)
      frames_to_write = static_cast<uint32_t>(max_frames - written);
    std::fwrite(mixed.data(), 2 * sizeof(int16_t), frames_to_write, out);
    written += frames_to_write;
    if (max_frames && written >= max_frames)
      break;
  }
  std::fseek(out, 0, SEEK_SET);
  write_wav_header(out, written);
  std::fclose(out);
  return true;
}

#if defined(_WIN32)
static bool play_audio(SsfSource&, Worker&) {
  std::fprintf(stderr, "ssf_vgm_ab_player: audio playback is not implemented on Windows yet\n");
  return false;
}
#elif defined(__APPLE__)
AudioQueueRef g_audio_queue = nullptr;
SsfSource* g_audio_ssf = nullptr;
Worker* g_audio_vgm = nullptr;

static bool fill_audioqueue_buffer(AudioQueueBufferRef buffer) {
  if (!g_audio_ssf || !g_audio_vgm)
    return false;
  static thread_local std::vector<int16_t> mixed;
  if (!read_pair(*g_audio_ssf, *g_audio_vgm, mixed))
    return false;
  if (mixed.empty())
    return false;
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

static bool play_audio(SsfSource& ssf, Worker& vgm) {
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
    std::fprintf(stderr, "ssf_vgm_ab_player: AudioQueueNewOutput failed: %d\n", static_cast<int>(err));
    return false;
  }

  g_audio_ssf = &ssf;
  g_audio_vgm = &vgm;
  AudioQueueBufferRef buffers[kQueueBuffers] = {};
  uint32_t primed = 0;
  for (uint32_t i = 0; i < kQueueBuffers; ++i) {
    err = AudioQueueAllocateBuffer(g_audio_queue, kFramesPerBlock * 2 * sizeof(int16_t), &buffers[i]);
    if (err != noErr) {
      std::fprintf(stderr, "ssf_vgm_ab_player: AudioQueueAllocateBuffer failed: %d\n", static_cast<int>(err));
      AudioQueueDispose(g_audio_queue, true);
      g_audio_queue = nullptr;
      return false;
    }
    if (!fill_audioqueue_buffer(buffers[i]))
      break;
    err = AudioQueueEnqueueBuffer(g_audio_queue, buffers[i], 0, nullptr);
    if (err != noErr)
      break;
    ++primed;
  }
  if (!primed) {
    std::fprintf(stderr, "ssf_vgm_ab_player: no audio buffers were primed\n");
    AudioQueueDispose(g_audio_queue, true);
    g_audio_queue = nullptr;
    return false;
  }

  err = AudioQueueStart(g_audio_queue, nullptr);
  if (err != noErr) {
    std::fprintf(stderr, "ssf_vgm_ab_player: AudioQueueStart failed: %d\n", static_cast<int>(err));
    AudioQueueDispose(g_audio_queue, true);
    g_audio_queue = nullptr;
    return false;
  }

  while (g_running.load())
    usleep(10000);
  AudioQueueStop(g_audio_queue, true);
  AudioQueueDispose(g_audio_queue, true);
  g_audio_queue = nullptr;
  g_audio_ssf = nullptr;
  g_audio_vgm = nullptr;
  return true;
}
#elif defined(SSFPLAY_HAVE_ALSA)
static bool play_audio(SsfSource& ssf, Worker& vgm) {
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
    if (!read_pair(ssf, vgm, mixed))
      break;
    const uint32_t frames = static_cast<uint32_t>(mixed.size() / 2);
    if (!frames)
      break;
    size_t offset = 0;
    while (offset < frames && g_running.load()) {
      snd_pcm_sframes_t wrote = snd_pcm_writei(pcm, mixed.data() + offset * 2, frames - offset);
      if (wrote < 0)
        wrote = snd_pcm_recover(pcm, static_cast<int>(wrote), 0);
      if (wrote < 0)
        break;
      offset += static_cast<size_t>(wrote);
    }
  }
  snd_pcm_drain(pcm);
  snd_pcm_close(pcm);
  return true;
}
#else
static bool play_audio(SsfSource& ssf, Worker& vgm) {
  std::fprintf(stderr, "ssf_vgm_ab_player: no audio backend, use --dump-wav\n");
  std::vector<int16_t> mixed;
  while (g_running.load()) {
    if (!read_pair(ssf, vgm, mixed))
      break;
    const uint32_t frames = static_cast<uint32_t>(mixed.size() / 2);
    if (!frames)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(frames * 1000 / kSampleRate));
  }
  return true;
}
#endif

static void handle_signal(int) {
  g_running = false;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_args(argc, argv, options))
    return 2;

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
#if !defined(_WIN32)
  std::signal(SIGPIPE, SIG_IGN);
#endif

  ssfplay_config config;
  ssfplay_config_init(&config);
  config.sample_rate = kSampleRate;
  ssfplay_decoder* decoder = nullptr;
  ssfplay_result result = ssfplay_open(options.ssf_input.c_str(), &config, &decoder);
  if (result != SSFPLAY_OK) {
    std::fprintf(stderr, "ssf_vgm_ab_player: %s: %s%s%s\n",
                 options.ssf_input.c_str(), ssfplay_result_string(result),
                 ssfplay_last_error()[0] ? ": " : "", ssfplay_last_error());
    return 1;
  }

  SsfSource ssf;
  ssf.decoder = decoder;

  Worker vgm;
  bool ok = start_worker(vgm, options);
  if (ok) {
    if (!options.dump_wav.empty()) {
      ok = dump_wav(ssf, vgm, options);
    } else {
      std::thread controls(control_thread);
      ok = play_audio(ssf, vgm);
      g_running = false;
      if (controls.joinable())
        controls.detach();
    }
  }

  close_worker(vgm);
  ssfplay_close(decoder);
  return ok ? 0 : 1;
}
