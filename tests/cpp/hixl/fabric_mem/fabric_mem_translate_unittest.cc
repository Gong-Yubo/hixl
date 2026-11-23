/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "fabric_mem/fabric_mem_transfer_service.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "fabric_mem/fabric_mem_types.h"

using hixl::AppendRemoteTranslatedOps;
using hixl::FindMappedSegment;
using hixl::RemoteSegmentEntry;
using hixl::Status;
using hixl::SUCCESS;
using hixl::TransferOpDesc;
using hixl::VaInfo;

namespace {

// Reference linear implementation (the pre-optimization semantics) for
// differential testing against the binary search.
bool ReferenceFind(const std::unordered_map<uintptr_t, VaInfo> &map, uintptr_t old_addr, uintptr_t &new_addr,
                   size_t &available_len) {
  for (const auto &item : map) {
    const auto &info = item.second;
    if (old_addr < info.va_addr) {
      continue;
    }
    const uintptr_t offset = old_addr - info.va_addr;
    if (offset >= info.len) {
      continue;
    }
    new_addr = item.first + offset;
    available_len = info.len - offset;
    return true;
  }
  return false;
}

std::vector<RemoteSegmentEntry> BuildSegments(const std::unordered_map<uintptr_t, VaInfo> &map) {
  std::vector<RemoteSegmentEntry> segments;
  segments.reserve(map.size());
  for (const auto &[new_va, info] : map) {
    segments.emplace_back(RemoteSegmentEntry{info.va_addr, new_va, info.len});
  }
  std::sort(segments.begin(), segments.end(),
            [](const RemoteSegmentEntry &lhs, const RemoteSegmentEntry &rhs) { return lhs.old_va < rhs.old_va; });
  return segments;
}

// ---------------------------------------------------------------------------
// FindMappedSegment binary search vs reference linear scan (differential).
// ---------------------------------------------------------------------------
TEST(FabricMemTranslateUTest, BinarySearchMatchesLinearScan) {
  const std::unordered_map<uintptr_t, VaInfo> map = {
      {0x5000, {0x3000, 0x1000}},
      {0x9000, {0x1000, 0x1000}},
      {0xA000, {0x2000, 0x500}},
      {0x20000, {0x10000, 0x4000}},
  };
  const auto segments = BuildSegments(map);
  std::mt19937 gen(42);
  std::uniform_int_distribution<uintptr_t> dist(0x0, 0x16000);
  for (int i = 0; i < 10000; ++i) {
    const uintptr_t addr = dist(gen);
    uintptr_t ref_new = 0;
    size_t ref_avail = 0;
    const bool ref_hit = ReferenceFind(map, addr, ref_new, ref_avail);
    const auto *seg = FindMappedSegment(addr, segments);
    if (!ref_hit) {
      EXPECT_EQ(seg, nullptr) << "unexpected hit at " << std::hex << addr;
      continue;
    }
    ASSERT_NE(seg, nullptr) << "missed hit at " << std::hex << addr;
    const size_t offset = addr - seg->old_va;
    EXPECT_EQ(seg->new_va + offset, ref_new);
    EXPECT_EQ(seg->len - offset, ref_avail);
  }
}

TEST(FabricMemTranslateUTest, BinarySearchHandlesBoundariesAndHoles) {
  const std::unordered_map<uintptr_t, VaInfo> map = {
      {0x5000, {0x3000, 0x1000}},   // [0x3000, 0x4000)
      {0x9000, {0x10000, 0x1000}},  // [0x10000, 0x11000) — gap between segments
  };
  const auto segments = BuildSegments(map);
  EXPECT_EQ(FindMappedSegment(0x2FFF, segments), nullptr);  // before first
  EXPECT_NE(FindMappedSegment(0x3000, segments), nullptr);  // first byte
  EXPECT_NE(FindMappedSegment(0x3FFF, segments), nullptr);  // last byte
  EXPECT_EQ(FindMappedSegment(0x4000, segments), nullptr);  // one past end
  EXPECT_EQ(FindMappedSegment(0x8000, segments), nullptr);  // in the hole
  EXPECT_NE(FindMappedSegment(0x10000, segments), nullptr);
  EXPECT_EQ(FindMappedSegment(0x11000, segments), nullptr);  // one past second
  EXPECT_EQ(FindMappedSegment(UINTPTR_MAX, segments), nullptr);
}

// ---------------------------------------------------------------------------
// AppendRemoteTranslatedOps: single segment, cross-segment split, cache reuse,
// and error cases.
// ---------------------------------------------------------------------------
TEST(FabricMemTranslateUTest, SingleSegmentTranslation) {
  const std::unordered_map<uintptr_t, VaInfo> map = {{0x5000, {0x3000, 0x1000}}};
  const auto segments = BuildSegments(map);
  std::vector<TransferOpDesc> out;
  const RemoteSegmentEntry *hit = nullptr;
  ASSERT_EQ(AppendRemoteTranslatedOps({0x100, 0x3800, 0x200}, segments, nullptr, hit, out), SUCCESS);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].local_addr, 0x100u);
  EXPECT_EQ(out[0].remote_addr, 0x5800u);  // 0x5000 + (0x3800 - 0x3000)
  EXPECT_EQ(out[0].len, 0x200u);
  EXPECT_EQ(hit, &segments[0]);
}

