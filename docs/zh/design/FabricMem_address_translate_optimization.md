# FabricMem 传输地址翻译优化（二分查找 + 连续段缓存）

> 本文描述 `ResolveTransferAddrs` 翻译路径的 O(N) → O(log N)/O(1) 优化。文中类名、函数名与代码路径均对照仓库源码核对。

## 1. 背景与问题

### 1.1 翻译在传输路径中的位置

FabricMem 的 `TransferSync`/`TransferAsync`（host unfold 路径，默认）按如下流程处理一批描述符（desc）：

```
TransferSync(op_descs)
  └─ IssueSyncCopy
       └─ ResolveTransferAddrs           ← 地址翻译（本文优化对象）
       └─ ProcessCopyWithAsync           ← 逐 desc 提交 aclrtMemcpyAsync
```

远程端导出的 FabricMem 段经 `Import` 后被映射到本进程的新 VA。客户端 desc 携带的是**对端注册时的旧 VA**，提交拷贝前必须翻译为 imported 新 VA：

```
old_remote_addr（desc 携带）  →  new_remote_addr（本端 imported VA）
```

### 1.2 原实现的开销

原翻译实现 `FindMappedAddrPrefix` 对 `new_va_to_old_va`（`unordered_map<new_va, VaInfo{old_va, len}>`）做**全表线性扫描**：

```cpp
for (const auto &item : new_va_to_old_va) {          // O(N)，N = 注册段数
  if (old_addr 在 [info.va_addr, info.va_addr+len) 内) { ... }
}
```

unordered_map 按 new_va 组织，无法支持按 old_va 的区间查找，只能逐个遍历。

**实测开销**（yuanrong 64K context GLM-5.3 KV cache 用例，单 load 27027 个 desc、远端注册 16 段）：

| 项 | 数值 |
| --- | --- |
| 单 desc 翻译 | O(16) 次哈希遍历 |
| 单 load 翻译总量 | 27027 × 16 ≈ 43 万次遍历 |
| per-desc 端到端提交开销 | 18~40us（翻译 + `aclrtMemcpyAsync`） |
| 裸 `aclrtMemcpyAsync` 成本（微基准） | ~3.5us |

即 per-desc 开销中翻译全表扫描占大头，导致 load kvc 性能瓶颈（基线 168.7ms，理想收敛目标 ~120ms）。

### 1.3 优化目标

1. 翻译复杂度降为 O(log N)，连续 desc 场景降到 O(1)；
2. 翻译语义（跨段拆分、溢出检查、错误码、日志）与原实现完全一致；
3. 公共 API 与外部行为不变。

## 2. 方案

### 2.1 排序段表

在 `FabricMemTransferContext` 中新增按 old_va 升序的段表，与既有 map **同时构建**（`BuildTransferContext`，每批传输一次，N 很小故 O(N log N) 构建成本可忽略）：

```cpp
struct RemoteSegmentEntry {
  uintptr_t old_va;   // 排序键：对端注册 VA
  uintptr_t new_va;   // 本端 imported VA
  size_t len;         // 段长度
};

struct FabricMemTransferContext {
  std::unordered_map<uintptr_t, VaInfo> remote_va_to_old_va;  // 保留（TransOpAddr 等使用）
  std::vector<RemoteSegmentEntry> remote_segments;            // 新增，按 old_va 升序
  ...
};
```

**数据契约**：`remote_segments` 必须与 `remote_va_to_old_va` 同源同填；手工构造 context 的场景（如单测）需同步填充两表。

### 2.2 二分查找（FindMappedSegment）

old_va 升序表中，最后一个满足 `old_va <= old_addr` 的段是唯一可能包含 `old_addr` 的候选：

```cpp
const RemoteSegmentEntry *FindMappedSegment(uintptr_t old_addr,
                                            const std::vector<RemoteSegmentEntry> &segments) {
  auto it = std::upper_bound(segments.begin(), segments.end(), old_addr,
                             [](uintptr_t addr, const RemoteSegmentEntry &e) { return addr < e.old_va; });
  if (it == segments.begin()) return nullptr;                  // 地址在所有段之前
  const auto &entry = *(it - 1);
  if (old_addr - entry.old_va >= entry.len) return nullptr;    // 落在段间空洞（减法防溢出）
  return &entry;
}
```

### 2.3 连续 desc 段缓存

客户端下发的相邻 desc 通常落在同一注册段内（同一 arena 的连续对象）。`AppendRemoteTranslatedOps` 携带上一次命中段，命中同段时跳过二分：

```cpp
const RemoteSegmentEntry *segment = cached_segment;
if (segment == nullptr || old_remote_addr < segment->old_va
    || old_remote_addr - segment->old_va >= segment->len) {
  segment = FindMappedSegment(old_remote_addr, segments);     // 缓存失效才二分
}
```

