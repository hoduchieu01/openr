/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Decision.h"

#include <chrono>
#include <set>
#include <string>
#include <unordered_set>

#include <fb303/ServiceData.h>
#include <folly/Format.h>
#include <folly/MapUtil.h>
#include <folly/Memory.h>
#include <folly/Optional.h>
#include <folly/String.h>
#include <folly/futures/Future.h>
#if FOLLY_USE_SYMBOLIZER
#include <folly/experimental/exception_tracer/ExceptionTracer.h>
#endif
#include <gflags/gflags.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/decision/PrefixState.h>
#include <openr/decision/RibEntry.h>

using namespace std;

namespace fb303 = facebook::fb303;

using apache::thrift::can_throw;
using apache::thrift::FRAGILE;
using apache::thrift::TEnumTraits;

using Metric = openr::LinkStateMetric;

using SpfResult = openr::LinkState::SpfResult;

namespace openr {

DecisionRouteUpdate
getRouteDelta(const DecisionRouteDb& newDb, const DecisionRouteDb& oldDb) {
  DecisionRouteUpdate delta;

  // unicastRoutesToUpdate
  for (const auto& [prefix, entry] : newDb.unicastEntries) {
    const auto& oldEntry = oldDb.unicastEntries.find(prefix);
    if (oldEntry != oldDb.unicastEntries.end() && oldEntry->second == entry) {
      continue;
    }

    // new prefix, or prefix entry changed
    delta.unicastRoutesToUpdate.emplace_back(entry);
  }

  // unicastRoutesToDelete
  for (const auto& [prefix, _] : oldDb.unicastEntries) {
    if (newDb.unicastEntries.count(prefix) == 0) {
      delta.unicastRoutesToDelete.emplace_back(toIPNetwork(prefix));
    }
  }

  // mplsRoutesToUpdate
  for (const auto& [label, entry] : newDb.mplsEntries) {
    const auto& oldEntry = oldDb.mplsEntries.find(label);
    if (oldEntry != oldDb.mplsEntries.cend() && oldEntry->second == entry) {
      continue;
    }
    delta.mplsRoutesToUpdate.emplace_back(entry);
  }

  // mplsRoutesToDelete
  for (const auto& [label, _] : oldDb.mplsEntries) {
    if (newDb.mplsEntries.count(label) == 0) {
      delta.mplsRoutesToDelete.emplace_back(label);
    }
  }
  return delta;
}

/**
 * Private implementation of the SpfSolver
 */
class SpfSolver::SpfSolverImpl {
 public:
  SpfSolverImpl(
      const std::string& myNodeName,
      bool enableV4,
      bool computeLfaPaths,
      bool enableOrderedFib,
      bool bgpDryRun,
      bool bgpUseIgpMetric)
      : myNodeName_(myNodeName),
        enableV4_(enableV4),
        computeLfaPaths_(computeLfaPaths),
        enableOrderedFib_(enableOrderedFib),
        bgpDryRun_(bgpDryRun),
        bgpUseIgpMetric_(bgpUseIgpMetric) {
    // Initialize stat keys
    fb303::fbData->addStatExportType("decision.adj_db_update", fb303::COUNT);
    fb303::fbData->addStatExportType(
        "decision.incompatible_forwarding_type", fb303::COUNT);
    fb303::fbData->addStatExportType(
        "decision.missing_loopback_addr", fb303::SUM);
    fb303::fbData->addStatExportType(
        "decision.no_route_to_label", fb303::COUNT);
    fb303::fbData->addStatExportType(
        "decision.no_route_to_prefix", fb303::COUNT);
    fb303::fbData->addStatExportType("decision.path_build_ms", fb303::AVG);
    fb303::fbData->addStatExportType("decision.prefix_db_update", fb303::COUNT);
    fb303::fbData->addStatExportType("decision.route_build_ms", fb303::AVG);
    fb303::fbData->addStatExportType("decision.route_build_runs", fb303::COUNT);
    fb303::fbData->addStatExportType(
        "decision.skipped_mpls_route", fb303::COUNT);
    fb303::fbData->addStatExportType(
        "decision.duplicate_node_label", fb303::COUNT);
    fb303::fbData->addStatExportType(
        "decision.skipped_unicast_route", fb303::COUNT);
    fb303::fbData->addStatExportType("decision.spf_ms", fb303::AVG);
    fb303::fbData->addStatExportType("decision.spf_runs", fb303::COUNT);
    fb303::fbData->addStatExportType("decision.errors", fb303::COUNT);
  }

  ~SpfSolverImpl() = default;

  //
  // mpls static route
  //

  bool staticRoutesUpdated();

  void pushRoutesDeltaUpdates(thrift::RouteDatabaseDelta& staticRoutesDelta);

  std::optional<DecisionRouteUpdate> processStaticRouteUpdates();

  thrift::StaticRoutes const& getStaticRoutes();

  //
  // best path calculation
  //

  // Build route database using global prefix database and cached SPF
  // computation from perspective of a given router.
  // Returns std::nullopt if myNodeName doesn't have any prefix database
  std::optional<DecisionRouteDb> buildRouteDb(
      const std::string& myNodeName,
      std::unordered_map<std::string, LinkState> const& areaLinkStates,
      PrefixState const& prefixState);

  // helpers used in best path calculation
  static std::pair<Metric, std::unordered_set<std::string>> getMinCostNodes(
      const SpfResult& spfResult, const std::set<std::string>& dstNodes);

  // spf counters
  void updateGlobalCounters();

 private:
  // no copy
  SpfSolverImpl(SpfSolverImpl const&) = delete;
  SpfSolverImpl& operator=(SpfSolverImpl const&) = delete;

  // Given prefixes and the nodes who announce it, get the ecmp routes.
  // emplace unicastEntry into unicastEntries if valid ecmp exists
  void selectEcmpOpenr(
      std::unordered_map<thrift::IpPrefix, RibUnicastEntry>& unicastEntries,
      std::string const& myNodeName,
      thrift::IpPrefix const& prefix,
      thrift::PrefixEntries const& prefixEntries,
      bool const isV4,
      std::unordered_map<std::string, LinkState> const& areaLinkStates);

  // Given bgp prefixes and the nodes who announce it, get the ecmp routes.
  // emplace unicastEntry into unicastEntries if valid ecmp exists
  void selectEcmpBgp(
      std::unordered_map<thrift::IpPrefix, RibUnicastEntry>& unicastEntries,
      std::string const& myNodeName,
      thrift::IpPrefix const& prefix,
      thrift::PrefixEntries const& prefixEntries,
      bool const isV4,
      std::unordered_map<std::string, LinkState> const& areaLinkStates,
      PrefixState const& prefixState);

  // Given prefixes and the nodes who announce it, get the kspf routes.
  void selectKsp2(
      std::unordered_map<thrift::IpPrefix, RibUnicastEntry>& unicastEntries,
      const thrift::IpPrefix& prefix,
      const string& myNodeName,
      BestPathCalResult const& bestPathCalResult,
      thrift::PrefixEntries const& prefixEntries,
      bool hasBgp,
      std::unordered_map<std::string, LinkState> const& areaLinkStates,
      PrefixState const& prefixState,
      const thrift::PrefixForwardingAlgorithm& pfxFwdAlg);

  // helper function to find the nodes for the nexthop for bgp route
  BestPathCalResult runBestPathSelectionBgp(
      std::string const& myNodeName,
      thrift::IpPrefix const& prefix,
      thrift::PrefixEntries const& prefixEntries,
      std::unordered_map<std::string, LinkState> const& areaLinkStates);

  BestPathCalResult getBestAnnouncingNodes(
      std::string const& myNodeName,
      thrift::IpPrefix const& prefix,
      thrift::PrefixEntries const& prefixEntries,
      bool const hasBgp,
      bool const useKsp2EdAlgo,
      std::unordered_map<std::string, LinkState> const& areaLinkStates);

  // helper to get min nexthop for a prefix, used in selectKsp2
  std::optional<int64_t> getMinNextHopThreshold(
      BestPathCalResult nodes, thrift::PrefixEntries const& prefixEntries);

  // helper to filter overloaded nodes for anycast addresses
  BestPathCalResult maybeFilterDrainedNodes(
      BestPathCalResult&& result,
      std::unordered_map<std::string, LinkState> const& areaLinkStates) const;

  // Give source node-name and dstNodeNames, this function returns the set of
  // nexthops (along with LFA if enabled) towards these set of dstNodeNames
  std::pair<
      Metric /* minimum metric to destination */,
      std::unordered_map<
          std::pair<std::string /* nextHopNodeName */, std::string /* dest */>,
          Metric /* the distance from the nexthop to the dest */>>
  getNextHopsWithMetric(
      const std::string& srcNodeName,
      const std::set<std::string>& dstNodeNames,
      bool perDestination,
      std::unordered_map<std::string, LinkState> const& areaLinkStates);

