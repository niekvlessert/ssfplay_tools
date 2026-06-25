#include <ssfplay/ssfplay.h>

#include <atomic>
#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <conio.h>
#elif defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <glob.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#elif defined(SSFPLAY_HAVE_ALSA)
#include <alsa/asoundlib.h>
#include <glob.h>
#include <sys/select.h>
#include <termios.h>
#else
#include <chrono>
#include <glob.h>
#include <sys/select.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#endif

namespace {

std::atomic<bool> g_quit(false);
std::atomic<bool> g_stop_track(false);
std::atomic<bool> g_paused(false);
std::atomic<int> g_nav_delta(0);
ssfplay_decoder* g_decoder = nullptr;

constexpr size_t kFramesPerBuffer = 4096;

void handle_signal(int) {
  g_quit = true;
  g_stop_track = true;
}

bool render_frames(std::vector<int16_t>& pcm, size_t frames, size_t* rendered) {
  pcm.resize(frames * 2);
  while (g_paused && !g_quit && !g_stop_track) {
    std::fill(pcm.begin(), pcm.end(), 0);
    *rendered = frames;
    return true;
  }
  ssfplay_result r = ssfplay_render(g_decoder, pcm.data(), frames, rendered);
  if (r == SSFPLAY_EOF || *rendered == 0) {
    g_stop_track = true;
    return *rendered != 0;
  }
  if (r != SSFPLAY_OK) {
    std::fprintf(stderr, "ssfplay_cli: render error: %s\n", ssfplay_error(g_decoder));
    g_stop_track = true;
    return false;
  }
  return true;
}

void request_next() {
  g_nav_delta = 1;
  g_stop_track = true;
}

void request_previous() {
  g_nav_delta = -1;
  g_stop_track = true;
}

void toggle_pause() {
  const bool paused = !g_paused.load();
  g_paused = paused;
  std::fprintf(stderr, paused ? "\nPaused\n" : "\nResumed\n");
}

void handle_command_char(char c) {
  if (c == 'n' || c == 'N')
    request_next();
  else if (c == 'p' || c == 'P')
    request_previous();
  else if (c == ' ')
    toggle_pause();
  else if (c == 'q' || c == 'Q') {
    g_quit = true;
    g_stop_track = true;
  }
}

#if defined(_WIN32)
void keyboard_thread() {
  while (!g_quit) {
    if (_kbhit()) {
      const int c = _getch();
      if (c >= 0)
        handle_command_char(static_cast<char>(c));
    }
    Sleep(20);
  }
}
#else
class TerminalRawMode {
 public:
  TerminalRawMode() : enabled_(false) {
    if (!isatty(STDIN_FILENO))
      return;
    if (tcgetattr(STDIN_FILENO, &saved_) != 0)
      return;
    termios raw = saved_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
      enabled_ = true;
  }
  ~TerminalRawMode() {
    if (enabled_)
      tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
  }

 private:
  bool enabled_;
  termios saved_ = {};
};

void keyboard_thread() {
  TerminalRawMode raw;
  while (!g_quit) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    timeval tv = {};
    tv.tv_sec = 0;
    tv.tv_usec = 50000;
    const int ready = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
    if (ready > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
      char c = 0;
      if (read(STDIN_FILENO, &c, 1) == 1)
        handle_command_char(c);
    }
  }
}
#endif

bool has_wildcard(const char* path) {
  return std::strchr(path, '*') || std::strchr(path, '?') || std::strchr(path, '[');
}

void append_input(std::vector<std::string>& files, const char* input) {
#if defined(_WIN32)
  if (!has_wildcard(input)) {
    files.emplace_back(input);
    return;
  }
  WIN32_FIND_DATAA data = {};
  HANDLE find = FindFirstFileA(input, &data);
  if (find == INVALID_HANDLE_VALUE) {
    files.emplace_back(input);
    return;
  }
  std::string prefix;
  const char* slash1 = std::strrchr(input, '\\');
  const char* slash2 = std::strrchr(input, '/');
  const char* slash = std::max(slash1, slash2);
  if (slash)
    prefix.assign(input, slash + 1);
  do {
    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      files.push_back(prefix + data.cFileName);
  } while (FindNextFileA(find, &data));
  FindClose(find);
#else
  if (!has_wildcard(input)) {
    files.emplace_back(input);
    return;
  }
  glob_t matches = {};
  if (glob(input, 0, nullptr, &matches) == 0) {
    for (size_t i = 0; i < matches.gl_pathc; ++i)
      files.emplace_back(matches.gl_pathv[i]);
  } else {
    files.emplace_back(input);
  }
  globfree(&matches);
#endif
}

