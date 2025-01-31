/*
 *  conn_builder.cpp
 *
 *  This file is part of NEST.
 *
 *  Copyright (C) 2004 The NEST Initiative
 *
 *  NEST is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  NEST is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with NEST.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "conn_builder.h"

// Includes from libnestutil:
#include "logging.h"

// Includes from nestkernel:
#include "conn_builder_impl.h"
#include "conn_parameter.h"
#include "exceptions.h"
#include "kernel_manager.h"
#include "nest_names.h"
#include "node.h"
#include "vp_manager_impl.h"

// Includes from sli:
#include "dict.h"
#include "fdstream.h"
#include "name.h"

// Includes from C++:
#include <algorithm>

nest::ConnBuilder::ConnBuilder( NodeCollectionPTR sources,
  NodeCollectionPTR targets,
  const DictionaryDatum& conn_spec,
  const std::vector< DictionaryDatum >& syn_specs )
  : sources_( sources )
  , targets_( targets )
  , allow_autapses_( true )
  , allow_multapses_( true )
  , make_symmetric_( false )
  , creates_symmetric_connections_( false )
  , exceptions_raised_( kernel().vp_manager.get_num_threads() )
  , use_structural_plasticity_( false )
  , parameters_requiring_skipping_()
  , param_dicts_()
{
  // We only read a subset of rule-related parameters here. The property 'rule'
  // has already been taken care of in ConnectionManager::get_conn_builder() and
  // rule-specific parameters are handled by the subclass constructors.
  updateValue< bool >( conn_spec, names::allow_autapses, allow_autapses_ );
  updateValue< bool >( conn_spec, names::allow_multapses, allow_multapses_ );
  updateValue< bool >( conn_spec, names::make_symmetric, make_symmetric_ );

  // Synapse-specific parameters that should be skipped when we set default synapse parameters
  skip_syn_params_ = {
    names::weight, names::delay, names::min_delay, names::max_delay, names::num_connections, names::synapse_model
  };

  default_weight_.resize( syn_specs.size() );
  default_delay_.resize( syn_specs.size() );
  default_weight_and_delay_.resize( syn_specs.size() );
  weights_.resize( syn_specs.size() );
  delays_.resize( syn_specs.size() );
  synapse_params_.resize( syn_specs.size() );
  synapse_model_id_.resize( syn_specs.size() );
  synapse_model_id_[ 0 ] = kernel().model_manager.get_synapse_model_id( "static_synapse" );
  param_dicts_.resize( syn_specs.size() );

  // loop through vector of synapse dictionaries, and set synapse parameters
  for ( size_t synapse_indx = 0; synapse_indx < syn_specs.size(); ++synapse_indx )
  {
    auto syn_params = syn_specs[ synapse_indx ];

    set_synapse_model_( syn_params, synapse_indx );
    set_default_weight_or_delay_( syn_params, synapse_indx );

    DictionaryDatum syn_defaults = kernel().model_manager.get_connector_defaults( synapse_model_id_[ synapse_indx ] );

#ifdef HAVE_MUSIC
    // We allow music_channel as alias for receptor_type during connection setup
    ( *syn_defaults )[ names::music_channel ] = 0;
#endif

    set_synapse_params( syn_defaults, syn_params, synapse_indx );
  }

  set_structural_plasticity_parameters( syn_specs );

  // If make_symmetric_ is requested, call reset on all parameters in order
  // to check if all parameters support symmetric connections
  if ( make_symmetric_ )
  {
    reset_weights_();
    reset_delays_();

    for ( auto params : synapse_params_ )
    {
      for ( auto synapse_parameter : params )
      {
        synapse_parameter.second->reset();
      }
    }
  }

  if ( not( sources_->valid() and targets_->valid() ) )
  {
    throw KernelException( "InvalidNodeCollection: sources and targets must be valid NodeCollections." );
  }
}

nest::ConnBuilder::~ConnBuilder()
{
  for ( auto weight : weights_ )
  {
    delete weight;
  }

  for ( auto delay : delays_ )
  {
    delete delay;
  }

  for ( auto params : synapse_params_ )
  {
    for ( auto synapse_parameter : params )
    {
      delete synapse_parameter.second;
    }
  }
}

bool
nest::ConnBuilder::change_connected_synaptic_elements( size_t snode_id, size_t tnode_id, const size_t tid, int update )
{
  int local = true;

  // check whether the source is on this mpi machine
  if ( kernel().node_manager.is_local_node_id( snode_id ) )
  {
    Node* const source = kernel().node_manager.get_node_or_proxy( snode_id, tid );
    const size_t source_thread = source->get_thread();

    // check whether the source is on our thread
    if ( tid == source_thread )
    {
      // update the number of connected synaptic elements
      source->connect_synaptic_element( pre_synaptic_element_name_, update );
    }
  }

  // check whether the target is on this mpi machine
  if ( not kernel().node_manager.is_local_node_id( tnode_id ) )
  {
    local = false;
  }
  else
  {
    Node* const target = kernel().node_manager.get_node_or_proxy( tnode_id, tid );
    const size_t target_thread = target->get_thread();
    // check whether the target is on our thread
    if ( tid != target_thread )
    {
      local = false;
    }
    else
    {
      // update the number of connected synaptic elements
      target->connect_synaptic_element( post_synaptic_element_name_, update );
    }
  }

  return local;
}

void
nest::ConnBuilder::connect()
{
  // We test here, and not in the ConnBuilder constructor, so the derived
  // classes are fully constructed when the test is executed
  for ( auto synapse_model_id : synapse_model_id_ )
  {
    const ConnectorModel& synapse_model = kernel().model_manager.get_connection_model( synapse_model_id );
    const bool requires_symmetric = synapse_model.has_property( ConnectionModelProperties::REQUIRES_SYMMETRIC );

    if ( requires_symmetric and not( is_symmetric() or make_symmetric_ ) )
    {
      throw BadProperty(
        "Connections with this synapse model can only be created as "
        "one-to-one connections with \"make_symmetric\" set to true "
        "or as all-to-all connections with equal source and target "
        "populations and default or scalar parameters." );
    }
  }

  if ( make_symmetric_ and not supports_symmetric() )
  {
    throw NotImplemented( "This connection rule does not support symmetric connections." );
  }

  if ( use_structural_plasticity_ )
  {
    if ( make_symmetric_ )
    {
      throw NotImplemented( "Symmetric connections are not supported in combination with structural plasticity." );
    }
    sp_connect_();
  }
  else
  {
    connect_();
    if ( make_symmetric_ and not creates_symmetric_connections_ )
    {
      // call reset on all parameters
      reset_weights_();
      reset_delays_();

      for ( auto params : synapse_params_ )
      {
        for ( auto synapse_parameter : params )
        {
          synapse_parameter.second->reset();
        }
      }

      std::swap( sources_, targets_ );
      connect_();
      std::swap( sources_, targets_ ); // re-establish original state
    }
  }
  // check if any exceptions have been raised
  for ( size_t tid = 0; tid < kernel().vp_manager.get_num_threads(); ++tid )
  {
    if ( exceptions_raised_.at( tid ).get() )
    {
      throw WrappedThreadException( *( exceptions_raised_.at( tid ) ) );
    }
  }
}

void
nest::ConnBuilder::disconnect()
{
  if ( use_structural_plasticity_ )
  {
    sp_disconnect_();
  }
  else
  {
    disconnect_();
  }

  // check if any exceptions have been raised
  for ( size_t tid = 0; tid < kernel().vp_manager.get_num_threads(); ++tid )
  {
    if ( exceptions_raised_.at( tid ).get() )
    {
      throw WrappedThreadException( *( exceptions_raised_.at( tid ) ) );
    }
  }
}

void
nest::ConnBuilder::update_param_dict_( size_t snode_id,
  Node& target,
  size_t target_thread,
  RngPtr rng,
  size_t synapse_indx )
{
  assert( kernel().vp_manager.get_num_threads() == static_cast< size_t >( param_dicts_[ synapse_indx ].size() ) );

  for ( auto synapse_parameter : synapse_params_[ synapse_indx ] )
  {
    if ( synapse_parameter.second->provides_long() )
    {
      // change value of dictionary entry without allocating new datum
      IntegerDatum* id = static_cast< IntegerDatum* >(
        ( ( *param_dicts_[ synapse_indx ][ target_thread ] )[ synapse_parameter.first ] ).datum() );
      ( *id ) = synapse_parameter.second->value_int( target_thread, rng, snode_id, &target );
    }
    else
    {
      // change value of dictionary entry without allocating new datum
      DoubleDatum* dd = static_cast< DoubleDatum* >(
        ( ( *param_dicts_[ synapse_indx ][ target_thread ] )[ synapse_parameter.first ] ).datum() );
      ( *dd ) = synapse_parameter.second->value_double( target_thread, rng, snode_id, &target );
    }
  }
}

void
nest::ConnBuilder::single_connect_( size_t snode_id, Node& target, size_t target_thread, RngPtr rng )
{
  if ( this->requires_proxies() and not target.has_proxies() )
  {
    throw IllegalConnection( "Cannot use this rule to connect to nodes without proxies (usually devices)." );
  }

  for ( size_t synapse_indx = 0; synapse_indx < synapse_params_.size(); ++synapse_indx )
  {
    update_param_dict_( snode_id, target, target_thread, rng, synapse_indx );

    if ( default_weight_and_delay_[ synapse_indx ] )
    {
      kernel().connection_manager.connect( snode_id,
        &target,
        target_thread,
        synapse_model_id_[ synapse_indx ],
        param_dicts_[ synapse_indx ][ target_thread ] );
    }
    else if ( default_weight_[ synapse_indx ] )
    {
      kernel().connection_manager.connect( snode_id,
        &target,
        target_thread,
        synapse_model_id_[ synapse_indx ],
        param_dicts_[ synapse_indx ][ target_thread ],
        delays_[ synapse_indx ]->value_double( target_thread, rng, snode_id, &target ) );
    }
    else if ( default_delay_[ synapse_indx ] )
    {
      kernel().connection_manager.connect( snode_id,
        &target,
        target_thread,
        synapse_model_id_[ synapse_indx ],
        param_dicts_[ synapse_indx ][ target_thread ],
        numerics::nan,
        weights_[ synapse_indx ]->value_double( target_thread, rng, snode_id, &target ) );
    }
    else
    {
      const double delay = delays_[ synapse_indx ]->value_double( target_thread, rng, snode_id, &target );
      const double weight = weights_[ synapse_indx ]->value_double( target_thread, rng, snode_id, &target );
      kernel().connection_manager.connect( snode_id,
        &target,
        target_thread,
        synapse_model_id_[ synapse_indx ],
        param_dicts_[ synapse_indx ][ target_thread ],
        delay,
        weight );
    }
  }
}

void
nest::ConnBuilder::set_synaptic_element_names( const std::string& pre_name, const std::string& post_name )
{
  if ( pre_name.empty() or post_name.empty() )
  {
    throw BadProperty( "synaptic element names cannot be empty." );
  }

  pre_synaptic_element_name_ = pre_name;
  post_synaptic_element_name_ = post_name;

  use_structural_plasticity_ = true;
}

bool
nest::ConnBuilder::all_parameters_scalar_() const
{
  bool all_scalar = true;

  for ( auto weight : weights_ )
  {
    if ( weight )
    {
      all_scalar = all_scalar and weight->is_scalar();
    }
  }

  for ( auto delay : delays_ )
  {
    if ( delay )
    {
      all_scalar = all_scalar and delay->is_scalar();
    }
  }

  for ( auto params : synapse_params_ )
  {
    for ( auto synapse_parameter : params )
    {
      all_scalar = all_scalar and synapse_parameter.second->is_scalar();
    }
  }

  return all_scalar;
}

bool
nest::ConnBuilder::loop_over_targets_() const
{
  return targets_->size() < kernel().node_manager.size() or not targets_->is_range()
    or parameters_requiring_skipping_.size() > 0;
}

void
nest::ConnBuilder::set_synapse_model_( DictionaryDatum syn_params, size_t synapse_indx )
{
  if ( not syn_params->known( names::synapse_model ) )
  {
    throw BadProperty( "Synapse spec must contain synapse model." );
  }
  const std::string syn_name = ( *syn_params )[ names::synapse_model ];

  // The following call will throw "UnknownSynapseType" if syn_name is not naming a known model
  const size_t synapse_model_id = kernel().model_manager.get_synapse_model_id( syn_name );
  synapse_model_id_[ synapse_indx ] = synapse_model_id;

  // We need to make sure that Connect can process all synapse parameters specified.
  const ConnectorModel& synapse_model = kernel().model_manager.get_connection_model( synapse_model_id );
  synapse_model.check_synapse_params( syn_params );
}

void
nest::ConnBuilder::set_default_weight_or_delay_( DictionaryDatum syn_params, size_t synapse_indx )
{
  DictionaryDatum syn_defaults = kernel().model_manager.get_connector_defaults( synapse_model_id_[ synapse_indx ] );

  // All synapse models have the possibility to set the delay (see SynIdDelay), but some have
  // homogeneous weights, hence it should be possible to set the delay without the weight.
  default_weight_[ synapse_indx ] = not syn_params->known( names::weight );

  default_delay_[ synapse_indx ] = not syn_params->known( names::delay );

  // If neither weight nor delay are given in the dict, we handle this separately. Important for
  // hom_w synapses, on which weight cannot be set. However, we use default weight and delay for
  // _all_ types of synapses.
  default_weight_and_delay_[ synapse_indx ] = ( default_weight_[ synapse_indx ] and default_delay_[ synapse_indx ] );

  if ( not default_weight_and_delay_[ synapse_indx ] )
  {
    weights_[ synapse_indx ] = syn_params->known( names::weight )
      ? ConnParameter::create( ( *syn_params )[ names::weight ], kernel().vp_manager.get_num_threads() )
      : ConnParameter::create( ( *syn_defaults )[ names::weight ], kernel().vp_manager.get_num_threads() );
    register_parameters_requiring_skipping_( *weights_[ synapse_indx ] );

    delays_[ synapse_indx ] = syn_params->known( names::delay )
      ? ConnParameter::create( ( *syn_params )[ names::delay ], kernel().vp_manager.get_num_threads() )
      : ConnParameter::create( ( *syn_defaults )[ names::delay ], kernel().vp_manager.get_num_threads() );
  }
  else if ( default_weight_[ synapse_indx ] )
  {
    delays_[ synapse_indx ] = syn_params->known( names::delay )
      ? ConnParameter::create( ( *syn_params )[ names::delay ], kernel().vp_manager.get_num_threads() )
      : ConnParameter::create( ( *syn_defaults )[ names::delay ], kernel().vp_manager.get_num_threads() );
  }
  register_parameters_requiring_skipping_( *delays_[ synapse_indx ] );
}

void
nest::ConnBuilder::set_synapse_params( DictionaryDatum syn_defaults, DictionaryDatum syn_params, size_t synapse_indx )
{
  for ( Dictionary::const_iterator default_it = syn_defaults->begin(); default_it != syn_defaults->end(); ++default_it )
  {
    const Name param_name = default_it->first;
    if ( skip_syn_params_.find( param_name ) != skip_syn_params_.end() )
    {
      continue; // weight, delay or other not-settable parameter
    }

    if ( syn_params->known( param_name ) )
    {
      synapse_params_[ synapse_indx ][ param_name ] =
        ConnParameter::create( ( *syn_params )[ param_name ], kernel().vp_manager.get_num_threads() );
      register_parameters_requiring_skipping_( *synapse_params_[ synapse_indx ][ param_name ] );
    }
  }

  // Now create dictionary with dummy values that we will use to pass settings to the synapses created. We
  // create it here once to avoid re-creating the object over and over again.
  for ( size_t tid = 0; tid < kernel().vp_manager.get_num_threads(); ++tid )
  {
    param_dicts_[ synapse_indx ].push_back( new Dictionary() );

    for ( auto param : synapse_params_[ synapse_indx ] )
    {
      if ( param.second->provides_long() )
      {
        ( *param_dicts_[ synapse_indx ][ tid ] )[ param.first ] = Token( new IntegerDatum( 0 ) );
      }
      else
      {
        ( *param_dicts_[ synapse_indx ][ tid ] )[ param.first ] = Token( new DoubleDatum( 0.0 ) );
      }
    }
  }
}

void
nest::ConnBuilder::set_structural_plasticity_parameters( std::vector< DictionaryDatum > syn_specs )
{
  bool have_structural_plasticity_parameters = false;
  for ( auto& syn_spec : syn_specs )
  {
    if ( syn_spec->known( names::pre_synaptic_element ) or syn_spec->known( names::post_synaptic_element ) )
    {
      have_structural_plasticity_parameters = true;
    }
  }

  if ( not have_structural_plasticity_parameters )
  {
    return;
  }

  if ( syn_specs.size() > 1 )
  {
    throw KernelException( "Structural plasticity can only be used with a single syn_spec." );
  }

  const DictionaryDatum syn_spec = syn_specs[ 0 ];
  if ( syn_spec->known( names::pre_synaptic_element ) xor syn_spec->known( names::post_synaptic_element ) )
  {
    throw BadProperty( "Structural plasticity requires both a pre- and postsynaptic element." );
  }

  pre_synaptic_element_name_ = getValue< std::string >( syn_spec, names::pre_synaptic_element );
  post_synaptic_element_name_ = getValue< std::string >( syn_spec, names::post_synaptic_element );
  use_structural_plasticity_ = true;
}

void
nest::ConnBuilder::reset_weights_()
{
  for ( auto weight : weights_ )
  {
    if ( weight )
    {
      weight->reset();
    }
  }
}

void
nest::ConnBuilder::reset_delays_()
{
  for ( auto delay : delays_ )
  {
    if ( delay )
    {
      delay->reset();
    }
  }
}

nest::OneToOneBuilder::OneToOneBuilder( const NodeCollectionPTR sources,
  const NodeCollectionPTR targets,
  const DictionaryDatum& conn_spec,
  const std::vector< DictionaryDatum >& syn_specs )
  : ConnBuilder( sources, targets, conn_spec, syn_specs )
{
  // make sure that target and source population have the same size
  if ( sources_->size() != targets_->size() )
  {
    throw DimensionMismatch( "Source and Target population must be of the same size." );
  }
}

void
nest::OneToOneBuilder::connect_()
{

#pragma omp parallel
  {
    // get thread id
    const size_t tid = kernel().vp_manager.get_thread_id();

    try
    {
      RngPtr rng = get_vp_specific_rng( tid );

      if ( loop_over_targets_() )
      {
        // A more efficient way of doing this might be to use NodeCollection's local_begin(). For this to work we would
        // need to change some of the logic, sources and targets might not be on the same process etc., so therefore
        // we are not doing it at the moment. This also applies to other ConnBuilders below.
        NodeCollection::const_iterator target_it = targets_->begin();
        NodeCollection::const_iterator source_it = sources_->begin();
        for ( ; target_it < targets_->end(); ++target_it, ++source_it )
        {
          assert( source_it < sources_->end() );

          const size_t snode_id = ( *source_it ).node_id;
          const size_t tnode_id = ( *target_it ).node_id;

          if ( snode_id == tnode_id and not allow_autapses_ )
          {
            continue;
          }

          Node* const target = kernel().node_manager.get_node_or_proxy( tnode_id, tid );
          if ( target->is_proxy() )
          {
            // skip array parameters handled in other virtual processes
            skip_conn_parameter_( tid );
            continue;
          }

          single_connect_( snode_id, *target, tid, rng );
        }
      }
      else
      {
        const SparseNodeArray& local_nodes = kernel().node_manager.get_local_nodes( tid );
        SparseNodeArray::const_iterator n;
        for ( n = local_nodes.begin(); n != local_nodes.end(); ++n )
        {
          Node* target = n->get_node();

          const size_t tnode_id = n->get_node_id();
          const long lid = targets_->get_lid( tnode_id );
          if ( lid < 0 ) // Is local node in target list?
          {
            continue;
          }

          // one-to-one, thus we can use target idx for source as well
          const size_t snode_id = ( *sources_ )[ lid ];
          if ( not allow_autapses_ and snode_id == tnode_id )
          {
            // no skipping required / possible,
            // as we iterate only over local nodes
            continue;
          }
          single_connect_( snode_id, *target, tid, rng );
        }
      }
    }
    catch ( std::exception& err )
    {
      // We must create a new exception here, err's lifetime ends at
      // the end of the catch block.
      exceptions_raised_.at( tid ) = std::shared_ptr< WrappedThreadException >( new WrappedThreadException( err ) );
    }
  }
}

void
nest::OneToOneBuilder::disconnect_()
{

#pragma omp parallel
  {
    // get thread id
    const size_t tid = kernel().vp_manager.get_thread_id();

    try
    {
      NodeCollection::const_iterator target_it = targets_->begin();
      NodeCollection::const_iterator source_it = sources_->begin();
      for ( ; target_it < targets_->end(); ++target_it, ++source_it )
      {
        assert( source_it < sources_->end() );

        const size_t tnode_id = ( *target_it ).node_id;
        const size_t snode_id = ( *source_it ).node_id;

        // check whether the target is on this mpi machine
        if ( not kernel().node_manager.is_local_node_id( tnode_id ) )
        {
          // Disconnecting: no parameter skipping required
          continue;
        }

        Node* const target = kernel().node_manager.get_node_or_proxy( tnode_id, tid );
        const size_t target_thread = target->get_thread();

        // check whether the target is a proxy
        if ( target->is_proxy() )
        {
          // Disconnecting: no parameter skipping required
          continue;
        }
        single_disconnect_( snode_id, *target, target_thread );
      }
    }
    catch ( std::exception& err )
    {
      // We must create a new exception here, err's lifetime ends at
      // the end of the catch block.
      exceptions_raised_.at( tid ) = std::shared_ptr< WrappedThreadException >( new WrappedThreadException( err ) );
    }
  }
}

void
nest::OneToOneBuilder::sp_connect_()
{

#pragma omp parallel
  {
    // get thread id
    const size_t tid = kernel().vp_manager.get_thread_id();

    try
    {
      RngPtr rng = get_vp_specific_rng( tid );

      NodeCollection::const_iterator target_it = targets_->begin();
      NodeCollection::const_iterator source_it = sources_->begin();
      for ( ; target_it < targets_->end(); ++target_it, ++source_it )
      {
        assert( source_it < sources_->end() );

        const size_t snode_id = ( *source_it ).node_id;
        const size_t tnode_id = ( *target_it ).node_id;

        if ( snode_id == tnode_id and not allow_autapses_ )
        {
          continue;
        }

        if ( not change_connected_synaptic_elements( snode_id, tnode_id, tid, 1 ) )
        {
          skip_conn_parameter_( tid );
          continue;
        }
        Node* const target = kernel().node_manager.get_node_or_proxy( tnode_id, tid );
        const size_t target_thread = target->get_thread();

        single_connect_( snode_id, *target, target_thread, rng );
      }
    }
    catch ( std::exception& err )
    {
      // We must create a new exception here, err's lifetime ends at
      // the end of the catch block.
      exceptions_raised_.at( tid ) = std::shared_ptr< WrappedThreadException >( new WrappedThreadException( err ) );
    }
  }
}

void
nest::OneToOneBuilder::sp_disconnect_()
{

#pragma omp parallel
  {
    // get thread id
    const size_t tid = kernel().vp_manager.get_thread_id();

    try
    {
      NodeCollection::const_iterator target_it = targets_->begin();
      NodeCollection::const_iterator source_it = sources_->begin();
      for ( ; target_it < targets_->end(); ++target_it, ++source_it )
      {
        assert( source_it < sources_->end() );

        const size_t snode_id = ( *source_it ).node_id;
        const size_t tnode_id = ( *target_it ).node_id;

        if ( not change_connected_synaptic_elements( snode_id, tnode_id, tid, -1 ) )
        {
          continue;
        }

        Node* const target = kernel().node_manager.get_node_or_proxy( tnode_id, tid );
        const size_t target_thread = target->get_thread();

        single_disconnect_( snode_id, *target, target_thread );
      }
    }
    catch ( std::exception& err )
    {
      // We must create a new exception here, err's lifetime ends at
      // the end of the catch block.
      exceptions_raised_.at( tid ) = std::shared_ptr< WrappedThreadException >( new WrappedThreadException( err ) );
    }
  }
}

void
nest::AllToAllBuilder::connect_()
{

#pragma omp parallel
  {
    // get thread id
    const size_t tid = kernel().vp_manager.get_thread_id();

    try
    {
      RngPtr rng = get_vp_specific_rng( tid );

      if ( loop_over_targets_() )
      {
        NodeCollection::const_iterator target_it = targets_->begin();
        for ( ; target_it < targets_->end(); ++target_it )
        {
          const size_t tnode_id = ( *target_it ).node_id;
          Node* const target = kernel().node_manager.get_node_or_proxy( tnode_id, tid );
          if ( target->is_proxy() )
          {
            skip_conn_parameter_( tid, sources_->size() );
            continue;
          }

          inner_connect_( tid, rng, target, tnode_id, true );
        }
      }
      else
      {
        const SparseNodeArray& local_nodes = kernel().node_manager.get_local_nodes( tid );
        SparseNodeArray::const_iterator n;
        for ( n = local_nodes.begin(); n != local_nodes.end(); ++n )
        {
          const size_t tnode_id = n->get_node_id();

          // Is the local node in the targets list?
          if ( targets_->get_lid( tnode_id ) < 0 )
          {
            continue;
          }

          inner_connect_( tid, rng, n->get_node(), tnode_id, false );
        }
      }
    }
    catch ( std::exception& err )
    {
      // We must create a new exception here, err's lifetime ends at
      // the end of the catch block.
      exceptions_raised_.at( tid ) = std::shared_ptr< WrappedThreadException >( new WrappedThreadException( err ) );
    }
  }
}

void
nest::AllToAllBuilder::inner_connect_( const int tid, RngPtr rng, Node* target, size_t tnode_id, bool skip )
{
  const size_t target_thread = target->get_thread();

  // check whether the target is on our thread
  if ( static_cast< size_t >( tid ) != target_thread )
  {
    if ( skip )
    {
      skip_conn_parameter_( tid, sources_->size() );
    }
    return;
  }

  NodeCollection::const_iterator source_it = sources_->begin();
  for ( ; source_it < sources_->end(); ++source_it )
  {
    const size_t snode_id = ( *source_it ).node_id;

    if ( not allow_autapses_ and snode_id == tnode_id )
    {
      if ( skip )
      {
        skip_conn_parameter_( target_thread );
      }
      continue;
    }

    single_connect_( snode_id, *target, target_thread, rng );
  }
}

void
nest::AllToAllBuilder::sp_connect_()
{
#pragma omp parallel
  {
    // get thread id
    const size_t tid = kernel().vp_manager.get_thread_id();
    try
    {
      RngPtr rng = get_vp_specific_rng( tid );

      NodeCollection::const_iterator target_it = targets_->begin();
      for ( ; target_it < targets_->end(); ++target_it )
      {
        const size_t tnode_id = ( *target_it ).node_id;

        NodeCollection::const_iterator source_it = sources_->begin();
        for ( ; source_it < sources_->end(); ++source_it )
        {
          const size_t snode_id = ( *source_it ).node_id;

          if ( not allow_autapses_ and snode_id == tnode_id )
          {
            skip_conn_parameter_( tid );
            continue;
          }
          if ( not change_connected_synaptic_elements( snode_id, tnode_id, tid, 1 ) )
          {
            skip_conn_parameter_( tid, sources_->size() );
            continue;
          }
          Node* const target = kernel().node_manager.get_node_or_proxy( tnode_id, tid );
          const size_t target_thread = target->get_thread();
          single_connect_( snode_id, *target, target_thread, rng );
        }
      }
    }
    catch ( std::exception& err )
    {
      // We must create a new exception here, err's lifetime ends at
      // the end of the catch block.
      exceptions_raised_.at( tid ) = std::shared_ptr< WrappedThreadException >( new WrappedThreadException( err ) );
    }
  }
}

void
nest::AllToAllBuilder::disconnect_()
{

#pragma omp parallel
  {
    // get thread id
    const size_t tid = kernel().vp_manager.get_thread_id();

    try
    {
      NodeCollection::const_iterator target_it = targets_->begin();
      for ( ; target_it < targets_->end(); ++target_it )
      {
        const size_t tnode_id = ( *target_it ).node_id;

        // check whether the target is on this mpi machine
        if ( not kernel().node_manager.is_local_node_id( tnode_id ) )
        {
          // Disconnecting: no parameter skipping required
          continue;
        }

        Node* const target = kernel().node_manager.get_node_or_proxy( tnode_id, tid );
        const size_t target_thread = target->get_thread();

        // check whether the target is a proxy
        if ( target->is_proxy() )
        {
          // Disconnecting: no parameter skipping required
          continue;
        }

        NodeCollection::const_iterator source_it = sources_->begin();
        for ( ; source_it < sources_->end(); ++source_it )
        {
          const size_t snode_id = ( *source_it ).node_id;
          single_disconnect_( snode_id, *target, target_thread );
        }
      }
    }
    catch ( std::exception& err )
    {
      // We must create a new exception here, err's lifetime ends at
      // the end of the catch block.
      exceptions_raised_.at( tid ) = std::shared_ptr< WrappedThreadException >( new WrappedThreadException( err ) );
    }
  }
}

void
nest::AllToAllBuilder::sp_disconnect_()
{
#pragma omp parallel
  {
    // get thread id
    const size_t tid = kernel().vp_manager.get_thread_id();

    try
    {
      NodeCollection::const_iterator target_it = targets_->begin();
      for ( ; target_it < targets_->end(); ++target_it )
      {
        const size_t tnode_id = ( *target_it ).node_id;

        NodeCollection::const_iterator source_it = sources_->begin();
        for ( ; source_it < sources_->end(); ++source_it )
        {
          const size_t snode_id = ( *source_it ).node_id;

          if ( not change_connected_synaptic_elements( snode_id, tnode_id, tid, -1 ) )
          {
            // Disconnecting: no parameter skipping required
            continue;
          }
          Node* const target = kernel().node_manager.get_node_or_proxy( tnode_id, tid );
          const size_t target_thread = target->get_thread();
          single_disconnect_( snode_id, *target, target_thread );
        }
      }
    }
    catch ( std::exception& err )
    {
      // We must create a new exception here, err's lifetime ends at
      // the end of the catch block.
      exceptions_raised_.at( tid ) = std::shared_ptr< WrappedThreadException >( new WrappedThreadException( err ) );
    }
  }
}

nest::FixedInDegreeBuilder::FixedInDegreeBuilder( NodeCollectionPTR sources,
  NodeCollectionPTR targets,
  const DictionaryDatum& conn_spec,
  const std::vector< DictionaryDatum >& syn_specs )
  : ConnBuilder( sources, targets, conn_spec, syn_specs )
{
  // check for potential errors
  long n_sources = static_cast< long >( sources_->size() );
  if ( n_sources == 0 )
  {
    throw BadProperty( "Source array must not be empty." );
  }
  ParameterDatum* pd = dynamic_cast< ParameterDatum* >( ( *conn_spec )[ names::indegree ].datum() );
  if ( pd )
  {
    indegree_ = *pd;
    // TODO: Checks of parameter range
  }
  else
  {
    // Assume indegree is a scalar
    const long value = ( *conn_spec )[ names::indegree ];
    indegree_ = std::shared_ptr< Parameter >( new ConstantParameter( value ) );

    // verify that indegree is not larger than source population if multapses are disabled
    if ( not allow_multapses_ )
    {
      if ( value > n_sources )
      {
        throw BadProperty( "Indegree cannot be larger than population size." );
      }
      else if ( value == n_sources and not allow_autapses_ )
      {
        LOG( M_WARNING,
          "FixedInDegreeBuilder::connect",
          "Multapses and autapses prohibited. When the sources and the targets "
          "have a non-empty intersection, the connect algorithm will enter an infinite loop." );
        return;
      }

      if ( value > 0.9 * n_sources )
      {
        LOG( M_WARNING,
          "FixedInDegreeBuilder::connect",
          "Multapses are prohibited and you request more than 90% connectivity. Expect long connecting times!" );
      }
    } // if (not allow_multapses_ )

    if ( value < 0 )
    {
      throw BadProperty( "Indegree cannot be less than zero." );
    }
  }
}

void
nest::FixedInDegreeBuilder::connect_()
{

#pragma omp parallel
  {
    // get thread id
    const size_t tid = kernel().vp_manager.get_thread_id();

    try
    {
      RngPtr rng = get_vp_specific_rng( tid );

      if ( loop_over_targets_() )
      {
        NodeCollection::const_iterator target_it = targets_->begin();
        for ( ; target_it < targets_->end(); ++target_it )
        {
          const size_t tnode_id = ( *target_it ).node_id;
          Node* const target = kernel().node_manager.get_node_or_proxy( tnode_id, tid );

          const long indegree_value = std::round( indegree_->value( rng, target ) );
          if ( target->is_proxy() )
          {
            // skip array parameters handled in other virtual processes
            skip_conn_parameter_( tid, indegree_value );
            continue;
          }

          inner_connect_( tid, rng, target, tnode_id, true, indegree_value );
        }
      }
      else
      {
        const SparseNodeArray& local_nodes = kernel().node_manager.get_local_nodes( tid );
        SparseNodeArray::const_iterator n;
        for ( n = local_nodes.begin(); n != local_nodes.end(); ++n )
        {
          const size_t tnode_id = n->get_node_id();

          // Is the local node in the targets list?
          if ( targets_->get_lid( tnode_id ) < 0 )
          {
            continue;
          }
          auto source = n->get_node();
          const long indegree_value = std::round( indegree_->value( rng, source ) );

          inner_connect_( tid, rng, source, tnode_id, false, indegree_value );
        }
      }
    }
    catch ( std::exception& err )
    {
      // We must create a new exception here, err's lifetime ends at
      // the end of the catch block.
      exceptions_raised_.at( tid ) = std::shared_ptr< WrappedThreadException >( new WrappedThreadException( err ) );
    }
  }
}

void
nest::FixedInDegreeBuilder::inner_connect_( const int tid,
  RngPtr rng,
  Node* target,
  size_t tnode_id,
  bool skip,
  long indegree_value )
{
  const size_t target_thread = target->get_thread();

  // check whether the target is on our thread
  if ( static_cast< size_t >( tid ) != target_thread )
  {
    // skip array parameters handled in other virtual processes
    if ( skip )
    {
      skip_conn_parameter_( tid, indegree_value );
    }
    return;
  }

  std::set< long > ch_ids;
  long n_rnd = sources_->size();

  for ( long j = 0; j < indegree_value; ++j )
  {
    unsigned long s_id;
    size_t snode_id;
    bool skip_autapse = false;
    bool skip_multapse = false;

    do
    {
      s_id = rng->ulrand( n_rnd );
      snode_id = ( *sources_ )[ s_id ];
      skip_autapse = not allow_autapses_ and snode_id == tnode_id;
      skip_multapse = not allow_multapses_ and ch_ids.find( s_id ) != ch_ids.end();
    } while ( skip_autapse or skip_multapse );

    if ( not allow_multapses_ )
    {
      ch_ids.insert( s_id );
    }

    single_connect_( snode_id, *target, target_thread, rng );
  }
}

nest::FixedOutDegreeBuilder::FixedOutDegreeBuilder( NodeCollectionPTR sources,
  NodeCollectionPTR targets,
  const DictionaryDatum& conn_spec,
  const std::vector< DictionaryDatum >& syn_specs )
  : ConnBuilder( sources, targets, conn_spec, syn_specs )
{
  // check for potential errors
  long n_targets = static_cast< long >( targets_->size() );
  if ( n_targets == 0 )
  {
    throw BadProperty( "Target array must not be empty." );
  }
  ParameterDatum* pd = dynamic_cast< ParameterDatum* >( ( *conn_spec )[ names::outdegree ].datum() );
  if ( pd )
  {
    outdegree_ = *pd;
    // TODO: Checks of parameter range
  }
  else
  {
    // Assume outdegree is a scalar
    const long value = ( *conn_spec )[ names::outdegree ];

    outdegree_ = std::shared_ptr< Parameter >( new ConstantParameter( value ) );

    // verify that outdegree is not larger than target population if multapses
    // are disabled
    if ( not allow_multapses_ )
    {
      if ( value > n_targets )
      {
        throw BadProperty( "Outdegree cannot be larger than population size." );
      }
      else if ( value == n_targets and not allow_autapses_ )
      {
        LOG( M_WARNING,
          "FixedOutDegreeBuilder::connect",
          "Multapses and autapses prohibited. When the sources and the targets "
          "have a non-empty intersection, the connect algorithm will enter an infinite loop." );
        return;
      }

      if ( value > 0.9 * n_targets )
      {
        LOG( M_WARNING,
          "FixedOutDegreeBuilder::connect",
          "Multapses are prohibited and you request more than 90% connectivity. Expect long connecting times!" );
      }
    }

    if ( value < 0 )
    {
      throw BadProperty( "Outdegree cannot be less than zero." );
    }
  }
}

void
nest::FixedOutDegreeBuilder::connect_()
{
  // get global rng that is tested for synchronization for all threads
  RngPtr grng = get_rank_synced_rng();

  NodeCollection::const_iterator source_it = sources_->begin();
  for ( ; source_it < sources_->end(); ++source_it )
  {
    const size_t snode_id = ( *source_it ).node_id;

    std::set< long > ch_ids;
    std::vector< size_t > tgt_ids_;
    const long n_rnd = targets_->size();

    Node* source_node = kernel().node_manager.get_node_or_proxy( snode_id );
    const long outdegree_value = std::round( outdegree_->value( grng, source_node ) );
    for ( long j = 0; j < outdegree_value; ++j )
    {
      unsigned long t_id;
      size_t tnode_id;
      bool skip_autapse = false;
      bool skip_multapse = false;

      do
      {
        t_id = grng->ulrand( n_rnd );
        tnode_id = ( *targets_ )[ t_id ];
        skip_autapse = not allow_autapses_ and tnode_id == snode_id;
        skip_multapse = not allow_multapses_ and ch_ids.find( t_id ) != ch_ids.end();
      } while ( skip_autapse or skip_multapse );

      if ( not allow_multapses_ )
      {
        ch_ids.insert( t_id );
      }

      tgt_ids_.push_back( tnode_id );
    }

#pragma omp parallel
    {
      // get thread id
      const size_t tid = kernel().vp_manager.get_thread_id();

      try
      {
        RngPtr rng = get_vp_specific_rng( tid );

        std::vector< size_t >::const_iterator tnode_id_it = tgt_ids_.begin();
        for ( ; tnode_id_it != tgt_ids_.end(); ++tnode_id_it )
        {
          Node* const target = kernel().node_manager.get_node_or_proxy( *tnode_id_it, tid );
          if ( target->is_proxy() )
          {
            // skip array parameters handled in other virtual processes
            skip_conn_parameter_( tid );
            continue;
          }

          single_connect_( snode_id, *target, tid, rng );
        }
      }
      catch ( std::exception& err )
      {
        // We must create a new exception here, err's lifetime ends at
        // the end of the catch block.
        exceptions_raised_.at( tid ) = std::shared_ptr< WrappedThreadException >( new WrappedThreadException( err ) );
      }
    }
  }
}

nest::FixedTotalNumberBuilder::FixedTotalNumberBuilder( NodeCollectionPTR sources,
  NodeCollectionPTR targets,
  const DictionaryDatum& conn_spec,
  const std::vector< DictionaryDatum >& syn_specs )
  : ConnBuilder( sources, targets, conn_spec, syn_specs )
  , N_( ( *conn_spec )[ names::N ] )
{

  // check for potential errors

  // verify that total number of connections is not larger than
  // N_sources*N_targets
  if ( not allow_multapses_ )
  {
    if ( ( N_ > static_cast< long >( sources_->size() * targets_->size() ) ) )
    {
      throw BadProperty( "Total number of connections cannot exceed product of source and target population sizes." );
    }
  }

  if ( N_ < 0 )
  {
    throw BadProperty( "Total number of connections cannot be negative." );
  }

  // for now multapses cannot be forbidden
  // TODO: Implement option for multapses_ = False, where already existing
  // connections are stored in
  // a bitmap
  if ( not allow_multapses_ )
  {
    throw NotImplemented( "Connect doesn't support the suppression of multapses in the FixedTotalNumber connector." );
  }
}

void
nest::FixedTotalNumberBuilder::connect_()
{
  const int M = kernel().vp_manager.get_num_virtual_processes();
  const long size_sources = sources_->size();
  const long size_targets = targets_->size();

  // drawing connection ids

  // Compute the distribution of targets over processes using the modulo
  // function
  std::vector< size_t > number_of_targets_on_vp( M, 0 );
  std::vector< size_t > local_targets;
  local_targets.reserve( size_targets / kernel().mpi_manager.get_num_processes() );
  for ( size_t t = 0; t < targets_->size(); t++ )
  {
    int vp = kernel().vp_manager.node_id_to_vp( ( *targets_ )[ t ] );
    ++number_of_targets_on_vp[ vp ];
    if ( kernel().vp_manager.is_local_vp( vp ) )
    {
      local_targets.push_back( ( *targets_ )[ t ] );
    }
  }

  // We use the multinomial distribution to determine the number of
  // connections that will be made on one virtual process, i.e. we
  // partition the set of edges into n_vps subsets. The number of
  // edges on one virtual process is binomially distributed with
  // the boundary condition that the sum of all edges over virtual
  // processes is the total number of edges.
  // To obtain the num_conns_on_vp we adapt the gsl
  // implementation of the multinomial distribution.

  // K from gsl is equivalent to M = n_vps
  // N is already taken from stack
  // p[] is targets_on_vp
  std::vector< long > num_conns_on_vp( M, 0 ); // corresponds to n[]

  // calculate exact multinomial distribution
  // get global rng that is tested for synchronization for all threads
  RngPtr grng = get_rank_synced_rng();

  // begin code adapted from gsl 1.8 //
  double sum_dist = 0.0; // corresponds to sum_p
  // norm is equivalent to size_targets
  unsigned int sum_partitions = 0; // corresponds to sum_n

  binomial_distribution bino_dist;
  for ( int k = 0; k < M; k++ )
  {
    // If we have distributed all connections on the previous processes we exit the loop. It is important to
    // have this check here, as N - sum_partition is set as n value for GSL, and this must be larger than 0.
    if ( N_ == sum_partitions )
    {
      break;
    }
    if ( number_of_targets_on_vp[ k ] > 0 )
    {
      double num_local_targets = static_cast< double >( number_of_targets_on_vp[ k ] );
      double p_local = num_local_targets / ( size_targets - sum_dist );

      binomial_distribution::param_type param( N_ - sum_partitions, p_local );
      num_conns_on_vp[ k ] = bino_dist( grng, param );
    }

    sum_dist += static_cast< double >( number_of_targets_on_vp[ k ] );
    sum_partitions += static_cast< unsigned int >( num_conns_on_vp[ k ] );
  }

  // end code adapted from gsl 1.8

#pragma omp parallel
  {
    // get thread id
    const size_t tid = kernel().vp_manager.get_thread_id();

    try
    {
      const size_t vp_id = kernel().vp_manager.thread_to_vp( tid );

      if ( kernel().vp_manager.is_local_vp( vp_id ) )
      {
        RngPtr rng = get_vp_specific_rng( tid );

        // gather local target node IDs
        std::vector< size_t > thread_local_targets;
        thread_local_targets.reserve( number_of_targets_on_vp[ vp_id ] );

        std::vector< size_t >::const_iterator tnode_id_it = local_targets.begin();
        for ( ; tnode_id_it != local_targets.end(); ++tnode_id_it )
        {
          if ( kernel().vp_manager.node_id_to_vp( *tnode_id_it ) == vp_id )
          {
            thread_local_targets.push_back( *tnode_id_it );
          }
        }

        assert( thread_local_targets.size() == number_of_targets_on_vp[ vp_id ] );

        while ( num_conns_on_vp[ vp_id ] > 0 )
        {

          // draw random numbers for source node from all source neurons
          const long s_index = rng->ulrand( size_sources );
          // draw random numbers for target node from
          // targets_on_vp on this virtual process
          const long t_index = rng->ulrand( thread_local_targets.size() );
          // map random number of source node to node ID corresponding to
          // the source_adr vector
          const long snode_id = ( *sources_ )[ s_index ];
          // map random number of target node to node ID using the
          // targets_on_vp vector
          const long tnode_id = thread_local_targets[ t_index ];

          Node* const target = kernel().node_manager.get_node_or_proxy( tnode_id, tid );
          const size_t target_thread = target->get_thread();

          if ( allow_autapses_ or snode_id != tnode_id )
          {
            single_connect_( snode_id, *target, target_thread, rng );
            num_conns_on_vp[ vp_id ]--;
          }
        }
      }
    }
    catch ( std::exception& err )
    {
      // We must create a new exception here, err's lifetime ends at
      // the end of the catch block.
      exceptions_raised_.at( tid ) = std::shared_ptr< WrappedThreadException >( new WrappedThreadException( err ) );
    }
  }
}


nest::BernoulliBuilder::BernoulliBuilder( NodeCollectionPTR sources,
  NodeCollectionPTR targets,
  const DictionaryDatum& conn_spec,
  const std::vector< DictionaryDatum >& syn_specs )
  : ConnBuilder( sources, targets, conn_spec, syn_specs )
{
  ParameterDatum* pd = dynamic_cast< ParameterDatum* >( ( *conn_spec )[ names::p ].datum() );
  if ( pd )
  {
    p_ = *pd;
    // TODO: Checks of parameter range
  }
  else
  {
    // Assume p is a scalar
    const double value = ( *conn_spec )[ names::p ];
    if ( value < 0 or 1 < value )
    {
      throw BadProperty( "Connection probability 0 <= p <= 1 required." );
    }
    p_ = std::shared_ptr< Parameter >( new ConstantParameter( value ) );
  }
}


void
nest::BernoulliBuilder::connect_()
{
#pragma omp parallel
  {
    // get thread id
    const size_t tid = kernel().vp_manager.get_thread_id();

    try
    {
      RngPtr rng = get_vp_specific_rng( tid );

      if ( loop_over_targets_() )
      {
        NodeCollection::const_iterator target_it = targets_->begin();
        for ( ; target_it < targets_->end(); ++target_it )
        {
          const size_t tnode_id = ( *target_it ).node_id;
          Node* const target = kernel().node_manager.get_node_or_proxy( tnode_id, tid );
          if ( target->is_proxy() )
          {
            // skip array parameters handled in other virtual processes
            skip_conn_parameter_( tid );
            continue;
          }

          inner_connect_( tid, rng, target, tnode_id );
        }
      }

      else
      {
        const SparseNodeArray& local_nodes = kernel().node_manager.get_local_nodes( tid );
        SparseNodeArray::const_iterator n;
        for ( n = local_nodes.begin(); n != local_nodes.end(); ++n )
        {
          const size_t tnode_id = n->get_node_id();

          // Is the local node in the targets list?
          if ( targets_->get_lid( tnode_id ) < 0 )
          {
            continue;
          }

          inner_connect_( tid, rng, n->get_node(), tnode_id );
        }
      }
    }
    catch ( std::exception& err )
    {
      // We must create a new exception here, err's lifetime ends at
      // the end of the catch block.
      exceptions_raised_.at( tid ) = std::shared_ptr< WrappedThreadException >( new WrappedThreadException( err ) );
    }
  } // of omp parallel
}

void
nest::BernoulliBuilder::inner_connect_( const int tid, RngPtr rng, Node* target, size_t tnode_id )
{
  const size_t target_thread = target->get_thread();

  // check whether the target is on our thread
  if ( static_cast< size_t >( tid ) != target_thread )
  {
    return;
  }

  // It is not possible to create multapses with this type of BernoulliBuilder,
  // hence leave out corresponding checks.

  NodeCollection::const_iterator source_it = sources_->begin();
  for ( ; source_it < sources_->end(); ++source_it )
  {
    const size_t snode_id = ( *source_it ).node_id;

    if ( not allow_autapses_ and snode_id == tnode_id )
    {
      continue;
    }
    if ( rng->drand() >= p_->value( rng, target ) )
    {
      continue;
    }

    single_connect_( snode_id, *target, target_thread, rng );
  }
}


nest::AuxiliaryBuilder::AuxiliaryBuilder( NodeCollectionPTR sources,
  NodeCollectionPTR targets,
  const DictionaryDatum& conn_spec,
  const std::vector< DictionaryDatum >& syn_spec )
  : ConnBuilder( sources, targets, conn_spec, syn_spec )
{
}

void
nest::AuxiliaryBuilder::single_connect( size_t snode_id, Node& tgt, size_t tid, RngPtr rng )
{
  single_connect_( snode_id, tgt, tid, rng );
}


nest::TripartiteBernoulliWithPoolBuilder::TripartiteBernoulliWithPoolBuilder( NodeCollectionPTR sources,
  NodeCollectionPTR targets,
  NodeCollectionPTR third,
  const DictionaryDatum& conn_spec,
  const std::map< Name, std::vector< DictionaryDatum > >& syn_specs )
  : ConnBuilder( sources,
    targets,
    conn_spec,
    // const_cast here seems required, clang complains otherwise; try to clean up when Datums disappear
    const_cast< std::map< Name, std::vector< DictionaryDatum > >& >( syn_specs )[ names::primary ] )
  , third_( third )
  , third_in_builder_( sources,
      third,
      conn_spec,
      const_cast< std::map< Name, std::vector< DictionaryDatum > >& >( syn_specs )[ names::third_in ] )
  , third_out_builder_( third,
      targets,
      conn_spec,
      const_cast< std::map< Name, std::vector< DictionaryDatum > >& >( syn_specs )[ names::third_out ] )
  , p_primary_( 1.0 )
  , p_third_if_primary_( 1.0 )
  , random_pool_( true )
  , pool_size_( third->size() )
  , targets_per_third_( targets->size() / third->size() )
{
  updateValue< double >( conn_spec, names::p_primary, p_primary_ );
  updateValue< double >( conn_spec, names::p_third_if_primary, p_third_if_primary_ );
  updateValue< long >( conn_spec, names::pool_size, pool_size_ );
  std::string pool_type;
  if ( updateValue< std::string >( conn_spec, names::pool_type, pool_type ) )
  {
    if ( pool_type == "random" )
    {
      random_pool_ = true;
    }
    else if ( pool_type == "block" )
    {
      random_pool_ = false;
    }
    else
    {
      throw BadProperty( "pool_type must be 'random' or 'block'" );
    }
  }

  if ( p_primary_ < 0 or 1 < p_primary_ )
  {
    throw BadProperty( "Probability of primary connection 0 ≤ p_primary ≤ 1 required" );
  }

  if ( p_third_if_primary_ < 0 or 1 < p_third_if_primary_ )
  {
    throw BadProperty( "Conditional probability of third-factor connection 0 ≤ p_third_if_primary ≤ 1 required" );
  }

  if ( pool_size_ < 1 or third->size() < pool_size_ )
  {
    throw BadProperty( "Pool size 1 ≤ pool_size ≤ size of third-factor population required" );
  }

  if ( not( random_pool_ or ( targets->size() * pool_size_ == third->size() )
         or ( pool_size_ == 1 and targets->size() % third->size() == 0 ) ) )
  {
    throw BadProperty(
      "The sizes of target and third-factor populations and the chosen pool size do not fit."
      " If pool_size == 1, the target population size must be a multiple of the third-factor"
      " population size. For pool_size > 1, size(targets) * pool_size == size(third factor)"
      " is required. For all other cases, use random pools." );
  }
}

size_t
nest::TripartiteBernoulliWithPoolBuilder::get_first_pool_index_( const size_t target_index ) const
{
  if ( pool_size_ > 1 )
  {
    return target_index * pool_size_;
  }

  return target_index / targets_per_third_; // intentional integer division
}

void
nest::TripartiteBernoulliWithPoolBuilder::connect_()
{
#pragma omp parallel
  {
    const size_t tid = kernel().vp_manager.get_thread_id();

    try
    {
      /* Random number generators:
       * - Use RNG generating same number sequence on all threads to decide which connections to create
       * - Use per-thread random number generator to randomize connection properties
       */
      RngPtr synced_rng = get_vp_synced_rng( tid );
      RngPtr rng = get_vp_specific_rng( tid );

      binomial_distribution bino_dist;
      binomial_distribution::param_type bino_param( sources_->size(), p_primary_ );

      // Iterate through target neurons. For each, three steps are done:
      // 1. draw indegree 2. select astrocyte pool 3. make connections
      for ( const auto& target : *targets_ )
      {
        const size_t tnode_id = target.node_id;
        Node* target_node = kernel().node_manager.get_node_or_proxy( tnode_id, tid );
        const bool local_target = not target_node->is_proxy();

        // step 1, draw indegree for this target
        const auto indegree = bino_dist( synced_rng, bino_param );
        if ( indegree == 0 )
        {
          continue; // no connections for this target
        }

        // step 2, build pool for target
        std::vector< NodeIDTriple > pool;
        pool.reserve( pool_size_ );
        if ( random_pool_ )
        {
          synced_rng->sample( third_->begin(), third_->end(), std::back_inserter( pool ), pool_size_ );
        }
        else
        {
          std::copy_n( third_->begin() + get_first_pool_index_( target.lid ), pool_size_, std::back_inserter( pool ) );
        }

        // step 3, iterate through indegree to make connections for this target
        //   - by construction, we cannot get multapses
        //   - if the target is also among sources, it can be drawn at most once;
        //     we ignore it then connecting if no autapses are wanted
        std::vector< NodeIDTriple > sources_to_connect_;
        sources_to_connect_.reserve( indegree );
        synced_rng->sample( sources_->begin(), sources_->end(), std::back_inserter( sources_to_connect_ ), indegree );

        for ( const auto source : sources_to_connect_ )
        {
          const auto snode_id = source.node_id;
          if ( not allow_autapses_ and snode_id == tnode_id )
          {
            continue;
          }

          if ( local_target )
          {
            // plain connect now with thread-local rng for randomized parameters
            single_connect_( snode_id, *target_node, tid, rng );
          }

          // conditionally connect third factor
          if ( not( synced_rng->drand() < p_third_if_primary_ ) )
          {
            continue;
          }

          // select third-factor neuron randomly from pool for this target
          const auto third_index = pool_size_ == 1 ? 0 : synced_rng->ulrand( pool_size_ );
          const auto third_node_id = pool[ third_index ].node_id;
          Node* third_node = kernel().node_manager.get_node_or_proxy( third_node_id, tid );
          const bool local_third_node = not third_node->is_proxy();

          if ( local_third_node )
          {
            // route via auxiliary builder who handles parameters
            third_in_builder_.single_connect( snode_id, *third_node, tid, rng );
          }

          // connection third-factor node to target if local
          if ( local_target )
          {
            // route via auxiliary builder who handles parameters
            third_out_builder_.single_connect( third_node_id, *target_node, tid, rng );
          }
        }
      }
    }
    catch ( std::exception& err )
    {
      // We must create a new exception here, err's lifetime ends at
      // the end of the catch block.
      exceptions_raised_.at( tid ) = std::shared_ptr< WrappedThreadException >( new WrappedThreadException( err ) );
    }
  }
}


