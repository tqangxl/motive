// Copyright 2015 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <string>
#include <sstream>
#include <vector>
#include "motive/math/bulk_spline_evaluator.h"
#include "motive/math/dual_cubic.h"

using mathfu::Lerp;

namespace fpl {

static const uint32_t kMask32True = 0xFFFFFFFF;
static const uint32_t kMask32False = 0x00000000;

// These functions are implemented in assembly language.
extern "C" void UpdateCubicXsAndGetMask_Neon(const float& delta_x,
                                             const float* x_ends, int num_xs,
                                             float* xs, uint8_t* masks);

// y_range pointer is of type BulkSplineEvaluator::YRange (not used here because
// it's private, and extern "C" functions cannot be friends).
extern "C" void EvaluateCubics_Neon(const CubicCurve* curves, const float* xs,
                                    const void* y_ranges, int num_curves,
                                    float* ys);

void BulkSplineEvaluator::SetNumIndices(const Index num_indices) {
  sources_.resize(num_indices);
  y_ranges_.resize(num_indices);
  cubic_xs_.resize(num_indices, 0.0f);
  cubic_x_ends_.resize(num_indices, 0.0f);
  cubics_.resize(num_indices);
  ys_.resize(num_indices, 0.0f);
  scratch_.resize(num_indices, 0);
}

void BulkSplineEvaluator::MoveIndex(const Index old_index,
                                    const Index new_index) {
  sources_[new_index] = sources_[old_index];
  y_ranges_[new_index] = y_ranges_[old_index];
  cubic_xs_[new_index] = cubic_xs_[old_index];
  cubic_x_ends_[new_index] = cubic_x_ends_[old_index];
  cubics_[new_index] = cubics_[old_index];
  ys_[new_index] = ys_[old_index];
}

void BulkSplineEvaluator::SetYRange(const Index index, const Range& valid_y,
                                    const bool modular_arithmetic) {
  YRange& r = y_ranges_[index];
  r.valid_y = valid_y;
  r.modular_arithmetic = modular_arithmetic ? kMask32True : kMask32False;
}

void BulkSplineEvaluator::SetSpline(const Index index,
                                    const SplinePlayback& playback) {
  Source& s = sources_[index];
  s.spline = playback.spline;
  s.x_index = kInvalidSplineIndex;
  s.repeat = playback.repeat;
  InitCubic(index, playback.start_x);
  EvaluateIndex(index);
}

void BulkSplineEvaluator::UpdateCubicXsAndGetMask_C(const float delta_x,
                                                    uint8_t* masks) {
  const int num_xs = NumIndices();
  const float* x_ends = &cubic_x_ends_.front();
  float* xs = &cubic_xs_.front();

  for (int i = 0; i < num_xs; ++i) {
    xs[i] += delta_x;
    masks[i] = xs[i] > x_ends[i] ? 0xFF : 0x00;
  }
}

// For each non-zero mask[i], append 'i' to 'indices'.
// Returns: final length of indices.
// TODO OPT: Add assembly version if generated code is poor.
static size_t ConvertMaskToIndices(const uint8_t* mask, size_t length,
                                   BulkSplineEvaluator::Index* indices) {
  size_t num_indices = 0;
  for (size_t i = 0; i < length; ++i) {
    indices[num_indices] = static_cast<uint16_t>(i);
    if (mask[i] != 0) {
      num_indices++;
    }
  }
  return num_indices;
}

// Get a byte mask for the indices to init, and then convert that byte mask
// into a list of indices. This algorithm is best for many SIMD implementations,
// since they have trouble converting masks into indices.
size_t BulkSplineEvaluator::UpdateCubicXs_TwoSteps(const float delta_x,
                                                   Index* indices_to_init) {
  // Use last half of 'indices_to_init' as a scratch buffer for 'mask'.
  // Must be the last half since we read 'mask' to write 'indices_to_init'
  // in ConvertMaskToIndices().
  const Index num_indices = NumIndices();
  uint8_t* mask = reinterpret_cast<uint8_t*>(&indices_to_init[num_indices / 2]);

  // Add delta_x to each of the cubic_xs_.
  // Set mask[i] to 0xFF if the cubic has gone past the end of its array.
  UpdateCubicXsAndGetMask(delta_x, mask);

  // Get indices that are true 0xFF in the mask array.
  return ConvertMaskToIndices(mask, num_indices, indices_to_init);
}

// Record the indices, as we go along, for every index we need to re-init.
// This algorithm is fastest when we process indices serially.
size_t BulkSplineEvaluator::UpdateCubicXs_OneStep(const float delta_x,
                                                  Index* indices_to_init) {
  const Index num_indices = NumIndices();
  size_t num_to_init = 0;

  for (Index i = 0; i < num_indices; ++i) {
    // Increment each cubic x value by delta_x.
    cubic_xs_[i] += delta_x;

    // When x has gone past the end of the cubic, it should be reinitialized.
    if (cubic_xs_[i] > cubic_x_ends_[i]) {
      indices_to_init[num_to_init++] = i;
    }
  }
  return num_to_init;
}

void BulkSplineEvaluator::InitCubic(const Index index, const float start_x) {
  // Do nothing if the requested index has no spline.
  Source& s = sources_[index];
  if (s.spline == nullptr) return;

  // Do nothing if the current cubic matches the current spline segment.
  float new_start_x = start_x;
  CompactSplineIndex x_index = s.spline->IndexForX(new_start_x, s.x_index + 1);
  assert(s.x_index != x_index);
  if (s.repeat && x_index == kAfterSplineIndex) {
    new_start_x -= s.spline->LengthX();
    x_index = s.spline->IndexForX(new_start_x, 0);
  }
  if (s.x_index == x_index) return;
  s.x_index = x_index;

  // Update the values related to x.
  const Range x_range = s.spline->RangeX(x_index);
  cubic_xs_[index] = new_start_x - x_range.start();
  cubic_x_ends_[index] = x_range.Length();

  // Initialize the cubic to interpolate the new spline segment.
  CubicCurve& c = cubics_[index];
  const CubicInit init = s.spline->CreateCubicInit(x_index);
  c.Init(init);

  // The start y value of the cubic is d.cubic.Coeff(0) (the constant
  // coefficient) since cubic_x=0 at the start. So to ensure that the cubic
  // is normalized, it sufficents to ensure that d.cubic.Coeff(0) is normalized.
  const YRange& r = y_ranges_[index];
  if (r.modular_arithmetic) {
    c.SetCoeff(0, r.valid_y.NormalizeWildValue(c.Coeff(0)));
  }
}

void BulkSplineEvaluator::EvaluateIndex(const Index index) {
  // Evaluate the cubic spline.
  CubicCurve& c = cubics_[index];
  float y = c.Evaluate(cubic_xs_[index]);

  // Clamp or normalize the y value, to bring into the valid y range.
  // Also adjust the constant of the cubic so that next time we evaluate the
  // cubic it will be inside the normalized range.
  const YRange& r = y_ranges_[index];
  if (r.modular_arithmetic) {
    const float adjustment = r.valid_y.ModularAdjustment(y);
    y += adjustment;
    c.SetCoeff(0, c.Coeff(0) + adjustment);
    assert(r.valid_y.Contains(y));

  } else {
    y = r.valid_y.Clamp(y);
  }

  // Remember this value in our results array.
  ys_[index] = y;
}

void BulkSplineEvaluator::EvaluateCubics_C() {
  for (Index index = 0; index < NumIndices(); ++index) {
    EvaluateIndex(index);
  }
}

void BulkSplineEvaluator::AdvanceFrame(const float delta_x) {
  // Add 'delta_x' to 'cubic_xs'.
  // Gather a list of indices that are now beyond the end of the cubic.
  Index* indices_to_init = scratch_.size() == 0 ? nullptr : &scratch_.front();
  const size_t num_to_init = UpdateCubicXs(delta_x, indices_to_init);

  // Reinitialize indices that have traversed beyond the end of their cubic.
  for (size_t i = 0; i < num_to_init; ++i) {
    const Index index = indices_to_init[i];
    InitCubic(index, X(index));
  }

  // Update 'ys_' array. Also might affect the constant coefficients of
  // 'cubics_', if we're adjusting for modular arithmetic.
  EvaluateCubics();
}

bool BulkSplineEvaluator::Valid(const Index index) const {
  return 0 <= index && index < NumIndices() &&
         sources_[index].spline != nullptr;
}

// Form the assembly function name by appending "_Neon", "_SSD2", or whatever
// MOTIVE_ASSEMBLY_TEST is defined to be.
#define MOTIVE_TOKEN_PASTE_NESTED(a, b) a##b
#define MOTIVE_TOKEN_PASTE(a, b) MOTIVE_TOKEN_PASTE_NESTED(a, b)
#define MOTIVE_ASSEMBLY_FUNCTION_NAME(name) \
  MOTIVE_TOKEN_PASTE(name, MOTIVE_ASSEMBLY_TEST)

// These inline functions are used to redirect calls to the C or assembly
// versions, or to run both versions and compare the output.
inline void BulkSplineEvaluator::UpdateCubicXsAndGetMask(const float delta_x,
                                                         uint8_t* masks) {
#if defined(MOTIVE_ASSEMBLY_TEST)
  const int num_xs = NumIndices();
  std::vector<float> xs_assembly(cubic_xs_);
  std::vector<uint8_t> masks_assembly(num_xs);

  UpdateCubicXsAndGetMask_C(delta_x, masks);
  MOTIVE_ASSEMBLY_FUNCTION_NAME(UpdateCubicXsAndGetMask_)(
      delta_x, &cubic_x_ends_.front(), num_xs, &xs_assembly.front(),
      &masks_assembly.front());

  for (int i = 0; i < num_xs; ++i) {
    assert(cubic_xs_[i] == xs_assembly[i]);
    assert(masks[i] == masks_assembly[i]);
  }

#else  // not defined(MOTIVE_ASSEMBLY_TEST)

  switch (optimization_) {
#if defined(MOTIVE_NEON)
    case kNeonOptimizations:
      UpdateCubicXsAndGetMask_Neon(delta_x, &cubic_x_ends_.front(),
                                   NumIndices(), &cubic_xs_.front(), masks);
      break;
#endif
    default:
      UpdateCubicXsAndGetMask_C(delta_x, masks);
  }

#endif  // not defined(MOTIVE_ASSEMBLY_TEST)
}

inline size_t BulkSplineEvaluator::UpdateCubicXs(const float delta_x,
                                                 Index* indices_to_init) {
#if defined(MOTIVE_ASSEMBLY_TEST)
  std::vector<float> xs_original(cubic_xs_);
  std::vector<Index> indices_one(NumIndices());

  const size_t num_one = UpdateCubicXs_OneStep(delta_x, &indices_one.front());
  std::vector<float> xs_one(cubic_xs_);

  cubic_xs_ = xs_original;
  const size_t num_two = UpdateCubicXs_TwoSteps(delta_x, indices_to_init);

  assert(num_two == num_one);
  for (size_t i = 0; i < num_two; ++i) {
    assert(indices_to_init[i] == indices_one[i]);
  }
  for (int i = 0; i < NumIndices(); ++i) {
    assert(cubic_xs_[i] == xs_one[i]);
  }
  return num_two;

#else  // not defined(MOTIVE_ASSEMBLY_TEST)

  switch (optimization_) {
#if defined(MOTIVE_NEON)
    case kNeonOptimizations:
      return UpdateCubicXs_TwoSteps(delta_x, indices_to_init);
#endif
    default:
      return UpdateCubicXs_OneStep(delta_x, indices_to_init);
  }

#endif  // not defined(MOTIVE_ASSEMBLY_TEST)
}

inline void BulkSplineEvaluator::EvaluateCubics() {
#if defined(MOTIVE_ASSEMBLY_TEST)
  std::vector<float> ys_assembly(NumIndices());
  std::vector<CubicCurve> cubics_assembly(cubics_);

  MOTIVE_ASSEMBLY_FUNCTION_NAME(EvaluateCubics_)(
      &cubics_assembly.front(), &cubic_xs_.front(), &y_ranges_.front(),
      NumIndices(), &ys_assembly.front());
  EvaluateCubics_C();

  for (int i = 0; i < NumIndices(); ++i) {
    assert(ys_assembly[i] == ys_[i]);
  }
  for (int i = 0; i < NumIndices(); ++i) {
    assert(cubics_assembly[i] == cubics_[i]);
  }
#else  // not defined(MOTIVE_ASSEMBLY_TEST)

  switch (optimization_) {
#if defined(MOTIVE_NEON)
    case kNeonOptimizations:
      EvaluateCubics_Neon(&cubics_.front(), &cubic_xs_.front(),
                          &y_ranges_.front(), NumIndices(), &ys_.front());
      break;
#endif
    default:
      EvaluateCubics_C();
  }

#endif  // not defined(MOTIVE_ASSEMBLY_TEST)
}

}  // namespace fpl
