// The client's side of the stdio transport over a server it starts as a child
// process (POSIX).
#include "jsonrpc.hpp"
#include "lines.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <exception>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <mcp/channel.hpp>
#include <mcp/client.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

// The environment the client was started with, which a server inherits.
extern char** environ;

namespace mcp {

namespace {

using jsonrpc::join;
using Clock = std::chrono::steady_clock;

[[nodiscard]] ClientError broken(std::string message) {
  return {.fault = Fault::transport,
          .code = 0,
          .message = std::move(message),
          .data = std::nullopt};
}

[[nodiscard]] std::string error_text(const int code) {
  return std::generic_category().message(code);
}

// The most read from a pipe at once.
constexpr std::size_t chunk_size = std::size_t{64} << 10U;

// A file descriptor, closed when it is destroyed.
class Fd {
public:
  Fd() noexcept = default;
  explicit Fd(const int fd) noexcept : fd_{fd} {}
  Fd(Fd&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  ~Fd() { reset(); }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

  void reset() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

private:
  int fd_{-1};
};

struct Pipe {
  Fd read;
  Fd write;
};

// Makes `fd` close on exec, and moves it above the standard streams, so that
// a dup2() onto one of them in the child cannot clobber it. Returns the
// descriptor, or -1.
[[nodiscard]] int keep_private(const int fd) noexcept {
  if (fd > STDERR_FILENO) {
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0) {
      return fd;
    }
    ::close(fd);
    return -1;
  }
  const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
  ::close(fd);
  return moved;
}

// The parts that differ between POSIX systems: how a pipe is made close on
// exec, how a write to a pipe that has no reader is kept from raising SIGPIPE,
// and how the server is kept from inheriting the client's other descriptors.
#if defined(__APPLE__)

// macOS has no pipe2(), so a fork of the program's own between the pipe() and
// the fcntl() may inherit the pipe. Servers the client starts do not: they
// inherit nothing but what the file actions name.
[[nodiscard]] int open_pipe(std::array<int, 2>& fds) noexcept {
  return ::pipe(fds.data());
}

// macOS raises SIGPIPE for a pipe on the process, which any thread may take,
// so blocking it in the writing thread is no help. The descriptor itself can
// be told not to raise it.
[[nodiscard]] int quiet_pipe(const int fd) noexcept {
  return ::fcntl(fd, F_SETNOSIGPIPE, 1) == 0 ? 0 : errno;
}

[[nodiscard]] ssize_t write_quietly(const int fd, const char* const data,
                                    const std::size_t size) noexcept {
  return ::write(fd, data, size);
}

[[nodiscard]] int inherit_nothing_else(posix_spawn_file_actions_t& actions,
                                       short& flags,
                                       const Stderr stderr_output) noexcept {
  flags = static_cast<short>(flags | POSIX_SPAWN_CLOEXEC_DEFAULT);
  // Every descriptor a file action does not name is closed, stderr too.
  return stderr_output == Stderr::inherit
             ? posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO)
             : 0;
}

#else

[[nodiscard]] int open_pipe(std::array<int, 2>& fds) noexcept {
  return ::pipe2(fds.data(), O_CLOEXEC);
}

[[nodiscard]] int quiet_pipe(const int /*fd*/) noexcept { return 0; }

// Linux raises SIGPIPE on the thread that writes, so while it is blocked in
// that thread a write leaves it pending there, to be taken back before it is
// unblocked. How the process handles SIGPIPE does not change.
[[nodiscard]] ssize_t write_quietly(const int fd, const char* const data,
                                    const std::size_t size) noexcept {
  sigset_t pipe_signal;
  sigemptyset(&pipe_signal);
  sigaddset(&pipe_signal, SIGPIPE);
  sigset_t pending;
  sigpending(&pending);
  // A SIGPIPE pending already is someone else's, and stays.
  const bool was_pending = sigismember(&pending, SIGPIPE) == 1;
  sigset_t old_mask;
  pthread_sigmask(SIG_BLOCK, &pipe_signal, &old_mask);
  const ssize_t written = ::write(fd, data, size);
  const int code = errno;
  if (written < 0 && code == EPIPE && !was_pending) {
    const timespec at_once{};
    while (sigtimedwait(&pipe_signal, nullptr, &at_once) < 0 && errno == EINTR) {
    }
  }
  pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
  errno = code;
  return written;
}

[[nodiscard]] int inherit_nothing_else(posix_spawn_file_actions_t& actions,
                                       short& /*flags*/,
                                       const Stderr /*stderr_output*/) noexcept {
  return posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);
}

