// IntelB60 - Pearl (PRL) miner for Intel Arc GPUs (fork of IAminer)
// Copyright (C) 2026 IAminer contributors. GPL-3.0-or-later (see LICENSE).
//
//   intelb60                 start mining (runs first-time setup if there is no intelb60.json)
//   intelb60 setup           interactive setup: pool, wallet, worker, GPUs, tuning
//   intelb60 tune            re-benchmark the GPUs and save the fastest settings
//   intelb60 devices         list Intel GPUs
//   intelb60 --pool HOST:PORT --wallet WALLET [--worker NAME] ...   run without a config file
#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "engine.hpp"
#include "miner.hpp"
#include "net.hpp"
#include "stratum.hpp"
#include "tune.hpp"
#include "util.hpp"
#include "version.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace intelb60;

namespace {

std::atomic<bool> g_stop{false};

#ifdef _WIN32
BOOL WINAPI ctrl_handler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
    g_stop = true;
    return TRUE;
  }
  return FALSE;
}
// Double-clicked from Explorer: keep the window open so the user can read an error.
bool launched_from_explorer() {
  DWORD pids[4];
  return GetConsoleProcessList(pids, 4) <= 1;
}
#endif

void install_signal_handlers() {
#ifdef _WIN32
  SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
  std::signal(SIGINT, [](int) { g_stop = true; });
  std::signal(SIGTERM, [](int) { g_stop = true; });
#endif
}

void pause_if_needed() {
#ifdef _WIN32
  if (launched_from_explorer()) {
    std::cout << "\nPress Enter to close this window...";
    std::string s;
    std::getline(std::cin, s);
  }
#endif
}

static std::string first_line_of(const char* path);
static std::string kernel_release();
static std::string os_release_pretty();
void banner() {
  std::cout << colored(Color::cyan, "\n"
      " ___ _   _ _____ _____ _       ____   __    ___  \n"
      "|_ _| \\ | |_   _| ____| |     | __ ) / /_  / _ \\ \n"
      " | ||  \\| | | | |  _| | |     |  _ \\| '_ \\| | | |\n"
      " | || |\\  | | | | |___| |___  | |_) | (_) | |_| |\n"
      "|___|_| \\_| |_| |_____|_____| |____/ \\___/ \\___/\n")
            << colored(Color::bold, "  B60 Pearl Tuner " INTELB60_VERSION "\n")
            << "  Optimized for Intel Arc Pro B60 (Battlemage) - Linux\n"
            << colored(Color::gray, "  Fee: 1% (60 s every 100 min) supports this fork's B60 tuning work.\n")
            << colored(Color::gray, "  Disclosed & documented - rebuild from source with the fee call removed to opt out.\n")
            << colored(Color::gray, std::string("  Platform: ") + os_release_pretty() + ", kernel " + kernel_release() + "\n\n");
}