  // This function converts best nexthop nodes to best nexthop adjacencies
  // which can then be passed to FIB for programming. It considers LFA and
  // parallel link logic (tested by our UT)
  // If swap label is provided then it will be used to associate SWAP or PHP
  // mpls action
  std::unordered_set<thrift::NextHopThrift> getNextHopsThrift(
      const std::string& myNodeName,
      const std::set<std::string>& dstNodeNames,
      bool isV4,
      bool perDestination,
      const Metric minMetric,
      std::unordered_map<std::pair<std::string, std::string>, Metric>
          nextHopNodes,
      std::optional<int32_t> swapLabel,
      std::unordered_map<std::string, LinkState> const& areaLinkStates,
      std::set<std::string> const& prefixAreas) const;

  thrift::StaticRoutes staticRoutes_;

  std::vector<thrift::RouteDatabaseDelta> staticRoutesUpdates_;

  const std::string myNodeName_;

  // is v4 enabled. If yes then Decision will forward v4 prefixes with v4
  // nexthops to Fib module for programming. Else it will just drop them.
  const bool enableV4_{false};

  const bool computeLfaPaths_{false};

  const bool enableOrderedFib_{false};

  const bool bgpDryRun_{false};

  // Use IGP metric in metric vector comparision
  const bool bgpUseIgpMetric_{false};
};

bool
SpfSolver::SpfSolverImpl::staticRoutesUpdated() {
  return staticRoutesUpdates_.size() > 0;
}

void
SpfSolver::SpfSolverImpl::pushRoutesDeltaUpdates(
    thrift::RouteDatabaseDelta& staticRoutesDelta) {
  staticRoutesUpdates_.emplace_back(std::move(staticRoutesDelta));
}

thrift::StaticRoutes const&
SpfSolver::SpfSolverImpl::getStaticRoutes() {
  return staticRoutes_;
}

std::optional<DecisionRouteDb>
SpfSolver::SpfSolverImpl::buildRouteDb(
    const std::string& myNodeName,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixState const& prefixState) {
  bool nodeExist{false};
  for (const auto& [_, linkState] : areaLinkStates) {
    nodeExist |= linkState.hasNode(myNodeName);
  }
  if (not nodeExist) {
    return std::nullopt;
  }

  const auto startTime = std::chrono::steady_clock::now();
  fb303::fbData->addStatValue("decision.route_build_runs", 1, fb303::COUNT);

  DecisionRouteDb routeDb{};

  //
  // Calculate unicast route best paths: IP and IP2MPLS routes
  //

  for (const auto& [prefix, prefixEntries] : prefixState.prefixes()) {
    bool hasBGP = false, hasNonBGP = false, missingMv = false;

    for (auto const& [node, areaToPrefixEntries] : prefixEntries) {
      for (auto const& [area, prefixEntry] : areaToPrefixEntries) {
        bool isBGP = prefixEntry.type == thrift::PrefixType::BGP;
        hasBGP |= isBGP;
        hasNonBGP |= !isBGP;
        if (isBGP and not prefixEntry.mv_ref().has_value()) {
          missingMv = true;
          LOG(ERROR) << "Prefix entry for prefix "
                     << toString(prefixEntry.prefix) << " advertised by "
                     << node << ", area " << area
                     << " is of type BGP but does not contain a metric vector.";
        }
      }
    }
    // skip adding route for BGP prefixes that have issues
    if (hasBGP) {
      if (hasNonBGP) {
        LOG(ERROR) << "Skipping route for prefix " << toString(prefix)
                   << " which is advertised with BGP and non-BGP type.";
        fb303::fbData->addStatValue(
            "decision.skipped_unicast_route", 1, fb303::COUNT);
        continue;
      }
      if (missingMv) {
        LOG(ERROR) << "Skipping route for prefix " << toString(prefix)
                   << " at least one advertiser is missing its metric vector.";
        fb303::fbData->addStatValue(
            "decision.skipped_unicast_route", 1, fb303::COUNT);
        continue;
      }
    }

    // skip adding route for prefixes advertised by this node
    if (prefixEntries.count(myNodeName) and not hasBGP) {
      continue;
    }

    // Check for enabledV4_
    auto prefixStr = prefix.prefixAddress.addr;
    bool isV4Prefix = prefixStr.size() == folly::IPAddressV4::byteCount();
    if (isV4Prefix && !enableV4_) {
      LOG(WARNING) << "Received v4 prefix while v4 is not enabled.";
      fb303::fbData->addStatValue(
          "decision.skipped_unicast_route", 1, fb303::COUNT);
      continue;
    }

    const auto& prefixForwardingAlgo =
        getPrefixForwardingAlgorithm(prefixEntries);
    const auto& prefixForwardingType = getPrefixForwardingType(prefixEntries);

    // MPLS for SP_ECMP / KSP2_ED_ECMP
    if (prefixForwardingType == thrift::PrefixForwardingType::SR_MPLS) {
      const auto nodes = getBestAnnouncingNodes(
          myNodeName, prefix, prefixEntries, hasBGP, true, areaLinkStates);
      if (not nodes.success or nodes.nodes.size() == 0) {
        continue;
      }
      selectKsp2(
          routeDb.unicastEntries,
          prefix,
          myNodeName,
          nodes,
          prefixEntries,
          hasBGP,
          areaLinkStates,
          prefixState,
          prefixForwardingAlgo);
    }
    // IP for SP_ECMP, KSP2_ED_ECMP is not supported in IP routing
    else {
      if (prefixForwardingAlgo == thrift::PrefixForwardingAlgorithm::SP_ECMP) {
        if (hasBGP) {
          selectEcmpBgp(
              routeDb.unicastEntries,
              myNodeName,
              prefix,
              prefixEntries,
              isV4Prefix,
              areaLinkStates,
              prefixState);
        } else {
          selectEcmpOpenr(
              routeDb.unicastEntries,
              myNodeName,
              prefix,
              prefixEntries,
              isV4Prefix,
              areaLinkStates);
        }
      } else {
        LOG(ERROR) << "prefix not supported: " << toString(prefix);
        fb303::fbData->addStatValue(
            "decision.incompatible_forwarding_type", 1, fb303::COUNT);
      }
    }

  } // for prefixState.prefixes()

  //
  // Create MPLS routes for all nodeLabel
  //
  std::unordered_map<int32_t, std::pair<std::string, RibMplsEntry>> labelToNode;
  for (const auto& [area, linkState] : areaLinkStates) {
    for (const auto& [_, adjDb] : linkState.getAdjacencyDatabases()) {
      const auto topLabel = adjDb.nodeLabel;
      // Top label is not set => Non-SR mode
      if (topLabel == 0) {
        continue;
      }
      // If mpls label is not valid then ignore it
      if (not isMplsLabelValid(topLabel)) {
        LOG(ERROR) << "Ignoring invalid node label " << topLabel << " of node "
                   << adjDb.thisNodeName;
        fb303::fbData->addStatValue(
            "decision.skipped_mpls_route", 1, fb303::COUNT);
        continue;
      }

      // There can be a temporary collision in node label allocation. Usually
      // happens when two segmented networks allocating labels from the same
      // range join together. In case of such conflict we respect the node label
      // of bigger node-ID
      auto iter = labelToNode.find(topLabel);
      if (iter != labelToNode.end()) {
        LOG(INFO) << "Find duplicate label " << topLabel << "from "
                  << iter->second.first << " " << adjDb.thisNodeName;
        fb303::fbData->addStatValue(
            "decision.duplicate_node_label", 1, fb303::COUNT);
        if (iter->second.first < adjDb.thisNodeName) {
          continue;
        }
      }

      // Install POP_AND_LOOKUP for next layer
      if (adjDb.thisNodeName == myNodeName) {
        thrift::NextHopThrift nh;
        nh.address = toBinaryAddress(folly::IPAddressV6("::"));
        nh.area_ref() = area;
        nh.mplsAction_ref() =
            createMplsAction(thrift::MplsActionCode::POP_AND_LOOKUP);
        labelToNode.erase(topLabel);
        labelToNode.emplace(
            topLabel,
            std::make_pair(adjDb.thisNodeName, RibMplsEntry(topLabel, {nh})));
        continue;
      }

      // Get best nexthop towards the node
      auto metricNhs = getNextHopsWithMetric(
          myNodeName, {adjDb.thisNodeName}, false, areaLinkStates);
      if (metricNhs.second.empty()) {
        LOG(WARNING) << "No route to nodeLabel " << std::to_string(topLabel)
                     << " of node " << adjDb.thisNodeName;
        fb303::fbData->addStatValue(
            "decision.no_route_to_label", 1, fb303::COUNT);
        continue;
      }

      // Create nexthops with appropriate MplsAction (PHP and SWAP). Note that
      // all nexthops are valid for routing without loops. Fib is responsible
      // for installing these routes by making sure it programs least cost
      // nexthops first and of same action type (based on HW limitations)
      labelToNode.erase(topLabel);
      labelToNode.emplace(
          topLabel,
          std::make_pair(
              adjDb.thisNodeName,
              RibMplsEntry(
                  topLabel,
                  getNextHopsThrift(
                      myNodeName,
                      {adjDb.thisNodeName},
                      false,
                      false,
                      metricNhs.first,
                      metricNhs.second,
                      topLabel,
                      areaLinkStates,
                      {area}))));
    }
  }

  for (auto& [label, nodeToEntry] : labelToNode) {
    routeDb.mplsEntries.emplace(label, std::move(nodeToEntry.second));
  }

  //
  // Create MPLS routes for all of our adjacencies
  //
  for (const auto& [_, linkState] : areaLinkStates) {
    for (const auto& link : linkState.linksFromNode(myNodeName)) {
      const auto topLabel = link->getAdjLabelFromNode(myNodeName);
      // Top label is not set => Non-SR mode
      if (topLabel == 0) {
        continue;
      }
      // If mpls label is not valid then ignore it
      if (not isMplsLabelValid(topLabel)) {
        LOG(ERROR) << "Ignoring invalid adjacency label " << topLabel
                   << " of link " << link->directionalToString(myNodeName);
        fb303::fbData->addStatValue(
            "decision.skipped_mpls_route", 1, fb303::COUNT);
        continue;
      }

      routeDb.mplsEntries.emplace(
          topLabel,
          RibMplsEntry(
              topLabel,
              {createNextHop(
                  link->getNhV6FromNode(myNodeName),
                  link->getIfaceFromNode(myNodeName),
                  link->getMetricFromNode(myNodeName),
                  createMplsAction(thrift::MplsActionCode::PHP),
                  false /* useNonShortestRoute */,
                  link->getArea())}));
    }
  }

  auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startTime);
  LOG(INFO) << "Decision::buildRouteDb took " << deltaTime.count() << "ms.";
  fb303::fbData->addStatValue(
      "decision.route_build_ms", deltaTime.count(), fb303::AVG);
  return routeDb;
} // buildRouteDb

