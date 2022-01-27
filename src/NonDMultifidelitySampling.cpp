/*  _______________________________________________________________________

    DAKOTA: Design Analysis Kit for Optimization and Terascale Applications
    Copyright 2014-2020
    National Technology & Engineering Solutions of Sandia, LLC (NTESS).
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Dakota directory.
    _______________________________________________________________________ */

//- Class:	 NonDMultifidelitySampling
//- Description: Implementation code for NonDMultifidelitySampling class
//- Owner:       Mike Eldred
//- Checked by:
//- Version:

#include "dakota_system_defs.hpp"
#include "dakota_data_io.hpp"
//#include "dakota_tabular_io.hpp"
#include "DakotaModel.hpp"
#include "DakotaResponse.hpp"
#include "NonDMultifidelitySampling.hpp"
#include "ProblemDescDB.hpp"
#include "ActiveKey.hpp"
#include "DakotaIterator.hpp"

#ifdef HAVE_NPSOL
#include "NPSOLOptimizer.hpp"
#endif
#ifdef HAVE_OPTPP
#include "SNLLOptimizer.hpp"
#endif

static const char rcsId[]="@(#) $Id: NonDMultifidelitySampling.cpp 7035 2010-10-22 21:45:39Z mseldre $";

namespace Dakota {


/** This constructor is called for a standard letter-envelope iterator 
    instantiation.  In this case, set_db_list_nodes has been called and 
    probDescDB can be queried for settings from the method specification. */
NonDMultifidelitySampling::
NonDMultifidelitySampling(ProblemDescDB& problem_db, Model& model):
  NonDNonHierarchSampling(problem_db, model)
{
  mlmfSubMethod = SUBMETHOD_MFMC; // if needed for numerical solves
}


NonDMultifidelitySampling::~NonDMultifidelitySampling()
{ }


/** The primary run function manages the general case: a hierarchy of model 
    forms (from the ordered model fidelities within a HierarchSurrModel), 
    each of which may contain multiple discretization levels. */
void NonDMultifidelitySampling::core_run()
{
  //sequence_models(); // enforce correlation condition (*** AFTER PILOT ***)

  // Initialize for pilot sample
  numSamples = pilotSamples[numApprox]; // last in pilot array

  switch (pilotMgmtMode) {
  case  ONLINE_PILOT: // iterated MFMC (default)
    multifidelity_mc();                  break;
  case OFFLINE_PILOT: // computes perf for offline pilot/Oracle correlation
    // convergenceTol option seems problematic since the only reference EstVar
    // comes from offline eval with Oracle/overkill N
    if (maxFunctionEvals == SZ_MAX) {
      Cerr << "Error: budget required for offline pilot mode." << std::endl;
      abort_handler(METHOD_ERROR);
    }
    multifidelity_mc_offline_pilot();    break;
  case PILOT_PROJECTION: // for algorithm assessment/selection
    multifidelity_mc_pilot_projection(); break;
  }

  // Notes on ACV + ensemble model classes:
  // > HierarchSurrModel is limiting (see MFMC) such that we may want to
  //   subsume it with NonHierarchSurrModel --> consider generalizations
  //   that can be deployed across the algorithm set:
  //   >> enhanced parallel usage --> avoid sync points in ACV clients
  //   >> enable sampling over shared + distinct model variable sets
}


/** This is the standard MFMC version that integrates the pilot alongside
    the sample adaptation and iterates to determine numH. */
void NonDMultifidelitySampling::multifidelity_mc()
{
  IntRealVectorMap sum_H;  IntRealMatrixMap sum_L_baseline, sum_LL, sum_LH;
  RealVector sum_HH, hf_targets;    RealMatrix var_L, eval_ratios;
  //Sizet2DArray N_L_baseline, N_LH;
  size_t num_steps = numApprox + 1;
  initialize_mf_sums(sum_L_baseline, sum_H, sum_LL, sum_LH, sum_HH);
  //initialize_counts(N_L_baseline, numH, N_LH);
  numH.assign(numFunctions, 0);

  while (numSamples && mlmfIter <= maxIterations) {

    // ----------------------------------------------------
    // Evaluate shared increment and increment accumulators
    // ----------------------------------------------------
    shared_increment(mlmfIter); // spans ALL models, blocking
    accumulate_mf_sums(sum_L_baseline, sum_H, sum_LL, sum_LH, sum_HH, numH);
    increment_equivalent_cost(numSamples, sequenceCost, 0, num_steps);

    // -------------------------------------------
    // Compute correlations and evaluation ratios:
    // -------------------------------------------
    // First, compute the LF/HF evaluation ratio using shared samples,
    // averaged over QoI.  This includes updating varH and rho2LH.
    compute_LH_correlation(sum_L_baseline[1], sum_H[1], sum_LL[1], sum_LH[1],
			   sum_HH, numH, var_L, varH, rho2LH);
    // estVarIter0 only uses HF pilot since CV terms (sum_L_shared / N_shared
    // - sum_L_refined / N_refined) cancel out prior to sample refinement.
    // (This differs from MLMC EstVar^0 which uses pilot for all levels.)
    if (mlmfIter == 0) {
      compute_mc_estimator_variance(varH, numH, estVarIter0);
      numHIter0 = numH;
    }
    // compute r* from rho2 and cost, either analytically or numerically
    mfmc_eval_ratios(var_L, rho2LH, sequenceCost, approxSequence, eval_ratios,
		     hf_targets);

    // -----------------------------------
    // Compute estimator variance metrics:
    // -----------------------------------
    // Compute the ratio of MC and MFMC estimator variance, which incorporates
    // anticipated variance reduction from upcoming application of eval_ratios.
    // > Note: this could be redundant for tol-based targets with m1* > pilot
    mfmc_estimator_variance(rho2LH, numH, hf_targets, approxSequence,
			    eval_ratios);

    ++mlmfIter;
  }

  // numH is converged --> finalize with LF increments and post-processing
  approx_increments(sum_L_baseline, sum_H, sum_LL, sum_LH, numH, approxSequence,
		    eval_ratios, hf_targets);
}


/** This MFMC version treats the pilot sample as a separate offline process. */
void NonDMultifidelitySampling::multifidelity_mc_offline_pilot()
{
  RealVector sum_H_pilot(numFunctions), sum_HH_pilot(numFunctions), hf_targets;
  RealMatrix sum_L_pilot(numFunctions, numApprox),
    sum_LL_pilot(numFunctions, numApprox),
    sum_LH_pilot(numFunctions, numApprox), var_L, eval_ratios;
  SizetArray N_shared_pilot(numFunctions, 0);
  //Sizet2DArray N_L_pilot, N_LH_pilot;
  //initialize_counts(N_L_pilot, N_H_pilot, N_LH_pilot);
  // ---------------------------------------------------------------------
  // Compute final rho2LH, varH, {eval,estvar} ratios from (oracle) pilot
  // treated as "offline" cost
  // ---------------------------------------------------------------------
  shared_increment(mlmfIter); // spans ALL models, blocking
  accumulate_mf_sums(sum_L_pilot, sum_H_pilot, sum_LL_pilot, sum_LH_pilot,
		     sum_HH_pilot, N_shared_pilot);
  //increment_equivalent_cost(...); // excluded
  compute_LH_correlation(sum_L_pilot, sum_H_pilot, sum_LL_pilot, sum_LH_pilot,
			 sum_HH_pilot, N_shared_pilot, var_L, varH, rho2LH);
  // compute r* from rho2 and cost and update the HF targets
  mfmc_eval_ratios(var_L, rho2LH, sequenceCost, approxSequence, eval_ratios,
		   hf_targets);

  // -----------------------------------
  // Compute "online" sample increments:
  // -----------------------------------
  IntRealVectorMap sum_H;  IntRealMatrixMap sum_L_baseline, sum_LL, sum_LH;
  RealVector sum_HH;     //Sizet2DArray N_L_baseline, N_LH;
  initialize_mf_sums(sum_L_baseline, sum_H, sum_LL, sum_LH, sum_HH);
  numH.assign(numFunctions, 0); //initialize_counts(N_L_baseline, numH, N_LH);
  // at least 2 samples reqd for variance (and resetting allSamples from pilot)
  int sample_increment = one_sided_delta(numH, hf_targets, 1); // numH is 0
  numSamples = std::max(sample_increment, 2);

  // As a first cut, don't reuse any of offline pilot for numH
  shared_increment(++mlmfIter); // spans ALL models, blocking
  accumulate_mf_sums(sum_L_baseline, sum_H, sum_LL, sum_LH, sum_HH, numH);
  increment_equivalent_cost(numSamples, sequenceCost, 0, numApprox+1);

  // Don't replace pilot-based varH (retain "oracle" rho2LH, varH) since this
  // introduces noise in the final MC/MFMC estimator variances.  It does
  // however result in mixing offline varH with online numH for estVarIter0.
  //compute_variance(sum_H[1], sum_HH, numH, varH); // online varH
  // With changes to print_results(), estVarIter0 no longer used for this mode.
  //compute_mc_estimator_variance(varH, numH, estVarIter0);  numHIter0 = numH;
  // Exclude pilot from R^2 benefit, but include any difference between numH
  // and hf_targets:
  mfmc_estimator_variance(rho2LH, numH, hf_targets, approxSequence,eval_ratios);

  // numH is converged --> finalize with LF increments and post-processing
  approx_increments(sum_L_baseline, sum_H, sum_LL, sum_LH, numH, approxSequence,
		    eval_ratios, hf_targets);
}


/** This MFMC version is for algorithm selection; it estimates the
    variance reduction from pilot-only sampling. */
void NonDMultifidelitySampling::multifidelity_mc_pilot_projection()
{
  RealVector sum_H(numFunctions), sum_HH(numFunctions), hf_targets;
  RealMatrix var_L, eval_ratios, sum_L_baseline(numFunctions, numApprox),
    sum_LL(numFunctions, numApprox), sum_LH(numFunctions, numApprox);
  //Sizet2DArray N_L_baseline, N_LH;
  numH.assign(numFunctions, 0); //initialize_counts(N_L_baseline, numH, N_LH);

  // ----------------------------------------------------
  // Evaluate shared increment and increment accumulators
  // ----------------------------------------------------
  shared_increment(mlmfIter); // spans ALL models, blocking
  accumulate_mf_sums(sum_L_baseline, sum_H, sum_LL, sum_LH, sum_HH, numH);
  increment_equivalent_cost(numSamples, sequenceCost, 0, numApprox+1);

  // -------------------------------------------
  // Compute correlations and evaluation ratios:
  // -------------------------------------------
  // First, compute the LF/HF evaluation ratio using shared samples,
  // averaged over QoI.  This includes updating varH and rho2LH.
  compute_LH_correlation(sum_L_baseline, sum_H, sum_LL, sum_LH, sum_HH, numH,
			 var_L, varH, rho2LH);
  // estVarIter0 only uses HF pilot since CV terms (sum_L_shared / N_shared
  // - sum_L_refined / N_refined) cancel out prior to sample refinement.
  // (This differs from MLMC EstVar^0 which uses pilot for all levels.)
  // > Note: numHIter0 may differ from pilotSamples[numApprox] for sim faults
  compute_mc_estimator_variance(varH, numH, estVarIter0);  numHIter0 = numH;
  // compute r* from rho2 and cost
  mfmc_eval_ratios(var_L, rho2LH, sequenceCost, approxSequence, eval_ratios,
		   hf_targets);

  // ----------------------
  // Compute EstVar ratios:
  // ----------------------
  // overwrite actual incurred numH with projected numH
  //SizetArray N_H_projected = numH; // more fine-grained bookkeeping if needed
  Sizet2DArray N_L_projected;  inflate(numH, N_L_projected);
  update_projected_samples(hf_targets, eval_ratios, numH, N_L_projected);
  // Compute the ratio of MC and MFMC mean squared errors, which incorporates
  // anticipated variance reduction from upcoming application of eval_ratios.
  // > Note: this could be redundant for tol-based targets with m1* > pilot
  mfmc_estimator_variance(rho2LH, numH, hf_targets, approxSequence,eval_ratios);

  // No LF increments or final moments for pilot projection

  finalize_counts(N_L_projected);
}


void NonDMultifidelitySampling::
update_hf_targets(const RealMatrix& eval_ratios, const RealVector& cost,
		  RealVector& hf_targets)
{
  // Full budget allocation: pilot sample + addtnl N_H; then optimal N_L 
  // > could consider under-relaxing the budget allocation to enable
  //   additional N_H increments + associated updates to shared samples
  //   for improving rho2LH et al.

  if (hf_targets.empty()) hf_targets.sizeUninitialized(numFunctions);
  // Scale this profile based on specified budget (maxFunctionEvals) if needed
  // using N_H = maxFunctionEvals / cost^T eval_ratios
  // > Pilot case iter = 0: can only scale back after shared_increment().
  //   Optimal profile can be hidden by one_sided_delta() with pilot --> optimal
  //   shape emerges from initialization cost as for ML cases controlled by
  //   convTol (allow budget overshoot due to overlap of optimal with pilot,
  //   rather than strictly allocating remaining budget)
  size_t qoi, approx;
  Real cost_H = cost[numApprox], inner_prod, budget = (Real)maxFunctionEvals;
  switch (optSubProblemForm) {
  // cases which average eval_ratios over QoI: can flatten loops
  case REORDERED_ANALYTIC_SOLUTION: //case N_VECTOR_LINEAR_CONSTRAINT:
    inner_prod = cost_H; // raw cost (un-normalized)
    for (approx=0; approx<numApprox; ++approx)
      inner_prod += cost[approx] * eval_ratios(0, approx);
    hf_targets = budget / inner_prod * cost_H; // normalized to equivHF
    break;
  default:
    for (qoi=0; qoi<numFunctions; ++qoi) {
      inner_prod = cost_H; // raw cost (un-normalized)
      for (approx=0; approx<numApprox; ++approx)
	inner_prod += cost[approx] * eval_ratios(qoi, approx);
      hf_targets[qoi] = budget / inner_prod * cost_H; // normalized to equivHF
    }
    break;
  }

  /* Alternate Scalar estimation (as in ACV)
  RealVector avg_eval_ratios; // *** change incoming API to avoid repeated avg
  average(eval_ratios, 0, avg_eval_ratios);// avg over qoi for each approx
  inner_prod = cost_H; // raw cost (un-normalized)
  for (approx=0; approx<numApprox; ++approx)
    inner_prod += cost[approx] * avg_eval_ratios[approx];
  hf_target = budget / inner_prod * cost_H; // normalized to equivHF
  */

  Cout << "Scaling profile for budget = " << maxFunctionEvals
       << ": average HF target = " << average(hf_targets) << std::endl;

  // For reference, compute est_var if pilot over-sample is ignored
  //mfmc_estvar_ratios(rho2_LH, approx_sequence, eval_ratios, estVarRatios);
  //RealVector est_var(numFunctions);
  //Real rnd_hf_target = std::floor(average(hf_targets) + .5);
  //for (size_t qoi=0; qoi<numFunctions; ++qoi)
  //  est_var[qoi] = estVarRatios[qoi] * varH[qoi] / rnd_hf_target;
  //Cout << "Avg est var neglecting pilot = " << average(est_var) << '\n';
}


void NonDMultifidelitySampling::
update_hf_targets(const RealMatrix& rho2_LH, const SizetArray& approx_sequence,
		  const RealMatrix& eval_ratios,  const RealVector& var_H,
		  const RealVector& estvar_iter0, RealVector& estvar_ratios,
		  RealVector& hf_targets)
{
  // Note: there is a circular dependency between estvar_ratios and hf_targets
  // > 1st compute r*,rho2 --> EstVar*, then compute EstVar*,tol --> m1* target
  // > if pilot > m1*, then done (numSamples = 0) other than reporting _actual_
  //   EstVar including additional pilot (actual EstVar should be < EstVar*,
  //   although estVarRatios may be > estVarRatio* due to decrease in MC EstVar)
  // > if m1* > pilot, then increment numSamples and continue
  mfmc_estvar_ratios(rho2_LH, approx_sequence, eval_ratios, estvar_ratios);
  // This computes estvar_ratios* from r*,rho2.  Next, m1* from estvar_ratios*;
  // then these estvar_ratios get replaced for actual profile

  // EstVar target = convTol * estvar_iter0 = estvar_ratio * var_H / N_H
  // --> N_H = estvar_ratio * var_H / convTol / estvar_iter0
  // Note: don't simplify further since estvar_iter0 is fixed based on pilot
  hf_targets = estvar_ratios;
  for (size_t qoi=0; qoi<numFunctions; ++qoi)
    hf_targets[qoi] *= var_H[qoi] / estvar_iter0[qoi] / convergenceTol;

  Cout << "Scaling profile for convergenceTol = " << convergenceTol
       << ": average HF target = " << average(hf_targets) << std::endl;
}


void NonDMultifidelitySampling::
update_projected_samples(const RealVector& hf_targets,
			 const RealMatrix& eval_ratios,
			 SizetArray& N_H_projected, Sizet2DArray& N_L_projected)
{
  increment_samples(N_H_projected,
		    one_sided_delta(N_H_projected, hf_targets, 1));

  size_t qoi, approx;  RealVector lf_targets(numFunctions, false);
  for (approx=0; approx<numApprox; ++approx) {
    for (qoi=0; qoi<numFunctions; ++qoi)
      lf_targets[qoi] = eval_ratios(qoi, approx) * hf_targets[qoi];
    SizetArray& N_L_a = N_L_projected[approx];
    increment_samples(N_L_a, one_sided_delta(N_L_a, lf_targets, 1));
  }
}


void NonDMultifidelitySampling::
approx_increments(IntRealMatrixMap& sum_L_baseline, IntRealVectorMap& sum_H,
		  IntRealMatrixMap& sum_LL, IntRealMatrixMap& sum_LH,
		  const SizetArray& N_H, const SizetArray& approx_sequence,
		  const RealMatrix& eval_ratios, const RealVector& hf_targets)
{
  // ----------------------------------------------------------------
  // Compute N_L increments based on eval ratio applied to final numH
  // ----------------------------------------------------------------
  // Note: these results do not affect the HF iteration loop and can be
  // performed after numH has converged, which simplifies maxFnEvals / convTol
  // logic (no need to further interrogate these throttles below)

  // maxIterations == 0 was inconsistent for targeting the pilot only case
  // (unlike all other throttle values, it did not follow the converged HF
  // iteration with LF increments).  Other ideas (some previously implemented):
  // > NonDControlVarSampling::finalCVRefinement (can be hard-wired false)
  // > maxFunctionEvals could be used as a second throttle (e.g., set equal to
  //   pilot) with additional checks embedded below
  // > use a special pilot-only mode (now implemented in pilotMgmtMode)

  // Notes on approximation sequencing for MFMC:
  // > approx must be ordered on increasing rho2_LH to enable r_i calculation
  //   (see mfmc_analytic_solution() and mfmc_reordered_analytic_solution())
  // > unlike ACV, we enforce that this ordering is retained within r_i through
  //   linear constraint definitions in nonhierarch_numerical_solution()
  //   >> ACV can order approx sample increments based on decreasing r_i
  //      _after_ an unordered nonhierarch_numerical_solution()

  // Pyramid/nested sampling: at step i, we sample approximation range
  // [0,numApprox-1-i] using the delta relative to the previous step
  IntRealMatrixMap sum_L_shared  = sum_L_baseline,
                   sum_L_refined = sum_L_baseline; // copies
  Sizet2DArray N_L_shared;  inflate(N_H, N_L_shared);
  Sizet2DArray N_L_refined = N_L_shared;
  for (size_t end=numApprox; end>0; --end) {
    // *** TO DO NON_BLOCKING: 2ND PASS ACCUMULATION AFTER 1ST PASS LAUNCH
    if (mfmc_approx_increment(eval_ratios, N_L_refined, hf_targets, mlmfIter,
			      approx_sequence, 0, end)) {
      // MFMC samples on [0, approx) --> sum_L_{shared,refined}
      accumulate_mf_sums(sum_L_shared, sum_L_refined, N_L_shared, N_L_refined,
			 approx_sequence, 0, end);
      increment_equivalent_cost(numSamples,sequenceCost,approx_sequence,0,end);
    }
  }

  // Compute/apply control variate parameter to estimate uncentered raw moments
  RealMatrix H_raw_mom(numFunctions, 4);
  mf_raw_moments(sum_L_baseline, sum_L_shared, sum_L_refined, sum_H, sum_LL,
		 sum_LH, N_L_shared, N_L_refined, numH, H_raw_mom);
  // Convert uncentered raw moment estimates to final moments (central or std)
  convert_moments(H_raw_mom, momentStats);
  // post final sample counts into format for final results reporting
  finalize_counts(N_L_refined);
}


bool NonDMultifidelitySampling::
mfmc_approx_increment(const RealMatrix& eval_ratios,
		      const Sizet2DArray& N_L_refined,
		      const RealVector& hf_targets, size_t iter,
		      const SizetArray& approx_sequence,
		      size_t start, size_t end)
{
  // Update LF samples based on evaluation ratio
  //   r = N_L/N_H -> N_L = r * N_H -> delta = N_L - N_H = (r-1) * N_H
  // Notes:
  // > the sample increment for the approx range is determined by approx[end-1]
  //   (helpful to refer to Figure 2(b) in ACV paper, noting index differences)
  // > N_L is updated prior to each call to approx_increment (*** if BLOCKING),
  //   allowing use of one_sided_delta() with latest counts

  // When to apply averaging requires some care.  To properly enforce scalar
  // budget for vector QoI, need to either scalarize to average targets from
  // the beginning (scalar hf_target -> scalar lf_target as in ACV) or retain
  // per-QoI targets until the very end (i.e., at numSamples estimation).
  // > all HF QoI get sampled together using a scalar count (average(N*_H))
  //   such that using N*_L(q) = r*(q) N*_H(q) seems non-optimal in isolation,
  //   but vector estimation in update_hf_targets() (avoiding any premature
  //   consolidation) requires vector estimation here to hit budget target.
  //   These LF targets subsequently get averaged by one_sided_delta().
  // > would be interesting to compare averaging from the start vs. averaging
  //   at the very end. *** TO DO ***
  //   Probably will be similar, in which case we will prefer the latter since
  //   it leaves the door open for per-QoI optimized sample profiles.
  // > Both MFMC and ACV defer rounding until the end (numSamples estimation).

  RealVector lf_targets(numFunctions, false);
  bool ordered = approx_sequence.empty();
  size_t qoi, approx = (ordered) ? end-1 : approx_sequence[end-1];
  for (qoi=0; qoi<numFunctions; ++qoi)
    lf_targets[qoi] = eval_ratios(qoi, approx) * hf_targets[qoi];
  // These approaches overshoot when combined with vector update_hf_targets():
  //   lf_targets[qoi] = eval_ratios(qoi, approx) * avg_hf_target;
  //   lf_target = avg_eval_ratios[approx] * avg_hf_target;
  const SizetArray& lf_curr = N_L_refined[approx];
  numSamples = one_sided_delta(lf_curr, lf_targets, 1); // average
  if (outputLevel >= DEBUG_OUTPUT)
    Cout << "Approx samples computed from average delta between targets:\n"
	 << lf_targets << " and current counts:\n" << lf_curr << std::endl;

  return approx_increment(iter, approx_sequence, start, end);
}


/** Multi-moment map-based, coarse-grained counter version used by
    MFMC following shared_increment() */
void NonDMultifidelitySampling::
accumulate_mf_sums(IntRealMatrixMap& sum_L_baseline, IntRealVectorMap& sum_H,
		   IntRealMatrixMap& sum_LL, // each L with itself
		   IntRealMatrixMap& sum_LH, // each L with H
		   RealVector& sum_HH, SizetArray& N_shared)
{
  // uses one set of allResponses with QoI aggregation across all Models,
  // ordered by unorderedModels[i-1], i=1:numApprox --> truthModel

  using std::isfinite;
  Real lf_fn, hf_fn, lf_prod, hf_prod;
  IntRespMCIter r_it; IntRVMIter h_it; IntRMMIter lb_it, ll_it, lh_it;
  int lb_ord, h_ord, ll_ord, lh_ord, active_ord, m;
  size_t qoi, approx, lf_index, hf_index;
  bool all_finite;

  for (r_it=allResponses.begin(); r_it!=allResponses.end(); ++r_it) {
    const Response&   resp    = r_it->second;
    const RealVector& fn_vals = resp.function_values();
    //const ShortArray& asv   = resp.active_set_request_vector();

    //if (outputLevel >= DEBUG_OUTPUT) { // sample dump for MATLAB checking
    //  size_t index = 0;
    //  for (approx=0; approx<=numApprox; ++approx)
    // 	  for (qoi=0; qoi<numFunctions; ++qoi, ++index)
    // 	    Cout << fn_vals[index] << ' ';
    //  Cout << '\n';
    //}

    for (qoi=0; qoi<numFunctions; ++qoi) {

      // see fault tol notes in NonDNonHierarchSampling::compute_correlation()
      all_finite = true;
      for (approx=0; approx<=numApprox; ++approx)
	if (!isfinite(fn_vals[approx * numFunctions + qoi])) // NaN or +/-Inf
	  { all_finite = false; break; }

      if (all_finite) {
	++N_shared[qoi];

	// High accumulations:
	hf_index = numApprox * numFunctions + qoi;
	hf_fn = fn_vals[hf_index];
	// High-High:
	sum_HH[qoi] += hf_fn * hf_fn; // a single vector for ord 1
	// High:
	h_it = sum_H.begin();  h_ord = (h_it == sum_H.end()) ? 0 : h_it->first;
	hf_prod = hf_fn;       active_ord = 1;
	while (h_ord) {
	  if (h_ord == active_ord) { // support general key sequence
	    h_it->second[qoi] += hf_prod;
	    ++h_it; h_ord = (h_it == sum_H.end()) ? 0 : h_it->first;
	  }
	  hf_prod *= hf_fn;  ++active_ord;
	}

	for (approx=0; approx<numApprox; ++approx) {
	  // Low accumulations:
	  lf_index = approx * numFunctions + qoi;
	  lf_fn = fn_vals[lf_index];

	  lb_it = sum_L_baseline.begin();
	  ll_it = sum_LL.begin();  lh_it = sum_LH.begin();
	  lb_ord = (lb_it == sum_L_baseline.end()) ? 0 : lb_it->first;
	  ll_ord = (ll_it == sum_LL.end())         ? 0 : ll_it->first;
	  lh_ord = (lh_it == sum_LH.end())         ? 0 : lh_it->first;
	  lf_prod = lf_fn;  hf_prod = hf_fn;  active_ord = 1;
	  while (lb_ord || ll_ord || lh_ord) {

	    // Low baseline
	    if (lb_ord == active_ord) { // support general key sequence
	      lb_it->second(qoi,approx) += lf_prod;  ++lb_it;
	      lb_ord = (lb_it == sum_L_baseline.end()) ? 0 : lb_it->first;
	    }
	    // Low-Low
	    if (ll_ord == active_ord) { // support general key sequence
	      ll_it->second(qoi,approx) += lf_prod * lf_prod;  ++ll_it;
	      ll_ord = (ll_it == sum_LL.end()) ? 0 : ll_it->first;
	    }
	    // Low-High
	    if (lh_ord == active_ord) { // support general key sequence
	      lh_it->second(qoi,approx) += lf_prod * hf_prod;
	      ++lh_it; lh_ord = (lh_it == sum_LH.end()) ? 0 : lh_it->first;
	    }

	    lf_prod *= lf_fn;  hf_prod *= hf_fn;  ++active_ord;
	  }
	}
      }
    }
  }
}


/** Single moment, coarse-grained counter version used by offline-pilot
    and pilot-projection MFMC following shared_increment() */
void NonDMultifidelitySampling::
accumulate_mf_sums(RealMatrix& sum_L_baseline, RealVector& sum_H,
		   RealMatrix& sum_LL, RealMatrix& sum_LH,
		   RealVector& sum_HH, SizetArray& N_shared)
{
  // uses one set of allResponses with QoI aggregation across all Models,
  // ordered by unorderedModels[i-1], i=1:numApprox --> truthModel

  using std::isfinite;
  Real lf_fn, hf_fn;  size_t qoi, approx, lf_index, hf_index;
  IntRespMCIter r_it; bool all_finite;

  for (r_it=allResponses.begin(); r_it!=allResponses.end(); ++r_it) {
    const Response&   resp    = r_it->second;
    const RealVector& fn_vals = resp.function_values();
    //const ShortArray& asv   = resp.active_set_request_vector();

    /*
    if (outputLevel >= DEBUG_OUTPUT) { // sample dump for MATLAB checking
      size_t index = 0;
      for (approx=0; approx<=numApprox; ++approx)
	for (qoi=0; qoi<numFunctions; ++qoi, ++index)
	  Cout << fn_vals[index] << ' ';
      Cout << '\n';
    }
    */

    for (qoi=0; qoi<numFunctions; ++qoi) {

      // see fault tol notes in NonDNonHierarchSampling::compute_correlation()
      all_finite = true;
      for (approx=0; approx<=numApprox; ++approx)
	if (!isfinite(fn_vals[approx * numFunctions + qoi])) // NaN or +/-Inf
	  { all_finite = false; break; }

      if (all_finite) {
	++N_shared[qoi];
	// High accumulations:
	hf_index = numApprox * numFunctions + qoi;
	hf_fn = fn_vals[hf_index];
	sum_H[qoi]  += hf_fn;         // High
	sum_HH[qoi] += hf_fn * hf_fn; // High-High

	for (approx=0; approx<numApprox; ++approx) {
	  lf_index = approx * numFunctions + qoi;
	  lf_fn = fn_vals[lf_index];
	  // Low accumulations:
	  sum_L_baseline(qoi,approx) += lf_fn; // Low
	  sum_LL(qoi,approx) += lf_fn * lf_fn; // Low-Low
	  // Low-High accumulation:
	  sum_LH(qoi,approx) += lf_fn * hf_fn;
	}
      }
    }
  }
}




/** Multi-moment map-based, fine-grained counter version used by MFMC
    following shared_increment()
void NonDMultifidelitySampling::
accumulate_mf_sums(IntRealMatrixMap& sum_L_baseline, IntRealVectorMap& sum_H,
		   IntRealMatrixMap& sum_LL, // each L with itself
		   IntRealMatrixMap& sum_LH, // each L with H
		   RealVector& sum_HH, Sizet2DArray& num_L_baseline,
		   SizetArray& num_H,  Sizet2DArray& num_LH)
{
  // uses one set of allResponses with QoI aggregation across all Models,
  // ordered by unorderedModels[i-1], i=1:numApprox --> truthModel

  using std::isfinite;
  Real lf_fn, hf_fn, lf_prod, hf_prod;
  IntRespMCIter r_it; IntRVMIter h_it; IntRMMIter lb_it, ll_it, lh_it;
  int lb_ord, h_ord, ll_ord, lh_ord, active_ord, m;
  size_t qoi, approx, lf_index, hf_index;
  bool hf_is_finite;

  for (r_it=allResponses.begin(); r_it!=allResponses.end(); ++r_it) {
    const Response&   resp    = r_it->second;
    const RealVector& fn_vals = resp.function_values();
    //const ShortArray& asv   = resp.active_set_request_vector();

    //if (outputLevel >= DEBUG_OUTPUT) { // sample dump for MATLAB checking
    //  size_t index = 0;
    //  for (approx=0; approx<=numApprox; ++approx)
    // 	for (qoi=0; qoi<numFunctions; ++qoi, ++index)
    // 	  Cout << fn_vals[index] << ' ';
    //  Cout << '\n';
    //}

    hf_index = numApprox * numFunctions;
    for (qoi=0; qoi<numFunctions; ++qoi, ++hf_index) {
      hf_fn = fn_vals[hf_index];
      hf_is_finite = isfinite(hf_fn);
      // High accumulations:
      if (hf_is_finite) { // neither NaN nor +/-Inf
	++num_H[qoi];
	// High-High:
	sum_HH[qoi] += hf_fn * hf_fn; // a single vector for ord 1
	// High:
	h_it = sum_H.begin();  h_ord = (h_it == sum_H.end()) ? 0 : h_it->first;
	hf_prod = hf_fn;       active_ord = 1;
	while (h_ord) {
	  if (h_ord == active_ord) { // support general key sequence
	    h_it->second[qoi] += hf_prod;
	    ++h_it; h_ord = (h_it == sum_H.end()) ? 0 : h_it->first;
	  }
	  hf_prod *= hf_fn;  ++active_ord;
	}
      }

      for (approx=0; approx<numApprox; ++approx) {
	lf_index = approx * numFunctions + qoi;
	lf_fn = fn_vals[lf_index];

	// Low accumulations:
	if (isfinite(lf_fn)) {
	  ++num_L_baseline[approx][qoi];
	  if (hf_is_finite) ++num_LH[approx][qoi];

	  lb_it = sum_L_baseline.begin();
	  ll_it = sum_LL.begin();  lh_it = sum_LH.begin();
	  lb_ord = (lb_it == sum_L_baseline.end()) ? 0 : lb_it->first;
	  ll_ord = (ll_it == sum_LL.end())         ? 0 : ll_it->first;
	  lh_ord = (lh_it == sum_LH.end())         ? 0 : lh_it->first;
	  lf_prod = lf_fn;  hf_prod = hf_fn;  active_ord = 1;
	  while (lb_ord || ll_ord || lh_ord) {

	    // Low baseline
	    if (lb_ord == active_ord) { // support general key sequence
	      lb_it->second(qoi,approx) += lf_prod;  ++lb_it;
	      lb_ord = (lb_it == sum_L_baseline.end()) ? 0 : lb_it->first;
	    }
	    // Low-Low
	    if (ll_ord == active_ord) { // support general key sequence
	      ll_it->second(qoi,approx) += lf_prod * lf_prod;  ++ll_it;
	      ll_ord = (ll_it == sum_LL.end()) ? 0 : ll_it->first;
	    }
	    // Low-High
	    if (lh_ord == active_ord) {
	      if (hf_is_finite)
		lh_it->second(qoi,approx) += lf_prod * hf_prod;
	      ++lh_it; lh_ord = (lh_it == sum_LH.end()) ? 0 : lh_it->first;
	    }

	    lf_prod *= lf_fn;  ++active_ord;
	    if (hf_is_finite)  hf_prod *= hf_fn;
	  }
	}
      }
    }
  }
}
*/


/** Single moment, fine-grained counter version used by offline-pilot
    and pilot-projection MFMC following shared_increment()
void NonDMultifidelitySampling::
accumulate_mf_sums(RealMatrix& sum_L_baseline, RealVector& sum_H,
		   RealMatrix& sum_LL, RealMatrix& sum_LH,
		   RealVector& sum_HH, Sizet2DArray& num_L_baseline,
		   SizetArray& num_H,  Sizet2DArray& num_LH)
{
  // uses one set of allResponses with QoI aggregation across all Models,
  // ordered by unorderedModels[i-1], i=1:numApprox --> truthModel

  using std::isfinite;
  Real lf_fn, hf_fn;  size_t qoi, approx, lf_index, hf_index;
  IntRespMCIter r_it; bool hf_is_finite;

  for (r_it=allResponses.begin(); r_it!=allResponses.end(); ++r_it) {
    const Response&   resp    = r_it->second;
    const RealVector& fn_vals = resp.function_values();
    //const ShortArray& asv   = resp.active_set_request_vector();

    //if (outputLevel >= DEBUG_OUTPUT) { // sample dump for MATLAB checking
    //  size_t index = 0;
    //  for (approx=0; approx<=numApprox; ++approx)
    // 	for (qoi=0; qoi<numFunctions; ++qoi, ++index)
    // 	  Cout << fn_vals[index] << ' ';
    //  Cout << '\n';
    //}

    hf_index = numApprox * numFunctions;
    for (qoi=0; qoi<numFunctions; ++qoi, ++hf_index) {
      hf_fn = fn_vals[hf_index];
      hf_is_finite = isfinite(hf_fn);
      // High accumulations:
      if (hf_is_finite) { // neither NaN nor +/-Inf
	++num_H[qoi];
	sum_H[qoi]  += hf_fn;         // High
	sum_HH[qoi] += hf_fn * hf_fn; // High-High
      }

      for (approx=0; approx<numApprox; ++approx) {
	lf_index = approx * numFunctions + qoi;
	lf_fn = fn_vals[lf_index];

	// Low accumulations:
	if (isfinite(lf_fn)) {
	  ++num_L_baseline[approx][qoi];
	  sum_L_baseline(qoi,approx) += lf_fn; // Low
	  sum_LL(qoi,approx) += lf_fn * lf_fn; // Low-Low
	  if (hf_is_finite) {
	    ++num_LH[approx][qoi];
	    sum_LH(qoi,approx) += lf_fn * hf_fn;// Low-High
	  }
	}
      }
    }
  }
}
*/


/** This version used by MFMC following approx_increment() */
void NonDMultifidelitySampling::
accumulate_mf_sums(IntRealMatrixMap& sum_L_shared,
		   IntRealMatrixMap& sum_L_refined, Sizet2DArray& num_L_shared,
		   Sizet2DArray& num_L_refined,
		   const SizetArray& approx_sequence,
		   size_t sequence_start, size_t sequence_end)
{
  // uses one set of allResponses with QoI aggregation across all Models,
  // led by the approx Model responses of interest

  using std::isfinite;
  Real fn_val, prod;
  int ls_ord, lr_ord, active_ord;
  size_t s, qoi, fn_index, approx, shared_end = sequence_end - 1;
  bool ordered = approx_sequence.empty();
  IntRespMCIter r_it; IntRMMIter ls_it, lr_it;

  for (r_it=allResponses.begin(); r_it!=allResponses.end(); ++r_it) {
    const Response&   resp    = r_it->second;
    const RealVector& fn_vals = resp.function_values();
    //const ShortArray& asv   = resp.active_set_request_vector();

    // accumulate for leading set of models (omit trailing truth),
    // but note that resp and asv are full aggregated length
    for (s=sequence_start; s<sequence_end; ++s) {

      approx = (ordered) ? s : approx_sequence[s];
      SizetArray& num_L_sh_a  = num_L_shared[approx];
      SizetArray& num_L_ref_a = num_L_refined[approx];
      fn_index = approx * numFunctions;

      for (qoi=0; qoi<numFunctions; ++qoi, ++fn_index) {
	//if (asv[fn_index] & 1) {
	  prod = fn_val = fn_vals[fn_index];
	  if (isfinite(fn_val)) { // neither NaN nor +/-Inf
	    ++num_L_ref_a[qoi];  active_ord = 1;
	    lr_it = sum_L_refined.begin();
	    lr_ord = (lr_it == sum_L_refined.end()) ? 0 : lr_it->first;
	    // for pyramid sampling, shared range is one less than refined,
	    // i.e. sum_L_{shared,refined} are both accumulated for all s except
	    // sequence_end-1, which accumulates only sum_L_refined.  See z^1
	    // sets in Fig. 2b of ACV paper.
	    if (s < shared_end) {
	      ++num_L_sh_a[qoi];
	      ls_it = sum_L_shared.begin();
	      ls_ord = (ls_it == sum_L_shared.end()) ? 0 : ls_it->first;
	    }
	    else
	      ls_ord = 0;
	    while (ls_ord || lr_ord) {
	      // Low shared
	      if (ls_ord == active_ord) { // support general key sequence
		ls_it->second(qoi,approx) += prod;  ++ls_it;
		ls_ord = (ls_it == sum_L_shared.end()) ? 0 : ls_it->first;
	      }
	      // Low refined
	      if (lr_ord == active_ord) { // support general key sequence
		lr_it->second(qoi,approx) += prod;  ++lr_it;
		lr_ord = (lr_it == sum_L_refined.end()) ? 0 : lr_it->first;
	      }
	      prod *= fn_val;  ++active_ord;
	    }
	  }
	//}
      }
    }
  }
}


/* Deactivated (see notes in compute_correlation())
void NonDMultifidelitySampling::
compute_LH_correlation(const RealMatrix& sum_L_shared, const RealVector& sum_H,
		       const RealMatrix& sum_LL, const RealMatrix& sum_LH,
		       const RealVector& sum_HH, const Sizet2DArray& N_L_shared,
		       const SizetArray& N_H,    const Sizet2DArray& N_LH,
		       RealMatrix& var_L,        RealVector& var_H,
		       RealMatrix& rho2_LH)
{
  if (var_L.empty())     var_L.shapeUninitialized(numFunctions, numApprox);
  if (var_H.empty())     var_H.sizeUninitialized(numFunctions);
  if (rho2_LH.empty()) rho2_LH.shapeUninitialized(numFunctions, numApprox);

  size_t approx, qoi;
  for (approx=0; approx<numApprox; ++approx) {
    const Real* sum_L_shared_a = sum_L_shared[approx];
    const Real*       sum_LL_a =       sum_LL[approx];
    const Real*       sum_LH_a =       sum_LH[approx];
    const SizetArray&    N_L_a =   N_L_shared[approx];
    const SizetArray&   N_LH_a =         N_LH[approx];
    Real*              var_L_a =        var_L[approx];
    Real*            rho2_LH_a =      rho2_LH[approx];
    for (qoi=0; qoi<numFunctions; ++qoi)
      compute_correlation(sum_L_shared_a[qoi], sum_H[qoi], sum_LL_a[qoi],
			  sum_LH_a[qoi], sum_HH[qoi], N_L_a[qoi], N_H[qoi],
			  N_LH_a[qoi], var_L_a[qoi], var_H[qoi],
			  rho2_LH_a[qoi]);
  }

  if (outputLevel >= DEBUG_OUTPUT)
    Cout << "rho2_LH in compute_LH_correlation():\n" << rho2_LH << std::endl;
}
*/


void NonDMultifidelitySampling::
compute_LH_correlation(const RealMatrix& sum_L_shared, const RealVector& sum_H,
		       const RealMatrix& sum_LL, const RealMatrix& sum_LH,
		       const RealVector& sum_HH, const SizetArray& N_shared,
		       RealMatrix& var_L, RealVector& var_H,RealMatrix& rho2_LH)
{
  if (var_L.empty())     var_L.shapeUninitialized(numFunctions, numApprox);
  if (var_H.empty())     var_H.sizeUninitialized(numFunctions);
  if (rho2_LH.empty()) rho2_LH.shapeUninitialized(numFunctions, numApprox);

  size_t approx, qoi;
  for (approx=0; approx<numApprox; ++approx) {
    const Real* sum_L_shared_a = sum_L_shared[approx];
    const Real*       sum_LL_a =       sum_LL[approx];
    const Real*       sum_LH_a =       sum_LH[approx];
    Real*              var_L_a =        var_L[approx];
    Real*            rho2_LH_a =      rho2_LH[approx];
    for (qoi=0; qoi<numFunctions; ++qoi)
      compute_correlation(sum_L_shared_a[qoi], sum_H[qoi], sum_LL_a[qoi],
			  sum_LH_a[qoi], sum_HH[qoi], N_shared[qoi],
			  var_L_a[qoi], var_H[qoi], rho2_LH_a[qoi]);
  }

  if (outputLevel >= DEBUG_OUTPUT)
    Cout << "rho2_LH in compute_LH_correlation():\n" << rho2_LH << std::endl;
}


void NonDMultifidelitySampling::
mfmc_eval_ratios(const RealMatrix& var_L, const RealMatrix& rho2_LH,
		 const RealVector& cost,  SizetArray& approx_sequence,
		 RealMatrix& eval_ratios, RealVector& hf_targets)
               //bool for_warm_start)
{
  if (eval_ratios.empty())
    eval_ratios.shapeUninitialized(numFunctions, numApprox);

  // -------------------------------------------------------------
  // Based on rho2_LH sequencing, determine best solution approach
  // -------------------------------------------------------------
  // compute a model sequence sorted by Low-High correlation
  // > rho2, N_L, N_H, {eval,estvar}_ratios, etc. are all ordered based on the
  //   user-provided model list ordering
  // > we employ approx_sequence to pair approximations using a different order
  //   for computing rho2_diff, cost --> eval_ratios --> estvar_ratios, but the
  //   results are indexed by the original approx ordering
  // > control variate compute/apply are per approx, so sequencing not required
  // > approx_increment requires model sequence to define the sample pyramid

  // Bomarito & Warner (NASA LaRC): stay within a numerical ACV-like approach
  // by defining F for this graph (hierarchical MFMC rather than peer ACV).
  // Encounter singularity when models are not sequenced for this graph,
  // which is addressed numerically by introducing a (diagonal) nugget.

  if (ordered_approx_sequence(rho2_LH)) // for all QoI across Approx sequence
    optSubProblemForm = ANALYTIC_SOLUTION;  
  else
    optSubProblemForm = //(for_warm_start) ? REORDERED_ANALYTIC_SOLUTION :
      N_VECTOR_LINEAR_CONSTRAINT;

  switch (optSubProblemForm) {
  case ANALYTIC_SOLUTION:
    Cout << "MFMC: model sequence provided is ordered in Low-High correlation "
	 << "for all QoI.\n      Computing standard analytic solution.\n"
	 << std::endl;
    approx_sequence.clear();
    mfmc_analytic_solution(rho2_LH, cost, eval_ratios);
    break;
  case REORDERED_ANALYTIC_SOLUTION: // inactive (future user override?)
    Cout << "MFMC: model sequence provided is out of order with respect to "
	 << "Low-High\n      correlation for at least one QoI.  Switching to "
	 << "alternate analytic solution.\n";
    mfmc_reordered_analytic_solution(rho2_LH, cost, approx_sequence,
				     eval_ratios, true); // monotonic r for seq
    break;
  default: { // any of several numerical optimization formulations
    Cout << "MFMC: model sequence provided is out of order with respect to "
	 << "Low-High\n      correlation for at least one QoI.  Switching to "
	 << "numerical solution.\n";
    Real avg_hf_target;
    mfmc_numerical_solution(var_L, rho2_LH, cost, approx_sequence, eval_ratios,
			    avg_hf_target);
    if (hf_targets.empty()) hf_targets.sizeUninitialized(numFunctions);
    hf_targets = avg_hf_target; // assign scalar to vector components
    break;
  }
  }

  // Numerical solution has updated hf_target from the computed N*.
  // Analytic solutions scale the profile to target budget | accuracy.
  switch (optSubProblemForm) {
  case ANALYTIC_SOLUTION:  case REORDERED_ANALYTIC_SOLUTION:
    if (maxFunctionEvals != SZ_MAX)
      update_hf_targets(eval_ratios, sequenceCost, hf_targets);
    else //if (convergenceTol != -DBL_MAX) *** TO DO: need special default value
      update_hf_targets(rho2_LH, approx_sequence, eval_ratios, varH,
			estVarIter0, estVarRatios, hf_targets);
    break;
  }
}


void NonDMultifidelitySampling::
mfmc_numerical_solution(const RealMatrix& var_L, const RealMatrix& rho2_LH,
			const RealVector& cost,  SizetArray& approx_sequence,
			RealMatrix& eval_ratios, Real& avg_hf_target)
{
  size_t qoi, approx, num_am1 = numApprox - 1;
  Real cost_L, cost_H = cost[numApprox], budget = (Real)maxFunctionEvals,
    avg_N_H = average(numH), r_i;
  RealVector avg_eval_ratios;

  if (mlmfIter == 0) {

    if (equivHFEvals >= budget) // only 1 feasible pt, no need for solve
      { eval_ratios = 1.;  avg_hf_target = avg_N_H;  return; }
    else { // compute initial estimate of r* from analytic MFMC

      // generate an initial guess using reordered approach (we know ordered
      // analytic can't be used or we wouldn't be using the numerical option).
      // Enforce that r increases monotonically across the approx_sequence for
      // consistency with linear constraints in the numerical soln to follow.
      mfmc_reordered_analytic_solution(rho2_LH, cost, approx_sequence,
				       eval_ratios, true);// monotonic r for seq
      average(eval_ratios, 0, avg_eval_ratios);// avg over qoi for each approx
      if (outputLevel >= NORMAL_OUTPUT)
        Cout << "Initial guess from analytic MFMC (average eval ratios):\n"
	     << avg_eval_ratios << std::endl;

      // scale to enforce budget constraint.  Since the profile does not emerge
      // (make numerical MFMC more resilient to pilot over-estimation like ACV),
      // don't select an infeasible initial guess:
      // > if N* < N_pilot, scale back r* for use initial = scaled_r*,N_pilot
      // > if N* > N_pilot, use initial = r*,N*
      avg_hf_target = allocate_budget(avg_eval_ratios, cost);
      if (avg_N_H > avg_hf_target) { // rescale r* for over-estimated pilot
	scale_to_budget_with_pilot(avg_eval_ratios, cost, avg_N_H);
	avg_hf_target = avg_N_H;
      }
    }
  }
  else //warm start from previous solution
    average(eval_ratios, 0, avg_eval_ratios);// avg over qoi

  // define covLH and covLL from rho2LH, var_L, varH
  correlation_sq_to_covariance(rho2_LH, var_L, varH, covLH);
  matrix_to_diagonal_array(var_L, covLL);

  // Base class implementation of numerical solve (shared with ACV):
  nonhierarch_numerical_solution(cost, approx_sequence, avg_eval_ratios,
				 avg_hf_target, numSamples, avgEstVar,
				 avgEstVarRatio);
  // MFMC normally uses a matrix of eval ratios, but numerical opt flattens
  // to a vector of design vars
  inflate(avg_eval_ratios, eval_ratios);
}


void NonDMultifidelitySampling::
mfmc_estimator_variance(const RealMatrix& rho2_LH, const SizetArray& N_H,
			const RealVector& hf_targets,
			const SizetArray& approx_sequence,
			const RealMatrix& eval_ratios)
{
  switch (optSubProblemForm) {
  // For these cases, it is convenient to compute estimator variance ratios
  // using the expression for R^2
  case ANALYTIC_SOLUTION:  case REORDERED_ANALYTIC_SOLUTION: {

    if (estVarRatios.empty()) estVarRatios.sizeUninitialized(numFunctions);

    bool scale_to_N_H = false;
    Real avg_hf_target = average(hf_targets);
    size_t qoi, approx, rnd_hf_target = (size_t)std::floor(avg_hf_target + .5);
    for (qoi=0; qoi<numFunctions; ++qoi)
      if (N_H[qoi] > rnd_hf_target)// over-shoot of tgt from pilot | iteration
	scale_to_N_H = true;

    if (scale_to_N_H) {
      // R^2 = \Sum_i [ (r_i -r_{i-1})/(r_i r_{i-1}) rho2_LH_i ]
      // --> take credit for N_H > N* by using r_actual < r* for N_H > m1*
      // --> N_L is kept fixed at r* m1* (see lf_targets in approx_increment()),
      //     but r_actual = N_L / N_H = r* m1* / N_H
      RealMatrix scaled_eval_ratios = eval_ratios; // copy
      Real star_to_actual;
      for (qoi=0; qoi<numFunctions; ++qoi) {
	star_to_actual = avg_hf_target / (Real)N_H[qoi];
	for (approx=0; approx<numApprox; ++approx) // no need to sequence
	  scaled_eval_ratios(qoi, approx) *= star_to_actual; // r* -> r_actual
      }
      mfmc_estvar_ratios(rho2_LH, approx_sequence, scaled_eval_ratios,
			 estVarRatios);

      // verify correct N_L is preserved after star_to_actual:
      //for (approx=0; approx<numApprox; ++approx) 
      //  Cout << "avg N_L[" <<approx<< "] = "<< N_L[approx]/numFunctions<<'\n';
    }
    else
      mfmc_estvar_ratios(rho2_LH, approx_sequence, eval_ratios, estVarRatios);

    if (outputLevel >= NORMAL_OUTPUT) {
      bool ordered = approx_sequence.empty();
      for (qoi=0; qoi<numFunctions; ++qoi) {
	for (size_t i=0; i<numApprox; ++i) {
	  approx = (ordered) ? i : approx_sequence[i];
	  Cout << "  QoI " << qoi+1 << " Approx " << approx+1
	    //<< ": cost_ratio = " << cost_H / cost_L
	       << ": rho2_LH = "    <<     rho2_LH(qoi,approx)
	       << " eval_ratio = "  << eval_ratios(qoi,approx) << '\n';
	}
	Cout << "QoI " << qoi+1 << ": variance reduction factor = "
	     << estVarRatios[qoi] << '\n';
      }
      Cout << std::endl;
    }
    // numSamples is relative to numH, but the approx_increments() below are
    // computed relative to hf_targets (independent of sunk cost for pilot)
    numSamples = one_sided_delta(numH, hf_targets, 1);
    //numSamples = std::min(num_samp_budget, num_samp_ctol); // enforce both
    break;
  }
  // For numerical cases, mfmc_numerical_solution() must incorporate varH/numH
  // in the objective and returns avg estvar as the final objective.  So estVar
  // is more direct here than estVarRatios, as for NonDACVSampling.
  //default:
  //  if (estvar_ratios.empty()) estvar_ratios.sizeUninitialized(numFunctions);
  //  for (size_t qoi=0; qoi<numFunctions; ++qoi)
  //    estvar_ratios[qoi] = avgEstVar / var_H[qoi] * N_H[qoi]; // (1-R^2)
  //  break;
  }
}


void NonDMultifidelitySampling::
mf_raw_moments(IntRealMatrixMap& sum_L_baseline, IntRealMatrixMap& sum_L_shared,
	       IntRealMatrixMap& sum_L_refined,  IntRealVectorMap& sum_H,
	       IntRealMatrixMap& sum_LL,       IntRealMatrixMap& sum_LH,
	       const Sizet2DArray& N_L_shared, const Sizet2DArray& N_L_refined,
	       const SizetArray&   N_H,        RealMatrix& H_raw_mom)
{
  // Note: ACV-like numerical solutions solve all-at-once for beta as a vector
  // > beta = [ C o F ]^{-1} [diag(F) o c] which, for diagonal F in MFMC,
  //   simplifies to beta_i = F_ii c_i / F_ii C_ii = covLH_i / varL_i
  // --> no need to incur matrix inversion overhead for MFMC; stick with same
  //     scalar approach used for analytic solutions.

  if (H_raw_mom.empty()) H_raw_mom.shapeUninitialized(numFunctions, 4);

  Real beta, sum_H_mq;
  size_t approx, qoi, N_H_q;//, N_shared;
  for (int mom=1; mom<=4; ++mom) {
    RealMatrix& sum_L_base_m = sum_L_baseline[mom];
    RealMatrix& sum_L_sh_m   = sum_L_shared[mom];
    RealMatrix& sum_L_ref_m  = sum_L_refined[mom];
    RealVector& sum_H_m      = sum_H[mom];
    RealMatrix& sum_LL_m     = sum_LL[mom];
    RealMatrix& sum_LH_m     = sum_LH[mom];

    if (outputLevel >= NORMAL_OUTPUT)
      Cout << "Moment " << mom << " estimator:\n";
    for (qoi=0; qoi<numFunctions; ++qoi) {
      sum_H_mq = sum_H_m[qoi];  N_H_q = N_H[qoi];
      Real& H_raw_mq = H_raw_mom(qoi, mom-1);
      H_raw_mq = sum_H_mq / N_H_q; // first term to be augmented
      for (approx=0; approx<numApprox; ++approx) {
	// Uses a baseline {sum,N}_L consistent with H,LL,LH accumulators:
	compute_mf_control(sum_L_base_m(qoi,approx), sum_H_mq,
			   sum_LL_m(qoi,approx), sum_LH_m(qoi,approx),
			   N_H_q, beta); // shared HF baseline
	if (outputLevel >= NORMAL_OUTPUT)
	  Cout << "   QoI " << qoi+1 << " Approx " << approx+1
	       << ": control variate beta = " << std::setw(9) << beta << '\n';
	// For MFMC, shared accumulators and counts telescope pairwise
	apply_control(sum_L_sh_m(qoi,approx),  N_L_shared[approx][qoi],
		      sum_L_ref_m(qoi,approx), N_L_refined[approx][qoi],
		      beta, H_raw_mq);
      }
    }
  }
  if (outputLevel >= NORMAL_OUTPUT) Cout << std::endl;
}


void NonDMultifidelitySampling::print_variance_reduction(std::ostream& s)
{
  switch (optSubProblemForm) {
  // For these cases, it is convenient to start from estVarRatios
  case ANALYTIC_SOLUTION:  case REORDERED_ANALYTIC_SOLUTION: {
    size_t wpp7 = write_precision + 7;
    s << "<<<<< Variance for mean estimator:\n";

    if (pilotMgmtMode != OFFLINE_PILOT)
      s << "      Initial MC (" << std::setw(4)
	<< (size_t)std::floor(average(numHIter0) + .5) << " HF samples): "
	<< std::setw(wpp7) << average(estVarIter0)<<'\n';

    RealVector mc_est_var(numFunctions, false),
             mfmc_est_var(numFunctions, false);
    for (size_t qoi=0; qoi<numFunctions; ++qoi) {
      mfmc_est_var[qoi]  = mc_est_var[qoi] = varH[qoi] / numH[qoi];
      mfmc_est_var[qoi] *= estVarRatios[qoi];
    }
    Real avg_mfmc_est_var = average(mfmc_est_var),
         avg_mc_est_var   = average(mc_est_var);
    String type = (pilotMgmtMode == PILOT_PROJECTION) ? "Projected":"    Final";
    s << "  " << type << "   MC (" << std::setw(4)
      << (size_t)std::floor(average(numH) + .5) << " HF samples): "
      << std::setw(wpp7) << avg_mc_est_var
      << "\n  " << type << " MFMC (sample profile):  "
      << std::setw(wpp7) << avg_mfmc_est_var
      << "\n  " << type << " MFMC ratio (1 - R^2):   "
      // report ratio of averages rather than average of ratios (consistent
      // with ACV definition which would have to recompute the latter)
      << std::setw(wpp7) << avg_mfmc_est_var / avg_mc_est_var << '\n';
    break;
  }
  // For numerical cases, mfmc_numerical_solution() must incorporate varH/numH
  // in the objective and returns avg estvar as the final objective.  So estVar
  // is more direct here than estVarRatios, as for NonDACVSampling.
  default: // numerical solution
    NonDNonHierarchSampling::print_variance_reduction(s); break;
  }
}

} // namespace Dakota