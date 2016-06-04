/*  _______________________________________________________________________

    DAKOTA: Design Analysis Kit for Optimization and Terascale Applications
    Copyright 2014 Sandia Corporation.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Dakota directory.
    _______________________________________________________________________ */

//- Class:       HierarchSurrModel
//- Description: Implementation code for the HierarchSurrModel class
//- Owner:       Mike Eldred
//- Checked by:

#include "HierarchSurrModel.hpp"
#include "ProblemDescDB.hpp"

static const char rcsId[]="@(#) $Id: HierarchSurrModel.cpp 6656 2010-02-26 05:20:48Z mseldre $";


namespace Dakota {

HierarchSurrModel::HierarchSurrModel(ProblemDescDB& problem_db):
  SurrogateModel(problem_db), hierModelEvalCntr(0),
  truthResponseRef(currentResponse.copy())
{
  // Hierarchical surrogate models pass through numerical derivatives
  supports_derivative_estimation(false);
  // initialize ignoreBounds even though it's irrelevant for pass through
  ignoreBounds = problem_db.get_bool("responses.ignore_bounds");
  // initialize centralHess even though it's irrelevant for pass through
  centralHess = problem_db.get_bool("responses.central_hess");

  const StringArray& ordered_model_ptrs
    = problem_db.get_sa("model.surrogate.ordered_model_pointers");

  size_t i, num_models = ordered_model_ptrs.size(),
    model_index = problem_db.get_db_model_node(); // for restoration

  const std::pair<short,short>& cv_view = currentVariables.view();
  orderedModels.resize(num_models);
  for (i=0; i<num_models; ++i) {
    problem_db.set_db_model_nodes(ordered_model_ptrs[i]);
    orderedModels[i] = problem_db.get_model();
    check_submodel_compatibility(orderedModels[i]);
    if (cv_view != orderedModels[i].current_variables().view()) {
      Cerr << "Error: variable views in hierarchical models must be identical."
	   << std::endl;
      abort_handler(-1);
    }
  }

  problem_db.set_db_model_nodes(model_index); // restore

  // default index values, to be overridden at run time
  lowFidelityIndices.first  = 0; lowFidelityIndices.second = 0;
  highFidelityIndices.first = num_models - 1;
  if (num_models == 1)
    { sameModelInstance = true;  highFidelityIndices.second = 1; }
  else
    { sameModelInstance = false; highFidelityIndices.second = 0; }
  
  // Correction is required in the hierarchical case (since without correction,
  // all HF model evaluations are wasted).  Omission of a correction type
  // should be prevented by the input specification.
  short corr_type = problem_db.get_short("model.surrogate.correction_type");
  if (!corr_type) {
    Cerr << "Error: correction is required with model hierarchies."<< std::endl;
    abort_handler(-1);
  }
  else // initialize the DiscrepancyCorrection using lowest fidelity model;
       // this LF model gets updated at run time
    deltaCorr.initialize(orderedModels[0], surrogateFnIndices, corr_type,
      problem_db.get_short("model.surrogate.correction_order"));
}


void HierarchSurrModel::
derived_init_communicators(ParLevLIter pl_iter, int max_eval_concurrency,
			   bool recurse_flag)
{
  // responseMode is a run-time setting (in SBLMinimizer, it is switched among
  // AUTO_CORRECTED_SURROGATE, BYPASS_SURROGATE, and UNCORRECTED_SURROGATE;
  // in NonDExpansion, it is switching between MODEL_DISCREPANCY and
  // UNCORRECTED_SURROGATE).  Since it is neither static nor generally
  // available at construct/init time, take a conservative approach with init
  // and free and a more aggressive approach with set.

  if (recurse_flag) {
    size_t i, model_index = probDescDB.get_db_model_node(), // for restoration
      num_models = orderedModels.size();

    // init and free must cover possible subset of active responseModes and
    // ordered model fidelities, but only 2 models at mpst will be active at
    // runtime.  In order to reduce the number of parallel configurations, we
    // group the responseModes into two sets: (1) the correction-based set
    // commonly used in surrogate-based optimization et al., and (2) the
    // aggregation-based set commonly used in multilevel/multifidelity UQ.

    // TO DO: would like a better detection option, but passing the mode from
    // the Iterator does not work in parallel w/o an additional bcast (Iterator
    // only instantiated on iteratorComm rank 0).  For now, we will infer it
    // from an associated method spec at init time.
    // Note: responseMode gets bcast at run time in component_parallel_mode()
    bool extra_deriv_config
      = (probDescDB.get_ushort("method.algorithm") & MINIMIZER_BIT);
      //(responseMode == UNCORRECTED_SURROGATE ||
      // responseMode == BYPASS_SURROGATE ||
      // responseMode == AUTO_CORRECTED_SURROGATE);

    for (i=0; i<num_models; ++i) {
      Model& model_i = orderedModels[i];
      // superset of possible init calls (two configurations for i > 0)
      probDescDB.set_db_model_nodes(model_i.model_id());
      model_i.init_communicators(pl_iter, max_eval_concurrency);
      if (extra_deriv_config) // && i) // mid and high fidelity only?
	model_i.init_communicators(pl_iter, model_i.derivative_concurrency());
    }


    /* This version inits only two models
    Model& lf_model = orderedModels[lowFidelityIndices.first];
    Model& hf_model = orderedModels[highFidelityIndices.first];

    // superset of possible init calls (two configurations for HF)
    probDescDB.set_db_model_nodes(lf_model.model_id());
    lf_model.init_communicators(pl_iter, max_eval_concurrency);

    probDescDB.set_db_model_nodes(hf_model.model_id());
    hf_model.init_communicators(pl_iter, hf_model.derivative_concurrency());
    hf_model.init_communicators(pl_iter, max_eval_concurrency);
    */


    /* This version does not support runtime updating of responseMode
    switch (responseMode) {
    case UNCORRECTED_SURROGATE:
      // LF are used in iterator evals
      lf_model.init_communicators(pl_iter, max_eval_concurrency);
      break;
    case AUTO_CORRECTED_SURROGATE:
      // LF are used in iterator evals
      lf_model.init_communicators(pl_iter, max_eval_concurrency);
      // HF evals are for correction and validation:
      // concurrency = one eval at a time * derivative concurrency per eval
      hf_model.init_communicators(pl_iter, hf_model.derivative_concurrency());
      break;
    case BYPASS_SURROGATE:
      // HF are used in iterator evals
      hf_model.init_communicators(pl_iter, max_eval_concurrency);
      break;
    case MODEL_DISCREPANCY: case AGGREGATED_MODELS:
      // LF and HF are used in iterator evals
      lf_model.init_communicators(pl_iter, max_eval_concurrency);
      hf_model.init_communicators(pl_iter, max_eval_concurrency);
      break;
    }
    */

    probDescDB.set_db_model_nodes(model_index); // restore all model nodes
  }
}


void HierarchSurrModel::
derived_set_communicators(ParLevLIter pl_iter, int max_eval_concurrency,
			  bool recurse_flag)
{
  miPLIndex = modelPCIter->mi_parallel_level_index(pl_iter);// run time setting

  // HierarchSurrModels do not utilize default set_ie_asynchronous_mode() as
  // they do not define the ie_parallel_level

  // This aggressive logic is appropriate for invocations of the Model via
  // Iterator::run(), but is fragile w.r.t. invocations of the Model outside
  // this scope (e.g., Model::evaluate() within SBLMinimizer).  The
  // default responseMode value is AUTO_CORRECTED_SURROGATE, which mitigates
  // the specific case of SBLMinimizer, but the general fragility remains.
  if (recurse_flag) {

    // bcast not needed for recurse_flag=false in serve_run call to set_comms
    //if (pl_iter->server_communicator_size() > 1)
    //  parallelLib.bcast(responseMode, *pl_iter);

    switch (responseMode) {
    case UNCORRECTED_SURROGATE: {
      Model& lf_model = orderedModels[lowFidelityIndices.first];
      lf_model.set_communicators(pl_iter, max_eval_concurrency);
      asynchEvalFlag     = lf_model.asynch_flag();
      evaluationCapacity = lf_model.evaluation_capacity();
      break;
    }
    case AUTO_CORRECTED_SURROGATE: {
      Model& lf_model = orderedModels[lowFidelityIndices.first];
      Model& hf_model = orderedModels[highFidelityIndices.first];
      lf_model.set_communicators(pl_iter, max_eval_concurrency);
      int hf_deriv_conc = hf_model.derivative_concurrency();
      hf_model.set_communicators(pl_iter, hf_deriv_conc);
      asynchEvalFlag = ( lf_model.asynch_flag() ||
	( hf_deriv_conc > 1 && hf_model.asynch_flag() ) );
      evaluationCapacity = std::max( lf_model.evaluation_capacity(),
				     hf_model.evaluation_capacity() );
      break;
    }
    case BYPASS_SURROGATE: {
      Model& hf_model = orderedModels[highFidelityIndices.first];
      hf_model.set_communicators(pl_iter, max_eval_concurrency);
      asynchEvalFlag     = hf_model.asynch_flag();
      evaluationCapacity = hf_model.evaluation_capacity();
      break;
    }
    case MODEL_DISCREPANCY: case AGGREGATED_MODELS: {
      Model& lf_model = orderedModels[lowFidelityIndices.first];
      Model& hf_model = orderedModels[highFidelityIndices.first];
      lf_model.set_communicators(pl_iter, max_eval_concurrency);
      if (sameModelInstance) {
 	asynchEvalFlag = lf_model.asynch_flag();
	evaluationCapacity = lf_model.evaluation_capacity();
      }
      else {
	hf_model.set_communicators(pl_iter, max_eval_concurrency);
	asynchEvalFlag = ( lf_model.asynch_flag() || hf_model.asynch_flag() );
	evaluationCapacity = std::max( lf_model.evaluation_capacity(),
				       hf_model.evaluation_capacity() );
      }
      break;
    }
    }
  }
}


void HierarchSurrModel::
derived_free_communicators(ParLevLIter pl_iter, int max_eval_concurrency,
			   bool recurse_flag)
{
  if (recurse_flag) {

    size_t i, num_models = orderedModels.size();
    bool extra_deriv_config = true;//(responseMode == UNCORRECTED_SURROGATE ||
                                   // responseMode == BYPASS_SURROGATE ||
                                   // responseMode == AUTO_CORRECTED_SURROGATE);
    for (i=0; i<num_models; ++i) {
      Model& model_i = orderedModels[i];
      // superset of possible init calls (two configurations for i > 0)
      model_i.free_communicators(pl_iter, max_eval_concurrency);
      if (extra_deriv_config) // && i) // mid and high fidelity only?
	model_i.free_communicators(pl_iter, model_i.derivative_concurrency());
    }


    /* This version frees only two models:
    // superset of possible free calls (two configurations for HF)
    orderedModels[lowFidelityIndices.first].free_communicators(pl_iter,
      max_eval_concurrency);
    Model& hf_model = orderedModels[highFidelityIndices.first];
    hf_model.free_communicators(pl_iter, hf_model.derivative_concurrency());
    hf_model.free_communicators(pl_iter, max_eval_concurrency);
    */


    /* This version does not support runtime updating of responseMode:
    switch (responseMode) {
    case UNCORRECTED_SURROGATE:
      lf_model.free_communicators(pl_iter, max_eval_concurrency);
      break;
    case AUTO_CORRECTED_SURROGATE:
      lf_model.free_communicators(pl_iter, max_eval_concurrency);
      hf_model.free_communicators(pl_iter, hf_model.derivative_concurrency());
      break;
    case BYPASS_SURROGATE:
      hf_model.free_communicators(pl_iter, max_eval_concurrency);
      break;
    case MODEL_DISCREPANCY: case AGGREGATED_MODELS:
      lf_model.free_communicators(pl_iter, max_eval_concurrency);
      hf_model.free_communicators(pl_iter, max_eval_concurrency);
      break;
    }
    */
  }
}


void HierarchSurrModel::build_approximation()
{
  Cout << "\n>>>>> Building hierarchical approximation.\n";

  // perform the eval for the low fidelity model
  // NOTE: For SBO, the low fidelity eval is performed externally and its
  // response is passed into compute_correction.
  // -->> move LF model out and restructure if(!approxBuilds)
  //ActiveSet temp_set = lf_model.current_response().active_set();
  //temp_set.request_values(1);
  //if (sameModelInstance)
  //  lf_model.solution_level_index(lowFidelityIndices.second);
  //lf_model.evaluate(temp_set);
  //const Response& lo_fi_response = lf_model.current_response();

  Model& hf_model = orderedModels[highFidelityIndices.first];
  if (hierarchicalTagging) {
    String eval_tag = evalTagPrefix + '.' + 
      boost::lexical_cast<String>(hierModelEvalCntr+1);
    hf_model.eval_tag_prefix(eval_tag);
  }

  // set HierarchSurrModel parallelism mode to HF model
  component_parallel_mode(HF_MODEL);

  // update HF model with current variable values/bounds/labels
  update_model(hf_model);

  // store inactive variable values for use in determining whether an
  // automatic rebuild of an approximation is required
  // (reference{C,D}{L,U}Bnds are not needed in the hierarchical case)
  const Variables& hf_vars = hf_model.current_variables();
  copy_data(hf_vars.inactive_continuous_variables(),      referenceICVars);
  copy_data(hf_vars.inactive_discrete_int_variables(),    referenceIDIVars);
  referenceIDSVars = hf_vars.inactive_discrete_string_variables();
  copy_data(hf_vars.inactive_discrete_real_variables(),   referenceIDRVars);

  // compute the response for the high fidelity model
  ShortArray total_asv(numFns, deltaCorr.data_order()), hf_asv, lf_asv;
  asv_mapping(total_asv, hf_asv, lf_asv, true);
  ActiveSet hf_set = truthResponseRef.active_set(); // copy
  hf_set.request_vector(hf_asv);
  if (sameModelInstance)
    hf_model.solution_level_index(highFidelityIndices.second);
  hf_model.evaluate(hf_set);
  truthResponseRef.update(hf_model.current_response());

  // could compute the correction to LF model here, but rely on an
  // external call for consistency with DataFitSurr and to facilitate SBO logic.
  //deltaCorr.compute(..., truthResponseRef, lo_fi_response);

  Cout << "\n<<<<< Hierarchical approximation build completed.\n";
  approxBuilds++;
}


/*
bool HierarchSurrModel::
build_approximation(const RealVector& c_vars, const Response& response)
{
  // NOTE: this fn not currently used by SBO, but it could be.

  // Verify data content in incoming response
  const ShortArray& asrv = response.active_set_request_vector();
  bool data_complete = true; short corr_order = dataCorr.correction_order();
  for (size_t i=0; i<numFns; i++)
    if ( ( corr_order == 2 &&  (asrv[i] & 7) != 7 ) ||
	 ( corr_order == 1 &&  (asrv[i] & 3) != 3 ) ||
	 ( corr_order == 0 && !(asrv[i] & 1) ) )
      data_complete = false;
  if (data_complete) {
    Cout << "\n>>>>> Updating hierarchical approximation.\n";

    // are these updates necessary?
    Model& hf_model = orderedModels[highFidelityIndices.first];
    currentVariables.continuous_variables(c_vars);
    update_model(hf_model);
    const Variables& hf_vars = hf_model.current_variables();
    copy_data(hf_vars.inactive_continuous_variables(), referenceICVars);
    copy_data(hf_vars.inactive_discrete_variables(),   referenceIDVars);

    truthResponseRef.update(response);

    Cout << "\n<<<<< Hierarchical approximation update completed.\n";
  }
  else {
    Cerr << "Warning: cannot use anchor point in HierarchSurrModel::"
	 << "build_approximation(RealVector&, Response&).\n";
    currentVariables.continuous_variables(c_vars);
    build_approximation();
  }
  return false; // correction is not embedded and must be computed (by SBO)
}
*/


/** Compute the response synchronously using LF model, HF model, or
    both (mixed case).  For the LF model portion, compute the high
    fidelity response if needed with build_approximation(), and, if
    correction is active, correct the low fidelity results. */
void HierarchSurrModel::derived_evaluate(const ActiveSet& set)
{
  ++hierModelEvalCntr;

  // define LF/HF evaluation requirements
  ShortArray hi_fi_asv, lo_fi_asv; bool hi_fi_eval, lo_fi_eval, mixed_eval;
  Response lo_fi_response, hi_fi_response; // don't use truthResponseRef
  switch (responseMode) {
  case UNCORRECTED_SURROGATE: case AUTO_CORRECTED_SURROGATE:
    asv_mapping(set.request_vector(), hi_fi_asv, lo_fi_asv, false);
    hi_fi_eval = !hi_fi_asv.empty(); lo_fi_eval = !lo_fi_asv.empty();
    mixed_eval = (hi_fi_eval && lo_fi_eval);            break;
  case BYPASS_SURROGATE:
    hi_fi_eval = true; lo_fi_eval = mixed_eval = false; break;
  case MODEL_DISCREPANCY: case AGGREGATED_MODELS:
    hi_fi_eval = lo_fi_eval = mixed_eval = true;        break;
  }

  Model& lf_model = orderedModels[lowFidelityIndices.first];
  Model& hf_model = orderedModels[highFidelityIndices.first];
  if (hierarchicalTagging) {
    String eval_tag = evalTagPrefix + '.' + 
      boost::lexical_cast<String>(hierModelEvalCntr+1);
    if (sameModelInstance) lf_model.eval_tag_prefix(eval_tag);
    else {
      if (hi_fi_eval) hf_model.eval_tag_prefix(eval_tag);
      if (lo_fi_eval) lf_model.eval_tag_prefix(eval_tag);
    }
  }

  if (sameModelInstance) update_model(lf_model);

  // Notes on repetitive setting of model.solution_level_index():
  // > when LF & HF are the same model, then setting the index for low or high
  //   invalidates the other fidelity definition.
  // > within a single derived_evaluate(), could protect these updates with
  //   "if (sameModelInstance && mixed_eval)", but this does not guard against
  //   changes in eval requirements from the previous evaluation.  Detecting
  //   the current solution index state is currently as expensive as resetting
  //   it, so just reset each time.
  
  // ------------------------------
  // Compute high fidelity response
  // ------------------------------
  if (hi_fi_eval) {
    component_parallel_mode(HF_MODEL); // TO DO: sameModelInstance
    if (sameModelInstance)
      hf_model.solution_level_index(highFidelityIndices.second);
    else
      update_model(hf_model);      
    switch (responseMode) {
    case UNCORRECTED_SURROGATE: case AUTO_CORRECTED_SURROGATE: {
      ActiveSet hi_fi_set = set; hi_fi_set.request_vector(hi_fi_asv);
      hf_model.evaluate(hi_fi_set);
      if (mixed_eval)
	hi_fi_response = (sameModelInstance) ? // deep copy or shared rep
	  hf_model.current_response().copy() : hf_model.current_response();
      else {
	currentResponse.active_set(hi_fi_set);
	currentResponse.update(hf_model.current_response());
      }
      break;
    }
    case BYPASS_SURROGATE:
      hf_model.evaluate(set);
      currentResponse.active_set(set);
      currentResponse.update(hf_model.current_response());
      break;
    case MODEL_DISCREPANCY: case AGGREGATED_MODELS:
      hf_model.evaluate(set);
      hi_fi_response = (sameModelInstance) ? hf_model.current_response().copy()
	             : hf_model.current_response(); // shared rep
      break;
    }
  }

  // -----------------------------
  // Compute low fidelity response
  // -----------------------------
  if (lo_fi_eval) {
    // pre-process
    switch (responseMode) {
    case AUTO_CORRECTED_SURROGATE:
      // if build_approximation has not yet been called, call it now
      if (!approxBuilds || force_rebuild())
	build_approximation();
      break;
    }

    // compute the LF response
    component_parallel_mode(LF_MODEL); // TO DO: sameModelInstance
    if (sameModelInstance)
      lf_model.solution_level_index(lowFidelityIndices.second);
    else update_model(lf_model);
    ActiveSet lo_fi_set;
    switch (responseMode) {
    case UNCORRECTED_SURROGATE: case AUTO_CORRECTED_SURROGATE:
      lo_fi_set = set; lo_fi_set.request_vector(lo_fi_asv);
      lf_model.evaluate(lo_fi_set); break;
    case MODEL_DISCREPANCY: case AGGREGATED_MODELS:
      lf_model.evaluate(set);       break;
    }

    // post-process
    switch (responseMode) {
    case AUTO_CORRECTED_SURROGATE: {
      // LF resp should not be corrected directly (see derived_synchronize())
      lo_fi_response = lf_model.current_response().copy();
      bool quiet_flag = (outputLevel < NORMAL_OUTPUT);
      if (!deltaCorr.computed())
	deltaCorr.compute(currentVariables, truthResponseRef, lo_fi_response,
			  quiet_flag);
      deltaCorr.apply(currentVariables, lo_fi_response, quiet_flag);
      if (!mixed_eval) {
	currentResponse.active_set(lo_fi_set);
	currentResponse.update(lo_fi_response);
      }
      break;
    }
    case UNCORRECTED_SURROGATE:
      if (mixed_eval)
	lo_fi_response = lf_model.current_response(); // shared rep
      else {
	currentResponse.active_set(lo_fi_set);
	currentResponse.update(lf_model.current_response());
      }
      break;
    }
  }

  // ------------------------------
  // perform any LF/HF aggregations
  // ------------------------------
  switch (responseMode) {
  case MODEL_DISCREPANCY: {
    // don't update surrogate data within deltaCorr's Approximations; just
    // update currentResponse (managed as surrogate data at a higher level)
    bool quiet_flag = (outputLevel < NORMAL_OUTPUT);
    currentResponse.active_set(set);
    deltaCorr.compute(hi_fi_response, lf_model.current_response(),
		      currentResponse, quiet_flag);
    break;
  }
  case AGGREGATED_MODELS:
    aggregate_response(hi_fi_response, lf_model.current_response(),
		       currentResponse);
    break;
  case UNCORRECTED_SURROGATE: case AUTO_CORRECTED_SURROGATE:
    if (mixed_eval) {
      currentResponse.active_set(set);
      response_mapping(hi_fi_response, lo_fi_response, currentResponse);
    }
    break;
  }
}


/** Compute the response asynchronously using LF model, HF model, or
    both (mixed case).  For the LF model portion, compute the high
    fidelity response with build_approximation() (for correcting the
    low fidelity results in derived_synchronize() and
    derived_synchronize_nowait()) if not performed previously. */
void HierarchSurrModel::derived_evaluate_nowait(const ActiveSet& set)
{
  ++hierModelEvalCntr;

  Model& lf_model = orderedModels[lowFidelityIndices.first];
  Model& hf_model = orderedModels[highFidelityIndices.first];

  ShortArray hi_fi_asv, lo_fi_asv;
  bool hi_fi_eval, lo_fi_eval, asynch_lo_fi = lf_model.asynch_flag(),
    asynch_hi_fi = hf_model.asynch_flag();
  switch (responseMode) {
  case UNCORRECTED_SURROGATE: case AUTO_CORRECTED_SURROGATE:
    asv_mapping(set.request_vector(), hi_fi_asv, lo_fi_asv, false);
    hi_fi_eval = !hi_fi_asv.empty(); lo_fi_eval = !lo_fi_asv.empty(); break;
  case BYPASS_SURROGATE:
    hi_fi_eval = true; lo_fi_eval = false;                            break;
  case MODEL_DISCREPANCY: case AGGREGATED_MODELS:
    hi_fi_eval = lo_fi_eval = true;                                   break;
  }

  if (hierarchicalTagging) {
    String eval_tag = evalTagPrefix + '.' + 
      boost::lexical_cast<String>(hierModelEvalCntr+1);
    if (sameModelInstance) lf_model.eval_tag_prefix(eval_tag);
    else {
      if (hi_fi_eval) hf_model.eval_tag_prefix(eval_tag);
      if (lo_fi_eval) lf_model.eval_tag_prefix(eval_tag);
    }
  }

  if (sameModelInstance) update_model(lf_model);
  
  // perform Model updates and define active sets for LF and HF evaluations
  ActiveSet hi_fi_set, lo_fi_set;
  if (hi_fi_eval) {
    // update HF model
    if (!sameModelInstance) update_model(hf_model);
    // update hi_fi_set
    hi_fi_set.derivative_vector(set.derivative_vector());
    switch (responseMode) {
    case UNCORRECTED_SURROGATE: case AUTO_CORRECTED_SURROGATE:
      hi_fi_set.request_vector(hi_fi_asv);            break;
    case BYPASS_SURROGATE: case MODEL_DISCREPANCY: case AGGREGATED_MODELS:
      hi_fi_set.request_vector(set.request_vector()); break;
    }
  }
  if (lo_fi_eval) {
    // if build_approximation has not yet been called, call it now
    if ( responseMode == AUTO_CORRECTED_SURROGATE &&
	 ( !approxBuilds || force_rebuild() ) )
      build_approximation();
    // update LF model
    if (!sameModelInstance) update_model(lf_model);
    // update lo_fi_set
    lo_fi_set.derivative_vector(set.derivative_vector());
    switch (responseMode) {
    case UNCORRECTED_SURROGATE: case AUTO_CORRECTED_SURROGATE:
      lo_fi_set.request_vector(lo_fi_asv);            break;
    case MODEL_DISCREPANCY: case AGGREGATED_MODELS:
      lo_fi_set.request_vector(set.request_vector()); break;
    }
  }

  // HierarchSurrModel's asynchEvalFlag is set if _either_ LF or HF is
  // asynchronous, resulting in use of derived_evaluate_nowait().
  // To manage general case of mixed asynch, launch nonblocking evals first,
  // followed by blocking evals.

  // For notes on repetitive setting of model.solution_level_index(), see
  // derived_evaluate() above.
  
  // launch nonblocking evals before any blocking ones
  if (hi_fi_eval && asynch_hi_fi) { // HF model may be executed asynchronously
    // don't need to set component parallel mode since only queues the job
    if (sameModelInstance)
      hf_model.solution_level_index(highFidelityIndices.second);
    hf_model.evaluate_nowait(hi_fi_set);
    // store map from HF eval id to HierarchSurrModel id
    truthIdMap[hf_model.evaluation_id()] = hierModelEvalCntr;
  }
  if (lo_fi_eval && asynch_lo_fi) { // LF model may be executed asynchronously
    // don't need to set component parallel mode since only queues the job
    if (sameModelInstance)
      lf_model.solution_level_index(lowFidelityIndices.second);
    lf_model.evaluate_nowait(lo_fi_set);
    // store map from LF eval id to HierarchSurrModel id
    surrIdMap[lf_model.evaluation_id()] = hierModelEvalCntr;
    // store variables set needed for correction
    if (responseMode == AUTO_CORRECTED_SURROGATE)
      rawVarsMap[hierModelEvalCntr] = currentVariables.copy();
  }

  // now launch any blocking evals
  if (hi_fi_eval && !asynch_hi_fi) { // execute HF synchronously & cache resp
    component_parallel_mode(HF_MODEL);
    if (sameModelInstance)
      hf_model.solution_level_index(highFidelityIndices.second);
    hf_model.evaluate(hi_fi_set);
    cachedTruthRespMap[hierModelEvalCntr/*hf_model.evaluation_id()*/]
      = hf_model.current_response().copy();
  }
  if (lo_fi_eval && !asynch_lo_fi) { // execute LF synchronously & cache resp
    component_parallel_mode(LF_MODEL);
    if (sameModelInstance)
      lf_model.solution_level_index(lowFidelityIndices.second);
    lf_model.evaluate(lo_fi_set);
    Response lo_fi_response(lf_model.current_response().copy());
    // correct LF response prior to caching
    if (responseMode == AUTO_CORRECTED_SURROGATE) {
      bool quiet_flag = (outputLevel < NORMAL_OUTPUT);
      if (!deltaCorr.computed())
	deltaCorr.compute(currentVariables, truthResponseRef, lo_fi_response,
			  quiet_flag);
      // correct synch cases now (asynch cases get corrected in
      // derived_synchronize_aggregate*)
      deltaCorr.apply(currentVariables, lo_fi_response, quiet_flag);
    }
    // cache corrected LF response for retrieval during synchronization
    cachedApproxRespMap[hierModelEvalCntr/*lf_model.evaluation_id()*/]
      = lo_fi_response; // already deep copied
  }
}


/** Blocking retrieval of asynchronous evaluations from LF model, HF
    model, or both (mixed case).  For the LF model portion, apply
    correction (if active) to each response in the array.
    derived_synchronize() is designed for the general case where
    derived_evaluate_nowait() may be inconsistent in its use of low
    fidelity evaluations, high fidelity evaluations, or both. */
const IntResponseMap& HierarchSurrModel::derived_synchronize()
{
  surrResponseMap.clear();

  if (sameModelInstance)
    return derived_synchronize_same_model();
  //else if (sameInterfaceInstance) // eliminate need w/ new completion logic
  //  return derived_synchronize_same_interface();
  else if (sameInterfaceInstance || truthIdMap.empty() || surrIdMap.empty())
    return derived_synchronize_distinct_model(); // 1 queue: blocking synch
  else
    return derived_synchronize_competing();//competing queues: nonblocking synch
}


const IntResponseMap& HierarchSurrModel::derived_synchronize_same_model()
{
  // retrieve LF and HF evals aggregated into a single IntResponseMap
  component_parallel_mode(LF_MODEL);
  const IntResponseMap& combined_resp_map
    = orderedModels[lowFidelityIndices.first].synchronize();

  // Separate & rekey LF/HF resp maps for input to derived_synchronize_combine()
  // don't loop over combined_resp_map twice; loop once over each IdMap
  IntResponseMap hf_resp_map, lf_resp_map;
  rekey_response_map_by_id(truthIdMap, combined_resp_map, hf_resp_map);
  bool deep_cp = (responseMode == AUTO_CORRECTED_SURROGATE);
  rekey_response_map_by_id(surrIdMap, combined_resp_map, lf_resp_map, deep_cp);
  
  // cached response maps keyed with hierModelEvalCntr in evaluate_nowait()
  hf_resp_map.insert(cachedTruthRespMap.begin(),  cachedTruthRespMap.end());
  lf_resp_map.insert(cachedApproxRespMap.begin(), cachedApproxRespMap.end());
  cachedTruthRespMap.clear(); cachedApproxRespMap.clear();

  derived_synchronize_combine(hf_resp_map, lf_resp_map, surrResponseMap);
  return surrResponseMap;
}


/*
const IntResponseMap& HierarchSurrModel::derived_synchronize_same_interface()
{
  // Completes all jobs at the shared Interface level, but returns only the
  // subset matched by the LF model bookkeeping
  component_parallel_mode(LF_MODEL);
  const IntResponseMap& lf_resp_map
    = orderedModels[lowFidelityIndices.first].synchronize();
  // Don't re-synchronize shared interface; only rekey existing completions
  //component_parallel_mode(HF_MODEL);
  const IntResponseMap& hf_resp_map
    = orderedModels[highFidelityIndices.first].synchronize()//_rekey();

  // Rekey LF/HF resp maps for input to derived_synchronize_combine()
  IntResponseMap hf_resp_map_rekey, lf_resp_map_rekey;
  rekey_response_map(hf_resp_map, hf_resp_map_rekey, truthIdMap);
  // Interface::rawResponseMap should _not_ be corrected directly since
  // rawResponseMap, beforeSynchCorePRPQueue, and data_pairs all share a
  // responseRep -> modifying rawResponseMap affects data_pairs.
  bool deep_copy = (responseMode == AUTO_CORRECTED_SURROGATE);
  rekey_response_map(lf_resp_map, lf_resp_map_rekey, surrIdMap, deep_copy);

  // cached response maps keyed with hierModelEvalCntr in evaluate_nowait()
  hf_resp_map_rekey.insert(cachedTruthRespMap.begin(),cachedTruthRespMap.end());
  lf_resp_map_rekey.insert(cachedApproxRespMap.begin(),
			   cachedApproxRespMap.end());
  cachedTruthRespMap.clear(); cachedApproxRespMap.clear();

  derived_synchronize_combine(hf_resp_map_rekey, lf_resp_map_rekey,
			      surrResponseMap);
  return surrResponseMap;
}
*/


const IntResponseMap& HierarchSurrModel::derived_synchronize_competing()
{
  // in this case, we don't want to starve either LF or HF scheduling by
  // blocking on one or the other --> leverage derived_synchronize_nowait()
  IntResponseMap aggregated_map; // accumulate surrResponseMap returns
  while (!truthIdMap.empty() || !surrIdMap.empty()) {
    // partial_map is a reference to surrResponseMap, returned by _nowait()
    const IntResponseMap& partial_map = derived_synchronize_nowait();
    if (!partial_map.empty())
      aggregated_map.insert(partial_map.begin(), partial_map.end());
  }

  // Note: cached response maps and any LF/HF aggregations are managed
  // within derived_synchronize_nowait()

  surrResponseMap = aggregated_map; // now replace surrResponseMap for return
  return surrResponseMap;
}


const IntResponseMap& HierarchSurrModel::derived_synchronize_distinct_model()
{
  // --------------------------
  // synchronize HF model evals
  // --------------------------
  IntResponseMap hf_resp_map_rekey, lf_resp_map_rekey; IntRespMCIter r_cit;
  if (!truthIdMap.empty()) { // synchronize HF evals
    component_parallel_mode(HF_MODEL);
    rekey_response_map(orderedModels[highFidelityIndices.first].synchronize(),
		       hf_resp_map_rekey, truthIdMap);
  }
  // add cached truth evals from:
  // (a) recovered HF asynch evals that could not be returned since LF
  //     eval portions were still pending, or
  // (b) synchronous HF evals performed within evaluate_nowait()
  hf_resp_map_rekey.insert(cachedTruthRespMap.begin(),
			   cachedTruthRespMap.end());
  cachedTruthRespMap.clear();

  // --------------------------
  // synchronize LF model evals
  // --------------------------
  if (!surrIdMap.empty()) { // synchronize LF evals
    component_parallel_mode(LF_MODEL);
    // Interface::rawResponseMap should _not_ be corrected directly since
    // rawResponseMap, beforeSynchCorePRPQueue, and data_pairs all share a
    // responseRep -> modifying rawResponseMap affects data_pairs.
    bool deep_copy = (responseMode == AUTO_CORRECTED_SURROGATE);
    rekey_response_map(orderedModels[lowFidelityIndices.first].synchronize(),
		       lf_resp_map_rekey, surrIdMap, deep_copy);
  }
  // add cached approx evals from:
  // (a) recovered LF asynch evals that could not be returned since HF
  //     eval portions were still pending, or
  // (b) synchronous LF evals performed within evaluate_nowait()
  lf_resp_map_rekey.insert(cachedApproxRespMap.begin(),
			   cachedApproxRespMap.end());
  cachedApproxRespMap.clear();

  // aggregate the LF/HF inputs into surrResponseMap
  derived_synchronize_combine(hf_resp_map_rekey, lf_resp_map_rekey,
			      surrResponseMap);
  return surrResponseMap;
}


void HierarchSurrModel::
derived_synchronize_combine(const IntResponseMap& hf_resp_map,
			    IntResponseMap& lf_resp_map,
			    IntResponseMap& combined_resp_map)
{
  // ------------------------------
  // perform any LF/HF aggregations
  // ------------------------------
  // {hf,lf}_resp_map may be partial sets (partial surrogateFnIndices
  // in {UN,AUTO_}CORRECTED_SURROGATE) or full sets (MODEL_DISCREPANCY,
  // AGGREGATED_MODELS).

  IntRespMCIter hf_cit = hf_resp_map.begin(), lf_cit = lf_resp_map.begin();
  bool quiet_flag = (outputLevel < NORMAL_OUTPUT);
  switch (responseMode) {
  case MODEL_DISCREPANCY:
    for (; hf_cit != hf_resp_map.end() && 
	   lf_cit != lf_resp_map.end(); ++hf_cit, ++lf_cit) {
      check_key(hf_cit->first, lf_cit->first);
      deltaCorr.compute(hf_cit->second, lf_cit->second,
			combined_resp_map[hf_cit->first], quiet_flag);
    }
    break;
  case AGGREGATED_MODELS:
    for (; hf_cit != hf_resp_map.end() && 
	   lf_cit != lf_resp_map.end(); ++hf_cit, ++lf_cit) {
      check_key(hf_cit->first, lf_cit->first);
      aggregate_response(hf_cit->second, lf_cit->second,
			 combined_resp_map[hf_cit->first]);
    }
    break;
  default: { // {UNCORRECTED,AUTO_CORRECTED,BYPASS}_SURROGATE modes
    if (lf_resp_map.empty()) { combined_resp_map = hf_resp_map; return; }
    if (responseMode == AUTO_CORRECTED_SURROGATE)
      compute_apply_delta(lf_resp_map);
    if (hf_resp_map.empty()) { combined_resp_map = lf_resp_map; return; }
    // process combinations of HF and LF completions
    Response empty_resp;
    while (hf_cit != hf_resp_map.end() || lf_cit != lf_resp_map.end()) {
      int hf_eval_id = (hf_cit == hf_resp_map.end()) ?
	INT_MAX : hf_cit->first;
      int lf_eval_id = (lf_cit == lf_resp_map.end()) ?
	INT_MAX : lf_cit->first;

      if (hf_eval_id < lf_eval_id) // only HF available
	{ response_mapping(hf_cit->second, empty_resp,
			   combined_resp_map[hf_eval_id]); ++hf_cit; }
      else if (lf_eval_id < hf_eval_id) // only LF available
	{ response_mapping(empty_resp, lf_cit->second,
			   combined_resp_map[lf_eval_id]); ++lf_cit; }
      else { // both LF and HF available
	response_mapping(hf_cit->second, lf_cit->second,
			 combined_resp_map[hf_eval_id]);
	++hf_cit; ++lf_cit;
      }
    }
    break;
  }
  }
}


/** Nonblocking retrieval of asynchronous evaluations from LF model,
    HF model, or both (mixed case).  For the LF model portion, apply
    correction (if active) to each response in the map.
    derived_synchronize_nowait() is designed for the general case
    where derived_evaluate_nowait() may be inconsistent in its use of
    actual evals, approx evals, or both. */
const IntResponseMap& HierarchSurrModel::derived_synchronize_nowait()
{
  surrResponseMap.clear();

  if (sameModelInstance)
    return derived_synchronize_same_model_nowait();
  //else if (sameInterfaceInstance)
  //  return derived_synchronize_same_interface_nowait();
  else
    return derived_synchronize_distinct_model_nowait();
}

  
const IntResponseMap& HierarchSurrModel::derived_synchronize_same_model_nowait()
{
  // retrieve LF and HF evals aggregated into a single IntResponseMap
  component_parallel_mode(LF_MODEL);
  const IntResponseMap& combined_resp_map
    = orderedModels[lowFidelityIndices.first].synchronize_nowait();

  // Separate & rekey LF/HF resp maps for input to
  // derived_synchronize_combine_nowait().  Don't loop over
  // combined_resp_map twice; loop once over each IdMap.
  IntResponseMap hf_resp_map, lf_resp_map;
  rekey_response_map_by_id(truthIdMap, combined_resp_map, hf_resp_map);
  bool deep_cp = (responseMode == AUTO_CORRECTED_SURROGATE);
  rekey_response_map_by_id(surrIdMap, combined_resp_map, lf_resp_map, deep_cp);

  // add any cached results (keyed with hierModelEvalCntr in evaluate_nowait())
  hf_resp_map.insert(cachedTruthRespMap.begin(),  cachedTruthRespMap.end());
  lf_resp_map.insert(cachedApproxRespMap.begin(), cachedApproxRespMap.end());
  cachedTruthRespMap.clear(); cachedApproxRespMap.clear();

  // aggregate lo and hi as needed
  derived_synchronize_combine_nowait(hf_resp_map, lf_resp_map, surrResponseMap);
  return surrResponseMap;
}


/*
const IntResponseMap& HierarchSurrModel::
derived_synchronize_same_interface_nowait()
{
  // retrieve LF and HF evals aggregated into a single IntResponseMap
  component_parallel_mode(LF_MODEL);
  const IntResponseMap& lf_resp_map
    = orderedModels[lowFidelityIndices.first].synchronize_nowait();
  const IntResponseMap& hf_resp_map
    = orderedModels[highFidelityIndices.first].synchronize_nowait()//_rekey();
    // don't synch interface/sub-model; only rekey existing completions

  // extract and rekey the LF and HF results
  // Rekey LF/HF resp maps for input to derived_synchronize_combine()
  IntResponseMap hf_resp_map_rekey, lf_resp_map_rekey;
  rekey_response_map(hf_resp_map, hf_resp_map_rekey, truthIdMap);
  bool deep_copy = (responseMode == AUTO_CORRECTED_SURROGATE);
  rekey_response_map(lf_resp_map, lf_resp_map_rekey, surrIdMap, deep_copy);

  // add any cached results (keyed with hierModelEvalCntr in evaluate_nowait())
  hf_resp_map_rekey.insert(cachedTruthRespMap.begin(),cachedTruthRespMap.end());
  lf_resp_map_rekey.insert(cachedApproxRespMap.begin(),
                           cachedApproxRespMap.end());
  cachedTruthRespMap.clear(); cachedApproxRespMap.clear();

  // aggregate lo and hi as needed
  derived_synchronize_combine_nowait(hf_resp_map_rekey, lf_resp_map_rekey,
                                     surrResponseMap);
  return surrResponseMap;
}
*/


const IntResponseMap& HierarchSurrModel::
derived_synchronize_distinct_model_nowait()
{
  // --------------------------
  // synchronize HF model evals
  // --------------------------
  IntResponseMap hf_resp_map_rekey; IntRespMCIter r_cit;
  if (!truthIdMap.empty()) { // synchronize HF evals
    component_parallel_mode(HF_MODEL);
    const IntResponseMap& hf_resp_map
      = orderedModels[highFidelityIndices.first].synchronize_nowait();
    // update map keys to use hierModelEvalCntr
    rekey_response_map(hf_resp_map, hf_resp_map_rekey, truthIdMap);
  }
  // add cached truth evals for processing, where evals are cached from:
  // (a) recovered HF asynch evals that could not be returned since LF
  //     eval portions were still pending, or
  // (b) synchronous HF evals performed within evaluate_nowait()
  hf_resp_map_rekey.insert(cachedTruthRespMap.begin(),
			   cachedTruthRespMap.end());
  cachedTruthRespMap.clear();

  // --------------------------
  // synchronize LF model evals
  // --------------------------
  IntResponseMap lf_resp_map_rekey;
  if (!surrIdMap.empty()) { // synchronize LF evals
    component_parallel_mode(LF_MODEL);
    const IntResponseMap& lf_resp_map
      = orderedModels[lowFidelityIndices.first].synchronize_nowait();
    // update map keys to use hierModelEvalCntr
    bool deep_copy = (responseMode == AUTO_CORRECTED_SURROGATE);
    rekey_response_map(lf_resp_map, lf_resp_map_rekey, surrIdMap, deep_copy);
  }
  // add cached approx evals for processing, where evals are cached from:
  // (a) recovered LF asynch evals that could not be returned since HF
  //     eval portions were still pending, or
  // (b) synchronous LF evals performed within evaluate_nowait()
  lf_resp_map_rekey.insert(cachedApproxRespMap.begin(),
			   cachedApproxRespMap.end());
  cachedApproxRespMap.clear();

  // if not returned previously, then we need to aggregate lo and hi
  derived_synchronize_combine_nowait(hf_resp_map_rekey, lf_resp_map_rekey,
				     surrResponseMap);
  return surrResponseMap;
}

  
void HierarchSurrModel::
derived_synchronize_combine_nowait(const IntResponseMap& hf_resp_map,
				   IntResponseMap& lf_resp_map,
				   IntResponseMap& combined_resp_map)
{
  // ------------------------------
  // perform any LF/HF aggregations
  // ------------------------------
  // {hf,lf}_resp_map may be partial sets (partial surrogateFnIndices
  // in {UN,AUTO_}CORRECTED_SURROGATE) or full sets (MODEL_DISCREPANCY).

  // Early return options avoid some overhead:
  if (surrIdMap.empty())  { combined_resp_map = hf_resp_map; return; }
  if (responseMode == AUTO_CORRECTED_SURROGATE)
    compute_apply_delta(lf_resp_map);
  if (truthIdMap.empty()) { combined_resp_map = lf_resp_map; return; }

  // invert truthIdMap and surrIdMap
  IntIntMap inverse_truth_id_map, inverse_surr_id_map; IntIntMCIter id_it;
  for (id_it=truthIdMap.begin(); id_it!=truthIdMap.end(); ++id_it)
    inverse_truth_id_map[id_it->second] = id_it->first;
  for (id_it=surrIdMap.begin();  id_it!=surrIdMap.end();  ++id_it)
    inverse_surr_id_map[id_it->second]  = id_it->first;
  
  // process any combination of HF and LF completions
  IntRespMCIter hf_cit = hf_resp_map.begin();
  IntRespMIter  lf_it  = lf_resp_map.begin();
  Response empty_resp;
  bool quiet_flag = (outputLevel < NORMAL_OUTPUT);
  while (hf_cit != hf_resp_map.end() || lf_it != lf_resp_map.end()) {
    int hf_eval_id = (hf_cit == hf_resp_map.end()) ? INT_MAX : hf_cit->first;
    int lf_eval_id = (lf_it  == lf_resp_map.end()) ? INT_MAX : lf_it->first;
    // process LF/HF results or cache them for next pass
    if (hf_eval_id < lf_eval_id) { // only HF available
      switch (responseMode) {
      case MODEL_DISCREPANCY: case AGGREGATED_MODELS:
	// cache HF response since LF contribution not yet available
	cachedTruthRespMap[hf_eval_id] = hf_cit->second; break;
      default: // {UNCORRECTED,AUTO_CORRECTED,BYPASS}_SURROGATE modes
	if (inverse_surr_id_map.find(hf_eval_id) != inverse_surr_id_map.end())
	  // LF contribution is pending -> cache HF response
	  cachedTruthRespMap[hf_eval_id] = hf_cit->second;
	else { // no LF component is pending -> HF contribution is sufficient
	  response_mapping(hf_cit->second, empty_resp,
			   surrResponseMap[hf_eval_id]);
	  truthIdMap.erase(inverse_truth_id_map[hf_eval_id]);
	}
	break;
      }
      ++hf_cit;
    }
    else if (lf_eval_id < hf_eval_id) { // only LF available
      switch (responseMode) {
      case MODEL_DISCREPANCY: case AGGREGATED_MODELS:
	// cache LF response since HF contribution is pending is sufficient
	cachedApproxRespMap[lf_eval_id] = lf_it->second; break;
      default: // {UNCORRECTED,AUTO_CORRECTED,BYPASS}_SURROGATE modes
	if (inverse_truth_id_map.find(lf_eval_id) != inverse_truth_id_map.end())
          // HF contribution is pending -> cache LF response
	  cachedApproxRespMap[lf_eval_id] = lf_it->second;
	else { // no HF component is pending -> LF contribution is sufficient
	  response_mapping(empty_resp, lf_it->second,
			   surrResponseMap[lf_eval_id]);
	  surrIdMap.erase(inverse_surr_id_map[lf_eval_id]);
	}
	break;
      }
      ++lf_it;
    }
    else { // both LF and HF available
      bool cache_for_pending_corr = false;
      switch (responseMode) {
      case MODEL_DISCREPANCY:
	deltaCorr.compute(hf_cit->second, lf_it->second,
			  surrResponseMap[hf_eval_id], quiet_flag); break;
      case AGGREGATED_MODELS:
	aggregate_response(hf_cit->second, lf_it->second,
			   surrResponseMap[hf_eval_id]);            break;
      default: // {UNCORRECTED,AUTO_CORRECTED,BYPASS}_SURROGATE modes
	response_mapping(hf_cit->second, lf_it->second,
			 surrResponseMap[hf_eval_id]);              break;
      }
      truthIdMap.erase(inverse_truth_id_map[hf_eval_id]);
      surrIdMap.erase(inverse_surr_id_map[lf_eval_id]);
      ++hf_cit; ++lf_it;
    }
  }
}


void HierarchSurrModel::compute_apply_delta(IntResponseMap& lf_resp_map)
{
  // Incoming we have a completed LF evaluation that may be used to compute a
  // correction and may be the target of application of a correction.

  // First, test if a correction is previously available or can now be computed
  bool corr_comp = deltaCorr.computed(), cache_for_pending_corr = false,
    quiet_flag = (outputLevel < NORMAL_OUTPUT);
  if (!corr_comp) {
    // compute a correction corresponding to the first entry in rawVarsMap
    IntVarsMCIter v_corr_cit = rawVarsMap.begin();
    if (v_corr_cit != rawVarsMap.end()) {
      // if corresponding LF response is complete, compute the delta
      IntRespMCIter lf_corr_cit = lf_resp_map.find(v_corr_cit->first);
      if (lf_corr_cit != lf_resp_map.end()) {
	deltaCorr.compute(v_corr_cit->second, truthResponseRef,
			  lf_corr_cit->second, quiet_flag);
	corr_comp = true;
      }
    }
  }

  // Next, apply the correction.  We cache an uncorrected eval when the
  // components necessary for correction are still pending (returning
  // corrected evals with the first available LF response would lead to
  // nondeterministic results).
  IntVarsMIter v_it; IntRespMIter lf_it; int lf_eval_id;
  for (lf_it=lf_resp_map.begin(); lf_it!=lf_resp_map.end(); ++lf_it) {
    lf_eval_id = lf_it->first; v_it = rawVarsMap.find(lf_eval_id);
    if (v_it != rawVarsMap.end()) {
      if (corr_comp) { // apply the correction to the LF response
	deltaCorr.apply(v_it->second, lf_it->second, quiet_flag);
	rawVarsMap.erase(v_it);
      }
      else // no new corrections can be applied -> cache uncorrected
	cachedApproxRespMap.insert(*lf_it);
    }
    // else correction already applied
  }
  // remove cached responses from lf_resp_map
  if (!corr_comp)
    for (lf_it =cachedApproxRespMap.begin();
	 lf_it!=cachedApproxRespMap.end(); ++lf_it)
      lf_resp_map.erase(lf_it->first);
}


void HierarchSurrModel::resize_response()
{
  size_t num_curr_fns;
  switch (responseMode) {
  case AGGREGATED_MODELS:
    num_curr_fns = surrogate_model().num_functions()
                 +     truth_model().num_functions();                     break;
  case BYPASS_SURROGATE:
    num_curr_fns = truth_model().num_functions();                         break;
  //case MODEL_DISCREPANCY:
  //  num_curr_fns = std::max(surrogate_model().num_functions(),
  //                          truth_model().num_functions());             break;
  default:
    num_curr_fns = surrogate_model().num_functions();                     break;
  }
  
  // gradient and Hessian settings are based on independent spec (not LF, HF)
  // --> preserve previous settings
  if (currentResponse.num_functions() != num_curr_fns) {
    currentResponse.reshape(num_curr_fns, currentVariables.cv(),
			    !currentResponse.function_gradients().empty(),
			    !currentResponse.function_hessians().empty());
    // update message lengths for send/receive of parallel jobs (normally
    // performed once in Model::init_communicators() just after construct time)
    estimate_message_lengths();
  }
}

  
void HierarchSurrModel::component_parallel_mode(short mode)
{
  // mode may be correct, but can't guarantee active parallel config is in sync
  //if (componentParallelMode == mode)
  //  return; // already in correct parallel mode

  // terminate previous serve mode (if active)
  size_t index; int iter_comm_size;
  if (componentParallelMode != mode) {
    if (componentParallelMode == LF_MODEL) { // old mode
      Model& lf_model = orderedModels[lowFidelityIndices.first];
      ParConfigLIter pc_it = lf_model.parallel_configuration_iterator();
      size_t index = lf_model.mi_parallel_level_index();
      if (pc_it->mi_parallel_level_defined(index) && 
	  pc_it->mi_parallel_level(index).server_communicator_size() > 1)
	lf_model.stop_servers();
    }
    else if (componentParallelMode == HF_MODEL) { // old mode
      Model& hf_model = orderedModels[highFidelityIndices.first];
      ParConfigLIter pc_it = hf_model.parallel_configuration_iterator();
      size_t index = hf_model.mi_parallel_level_index();
      if (pc_it->mi_parallel_level_defined(index) && 
	  pc_it->mi_parallel_level(index).server_communicator_size() > 1)
	hf_model.stop_servers();
    }
  }

  // set ParallelConfiguration for new mode and retrieve new data
  if (mode == HF_MODEL) { // new mode
    // activation delegated to HF model
  }
  else if (mode == LF_MODEL) { // new mode
    // activation delegated to LF model
  }

  // activate new serve mode (matches HierarchSurrModel::serve_run(pl_iter)).
  // These bcasts match the outer parallel context (pl_iter).
  if (componentParallelMode != mode &&
      modelPCIter->mi_parallel_level_defined(miPLIndex)) {
    const ParallelLevel& mi_pl = modelPCIter->mi_parallel_level(miPLIndex);
    if (mi_pl.server_communicator_size() > 1) {
      parallelLib.bcast(mode, mi_pl);
      if (mode == HF_MODEL)
	parallelLib.bcast(responseMode, mi_pl);
    }
  }

  componentParallelMode = mode;
}


void HierarchSurrModel::serve_run(ParLevLIter pl_iter, int max_eval_concurrency)
{
  set_communicators(pl_iter, max_eval_concurrency, false); // don't recurse

  // manage LF model and HF model servers, matching communication from
  // HierarchSurrModel::component_parallel_mode()
  componentParallelMode = 1;
  while (componentParallelMode) {
    parallelLib.bcast(componentParallelMode, *pl_iter); // outer context
    if (componentParallelMode == LF_MODEL) {
      orderedModels[lowFidelityIndices.first].serve_run(pl_iter,
	max_eval_concurrency);
      // Note: ignores erroneous BYPASS_SURROGATE to avoid responseMode bcast
    }
    else if (componentParallelMode == HF_MODEL) {
      // receive responseMode from HierarchSurrModel::component_parallel_mode()
      parallelLib.bcast(responseMode, *pl_iter);
      // employ correct iterator concurrency for HF model
      switch (responseMode) {
      case UNCORRECTED_SURROGATE:
	Cerr << "Error: setting parallel mode to HF_MODEL is erroneous for a "
	     << "response mode of UNCORRECTED_SURROGATE." << std::endl;
	abort_handler(-1);
	break;
      case AUTO_CORRECTED_SURROGATE: {
	Model& hf_model = orderedModels[highFidelityIndices.first];
	hf_model.serve_run(pl_iter, hf_model.derivative_concurrency());
	break;
      }
      case BYPASS_SURROGATE: case MODEL_DISCREPANCY: case AGGREGATED_MODELS:
	orderedModels[highFidelityIndices.first].serve_run(pl_iter,
	  max_eval_concurrency);
	break;
      }
    }
  }
}


void HierarchSurrModel::update_model(Model& model)
{
  // update model with currentVariables/userDefinedConstraints data.  In the
  // hierarchical case, the variables view in LF/HF models correspond to the
  // currentVariables view.  Note: updating the bounds is not strictly necessary
  // in common usage for the HF model (when a single model evaluated only at the
  // TR center), but is needed for the LF model and could be relevant in cases
  // where the HF model involves additional surrogates/nestings.

  // vars
  model.continuous_variables(currentVariables.continuous_variables());
  model.discrete_int_variables(currentVariables.discrete_int_variables());
  model.discrete_string_variables(currentVariables.discrete_string_variables());
  model.discrete_real_variables(currentVariables.discrete_real_variables());
  // bound constraints
  model.continuous_lower_bounds(
    userDefinedConstraints.continuous_lower_bounds());
  model.continuous_upper_bounds(
    userDefinedConstraints.continuous_upper_bounds());
  model.discrete_int_lower_bounds(
    userDefinedConstraints.discrete_int_lower_bounds());
  model.discrete_int_upper_bounds(
    userDefinedConstraints.discrete_int_upper_bounds());
  model.discrete_real_lower_bounds(
    userDefinedConstraints.discrete_real_lower_bounds());
  model.discrete_real_upper_bounds(
    userDefinedConstraints.discrete_real_upper_bounds());
  // linear constraints
  if (userDefinedConstraints.num_linear_ineq_constraints()) {
    model.linear_ineq_constraint_coeffs(
      userDefinedConstraints.linear_ineq_constraint_coeffs());
    model.linear_ineq_constraint_lower_bounds(
      userDefinedConstraints.linear_ineq_constraint_lower_bounds());
    model.linear_ineq_constraint_upper_bounds(
      userDefinedConstraints.linear_ineq_constraint_upper_bounds());
  }
  if (userDefinedConstraints.num_linear_eq_constraints()) {
    model.linear_eq_constraint_coeffs(
      userDefinedConstraints.linear_eq_constraint_coeffs());
    model.linear_eq_constraint_targets(
      userDefinedConstraints.linear_eq_constraint_targets());
  }
  // nonlinear constraints
  if (userDefinedConstraints.num_nonlinear_ineq_constraints()) {
    model.nonlinear_ineq_constraint_lower_bounds(
      userDefinedConstraints.nonlinear_ineq_constraint_lower_bounds());
    model.nonlinear_ineq_constraint_upper_bounds(
      userDefinedConstraints.nonlinear_ineq_constraint_upper_bounds());
  }
  if (userDefinedConstraints.num_nonlinear_eq_constraints())
    model.nonlinear_eq_constraint_targets(
      userDefinedConstraints.nonlinear_eq_constraint_targets());

  // Set the low/high fidelity model variable descriptors with the variable
  // descriptors from currentVariables (eliminates the need to replicate
  // variable descriptors in the input file).  This only needs to be performed
  // once (as opposed to the other updates above).  However, performing this set
  // in the constructor does not propagate properly for multiple surrogates/
  // nestings since the sub-model construction (and therefore any sub-sub-model
  // constructions) must finish before calling any set functions on it.  That
  // is, after-the-fact updating in constructors only propagates one level,
  // whereas before-the-fact updating in compute/build functions propagates
  // multiple levels.
  if (!approxBuilds) {
    // active not currently necessary, but included for completeness and 
    // consistency with global approximation case
    model.continuous_variable_labels(
      currentVariables.continuous_variable_labels());
    model.discrete_int_variable_labels(
      currentVariables.discrete_int_variable_labels());
    model.discrete_string_variable_labels(
      currentVariables.discrete_string_variable_labels());
    model.discrete_real_variable_labels(
      currentVariables.discrete_real_variable_labels());
    short active_view = currentVariables.view().first;
    if (active_view != RELAXED_ALL && active_view != MIXED_ALL) {
      // inactive needed for Nested/Surrogate propagation
      model.inactive_continuous_variable_labels(
        currentVariables.inactive_continuous_variable_labels());
      model.inactive_discrete_int_variable_labels(
        currentVariables.inactive_discrete_int_variable_labels());
      model.inactive_discrete_string_variable_labels(
        currentVariables.inactive_discrete_string_variable_labels());
      model.inactive_discrete_real_variable_labels(
        currentVariables.inactive_discrete_real_variable_labels());
    }
  }
}

} // namespace Dakota
