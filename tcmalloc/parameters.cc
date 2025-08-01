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
#include "tcmalloc/parameters.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/time/time.h"
#include "tcmalloc/common.h"
#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/experiment_config.h"
#include "tcmalloc/huge_page_aware_allocator.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal_malloc_extension.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/page_allocator.h"
#include "tcmalloc/selsan/selsan.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/thread_cache.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

template <typename T>
static constexpr T DefaultOrDebugValue(T default_val, T debug_val) {
#ifdef NDEBUG
  return default_val;
#else
  return debug_val;
#endif
}

// As decide_subrelease() is determined at runtime, we cannot require constant
// initialization for the atomic.  This avoids an initialization order fiasco.
static std::atomic<bool>& hpaa_subrelease_ptr() {
  ABSL_CONST_INIT static absl::once_flag flag;
  ABSL_CONST_INIT static std::atomic<bool> v{false};
  absl::base_internal::LowLevelCallOnce(&flag, [&]() {
    v.store(huge_page_allocator_internal::decide_subrelease(),
            std::memory_order_relaxed);
  });

  return v;
}

// As background_process_actions_enabled_ptr() are determined at runtime, we
// cannot require constant initialization for the atomic.  This avoids an
// initialization order fiasco.
static std::atomic<bool>& background_process_actions_enabled_ptr() {
  ABSL_CONST_INIT static absl::once_flag flag;
  ABSL_CONST_INIT static std::atomic<bool> v{false};
  absl::base_internal::LowLevelCallOnce(
      &flag, [&]() { v.store(true, std::memory_order_relaxed); });
  return v;
}

// As background_process_sleep_interval_ns() are determined at runtime, we
// cannot require constant initialization for the atomic.  This avoids an
// initialization order fiasco.
static std::atomic<int64_t>& background_process_sleep_interval_ns() {
  ABSL_CONST_INIT static absl::once_flag flag;
  ABSL_CONST_INIT static std::atomic<int64_t> v{0};
  absl::base_internal::LowLevelCallOnce(&flag, [&]() {
    v.store(absl::ToInt64Nanoseconds(absl::Seconds(1)),
            std::memory_order_relaxed);
  });
  return v;
}

// As skip_subrelease_short_interval_ns(), and
// skip_subrelease_long_interval_ns() are determined at runtime, we cannot
// require constant initialization for the atomic.  This avoids an
// initialization order fiasco.
//
// Configures short and long intervals to zero by default. We expect to set them
// to the non-zero durations once the feature is no longer experimental.
static std::atomic<int64_t>& skip_subrelease_short_interval_ns() {
  ABSL_CONST_INIT static absl::once_flag flag;
  ABSL_CONST_INIT static std::atomic<int64_t> v{0};
  absl::Duration interval;
#if defined(TCMALLOC_INTERNAL_SMALL_BUT_SLOW)
  interval = absl::ZeroDuration();
#else
  interval = absl::Seconds(60);
#endif

  absl::base_internal::LowLevelCallOnce(&flag, [&]() {
    // clang-format off
    v.store(absl::ToInt64Nanoseconds(interval), std::memory_order_relaxed);
    // clang-format on
  });
  return v;
}

// As usermode_hugepage_collapse_enabled() is determined at runtime, we
// cannot require constant initialization for the atomic. This avoids an
// initialization order fiasco.
static std::atomic<bool>& usermode_hugepage_collapse_enabled() {
  ABSL_CONST_INIT static std::atomic<bool> v{true};
  return v;
}

static std::atomic<int64_t>& skip_subrelease_long_interval_ns() {
  ABSL_CONST_INIT static absl::once_flag flag;
  ABSL_CONST_INIT static std::atomic<int64_t> v{0};
  absl::Duration interval;
#if defined(TCMALLOC_INTERNAL_SMALL_BUT_SLOW)
  interval = absl::ZeroDuration();
#else
  interval = absl::Seconds(300);
#endif

  absl::base_internal::LowLevelCallOnce(&flag, [&]() {
    // clang-format off
    v.store(absl::ToInt64Nanoseconds(interval), std::memory_order_relaxed);
    // clang-format on
  });
  return v;
}