BestPathCalResult
SpfSolver::SpfSolverImpl::getBestAnnouncingNodes(
    std::string const& myNodeName,
    thrift::IpPrefix const& prefix,
    thrift::PrefixEntries const& prefixEntries,
    bool const hasBgp,
    bool const useKsp2EdAlgo,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) {
  BestPathCalResult ret;

  // if it is openr route, all nodes are considered as best nodes.
  if (not hasBgp) {
    if (prefixEntries.count(myNodeName) > 0) {
      return BestPathCalResult{};
    }

    for (auto const& [node, areaToPrefixEntries] : prefixEntries) {
      for (auto const& [area, prefixEntry] : areaToPrefixEntries) {
        auto const& linkState = areaLinkStates.at(area);
        auto const& mySpfResult = linkState.getSpfResult(myNodeName);

        // Skip unreachable nodes
        auto it = mySpfResult.find(node);
        if (it == mySpfResult.end()) {
          LOG(ERROR) << "No route to " << node
                     << ". Skipping considering this.";
          // skip if no route to node
          continue;
        }

        // choose lowest node name
        if (ret.bestNode.empty() or node.compare(ret.bestNode) < 0) {
          ret.bestNode = node;
          ret.bestArea = area;
        }

        ret.nodes.insert(node);
        ret.areas.insert(area);
      }
    }
    ret.success = true;
    return maybeFilterDrainedNodes(std::move(ret), areaLinkStates);
  }

  // for bgp route, we need to run best path calculation algorithm to get
  // the nodes
  ret = runBestPathSelectionBgp(
      myNodeName, prefix, prefixEntries, areaLinkStates);

  // best path calculation failure
  if (not ret.success) {
    LOG(WARNING) << "No route to BGP prefix " << toString(prefix);
    fb303::fbData->addStatValue("decision.no_route_to_prefix", 1, fb303::COUNT);
    return BestPathCalResult{};
  }

  // ecmp
  if (not useKsp2EdAlgo) {
    // not announcing BGP prefix originated by self
    if (ret.nodes.count(myNodeName)) {
      VLOG(2) << "Ignoring route to BGP prefix " << toString(prefix)
              << ". Best path originated by self.";
      return BestPathCalResult{};
    }

    return maybeFilterDrainedNodes(std::move(ret), areaLinkStates);
  }

  // ksp2
  bool labelExistForMyNode{false};
  if (prefixEntries.count(myNodeName) > 0) {
    for (const auto& [_, prefixEntry] : prefixEntries.at(myNodeName)) {
      labelExistForMyNode |= prefixEntry.prependLabel_ref().has_value();
    }
  }
  // In ksp2 algorithm, we consider program our own advertised prefix if
  // there are other nodes announcing it and prepend label associated with
  // it.
  if (not ret.nodes.count(myNodeName) or
      (ret.nodes.size() > 1 and labelExistForMyNode)) {
    return maybeFilterDrainedNodes(std::move(ret), areaLinkStates);
  }

  VLOG(2) << "Ignoring route to BGP prefix " << toString(prefix)
          << ". Best path originated by self.";
  return BestPathCalResult{};
}

std::optional<int64_t>
SpfSolver::SpfSolverImpl::getMinNextHopThreshold(
    BestPathCalResult nodes, thrift::PrefixEntries const& prefixEntries) {
  std::optional<int64_t> maxMinNexthopForPrefix = std::nullopt;
  for (const auto& node : nodes.nodes) {
    if (prefixEntries.count(node) > 0) {
      for (const auto& [_, prefixEntry] : prefixEntries.at(node)) {
        maxMinNexthopForPrefix = prefixEntry.minNexthop_ref().has_value() &&
                (not maxMinNexthopForPrefix.has_value() ||
                 prefixEntry.minNexthop_ref().value() >
                     maxMinNexthopForPrefix.value())
            ? prefixEntry.minNexthop_ref().value()
            : maxMinNexthopForPrefix;
      }
    }
  }
  return maxMinNexthopForPrefix;
}

BestPathCalResult
SpfSolver::SpfSolverImpl::maybeFilterDrainedNodes(
    BestPathCalResult&& result,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) const {
  BestPathCalResult filtered = result;
  for (const auto& [_, linkState] : areaLinkStates) {
    for (auto iter = filtered.nodes.begin(); iter != filtered.nodes.end();) {
      if (linkState.isNodeOverloaded(*iter)) {
        iter = filtered.nodes.erase(iter);
      } else {
        ++iter;
      }
    }
  }
  return filtered.nodes.empty() ? result : filtered;
}

void
SpfSolver::SpfSolverImpl::selectEcmpOpenr(
    std::unordered_map<thrift::IpPrefix, RibUnicastEntry>& unicastEntries,
    std::string const& myNodeName,
    thrift::IpPrefix const& prefix,
    thrift::PrefixEntries const& prefixEntries,
    bool const isV4,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) {
  // Prepare list of nodes announcing the prefix
  const auto& ret = getBestAnnouncingNodes(
      myNodeName, prefix, prefixEntries, false, false, areaLinkStates);
  if (not ret.success) {
    return;
  }

  std::set<std::string> prefixNodes = ret.nodes;

  const bool perDestination = getPrefixForwardingType(prefixEntries) ==
      thrift::PrefixForwardingType::SR_MPLS;

  const auto metricNhs = getNextHopsWithMetric(
      myNodeName, prefixNodes, perDestination, areaLinkStates);
  if (metricNhs.second.empty()) {
    LOG(WARNING) << "No route to prefix " << toString(prefix)
                 << ", advertised by: " << folly::join(", ", prefixNodes);
    fb303::fbData->addStatValue("decision.no_route_to_prefix", 1, fb303::COUNT);
    return;
  }

  RibUnicastEntry entry(
      toIPNetwork(prefix), // prefix
      getNextHopsThrift(
          myNodeName,
          prefixNodes,
          isV4,
          perDestination,
          metricNhs.first,
          metricNhs.second,
          std::nullopt,
          areaLinkStates,
          ret.areas), // nexthops
      prefixEntries.at(ret.bestNode).at(ret.bestArea), // bestPrefixEntry
      ret.bestArea); // bestArea
  unicastEntries.emplace(prefix, std::move(entry));
}