### 2.4 语义保持

与原实现逐项对齐：

| 语义 | 处理 |
| --- | --- |
| 单 desc 跨多个注册段 | 按段边界拆分输出多条 translated desc，local_addr 依次累加 |
| `new_va + offset` 溢出 | `segment->new_va <= UINT64_MAX - offset_in_segment` 检查，失败返回 PARAM_INVALID |
| 地址未注册 / len 为 0 / 地址溢出 | PARAM_INVALID（103900），错误日志格式不变 |
| 未命中段 | 返回 nullptr → PARAM_INVALID |

### 2.5 复杂度对比

| 场景 | 原实现 | 优化后 |
| --- | --- | --- |
| 单 desc 翻译 | O(N) 全表线性扫描 | O(log N) 二分 |
| 连续 desc（同段，最常见） | O(N) | O(1) 缓存命中 |
| 单 load 总翻译（27027 desc） | ~43 万次哈希遍历 | ~27027 次缓存判断 + 少量二分 |

## 3. 接口变更

| 接口 | 变更类型 | 说明 |
| --- | --- | --- |
| `FabricMemTransferContext::remote_segments` | 新增字段 | 排序段表；与 `remote_va_to_old_va` 同填 |
| `RemoteSegmentEntry` | 新增结构 | old_va/new_va/len 三元组 |
| `FindMappedSegment` | 新增内部函数 | 二分查找，`namespace hixl`，头文件声明供单测引用 |
| `AppendRemoteTranslatedOps` | 内部函数签名变更 | 参数从 map 改为 `segments + cached_segment + hit_segment`（缓存透传） |
| `ResolveTransferAddrs` | 实现变更 | 循环内维护 `cached_segment` 跨 desc 传递 |
| `BuildTransferContext` | 实现变更 | 填充 map 的同时构建并排序 `remote_segments` |
| `TransOpAddr` | **不变** | public 单地址工具 API，保留原线性实现（非热路径） |

公共 API（`Initialize`/`TransferSync`/`TransferAsync` 等）无任何变化。

## 4. 正确性

### 4.1 单测（fabric_mem_translate_unittest.cc，6 用例）

| 用例 | 覆盖点 |
| --- | --- |
| BinarySearchMatchesLinearScan | 二分 vs 线性参考实现对拍：10000 个随机地址，new_va 与 available_len 逐一相等 |
| BinarySearchHandlesBoundariesAndHoles | 段首/段尾/段前/段后一格/段间空洞/UINTPTR_MAX |
| SingleSegmentTranslation | 单段命中，new_va 偏移计算，hit 指针 |
| CrossSegmentSplitPreservesOrderAndAddresses | 跨段拆分：两条 translated 的 local/remote/len |
| CacheReuseSkipsBinarySearchForContiguousDescs | 连续 desc 缓存命中；换段后回退二分 |
| InvalidInputsAreRejected | 未注册地址、零长度、地址溢出 → PARAM_INVALID |

### 4.2 既有测试适配

既有用例手工构造 `FabricMemTransferContext` 时仅填充 map，已适配为两表同填（`DeviceForeignImportsEveryBlockAndSplitsD2dAcrossBlocks` 等）。

### 4.3 回归

容器（CANN 9.2.0 环境）全套 `fabric_mem_test`：失败集与优化前基线完全一致（24 个环境性失败，stub runtime 下 AicpuUnfold 等用例固有失败），**无新增回归**；新增 6 用例全部通过。

## 5. 验证

1. **容器单测**（已完成）：CANN 9.2.0 环境 cmake 构建 + 单测 6/6 + 全套回归对比；
2. **真机性能验证**（待执行）：编译替换 `libcann_hixl.so`，重跑 yuanrong case2（64K GLM-5.3 KV cache），判据：
   - `HCCS_HIXL_TRANSFER_SYNC_READ` 单 chunk 时长（117 desc）较基线 ~4.8ms 显著下降；
   - load kvc 较基线 168.7ms 向 ~120ms 收敛；
   - store kvc / TTFT / TPS 无回归。

## 6. 风险与回滚

| 风险 | 缓解 |
| --- | --- |
| `remote_segments` 与 map 不一致（新增填充点遗漏） | 契约集中于 `BuildTransferContext` 一处；单测覆盖手工构造场景 |
| 未来新增段注册/更新路径未同步两表 | 该路径仅有注册/销毁两处，改动时以"两表同填"约定评审 |
| 段间重叠（理论上不应发生） | 二分取"最后一个 old_va ≤ addr"，与原线性实现"首个命中"语义在重叠段场景不同——当前注册逻辑保证段不重叠，单测对拍亦基于不重叠假设 |
| 回滚 | 单 commit 回退（`git revert`），无持久化状态变更 |
