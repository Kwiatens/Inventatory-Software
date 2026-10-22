// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// One supplier's inventory refresh. Lookups run one at a time on a worker thread and are merged on
// the UI thread. The worker reads the client through a raw pointer; the client is declared before
// the future, so a running lookup is always waited for before the client is destroyed.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace inventatory {

template <typename Client, typename Request, typename Result>
struct SupplierRefresh {
  std::deque<std::pair<std::string, Request>> queue;  // Item id, and what to look the item up by.
  std::unique_ptr<Client> client;
  std::future<Result> future;
  std::uint64_t generation = 0;  // The workspace the run belongs to.
  std::size_t total = 0;
  std::size_t completed = 0;
  std::size_t succeeded = 0;
  std::size_t notFound = 0;
  std::size_t ambiguous = 0;
  std::size_t errors = 0;
  bool changed = false;
  std::string lastError;

  bool running() const { return !queue.empty() || future.valid(); }

  // Waits for a running lookup, then clears the run.
  void stop() {
    queue.clear();
    if (future.valid()) {
      future.wait();
      future = {};
    }
    client.reset();
    total = completed = succeeded = notFound = ambiguous = errors = 0;
    changed = false;
    generation = 0;
    lastError.clear();
  }

  // The started lookup's result once it is ready.
  std::optional<Result> takeResult() {
    if (!future.valid() || future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      return std::nullopt;
    }
    return future.get();
  }

  // Starts the next queued lookup; lookup(client, request, result) runs on the worker. False when
  // nothing is queued or the run has no client.
  template <typename Lookup>
  bool startNext(Lookup lookup) {
    if (queue.empty() || client == nullptr) return false;
    const auto entry = queue.front();
    queue.pop_front();
    future = std::async(std::launch::async, [client = client.get(), entry, generation = generation, lookup] {
      Result result;
      result.itemId = entry.first;
      result.workspaceGeneration = generation;
      lookup(*client, entry.second, result);
      return result;
    });
    return true;
  }
};

// "3 updated, 1 not found, 0 ambiguous, 2 errors": a TME or Mouser run's counts.
template <typename Run>
std::string catalogueRefreshCounts(const Run& run) {
  return std::to_string(run.succeeded) + " updated, " + std::to_string(run.notFound) + " not found, " +
         std::to_string(run.ambiguous) + " ambiguous, " + std::to_string(run.errors) + " errors";
}

}  // namespace inventatory
