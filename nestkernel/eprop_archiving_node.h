/*
 *  eprop_archiving_node.h
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

#ifndef EPROP_ARCHIVING_NODE_H
#define EPROP_ARCHIVING_NODE_H

// C++
#include <deque>

// nestkernel
#include "archiving_node.h"
#include "histentry.h"
#include "nest_time.h"
#include "nest_types.h"
#include "synaptic_element.h"

// sli
#include "dictdatum.h"

/**
 * A node which archives the spike history and the history of the
 * dynamic variables associated with e-prop plasticity.
 */
namespace nest
{

class EpropArchivingNode : public ArchivingNode
{

public:
  EpropArchivingNode();
  EpropArchivingNode( const EpropArchivingNode& );

  void init_update_history( double delay );

protected:
  double calculate_v_m_pseudo_deriv( double V_m, double V_th, double V_th_const ) const;

  void erase_unneeded_update_history();
  void write_error_signal_to_eprop_history( long time_step, double error_signal );
  void write_learning_signal_to_eprop_history( LearningSignalConnectionEvent& e );

  void write_update_to_history( double t_last_update, double t_current_update );

  void write_spike_history( long time_step );


private:
  double eps_ = 1e-6;

  std::deque< histentry_eprop_archive > eprop_history_;
  std::vector< histentry_eprop_update > update_history_;
};

} // namespace nest
#endif // EPROP_ARCHIVING_NODE_H