nest::SymmetricBernoulliBuilder::SymmetricBernoulliBuilder( NodeCollectionPTR sources,
  NodeCollectionPTR targets,
  const DictionaryDatum& conn_spec,
  const std::vector< DictionaryDatum >& syn_specs )
  : ConnBuilder( sources, targets, conn_spec, syn_specs )
  , p_( ( *conn_spec )[ names::p ] )
{
  // This connector takes care of symmetric connections on its own
  creates_symmetric_connections_ = true;

  if ( p_ < 0 or 1 <= p_ )
  {
    throw BadProperty( "Connection probability 0 <= p < 1 required." );
  }

  if ( not allow_multapses_ )
  {
    throw BadProperty( "Multapses must be enabled." );
  }

  if ( allow_autapses_ )
  {
    throw BadProperty( "Autapses must be disabled." );
  }

  if ( not make_symmetric_ )
  {
    throw BadProperty( "Symmetric connections must be enabled." );
  }
}


void
nest::SymmetricBernoulliBuilder::connect_()
{
#pragma omp parallel
  {
    const size_t tid = kernel().vp_manager.get_thread_id();

    // Use RNG generating same number sequence on all threads
    RngPtr synced_rng = get_vp_synced_rng( tid );

    try
    {
      binomial_distribution bino_dist;
      binomial_distribution::param_type param( sources_->size(), p_ );

      unsigned long indegree;
      size_t snode_id;
      std::set< size_t > previous_snode_ids;
      Node* target;
      size_t target_thread;
      Node* source;
      size_t source_thread;

      for ( NodeCollection::const_iterator tnode_id = targets_->begin(); tnode_id != targets_->end(); ++tnode_id )
      {
        // sample indegree according to truncated Binomial distribution
        indegree = sources_->size();
        while ( indegree >= sources_->size() )
        {
          indegree = bino_dist( synced_rng, param );
        }
        assert( indegree < sources_->size() );

        target = kernel().node_manager.get_node_or_proxy( ( *tnode_id ).node_id, tid );
        target_thread = tid;

        // check whether the target is on this thread
        if ( target->is_proxy() )
        {
          target_thread = invalid_thread;
        }

        previous_snode_ids.clear();

        // choose indegree number of sources randomly from all sources
        size_t i = 0;
        while ( i < indegree )
        {
          snode_id = ( *sources_ )[ synced_rng->ulrand( sources_->size() ) ];

          // Avoid autapses and multapses. Due to symmetric connectivity,
          // multapses might exist if the target neuron with node ID snode_id draws the
          // source with node ID tnode_id while choosing sources itself.
          if ( snode_id == ( *tnode_id ).node_id or previous_snode_ids.find( snode_id ) != previous_snode_ids.end() )
          {
            continue;
          }
          previous_snode_ids.insert( snode_id );

          source = kernel().node_manager.get_node_or_proxy( snode_id, tid );
          source_thread = tid;

          if ( source->is_proxy() )
          {
            source_thread = invalid_thread;
          }

          // if target is local: connect
          if ( target_thread == tid )
          {
            assert( target );
            single_connect_( snode_id, *target, target_thread, synced_rng );
          }

          // if source is local: connect
          if ( source_thread == tid )
          {
            assert( source );
            single_connect_( ( *tnode_id ).node_id, *source, source_thread, synced_rng );
          }

          ++i;
        }
      }
    }
    catch ( std::exception& err )
    {
      // We must create a new exception here, err's lifetime ends at
      // the end of the catch block.
      exceptions_raised_.at( tid ) = std::shared_ptr< WrappedThreadException >( new WrappedThreadException( err ) );
    }
  }
}


