// Runs the real binary through the shell and checks output and exit codes.

#include <sys/wait.h>

#include "check.h"
#include "segment.h"

using namespace testing;

namespace {

struct Result {
  int code;
  std::string out;
  std::string err;
};

std::string quoted(const std::string& text) { return "'" + text + "'"; }

class Cli {
 public:
  Result run(const std::string& args, const std::string& input = "") {
    const std::string in_path = dir_.sub("cli.in");
    write_file(in_path, input);
    return finish(quoted(TINYKV_CLI_PATH) + " --dir " + quoted(store_dir()) + " --sync never " + args +
                  " <" + quoted(in_path));
  }

  // Runs with exactly these arguments, for testing option parsing itself.
  Result run_raw(const std::string& args) {
    return finish(quoted(TINYKV_CLI_PATH) + " " + args + " </dev/null");
  }

  std::string store_dir() const { return dir_.sub("store"); }

 private:
  Result finish(const std::string& command) {
    const std::string out_path = dir_.sub("cli.out");
    const std::string err_path = dir_.sub("cli.err");
    const std::string full = command + " >" + quoted(out_path) + " 2>" + quoted(err_path);
    const int status = std::system(full.c_str());
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, read_file(out_path), read_file(err_path)};
  }

  TempDir dir_;
};

bool contains_text(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("put get del round trip through separate processes") {
  Cli cli;
  CHECK_EQ(cli.run("put city Lisbon").code, 0);
  const Result got = cli.run("get city");
  CHECK_EQ(got.code, 0);
  CHECK_EQ(got.out, std::string("Lisbon\n"));
  CHECK_EQ(cli.run("del city").code, 0);
  CHECK_EQ(cli.run("get city").code, 1);
  CHECK_EQ(cli.run("del city").code, 1);
}

TEST_CASE("keys and scan print in sorted order") {
  Cli cli;
  cli.run("put b 2");
  cli.run("put a 1");
  cli.run("put ab 3");
  cli.run("put c 4");
  CHECK_EQ(cli.run("keys").out, std::string("a\nab\nb\nc\n"));
  CHECK_EQ(cli.run("keys a").out, std::string("a\nab\n"));
  CHECK_EQ(cli.run("scan a").out, std::string("a\t1\nab\t3\n"));
  CHECK_EQ(cli.run("scan --from ab --to c").out, std::string("ab\t3\nb\t2\n"));
  CHECK_EQ(cli.run("scan --limit 1").out, std::string("a\t1\n"));
}

TEST_CASE("batch reads stdin and is all or nothing") {
  Cli cli;
  const Result ok = cli.run("batch", "# a comment\n\nput x hello world\nput y 2\ndel y\n");
  CHECK_EQ(ok.code, 0);
  CHECK_EQ(cli.run("get x").out, std::string("hello world\n"));
  CHECK_EQ(cli.run("get y").code, 1);

  const Result bad = cli.run("batch", "put z 1\nfrobnicate z\n");
  CHECK_EQ(bad.code, 2);
  CHECK(contains_text(bad.err, "batch line 2"));
  CHECK_EQ(cli.run("get z").code, 1);
}

TEST_CASE("stats and compact report sensible numbers") {
  Cli cli;
  cli.run("put a 1");
  cli.run("put a 2");
  const Result before = cli.run("stats");
  CHECK_EQ(before.code, 0);
  CHECK(contains_text(before.out, "live_keys"));
  CHECK_EQ(cli.run("compact").code, 0);
  CHECK(contains_text(cli.run("stats").out, "garbage_bytes"));
  CHECK_EQ(cli.run("get a").out, std::string("2\n"));
}

TEST_CASE("verify exit code follows the store's health") {
  Cli cli;
  CHECK_EQ(cli.run("put a 1").code, 0);
  CHECK_EQ(cli.run("put b 2").code, 0);
  CHECK_EQ(cli.run("verify").code, 0);
  const std::string segment = segment_files(cli.store_dir()).back();

  // The last segment's tail is a warning, not a failure.
  fs::resize_file(segment, fs::file_size(segment) - 1);
  CHECK_EQ(cli.run("verify").code, 0);

  // A second segment makes the damaged one sealed, which is a failure.
  write_file(tinykv::segment_path(cli.store_dir(), 2), "");
  const Result sealed = cli.run("verify");
  CHECK_EQ(sealed.code, 3);
}

TEST_CASE("dump shows records and optionally values") {
  Cli cli;
  cli.run("put a secret");
  CHECK(!contains_text(cli.run("dump").out, "secret"));
  CHECK(contains_text(cli.run("dump --values").out, "value=secret"));
}

TEST_CASE("usage problems exit with 2") {
  Cli cli;
  CHECK_EQ(cli.run("").code, 2);
  CHECK_EQ(cli.run("frobnicate").code, 2);
  CHECK_EQ(cli.run("put only-key").code, 2);
  CHECK_EQ(cli.run("get").code, 2);
  CHECK_EQ(cli.run("scan --limit abc").code, 2);
  CHECK_EQ(cli.run("dump --bogus").code, 2);
  CHECK_EQ(cli.run_raw("put a b").code, 2);  // No --dir.
  CHECK_EQ(cli.run("put '' value").code, 2);  // Empty key, refused by the library.
}

TEST_CASE("sync and segment options are accepted and validated") {
  Cli cli;
  const std::string dir = quoted(cli.store_dir());
  CHECK_EQ(cli.run_raw("--dir " + dir + " --sync always put a 1").code, 0);
  CHECK_EQ(cli.run_raw("--dir " + dir + " --sync 5 put b 2").code, 0);
  CHECK_EQ(cli.run_raw("--dir " + dir + " --sync sometimes put c 3").code, 2);
  CHECK_EQ(cli.run_raw("--dir " + dir + " --segment-bytes 256 --no-hints put d 4").code, 0);
  CHECK_EQ(cli.run_raw("--dir " + dir + " --segment-bytes 1 put e 5").code, 2);
}

TEST_CASE("damage needs --salvage and store errors exit with 3") {
  Cli cli;
  for (int i = 0; i < 12; ++i) {
    const std::string key = "key" + std::to_string(i);
    const Result put = cli.run_raw("--dir " + quoted(cli.store_dir()) +
                                   " --sync never --segment-bytes 128 put " + key + " value");
    CHECK_EQ(put.code, 0);
  }
  const std::vector<std::string> segments = segment_files(cli.store_dir());
  REQUIRE(segments.size() >= 2);
  flip_byte(segments.front(), 40);

  const Result refused = cli.run("--no-hints get key11");
  CHECK_EQ(refused.code, 3);
  const Result salvaged = cli.run("--no-hints --salvage get key11");
  CHECK_EQ(salvaged.code, 0);
  CHECK_EQ(salvaged.out, std::string("value\n"));
  CHECK(contains_text(salvaged.err, "skipped"));
}

TEST_MAIN