static std::atomic<int64_t>& cache_demand_release_short_interval_ns() {
  ABSL_CONST_INIT static absl::once_flag flag;
  ABSL_CONST_INIT static std::atomic<int64_t> v{0};
  absl::base_internal::LowLevelCallOnce(&flag, [&]() {
    // clang-format off
    v.store(absl::ToInt64Nanoseconds(
#if defined(TCMALLOC_INTERNAL_SMALL_BUT_SLOW)
                absl::ZeroDuration()
#else
                absl::Seconds(10)
#endif
                    ),
            std::memory_order_relaxed);
    // clang-format on
  });
  return v;
}

static std::atomic<int64_t>& cache_demand_release_long_interval_ns() {
  ABSL_CONST_INIT static absl::once_flag flag;
  ABSL_CONST_INIT static std::atomic<int64_t> v{0};
  absl::base_internal::LowLevelCallOnce(&flag, [&]() {
    // clang-format off
    v.store(absl::ToInt64Nanoseconds(
#if defined(TCMALLOC_INTERNAL_SMALL_BUT_SLOW)
                absl::ZeroDuration()
#else
                absl::Seconds(30)
#endif
                    ),
            std::memory_order_relaxed);
    // clang-format on
  });
  return v;
}

uint64_t Parameters::heap_size_hard_limit() {
  return tc_globals.page_allocator().limit(PageAllocator::kHard);
}

void Parameters::set_heap_size_hard_limit(uint64_t value) {
  TCMalloc_Internal_SetHeapSizeHardLimit(value);
}

bool Parameters::hpaa_subrelease() {
  return hpaa_subrelease_ptr().load(std::memory_order_relaxed);
}

void Parameters::set_hpaa_subrelease(bool value) {
  TCMalloc_Internal_SetHPAASubrelease(value);
}

absl::Duration Parameters::huge_cache_release_time() {
  ABSL_CONST_INIT static absl::once_flag flag;
  ABSL_CONST_INIT static std::atomic<int32_t> v{1};
  absl::base_internal::LowLevelCallOnce(&flag, [&]() {
    v.store(IsExperimentActive(
                Experiment::TEST_ONLY_TCMALLOC_HUGE_CACHE_RELEASE_30S)
                ? 30
                : 1,
            std::memory_order_relaxed);
  });
  return absl::Seconds(v.load(std::memory_order_relaxed));
}

ABSL_CONST_INIT std::atomic<MallocExtension::BytesPerSecond>
    Parameters::background_release_rate_(MallocExtension::BytesPerSecond{
        0
    });

ABSL_CONST_INIT std::atomic<int64_t> Parameters::guarded_sampling_interval_(
    5 * kDefaultProfileSamplingInterval);
// TODO(b/285379004):  Remove this opt-out.
ABSL_CONST_INIT std::atomic<bool> Parameters::release_partial_alloc_pages_(
    true);
// TODO(b/328440160):  Remove this opt-out.
ABSL_CONST_INIT std::atomic<bool> Parameters::huge_region_demand_based_release_(
    false);
// TODO(b/123345734): Remove the flag when experimentation is done.
ABSL_CONST_INIT std::atomic<bool> Parameters::resize_size_class_max_capacity_(
    true);
ABSL_CONST_INIT std::atomic<bool> Parameters::huge_cache_demand_based_release_(
    false);
// TODO(b/199203282):  Remove this opt-out.
ABSL_CONST_INIT std::atomic<bool> Parameters::release_pages_from_huge_region_(
    true);
ABSL_CONST_INIT std::atomic<int64_t> Parameters::max_total_thread_cache_bytes_(
    kDefaultOverallThreadCacheSize);
ABSL_CONST_INIT std::atomic<double>
    Parameters::peak_sampling_heap_growth_fraction_(1.1);
ABSL_CONST_INIT std::atomic<bool> Parameters::per_cpu_caches_enabled_(
#if defined(TCMALLOC_DEPRECATED_PERTHREAD)
    false
#else
    true
#endif
);
ABSL_CONST_INIT std::atomic<bool> Parameters::per_cpu_caches_dynamic_slab_(
    true);
ABSL_CONST_INIT std::atomic<tcmalloc::hot_cold_t>
    Parameters::min_hot_access_hint_(kDefaultMinHotAccessHint);