#if defined(_WIN32)
bool play_decoder(uint32_t sample_rate) {
  HWAVEOUT wave = nullptr;
  WAVEFORMATEX fmt = {};
  fmt.wFormatTag = WAVE_FORMAT_PCM;
  fmt.nChannels = 2;
  fmt.nSamplesPerSec = sample_rate;
  fmt.wBitsPerSample = 16;
  fmt.nBlockAlign = 4;
  fmt.nAvgBytesPerSec = sample_rate * fmt.nBlockAlign;
  if (waveOutOpen(&wave, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
    std::fprintf(stderr, "ssfplay_cli: waveOutOpen failed\n");
    return false;
  }
  while (!g_quit && !g_stop_track) {
    if (g_paused) {
      Sleep(20);
      continue;
    }
    std::vector<int16_t> pcm;
    size_t rendered = 0;
    if (!render_frames(pcm, kFramesPerBuffer, &rendered) && !rendered) break;
    WAVEHDR hdr = {};
    hdr.lpData = reinterpret_cast<LPSTR>(pcm.data());
    hdr.dwBufferLength = static_cast<DWORD>(rendered * 4);
    if (waveOutPrepareHeader(wave, &hdr, sizeof(hdr)) != MMSYSERR_NOERROR) break;
    if (waveOutWrite(wave, &hdr, sizeof(hdr)) != MMSYSERR_NOERROR) break;
    while (!g_quit && !g_stop_track && !(hdr.dwFlags & WHDR_DONE)) Sleep(5);
    waveOutUnprepareHeader(wave, &hdr, sizeof(hdr));
  }
  waveOutReset(wave);
  waveOutClose(wave);
  return true;
}
#elif defined(__APPLE__)
AudioQueueRef g_queue = nullptr;

void fill_audioqueue_buffer(AudioQueueBufferRef buffer) {
  std::vector<int16_t> pcm;
  size_t rendered = 0;
  render_frames(pcm, kFramesPerBuffer, &rendered);
  buffer->mAudioDataByteSize = static_cast<UInt32>(rendered * 4);
  if (rendered)
    std::memcpy(buffer->mAudioData, pcm.data(), rendered * 4);
}

void audioqueue_callback(void*, AudioQueueRef, AudioQueueBufferRef buffer) {
  if (g_quit || g_stop_track) return;
  fill_audioqueue_buffer(buffer);
  if (!g_quit && !g_stop_track)
    AudioQueueEnqueueBuffer(g_queue, buffer, 0, nullptr);
}

bool play_decoder(uint32_t sample_rate) {
  AudioStreamBasicDescription fmt = {};
  fmt.mSampleRate = sample_rate;
  fmt.mFormatID = kAudioFormatLinearPCM;
  fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
  fmt.mChannelsPerFrame = 2;
  fmt.mBitsPerChannel = 16;
  fmt.mBytesPerFrame = 4;
  fmt.mBytesPerPacket = 4;
  fmt.mFramesPerPacket = 1;
  OSStatus err = AudioQueueNewOutput(&fmt, audioqueue_callback, nullptr, nullptr, nullptr, 0, &g_queue);
  if (err != noErr) {
    std::fprintf(stderr, "ssfplay_cli: AudioQueueNewOutput failed: %d\n", static_cast<int>(err));
    return false;
  }
  AudioQueueBufferRef buffers[3] = {};
  for (auto& buffer : buffers) {
    err = AudioQueueAllocateBuffer(g_queue, kFramesPerBuffer * 4, &buffer);
    if (err != noErr) return false;
    fill_audioqueue_buffer(buffer);
    AudioQueueEnqueueBuffer(g_queue, buffer, 0, nullptr);
  }
  AudioQueueStart(g_queue, nullptr);
  while (!g_quit && !g_stop_track) usleep(10000);
  AudioQueueStop(g_queue, true);
  AudioQueueDispose(g_queue, true);
  g_queue = nullptr;
  return true;
}
#elif defined(SSFPLAY_HAVE_ALSA)
bool play_decoder(uint32_t sample_rate) {
  snd_pcm_t* pcm_handle = nullptr;
  if (snd_pcm_open(&pcm_handle, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
    std::fprintf(stderr, "ssfplay_cli: unable to open ALSA default device\n");
    return false;
  }
  if (snd_pcm_set_params(pcm_handle, SND_PCM_FORMAT_S16_LE,
                         SND_PCM_ACCESS_RW_INTERLEAVED, 2, sample_rate, 1,
                         100000) < 0) {
    std::fprintf(stderr, "ssfplay_cli: unable to configure ALSA device\n");
    snd_pcm_close(pcm_handle);
    return false;
  }
  while (!g_quit && !g_stop_track) {
    if (g_paused) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    std::vector<int16_t> pcm;
    size_t rendered = 0;
    if (!render_frames(pcm, kFramesPerBuffer, &rendered) && !rendered) break;
    size_t offset = 0;
    while (offset < rendered && !g_quit && !g_stop_track) {
      snd_pcm_sframes_t wrote = snd_pcm_writei(
          pcm_handle, pcm.data() + offset * 2, rendered - offset);
      if (wrote < 0) wrote = snd_pcm_recover(pcm_handle, static_cast<int>(wrote), 0);
      if (wrote < 0) {
        std::fprintf(stderr, "ssfplay_cli: ALSA write failed\n");
        g_stop_track = true;
        break;
      }
      offset += static_cast<size_t>(wrote);
    }
  }
  snd_pcm_drain(pcm_handle);
  snd_pcm_close(pcm_handle);
  return true;
}
#else
bool play_decoder(uint32_t sample_rate) {
  std::fprintf(stderr, "ssfplay_cli: no audio backend was built; rendering silently\n");
  while (!g_quit && !g_stop_track) {
    std::vector<int16_t> pcm;
    size_t rendered = 0;
    if (!render_frames(pcm, kFramesPerBuffer, &rendered) && !rendered) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(rendered * 1000 / sample_rate));
  }
  return true;
}
#endif

void usage(const char* argv0) {
  std::printf("Usage: %s [--rate hz] [--quality 0-10] file.ssf [...]\n"
              "Controls while playing: Space pause/resume, N next, P previous, Q quit\n",
              argv0);
}

}  // namespace

