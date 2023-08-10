////////////////////////////////////////////////////////////////////////////////
// Copyright 2019-2022 Lawrence Livermore National Security, LLC and other
// DiHydrogen Project Developers. See the top-level LICENSE file for details.
//
// SPDX-License-Identifier: Apache-2.0
////////////////////////////////////////////////////////////////////////////////

#pragma once

/** @file
 *
 * Local tensors that live on CPUs.
 */

#include "h2/tensor/tensor.hpp"
#include "h2/tensor/strided_memory.hpp"
#include "h2/tensor/tensor_types.hpp"
#include "strided_memory.hpp"
#include "tensor_types.hpp"

namespace h2 {

template <typename T>
class Tensor<T, Device::CPU> : public BaseTensor<T> {
public:
  using value_type = T;
  static constexpr Device device = Device::CPU;

  Tensor(ShapeTuple shape_, DimensionTypeTuple dim_types_) :
    BaseTensor<T>(shape_, dim_types_),
    tensor_memory(shape_)
  {}

  Tensor() : Tensor(ShapeTuple(), DimensionTypeTuple()) {}

  Tensor(T* buffer, ShapeTuple shape_, DimensionTypeTuple dim_types_,
         StrideTuple strides_) :
    BaseTensor<T>(ViewType::Mutable, shape_, dim_types_),
    tensor_memory(buffer, shape_, strides_)
  {}

  Tensor(const T* buffer, ShapeTuple shape_, DimensionTypeTuple dim_types_,
         StrideTuple strides_) :
    BaseTensor<T>(ViewType::Const, shape_, dim_types_),
    tensor_memory(const_cast<T*>(buffer), shape_, strides_)
  {}

  StrideTuple strides() const H2_NOEXCEPT override {
    return tensor_memory.strides();
  }

  typename StrideTuple::type strides(typename StrideTuple::size_type i) const H2_NOEXCEPT override {
    return tensor_memory.strides()[i];
  }

  bool is_contiguous() const H2_NOEXCEPT override {
    return are_strides_contiguous(this->tensor_shape, tensor_memory.strides());
  }

  Device get_device() const H2_NOEXCEPT override { return device; }

  void empty() override {
    tensor_memory = StridedMemory<T, Device::CPU>();
    this->tensor_shape = ShapeTuple();
    this->tensor_dim_types = DimensionTypeTuple();
    if (this->is_view()) {
      this->tensor_view_type = ViewType::None;
    }
  }

  void resize(ShapeTuple new_shape) override {
    if (this->is_view()) {
      throw H2Exception("Cannot resize a view");
    }
    if (new_shape.size() > this->tensor_shape.size()) {
      throw H2Exception("Must provide dimension types to resize larger");
    }
    tensor_memory = StridedMemory<T, Device::CPU>(new_shape);
    this->tensor_shape = new_shape;
    this->tensor_dim_types.set_size(new_shape.size());
  }

  void resize(ShapeTuple new_shape, DimensionTypeTuple new_dim_types) override {
    if (this->is_view()) {
      throw H2Exception("Cannot resize a view");
    }
    tensor_memory = StridedMemory<T, Device::CPU>(new_shape);
    this->tensor_shape = new_shape;
    this->tensor_dim_types = new_dim_types;
  }

  T* data() override {
    if (this->tensor_view_type == ViewType::Const) {
      throw H2Exception("Cannot access non-const buffer of const view");
    }
    return tensor_memory.data();
  }

  const T* data() const override
  {
    return tensor_memory.const_data();
  }

  const T* const_data() const override {
    return tensor_memory.const_data();
  }

  void ensure() override {
    // TODO
  }

  void release() override {
    // TODO
  }

  Tensor<T, Device::CPU>* contiguous() override {
    if (is_contiguous()) {
      return view();
    }
    throw H2Exception("contiguous() not implemented");
  }

  Tensor<T, Device::CPU>* view() override {
    return view(CoordTuple(TuplePad<CoordTuple>(this->tensor_shape.size(), ALL)));
  }

  Tensor<T, Device::CPU>* view(CoordTuple coords) override {
    return new Tensor<T, Device::CPU>(
      ViewType::Mutable,
      tensor_memory,
      get_range_shape(coords, this->tensor_shape),
      filter_by_trivial(coords, this->tensor_dim_types),
      coords);
  }

  Tensor<T, Device::CPU>* operator()(CoordTuple coords) override
  {
    return view(coords);
  }

  void unview() override {
    H2_ASSERT_DEBUG(this->is_view(), "Must be a view to unview");
    empty();  // Emptying a view is equivalent to unviewing.
  }

  const Tensor<T, Device::CPU>* const_view() const override {
    return const_view(CoordTuple(TuplePad<CoordTuple>(this->tensor_shape.size(), ALL)));
  }

  const Tensor<T, Device::CPU>* const_view(CoordTuple coords) const override {
    return new Tensor<T, Device::CPU>(
      ViewType::Const,
      tensor_memory,
      get_range_shape(coords, this->tensor_shape),
      filter_by_trivial(coords, this->tensor_dim_types),
      coords);
  }

  const Tensor<T, Device::CPU>* operator()(CoordTuple coords) const override {
    return const_view(coords);
  }

  T get(SingleCoordTuple coords) const override {
    return *(tensor_memory.get(coords));
  }

private:
  /** Underlying memory buffer for the tensor. */
  StridedMemory<T, Device::CPU> tensor_memory;

  /** Private constructor for views. */
  Tensor(ViewType view_type_, const StridedMemory<T, Device::CPU>& mem_,
         ShapeTuple shape_, DimensionTypeTuple dim_types_, CoordTuple coords) :
    BaseTensor<T>(view_type_, shape_, dim_types_),
    tensor_memory(mem_, coords)
  {}
};

}  // namespace h2