static std::string first_line_of(const char* path) {
  std::ifstream f(path);
  std::string s;
  std::getline(f, s);
  return s;
}
static std::string kernel_release() { return first_line_of("/proc/sys/kernel/osrelease"); }
static std::string os_release_pretty() {
  std::ifstream f("/etc/os-release");
  std::string line;
  while (std::getline(f, line))
    if (line.rfind("PRETTY_NAME=", 0) == 0) {
      std::string v = line.substr(12);
      if (v.size() > 1 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
      return v;
    }
  return "Linux";
}

void usage() {
  std::cout <<
      "Usage: intelb60 [command] [options]\n\n"
      "Commands:\n"
      "  (none)            start mining; runs setup first if there is no config file\n"
      "  setup             interactive setup (pool, wallet, worker, GPUs, tuning)\n"
      "  tune              benchmark each GPU and save the fastest settings\n"
      "  devices           list Intel GPUs\n"
      "  check             log in to the pool and wait for a job (tests pool + wallet, no GPU needed)\n\n"
      "Options:\n"
      "  -o, --pool HOST:PORT   pool address (prefix stratum+ssl:// or use --tls for TLS)\n"
      "  -u, --wallet WALLET    PRL wallet address or pool account name\n"
      "  -w, --worker NAME      worker / rig name (default: computer name)\n"
      "  -p, --pass PASSWORD    pool password (default: x)\n"
      "      --tls / --no-tls   force TLS on or off\n"
      "      --gpus LIST        comma-separated GPU indexes to use (default: all discrete Arc GPUs)\n"
      "  -c, --config FILE      config file (default: intelb60.json next to the program)\n"
      "      --log-file FILE    also write the log to FILE\n"
      "      --no-color         plain output\n"
      "  -v, --verbose          show pool traffic\n"
      "      --quick            faster, coarser tuning\n"
      "  -h, --help             this help\n"
      "      --version          print the version\n";
}

std::string ask(const std::string& question, const std::string& def) {
  std::cout << colored(Color::bold, question);
  if (!def.empty()) std::cout << colored(Color::gray, " [" + def + "]");
  std::cout << ": " << std::flush;
  std::string line;
  if (!std::getline(std::cin, line)) return def;
  while (!line.empty() && std::isspace((unsigned char)line.back())) line.pop_back();
  while (!line.empty() && std::isspace((unsigned char)line.front())) line.erase(line.begin());
  return line.empty() ? def : line;
}

bool ask_yes(const std::string& question, bool def) {
  std::string a = ask(question + (def ? " (Y/n)" : " (y/N)"), "");
  if (a.empty()) return def;
  return a[0] == 'y' || a[0] == 'Y';
}

std::vector<GpuInfo> discrete_gpus(const std::vector<GpuInfo>& all) {
  std::vector<GpuInfo> out;
  for (const auto& g : all) if (!g.integrated) out.push_back(g);
  return out;
}

void print_devices(const std::vector<GpuInfo>& gpus) {
  if (gpus.empty()) {
    std::cout << colored(Color::red, "No Intel GPUs found via Level Zero.\n")
              << "Install the Intel GPU driver (Windows: Arc driver; Linux: intel-opencl-icd + libze-intel-gpu1)\n";
    return;
  }
  for (const auto& g : gpus) {
    char mem[32];
    std::snprintf(mem, sizeof mem, "%.1f GB", (double)g.memory / (1 << 30));
    std::cout << "  GPU" << g.index << "  " << g.name << "  " << mem
              << (g.integrated ? colored(Color::gray, "  (integrated, not used by default)") : "") << "\n";
  }
}

// Apply tuning (or defaults) to every enabled device of cfg.
void tune_devices(Config& cfg, const std::vector<GpuInfo>& gpus, bool quick) {
  for (auto& d : cfg.devices) {
    if (!d.enabled) continue;
    uint64_t mem = 0;
    for (const auto& g : gpus) if (g.index == d.index) mem = g.memory;
    log_line(Color::cyan, "tuning GPU" + std::to_string(d.index) + " (" + d.name + ")" +
                              (quick ? " - quick" : " - about 1-2 minutes"));
    auto results = tune_gpu(d.index, mem, quick, g_stop);
    if (g_stop) return;
    if (results.empty()) {
      d.shape = default_shape(mem);
      log_line(Color::yellow, "  tuning failed; using defaults");
      continue;
    }
    d.shape = results.front().shape;
    d.tuned_rate = results.front().rate;
    log_line(Color::green, "  best: m=" + std::to_string(d.shape.m) + " k=" + std::to_string(d.shape.k) + "  " +
                               format_rate(d.tuned_rate));
  }
}

void fill_devices(Config& cfg, const std::vector<GpuInfo>& gpus, const std::string& gpu_list) {
  std::vector<int> wanted;
  if (!gpu_list.empty()) {
    std::stringstream ss(gpu_list);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
      try { wanted.push_back(std::stoi(tok)); } catch (...) {}
    }
  }
  cfg.devices.clear();
  for (const auto& g : gpus) {
    DeviceConfig d;
    d.index = g.index;
    d.name = g.name;
    d.enabled = wanted.empty() ? !g.integrated
                               : std::find(wanted.begin(), wanted.end(), g.index) != wanted.end();
    d.shape = default_shape(g.memory);
    cfg.devices.push_back(d);
  }
}

