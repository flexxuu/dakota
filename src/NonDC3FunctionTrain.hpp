/*  _______________________________________________________________________

    DAKOTA: Design Analysis Kit for Optimization and Terascale Applications
    Copyright 2014 Sandia Corporation.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Dakota directory.
    _______________________________________________________________________ */

//- Class:       NonDC3FunctionTrain
//- Description: Iterator for the 
//- Owner:       Alex Gorodetsky, Sandia National Laboratories

#ifndef NOND_C3_FUNCTION_TRAIN_H
#define NOND_C3_FUNCTION_TRAIN_H

// #include "DakotaNonD.hpp

#include "NonDExpansion.hpp"

namespace Dakota {

/// Nonintrusive uncertainty quantification with the C3 library ...

/** The NonDC3FunctionTrain class uses ... */

class NonDC3FunctionTrain: public NonDExpansion
{
public:

  //
  //- Heading: Constructors and destructor
  //

  /// standard constructor
  NonDC3FunctionTrain(ProblemDescDB& problem_db, Model& model);
  /// destructor
  ~NonDC3FunctionTrain();

protected:

  //
  //- Heading: Constructors
  //

  /// base constructor for DB construction of multilevel/multifidelity PCE
  /// (method_name is not necessary, rather it is just a convenient overload
  /// allowing the derived ML FT class to bypass the standard FT ctor)
  NonDC3FunctionTrain(unsigned short method_name, ProblemDescDB& problem_db,
		      Model& model);

  //
  //- Heading: Virtual function redefinitions
  //

  void resolve_inputs(short& u_space_type, short& data_order);
  void initialize_u_space_model();

  // TODO
  //void compute_expansion();
  // perform a forward uncertainty propagation using PCE/SC methods
  //void core_run();

  void push_increment();
  void update_samples_from_order_increment();
  //void update_samples_from_order_decrement();
  void sample_allocation_metric(Real& regress_metric, Real power);

  /// override certain print functions
  void print_moments(std::ostream& s);
  void print_sobol_indices(std::ostream& s);

  //
  //- Heading: Member function definitions
  //

  /// configure u_space_sampler and approx_type based on regression
  /// specification
  bool config_regression(size_t colloc_pts, size_t regress_size,
			 Iterator& u_space_sampler, Model& g_u_model);

  /// Publish configuration data for initial function train cores, prior to
  /// any adaptation
  void push_c3_core_rank(size_t start_rank);
  /// Publish configuration data for initial function train cores, prior to
  /// any adaptation
  void push_c3_core_orders(const UShortArray& start_orders);
  /// Publish options from C3 input specification (not needed if model-driven
  /// specification: already extracted by iteratedModel)
  void push_c3_db_options();

  //
  //- Heading: Data
  //

  /// user-specified file for importing build points
  String importBuildPointsFile;

  /// scalar specification for initial rank (prior to adapt_rank)
  size_t startRankSpec;
  /// scalar specification for initial basis order (prior to uniform refinement)
  unsigned short startOrderSpec;

private:

  //
  //- Heading: Member function definitions
  //

  //static int qoi_eval(size_t num_samp,        // number of evaluations
  // 			const double* var_sets, // num_vars x num_evals
  // 			double* qoi_sets,       // num_fns x num_evals
  // 			void* args);            // optional arguments

  //
  //- Heading: Data
  //

  // for decremented order without recomputation from previous ranks
  //int prevSamplesOnModel;

  // pointer to the active object instance used within the static evaluator
  // functions in order to avoid the need for static data
  //static NonDC3FunctionTrain* c3Instance;
};
    
} // namespace Dakota

#endif