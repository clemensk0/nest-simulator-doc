/*
 *  genericmodel.h
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

#ifndef GENERICMODEL_H
#define GENERICMODEL_H

// C++ includes:
#include <new>

// Includes from nestkernel:
#include "model.h"

namespace nest
{
/**
 * Generic Model template.
 *
 * The template GenericModel should be used
 * as base class for custom model classes. It already includes the
 * element factory functionality, as well as a pool based memory
 * manager, so that the user can concentrate on the "real" model
 * aspects.
 * @ingroup user_interface
 */
template < typename ElementT >
class GenericModel : public Model
{
public:
  GenericModel( const std::string&, const std::string& deprecation_info );

  /**
   * Create copy of model with new name.
   */
  GenericModel( const GenericModel&, const std::string& );

  /**
   * Return pointer to cloned model with same name.
   */
  Model* clone( const std::string& ) const override;

  bool has_proxies() override;
  bool one_node_per_process() override;
  bool is_off_grid() override;
  void calibrate_time( const TimeConverter& tc ) override;

  /**
   * Send a test event to a target node.
   *
   * This is a forwarding function that calls Node::send_test_event() from the prototype.
   * Since proxies know the model they represent, they can now answer a call to check
   * connection by referring back to the model.
   */
  size_t send_test_event( Node&, size_t, synindex, bool ) override;

  void sends_secondary_event( GapJunctionEvent& ge ) override;

  SignalType sends_signal() const override;

  void sends_secondary_event( InstantaneousRateConnectionEvent& re ) override;

  void sends_secondary_event( DiffusionConnectionEvent& de ) override;

  void sends_secondary_event( DelayedRateConnectionEvent& re ) override;

  void sends_secondary_event( SICEvent& sic ) override;

  Node const& get_prototype() const override;

  void set_model_id( int ) override;

  int get_model_id() override;

  void deprecation_warning( const std::string& ) override;

private:
  void set_status_( DictionaryDatum ) override;
  DictionaryDatum get_status_() override;

  size_t get_element_size() const override;

  /**
   * Call placement new on the supplied memory position.
   */
  Node* create_() override;

  /**
   * Prototype node from which all instances are constructed.
   */
  ElementT proto_;

  /**
   * String containing deprecation information; empty if model not deprecated.
   */
  std::string deprecation_info_;

  //! False until deprecation warning has been issued once
  bool deprecation_warning_issued_;
};

template < typename ElementT >
GenericModel< ElementT >::GenericModel( const std::string& name, const std::string& deprecation_info )
  : Model( name )
  , proto_()
  , deprecation_info_( deprecation_info )
  , deprecation_warning_issued_( false )
{
  set_threads();
}

template < typename ElementT >
GenericModel< ElementT >::GenericModel( const GenericModel& oldmod, const std::string& newname )
  : Model( newname )
  , proto_( oldmod.proto_ )
  , deprecation_info_( oldmod.deprecation_info_ )
  , deprecation_warning_issued_( false )
{
  set_type_id( oldmod.get_type_id() );
  set_threads();
}

template < typename ElementT >
Model*
GenericModel< ElementT >::clone( const std::string& newname ) const
{
  return new GenericModel( *this, newname );
}

template < typename ElementT >
Node*
GenericModel< ElementT >::create_()
{
  Node* n = new ElementT( proto_ );
  return n;
}

template < typename ElementT >
inline bool
GenericModel< ElementT >::has_proxies()
{
  return proto_.has_proxies();
}

template < typename ElementT >
inline bool
GenericModel< ElementT >::one_node_per_process()
{
  return proto_.one_node_per_process();
}

template < typename ElementT >
inline bool
GenericModel< ElementT >::is_off_grid()
{
  return proto_.is_off_grid();
}

template < typename ElementT >
inline void
GenericModel< ElementT >::calibrate_time( const TimeConverter& tc )
{
  proto_.calibrate_time( tc );
}

template < typename ElementT >
inline size_t
GenericModel< ElementT >::send_test_event( Node& target, size_t receptor, synindex syn_id, bool dummy_target )
{
  return proto_.send_test_event( target, receptor, syn_id, dummy_target );
}

template < typename ElementT >
inline void
GenericModel< ElementT >::sends_secondary_event( GapJunctionEvent& ge )
{
  return proto_.sends_secondary_event( ge );
}

template < typename ElementT >
inline void
GenericModel< ElementT >::sends_secondary_event( InstantaneousRateConnectionEvent& re )
{
  return proto_.sends_secondary_event( re );
}

template < typename ElementT >
inline void
GenericModel< ElementT >::sends_secondary_event( DiffusionConnectionEvent& de )
{
  return proto_.sends_secondary_event( de );
}

template < typename ElementT >
inline void
GenericModel< ElementT >::sends_secondary_event( DelayedRateConnectionEvent& re )
{
  return proto_.sends_secondary_event( re );
}

template < typename ElementT >
inline void
GenericModel< ElementT >::sends_secondary_event( SICEvent& sic )
{
  return proto_.sends_secondary_event( sic );
}

template < typename ElementT >
inline nest::SignalType
GenericModel< ElementT >::sends_signal() const
{
  return proto_.sends_signal();
}

template < typename ElementT >
void
GenericModel< ElementT >::set_status_( DictionaryDatum d )
{
  proto_.set_status( d );
}

template < typename ElementT >
DictionaryDatum
GenericModel< ElementT >::get_status_()
{
  DictionaryDatum d = proto_.get_status_base();
  ( *d )[ names::elementsize ] = sizeof( ElementT );
  return d;
}

template < typename ElementT >
size_t
GenericModel< ElementT >::get_element_size() const
{
  return sizeof( ElementT );
}

template < typename ElementT >
Node const&
GenericModel< ElementT >::get_prototype() const
{
  return proto_;
}

template < typename ElementT >
void
GenericModel< ElementT >::set_model_id( int i )
{
  proto_.set_model_id( i );
}

template < typename ElementT >
int
GenericModel< ElementT >::get_model_id()
{
  return proto_.get_model_id();
}
}

#endif