ABSL_CONST_INIT std::atomic<double>
    Parameters::per_cpu_caches_dynamic_slab_grow_threshold_(0.9);
ABSL_CONST_INIT std::atomic<double>
    Parameters::per_cpu_caches_dynamic_slab_shrink_threshold_(0.4);

ABSL_CONST_INIT std::atomic<int64_t> Parameters::profile_sampling_interval_(
    kDefaultProfileSamplingInterval);

ABSL_CONST_INIT std::atomic<bool> Parameters::release_free_swapped_(false);

bool Parameters::background_process_actions_enabled() {
  return background_process_actions_enabled_ptr().load(
      std::memory_order_relaxed);
}

absl::Duration Parameters::background_process_sleep_interval() {
  return absl::Nanoseconds(
      background_process_sleep_interval_ns().load(std::memory_order_relaxed));
}

absl::Duration Parameters::filler_skip_subrelease_short_interval() {
  return absl::Nanoseconds(
      skip_subrelease_short_interval_ns().load(std::memory_order_relaxed));
}

absl::Duration Parameters::filler_skip_subrelease_long_interval() {
  return absl::Nanoseconds(
      skip_subrelease_long_interval_ns().load(std::memory_order_relaxed));
}

absl::Duration Parameters::cache_demand_release_short_interval() {
  return absl::Nanoseconds(
      cache_demand_release_short_interval_ns().load(std::memory_order_relaxed));
}

absl::Duration Parameters::cache_demand_release_long_interval() {
  return absl::Nanoseconds(
      cache_demand_release_long_interval_ns().load(std::memory_order_relaxed));
}

bool Parameters::usermode_hugepage_collapse() {
  return usermode_hugepage_collapse_enabled().load(std::memory_order_relaxed);
}

bool Parameters::sparse_trackers_coarse_longest_free_range() {
  ABSL_CONST_INIT static absl::once_flag flag;
  ABSL_CONST_INIT static std::atomic<bool> v{false};
  absl::base_internal::LowLevelCallOnce(&flag, [&]() {
    v.store(IsExperimentActive(
                Experiment::TEST_ONLY_TCMALLOC_COARSE_LFR_TRACKERS) ||
                IsExperimentActive(Experiment::TCMALLOC_COARSE_LFR_TRACKERS),
            std::memory_order_relaxed);
  });
  return v;
}

int32_t Parameters::max_per_cpu_cache_size() {
  return tc_globals.cpu_cache().CacheLimit();
}

bool TCMalloc_Internal_GetReleaseFreeSwapped() {
  return Parameters::release_free_swapped();
}

int ABSL_ATTRIBUTE_WEAK default_want_disable_dynamic_slabs();