BestPathCalResult
SpfSolver::SpfSolverImpl::runBestPathSelectionBgp(
    std::string const& myNodeName,
    thrift::IpPrefix const& prefix,
    thrift::PrefixEntries const& prefixEntries,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) {
  BestPathCalResult ret;
  for (auto const& [nodeName, areaToPrefixEntries] : prefixEntries) {
    for (auto const& [area, prefixEntry] : areaToPrefixEntries) {
      auto const& linkState = areaLinkStates.at(area);
      auto const& mySpfResult = linkState.getSpfResult(myNodeName);

      // Skip unreachable nodes
      auto it = mySpfResult.find(nodeName);
      if (it == mySpfResult.end()) {
        LOG(ERROR) << "No route to " << nodeName
                   << ". Skipping considering this.";
        // skip if no route to node
        continue;
      }

      // Sanity check that OPENR_IGP_COST shouldn't exist
      if (MetricVectorUtils::getMetricEntityByType(
              can_throw(*prefixEntry.mv_ref()),
              static_cast<int64_t>(thrift::MetricEntityType::OPENR_IGP_COST))) {
        LOG(ERROR)
            << "Received unexpected metric entity OPENR_IGP_COST in metric"
            << " vector for prefix " << toString(prefix) << " from node "
            << nodeName << ". Ignoring";
        continue;
      }

      // Copy is intentional - As we will need to augment metric vector with
      // IGP_COST
      thrift::MetricVector metricVector = can_throw(*prefixEntry.mv_ref());

      // Associate IGP_COST to prefixEntry
      if (bgpUseIgpMetric_) {
        const auto igpMetric = static_cast<int64_t>(it->second.metric());
        if (not ret.bestIgpMetric.has_value() or
            *(ret.bestIgpMetric) > igpMetric) {
          ret.bestIgpMetric = igpMetric;
        }
        metricVector.metrics.emplace_back(MetricVectorUtils::createMetricEntity(
            static_cast<int64_t>(thrift::MetricEntityType::OPENR_IGP_COST),
            static_cast<int64_t>(thrift::MetricEntityPriority::OPENR_IGP_COST),
            thrift::CompareType::WIN_IF_NOT_PRESENT,
            false, /* isBestPathTieBreaker */
            /* lowest metric wins */
            {-1 * igpMetric}));
        VLOG(2) << "Attaching IGP metric of " << igpMetric << " to prefix "
                << toString(prefix) << " for node " << nodeName;
      }

      switch (ret.bestVector.has_value()
                  ? MetricVectorUtils::compareMetricVectors(
                        metricVector, *(ret.bestVector))
                  : MetricVectorUtils::CompareResult::WINNER) {
      case MetricVectorUtils::CompareResult::WINNER:
        ret.nodes.clear();
        FOLLY_FALLTHROUGH;
      case MetricVectorUtils::CompareResult::TIE_WINNER:
        ret.bestVector = std::move(metricVector);
        ret.bestNode = nodeName;
        ret.bestArea = area;
        FOLLY_FALLTHROUGH;
      case MetricVectorUtils::CompareResult::TIE_LOOSER:
        ret.nodes.emplace(nodeName);
        ret.areas.emplace(area);
        break;
      case MetricVectorUtils::CompareResult::TIE:
        LOG(ERROR) << "Tie ordering prefix entries. Skipping route for prefix: "
                   << toString(prefix);
        return ret;
      case MetricVectorUtils::CompareResult::ERROR:
        LOG(ERROR)
            << "Error ordering prefix entries. Skipping route for prefix: "
            << toString(prefix);
        return ret;
      default:
        break;
      }
    }
  }
  ret.success = true;
  return maybeFilterDrainedNodes(std::move(ret), areaLinkStates);
}

void
SpfSolver::SpfSolverImpl::selectEcmpBgp(
    std::unordered_map<thrift::IpPrefix, RibUnicastEntry>& unicastEntries,
    std::string const& myNodeName,
    thrift::IpPrefix const& prefix,
    thrift::PrefixEntries const& prefixEntries,
    bool const isV4,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixState const& prefixState) {
  std::string bestNode;
  // order is intended to comply with API used later.
  std::set<std::string> nodes;
  std::optional<thrift::MetricVector> bestVector{std::nullopt};

  const auto dstInfo = getBestAnnouncingNodes(
      myNodeName, prefix, prefixEntries, true, false, areaLinkStates);
  if (not dstInfo.success) {
    return;
  }

  if (dstInfo.nodes.empty() or dstInfo.nodes.count(myNodeName)) {
    // do not program a route if we are advertising a best path to it or there
    // is no path to it
    if (not dstInfo.nodes.count(myNodeName)) {
      LOG(WARNING) << "No route to BGP prefix " << toString(prefix);
      fb303::fbData->addStatValue(
          "decision.no_route_to_prefix", 1, fb303::COUNT);
    }
    return;
  }

  auto bestNextHop = prefixState.getLoopbackVias(
      {dstInfo.bestNode}, isV4, dstInfo.bestIgpMetric);
  if (bestNextHop.size() != 1) {
    fb303::fbData->addStatValue(
        "decision.missing_loopback_addr", 1, fb303::SUM);
    LOG(ERROR) << "Cannot find the best paths loopback address. "
               << "Skipping route for prefix: " << toString(prefix);
    return;
  }

  const auto nextHopsWithMetric =
      getNextHopsWithMetric(myNodeName, dstInfo.nodes, false, areaLinkStates);

  RibUnicastEntry entry(
      toIPNetwork(prefix),
      getNextHopsThrift(
          myNodeName,
          dstInfo.nodes,
          isV4,
          false,
          nextHopsWithMetric.first,
          nextHopsWithMetric.second,
          std::nullopt,
          areaLinkStates,
          dstInfo.areas), // nexthops
      thrift::PrefixEntry(prefixEntries.at(dstInfo.bestNode)
                              .at(dstInfo.bestArea)), // bestPrefixEntry
      dstInfo.bestArea, // bestArea
      bgpDryRun_, // doNotInstall
      bestNextHop.at(0) // bestNexthop
  );

  unicastEntries.emplace(prefix, std::move(entry));
}

std::optional<DecisionRouteUpdate>
SpfSolver::SpfSolverImpl::processStaticRouteUpdates() {
  std::unordered_map<int32_t, thrift::MplsRoute> routesToUpdate;
  std::unordered_set<int32_t> routesToDel;

  // squash the updates together.
  for (const auto& staticRoutesUpdate : staticRoutesUpdates_) {
    for (const auto& mplsRoutesToUpdate :
         staticRoutesUpdate.mplsRoutesToUpdate) {
      LOG(INFO) << "adding: " << mplsRoutesToUpdate.topLabel;
      routesToUpdate[mplsRoutesToUpdate.topLabel] = mplsRoutesToUpdate;
      routesToDel.erase(mplsRoutesToUpdate.topLabel);
    }

    for (const auto& mplsRoutesToDelete :
         staticRoutesUpdate.mplsRoutesToDelete) {
      LOG(INFO) << "erasing: " << mplsRoutesToDelete;
      routesToDel.insert(mplsRoutesToDelete);
      routesToUpdate.erase(mplsRoutesToDelete);
    }
  }
  staticRoutesUpdates_.clear();

  if (routesToUpdate.size() == 0 && routesToDel.size() == 0) {
    return {};
  }

  DecisionRouteUpdate ret;
  for (const auto& [label, tMplsRoute] : routesToUpdate) {
    staticRoutes_.mplsRoutes[label] = tMplsRoute.nextHops;
    ret.mplsRoutesToUpdate.emplace_back(RibMplsEntry::fromThrift(tMplsRoute));
  }

  for (const auto& routeToDel : routesToDel) {
    staticRoutes_.mplsRoutes.erase(routeToDel);
    ret.mplsRoutesToDelete.push_back(routeToDel);
  }

  return ret;
}