#endif

// Returns 0, or why the pipe could not be made.
[[nodiscard]] int make_pipe(Pipe& pipe) noexcept {
  std::array<int, 2> fds{-1, -1};
  if (open_pipe(fds) != 0) {
    return errno;
  }
  pipe.read = Fd{keep_private(fds[0])};
  pipe.write = Fd{keep_private(fds[1])};
  return pipe.read.valid() && pipe.write.valid() ? 0 : errno;
}

// The server's process. It is signalled only until it has been reaped, so that
// a signal never reaches a process that took its id over.
class Child {
public:
  explicit Child(const pid_t pid) noexcept : pid_{pid} {}

  // Waits until the process has exited and been reaped, or until `deadline`,
  // or until `give_up` says so. Returns whether it has been reaped.
  template <typename GiveUp>
  bool wait_until(const Clock::time_point deadline, const GiveUp& give_up) {
    // The status of a child has no descriptor to wait on in POSIX, so the
    // wait looks again, at first often and then less so.
    std::chrono::milliseconds pause{1};
    for (;;) {
      if (reap(WNOHANG)) {
        return true;
      }
      const Clock::time_point now = Clock::now();
      if (now >= deadline || give_up()) {
        return false;
      }
      std::this_thread::sleep_for(std::min<Clock::duration>(pause, deadline - now));
      pause = std::min(pause * 2, std::chrono::milliseconds{20});
    }
  }

  // Waits until the process has exited, which it will once it is killed.
  void wait() { reap(0); }

  // Sends `number` to the process group the server leads, and to the server
  // itself if it has left the group, unless it has been reaped.
  void signal(const int number) {
    const std::scoped_lock lock{mutex_};
    if (!reaped_ && (::kill(-pid_, number) != 0 || ::getpgid(pid_) != pid_)) {
      ::kill(pid_, number);
    }
  }

  // How the server ended, once it has been reaped.
  [[nodiscard]] std::string ending(const std::string_view name) {
    const std::scoped_lock lock{mutex_};
    if (!status_.has_value()) {
      return join({name, " has ended"});
    }
    if (WIFEXITED(*status_)) {
      return join(
          {name, " exited with status ", std::to_string(WEXITSTATUS(*status_))});
    }
    if (WIFSIGNALED(*status_)) {
      return join({name, " was killed by signal ", std::to_string(WTERMSIG(*status_))});
    }
    return join({name, " has ended"});
  }

private:
  bool reap(const int options) {
    const std::scoped_lock lock{mutex_};
    while (!reaped_) {
      int status = 0;
      const pid_t got = ::waitpid(pid_, &status, options);
      if (got == pid_) {
        reaped_ = true;
        status_ = status;
      } else if (got < 0 && errno == EINTR) {
        continue;
      } else if (got < 0) {
        // The program reaped it itself, or ignores SIGCHLD: it is gone, and
        // how it ended is not known.
        reaped_ = true;
      } else {
        break;
      }
    }
    return reaped_;
  }

  pid_t pid_;
  std::mutex mutex_;
  bool reaped_{};
  std::optional<int> status_;
};

// The environment of the server: the client's own, with the command's
// variables in place of those of the same name. A later variable of the
// command's replaces an earlier one.
[[nodiscard]] std::vector<std::string>
environment_of(const std::vector<EnvironmentVariable>& added) {
  const auto named = [](const std::string_view name) {
    return
        [name](const EnvironmentVariable& variable) { return variable.name == name; };
  };
  std::vector<std::string> merged;
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string_view text{*entry};
    if (std::ranges::none_of(added, named(text.substr(0, text.find('='))))) {
      merged.emplace_back(text);
    }
  }
  for (auto variable = added.begin(); variable != added.end(); ++variable) {
    if (std::none_of(std::next(variable), added.end(), named(variable->name))) {
      merged.push_back(variable->name + "=" + variable->value);
    }
  }
  return merged;
}

