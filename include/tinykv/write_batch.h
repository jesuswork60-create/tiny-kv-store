#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tinykv {

// An ordered list of writes applied all-or-nothing by Store::write().
// Later operations on a key win over earlier ones in the same batch.
class WriteBatch {
 public:
  struct Op {
    bool is_put;
    std::string key;
    std::string value;
  };

  void put(std::string_view key, std::string_view value) {
    ops_.push_back(Op{true, std::string(key), std::string(value)});
  }
  void del(std::string_view key) {
    ops_.push_back(Op{false, std::string(key), std::string()});
  }
  void clear() { ops_.clear(); }

  bool empty() const { return ops_.empty(); }
  std::size_t size() const { return ops_.size(); }
  const std::vector<Op>& ops() const { return ops_; }

 private:
  std::vector<Op> ops_;
};

}  // namespace tinykv
