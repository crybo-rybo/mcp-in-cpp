// Reads a stream of server-sent events as the HTML standard defines them,
// from parts of any size, as they arrive over HTTP.
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

class EventStream {
public:
  // One event: its type, `message` when the stream named none, and its data,
  // its `data` lines joined by line feeds.
  struct Event {
    std::string_view type;
    std::string_view data;
  };

  // `most` bounds a line and an event's data, so that a stream without line
  // breaks cannot grow without end.
  explicit EventStream(const std::size_t most) noexcept : most_{most} {}

  // Reads the next part of the stream, and hands each event it completes to
  // `deliver`. A line may end with CR, LF or CRLF, and a part may end inside a
  // line, inside a line break, or inside a UTF-8 character. Returns false once
  // a line or an event has grown past the bound, after which the stream reads
  // nothing more.
  template <typename Deliver>
  bool feed(const std::string_view part, Deliver&& deliver) {
    for (const char c : part) {
      if (failed_) {
        return false;
      }
      if (after_cr_) {
        after_cr_ = false;
        if (c == '\n') {
          continue;
        }
      }
      if (c == '\r' || c == '\n') {
        after_cr_ = c == '\r';
        line(deliver);
        line_.clear();
      } else if (line_.size() >= most_) {
        failed_ = true;
      } else {
        line_ += c;
      }
    }
    return !failed_;
  }

  // The last event id the stream set, or empty.
  [[nodiscard]] const std::string& last_id() const noexcept { return id_; }
  // How long the stream asked a client to wait before it reconnects, if it did.
  [[nodiscard]] std::optional<std::chrono::milliseconds> retry() const noexcept {
    return retry_;
  }

private:
  template <typename Deliver> void line(Deliver& deliver) {
    std::string_view text = line_;
    if (first_) {
      first_ = false;
      // A byte order mark may open the stream.
      if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
      }
    }
    if (text.empty()) {
      dispatch(deliver);
      return;
    }
    if (text.front() == ':') {
      // A comment, such as a keepalive.
      return;
    }
    const std::size_t colon = text.find(':');
    const std::string_view field = text.substr(0, colon);
    std::string_view value =
        colon == std::string_view::npos ? std::string_view{} : text.substr(colon + 1);
    if (value.starts_with(' ')) {
      value.remove_prefix(1);
    }
    if (field == "data") {
      if (data_.size() + value.size() + 1 > most_) {
        failed_ = true;
        return;
      }
      data_ += value;
      data_ += '\n';
    } else if (field == "event") {
      type_ = value;
    } else if (field == "id") {
      if (value.find('\0') == std::string_view::npos) {
        id_ = value;
      }
    } else if (field == "retry") {
      std::size_t milliseconds = 0;
      bool digits = !value.empty();
      for (const char c : value) {
        digits = digits && c >= '0' && c <= '9';
        milliseconds = milliseconds * 10 + static_cast<std::size_t>(c - '0');
      }
      if (digits && value.size() <= 9) {
        retry_ = std::chrono::milliseconds{milliseconds};
      }
    }
    // Any other field is ignored.
  }

  template <typename Deliver> void dispatch(Deliver& deliver) {
    if (data_.empty()) {
      type_.clear();
      return;
    }
    data_.pop_back();
    const std::string type = std::exchange(type_, {});
    const std::string data = std::exchange(data_, {});
    deliver(
        Event{.type = type.empty() ? std::string_view{"message"} : type, .data = data});
  }

  std::size_t most_;
  std::string line_;
  std::string data_;
  std::string type_;
  std::string id_;
  std::optional<std::chrono::milliseconds> retry_;
  bool after_cr_{};
  bool first_{true};
  bool failed_{};
};

} // namespace mcp