// Pointers to strings, ended by a null pointer, as exec takes them. The strings
// must outlive them.
[[nodiscard]] std::vector<char*> pointers_to(std::vector<std::string>& strings) {
  std::vector<char*> pointers;
  pointers.reserve(strings.size() + 1);
  for (std::string& text : strings) {
    pointers.push_back(text.data());
  }
  pointers.push_back(nullptr);
  return pointers;
}

class Process final : public Channel {
public:
  explicit Process(Command command)
      : command_{std::move(command)},
        grace_{std::max(command_.grace, std::chrono::milliseconds{0})} {}

  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;

  ~Process() override { stop(); }

  Reply<void> open(Incoming& incoming) override {
    if (Reply<void> started = spawn(); !started) {
      return started;
    }
    try {
      reader_ = std::jthread{[this, &incoming] { read(incoming); }};
    } catch (const std::system_error& error) {
      stop();
      return std::unexpected(
          broken(join({"cannot start a thread to read: ", error.what()})));
    }
    return {};
  }

  Reply<void> send(const Outgoing& message) override;

  // Nothing but a notification tells a server on stdio.
  bool abandon(std::string_view /*id*/) override { return false; }

  void close() override { stop(); }

private:
  Reply<void> spawn();
  void read(Incoming& incoming);
  // How the server ended, waiting the grace period for it to exit, or
  // `otherwise` if it does not.
  std::string ended(std::string_view otherwise);
  // Ends the server as the protocol says, and the reader.
  void stop();

  Command command_;
  std::chrono::milliseconds grace_;
  std::optional<Child> child_;
  // A write that blocks on a server that does not read holds it, so closing
  // the server's input waits for it only so long.
  std::timed_mutex writing_;
  // The server's input, and why it can take no more, once it cannot.
  Fd input_;
  std::optional<std::string> refused_;
  // The server's stdout and stderr, which the reader reads.
  Fd output_;
  Fd errors_;
  // Written to when the reader is to stop.
  Pipe wake_;
  std::atomic<bool> closing_{false};
  bool stopped_{};
  // Last, so that it stops before what it reads is destroyed.
  std::jthread reader_;
};

