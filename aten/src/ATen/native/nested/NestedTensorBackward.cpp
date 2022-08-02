#include <ATen/native/nested/NestedTensorMath.h>

#include <ATen/ATen.h>
#include <ATen/AccumulateType.h>
#include <ATen/NamedTensorUtils.h>
#include <ATen/WrapDimUtils.h>
#include <ATen/core/op_registration/op_registration.h>
#include <ATen/native/layer_norm.h>
#include <ATen/NestedTensorImpl.h>
#include <c10/core/DispatchKey.h>
#include <ATen/native/nested/NestedTensorMath.h>

namespace at {
namespace native {

std::tuple<Tensor, Tensor, Tensor> nested_linear_backward(
    const Tensor& input,
    const Tensor& grad_output,
    const Tensor& weight,
    std::array<bool, 3> output_mask) {
  if (!grad_output.defined()) {
    return std::tuple<Tensor, Tensor, Tensor>{Tensor(), Tensor(), Tensor()};
  }
  Tensor grad_input, grad_weight, grad_bias;
  auto* nt_grad_output = get_nested_tensor_impl(grad_output);
  auto* nt_input = get_nested_tensor_impl(input);
  TORCH_INTERNAL_ASSERT(nt_grad_output != nullptr);
  TORCH_INTERNAL_ASSERT(nt_input != nullptr);
  TORCH_CHECK(nested_tensor_impl_is_contiguous(nt_grad_output));
  auto grad_ouput_buffer = nt_grad_output->get_buffer();
  auto input_buffer = nt_input->get_buffer();

  auto reshaped_grad = grad_ouput_buffer.reshape({-1, weight.size(0)});

  if (output_mask[0]) {
    auto grad_input_buffer = at::mm(reshaped_grad, weight).view({-1});
    auto grad_input_nt_size = nt_input->get_nested_size_tensor().clone();
    grad_input = wrap_buffer(grad_input_buffer, grad_input_nt_size);
  }
  if (output_mask[1]) {
    grad_weight =
        at::mm(reshaped_grad.t(), input_buffer.reshape({-1, weight.size(1)}));
  }
  if (output_mask[2]) {
    grad_bias = reshaped_grad.sum(0);
  }
  return std::tuple<Tensor, Tensor, Tensor>{grad_input, grad_weight, grad_bias};
}

Tensor _reshape_nested_backward(const Tensor& self, const Tensor& grad) {
  auto self_ptr = get_nested_tensor_impl(self);
  // TODO: this is to reproduce self_ptr->opt_sizes_
  //       if an accessor is provided in the future, can replace this
  std::vector<int64_t> sizes;
  for (int64_t i = 0; i < self_ptr->dim(); i++) {
    c10::optional<int64_t> opt_size = self_ptr->opt_size(i);
    if (opt_size.has_value()) {
      sizes.push_back(*opt_size);
    }
    else {
      sizes.push_back(-1);
    }
  }
  return grad.reshape(sizes);
}

// Rudimentary sum backward assuming the conditions in #82387
Tensor NestedTensor_sum_dim_backward_CPU(
  const Tensor& grad,
  // sizes is a dummy right now, it exists so we can dispatch sum_backward to
  // this function for NestedTensor, since we cannot change the signature of
  // sum_backward
  IntArrayRef sizes,
  OptionalIntArrayRef opt_dims,
  bool keepdim,
  const c10::optional<Tensor>& nested_self) {
  TORCH_CHECK(nested_self.has_value());
  auto nt_self = get_nested_tensor_impl(nested_self.value());
  auto nt_grad = get_nested_tensor_impl(grad);
  const Tensor& grad_buffer = nt_grad->get_buffer();
  const Tensor& self_buffer = nt_self->get_buffer();
  auto grad_sizes = nt_grad->get_nested_size_tensor();
  auto self_sizes = nt_self->get_nested_size_tensor();
  int64_t ntensors = nt_self->size(0);
  const Tensor& self_grad_buffer = self_buffer.new_empty(self_buffer.sizes());

  auto num_segments = at::prod(grad_sizes, -1);
  auto segment_lengths = self_sizes.select(1, -1);

  // This logic assumes for now that
  // (1) all the gradient nested tensors are contiguous
  // (2) the gradient nested tensors are stored contiguously in the buffer
  AT_DISPATCH_ALL_TYPES_AND2(
    ScalarType::Half, ScalarType::BFloat16, self_grad_buffer.scalar_type(), "nested_sum_dim_cpu", [&]() {
    auto* self_grad_data = self_grad_buffer.data_ptr<scalar_t>();
    const auto* output_grad_data = grad_buffer.data_ptr<scalar_t>();
    int64_t out_idx = 0, in_idx = 0;
    for (const auto i : c10::irange(ntensors)) {
      int64_t segments = num_segments[i].item<int64_t>();
      int64_t segment_length = segment_lengths[i].item<int64_t>();
      for (auto j = 0; j < segments; j++) {
        scalar_t output_grad = output_grad_data[out_idx];
        for (auto k = 0; k < segment_length; k++) {
          self_grad_data[in_idx] = output_grad;
          in_idx += 1;
        }
        out_idx += 1;
      }
    }
  });

  return wrap_buffer(self_grad_buffer, self_sizes);

}

} // namespace native
} // namespace at