TEST(FabricMemTranslateUTest, CrossSegmentSplitPreservesOrderAndAddresses) {
  const std::unordered_map<uintptr_t, VaInfo> map = {
      {0x5000, {0x3000, 0x1000}},
      {0x9000, {0x4000, 0x800}},
  };
  const auto segments = BuildSegments(map);
  std::vector<TransferOpDesc> out;
  const RemoteSegmentEntry *hit = nullptr;
  // [0x3F00, 0x4800) crosses the 0x4000 boundary: first segment 0x100 bytes,
  // second segment the remaining 0x800 bytes.
  ASSERT_EQ(AppendRemoteTranslatedOps({0x100, 0x3F00, 0x900}, segments, nullptr, hit, out), SUCCESS);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].local_addr, 0x100u);
  EXPECT_EQ(out[0].remote_addr, 0x5F00u);  // 0x5000 + (0x3F00 - 0x3000)
  EXPECT_EQ(out[0].len, 0x100u);
  EXPECT_EQ(out[1].local_addr, 0x200u);
  EXPECT_EQ(out[1].remote_addr, 0x9000u);  // 0x9000 + 0
  EXPECT_EQ(out[1].len, 0x800u);
}

TEST(FabricMemTranslateUTest, CacheReuseSkipsBinarySearchForContiguousDescs) {
  const std::unordered_map<uintptr_t, VaInfo> map = {
      {0x5000, {0x3000, 0x1000}},
      {0x9000, {0x4000, 0x800}},
  };
  const auto segments = BuildSegments(map);
  std::vector<TransferOpDesc> out;
  const RemoteSegmentEntry *cached = nullptr;
  // Two contiguous descriptors inside the same segment.
  ASSERT_EQ(AppendRemoteTranslatedOps({0x100, 0x3800, 0x100}, segments, cached, cached, out), SUCCESS);
  ASSERT_EQ(AppendRemoteTranslatedOps({0x200, 0x3900, 0x100}, segments, cached, cached, out), SUCCESS);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].remote_addr, 0x5800u);
  EXPECT_EQ(out[1].remote_addr, 0x5900u);
  // A descriptor in another segment must fall back to the binary search.
  ASSERT_EQ(AppendRemoteTranslatedOps({0x300, 0x4500, 0x100}, segments, cached, cached, out), SUCCESS);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[2].remote_addr, 0x9500u);
}

TEST(FabricMemTranslateUTest, InvalidInputsAreRejected) {
  const std::unordered_map<uintptr_t, VaInfo> map = {{0x5000, {0x3000, 0x1000}}};
  const auto segments = BuildSegments(map);
  std::vector<TransferOpDesc> out;
  const RemoteSegmentEntry *hit = nullptr;
  EXPECT_NE(AppendRemoteTranslatedOps({0x100, 0x2000, 0x100}, segments, nullptr, hit, out),
            SUCCESS);  // unregistered remote address
  EXPECT_NE(AppendRemoteTranslatedOps({0x100, 0x3800, 0}, segments, nullptr, hit, out),
            SUCCESS);  // zero length
  EXPECT_NE(AppendRemoteTranslatedOps({UINTPTR_MAX, 0x3800, 0x10}, segments, nullptr, hit, out),
            SUCCESS);  // local address overflow
}

}  // namespace