bool run_setup(Config& cfg, const std::string& path, bool quick) {
  std::cout << colored(Color::bold, "IntelB60 setup\n") << "Press Enter to accept the value in [brackets].\n\n";
  auto gpus = list_gpus();
  std::cout << "Intel GPUs found:\n";
  print_devices(gpus);
  if (discrete_gpus(gpus).empty()) {
    std::cout << colored(Color::red, "\nNo discrete Intel Arc GPU found; IntelB60 needs one.\n");
    if (gpus.empty()) return false;
    if (!ask_yes("Continue with the integrated GPU anyway?", false)) return false;
  }
  std::cout << "\nPool:\n"
               "  1) Kryptex  - North America (prl-us.kryptex.network:8048, TLS)\n"
               "  2) Kryptex  - Europe        (prl-eu.kryptex.network:8048, TLS)\n"
               "  3) Kryptex  - Asia          (prl-sg.kryptex.network:8048, TLS)\n"
               "  4) HeroMiners               (pearl.herominers.com:1200, TLS)\n"
               "  5) Other pool (enter address)\n";
  std::string choice = ask("Choose a pool", "1");
  if (choice == "1") { cfg.pool = "prl-us.kryptex.network:8048"; cfg.tls = true; }
  else if (choice == "2") { cfg.pool = "prl-eu.kryptex.network:8048"; cfg.tls = true; }
  else if (choice == "3") { cfg.pool = "prl-sg.kryptex.network:8048"; cfg.tls = true; }
  else if (choice == "4") { cfg.pool = "pearl.herominers.com:1200"; cfg.tls = true; }
  else {
    for (;;) {
      std::string url = ask("Pool address (host:port)", cfg.pool);
      std::string host; int port; bool tls_hint;
      if (parse_pool_url(url, host, port, tls_hint)) {
        cfg.pool = url;
        cfg.tls = tls_hint || ask_yes("Use TLS/SSL?", false);
        break;
      }
      std::cout << colored(Color::red, "  expected host:port, e.g. pool.example.com:3333\n");
    }
  }
  bool kryptex = cfg.pool.find("kryptex") != std::string::npos;
  std::cout << "\n" << (kryptex ? "Kryptex: enter your Kryptex username (krx...) or your PRL wallet address.\n"
                                : "Enter the PRL wallet address (or account name) this pool pays.\n");
  for (;;) {
    cfg.wallet = ask("Wallet", cfg.wallet);
    if (!cfg.wallet.empty() && cfg.wallet.find(' ') == std::string::npos) break;
    std::cout << colored(Color::red, "  a wallet is required\n");
  }
  if (cfg.worker.empty()) cfg.worker = hostname();
  cfg.worker = ask("Worker name", cfg.worker);
  cfg.password = ask("Pool password", cfg.password.empty() ? "x" : cfg.password);

  std::cout << "\n";
  fill_devices(cfg, gpus, "");
  int enabled = 0;
  for (const auto& d : cfg.devices) enabled += d.enabled;
  if (cfg.devices.size() > 1 || enabled == 0) {
    std::string def;
    for (const auto& d : cfg.devices) if (d.enabled) def += (def.empty() ? "" : ",") + std::to_string(d.index);
    if (def.empty()) def = "0";
    fill_devices(cfg, gpus, ask("GPUs to mine on (comma-separated)", def));
  }

  if (ask_yes("\nTune your GPUs now for the best hashrate? (recommended, ~1-2 min per GPU)", true))
    tune_devices(cfg, gpus, quick);
  if (g_stop) return false;

  std::string err;
  if (!save_config(path, cfg, err)) {
    std::cout << colored(Color::red, err + "\n");
    return false;
  }
  std::cout << colored(Color::green, "\nSaved " + path + "\n")
            << "Run `intelb60 setup` any time to change these settings.\n\n";
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  std::string command, pool, wallet, worker, pass, gpu_list, config_path, log_file;
  int tls_flag = -1;
  bool no_color = false, verbose = false, quick = false;

  for (size_t i = 0; i < args.size(); i++) {
    const std::string& a = args[i];
    auto next = [&](std::string& out) {
      if (i + 1 >= args.size()) { std::cerr << "missing value for " << a << "\n"; std::exit(2); }
      out = args[++i];
    };
    if (a == "-h" || a == "--help") { usage(); return 0; }
    else if (a == "--version") { std::cout << INTELB60_NAME " " INTELB60_VERSION "\n"; return 0; }
    else if (a == "-o" || a == "--pool" || a == "--url") next(pool);
    else if (a == "-u" || a == "--wallet" || a == "--user") next(wallet);
    else if (a == "-w" || a == "--worker") next(worker);
    else if (a == "-p" || a == "--pass" || a == "--password") next(pass);
    else if (a == "--gpus" || a == "--devices") next(gpu_list);
    else if (a == "-c" || a == "--config") next(config_path);
    else if (a == "--log-file") next(log_file);
    else if (a == "--tls") tls_flag = 1;
    else if (a == "--no-tls") tls_flag = 0;
    else if (a == "--no-color") no_color = true;
    else if (a == "-v" || a == "--verbose") verbose = true;
    else if (a == "--quick") quick = true;
    else if (command.empty() && a[0] != '-') command = a;
    else { std::cerr << "unknown option: " << a << "\n\n"; usage(); return 2; }
  }

  // Level Zero: allow single device allocations above 4 GiB (large matrix shapes). Must be set
  // before the SYCL runtime initialises.
#ifdef _WIN32
  if (!std::getenv("UR_L0_ENABLE_RELAXED_ALLOCATION_LIMITS")) _putenv_s("UR_L0_ENABLE_RELAXED_ALLOCATION_LIMITS", "1");
#else
  setenv("UR_L0_ENABLE_RELAXED_ALLOCATION_LIMITS", "1", 0);
#endif
  console_init(!no_color);
  install_signal_handlers();
  net_global_init();
  banner();
  if (config_path.empty()) config_path = default_config_path();

  try {
    if (command == "devices") {
      print_devices(list_gpus());
      return 0;
    }

    Config cfg;
    bool have_config = std::filesystem::exists(config_path);
    if (have_config) {
      std::string err;
      if (!load_config(config_path, cfg, err)) {
        log_line(Color::red, err);
        pause_if_needed();
        return 2;
      }
    }

    if (command == "setup" || (command.empty() && !have_config && pool.empty())) {
      if (!run_setup(cfg, config_path, quick)) { pause_if_needed(); return 1; }
      if (command == "setup" && !ask_yes("Start mining now?", true)) return 0;
    } else if (command == "tune") {
      auto gpus = list_gpus();
      if (cfg.devices.empty()) fill_devices(cfg, gpus, gpu_list);
      tune_devices(cfg, gpus, quick);
      std::string err;
      if (!g_stop && !save_config(config_path, cfg, err)) log_line(Color::red, err);
      else if (!g_stop) log_line(Color::green, "saved " + config_path);
      return 0;
    } else if (command == "check") {
      // Log in to the pool and wait for a job: verifies pool address, TLS and wallet without a GPU.
      if (!pool.empty()) { cfg.pool = pool; cfg.tls = false; }
      if (!wallet.empty()) cfg.wallet = wallet;
      if (!worker.empty()) cfg.worker = worker;
      if (!pass.empty()) cfg.password = pass;
      if (tls_flag >= 0) cfg.tls = tls_flag == 1;
      if (cfg.worker.empty()) cfg.worker = hostname();
      PoolConfig pc;
      bool tls_hint = false;
      if (cfg.wallet.empty() || !parse_pool_url(cfg.pool, pc.host, pc.port, tls_hint)) {
        log_line(Color::red, "need a pool (host:port) and a wallet: intelb60 check -o HOST:PORT -u WALLET");
        return 2;
      }
      pc.tls = cfg.tls || tls_hint;
      pc.wallet = cfg.wallet;
      pc.worker = cfg.worker;
      pc.password = cfg.password.empty() ? "x" : cfg.password;
      pc.label = "check";
      PoolClient client(pc, {}, verbose);
      client.start();
      auto deadline = Clock::now() + std::chrono::seconds(20);
      while (!g_stop && Clock::now() < deadline && !(client.connected() && client.has_job()))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      bool ok = client.connected() && client.has_job();
      client.stop();
      log_line(ok ? Color::green : Color::red, ok ? "pool check passed: logged in and received a job"
                                                  : "pool check FAILED (see messages above)");
      pause_if_needed();
      return ok ? 0 : 1;
    } else if (!command.empty()) {
      std::cerr << "unknown command: " << command << "\n\n";
      usage();
      return 2;
    }

    // Command-line overrides (also allow running with no config file at all).
    // A pool given on the command line brings its own TLS choice (--tls or a stratum+ssl:// prefix);
    // don't inherit the config file's setting for a different pool.
    if (!pool.empty()) { cfg.pool = pool; cfg.tls = false; }
    if (!wallet.empty()) cfg.wallet = wallet;
    if (!worker.empty()) cfg.worker = worker;
    if (!pass.empty()) cfg.password = pass;
    if (tls_flag >= 0) cfg.tls = tls_flag == 1;
    if (!log_file.empty()) cfg.log_file = log_file;
    if (verbose) cfg.verbose = true;
    if (cfg.worker.empty()) cfg.worker = hostname();
    if (cfg.wallet.empty()) {
      log_line(Color::red, "no wallet configured: run `intelb60 setup` or pass --wallet");
      pause_if_needed();
      return 2;
    }
    if (cfg.devices.empty() || !gpu_list.empty()) {
      auto gpus = list_gpus();
      auto keep = cfg.devices;
      fill_devices(cfg, gpus, gpu_list);
      for (auto& d : cfg.devices)  // keep tuned shapes from the config for GPUs that are still present
        for (const auto& k : keep) if (k.index == d.index && k.name == d.name) { d.shape = k.shape; d.tuned_rate = k.tuned_rate; }
    }
    if (!cfg.log_file.empty()) log_open_file(cfg.log_file);

    log_line(Color::white, "pool " + cfg.pool + (cfg.tls ? " (TLS)" : "") + ", wallet " + cfg.wallet +
                               ", worker " + cfg.worker);
    int rc = run_miner(cfg, g_stop);
    if (rc != 0) pause_if_needed();
    return rc;
  } catch (const std::exception& e) {
    log_line(Color::red, std::string("fatal: ") + e.what());
    pause_if_needed();
    return 1;
  }
}
