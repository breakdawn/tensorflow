/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// HLO shardings describe how an HLO instruction is split across multiple
// computations.

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_HLO_SHARDING_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_HLO_SHARDING_H_

#include <map>
#include <string>
#include <vector>

#include "tensorflow/compiler/xla/array.h"
#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/protobuf_util.h"
#include "tensorflow/compiler/xla/shape_tree.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/lib/gtl/array_slice.h"
#include "tensorflow/core/lib/hash/hash.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/types.h"

namespace xla {

// HLO shardings describe how an HLO instruction is split across multiple
// computations.
class HloSharding {
 public:
  // Creates a trivial sharding that replicates a maximal tile across all
  // devices.
  static HloSharding Replicate() { return HloSharding(); }

  // Creates a sharding that emulates device placement; a tile shape equal to
  // the input shape (one tile) assigned to a single device.
  static HloSharding AssignDevice(int64 device_id);

  // Creates a new sharding which splits a shape into tiles each with shape
  // `tile_shape`. Each tile is assigned to one device, which is specified by
  // `tile_assignment`. Any tensor not a multiple of the tile size in any
  // dimension is implicitly padded to the tile size.
  //
  // e.g. Tile({2, 2}, {0, 1}) on a tensor of shape {3, 2} would look like:
  //      2     1 padding
  //   <------><->
  //   +----+----+
  //   | 0  |  1 |
  //   +----+----+
  //
  // Split into two tiles, one of which is implicitly padded by one.
  static HloSharding Tile(const Shape& tile_shape,
                          const Array<int64>& tile_assignment) {
    return HloSharding(tile_shape, tile_assignment);
  }

  // Creates a new sharding which splits a one-dimensional input shape into
  // `num_tiles` tiles.
  static HloSharding Tile1D(const Shape& input_shape, int64 num_tiles);

  // Creates a new sharding for a tuple type. The given ShapeTree must have
  // elements for every leaf shape contained in the tuple.
  static HloSharding Tuple(const ShapeTree<HloSharding>& sub_shardings);

  // Creates a new sharding for a tuple type. The number of elements in
  // shardings must match the number of leaf nodes in tuple_shape. For
  // empty tuples, the shardings array must have one element.
  static HloSharding Tuple(const Shape& tuple_shape,
                           tensorflow::gtl::ArraySlice<HloSharding> shardings);

  // Create a new sharding from a protobuf OpSharding.
  static StatusOr<HloSharding> FromProto(const OpSharding& proto);

  // Checks whether device is a reserved device number. A reserved device number
  // has usually a special meaning, with dedicated handling logic.
  static bool IsReservedDevice(int64 device) { return device < 0; }

  OpSharding ToProto() const;

  // Note that this string canonically has outer curly braces, e.g.
  // "{replicated}".
  string ToString() const;

  // Validate that this sharding can be applied to a tensor with shape `shape`.
  Status Validate(const Shape& shape, int64 num_devices) const;

  // Returns true if the sharding has tuple type.
  bool IsTuple() const { return tuple_; }

  // Returns true if the sharding is trivial: replicate on all devices.
  bool IsReplicated() const {
    if (!IsTuple()) {
      return replicated_;
    }
    return std::all_of(tuple_elements_.begin(), tuple_elements_.end(),
                       [](const HloSharding& s) { return s.IsReplicated(); });
  }

  // Returns true if the tile size is the same as the input size.
  bool IsTileMaximal() const {
    if (!IsTuple()) {
      return maximal_;
    }
    return std::all_of(tuple_elements_.begin(), tuple_elements_.end(),
                       [](const HloSharding& s) { return s.IsTileMaximal(); });
  }

  // Returns true if the sharding defines an operation on the given device.
  bool UsesDevice(int64 device) const;

  // Retrieves an histogram of the devices used by the sharding. The returned
  // map has the device number as key, and the occurrence count as value.
  // If a sharding does not have a device, it will not be incuded in the
  // histogram. The count argument, if not nullptr, will receive the total
  // number of elements this sharding is made of (one for array, N leaves for
  // tuples).
  std::map<int64, int64> UsedDevices(int64* count) const;

  // Returns the tile that should be executed on the given device.
  // REQUIRES: !IsTuple()
  std::vector<int64> TileIndexForDevice(int64 device) const;

  // Returns the device that should execute the given tile.
  // It is an error to call this if is_replicated() is true.
  // REQUIRES: !IsTuple()
  int64 DeviceForTileIndex(tensorflow::gtl::ArraySlice<int64> index) const;

  // Given a device ID, returns the offset within the input space of the
  // tile that should be executed on the given core. This returns the lower
  // extent of the tile in the input space.
  // REQUIRES: !IsTuple()
  std::vector<int64> TileOffsetForDevice(int64 device) const;

  // Given a device ID, returns the limit within the input space of the
  // tile that should be executed on the given core. This returns the upper
  // extent of the tile in the input space.
  // REQUIRES: !IsTuple()
  std::vector<int64> TileLimitForDevice(int64 device) const;

  // Returns the single device this op operates on.
  // REQUIRES: !IsTuple&& !Replicated() && IsTileMaximal()
  StatusOr<int64> UniqueDevice() const;

  // Returns true if this op only uses a single device.
  bool HasUniqueDevice() const;

