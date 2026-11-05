// tinykv: command-line front end for the tiny-kv-store library.

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "tinykv/inspect.h"
#include "tinykv/store.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitNotFound = 1;
constexpr int kExitUsage = 2;
constexpr int kExitStoreError = 3;

const char kUsage[] =
    R"(usage: tinykv --dir DIR [options] COMMAND [args]

commands:
  put KEY VALUE              store a value
  get KEY                    print a value (exit 1 if absent)
  del KEY                    delete a key (exit 1 if absent)
  keys [PREFIX]              list keys in sorted order
  scan [PREFIX] [--from KEY] [--to KEY] [--limit N]
                             print "key<TAB>value" lines in sorted order
  batch                      apply "put KEY VALUE" and "del KEY" lines from
                             stdin as one atomic batch; # starts a comment
  stats                      print store statistics
  compact                    rewrite live data and remove older segments
  verify                     check all files without changing them (exit 3 on errors)
  dump [--values]            list every record in every segment

options:
  --dir DIR                  store directory (required)
  --sync always|never|N      fsync per write, never, or every N writes (default always)
  --segment-bytes N          rotate segments at this size
  --salvage                  open despite damage in sealed segments
  --no-hints                 ignore hint files and scan every segment
)";

struct UsageError {
  std::string message;
};

std::uint64_t parse_number(const std::string& text, const std::string& what) {
  const bool digits = !text.empty() && text.find_first_not_of("0123456789") == std::string::npos;
  if (!digits || text.size() > 18) throw UsageError{what + " must be a number: " + text};
  return std::stoull(text);
}

struct Invocation {
  std::string dir;
  tinykv::Options options;
  std::string command;
  std::vector<std::string> args;
};

Invocation parse_arguments(int argc, char** argv) {
  Invocation inv;
  int i = 1;
  const auto value_of = [&](const std::string& flag) -> std::string {
    if (++i >= argc) throw UsageError{flag + " needs a value"};
    return argv[i];
  };
  for (; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--dir") {
      inv.dir = value_of(arg);
    } else if (arg == "--sync") {
      const std::string mode = value_of(arg);
      if (mode == "always") {
        inv.options.sync = tinykv::SyncMode::Always;
      } else if (mode == "never") {
        inv.options.sync = tinykv::SyncMode::Never;
      } else {
        inv.options.sync = tinykv::SyncMode::EveryN;
        inv.options.sync_every = static_cast<std::uint32_t>(parse_number(mode, "--sync"));
      }
    } else if (arg == "--segment-bytes") {
      inv.options.max_segment_bytes = parse_number(value_of(arg), arg);
    } else if (arg == "--salvage") {
      inv.options.salvage = true;
    } else if (arg == "--no-hints") {
      inv.options.use_hints = false;
    } else if (arg.rfind("--", 0) == 0) {
      throw UsageError{"unknown option " + arg};
    } else {
      break;
    }
  }
  if (i >= argc) throw UsageError{"missing command"};
  inv.command = argv[i++];
  for (; i < argc; ++i) inv.args.emplace_back(argv[i]);
  if (inv.dir.empty()) throw UsageError{"--dir is required"};
  return inv;
}

void require_args(const Invocation& inv, std::size_t count) {
  if (inv.args.size() != count) {
    throw UsageError{inv.command + " takes " + std::to_string(count) + " argument(s)"};
  }
}

std::unique_ptr<tinykv::Store> open_store(const Invocation& inv) {
  auto store = tinykv::Store::open(inv.dir, inv.options);
  const tinykv::RecoveryReport& report = store->recovery_report();
  if (report.tail_repair) {
    std::cerr << "warning: truncated " << report.tail_repair->truncated_bytes
              << " bytes from segment " << report.tail_repair->segment_id << " ("
              << report.tail_repair->reason << ")\n";
  }
  for (const auto& salvaged : report.salvaged) {
    std::cerr << "warning: skipped " << salvaged.skipped_bytes << " bytes of segment "
              << salvaged.segment_id << " (" << salvaged.reason << ")\n";
  }
  return store;
}

tinykv::WriteBatch read_batch(std::istream& in) {
  tinykv::WriteBatch batch;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(in, line)) {
    ++line_number;
    std::istringstream fields(line);
    std::string op;
    std::string key;
    if (!(fields >> op) || op[0] == '#') continue;
    if (!(fields >> key)) {
      throw UsageError{"batch line " + std::to_string(line_number) + ": missing key"};
    }
    if (op == "put") {
      std::string value;
      std::getline(fields >> std::ws, value);
      batch.put(key, value);
    } else if (op == "del") {
      batch.del(key);
    } else {
      throw UsageError{"batch line " + std::to_string(line_number) + ": unknown operation " + op};
    }
  }
  return batch;
}

