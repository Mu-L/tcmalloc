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

#include <cstddef>

#include "fuzztest/fuzztest.h"
#include "absl/log/check.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/mock_central_freelist.h"
#include "tcmalloc/mock_transfer_cache.h"
#include "tcmalloc/transfer_cache_internals.h"
#include "tcmalloc/transfer_cache_stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {
namespace {

using TransferCache =
    internal_transfer_cache::TransferCache<MockCentralFreeList,
                                           FakeTransferCacheManager>;
using TransferCacheEnv = FakeTransferCacheEnvironment<TransferCache>;

void FuzzTransferCache(const std::string& s) {
  const char* data = s.data();
  size_t size = s.size();

  TransferCacheEnv env;
  // TODO(b/271282540): We should also add a capability to fuzz-test multiple
  // size classes.
  constexpr int kBatchSize = TransferCache::Manager::num_objects_to_move(1);
  for (int i = 0; i < size; ++i) {
    switch (data[i] % 10) {
      case 0: {
        const tcmalloc_internal::TransferCacheStats stats =
            env.transfer_cache().GetStats();
        // Confirm that we are always able to grow the cache provided we have
        // sufficient capacity to grow.
        const bool expected = stats.capacity + kBatchSize <= stats.max_capacity;
        CHECK_EQ(env.Grow(), expected);
        break;
      }
      case 1: {
        const tcmalloc_internal::TransferCacheStats stats =
            env.transfer_cache().GetStats();
        // Confirm that we are always able to shrink the cache provided we have
        // sufficient capacity to shrink.
        const bool expected = stats.capacity > kBatchSize;
        CHECK_EQ(env.Shrink(), expected);
        break;
      }
      case 2:
        env.TryPlunder();
        break;
      case 3:
        env.transfer_cache().GetStats();
        break;
      default:
        if (++i < size) {
          int batch = data[i] % 32;
          if (data[i - 1] % 2) {
            env.Insert(batch);
          } else {
            env.Remove(batch);
          }
        }
        break;
    }
  }
}

FUZZ_TEST(TransferCacheTest, FuzzTransferCache)
    ;

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END
