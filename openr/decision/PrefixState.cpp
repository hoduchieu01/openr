/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/decision/PrefixState.h"

#include <openr/common/Util.h>

using apache::thrift::can_throw;

namespace openr {

void
PrefixState::deleteLoopbackPrefix(
    thrift::IpPrefix const& prefix, const std::string& nodeName) {
  auto addrSize = prefix.prefixAddress.addr.size();
  if (addrSize == folly::IPAddressV4::byteCount() &&
      folly::IPAddressV4::bitCount() == prefix.prefixLength) {
    if (nodeHostLoopbacksV4_.find(nodeName) != nodeHostLoopbacksV4_.end() &&
        prefix.prefixAddress == nodeHostLoopbacksV4_.at(nodeName)) {
      nodeHostLoopbacksV4_.erase(nodeName);
    }
  }
  if (addrSize == folly::IPAddressV6::byteCount() &&
      folly::IPAddressV6::bitCount() == prefix.prefixLength) {
    if (nodeHostLoopbacksV6_.find(nodeName) != nodeHostLoopbacksV6_.end() &&
        nodeHostLoopbacksV6_.at(nodeName) == prefix.prefixAddress) {
      nodeHostLoopbacksV6_.erase(nodeName);
    }
  }
}

std::unordered_set<thrift::IpPrefix>
PrefixState::updatePrefixDatabase(thrift::PrefixDatabase const& prefixDb) {
  std::unordered_set<thrift::IpPrefix> changed;

  auto const& nodeName = prefixDb.thisNodeName;
  auto const& area = prefixDb.area;

  // Get old and new set of prefixes - NOTE explicit copy
  const std::set<thrift::IpPrefix> oldPrefixSet =
      nodeToPrefixes_[nodeName][area];

  // update the entry
  auto& newPrefixSet = nodeToPrefixes_[nodeName][area];
  newPrefixSet.clear();
  for (const auto& prefixEntry : prefixDb.prefixEntries) {
    newPrefixSet.emplace(prefixEntry.prefix);
  }

  // Remove old prefixes first
  for (const auto& prefix : oldPrefixSet) {
    if (newPrefixSet.count(prefix)) {
      continue;
    }

    VLOG(1) << "Prefix " << toString(prefix) << " has been withdrawn by "
            << nodeName << " from area " << area;

    auto& entriesByOriginator = prefixes_.at(prefix);

    // skip duplicate withdrawn
    if (not entriesByOriginator.count(nodeName)) {
      continue;
    }

    // remove route from advertised from <node, area>
    entriesByOriginator.at(nodeName).erase(area);

    // remove node map if routes from all areas are withdrawn
    if (entriesByOriginator.at(nodeName).empty()) {
      entriesByOriginator.erase(nodeName);
    }

    // remove prefix if routes are withdrawn
    if (entriesByOriginator.empty()) {
      prefixes_.erase(prefix);
    }

    deleteLoopbackPrefix(prefix, nodeName);
    changed.insert(prefix);
  }

  // update prefix entry for new announcement
  for (const auto& prefixEntry : prefixDb.prefixEntries) {
    auto& entriesByOriginator = prefixes_[prefixEntry.prefix];

    // This prefix has no change. Skip rest of code!
    if (entriesByOriginator.count(nodeName) > 0 and
        entriesByOriginator.at(nodeName).count(area) > 0 and
        entriesByOriginator.at(nodeName).at(area) == prefixEntry) {
      continue;
    }

    // Add or Update prefix
    entriesByOriginator[nodeName][area] = prefixEntry;
    changed.insert(prefixEntry.prefix);

    VLOG(1) << "Prefix " << toString(prefixEntry.prefix)
            << " has been advertised/updated by node " << nodeName
            << " from area " << area;

    // Keep track of loopback addresses (v4 / v6) for each node
    if (thrift::PrefixType::LOOPBACK == prefixEntry.type) {
      auto addrSize = prefixEntry.prefix.prefixAddress.addr.size();
      if (addrSize == folly::IPAddressV4::byteCount() &&
          folly::IPAddressV4::bitCount() == prefixEntry.prefix.prefixLength) {
        nodeHostLoopbacksV4_[nodeName] = prefixEntry.prefix.prefixAddress;
      }
      if (addrSize == folly::IPAddressV6::byteCount() &&
          folly::IPAddressV6::bitCount() == prefixEntry.prefix.prefixLength) {
        nodeHostLoopbacksV6_[nodeName] = prefixEntry.prefix.prefixAddress;
      }
    }
  }

  if (newPrefixSet.empty()) {
    nodeToPrefixes_.erase(nodeName);
  }

  return changed;
}

std::unordered_map<std::string /* nodeName */, thrift::PrefixDatabase>
PrefixState::getPrefixDatabases() const {
  std::unordered_map<std::string, thrift::PrefixDatabase> prefixDatabases;
  for (auto const& [node, areaToPrefixes] : nodeToPrefixes_) {
    for (auto const& [area, prefixes] : areaToPrefixes) {
      thrift::PrefixDatabase prefixDb;
      prefixDb.thisNodeName = node;
      prefixDb.area_ref() = area;
      for (auto const& prefix : prefixes) {
        prefixDb.prefixEntries.emplace_back(
            prefixes_.at(prefix).at(node).at(area));
      }
      prefixDatabases.emplace(node, std::move(prefixDb));
    }
  }
  return prefixDatabases;
}

std::vector<thrift::NextHopThrift>
PrefixState::getLoopbackVias(
    std::unordered_set<std::string> const& nodes,
    bool const isV4,
    std::optional<int64_t> const& igpMetric) const {
  std::vector<thrift::NextHopThrift> result;
  result.reserve(nodes.size());
  auto const& hostLoopBacks =
      isV4 ? nodeHostLoopbacksV4_ : nodeHostLoopbacksV6_;
  for (auto const& node : nodes) {
    if (!hostLoopBacks.count(node)) {
      LOG(ERROR) << "No loopback for node " << node;
    } else {
      result.emplace_back(createNextHop(
          hostLoopBacks.at(node), std::nullopt, igpMetric.value_or(0)));
    }
  }
  return result;
}
} // namespace openr