void
SpfSolver::SpfSolverImpl::selectKsp2(
    std::unordered_map<thrift::IpPrefix, RibUnicastEntry>& unicastEntries,
    const thrift::IpPrefix& prefix,
    const string& myNodeName,
    BestPathCalResult const& bestPathCalResult,
    thrift::PrefixEntries const& prefixEntries,
    bool hasBgp,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixState const& prefixState,
    const thrift::PrefixForwardingAlgorithm& pfxFwdAlg) {
  RibUnicastEntry entry(toIPNetwork(prefix));
  bool selfNodeContained{false};

  std::vector<LinkState::Path> paths;

  for (const auto& [_, linkState] : areaLinkStates) {
    // find shortest and sec shortest routes towards each node.
    for (const auto& node : bestPathCalResult.nodes) {
      // if ourself is considered as ECMP nodes.
      if (node == myNodeName) {
        selfNodeContained = true;
        continue;
      }
      for (auto const& path : linkState.getKthPaths(myNodeName, node, 1)) {
        paths.push_back(path);
      }
    }

    if (pfxFwdAlg == thrift::PrefixForwardingAlgorithm::KSP2_ED_ECMP) {
      // when get to second shortes routes, we want to make sure the shortest
      // route is not part of second shortest route to avoid double spraying
      // issue
      size_t const firstPathsSize = paths.size();
      for (const auto& node : bestPathCalResult.nodes) {
        for (auto const& secPath : linkState.getKthPaths(myNodeName, node, 2)) {
          bool add = true;
          for (size_t i = 0; i < firstPathsSize; ++i) {
            // this could happen for anycast VIPs.
            // for example, in a full mesh topology contains A, B and C. B and C
            // both annouce a prefix P. When A wants to talk to P, it's shortes
            // paths are A->B and A->C. And it is second shortest path is
            // A->B->C and A->C->B. In this case,  A->B->C containser A->B
            // already, so we want to avoid this.
            if (LinkState::pathAInPathB(paths[i], secPath)) {
              add = false;
              break;
            }
          }
          if (add) {
            paths.push_back(secPath);
          }
        }
      }
    }
  }

  if (paths.size() == 0) {
    return;
  }

  for (const auto& path : paths) {
    for (const auto& [area, linkState] : areaLinkStates) {
      Metric cost = 0;
      std::list<int32_t> labels;
      // if self node is one of it's ecmp, it means this prefix is anycast and
      // we need to add prepend label which is static MPLS route the destination
      // prepared.
      auto nextNodeName = myNodeName;
      for (auto& link : path) {
        cost += link->getMetricFromNode(nextNodeName);
        nextNodeName = link->getOtherNodeName(nextNodeName);
        labels.push_front(
            linkState.getAdjacencyDatabases().at(nextNodeName).nodeLabel);
      }
      labels.pop_back(); // Remove first node's label to respect PHP
      if (prefixEntries.at(nextNodeName).at(area).prependLabel_ref()) {
        // add prepend label to bottom of the stack
        labels.push_front(
            *prefixEntries.at(nextNodeName).at(area).prependLabel_ref());
      }

      // Create nexthop
      CHECK_GE(path.size(), 1);
      auto const& firstLink = path.front();
      std::optional<thrift::MplsAction> mplsAction;
      if (labels.size()) {
        std::vector<int32_t> labelVec{labels.begin(), labels.end()};
        mplsAction = createMplsAction(
            thrift::MplsActionCode::PUSH, std::nullopt, std::move(labelVec));
      }

      auto const& prefixStr = prefix.prefixAddress.addr;
      bool isV4Prefix = prefixStr.size() == folly::IPAddressV4::byteCount();

      entry.nexthops.emplace(createNextHop(
          isV4Prefix ? firstLink->getNhV4FromNode(myNodeName)
                     : firstLink->getNhV6FromNode(myNodeName),
          firstLink->getIfaceFromNode(myNodeName),
          cost,
          mplsAction,
          true /* useNonShortestRoute */,
          firstLink->getArea()));
    }
  }

  int staticNexthops = 0;
  // if self node is one of it's ecmp node, we need to program nexthops which
  // provided by ourself in this case.
  if (selfNodeContained) {
    // TODO: MPLS can only be originated to one area
    CHECK_EQ(1, prefixEntries.at(myNodeName).size());
    auto label = can_throw(
        *prefixEntries.at(myNodeName).begin()->second.prependLabel_ref());
    auto routeIter = staticRoutes_.mplsRoutes.find(label);
    if (routeIter != staticRoutes_.mplsRoutes.end()) {
      for (const auto& nh : routeIter->second) {
        staticNexthops++;
        entry.nexthops.emplace(createNextHop(
            nh.address,
            std::nullopt,
            0,
            std::nullopt,
            true /* useNonShortestRoute */,
            prefixEntries.at(myNodeName).begin()->first));
      }
    } else {
      LOG(ERROR) << "Static nexthops not exist for static mpls label: "
                 << label;
    }
  }

  // if we have set minNexthop for prefix and # of nexthop didn't meet the
  // the threshold, we will ignore this route.
  auto minNextHop = getMinNextHopThreshold(bestPathCalResult, prefixEntries);
  auto dynamicNextHop =
      static_cast<int64_t>(entry.nexthops.size()) - staticNexthops;
  if (minNextHop.has_value() && minNextHop.value() > dynamicNextHop) {
    LOG(WARNING) << "Dropping routes to " << toString(prefix) << " because of "
                 << dynamicNextHop << " of nexthops is smaller than "
                 << minNextHop.value();
    return;
  }

  if (hasBgp) {
    auto bestNextHop = prefixState.getLoopbackVias(
        {bestPathCalResult.bestNode},
        prefix.prefixAddress.addr.size() == folly::IPAddressV4::byteCount(),
        bestPathCalResult.bestIgpMetric);
    if (bestNextHop.size() == 1) {
      entry.bestNexthop = bestNextHop.at(0);
      entry.bestPrefixEntry = prefixEntries.at(bestPathCalResult.bestNode)
                                  .at(bestPathCalResult.bestArea);
      entry.doNotInstall = bgpDryRun_;
    }
  }
  unicastEntries.emplace(prefix, std::move(entry));
}

std::pair<Metric, std::unordered_set<std::string>>
SpfSolver::SpfSolverImpl::getMinCostNodes(
    const SpfResult& spfResult, const std::set<std::string>& dstNodeNames) {
  Metric shortestMetric = std::numeric_limits<Metric>::max();

  // find the set of the closest nodes to our destination
  std::unordered_set<std::string> minCostNodes;
  for (const auto& dstNode : dstNodeNames) {
    auto it = spfResult.find(dstNode);
    if (it == spfResult.end()) {
      continue;
    }
    const auto nodeDistance = it->second.metric();
    if (shortestMetric >= nodeDistance) {
      if (shortestMetric > nodeDistance) {
        shortestMetric = nodeDistance;
        minCostNodes.clear();
      }
      minCostNodes.emplace(dstNode);
    }
  }

  return std::make_pair(shortestMetric, std::move(minCostNodes));
}

std::pair<
    Metric /* min metric to destination */,
    std::unordered_map<
        std::pair<std::string /* nextHopNodeName */, std::string /* dstNode */>,
        Metric /* the distance from the nexthop to the dest */>>
SpfSolver::SpfSolverImpl::getNextHopsWithMetric(
    const std::string& myNodeName,
    const std::set<std::string>& dstNodeNames,
    bool perDestination,
    std::unordered_map<std::string, LinkState> const& areaLinkStates) {
  // build up next hop nodes both nodes that are along a shortest path to the
  // prefix and, if enabled, those with an LFA path to the prefix
  std::unordered_map<
      std::pair<std::string /* nextHopNodeName */, std::string /* dstNode */>,
      Metric /* the distance from the nexthop to the dest */>
      nextHopNodes;
  Metric shortestMetric = std::numeric_limits<Metric>::max();

  for (auto const& [_, linkState] : areaLinkStates) {
    auto const& shortestPathsFromHere = linkState.getSpfResult(myNodeName);
    auto const& minMetricNodes =
        getMinCostNodes(shortestPathsFromHere, dstNodeNames);

    // Choose routes with lowest Metric
    // if Metric is the same, ecmp in multiple area
    // TODO: Should we consider ecmp in different area with different Metric?
    if (shortestMetric < minMetricNodes.first) {
      continue;
    }

    if (shortestMetric > minMetricNodes.first) {
      shortestMetric = minMetricNodes.first;
      nextHopNodes.clear();
    }

    auto const& minCostNodes = minMetricNodes.second;
    // If no node is reachable then return
    if (minCostNodes.empty()) {
      continue;
    }

    // Add neighbors with shortest path to the prefix
    for (const auto& dstNode : minCostNodes) {
      const auto dstNodeRef = perDestination ? dstNode : "";
      for (const auto& nhName : shortestPathsFromHere.at(dstNode).nextHops()) {
        nextHopNodes[std::make_pair(nhName, dstNodeRef)] = shortestMetric -
            linkState.getMetricFromAToB(myNodeName, nhName).value();
      }
    }

    // add any other neighbors that have LFA paths to the prefix
    if (computeLfaPaths_) {
      for (auto link : linkState.linksFromNode(myNodeName)) {
        if (!link->isUp()) {
          continue;
        }
        const auto& neighborName = link->getOtherNodeName(myNodeName);
        auto const& shortestPathsFromNeighbor =
            linkState.getSpfResult(neighborName);

        const auto neighborToHere =
            shortestPathsFromNeighbor.at(myNodeName).metric();
        for (const auto& dstNode : dstNodeNames) {
          auto shortestPathItr = shortestPathsFromNeighbor.find(dstNode);
          if (shortestPathItr == shortestPathsFromNeighbor.end()) {
            continue;
          }
          const auto distanceFromNeighbor = shortestPathItr->second.metric();

          // This is the LFA condition per RFC 5286
          if (distanceFromNeighbor < shortestMetric + neighborToHere) {
            const auto nextHopKey =
                std::make_pair(neighborName, perDestination ? dstNode : "");
            auto nextHopItr = nextHopNodes.find(nextHopKey);
            if (nextHopItr == nextHopNodes.end()) {
              nextHopNodes.emplace(nextHopKey, distanceFromNeighbor);
            } else if (nextHopItr->second > distanceFromNeighbor) {
              nextHopItr->second = distanceFromNeighbor;
            }
          } // end if
        } // end for dstNodeNames
      } // end for linkState.linksFromNode(myNodeName)
    }
  }

  return std::make_pair(shortestMetric, nextHopNodes);
}

