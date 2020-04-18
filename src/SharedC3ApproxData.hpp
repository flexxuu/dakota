/*  _______________________________________________________________________

    DAKOTA: Design Analysis Kit for Optimization and Terascale Applications
    Copyright 2014 Sandia Corporation.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Dakota directory.
    _______________________________________________________________________ */

//- Class:        SharedC3ApproxData
//- Description:  Base Class for C3 aprpoximation interface
//-               
//- Owner:        Alex Gorodetsky

#ifndef SHARED_C3_APPROX_DATA_H
#define SHARED_C3_APPROX_DATA_H

#include "SharedApproxData.hpp"
#include "SharedRegressOrthogPolyApproxData.hpp"
#include "dakota_c3_include.hpp"

namespace Dakota {


/// Derived approximation class for global basis polynomials.

/** The SharedC3ApproxData class provides a global approximation
    based on basis polynomials.  This includes orthogonal polynomials
    used for polynomial chaos expansions and interpolation polynomials
    used for stochastic collocation. */

class SharedC3ApproxData: public SharedApproxData
{
  //
  //- Heading: Friends
  //

  friend class C3Approximation;

public:

  //
  //- Heading: Constructor and destructor
  //

  /// default constructor
  SharedC3ApproxData();
  /// standard ProblemDescDB-driven constructor
  SharedC3ApproxData(ProblemDescDB& problem_db, size_t num_vars);
  /// on-the-fly constructor (no problem DB)
  SharedC3ApproxData(const String& approx_type,
                     const UShortArray& approx_order, size_t num_vars,
                     short data_order, short output_level);
  /// destructor
  ~SharedC3ApproxData();

  //
  //- Heading: Member functions
  //

  /// return number of FT unknowns given scalars: num vars, rank, order
  static size_t regression_size(size_t num_v, size_t rank,
				const UShortArray& orders);
  /// return number of FT unknowns using numVars, start_rank(), start_orders()
  size_t regression_size();

  void set_parameter(String var, const UShortArray& val);
  void set_parameter(String var, unsigned short     val);
  void set_parameter(String var, size_t val);
  void set_parameter(String var, bool   val);
  void set_parameter(String var, short  val);
  void set_parameter(String var, double val);
  void set_parameter(String var, int    val);

  // set SharedOrthogPolyApproxData::basisTypes
  //void basis_types(const ShortArray& basis_types);
  // get SharedOrthogPolyApproxData::basisTypes
  //const ShortArray& basis_types() const;

  /// return current basis polynomial order (active key in startOrders)
  const UShortArray& start_orders() const;
  /// return current expansion rank (active key in startRank)
  size_t start_rank() const;

  /// update oneApproxOpts with active basis orders after an order change
  void update_basis();
  /// update oneApproxOpts with passed basis orders after an order change
  void update_basis(const UShortArray& start_orders, unsigned short max_order);

protected:

  //
  //- Heading: Virtual function redefinitions
  //

  void active_model_key(const UShortArray& key);
  //void clear_model_keys();

  void construct_basis(const Pecos::MultivariateDistribution& mv_dist);

  short discrepancy_type() const;

  // Activate as additional features (and basis types) come online
  
  //void update_basis_distribution_parameters(        // beyond STD_{NORMAL,
  //  const Pecos::MultivariateDistribution& u_dist); //             UNIFORM}

  //void configuration_options(const Pecos::ExpansionConfigOptions& ec_options);
  //void configuration_options(const Pecos::BasisConfigOptions&     bc_options);

  void random_variables_key(const BitArray& random_vars_key);

  //void refinement_statistics_type(short stats_type);

  //const Pecos::BitArrayULongMap& SharedApproxData::sobol_index_map();

  void build();
  //void rebuild(); // defaults to build()

  void increment_order();
  void decrement_order();

  void   pop(bool save_surr_data);
  bool   push_available();
  size_t push_index(const UShortArray& key);
  void   post_push();

  void pre_combine();
  //void post_combine();

  //
  //- Heading: Data
  //

  // This data is shared per QoI _and_ per level
  //
  /// 1D approximation options (basis type, poly order, etc.)
  std::vector<OneApproxOpts*> oneApproxOpts;//OneApproxOpts ** oneApproxOpts;
  /// n-D approximation options, augmenting 1D options
  MultiApproxOpts* multiApproxOpts;

