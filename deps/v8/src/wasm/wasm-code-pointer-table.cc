// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/wasm/wasm-code-pointer-table.h"

#include "src/sandbox/external-entity-table-inl.h"
#include "src/wasm/wasm-code-pointer-table-inl.h"

namespace v8::internal::wasm {

void WasmCodePointerTable::Initialize() { Base::Initialize(); }

void WasmCodePointerTable::TearDown() {
  FreeNativeFunctionHandles();
  SweepSegments(0);
  DCHECK(freelist_head_.load().is_empty());
  Base::TearDown();
}

DEFINE_LAZY_LEAKY_OBJECT_GETTER(WasmCodePointerTable,
                                GetProcessWideWasmCodePointerTable)

std::vector<uint32_t> WasmCodePointerTable::FreelistToVector(
    WasmCodePointerTable::FreelistHead freelist) {
  DCHECK(!freelist.is_empty());
  std::vector<uint32_t> entries(freelist.length());

  uint32_t entry = freelist.next();
  for (uint32_t i = 0; i < freelist.length(); i++) {
    entries[i] = entry;
    entry = at(entry).GetNextFreelistEntryIndex();
  }

  return entries;
}

WasmCodePointerTable::FreelistHead WasmCodePointerTable::VectorToFreelist(
    std::vector<uint32_t> entries) {
  if (entries.empty()) {
    return FreelistHead();
  }

  FreelistHead new_freelist =
      FreelistHead(entries[0], static_cast<uint32_t>(entries.size()));

  WriteScope write_scope("Freelist write");
  for (size_t i = 0; i < entries.size() - 1; i++) {
    uint32_t entry = entries[i];
    uint32_t next_entry = entries[i + 1];
    at(entry).MakeFreelistEntry(next_entry);
  }

  return new_freelist;
}

void WasmCodePointerTable::SweepSegments(size_t threshold) {
  if (threshold < kEntriesPerSegment) {
    // We need at least a whole empty segment if we want to sweep anything.
    threshold = kEntriesPerSegment;
  }

  FreelistHead initial_head, empty_freelist;
  do {
    initial_head = ReadFreelistHead();
    if (initial_head.length() < threshold) {
      return;
    }

    // Try to unlink the freelist. If it fails, try again.
  } while (
      !freelist_head_.compare_exchange_strong(initial_head, empty_freelist));

  // We unlinked the whole free list, so we have exclusive access to it at
  // this point.

  // Now search for empty segments (== all entries are freelist entries) and
  // unlink them.

  std::vector<uint32_t> freelist_entries = FreelistToVector(initial_head);
  std::sort(freelist_entries.begin(), freelist_entries.end());

  // The minimum threshold is kEntriesPerSegment.
  DCHECK_GE(freelist_entries.size(), kEntriesPerSegment);

  // We iterate over all freelist entries and copy them over to a new vector,
  // while skipping and unmapping empty segments.
  std::vector<uint32_t> new_freelist_entries;
  for (size_t i = 0; i < freelist_entries.size(); i++) {
    uint32_t entry = freelist_entries[i];
    Segment segment = Segment::Containing(entry);

    if (segment.first_entry() == entry &&
        i + kEntriesPerSegment - 1 < freelist_entries.size()) {
      uint32_t last_entry = freelist_entries[i + kEntriesPerSegment - 1];
      if (segment.last_entry() == last_entry) {
        // The whole segment is empty. Delete the segment and skip all
        // entries;
        FreeTableSegment(segment);
        i += kEntriesPerSegment - 1;
        continue;
      }
    }

    new_freelist_entries.push_back(entry);
  }

  DCHECK_LE(new_freelist_entries.size(), freelist_entries.size());
  DCHECK(IsAligned(freelist_entries.size() - new_freelist_entries.size(),
                   kEntriesPerSegment));

  if (new_freelist_entries.empty()) {
    return;
  }

  // Finally, add the new freelist back.

  uint32_t last_element = new_freelist_entries.back();
  FreelistHead new_freelist = VectorToFreelist(new_freelist_entries);

  LinkFreelist(new_freelist, last_element);
}

WasmCodePointer WasmCodePointerTable::GetOrCreateHandleForNativeFunction(
    Address addr) {
  base::MutexGuard guard(&native_function_map_mutex_);
  auto it = native_function_map_.find(addr);
  if (it != native_function_map_.end()) {
    return it->second;
  }

  WasmCodePointer handle = AllocateAndInitializeEntry(addr, -1);
  native_function_map_.insert({addr, handle});

  return handle;
}

bool WasmCodePointerTable::EntrypointEqualTo(WasmCodePointer index,
                                             Address address) {
  return at(index.value()).GetEntrypointWithoutSignatureCheck() == address;
}

void WasmCodePointerTable::FreeNativeFunctionHandles() {
  base::MutexGuard guard(&native_function_map_mutex_);
  for (auto const& [address, handle] : native_function_map_) {
    FreeEntry(handle);
  }
  native_function_map_.clear();
}

}  // namespace v8::internal::wasm