  // Returns the ShapeTree containing the shardings for each element of this
  // tuple, if IsTuple, or a ShapeTree with a single element containing this
  // sharding. Only the leaf elements are populated. This creates a new
  // ShapeTree object so is not cheap.
  StatusOr<ShapeTree<HloSharding>> AsShapeTree(const Shape& shape) const;
  ShapeTree<HloSharding> GetAsShapeTree(const Shape& shape) const {
    return AsShapeTree(shape).ValueOrDie();
  }

  // Retrieves the sub sharding at a given index, out of a tuple sharding.
  // REQUIRES: IsTuple()
  HloSharding GetSubSharding(const Shape& shape, const ShapeIndex& index) const;

  // If the current sharding is a tuple sharding, return itself as result.
  // Otherwise returns a tuple sharding for the input shape, with all the leaves
  // having this object sharding.
  StatusOr<HloSharding> GetTupleSharding(const Shape& shape) const;

  // Extracts the sharding that is common within the current sharding.
  // If the current sharding is not a tuple sharding, the current sharding will
  // be returned. If it is a tuple, and all the tuple elements are common, the
  // common element will be returned. Otherwise the optional will contain no
  // value.
  tensorflow::gtl::optional<HloSharding> ExtractSingleSharding() const;

  bool operator==(const HloSharding& other) const {
    return replicated_ == other.replicated_ && maximal_ == other.maximal_ &&
           ShapeUtil::Compatible(tile_shape_, other.tile_shape_) &&
           tile_assignment_ == other.tile_assignment_ &&
           tuple_elements_ == other.tuple_elements_;
  }
  bool operator!=(const HloSharding& other) const { return !(*this == other); }

  size_t Hash() const;

  struct Hasher {
    size_t operator()(const HloSharding& sharding) const {
      return sharding.Hash();
    }
  };

  // Gets the tile shape.
  // REQUIRES: !IsTileMaximal() && !IsTuple()
  const Shape& tile_shape() const { return tile_shape_; }
  // Gets the tile assignment tensor.
  // REQUIRES: !IsReplicated() && !IsTuple()
  const Array<int64>& tile_assignment() const { return tile_assignment_; }

  // Returns the flattened list of all the leaf shardings in a tuple shape, by
  // pre-order walk (ShapeTree iterator order).
  // REQUIRES: IsTuple().
  const std::vector<HloSharding>& tuple_elements() const {
    return tuple_elements_;
  }

  // Return a new sharding that can apply to the given new shape.
  // If this sharding is tile-maximal, the returned sharding will be the same as
  // this sharding. If this sharding is not tile-maximal, the returned
  // sharding's tile size will differ:
  //   - Non-sharded dimensions will be adapted to be the same as `new_shape`;
  //     tile_dimension(i) = new_shape.dimensions(i);
  //   - Sharded dimensions will be kept the same unless `transform` is supplied
  //     in which case tile_dimension(i) = transform(i, tile_dimension(i));
  // REQUIRES: !IsTuple().
  HloSharding TransformShardedTileShape(
      const Shape& new_shape,
      const std::function<int64(int64, int64)>& transform = nullptr) const;

 private:
  HloSharding()
      : replicated_(true),
        maximal_(true),
        tuple_(false),
        tile_shape_(),
        tile_assignment_({0}) {}
  // device_id values:
  // -2: magic number to mean unassigned device, used by spatial partitioning
  // -1: the id of the host
  //  0 or positive: the id of a device
  // NOTE(dimvar): -1 is needed for outside compilation. It can be removed once
  // we have fully switched to the side-effect tokens.
  explicit HloSharding(int64 device_id)
      : replicated_(false),
        maximal_(true),
        tuple_(false),
        tile_shape_(),
        tile_assignment_({1}, device_id) {}
  HloSharding(const Shape& tile_shape, const Array<int64>& tile_assignment)
      : replicated_(false),
        maximal_(false),
        tuple_(false),
        tile_shape_(tile_shape),
        tile_assignment_(tile_assignment) {}
  HloSharding(const std::vector<HloSharding>& tuple_shardings)
      : replicated_(false),
        maximal_(false),
        tuple_(true),
        tile_assignment_({0}),
        tuple_elements_(tuple_shardings) {}

  // Checks that the number of elements in tuple_elements_ is consistent with
  // the tuple shape passes as argument.
  Status CheckLeafCount(const Shape& shape) const;

  // Internal helper to validate a tuple sharding.
  Status ValidateTuple(const Shape& shape, int64 num_devices) const;

  // Internal helper to validate a non-tuple (leaf) sharding.
  Status ValidateNonTuple(const Shape& shape, int64 num_devices) const;

  // Returns the number of tuple_elements_ entries to fit the shape.
  static int64 RequiredLeaves(const Shape& shape);

  bool replicated_;
  bool maximal_;
  bool tuple_;
  Shape tile_shape_;
  Array<int64> tile_assignment_;
  // Only non-empty when tuple_ is true, but because empty tuples are allowed
  // may also be empty even then. This is a flattened list of all the leaf
  // shardings in a tuple shape, by pre-order walk (ShapeTree iterator order).
  std::vector<HloSharding> tuple_elements_;
};

std::ostream& operator<<(std::ostream& out, const HloSharding& sharding);

}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_HLO_SHARDING_H_