std::unordered_set<thrift::NextHopThrift>
SpfSolver::SpfSolverImpl::getNextHopsThrift(
    const std::string& myNodeName,
    const std::set<std::string>& dstNodeNames,
    bool isV4,
    bool perDestination,
    const Metric minMetric,
    std::unordered_map<std::pair<std::string, std::string>, Metric>
        nextHopNodes,
    std::optional<int32_t> swapLabel,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    std::set<std::string> const& prefixAreas) const {
  CHECK(not nextHopNodes.empty());

  std::unordered_set<thrift::NextHopThrift> nextHops;
  for (const auto& [area, linkState] : areaLinkStates) {
    // ignore area that is not advertising the prefix
    if (not prefixAreas.count(area)) {
      continue;
    }

    for (const auto& link : linkState.linksFromNode(myNodeName)) {
      for (const auto& dstNode :
           perDestination ? dstNodeNames : std::set<std::string>{""}) {
        const auto neighborNode = link->getOtherNodeName(myNodeName);
        const auto search =
            nextHopNodes.find(std::make_pair(neighborNode, dstNode));

        // Ignore overloaded links or nexthops
        if (search == nextHopNodes.end() or not link->isUp()) {
          continue;
        }

        // Ignore link if other side of link is one of our destination and we
        // are trying to send to dstNode via neighbor (who is also our
        // destination)
        if (not dstNode.empty() and dstNodeNames.count(neighborNode) and
            neighborNode != dstNode) {
          continue;
        }

        // Ignore nexthops that are not shortest if lfa is disabled. All links
        // towards the nexthop on shortest path are LFA routes.
        Metric distOverLink =
            link->getMetricFromNode(myNodeName) + search->second;
        if (not computeLfaPaths_ and distOverLink != minMetric) {
          continue;
        }

        // Create associated mpls action if swapLabel is provided
        std::optional<thrift::MplsAction> mplsAction;
        if (swapLabel.has_value()) {
          CHECK(not mplsAction.has_value());
          const bool isNextHopAlsoDst = dstNodeNames.count(neighborNode);
          mplsAction = createMplsAction(
              isNextHopAlsoDst ? thrift::MplsActionCode::PHP
                               : thrift::MplsActionCode::SWAP,
              isNextHopAlsoDst ? std::nullopt : swapLabel);
        }

        // Create associated mpls action if dest node is not empty and
        // destination is not our neighbor
        if (not dstNode.empty() and dstNode != neighborNode) {
          // Validate mpls label before adding mplsAction
          auto const dstNodeLabel =
              linkState.getAdjacencyDatabases().at(dstNode).nodeLabel;
          if (not isMplsLabelValid(dstNodeLabel)) {
            continue;
          }
          CHECK(not mplsAction.has_value());
          mplsAction = createMplsAction(
              thrift::MplsActionCode::PUSH,
              std::nullopt,
              std::vector<int32_t>{dstNodeLabel});
        }

        // if we are computing LFA paths, any nexthop to the node will do
        // otherwise, we only want those nexthops along a shortest path
        nextHops.emplace(createNextHop(
            isV4 ? link->getNhV4FromNode(myNodeName)
                 : link->getNhV6FromNode(myNodeName),
            link->getIfaceFromNode(myNodeName),
            distOverLink,
            mplsAction,
            false /* useNonShortestRoute */,
            link->getArea()));
      } // end for perDestination ...
    } // end for linkState ...
  }
  return nextHops;
}

//
// Public SpfSolver
//

SpfSolver::SpfSolver(
    const std::string& myNodeName,
    bool enableV4,
    bool computeLfaPaths,
    bool enableOrderedFib,
    bool bgpDryRun,
    bool bgpUseIgpMetric)
    : impl_(new SpfSolver::SpfSolverImpl(
          myNodeName,
          enableV4,
          computeLfaPaths,
          enableOrderedFib,
          bgpDryRun,
          bgpUseIgpMetric)) {}

SpfSolver::~SpfSolver() {}

bool
SpfSolver::staticRoutesUpdated() {
  return impl_->staticRoutesUpdated();
}

void
SpfSolver::pushRoutesDeltaUpdates(
    thrift::RouteDatabaseDelta& staticRoutesDelta) {
  return impl_->pushRoutesDeltaUpdates(staticRoutesDelta);
}

thrift::StaticRoutes const&
SpfSolver::getStaticRoutes() {
  return impl_->getStaticRoutes();
}

std::optional<DecisionRouteDb>
SpfSolver::buildRouteDb(
    const std::string& myNodeName,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixState const& prefixState) {
  return impl_->buildRouteDb(myNodeName, areaLinkStates, prefixState);
}

std::optional<DecisionRouteUpdate>
SpfSolver::processStaticRouteUpdates() {
  return impl_->processStaticRouteUpdates();
}

//
// Decision class implementation
//

Decision::Decision(
    std::shared_ptr<const Config> config,
    bool computeLfaPaths,
    bool bgpDryRun,
    std::chrono::milliseconds debounceMinDur,
    std::chrono::milliseconds debounceMaxDur,
    messaging::RQueue<thrift::Publication> kvStoreUpdatesQueue,
    messaging::RQueue<thrift::RouteDatabaseDelta> staticRoutesUpdateQueue,
    messaging::ReplicateQueue<DecisionRouteUpdate>& routeUpdatesQueue)
    : config_(config),
      routeUpdatesQueue_(routeUpdatesQueue),
      myNodeName_(config->getConfig().node_name),
      pendingUpdates_(config->getConfig().node_name),
      rebuildRoutesDebounced_(
          getEvb(), debounceMinDur, debounceMaxDur, [this]() noexcept {
            rebuildRoutes("DECISION_DEBOUNCE");
          }) {
  auto tConfig = config->getConfig();
  spfSolver_ = std::make_unique<SpfSolver>(
      tConfig.node_name,
      tConfig.enable_v4_ref().value_or(false),
      computeLfaPaths,
      tConfig.enable_ordered_fib_programming_ref().value_or(false),
      bgpDryRun,
      tConfig.bgp_use_igp_metric_ref().value_or(false));

  coldStartTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    pendingUpdates_.setNeedsFullRebuild();
    rebuildRoutes("COLD_START_UPDATE");
  });
  if (auto eor = config->getConfig().eor_time_s_ref()) {
    coldStartTimer_->scheduleTimeout(std::chrono::seconds(*eor));
  }

  // Schedule periodic timer for counter submission
  counterUpdateTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    updateGlobalCounters();
    // Schedule next counters update
    counterUpdateTimer_->scheduleTimeout(Constants::kCounterSubmitInterval);
  });
  counterUpdateTimer_->scheduleTimeout(Constants::kCounterSubmitInterval);

  // Schedule periodic timer to decremtOrderedFibHolds
  if (tConfig.enable_ordered_fib_programming_ref().value_or(false)) {
    orderedFibTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
      LOG(INFO) << "Decrementing Holds";
      if (decrementOrderedFibHolds()) {
        auto timeout = getMaxFib();
        LOG(INFO) << "Scheduling next hold decrement in " << timeout.count()
                  << "ms";
        orderedFibTimer_->scheduleTimeout(getMaxFib());
      }
    });
  }

  // Add reader to process publication from KvStore
  addFiberTask([q = std::move(kvStoreUpdatesQueue), this]() mutable noexcept {
    LOG(INFO) << "Starting KvStore updates processing fiber";
    while (true) {
      auto maybeThriftPub = q.get(); // perform read
      VLOG(2) << "Received KvStore update";
      if (maybeThriftPub.hasError()) {
        LOG(INFO) << "Terminating KvStore updates processing fiber";
        break;
      }
      try {
        processPublication(maybeThriftPub.value());
      } catch (const std::exception& e) {
#if FOLLY_USE_SYMBOLIZER
        // collect stack strace then fail the process
        for (auto& exInfo : folly::exception_tracer::getCurrentExceptions()) {
          LOG(ERROR) << exInfo;
        }
#endif
        // FATAL to produce core dump
        LOG(FATAL) << "Exception occured in Decision::processPublication - "
                   << folly::exceptionStr(e);
      }
      // compute routes with exponential backoff timer if needed
      if (pendingUpdates_.needsRouteUpdate()) {
        rebuildRoutesDebounced_();
      }
    }
  });

  // Add reader to process publication from KvStore
  addFiberTask(
      [q = std::move(staticRoutesUpdateQueue), this]() mutable noexcept {
        LOG(INFO) << "Starting static routes update processing fiber";
        while (true) {
          auto maybeThriftPub = q.get(); // perform read
          VLOG(2) << "Received static routes update";
          if (maybeThriftPub.hasError()) {
            LOG(INFO) << "Terminating prefix manager update processing fiber";
            break;
          }
          // Apply publication and update stored update status
          pushRoutesDeltaUpdates(maybeThriftPub.value());
          rebuildRoutesDebounced_();
        }
      });

  // Create RibPolicy timer to process routes on policy expiry
  ribPolicyTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    LOG(WARNING) << "RibPolicy is expired";
    pendingUpdates_.setNeedsFullRebuild();
    rebuildRoutes("RIB_POLICY_EXPIRED");
  });
}