Reply<void> Process::spawn() {
  const std::string& program = command_.command;
  if (program.empty()) {
    return std::unexpected(broken("there is no command to start"));
  }
  const auto failed = [&program](const int code) {
    return std::unexpected(
        broken(join({"cannot start ", program, ": ", error_text(code)})));
  };
  for (const EnvironmentVariable& variable : command_.environment) {
    if (variable.name.empty() || variable.name.find('=') != std::string::npos) {
      return std::unexpected(
          broken(join({"cannot start ", program, ": \"", variable.name,
                       "\" cannot name an environment variable"})));
    }
  }
  if (std::error_code error;
      !command_.directory.empty() &&
      !std::filesystem::is_directory(command_.directory, error)) {
    return std::unexpected(
        broken(join({"cannot start ", program, " in ", command_.directory.string(),
                     ": there is no such directory"})));
  }

  Pipe input;
  Pipe output;
  Pipe errors;
  if (const int code = make_pipe(input); code != 0) {
    return failed(code);
  }
  if (const int code = make_pipe(output); code != 0) {
    return failed(code);
  }
  if (const int code = make_pipe(wake_); code != 0) {
    return failed(code);
  }
  if (command_.stderr_output == Stderr::lines) {
    if (const int code = make_pipe(errors); code != 0) {
      return failed(code);
    }
  }
  if (const int code = quiet_pipe(input.write.get()); code != 0) {
    return failed(code);
  }

  // Cleans up the spawn's attributes and file actions however it goes.
  struct Setup {
    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
    Setup() {
      posix_spawn_file_actions_init(&actions);
      posix_spawnattr_init(&attributes);
    }
    Setup(const Setup&) = delete;
    Setup& operator=(const Setup&) = delete;
    ~Setup() {
      posix_spawnattr_destroy(&attributes);
      posix_spawn_file_actions_destroy(&actions);
    }
  } setup;
  posix_spawn_file_actions_t& actions = setup.actions;
  posix_spawnattr_t& attributes = setup.attributes;

  // The server's own process group, an empty signal mask, and SIGPIPE as a
  // program started from a shell has it, whatever the client's threads do.
  short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
  sigset_t no_signals;
  sigemptyset(&no_signals);
  sigset_t defaults;
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGPIPE);

  const std::string directory = command_.directory.string();
  int code = posix_spawn_file_actions_adddup2(&actions, input.read.get(), STDIN_FILENO);
  if (code == 0) {
    code =
        posix_spawn_file_actions_adddup2(&actions, output.write.get(), STDOUT_FILENO);
  }
  if (code == 0 && command_.stderr_output == Stderr::discard) {
    code = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                            O_WRONLY, 0);
  }
  if (code == 0 && command_.stderr_output == Stderr::lines) {
    code =
        posix_spawn_file_actions_adddup2(&actions, errors.write.get(), STDERR_FILENO);
  }
  if (code == 0 && !directory.empty()) {
    code = posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
  }
  if (code == 0) {
    code = inherit_nothing_else(actions, flags, command_.stderr_output);
  }
  if (code == 0) {
    code = posix_spawnattr_setflags(&attributes, flags);
  }
  if (code == 0) {
    code = posix_spawnattr_setpgroup(&attributes, 0);
  }
  if (code == 0) {
    code = posix_spawnattr_setsigmask(&attributes, &no_signals);
  }
  if (code == 0) {
    code = posix_spawnattr_setsigdefault(&attributes, &defaults);
  }
  if (code != 0) {
    return failed(code);
  }

  std::vector<std::string> arguments;
  arguments.reserve(command_.arguments.size() + 1);
  arguments.push_back(program);
  arguments.insert(arguments.end(), command_.arguments.begin(),
                   command_.arguments.end());
  std::vector<std::string> environment = environment_of(command_.environment);
  const std::vector<char*> argv = pointers_to(arguments);
  const std::vector<char*> envp = pointers_to(environment);

  pid_t pid = 0;
  code = posix_spawnp(&pid, program.c_str(), &actions, &attributes, argv.data(),
                      envp.data());
  if (code != 0) {
    return failed(code);
  }
  child_.emplace(pid);
  // The child's ends of the pipes close here, so that the client sees the end
  // of the server's output when the server closes it.
  input_ = std::move(input.write);
  output_ = std::move(output.read);
  errors_ = std::move(errors.read);
  return {};
}

std::string Process::ended(const std::string_view otherwise) {
  const bool exited =
      child_->wait_until(Clock::now() + grace_, [this] { return closing_.load(); });
  return exited ? child_->ending(command_.command)
                : join({command_.command, otherwise});
}

Reply<void> Process::send(const Outgoing& message) {
  std::string line;
  line.reserve(message.message.size() + 1);
  line.append(message.message);
  line.push_back('\n');

  const std::scoped_lock lock{writing_};
  if (refused_.has_value()) {
    return std::unexpected(broken(*refused_));
  }
  if (!input_.valid()) {
    return std::unexpected(broken("the client is closed"));
  }
  std::string_view rest{line};
  while (!rest.empty()) {
    const ssize_t written = write_quietly(input_.get(), rest.data(), rest.size());
    if (written >= 0) {
      rest.remove_prefix(static_cast<std::size_t>(written));
      continue;
    }
    const int code = errno;
    if (code == EINTR) {
      continue;
    }
    // A server that no longer reads its input has usually exited, and how it
    // did says more than the error does.
    refused_ =
        code == EPIPE
            ? ended(" no longer reads its input")
            : join({"cannot write to ", command_.command, ": ", error_text(code)});
    return std::unexpected(broken(*refused_));
  }
  return {};
}