nest::SPBuilder::SPBuilder( NodeCollectionPTR sources,
  NodeCollectionPTR targets,
  const DictionaryDatum& conn_spec,
  const std::vector< DictionaryDatum >& syn_specs )
  : ConnBuilder( sources, targets, conn_spec, syn_specs )
{
  // Check that both pre and postsynaptic element are provided
  if ( not use_structural_plasticity_ )
  {
    throw BadProperty( "pre_synaptic_element and/or post_synaptic_elements is missing." );
  }
}

void
nest::SPBuilder::update_delay( long& d ) const
{
  if ( get_default_delay() )
  {
    DictionaryDatum syn_defaults = kernel().model_manager.get_connector_defaults( get_synapse_model() );
    const double delay = getValue< double >( syn_defaults, "delay" );
    d = Time( Time::ms( delay ) ).get_steps();
  }
}

void
nest::SPBuilder::sp_connect( const std::vector< size_t >& sources, const std::vector< size_t >& targets )
{
  connect_( sources, targets );

  // check if any exceptions have been raised
  for ( size_t tid = 0; tid < kernel().vp_manager.get_num_threads(); ++tid )
  {
    if ( exceptions_raised_.at( tid ).get() )
    {
      throw WrappedThreadException( *( exceptions_raised_.at( tid ) ) );
    }
  }
}

