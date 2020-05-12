#pragma once

#include "datastructures/PointBuffer.h"
#include "io/PointsPersistence.h"
#include "process/Tiler.h"
#include "tiling/Node.h"
#include "tiling/Sampling.h"

#include <memory>
#include <taskflow/taskflow.hpp>
#include <vector>

struct ProgressReporter;

/**
 * Helper structure that stores PointBuffer objects in a thread-safe manner. This is used to cache
 * the points loaded from disk at each node.
 */
struct PointsCache
{
  PointsCache() {}
  PointsCache(const PointsCache&) = delete;
  PointsCache(PointsCache&&) = delete;
  PointsCache& operator=(const PointsCache&) = delete;
  PointsCache& operator=(PointsCache&&) = delete;

  PointBuffer& emplace_points(PointBuffer&& points)
  {
    std::lock_guard guard{ _lock };
    _cache.push_back(std::make_unique<PointBuffer>(std::move(points)));
    return *_cache.back();
  }

  void clear()
  {
    std::lock_guard guard{ _lock };
    _cache.clear();
  }

private:
  std::vector<std::unique_ptr<PointBuffer>> _cache;
  std::mutex _lock;
};

/**
 * Base class for different tiling algorithms
 */
struct TilingAlgorithmBase
{
  /**
   * Build an execution graph for tiling the given PointBuffer
   */
  virtual void build_execution_graph(PointBuffer& points, const AABB& bounds, tf::Taskflow& tf) = 0;
};

/**
 * Version 1 of the tiling algorithm, as presented in the first version of my paper. It uses:
 *
 * -  Parallel indexing
 * -  Sequential sorting
 * -  Processing from the root node
 */
struct TilingAlgorithmV1 : TilingAlgorithmBase
{
  TilingAlgorithmV1(SamplingStrategy& sampling_strategy,
                    ProgressReporter* progress_reporter,
                    PointsPersistence& persistence,
                    TilerMetaParameters meta_parameters,
                    size_t concurrency);

  void build_execution_graph(PointBuffer& points, const AABB& bounds, tf::Taskflow& tf) override;

private:
  SamplingStrategy& _sampling_strategy;
  ProgressReporter* _progress_reporter;
  PointsPersistence& _persistence;
  TilerMetaParameters _meta_parameters;
  size_t _concurrency;

  octree::NodeData _root_node_points;
  PointsCache _points_cache;
};

/**
 * Optimized version of the tiling algorithm. It uses:
 *
 * -  Parallel indexing
 * -  Skipping root node and selecting 'desired_parallelism' nodes for processing
 * -  Parallel sorting for each selected node
 *
 * In general, a lot of parallel map/reduce operations
 */
struct TilingAlgorithmV2 : TilingAlgorithmBase
{
  TilingAlgorithmV2(SamplingStrategy& sampling_strategy,
                    ProgressReporter* progress_reporter,
                    PointsPersistence& persistence,
                    TilerMetaParameters meta_parameters,
                    size_t concurrency);

  void build_execution_graph(PointBuffer& points, const AABB& bounds, tf::Taskflow& tf) override;

private:
  SamplingStrategy& _sampling_strategy;
  ProgressReporter* _progress_reporter;
  PointsPersistence& _persistence;
  TilerMetaParameters _meta_parameters;
  size_t _concurrency;

  octree::NodeData _root_node_points;
  PointsCache _points_cache;
};