folly::SemiFuture<std::unique_ptr<thrift::RouteDatabase>>
Decision::getDecisionRouteDb(std::string nodeName) {
  folly::Promise<std::unique_ptr<thrift::RouteDatabase>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([p = std::move(p), nodeName, this]() mutable {
    thrift::RouteDatabase routeDb;

    if (nodeName.empty()) {
      nodeName = myNodeName_;
    }
    auto maybeRouteDb =
        spfSolver_->buildRouteDb(nodeName, areaLinkStates_, prefixState_);
    if (maybeRouteDb.has_value()) {
      routeDb = maybeRouteDb->toThrift();
    }

    // static routes
    for (const auto& [key, val] : spfSolver_->getStaticRoutes().mplsRoutes) {
      routeDb.mplsRoutes.emplace_back(createMplsRoute(key, val));
    }

    routeDb.thisNodeName = nodeName;
    p.setValue(std::make_unique<thrift::RouteDatabase>(std::move(routeDb)));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<thrift::StaticRoutes>>
Decision::getDecisionStaticRoutes() {
  folly::Promise<std::unique_ptr<thrift::StaticRoutes>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([p = std::move(p), this]() mutable {
    auto staticRoutes = spfSolver_->getStaticRoutes();
    p.setValue(std::make_unique<thrift::StaticRoutes>(std::move(staticRoutes)));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<thrift::AdjDbs>>
Decision::getDecisionAdjacencyDbs() {
  folly::Promise<std::unique_ptr<thrift::AdjDbs>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([p = std::move(p), this]() mutable {
    auto search =
        areaLinkStates_.find(thrift::KvStore_constants::kDefaultArea());
    p.setValue(std::make_unique<thrift::AdjDbs>(
        search != areaLinkStates_.end() ? search->second.getAdjacencyDatabases()
                                        : thrift::AdjDbs{}));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdjacencyDatabase>>>
Decision::getAllDecisionAdjacencyDbs() {
  folly::Promise<std::unique_ptr<std::vector<thrift::AdjacencyDatabase>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([p = std::move(p), this]() mutable {
    auto adjDbs = std::make_unique<std::vector<thrift::AdjacencyDatabase>>();
    for (auto const& [_, linkState] : areaLinkStates_) {
      for (auto const& [_, db] : linkState.getAdjacencyDatabases()) {
        adjDbs->push_back(db);
      }
    }
    p.setValue(std::move(adjDbs));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<thrift::PrefixDbs>>
Decision::getDecisionPrefixDbs() {
  folly::Promise<std::unique_ptr<thrift::PrefixDbs>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([p = std::move(p), this]() mutable {
    p.setValue(
        std::make_unique<thrift::PrefixDbs>(prefixState_.getPrefixDatabases()));
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
Decision::setRibPolicy(thrift::RibPolicy const& ribPolicyThrift) {
  auto [p, sf] = folly::makePromiseContract<folly::Unit>();
  if (not config_->isRibPolicyEnabled()) {
    thrift::OpenrError error;
    error.message = "RibPolicy feature is not enabled";
    p.setException(error);
    return std::move(sf);
  }

  std::unique_ptr<RibPolicy> ribPolicy;
  try {
    ribPolicy = std::make_unique<RibPolicy>(ribPolicyThrift);
  } catch (thrift::OpenrError const& e) {
    p.setException(e);
    return std::move(sf);
  }

  runInEventBaseThread(
      [this, p = std::move(p), ribPolicy = std::move(ribPolicy)]() mutable {
        const auto durationLeft = ribPolicy->getTtlDuration();
        if (durationLeft.count() <= 0) {
          LOG(ERROR)
              << "Ignoring RibPolicy update with new instance because of "
              << "staleness. Validity " << durationLeft.count() << "ms";
          return;
        }

        // Update local policy instance
        LOG(INFO) << "Updating RibPolicy with new instance. Validity "
                  << durationLeft.count() << "ms";
        ribPolicy_ = std::move(ribPolicy);

        // Schedule timer for processing routes on expiry
        ribPolicyTimer_->scheduleTimeout(durationLeft);

        // Trigger route computation
        pendingUpdates_.setNeedsFullRebuild();
        rebuildRoutes("RIB_POLICY_UPDATE");

        // Mark the policy update request to be done
        p.setValue();
      });
  return std::move(sf);
}

folly::SemiFuture<thrift::RibPolicy>
Decision::getRibPolicy() {
  auto [p, sf] = folly::makePromiseContract<thrift::RibPolicy>();
  if (not config_->isRibPolicyEnabled()) {
    thrift::OpenrError error;
    error.message = "RibPolicy feature is not enabled";
    p.setException(error);
    return std::move(sf);
  }

  runInEventBaseThread([this, p = std::move(p)]() mutable {
    if (ribPolicy_) {
      p.setValue(ribPolicy_->toThrift());
    } else {
      thrift::OpenrError e;
      e.message = "RibPolicy is not configured";
      p.setException(e);
    }
  });
  return std::move(sf);
}

std::optional<thrift::PrefixDatabase>
Decision::updateNodePrefixDatabase(
    const std::string& key, const thrift::PrefixDatabase& prefixDb) {
  auto const& nodeName = prefixDb.thisNodeName;

  auto prefixKey = PrefixKey::fromStr(key);
  // per prefix key
  if (prefixKey.hasValue()) {
    if (prefixDb.deletePrefix) {
      perPrefixPrefixEntries_[nodeName].erase(prefixKey.value().getIpPrefix());
    } else {
      CHECK_EQ(1, prefixDb.prefixEntries_ref()->size());
      auto prefixEntry = prefixDb.prefixEntries_ref()->at(0);

      // Ignore self redistributed route reflection
      // These routes are programmed by Decision,
      // re-origintaed by me to areas that do not have the best prefix entry
      if (nodeName == myNodeName_ && prefixEntry.area_stack_ref()->size() > 0 &&
          areaLinkStates_.count(prefixEntry.area_stack_ref()->at(0))) {
        return std::nullopt;
      }

      perPrefixPrefixEntries_[nodeName][prefixKey.value().getIpPrefix()] =
          prefixEntry;
    }
  } else {
    fullDbPrefixEntries_[nodeName].clear();
    for (auto const& entry : prefixDb.prefixEntries) {
      fullDbPrefixEntries_[nodeName][entry.prefix] = entry;
    }
  }

  thrift::PrefixDatabase nodePrefixDb;
  nodePrefixDb.thisNodeName = nodeName;
  nodePrefixDb.perfEvents_ref().copy_from(prefixDb.perfEvents_ref());
  nodePrefixDb.prefixEntries.reserve(perPrefixPrefixEntries_[nodeName].size());
  for (auto& kv : perPrefixPrefixEntries_[nodeName]) {
    nodePrefixDb.prefixEntries.emplace_back(kv.second);
  }
  for (auto& kv : fullDbPrefixEntries_[nodeName]) {
    if (not perPrefixPrefixEntries_[nodeName].count(kv.first)) {
      nodePrefixDb.prefixEntries.emplace_back(kv.second);
    }
  }
  return nodePrefixDb;
}

void
Decision::processPublication(thrift::Publication const& thriftPub) {
  CHECK(not thriftPub.area.empty());
  auto const& area = thriftPub.area;

  if (!areaLinkStates_.count(area)) {
    areaLinkStates_.emplace(area, area);
  }
  auto& areaLinkState = areaLinkStates_.at(area);

  // LSDB addition/update
  // deserialize contents of every LSDB key

  // Nothing to process if no adj/prefix db changes
  if (thriftPub.keyVals.empty() and thriftPub.expiredKeys.empty()) {
    return;
  }

  for (const auto& kv : thriftPub.keyVals) {
    const auto& key = kv.first;
    const auto& rawVal = kv.second;
    std::string nodeName = getNodeNameFromKey(key);

    if (not rawVal.value_ref().has_value()) {
      // skip TTL update
      DCHECK(rawVal.ttlVersion > 0);
      continue;
    }

    try {
      if (key.find(Constants::kAdjDbMarker.toString()) == 0) {
        // update adjacencyDb
        auto adjacencyDb =
            fbzmq::util::readThriftObjStr<thrift::AdjacencyDatabase>(
                rawVal.value_ref().value(), serializer_);
        // TODO this sould come from KvStore.
        adjacencyDb.area_ref() = area;
        CHECK_EQ(nodeName, adjacencyDb.thisNodeName);
        LinkStateMetric holdUpTtl = 0, holdDownTtl = 0;
        if (config_->getConfig().enable_ordered_fib_programming_ref().value_or(
                false)) {
          if (auto maybeHoldUpTtl = areaLinkState.getHopsFromAToB(
                  myNodeName_, adjacencyDb.thisNodeName)) {
            holdUpTtl = maybeHoldUpTtl.value();
            holdDownTtl =
                areaLinkState.getMaxHopsToNode(adjacencyDb.thisNodeName) -
                holdUpTtl;
          }
        }
        fb303::fbData->addStatValue("decision.adj_db_update", 1, fb303::COUNT);
        pendingUpdates_.applyLinkStateChange(
            adjacencyDb.thisNodeName,
            areaLinkState.updateAdjacencyDatabase(
                adjacencyDb, holdUpTtl, holdDownTtl),
            castToStd(adjacencyDb.perfEvents_ref()));
        if (areaLinkState.hasHolds() && orderedFibTimer_ != nullptr &&
            !orderedFibTimer_->isScheduled()) {
          orderedFibTimer_->scheduleTimeout(getMaxFib());
        }
        continue;
      }

      if (key.find(Constants::kPrefixDbMarker.toString()) == 0) {
        // update prefixDb
        auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            rawVal.value_ref().value(), serializer_);
        CHECK_EQ(nodeName, prefixDb.thisNodeName);
        auto maybeNodePrefixDb = updateNodePrefixDatabase(key, prefixDb);
        if (not maybeNodePrefixDb.has_value()) {
          continue;
        }
        auto nodePrefixDb = maybeNodePrefixDb.value();
        // TODO - this should directly come from KvStore.
        nodePrefixDb.area = area;
        VLOG(1) << "Updating prefix database for node " << nodeName
                << " from area " << area;
        fb303::fbData->addStatValue(
            "decision.prefix_db_update", 1, fb303::COUNT);
        pendingUpdates_.applyPrefixStateChange(
            prefixState_.updatePrefixDatabase(nodePrefixDb)),
            castToStd(nodePrefixDb.perfEvents_ref());
        continue;
      }

      if (key.find(Constants::kFibTimeMarker.toString()) == 0) {
        try {
          std::chrono::milliseconds fibTime{stoll(rawVal.value_ref().value())};
          fibTimes_[nodeName] = fibTime;
        } catch (...) {
          LOG(ERROR) << "Could not convert "
                     << Constants::kFibTimeMarker.toString()
                     << " value to int64";
        }
        continue;
      }
    } catch (const std::exception& e) {
      LOG(ERROR) << "Failed to deserialize info for key " << key
                 << ". Exception: " << folly::exceptionStr(e);
    }
  }

  // LSDB deletion
  for (const auto& key : thriftPub.expiredKeys) {
    std::string nodeName = getNodeNameFromKey(key);

    if (key.find(Constants::kAdjDbMarker.toString()) == 0) {
      pendingUpdates_.applyLinkStateChange(
          nodeName,
          areaLinkState.deleteAdjacencyDatabase(nodeName),
          castToStd(thrift::PrefixDatabase().perfEvents_ref()));
      continue;
    }

    if (key.find(Constants::kPrefixDbMarker.toString()) == 0) {
      // manually build delete prefix db to signal delete just as a client would
      thrift::PrefixDatabase deletePrefixDb;
      deletePrefixDb.thisNodeName = nodeName;
      deletePrefixDb.deletePrefix = true;

      auto maybeNodePrefixDb = updateNodePrefixDatabase(key, deletePrefixDb);
      if (not maybeNodePrefixDb.has_value()) {
        continue;
      }
      auto nodePrefixDb = maybeNodePrefixDb.value();

      // TODO - this should directly come from KvStore.
      nodePrefixDb.area = area;
      pendingUpdates_.applyPrefixStateChange(
          prefixState_.updatePrefixDatabase(nodePrefixDb));
      continue;
    }
  }
}

void
Decision::pushRoutesDeltaUpdates(
    thrift::RouteDatabaseDelta& staticRoutesDelta) {
  spfSolver_->pushRoutesDeltaUpdates(staticRoutesDelta);
}

void
Decision::rebuildRoutes(std::string const& event) {
  if (coldStartTimer_->isScheduled()) {
    return;
  }

  pendingUpdates_.addEvent(event);
  VLOG(1) << "Decision: processing " << pendingUpdates_.getCount()
          << " accumulated updates.";
  if (pendingUpdates_.perfEvents()) {
    if (auto expectedDuration = getDurationBetweenPerfEvents(
            *pendingUpdates_.perfEvents(),
            "DECISION_RECEIVED",
            "DECISION_DEBOUNCE")) {
      VLOG(1) << "Debounced " << pendingUpdates_.getCount() << " events over "
              << expectedDuration->count() << "ms.";
    }
  }
  // we need to update  static route first, because there maybe routes
  // depending on static routes.
  bool staticRoutesUpdated{false};
  if (spfSolver_->staticRoutesUpdated()) {
    staticRoutesUpdated = true;
    if (auto maybeRouteDbDelta = spfSolver_->processStaticRouteUpdates()) {
      routeUpdatesQueue_.push(std::move(maybeRouteDbDelta.value()));
    }
  }

  std::optional<DecisionRouteDb> maybeRouteDb = std::nullopt;
  if (pendingUpdates_.needsRouteUpdate() || staticRoutesUpdated) {
    // if only static routes gets updated, we still need to update routes
    // because there maybe routes depended on static routes.
    maybeRouteDb =
        spfSolver_->buildRouteDb(myNodeName_, areaLinkStates_, prefixState_);
  }
  pendingUpdates_.addEvent("ROUTE_UPDATE");
  if (maybeRouteDb.has_value()) {
    sendRouteUpdate(std::move(*maybeRouteDb), pendingUpdates_.moveOutEvents());
  } else {
    LOG(WARNING) << "rebuildRoutes incurred no routes";
  }

  pendingUpdates_.reset();
}

bool
Decision::decrementOrderedFibHolds() {
  bool topoChanged = false;
  bool stillHasHolds = false;
  for (auto& [_, linkState] : areaLinkStates_) {
    pendingUpdates_.applyLinkStateChange(
        myNodeName_, linkState.decrementHolds());
    stillHasHolds |= linkState.hasHolds();
  }
  if (pendingUpdates_.needsRouteUpdate()) {
    rebuildRoutes("ORDERED_FIB_HOLDS_EXPIRED");
  }
  return stillHasHolds;
}

void
Decision::sendRouteUpdate(
    DecisionRouteDb&& routeDb, std::optional<thrift::PerfEvents>&& perfEvents) {
  //
  // Apply RibPolicy to computed route db before sending out
  //
  if (ribPolicy_ && ribPolicy_->isActive()) {
    auto i = routeDb.unicastEntries.begin();
    while (i != routeDb.unicastEntries.end()) {
      auto& entry = i->second;
      if (ribPolicy_->applyAction(entry)) {
        VLOG(1) << "RibPolicy transformed the route "
                << folly::IPAddress::networkToString(entry.prefix);
      }
      // Skip route if no valid next-hop
      if (entry.nexthops.empty()) {
        VLOG(1) << "Removing route for "
                << folly::IPAddress::networkToString(entry.prefix)
                << " because of no remaining valid next-hops";
        i = routeDb.unicastEntries.erase(i);
        continue;
      }
      ++i;
    }
  }

  auto delta = getRouteDelta(routeDb, routeDb_);

  // update decision routeDb cache
  routeDb_ = std::move(routeDb);

  // publish the new route state to fib
  delta.perfEvents = perfEvents;
  routeUpdatesQueue_.push(std::move(delta));
}

std::chrono::milliseconds
Decision::getMaxFib() {
  std::chrono::milliseconds maxFib{1};
  for (auto& kv : fibTimes_) {
    maxFib = std::max(maxFib, kv.second);
  }
  return maxFib;
}

void
Decision::updateGlobalCounters() const {
  size_t numAdjacencies = 0, numPartialAdjacencies = 0;
  std::unordered_set<std::string> nodeSet;
  for (auto const& [_, linkState] : areaLinkStates_) {
    numAdjacencies += linkState.numLinks();
    auto const& mySpfResult = linkState.getSpfResult(myNodeName_);
    for (auto const& kv : linkState.getAdjacencyDatabases()) {
      nodeSet.insert(kv.first);
      const auto& adjDb = kv.second;
      size_t numLinks = linkState.linksFromNode(kv.first).size();
      // Consider partial adjacency only iff node is reachable from current node
      if (mySpfResult.count(adjDb.thisNodeName) && 0 != numLinks) {
        // only add to the count if this node is not completely disconnected
        size_t diff = adjDb.adjacencies.size() - numLinks;
        // Number of links (bi-directional) must be <= number of adjacencies
        CHECK_GE(diff, 0);
        numPartialAdjacencies += diff;
      }
    }
  }

  // Add custom counters
  fb303::fbData->setCounter(
      "decision.num_partial_adjacencies", numPartialAdjacencies);
  fb303::fbData->setCounter(
      "decision.num_complete_adjacencies", numAdjacencies);
  // When node has no adjacencies then linkState reports 0
  fb303::fbData->setCounter(
      "decision.num_nodes", std::max(nodeSet.size(), static_cast<size_t>(1ul)));
  fb303::fbData->setCounter(
      "decision.num_prefixes", prefixState_.prefixes().size());
  fb303::fbData->setCounter(
      "decision.num_nodes_v4_loopbacks",
      prefixState_.getNodeHostLoopbacksV4().size());
  fb303::fbData->setCounter(
      "decision.num_nodes_v6_loopbacks",
      prefixState_.getNodeHostLoopbacksV6().size());
}

} // namespace openr