void
nest::SPBuilder::connect_()
{
  throw NotImplemented( "Connection without structural plasticity is not possible for this connection builder." );
}

/**
 * In charge of dynamically creating the new synapses
 */
void
nest::SPBuilder::connect_( NodeCollectionPTR, NodeCollectionPTR )
{
  throw NotImplemented( "Connection without structural plasticity is not possible for this connection builder." );
}

void
nest::SPBuilder::connect_( const std::vector< size_t >& sources, const std::vector< size_t >& targets )
{
  // Code copied and adapted from OneToOneBuilder::connect_()
  // make sure that target and source population have the same size
  if ( sources.size() != targets.size() )
  {
    throw DimensionMismatch( "Source and target population must be of the same size." );
  }

#pragma omp parallel
  {
    // get thread id
    const size_t tid = kernel().vp_manager.get_thread_id();

    try
    {
      RngPtr rng = get_vp_specific_rng( tid );

      std::vector< size_t >::const_iterator tnode_id_it = targets.begin();
      std::vector< size_t >::const_iterator snode_id_it = sources.begin();
      for ( ; tnode_id_it != targets.end(); ++tnode_id_it, ++snode_id_it )
      {
        assert( snode_id_it != sources.end() );

        if ( *snode_id_it == *tnode_id_it and not allow_autapses_ )
        {
          continue;
        }

        if ( not change_connected_synaptic_elements( *snode_id_it, *tnode_id_it, tid, 1 ) )
        {
          skip_conn_parameter_( tid );
          continue;
        }
        Node* const target = kernel().node_manager.get_node_or_proxy( *tnode_id_it, tid );

        single_connect_( *snode_id_it, *target, tid, rng );
      }
    }
    catch ( std::exception& err )
    {
      // We must create a new exception here, err's lifetime ends at
      // the end of the catch block.
      exceptions_raised_.at( tid ) = std::shared_ptr< WrappedThreadException >( new WrappedThreadException( err ) );
    }
  }
}
