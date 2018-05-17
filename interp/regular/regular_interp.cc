#include <Eigen/Core>

#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/shape_inference.h"

using namespace tensorflow;

REGISTER_OP("InterpRegular")
  .Attr("ndim: int")
  .Attr("T: {float, double}")
  .Attr("check_sorted: bool = true")
  .Input("points: ndim * T")
  .Input("values: T")
  .Input("xi: T")
  .Output("zi: T")
  .SetShapeFn([](shape_inference::InferenceContext* c) {
    int ndim;
    TF_RETURN_IF_ERROR(c->GetAttr("ndim", &ndim));

    shape_inference::ShapeHandle shape, values_shape, xi_shape, zi_shape, tmp;

    // Get the dimensions of each axis
    TF_RETURN_IF_ERROR(c->WithRank(c->input(0), 1, &shape));
    for (int i = 1; i < ndim; ++i) {
      TF_RETURN_IF_ERROR(c->WithRank(c->input(i), 1, &tmp));
      TF_RETURN_IF_ERROR(c->Concatenate(shape, tmp, &shape));
    }

    // Make sure that the first ndim axes of values have the right shape
    TF_RETURN_IF_ERROR(c->WithRankAtLeast(c->input(ndim), ndim, &values_shape));
    TF_RETURN_IF_ERROR(c->Subshape(values_shape, ndim, &zi_shape));
    TF_RETURN_IF_ERROR(c->Subshape(values_shape, 0, ndim, &values_shape));
    TF_RETURN_IF_ERROR(c->Merge(shape, values_shape, &values_shape));

    // Make sure that the last dimension of xi is ndim
    TF_RETURN_IF_ERROR(c->WithRankAtLeast(c->input(ndim+1), 1, &xi_shape));
    shape_inference::DimensionHandle dim = c->Dim(xi_shape, -1);
    TF_RETURN_IF_ERROR(c->WithValue(dim, ndim, &dim));

    // Compute the output shape
    TF_RETURN_IF_ERROR(c->Subshape(xi_shape, 0, -1, &xi_shape));
    TF_RETURN_IF_ERROR(c->Concatenate(xi_shape, zi_shape, &zi_shape));
    c->set_output(0, zi_shape);

    return Status::OK();
  });

// TODO: use http://en.cppreference.com/w/cpp/algorithm/lower_bound instead
template <typename T>
inline int64 search_sorted (int64 N, const typename TTypes<T>::ConstFlat& x, const T& value) {
  if (value <= x(0)) {
    return 0;
  }
  if (value >= x(N-1)) {
    return N+1;
  }
  int64 left = 0, right = N-1;
  while (left < right) {
    int64 middle = left + ((right - left) >> 1);
    if (x(middle) < value) {
      left = middle + 1;
    } else {
      right = middle;
    }
  }
  return right;
}

template <typename T>
class InterpRegularOp : public OpKernel {
 public:
  explicit InterpRegularOp(OpKernelConstruction* context) : OpKernel(context) {
    OP_REQUIRES_OK(context, context->GetAttr("ndim", &ndim_));
    OP_REQUIRES(context, (ndim_ >= 1), errors::InvalidArgument("'ndim' must be >= 1"));
    OP_REQUIRES_OK(context, context->GetAttr("check_sorted", &check_sorted_));
  }