int main(int argc, char** argv) {
  ssfplay_config config;
  ssfplay_config_init(&config);
  int first_file = 1;
  for (; first_file < argc; ++first_file) {
    if (!std::strcmp(argv[first_file], "--help") || !std::strcmp(argv[first_file], "-h")) {
      usage(argv[0]);
      return 0;
    }
    if (!std::strcmp(argv[first_file], "--quality") || !std::strcmp(argv[first_file], "-q")) {
      if (++first_file >= argc) { usage(argv[0]); return 2; }
      config.resampler_quality = static_cast<uint32_t>(std::strtoul(argv[first_file], nullptr, 10));
      continue;
    }
    if (!std::strcmp(argv[first_file], "--rate") || !std::strcmp(argv[first_file], "-r")) {
      if (++first_file >= argc) { usage(argv[0]); return 2; }
      config.sample_rate = static_cast<uint32_t>(std::strtoul(argv[first_file], nullptr, 10));
      continue;
    }
    break;
  }
  if (first_file >= argc) {
    usage(argv[0]);
    return 2;
  }
  std::vector<std::string> files;
  for (int i = first_file; i < argc; ++i)
    append_input(files, argv[i]);
  if (files.empty()) {
    usage(argv[0]);
    return 2;
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  std::thread controls(keyboard_thread);
  for (size_t i = 0; i < files.size() && !g_quit;) {
    ssfplay_decoder* decoder = nullptr;
    ssfplay_result result = ssfplay_open(files[i].c_str(), &config, &decoder);
    if (result != SSFPLAY_OK) {
      std::fprintf(stderr, "ssfplay_cli: %s: %s%s%s\n", files[i].c_str(),
                   ssfplay_result_string(result),
                   ssfplay_last_error()[0] ? ": " : "", ssfplay_last_error());
      ++i;
      continue;
    }
    const char* title = ssfplay_metadata(decoder, SSFPLAY_METADATA_TITLE);
    if (title && title[0])
      std::printf("Playing %zu/%zu: %s (%s)\n",
                  i + 1, files.size(), title, files[i].c_str());
    else
      std::printf("Playing %zu/%zu: %s\n", i + 1, files.size(), files[i].c_str());
    std::printf("Controls: Space pause/resume, N next, P previous, Q quit\n");
    g_decoder = decoder;
    g_stop_track = false;
    g_paused = false;
    g_nav_delta = 0;
    play_decoder(ssfplay_sample_rate(decoder));
    g_decoder = nullptr;
    ssfplay_close(decoder);

    const int delta = g_nav_delta.exchange(0);
    if (g_quit)
      break;
    if (delta < 0) {
      i = (i == 0) ? files.size() - 1 : i - 1;
    } else if (delta > 0) {
      i = (i + 1) % files.size();
    } else {
      ++i;
    }
  }
  g_quit = true;
  if (controls.joinable())
    controls.join();
  return 0;
}
