# -*- coding: utf-8 -*-
#
# hl_api_info.py
#
# This file is part of NEST.
#
# Copyright (C) 2004 The NEST Initiative
#
# NEST is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# NEST is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with NEST.  If not, see <http://www.gnu.org/licenses/>.

"""
Functions to get information on NEST.
"""

from .hl_api_helper import *
import nest


@check_stack
def sysinfo():
    """Print information on the platform on which NEST was compiled."""

    sr("sysinfo")


@check_stack
def version():
    """Return the NEST version.

    Returns
    -------
    str:
        The version of NEST.
    """

    sr("statusdict [[ /kernelname /version ]] get")
    return " ".join(spp())


@check_stack
def authors():
    """Print the authors of NEST."""

    sr("authors")


@check_stack
def helpdesk(browser="firefox"):
    """Open the NEST helpdesk in the given browser.

    The default browser is firefox.

    Parameters
    ----------
    browser : str, optional
        Name of the browser to use
    """

    sr("/helpdesk << /command (%s) >> SetOptions" % browser)
    sr("helpdesk")


@check_stack
def help(obj=None, pager="less"):
    """Show the help page for the given object using the given pager.

    The default pager is less.

    Parameters
    ----------
    obj : object, optional
        Object to display help for
    pager : str, optional
        Pager to use
    """

    if obj is not None:
        sr("/page << /command (%s) >> SetOptions" % pager)
        sr("/%s help" % obj)
    else:
        print("Type 'nest.helpdesk()' to access the online documentation "
              "in a browser.")
        print("Type 'nest.help(object)' to get help on a NEST object or "
              "command.\n")
        print("Type 'nest.Models()' to see a list of available models "
              "in NEST.\n")
        print("Type 'nest.authors()' for information about the makers "
              "of NEST.")
        print("Type 'nest.sysinfo()' to see details on the system "
              "configuration.")
        print("Type 'nest.version()' for information about the NEST "
              "version.\n")
        print("For more information visit http://www.nest-simulator.org.")


@check_stack
def get_verbosity():
    """Return verbosity level of NEST's messages.

    Returns
    -------
    int:
        The current verbosity level
    """

    sr('verbosity')
    return spp()


@check_stack
def set_verbosity(level):
    """Change verbosity level for NEST's messages.

    Parameters
    ----------
    level : str
        Can be one of 'M_FATAL', 'M_ERROR', 'M_WARNING', or 'M_INFO'.
    """

    sr("%s setverbosity" % level)


@check_stack
def get_argv():
    """Return argv as seen by NEST.

    This is similar to Python sys.argv but might have changed after
    MPI initialization.

    Returns
    -------
    tuple:
        Argv, as seen by NEST.
    """
    sr('statusdict')
    statusdict = spp()
    return statusdict['argv']


@check_stack
def message(level, sender, text):
    """Print a message using NEST's message system.

    Parameters
    ----------
    level :
        Level
    sender :
        Message sender
    text : str
        Text to be sent in the message
    """

    sps(level)
    sps(sender)
    sps(text)
    sr('message')


@check_stack
def SetStatus(nodes, params, val=None):
    """Set the parameters of nodes or connections to params.

    If val is given, params has to be the name
    of an attribute, which is set to val on the nodes/connections. val
    can be a single value or a list of the same size as nodes.

    Parameters
    ----------
    nodes : GIDCollection or tuple
        Either a GIDCollection representing nodes, or a tuple of connection
        handles as returned by GetConnections()
    params : str or dict or list
        Dictionary of parameters or list of dictionaries of parameters of
        same length as nodes. If val is given, this has to be the name of
        a model property as a str.
    val : str, optional
        If given, params has to be the name of a model property.

    Raises
    ------
    TypeError
        Description
    """

   # if not (isinstance(nodes, nest.GIDCollection) or isinstance(nodes, tuple)):
  #      raise TypeError("The first input (nodes) must be a GIDCollection or \
   #                      a tuple of connection handles ")

    # This was added to ensure that the function is a nop (instead of,
    # for instance, raising an exception) when applied to an empty list,
    # which is an artifact of the API operating on lists, rather than
    # relying on language idioms, such as comprehensions
    #
    if len(nodes) == 0:
        return

    if val is not None and is_literal(params):
        if is_iterable(val) and not isinstance(val, (uni_str, dict)):
            params = [{params: x} for x in val]
        else:
            params = {params: val}

    print("her!")
    params = broadcast(params, len(nodes), (dict,), "params")
    if len(nodes) != len(params):
        print("her! i if")
        raise TypeError(
            "status dict must be a dict, or list of dicts of length 1 "
            "or len(nodes)")

    if is_sequence_of_connections(nodes):
        print("her! i if2")
        pcd(nodes)
    else:
        print("her! i else")
        sps(nodes)

    sps(params)
    print("her!")
    sr('2 arraystore')
    print("her!")
    sr('Transpose { arrayload pop SetStatus } forall')
    print("her!")


@check_stack
def GetStatus(nodes, keys=None):
    """Return the parameter dictionaries of nodes or connections.

    If keys is given, a list of values is returned instead. keys may also be a
    list, in which case the returned list contains lists of values.

    Parameters
    ----------
    nodes : GIDCollection or tuple
        Either a GIDCollection representing nodes, or a tuple of connection
        handles as returned by GetConnections()
    keys : str or list, optional
        String or a list of strings naming model properties. GetDefaults then
        returns a single value or a list of values belonging to the keys
        given.

    Returns
    -------
    dict:
        All parameters
    type:
        If keys is a string, the corrsponding default parameter is returned
    list:
        If keys is a list of strings, a list of corrsponding default parameters
        is returned

    Raises
    ------
    TypeError
        Description
    """

    if not (isinstance(nodes, nest.GIDCollection) or isinstance(nodes, tuple)):
        raise TypeError("The first input (nodes) must be a GIDCollection or \
                         a tuple of connection handles ")

    if len(nodes) == 0:
        return nodes

    if keys is None:
        cmd = '{ GetStatus } Map'
    elif is_literal(keys):
        cmd = '{{ GetStatus /{0} get }} Map'.format(keys)
    elif is_iterable(keys):
        keys_str = " ".join("/{0}".format(x) for x in keys)
        cmd = '{{ GetStatus }} Map {{ [ [ {0} ] ] get }} Map'.format(keys_str)
    else:
        raise TypeError("keys should be either a string or an iterable")

    if is_sequence_of_connections(nodes):
        pcd(nodes)
    else:
        sps(nodes)

    sr(cmd)

    return spp()