  // these are stored in oneApproxOpts, but currently need to be cached
  // to persist between set_parameter() and construct_basis()

  /// starting user specification for polynomial orders (from start_order
  /// scalar plus anisotropic dimension preference)
  UShortArray startOrderSpec;
  /// starting values for polynomial order (prior to adaptive refinement);
  /// for each model key, there is an array of polynomial orders per variable
  std::map<UShortArray, UShortArray> startOrders;
  /// polynomial basis order for combined expansion for each variable core
  UShortArray combinedOrders;
  /// maximum value for polynomial order (if adapted)
  unsigned short maxOrder;

  // the remaining data are separate as can be seen in C3Approximation::build()

  /// starting user specification for rank (not augmented by dimension
  /// preference); Note: rank sequence spec is managed externally and becomes
  /// reflected in startRank model index mapping
  size_t startRankSpec; // or SizetArray for start_rank_sequence spec
  /// starting values for rank (note: adapt_rank currently covers refinement);
  /// for each model index key, there is a scalar starting rank (recovered
  /// rank in C3FnrainPtrs can vary per core/variable and per QoI)
  std::map<UShortArray, size_t> startRank;
  /// user specification for increment in rank used within adapt_rank
  size_t kickRank;
  /// user specification for maximum rank used within adapt_rank 
  size_t maxRank;
  /// internal C3 adaptation that identifies the best rank representation
  /// for a set of sample data based on cross validation
  bool adaptRank;

  short  regressType;
  double regressRegParam; // penalty parameter if regularized regression
  double solverTol;
  double roundingTol;
  double arithmeticTol;
  int maxSolverIterations;
  int crossMaxIter;
  int c3Verbosity;

  /// a more general adaptive construction option, distinct from adapt_rank
  bool adaptConstruct; // inactive placeholder for now
  /// C3 FT can support CV over polynomial order in addition to adapt_rank
  bool crossVal;       // inactive placeholder for now

  /// type of discrepancy calculation: additive, multiplicative, or both
  short combineType;
  /// type of multilevel discrepancy emulation: distinct or recursive
  short discrepancyType;
  /// type of multilevel strategy for sample allocation: ESTIMATOR_VARIANCE,
  /// RANK_SAMPLING, GREEDY
  short allocControl;
  // indicates refinement based on active or combined statistics
  //short refineStatsType;

  // key identifying the subset of build variables that can be treated
  // as random, for purposes of computing statistics
  //BitArray ranVarsKey; // stored locally rather than passed to library (Pecos)
  /// indices for random subset when approximating in all-variables mode
  SizetArray randomIndices;

  /// number of instances within the popped arrays (mostly a placeholder for
  /// supporting push_available())
  std::map<UShortArray, size_t> poppedCounts;

private:

  //
  //- Heading: Convenience functions
  //