  void Compute(OpKernelContext* context) override {
    typedef Eigen::Map<const Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> ConstMatrix;
    typedef Eigen::Map<Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> Matrix;

    // Check the dimensions of the input values
    const Tensor& values_tensor = context->input(ndim_);
    int64 values_dims = values_tensor.dims();
    OP_REQUIRES(context, (values_dims >= ndim_),
                errors::InvalidArgument("'values' must be at least 'ndim'-dimensional"));

    // Compute the total number of grid points
    Eigen::Array<int64, Eigen::Dynamic, 1> grid_dims(ndim_);
    for (int64 i = 0; i < ndim_; ++i) grid_dims(i) = values_tensor.dim_size(i);
    int64 ngrid = grid_dims.prod();

    // Compute the total number of outputs
    int64 nout = 1;
    for (int64 i = ndim_; i < values_dims; ++i) nout *= values_tensor.dim_size(i);

    // Check the dimensions of the test points
    const Tensor& xi_tensor = context->input(ndim_+1);
    int64 xi_dims = xi_tensor.dims();
    OP_REQUIRES(context, (xi_dims >= 1),
                errors::InvalidArgument("'xi' must be at least 1-dimensional"));
    OP_REQUIRES(context, (xi_tensor.dim_size(xi_dims - 1) == ndim_),
                errors::InvalidArgument("The last dimension of 'xi' must be 'ndim'"));

    // Compute the total number of test points
    int64 ntest = 1;
    for (int64 i = 0; i < xi_dims-1; ++i) ntest *= xi_tensor.dim_size(i);

    // Cast the values and test points as a matrix with the right shape
    const auto values = ConstMatrix(values_tensor.template flat<T>().data(), ngrid, nout);
    const auto xi = ConstMatrix(xi_tensor.template flat<T>().data(), ntest, ndim_);

    // Allocate temporary arrays to store indices and weights
    Eigen::Matrix<int64, Eigen::Dynamic, Eigen::Dynamic> inds(ntest, ndim_);
    Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic> norm_dist(ntest, ndim_);

    // Compute the output shapes
    auto values_shape = values_tensor.shape(),
         zi_shape = xi_tensor.shape();
    values_shape.RemoveDimRange(0, ndim_);
    zi_shape.RemoveLastDims(1);
    zi_shape.AppendShape(values_shape);
    Tensor* zi_tensor = NULL;
    OP_REQUIRES_OK(context, context->allocate_output(0, zi_shape, &zi_tensor));
    auto zi = Matrix(zi_tensor->template flat<T>().data(), ntest, nout);
    zi.setZero();

    // Loop over dimensions and compute the indices of each test point in each grid
    for (int n = 0; n < ndim_; ++n) {
      // Check the grid definition
      const Tensor& points_tensor = context->input(n);
      int64 N = points_tensor.NumElements();
      OP_REQUIRES(context, (points_tensor.dims() == 1),
                  errors::InvalidArgument("each tensor in 'points' must be exactly 1-dimensional"));
      OP_REQUIRES(context, (grid_dims(n) == N),
                  errors::InvalidArgument("the first 'ndim' dimensions of 'values' must have the same shape as points"));
      const auto points = points_tensor.template flat<T>();
      if (check_sorted_) {
        for (int64 k = 0; k < N-1; ++k)
          OP_REQUIRES(context, (points(k+1) > points(k)), errors::InvalidArgument("each tensor in 'points' must be sorted"));
      }

      // Find where the point should be inserted into the grid
      for (int64 m = 0; m < ntest; ++m) {
        bool out_of_bounds = false;
        int64 ind = search_sorted(N, points, xi(m, n)) - 1;
        if (ind < 0) {
          out_of_bounds = true;
          ind = 0;
        }
        if (ind > N-2) {
          out_of_bounds = true;
          ind = N-2;
        }
        inds(m, n) = ind;
        norm_dist(m, n) = (xi(m, n) - points(ind)) / (points(ind+1) - points(ind));
      }
    }

    // Loop over test points and compute the interpolation for that point
    unsigned ncorner = pow(2, ndim_);
    for (int n = 0; n < ntest; ++n) {

      // Madness to find the coordinates of every corner
      for (unsigned corner = 0; corner < ncorner; ++corner) {
        int64 factor = 1;
        int64 ind = 0;
        T weight = 1.0;
        for (int dim = ndim_-1; dim >= 0; --dim) {
          unsigned offset = (corner >> unsigned(dim)) & 1;
          ind += factor * (inds(n, dim) + offset);
          factor *= grid_dims(dim);
          if (offset == 1) {
            weight *= norm_dist(n, dim);
          } else {
            weight *= 1.0 - norm_dist(n, dim);
          }
        }

        zi.row(n) += weight * values.row(ind);
      }
    }
  }
 private:
  int ndim_;
  bool check_sorted_;
};


#define REGISTER_KERNEL(type)                                               \
  REGISTER_KERNEL_BUILDER(                                                  \
      Name("InterpRegular").Device(DEVICE_CPU).TypeConstraint<type>("T"),   \
      InterpRegularOp<type>)

REGISTER_KERNEL(float);
REGISTER_KERNEL(double);

#undef REGISTER_KERNEL