/*  _______________________________________________________________________

    DAKOTA: Design Analysis Kit for Optimization and Terascale Applications
    Copyright 2014-2020 National Technology & Engineering Solutions of Sandia, LLC (NTESS).
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Dakota directory.
    _______________________________________________________________________ */

#include "util_common.hpp"
#include "util_math_tools.hpp"

#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int.hpp>

namespace dakota {
namespace util {

// ------------------------------------------------------------

int n_choose_k(int n, int k)
{
  double value = 1.0;
  for (int i = 0; i < n-k; i++)
    value *= (double)(n - i) / (double)(n - k - i);

  return (int) round(value);
}

// ------------------------------------------------------------

void random_permutation(const int num_pts,
  const unsigned int seed, VectorXi &permutations){

  /*
  using boost::uniform_int<> = NumberDistribution;
  using boost::mt19937 = RNG;
  using boost::variate_generator<RNG&, NumberDistribution> Generator;

  NumberDistribution distribution(0, num_pts);
  RNG mtwister;
  mtwister.seed(seed);
  Generator numberGenerator(mtwister, distribution);
  */

  boost::random::mt19937 generator(seed);
  boost::random::uniform_int_distribution<> dist(0, num_pts - 1);
  int index, tmp;
  for (int i = 0; i < num_pts; i++) {
    index = dist(generator);
    tmp = permutations(i);
    permutations(i) = permutations(index);
    permutations(index) = tmp;
  }
}

// ------------------------------------------------------------

void create_cv_folds(const int num_folds, const int num_pts,
  std::vector<VectorXi> &fold_indices, const int seed) {

  if (num_pts < num_folds) {
    error("Number of cv folds > number of points");
  }

  fold_indices.resize(num_folds);
  VectorXi permutation_indices =
    VectorXi::LinSpaced(num_pts, 0, num_pts-1);

  if (seed == 0) {
    random_permutation(num_pts, (unsigned int)std::time(0),
		  permutation_indices);
  }
  else if (seed > 0) {
    random_permutation(num_pts, (unsigned int)seed,
		  permutation_indices);
  }

  int start_ind, end_ind, max_fold_size, fold_size;
  max_fold_size = std::round(double(num_pts) / double(num_folds));
  start_ind = 0;
  
  for (int k = 1; k <= num_folds; k++) {
    if ( k != num_folds)
      end_ind = start_ind + max_fold_size;
    else
      end_ind = num_pts;
    fold_size = end_ind - start_ind;
    fold_indices[k-1] = permutation_indices.segment(start_ind, fold_size);
    start_ind = end_ind;
  }
}

} // namespace util
} // namespace dakota