  //
  //- Heading: Data
  //

};


inline SharedC3ApproxData::SharedC3ApproxData()
{ }


inline void SharedC3ApproxData::active_model_key(const UShortArray& key)
{
  // set activeKey and approxDataKeys
  SharedApproxData::active_model_key(key);

  // these aren't used enough to warrant active iterators
  bool form = false;
  if (startOrders.find(key) == startOrders.end())
    { startOrders[key] = startOrderSpec;  form = true; }
  if (startRank.find(key) == startRank.end()) 
    { startRank[key]   = startRankSpec;   form = true; }
  if (form) formUpdated[key] = true;
}


inline void SharedC3ApproxData::update_basis()
{ update_basis(startOrders[activeKey], maxOrder); }


inline short SharedC3ApproxData::discrepancy_type() const
{ return discrepancyType; }


inline const UShortArray& SharedC3ApproxData::start_orders() const
{
  std::map<UShortArray, UShortArray>::const_iterator cit
    = startOrders.find(activeKey);
  return (cit == startOrders.end()) ? startOrderSpec : cit->second;
}


inline size_t SharedC3ApproxData::start_rank() const
{
  std::map<UShortArray, size_t>::const_iterator cit = startRank.find(activeKey);
  return (cit == startRank.end()) ? startRankSpec : cit->second;
}


/** simplified estimation for scalar-valued rank and order (e.g., from 
    start rank/order user specification)
inline size_t SharedC3ApproxData::
regression_size(size_t num_v, size_t rank, size_t order)
{
  // Each dimension has its own rank within the product of function cores.
  // This fn estimates for the case where rank and order are either constant
  // across dimensions or averaged into a scalar.
  // > the first and last core contribute p*r terms
  // > the middle cores contribute r*r*p terms
  size_t p = order+1.;
  switch (num_v) {
  case 1:  return p;         break; // collapses to a 1D PCE
  case 2:  return 2.*p*rank; break; // first and last core, no middle
  default: return p*rank*(2. + (num_v-2)*rank); break; // first,last,middle
  }
}
*/


/** simplified estimation for scalar-valued rank and vector-valued order
    (e.g., from start rank/start order/dimension pref user specification) */
inline size_t SharedC3ApproxData::
regression_size(size_t num_v, size_t rank, const UShortArray& orders)
{
  // Each dimension has its own rank within the product of function cores.
  // This fn estimates for the case where rank and order are either constant
  // across dimensions or averaged into a scalar.
  // > the first and last core contribute p*r terms
  // > the middle cores contribute r*r*p terms
  switch (num_v) {
  case 1:  return  orders[0]+1;                 break; // collapses to 1D PCE
  case 2:  return (orders[0]+orders[1]+2)*rank; break; // first,last core
  default: { // first, last, and num_v-2 middle cores
    size_t core, num_vm1 = num_v - 1,
      sum = orders[0] + orders[num_vm1] + 2; // first, last
    for (core=1; core<num_vm1; ++core)
      sum += rank * (orders[core] + 1);  // num_v-2 middle cores
    return sum * rank;  break;
  }
  }
}


inline size_t SharedC3ApproxData::regression_size()
{ return regression_size(numVars, start_rank(), start_orders()); }
// TO DO: incorporate dimension preference -> ranks array


inline void SharedC3ApproxData::
set_parameter(String var, const UShortArray& val)
{
  if (var.compare("start_poly_order") == 0)
    { startOrders[activeKey] = /*startOrderSpec =*/ val; /*update_basis();*/ }
  else std::cerr << "Unrecognized C3 parameter: " << var << std::endl;
}


inline void SharedC3ApproxData::set_parameter(String var, unsigned short val)
{
  if (var.compare("max_poly_order")  == 0)
    { maxOrder = val; /*update_basis();*/ }
  else std::cerr << "Unrecognized C3 parameter: " << var << std::endl;
}


inline void SharedC3ApproxData::set_parameter(String var, size_t val)
{
  if (var.compare("start_rank") == 0)
    startRank[activeKey] = /*startRankSpec =*/ val;
  else if (var.compare("kick_rank")  == 0)  kickRank = val;
  else if (var.compare("max_rank")   == 0)   maxRank = val;
  else std::cerr << "Unrecognized C3 parameter: " << var << std::endl;
}


inline void SharedC3ApproxData::set_parameter(String var, bool val)
{
  if (var.compare("adapt_rank") == 0)  adaptRank = val;
  else std::cerr << "Unrecognized C3 parameter: " << var << std::endl;
}


inline void SharedC3ApproxData::set_parameter(String var, short val)
{
  if      (var.compare("regress_type")     == 0)     regressType = val;
  else if (var.compare("discrepancy_type") == 0) discrepancyType = val;
  else if (var.compare("alloc_control")    == 0)    allocControl = val;
  else if (var.compare("combine_type")     == 0)     combineType = val;
  else std::cerr << "Unrecognized C3 parameter: " << var << std::endl;
}


inline void SharedC3ApproxData::set_parameter(String var, double val)
{
  if      (var.compare("solver_tol")               == 0)       solverTol = val;
  else if (var.compare("rounding_tol")             == 0)     roundingTol = val;
  else if (var.compare("arithmetic_tol")           == 0)   arithmeticTol = val;
  else if (var.compare("regularization_parameter") == 0) regressRegParam = val;
  else std::cerr << "Unrecognized C3 parameter: " << var << std::endl;
}


inline void SharedC3ApproxData::set_parameter(String var, int val)
{
  if      (var.compare("max_cross_iterations")  == 0)        crossMaxIter = val;
  else if (var.compare("max_solver_iterations") == 0) maxSolverIterations = val;
  else if (var.compare("verbosity")             == 0)         c3Verbosity = val;
  else std::cerr << "Unrecognized C3 parameter: " << var << std::endl;
}


inline void SharedC3ApproxData::
random_variables_key(const BitArray& random_vars_key)
{
  // store incoming mask:
  //ranVarsKey = random_vars_key;

  // convert incoming mask to a set of random indices:
  if (random_vars_key.empty()) {
    randomIndices.resize(numVars);
    for (size_t i=0; i<numVars; ++i)
      randomIndices[i] = i;
  }
  else {
    // NonDExpansion::initialize_u_space_model() assigns startCAUV,numCAUV
    // subset within numContinuousVars
    size_t i, cntr, num_rand = random_vars_key.count();
    randomIndices.resize(num_rand);
    for (i=0, cntr=0; i<numVars; ++i)
      if (random_vars_key[i])
	randomIndices[cntr++] = i;
  }
}


//inline void SharedC3ApproxData::refinement_statistics_type(short stats_type)
//{ refineStatsType = stats_type; }


inline void SharedC3ApproxData::build()
{
  // Ideally this would occur in post_build(), but for now there is only a
  // SharedApproxData::build() that precedes Approximation::build()

  // formulation updates (rank,order increments/decrements) have been
  // synchronized once build is complete (no longer need to force a rebuild):
  formUpdated[activeKey] = false;
}


inline void SharedC3ApproxData::increment_order()
{
  std::map<UShortArray, UShortArray>::iterator it = startOrders.find(activeKey);
  UShortArray& active_so = it->second;  bool incremented = false;
  for (size_t i=0; i<numVars; ++i) {
    unsigned short& oi = active_so[i];
    // default maxOrder is 10 (tensor train can be more conservative than PCE).
    // could consider a kickOrder (parallel to kickRank), but other
    // advancements (regression PCE) use exp order increment of 1
    if (oi < maxOrder)
      { ++oi; incremented = true; }
  }
  if (incremented)
    update_basis(active_so, maxOrder);
  else {
    Cerr << "Error: SharedC3ApproxData::increment_order() cannot advance "
	 << "beyond maxOrder." << std::endl;
    abort_handler(APPROX_ERROR);
  }
}


inline void SharedC3ApproxData::decrement_order()
{
  std::map<UShortArray, UShortArray>::iterator it = startOrders.find(activeKey);
  UShortArray& active_so = it->second;  bool decremented = false;
  for (size_t i=0; i<numVars; ++i) {
    unsigned short& oi = active_so[i];
    // could consider a kickOrder (parallel to kickRank), but other
    // advancements (regression PCE) use exp order decrement of 1
    if (oi)
      { --oi; decremented = true; }
  }
  if (decremented)
    update_basis(active_so, maxOrder);
  else {
    Cerr << "Error: SharedC3ApproxData::decrement_order() has reached 0."
	 << std::endl;
    abort_handler(APPROX_ERROR);
  }
}


inline void SharedC3ApproxData::pop(bool save_surr_data)
{
  std::map<UShortArray, size_t>::iterator it = poppedCounts.find(activeKey);
  if (it == poppedCounts.end())
    poppedCounts[activeKey] = 1;
  else
    ++it->second; // increment count
}


inline bool SharedC3ApproxData::push_available()
{
  std::map<UShortArray, size_t>::iterator it = poppedCounts.find(activeKey);
  return (it != poppedCounts.end() && it->second > 0);
}


inline size_t SharedC3ApproxData::push_index(const UShortArray& key)
{
  std::map<UShortArray, size_t>::iterator it = poppedCounts.find(key);
  if (it == poppedCounts.end() || it->second == 0) {
    Cerr << "Error: bad lookup in SharedC3ApproxData::push_index(key)"
	 << std::endl;
    abort_handler(APPROX_ERROR);
  }
  // there should only be a single refinement candidate per level (as for
  // uniform refinement cases in Pecos), but this is more general:
  else
    return it->second - 1; // last in, first out
}


inline void SharedC3ApproxData::post_push()
{
  std::map<UShortArray, size_t>::iterator it = poppedCounts.find(activeKey);
  if (it != poppedCounts.end() && it->second > 0)
    --it->second; // decrement count
}

} // namespace Dakota

#endif