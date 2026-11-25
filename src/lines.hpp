// Splits what a server writes into lines, as the stdio channels of a client
// read it, and bounds how long a line may grow, so that a server that never
// ends one cannot make the client hold all it writes.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace mcp::detail {

class LineSplitter {
public:
  // `max` is the longest line taken, in bytes, not counting its line break.
  explicit LineSplitter(const std::size_t max) noexcept : max_{max} {}

  // Takes the next bytes. Calls `line` with each line they complete, without
  // its line break or a carriage return before it, and `overlong` with the
  // first `max` bytes of each line that is longer than that; the rest of such
  // a line is passed over.
  template <typename Line, typename Overlong>
  void take(std::string_view bytes, Line&& line, Overlong&& overlong) {
    while (!bytes.empty()) {
      const std::size_t end = bytes.find('\n');
      const std::string_view piece = bytes.substr(0, end);
      if (skipping_) {
        if (end == std::string_view::npos) {
          return;
        }
        skipping_ = false;
      } else if (end == std::string_view::npos) {
        // One byte more than the bound may be the carriage return of a line
        // that fits.
        if (partial_.size() + piece.size() > max_ + 1) {
          partial_.append(piece.substr(0, max_ + 1 - partial_.size()));
          overlong(std::string_view{partial_}.substr(0, max_));
          partial_.clear();
          skipping_ = true;
          return;
        }
        partial_.append(piece);
        return;
      } else if (partial_.empty()) {
        complete(piece, line, overlong);
      } else {
        partial_.append(piece);
        complete(partial_, line, overlong);
        partial_.clear();
      }
      bytes.remove_prefix(end + 1);
    }
  }

  // Ends the input: a last line without a line break is a line too.
  template <typename Line, typename Overlong>
  void finish(Line&& line, Overlong&& overlong) {
    if (!skipping_ && !partial_.empty()) {
      complete(partial_, line, overlong);
    }
    partial_.clear();
    skipping_ = false;
  }

private:
  template <typename Line, typename Overlong>
  void complete(std::string_view text, Line& line, Overlong& overlong) const {
    if (!text.empty() && text.back() == '\r') {
      text.remove_suffix(1);
    }
    if (text.size() > max_) {
      overlong(text.substr(0, max_));
    } else {
      line(text);
    }
  }

  std::size_t max_;
  std::string partial_;
  // Whether the rest of an overlong line is still to come.
  bool skipping_{};
};

} // namespace mcp::detail
