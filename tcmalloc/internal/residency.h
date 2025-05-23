// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCMALLOC_INTERNAL_RESIDENCY_H_
#define TCMALLOC_INTERNAL_RESIDENCY_H_

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include <optional>
#include <ostream>

#include "absl/status/status.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/range_tracker.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

constexpr int kMaxResidencyBits = 512;
// Abstract base class for Residency, which offers information about memory
// residency: whether or not specific spans of memory are resident in core ("m
// in core"), swapped, or not present.
class Residency {
 public:
  virtual ~Residency() = default;

  struct Info {
    size_t bytes_resident = 0;
    size_t bytes_swapped = 0;
  };

  virtual std::optional<Info> Get(const void* addr, size_t size) = 0;
  virtual inline size_t GetNativePagesInHugePage() const = 0;

  // Struct is ordered with bitmaps first to optimize cacheline usage.
  struct SinglePageBitmaps {
    Bitmap<kMaxResidencyBits> unbacked;
    Bitmap<kMaxResidencyBits> swapped;
    absl::StatusCode status;
  };

  // Using a hugepage-aligned address, parse through /proc/self/pagemap
  // to output two bitmaps - one for pages that are unbacked and one for pages
  // that are swapped. Hugepage-sized regions are assumed to be 2MiB in size. A
  // SinglePageBitmaps struct is returned with the status, the page_unbacked
  // bitmap, and the page_swapped bitmap.
  virtual SinglePageBitmaps GetUnbackedAndSwappedBitmaps(const void* addr) = 0;
};

// Residency offers information about memory residency: whether or not specific
// spans of memory are resident in core ("m in core"), swapped, or not present.
// Originally, this was implemented via the mincore syscall, but has since been
// abstracted to provide more information.
class ResidencyPageMap : public Residency {
 public:
  // This class keeps an open file handle to procfs. Destroy the object to
  // reclaim it.
  ResidencyPageMap();
  ~ResidencyPageMap() override;

  // Query a span of memory starting from `addr` for `size` bytes.
  //
  // We use std::optional for return value as std::optional guarantees that no
  // dynamic memory allocation would happen.  In contrast, absl::StatusOr may
  // dynamically allocate memory when needed.  Using std::optional allows us to
  // use the function in places where memory allocation is prohibited.
  //
  // This is NOT thread-safe. Do not use multiple copies of this class across
  // threads.
  std::optional<Info> Get(const void* addr, size_t size) override;

  // Getter method for kNativePagesInHugePage.
  size_t GetNativePagesInHugePage() const override {
    return kNativePagesInHugePage;
  }

  // Using a hugepage-aligned address, parse through /proc/self/pagemap
  // to output two bitmaps - one for pages that are unbacked and one for pages
  // that are swapped. Hugepage-sized regions are assumed to be 2MiB in size. A
  // SinglePageBitmaps struct is returned with the status, the page_unbacked
  // bitmap, and the page_swapped bitmap.
  SinglePageBitmaps GetUnbackedAndSwappedBitmaps(const void* addr) override;

 private:
  // This helper seeks the internal file to the correct location for the given
  // virtual address.
  absl::StatusCode Seek(uintptr_t vaddr);
  // This helper reads information for a single page. This is useful for the
  // boundaries. It continues the read from the last Seek() or last Read
  // operation.
  std::optional<uint64_t> ReadOne();
  // This helper reads information for `num_pages` worth of _full_ pages and
  // puts the results into `info`. It continues the read from the last Seek() or
  // last Read operation.
  absl::StatusCode ReadMany(int64_t num_pages, Info& info);

  // For testing.
  friend class ResidencySpouse;
  explicit ResidencyPageMap(const char* alternate_filename);

  // Size of the buffer used to gather results.
  static constexpr int kBufferLength = 4096;
  static constexpr int kPagemapEntrySize = 8;
  static constexpr int kEntriesInBuf = kBufferLength / kPagemapEntrySize;

  const size_t kPageSize = GetPageSize();

  static constexpr uintptr_t kHugePageMask = ~(kHugePageSize - 1);
  const size_t kNativePagesInHugePage = kHugePageSize / kPageSize;

  uint64_t buf_[kEntriesInBuf];
  const int fd_;
  const size_t kSizeOfHugepageInPagemap =
      kPagemapEntrySize * kNativePagesInHugePage;
};

inline std::ostream& operator<<(std::ostream& stream,
                                const Residency::Info& rhs) {
  return stream << "{.resident = " << rhs.bytes_resident
                << ", .swapped = " << rhs.bytes_swapped << "}";
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_RESIDENCY_H_
