/*  _______________________________________________________________________

    DAKOTA: Design Analysis Kit for Optimization and Terascale Applications
    Copyright (c) 2010, Sandia National Laboratories.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Dakota directory.
    _______________________________________________________________________ */

//- Class:       SingleModel
//- Description: A simple model mapping variables into responses using a
//-              single interface.
//- Owner:       Mike Eldred
//- Checked by:
//- Version: $Id: SingleModel.hpp 6492 2009-12-19 00:04:28Z briadam $

#ifndef SINGLE_MODEL_H
#define SINGLE_MODEL_H

#include "DakotaModel.hpp"
#include "DakotaInterface.hpp"
#include "ParallelLibrary.hpp"


namespace Dakota {

/// Derived model class which utilizes a single interface to map
/// variables into responses.

/** The SingleModel class is the simplest of the derived model
    classes.  It provides the capabilities of the original Model class,
    prior to the development of surrogate and nested model extensions.
    The derived response computation and synchronization functions
    utilize a single interface to perform the function evaluations. */

class SingleModel: public Model
{
public:
  
  //
  //- Heading: Constructor and destructor
  //

  SingleModel(ProblemDescDB& problem_db); ///< constructor
  ~SingleModel();                         ///< destructor
    
protected:

  //
  //- Heading: Virtual function redefinitions
  //

  /// return userDefinedInterface
  Interface& interface();

  // Perform the response computation portions specific to this derived 
  // class.  In this case, it simply employs userDefinedInterface.map()/
  // synch()/synch_nowait()
  //
  /// portion of compute_response() specific to SingleModel
  /// (invokes a synchronous map() on userDefinedInterface)
  void derived_compute_response(const ActiveSet& set);
  /// portion of asynch_compute_response() specific to SingleModel
  /// (invokes an asynchronous map() on userDefinedInterface)
  void derived_asynch_compute_response(const ActiveSet& set);
  /// portion of synchronize() specific to SingleModel
  /// (invokes synch() on userDefinedInterface)
  const IntResponseMap& derived_synchronize();
  /// portion of synchronize_nowait() specific to SingleModel
  /// (invokes synch_nowait() on userDefinedInterface)
  const IntResponseMap& derived_synchronize_nowait();

  /// SingleModel only supports parallelism in userDefinedInterface,
  /// so this virtual function redefinition is simply a sanity check.
  void component_parallel_mode(short mode);

  /// return userDefinedInterface synchronization setting
  String local_eval_synchronization();
  /// return userDefinedInterface asynchronous evaluation concurrency
  int local_eval_concurrency();

  /// flag which prevents overloading the master with a multiprocessor
  /// evaluation (request forwarded to userDefinedInterface)
  bool derived_master_overload() const;
  /// set up SingleModel for parallel operations (request forwarded to
  /// userDefinedInterface)
  void derived_init_communicators(int max_iterator_concurrency,
				  bool recurse_flag = true);
  /// set up SingleModel for serial operations (request forwarded to
  /// userDefinedInterface).
  void derived_init_serial();
  /// set active parallel configuration for the SingleModel
  /// (request forwarded to userDefinedInterface)
  void derived_set_communicators(int max_iterator_concurrency,
				 bool recurse_flag = true);
  /// deallocate communicator partitions for the SingleModel
  /// (request forwarded to userDefinedInterface)
  void derived_free_communicators(int max_iterator_concurrency,
				  bool recurse_flag = true);

  /// Service userDefinedInterface job requests received from the master.
  /// Completes when a termination message is received from stop_servers().
  void serve(int max_iterator_concurrency);
  /// executed by the master to terminate userDefinedInterface server
  /// operations when SingleModel iteration is complete.
  void stop_servers();

  /// return the userDefinedInterface identifier
  const String& interface_id() const;
  /// return the current evaluation id for the SingleModel
  /// (request forwarded to userDefinedInterface)
  int evaluation_id() const;
  /// return flag indicated usage of an evaluation cache by the SingleModel
  /// (request forwarded to userDefinedInterface)
  bool evaluation_cache() const;

