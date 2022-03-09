// Copyright 2022, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//
#pragma once

#include <string_view>

#include "core/core_types.h"

namespace dfly {

// blob strings of upto ~64KB. Small sizes are probably predominant
// for in-mmeory workloads, especially for keys.
// Please note that this class does not have automatic constructors and destructors, therefore
// it requires explicit management.
class SmallString {
  static constexpr unsigned kPrefLen = 10;

 public:

  static void InitThreadLocal(void * heap);

  void Reset() {
    size_ = 0;
  }

  void Assign(std::string_view s);
  void Free();

  bool Equal(std::string_view o) const;
  bool Equal(const SmallString& mps) const;

  uint16_t size() const {
    return size_;
  }

  uint64_t HashCode() const;

  // I am lying here. we should use mi_malloc_usable size really.
  uint16_t MallocUsed() const {
    return size_ >= kPrefLen + 8 ? size_ - kPrefLen : 8;
  }

  void Get(std::string* dest) const;

  // returns 1 or 2 slices representing this small string.
  // Guarantees zero copy, i.e. dest will not point to any of external buffers.
  // With current implementation, it will return 2 slices for a non-empty string.
  unsigned GetV(std::string_view dest[2]) const;

 private:
  // prefix of the string that is broken down into 2 parts.
  char prefix_[kPrefLen];

  uint32_t small_ptr_;  // 32GB capacity because we ignore 3 lsb bits (i.e. x8).
  uint16_t size_;       // uint16_t - total size (including prefix)

} __attribute__((packed));

}  // namespace dfly