int run_scan(tinykv::Store& store, const std::vector<std::string>& args) {
  tinykv::ScanRange range;
  bool have_prefix = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto next_value = [&]() -> const std::string& {
      if (++i >= args.size()) throw UsageError{"scan option needs a value"};
      return args[i];
    };
    if (args[i] == "--from") {
      range.start = next_value();
    } else if (args[i] == "--to") {
      range.end = next_value();
    } else if (args[i] == "--limit") {
      range.limit = static_cast<std::size_t>(parse_number(next_value(), "--limit"));
    } else if (!have_prefix) {
      range.prefix = args[i];
      have_prefix = true;
    } else {
      throw UsageError{"unexpected scan argument " + args[i]};
    }
  }
  for (auto it = store.scan(range); it.valid(); it.next()) {
    std::cout << it.key() << '\t' << it.value() << '\n';
  }
  return kExitOk;
}

int run_stats(const tinykv::Store& store) {
  const tinykv::Stats s = store.stats();
  std::cout << "live_keys:     " << s.live_keys << '\n'
            << "live_bytes:    " << s.live_bytes << '\n'
            << "garbage_bytes: " << s.garbage_bytes << '\n'
            << "total_bytes:   " << s.total_bytes << '\n'
            << "segments:      " << s.segments << '\n'
            << "active:        " << s.active_segment << '\n'
            << "last_sequence: " << s.last_sequence << '\n';
  return kExitOk;
}

int run_verify(const std::string& dir) {
  const tinykv::VerifyReport report = tinykv::verify_store(dir);
  for (const tinykv::VerifyIssue& issue : report.issues) {
    const bool is_error = issue.severity == tinykv::VerifyIssue::Severity::Error;
    std::cout << (is_error ? "error" : "warning") << ": segment " << issue.segment_id << ": "
              << issue.message << '\n';
  }
  std::cout << report.segments << " segment(s), " << report.records << " records, "
            << report.live_keys << " live keys, " << report.hints_checked << " hints checked: "
            << (report.ok() ? "ok" : "FAILED") << '\n';
  return report.ok() ? kExitOk : kExitStoreError;
}

int dispatch(const Invocation& inv) {
  // These two only read files, so they must not go through open(), which
  // repairs what they are meant to report.
  if (inv.command == "verify") {
    require_args(inv, 0);
    return run_verify(inv.dir);
  }
  if (inv.command == "dump") {
    tinykv::DumpOptions options;
    if (!inv.args.empty()) {
      require_args(inv, 1);
      if (inv.args[0] != "--values") throw UsageError{"unknown dump option " + inv.args[0]};
      options.show_values = true;
    }
    tinykv::dump_store(inv.dir, std::cout, options);
    return kExitOk;
  }

  // Check arguments before open() creates or repairs anything.
  tinykv::WriteBatch batch;
  if (inv.command == "put") {
    require_args(inv, 2);
  } else if (inv.command == "get" || inv.command == "del") {
    require_args(inv, 1);
  } else if (inv.command == "stats" || inv.command == "compact") {
    require_args(inv, 0);
  } else if (inv.command == "batch") {
    require_args(inv, 0);
    batch = read_batch(std::cin);
  } else if (inv.command != "keys" && inv.command != "scan") {
    throw UsageError{"unknown command " + inv.command};
  }
  if (inv.command == "keys" && inv.args.size() > 1) throw UsageError{"keys takes at most a prefix"};

  const auto store = open_store(inv);
  if (inv.command == "put") {
    store->put(inv.args[0], inv.args[1]);
  } else if (inv.command == "get") {
    const auto value = store->get(inv.args[0]);
    if (!value) return kExitNotFound;
    std::cout << *value << '\n';
  } else if (inv.command == "del") {
    if (!store->del(inv.args[0])) return kExitNotFound;
  } else if (inv.command == "keys") {
    for (const std::string& key : store->keys(inv.args.empty() ? "" : inv.args[0])) {
      std::cout << key << '\n';
    }
  } else if (inv.command == "scan") {
    return run_scan(*store, inv.args);
  } else if (inv.command == "batch") {
    store->write(batch);
    std::cout << "applied " << batch.size() << " operations\n";
  } else if (inv.command == "stats") {
    return run_stats(*store);
  } else {
    const tinykv::CompactionStats c = store->compact();
    std::cout << "kept " << c.keys_written << " keys, " << c.bytes_before << " -> " << c.bytes_after
              << " bytes, " << c.segments_removed << " old segment(s) removed\n";
  }
  store->close();
  return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return dispatch(parse_arguments(argc, argv));
  } catch (const UsageError& error) {
    std::cerr << "tinykv: " << error.message << "\n\n" << kUsage;
    return kExitUsage;
  } catch (const tinykv::Error& error) {
    std::cerr << "tinykv: " << error.what() << '\n';
    return error.code() == tinykv::ErrorCode::InvalidArgument ? kExitUsage : kExitStoreError;
  }
}
