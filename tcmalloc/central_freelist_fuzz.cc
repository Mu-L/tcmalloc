// Copyright 2020 The TCMalloc Authors
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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "absl/log/check.h"
#include "absl/types/span.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/mock_static_forwarder.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/span_stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {
namespace {

using CentralFreeList = central_freelist_internal::CentralFreeList<
    tcmalloc_internal::MockStaticForwarder>;
using CentralFreelistEnv = FakeCentralFreeListEnvironment<CentralFreeList>;

void FuzzCFL(const std::string& s) {
  const char* data = s.data();
  size_t size = s.size();

  if (size < 11 || size > 100000) {
    // size < 11 for bare minimum fuzz test for a single operation.
    // Avoid overly large inputs as we perform some shuffling and checking.
    return;
  }
  // object_size can be at most kMaxSize.  The current maximum value of kMaxSize
  // is 2^18.  So we use the first 24 bits to set object_size.
  //
  // TODO(b/271282540): Convert these to strongly typed fuzztest parameters.
  const size_t object_size = data[0] | (data[1] << 8) | (data[2] << 16);
  const size_t num_pages = data[3];
  const size_t num_objects_to_move = data[4];
  // data[5]: reserved
  data += 6;
  size -= 6;
  if (!SizeMap::IsValidSizeClass(object_size, num_pages, num_objects_to_move)) {
    return;
  }
  CentralFreelistEnv env(object_size, num_pages, num_objects_to_move);
  std::vector<void*> objects;

  for (int i = 0; i + 5 < size; i += 5) {
    // data[N] : choose the operation.
    const uint8_t op = data[i];
    // We only use data[N+1] right now. data[N+4:N+2] are currently reserved.
    // TODO(271282540): Add support for multiple size classes for fuzzing.
    uint32_t value;
    memcpy(&value, &data[i + 1], sizeof(value));

    switch (op & 0x7) {
      case 0: {
        // Allocate objects.
        // value[7:0] : number of objects to allocate.
        const uint8_t num_objects = value & 0x00FF;
        void* batch[kMaxObjectsToMove];
        const size_t n = num_objects % kMaxObjectsToMove + 1;
        int allocated =
            env.central_freelist().RemoveRange(absl::MakeSpan(batch, n));
        objects.insert(objects.end(), batch, batch + allocated);
        break;
      }
      case 1: {
        // Deallocate objects if number of previously allocated objects is
        // non-empty. value[7:0] : number of objects to deallocate.
        if (objects.empty()) break;

        const uint8_t num_objects = value & 0x00FF;
        const size_t n = std::min<size_t>(num_objects % kMaxObjectsToMove + 1,
                                          objects.size());
        env.central_freelist().InsertRange({&objects[objects.size() - n], n});
        objects.resize(objects.size() - n);
        break;
      }
      case 2: {
        // Shuffle allocated objects such that we don't return them in the
        // same order we allocated them.
        const int seed = value & 0x00FF;
        std::mt19937 rng(seed);
        // Limit number of elements to shuffle so that we don't spend a lot of
        // time in shuffling a large number of objects.
        constexpr int kMaxToShuffle = 10 * kMaxObjectsToMove;
        if (objects.size() <= kMaxToShuffle) {
          std::shuffle(objects.begin(), objects.end(), rng);
        } else {
          std::shuffle(objects.end() - kMaxToShuffle, objects.end(), rng);
        }
        break;
      }
      case 3: {
        // Check stats.
        tcmalloc_internal::SpanStats stats =
            env.central_freelist().GetSpanStats();
        // Spans with objects_per_span = 1 skip most of the logic in the
        // central freelist including stats updates.  So skip the check for
        // objects_per_span = 1.
        if (env.objects_per_span() != 1) {
          CHECK_EQ(env.central_freelist().length() + objects.size(),
                   stats.obj_capacity);
          if (objects.empty()) {
            CHECK_EQ(stats.num_live_spans(), 0);
          } else {
            CHECK_GT(stats.num_live_spans(), 0);
          }
        }
        break;
      }
      case 4: {
        std::string s;
        s.resize(1 << 20);
        Printer p(&s[0], s.size());
        env.central_freelist().PrintSpanUtilStats(p);
        env.central_freelist().PrintSpanLifetimeStats(p);

        PbtxtRegion region(p, kTop);
        env.central_freelist().PrintSpanUtilStatsInPbtxt(region);
        env.central_freelist().PrintSpanLifetimeStatsInPbtxt(region);
        break;
      }
    }
  }

  // Clean up.
  const size_t allocated = objects.size();
  size_t returned = 0;
  while (returned < allocated) {
    const size_t to_return = std::min(allocated - returned, kMaxObjectsToMove);
    env.central_freelist().InsertRange({&objects[returned], to_return});
    returned += to_return;
  }
}

FUZZ_TEST(CentralFreeListTest, FuzzCFL)
    ;

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END