  /// set the evaluation counter reference points for the SingleModel
  /// (request forwarded to userDefinedInterface)
  void set_evaluation_reference();
  /// request fine-grained evaluation reporting within the userDefinedInterface
  void fine_grained_evaluation_counters();
  /// print the evaluation summary for the SingleModel
  /// (request forwarded to userDefinedInterface)
  void print_evaluation_summary(std::ostream& s, bool minimal_header = false,
				bool relative_count = true) const;

  /// set the hierarchical eval ID tag prefix
  virtual void eval_tag_prefix(const String& eval_id_str);
  
private:

  //
  //- Heading: Convenience member functions
  //
    
  //
  //- Heading: Data members
  //

  /// the interface used for mapping variables to responses
  Interface userDefinedInterface;
};


inline SingleModel::~SingleModel()
{ } // Virtual destructor handles referenceCount at Strategy level.


inline Interface& SingleModel::interface()
{ return userDefinedInterface; }


inline void SingleModel::derived_compute_response(const ActiveSet& set)
{ userDefinedInterface.map(currentVariables, set, currentResponse); }


inline void SingleModel::derived_asynch_compute_response(const ActiveSet& set)
{ userDefinedInterface.map(currentVariables, set, currentResponse, true); }


inline const IntResponseMap& SingleModel::derived_synchronize()
{ return userDefinedInterface.synch(); }


inline const IntResponseMap& SingleModel::derived_synchronize_nowait()
{ return userDefinedInterface.synch_nowait(); }


inline String SingleModel::local_eval_synchronization()
{
  return ( userDefinedInterface.asynch_local_evaluation_concurrency() == 1 ) ?
    String("synchronous") : userDefinedInterface.interface_synchronization();
}


inline int SingleModel::local_eval_concurrency()
{ return userDefinedInterface.asynch_local_evaluation_concurrency(); }


inline bool SingleModel::derived_master_overload() const
{
  return ( userDefinedInterface.iterator_eval_dedicated_master_flag() && 
           userDefinedInterface.multi_proc_eval_flag() ) ? true : false;
}


inline void SingleModel::
derived_init_communicators(int max_iterator_concurrency, bool recurse_flag)
{
  userDefinedInterface.init_communicators(messageLengths, 
					  max_iterator_concurrency);
}


inline void SingleModel::derived_init_serial()
{ userDefinedInterface.init_serial(); }


inline void SingleModel::
derived_set_communicators(int max_iterator_concurrency, bool recurse_flag)
{
  parallelLib.parallel_configuration_iterator(modelPCIter);
  userDefinedInterface.set_communicators(messageLengths, 
					 max_iterator_concurrency);
}


inline void SingleModel::
derived_free_communicators(int max_iterator_concurrency, bool recurse_flag)
{
  parallelLib.parallel_configuration_iterator(modelPCIter);
  userDefinedInterface.free_communicators();
}


inline void SingleModel::serve(int max_iterator_concurrency)
{
  set_communicators(max_iterator_concurrency, false); // no recursion (moot)
  //parallelLib.parallel_configuration_iterator(modelPCIter);
  userDefinedInterface.serve_evaluations();
}


inline void SingleModel::stop_servers()
{
  parallelLib.parallel_configuration_iterator(modelPCIter);
  userDefinedInterface.stop_evaluation_servers();
}


inline const String& SingleModel::interface_id() const
{ return userDefinedInterface.interface_id(); }


inline int SingleModel::evaluation_id() const
{ return userDefinedInterface.evaluation_id(); }


inline bool SingleModel::evaluation_cache() const
{ return userDefinedInterface.evaluation_cache(); }


inline void SingleModel::set_evaluation_reference()
{ userDefinedInterface.set_evaluation_reference(); }


inline void SingleModel::fine_grained_evaluation_counters()
{ userDefinedInterface.fine_grained_evaluation_counters(numFns); }


inline void SingleModel::
print_evaluation_summary(std::ostream& s, bool minimal_header,
			 bool relative_count) const
{
  userDefinedInterface.print_evaluation_summary(s, minimal_header,
						relative_count);
}

} // namespace Dakota

#endif