void Process::read(Incoming& incoming) {
  detail::LineSplitter out_lines{command_.max_line};
  detail::LineSplitter error_lines{command_.max_line};
  // Once the client has been told the channel is gone, it is told nothing more.
  bool told = false;
  const auto end = [&](std::string why) {
    if (!told && !closing_.load()) {
      incoming.closed(broken(std::move(why)));
    }
    told = true;
  };
  const auto message = [&](const std::string_view line) {
    if (!line.empty() && !told && !closing_.load()) {
      incoming.message(line);
    }
  };
  const auto overlong = [&](std::string_view /*start*/) {
    end(join({command_.command, " wrote a line longer than ",
              std::to_string(command_.max_line),
              " bytes, the most this client reads"}));
  };
  const auto log = [this](const std::string_view line) {
    if (command_.stderr_line) {
      try {
        command_.stderr_line(line);
      } catch (...) {
        // The program's own function failed, which is not the server's doing.
        static_cast<void>(std::current_exception());
      }
    }
  };

  std::string chunk(chunk_size, '\0');
  // Reads once from a pipe that poll() found ready. Returns whether it is still
  // open.
  const auto read_from = [&chunk](const Fd& fd, detail::LineSplitter& lines,
                                  const auto& line, const auto& too_long) {
    const ssize_t got = ::read(fd.get(), chunk.data(), chunk.size());
    if (got < 0 && errno == EINTR) {
      return true;
    }
    if (got <= 0) {
      lines.finish(line, too_long);
      return false;
    }
    lines.take(std::string_view{chunk.data(), static_cast<std::size_t>(got)}, line,
               too_long);
    return true;
  };

  bool output_open = true;
  bool errors_open = errors_.valid();
  bool woken = false;
  while ((output_open || errors_open) && !woken) {
    // A negative descriptor is passed over.
    std::array<pollfd, 3> ready{
        {{.fd = wake_.read.get(), .events = POLLIN, .revents = 0},
         {.fd = output_open ? output_.get() : -1, .events = POLLIN, .revents = 0},
         {.fd = errors_open ? errors_.get() : -1, .events = POLLIN, .revents = 0}}};
    if (::poll(ready.data(), static_cast<nfds_t>(ready.size()), -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      end(join({"cannot wait for ", command_.command, ": ", error_text(errno)}));
      return;
    }
    // Stderr first, so that what the server logged before a message comes
    // before it.
    if (ready[2].revents != 0) {
      errors_open = read_from(errors_, error_lines, log, log);
    }
    if (ready[1].revents != 0) {
      output_open = read_from(output_, out_lines, message, overlong);
      if (!output_open) {
        end(ended(" closed its output"));
      }
    }
    woken = ready[0].revents != 0;
  }
  // Once the server has been reaped, what it wrote to stderr before it ended is
  // still in the pipe. That is read without waiting for more, and only so much
  // of it, since a process the server started may hold the pipe open.
  if (woken && errors_open) {
    for (int round = 0; errors_open && round < 16; ++round) {
      pollfd waiting{.fd = errors_.get(), .events = POLLIN, .revents = 0};
      if (::poll(&waiting, 1, 0) <= 0) {
        break;
      }
      errors_open = read_from(errors_, error_lines, log, log);
    }
    if (errors_open) {
      error_lines.finish(log, log);
    }
  }
}

void Process::stop() {
  if (stopped_ || !child_.has_value()) {
    return;
  }
  stopped_ = true;
  closing_.store(true);
  // Closing the server's input asks it to exit.
  std::unique_lock lock{writing_, std::defer_lock};
  const bool closed = lock.try_lock_for(grace_);
  if (closed) {
    input_.reset();
    lock.unlock();
  }
  const auto never = [] { return false; };
  if (!closed || !child_->wait_until(Clock::now() + grace_, never)) {
    child_->signal(SIGTERM);
    if (!child_->wait_until(Clock::now() + grace_, never)) {
      child_->signal(SIGKILL);
      child_->wait();
    }
  }
  if (!closed) {
    // The write that held the input has failed, now that the server is gone.
    lock.lock();
    input_.reset();
    lock.unlock();
  }
  if (reader_.joinable()) {
    constexpr char wake = 'w';
    static_cast<void>(::write(wake_.write.get(), &wake, 1));
    reader_.join();
  }
}

} // namespace

Reply<Client> launch(Command command, ClientOptions options) {
  return Client::open(std::make_unique<Process>(std::move(command)),
                      std::move(options));
}

} // namespace mcp