// TODO(b/271475288): remove the default_want_disable_dynamic_slabs opt-out
// some time after 2023-02-01.
static bool want_disable_dynamic_slabs() {
  if (default_want_disable_dynamic_slabs == nullptr) return false;
  return default_want_disable_dynamic_slabs() > 0;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

using tcmalloc::tcmalloc_internal::Parameters;
using tcmalloc::tcmalloc_internal::tc_globals;

extern "C" {

int64_t MallocExtension_Internal_GetProfileSamplingInterval() {
  return Parameters::profile_sampling_interval();
}

void MallocExtension_Internal_SetProfileSamplingInterval(int64_t value) {
  Parameters::set_profile_sampling_interval(value);
}

int64_t MallocExtension_Internal_GetGuardedSamplingInterval() {
  return Parameters::guarded_sampling_interval();
}

void MallocExtension_Internal_SetGuardedSamplingInterval(int64_t value) {
  Parameters::set_guarded_sampling_interval(value);
}

int64_t MallocExtension_Internal_GetMaxTotalThreadCacheBytes() {
  return Parameters::max_total_thread_cache_bytes();
}

void MallocExtension_Internal_SetMaxTotalThreadCacheBytes(int64_t value) {
  Parameters::set_max_total_thread_cache_bytes(value);
}

bool MallocExtension_Internal_GetBackgroundProcessActionsEnabled() {
  return Parameters::background_process_actions_enabled();
}

void MallocExtension_Internal_SetBackgroundProcessActionsEnabled(bool value) {
  TCMalloc_Internal_SetBackgroundProcessActionsEnabled(value);
}

void MallocExtension_Internal_GetBackgroundProcessSleepInterval(
    absl::Duration* ret) {
  *ret = Parameters::background_process_sleep_interval();
}

void MallocExtension_Internal_SetBackgroundProcessSleepInterval(
    absl::Duration value) {
  TCMalloc_Internal_SetBackgroundProcessSleepInterval(value);
}

void MallocExtension_Internal_GetSkipSubreleaseShortInterval(
    absl::Duration* ret) {
  *ret = Parameters::filler_skip_subrelease_short_interval();
}

void MallocExtension_Internal_SetSkipSubreleaseShortInterval(
    absl::Duration value) {
  Parameters::set_filler_skip_subrelease_short_interval(value);
}

void MallocExtension_Internal_GetSkipSubreleaseLongInterval(
    absl::Duration* ret) {
  *ret = Parameters::filler_skip_subrelease_long_interval();
}

void MallocExtension_Internal_SetSkipSubreleaseLongInterval(
    absl::Duration value) {
  Parameters::set_filler_skip_subrelease_long_interval(value);
}

void MallocExtension_Internal_GetCacheDemandReleaseShortInterval(
    absl::Duration* ret) {
  *ret = Parameters::cache_demand_release_short_interval();
}

void MallocExtension_Internal_SetCacheDemandReleaseShortInterval(
    absl::Duration value) {
  Parameters::set_cache_demand_release_short_interval(value);
}

void MallocExtension_Internal_GetCacheDemandReleaseLongInterval(
    absl::Duration* ret) {
  *ret = Parameters::cache_demand_release_long_interval();
}

void MallocExtension_Internal_SetCacheDemandReleaseLongInterval(
    absl::Duration value) {
  Parameters::set_cache_demand_release_long_interval(value);
}

tcmalloc::MallocExtension::BytesPerSecond
MallocExtension_Internal_GetBackgroundReleaseRate() {
  return Parameters::background_release_rate();
}

void MallocExtension_Internal_SetBackgroundReleaseRate(
    tcmalloc::MallocExtension::BytesPerSecond rate) {
  Parameters::set_background_release_rate(rate);
}

void TCMalloc_Internal_SetBackgroundReleaseRate(size_t value) {
  Parameters::background_release_rate_.store(
      static_cast<tcmalloc::MallocExtension::BytesPerSecond>(value));
}

uint64_t TCMalloc_Internal_GetHeapSizeHardLimit() {
  // Under ASan we could get here before globals have been initialized.
  tc_globals.InitIfNecessary();
  return Parameters::heap_size_hard_limit();
}

bool TCMalloc_Internal_GetHPAASubrelease() {
  return Parameters::hpaa_subrelease();
}

bool TCMalloc_Internal_GetReleasePartialAllocPagesEnabled() {
  return Parameters::release_partial_alloc_pages();
}

bool TCMalloc_Internal_GetHugeCacheDemandBasedRelease() {
  return Parameters::huge_cache_demand_based_release();
}

bool TCMalloc_Internal_GetHugeRegionDemandBasedRelease() {
  return Parameters::huge_region_demand_based_release();
}

bool TCMalloc_Internal_GetReleasePagesFromHugeRegionEnabled() {
  return Parameters::release_pages_from_huge_region();
}

bool TCMalloc_Internal_GetUsermodeHugepageCollapse() {
  return Parameters::usermode_hugepage_collapse();
}

bool TCMalloc_Internal_GetResizeSizeClassMaxCapacityEnabled() {
  return Parameters::resize_size_class_max_capacity();
}

double TCMalloc_Internal_GetPeakSamplingHeapGrowthFraction() {
  return Parameters::peak_sampling_heap_growth_fraction();
}

bool TCMalloc_Internal_GetPerCpuCachesEnabled() {
  return Parameters::per_cpu_caches();
}

void TCMalloc_Internal_SetGuardedSamplingInterval(int64_t v) {
  Parameters::guarded_sampling_interval_.store(v, std::memory_order_relaxed);
}

int TCMalloc_Internal_GetSelSanPercent() {
  return tcmalloc::tcmalloc_internal::selsan::SamplingPercent();
}

void TCMalloc_Internal_SetSelSanPercent(int v) {
  tcmalloc::tcmalloc_internal::selsan::SetSamplingPercent(v);
}

// update_lock guards changes via SetHeapSizeHardLimit.
ABSL_CONST_INIT static absl::base_internal::SpinLock update_lock(
    absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY);

void TCMalloc_Internal_SetHeapSizeHardLimit(uint64_t value) {
  // limit == 0 implies no limit.
  value = value > 0 ? value : std::numeric_limits<size_t>::max();
  // Ensure that page allocator is set up.
  tc_globals.InitIfNecessary();

  tcmalloc::tcmalloc_internal::AllocationGuardSpinLockHolder l(&update_lock);

  using tcmalloc::tcmalloc_internal::PageAllocator;
  const size_t old_limit =
      tc_globals.page_allocator().limit(PageAllocator::kHard);
  tc_globals.page_allocator().set_limit(value, PageAllocator::kHard);
  if (value != old_limit) {
    TC_LOG("[tcmalloc] set page heap hard limit to %v bytes", value);
  }
}

void TCMalloc_Internal_SetHPAASubrelease(bool v) {
  tcmalloc::tcmalloc_internal::hpaa_subrelease_ptr().store(
      v, std::memory_order_relaxed);
}

void TCMalloc_Internal_SetReleasePartialAllocPagesEnabled(bool v) {
  Parameters::release_partial_alloc_pages_.store(v, std::memory_order_relaxed);
}

void TCMalloc_Internal_SetHugeCacheDemandBasedRelease(bool v) {
  Parameters::huge_cache_demand_based_release_.store(v,
                                                     std::memory_order_relaxed);
}

void TCMalloc_Internal_SetHugeRegionDemandBasedRelease(bool v) {
  Parameters::huge_region_demand_based_release_.store(
      v, std::memory_order_relaxed);
}

void TCMalloc_Internal_SetUsermodeHugepageCollapse(bool v) {
  tcmalloc::tcmalloc_internal::usermode_hugepage_collapse_enabled().store(
      v, std::memory_order_relaxed);
}

void TCMalloc_Internal_SetReleasePagesFromHugeRegionEnabled(bool v) {
  Parameters::release_pages_from_huge_region_.store(v,
                                                    std::memory_order_relaxed);
}

void TCMalloc_Internal_SetResizeSizeClassMaxCapacityEnabled(bool v) {
  Parameters::resize_size_class_max_capacity_.store(v,
                                                    std::memory_order_relaxed);
}

void TCMalloc_Internal_SetMaxPerCpuCacheSize(int32_t v) {
  tcmalloc::tcmalloc_internal::tc_globals.cpu_cache().SetCacheLimit(v);
}

void TCMalloc_Internal_SetMaxTotalThreadCacheBytes(int64_t v) {
  Parameters::max_total_thread_cache_bytes_.store(v, std::memory_order_relaxed);
  tcmalloc::tcmalloc_internal::ThreadCache::set_overall_thread_cache_size(v);
}

void TCMalloc_Internal_SetPeakSamplingHeapGrowthFraction(double v) {
  Parameters::peak_sampling_heap_growth_fraction_.store(
      v, std::memory_order_relaxed);
}

void TCMalloc_Internal_SetPerCpuCachesEnabled(bool v) {
#if !defined(TCMALLOC_DEPRECATED_PERTHREAD)
  if (!v) {
    TC_LOG(
        "Using per-thread caches requires linking against "
        ":tcmalloc_deprecated_perthread.");
    return;
  }
#endif  // !TCMALLOC_DEPRECATED_PERTHREAD

  TCMalloc_Internal_SetPerCpuCachesEnabledNoBuildRequirement(v);
}

void TCMalloc_Internal_SetPerCpuCachesEnabledNoBuildRequirement(bool v) {
  Parameters::per_cpu_caches_enabled_.store(v, std::memory_order_relaxed);
}

void TCMalloc_Internal_SetProfileSamplingInterval(int64_t v) {
  Parameters::profile_sampling_interval_.store(v, std::memory_order_relaxed);
}

void TCMalloc_Internal_SetBackgroundProcessActionsEnabled(bool v) {
  tcmalloc::tcmalloc_internal::background_process_actions_enabled_ptr().store(
      v, std::memory_order_relaxed);
}

void TCMalloc_Internal_SetBackgroundProcessSleepInterval(absl::Duration v) {
  tcmalloc::tcmalloc_internal::background_process_sleep_interval_ns().store(
      absl::ToInt64Nanoseconds(v), std::memory_order_relaxed);
}

void TCMalloc_Internal_GetHugePageFillerSkipSubreleaseShortInterval(
    absl::Duration* v) {
  *v = Parameters::filler_skip_subrelease_short_interval();
}

void TCMalloc_Internal_SetHugePageFillerSkipSubreleaseShortInterval(
    absl::Duration v) {
  tcmalloc::tcmalloc_internal::skip_subrelease_short_interval_ns().store(
      absl::ToInt64Nanoseconds(v), std::memory_order_relaxed);
}

void TCMalloc_Internal_GetHugePageFillerSkipSubreleaseLongInterval(
    absl::Duration* v) {
  *v = Parameters::filler_skip_subrelease_long_interval();
}

void TCMalloc_Internal_SetHugePageFillerSkipSubreleaseLongInterval(
    absl::Duration v) {
  tcmalloc::tcmalloc_internal::skip_subrelease_long_interval_ns().store(
      absl::ToInt64Nanoseconds(v), std::memory_order_relaxed);
}

void TCMalloc_Internal_GetHugeCacheDemandReleaseShortInterval(
    absl::Duration* v) {
  *v = Parameters::cache_demand_release_short_interval();
}

void TCMalloc_Internal_SetHugeCacheDemandReleaseShortInterval(
    absl::Duration v) {
  tcmalloc::tcmalloc_internal::cache_demand_release_short_interval_ns().store(
      absl::ToInt64Nanoseconds(v), std::memory_order_relaxed);
}

void TCMalloc_Internal_GetHugeCacheDemandReleaseLongInterval(
    absl::Duration* v) {
  *v = Parameters::cache_demand_release_long_interval();
}

void TCMalloc_Internal_SetHugeCacheDemandReleaseLongInterval(absl::Duration v) {
  tcmalloc::tcmalloc_internal::cache_demand_release_long_interval_ns().store(
      absl::ToInt64Nanoseconds(v), std::memory_order_relaxed);
}

bool TCMalloc_Internal_GetPerCpuCachesDynamicSlabEnabled() {
  return Parameters::per_cpu_caches_dynamic_slab_enabled();
}

void TCMalloc_Internal_SetPerCpuCachesDynamicSlabEnabled(bool v) {
  // We only allow disabling dynamic slabs using both the flag and
  // want_disable_dynamic_slabs.
  if (!v && !tcmalloc::tcmalloc_internal::want_disable_dynamic_slabs()) return;
  Parameters::per_cpu_caches_dynamic_slab_.store(v, std::memory_order_relaxed);
}

double TCMalloc_Internal_GetPerCpuCachesDynamicSlabGrowThreshold() {
  return Parameters::per_cpu_caches_dynamic_slab_grow_threshold();
}

void TCMalloc_Internal_SetPerCpuCachesDynamicSlabGrowThreshold(double v) {
  Parameters::per_cpu_caches_dynamic_slab_grow_threshold_.store(
      v, std::memory_order_relaxed);
}

double TCMalloc_Internal_GetPerCpuCachesDynamicSlabShrinkThreshold() {
  return Parameters::per_cpu_caches_dynamic_slab_shrink_threshold();
}

void TCMalloc_Internal_SetPerCpuCachesDynamicSlabShrinkThreshold(double v) {
  Parameters::per_cpu_caches_dynamic_slab_shrink_threshold_.store(
      v, std::memory_order_relaxed);
}

uint8_t TCMalloc_Internal_GetMinHotAccessHint() {
  return static_cast<uint8_t>(Parameters::min_hot_access_hint());
}

void TCMalloc_Internal_SetMinHotAccessHint(uint8_t v) {
  Parameters::min_hot_access_hint_.store(static_cast<tcmalloc::hot_cold_t>(v),
                                         std::memory_order_relaxed);
}

void TCMalloc_Internal_SetReleaseFreeSwapped(bool v) {
  Parameters::release_free_swapped_.store(v, std::memory_order_relaxed);
}

}  // extern "C"

GOOGLE_MALLOC_SECTION_END
