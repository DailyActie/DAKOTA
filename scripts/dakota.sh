#!/bin/sh

#    _______________________________________________________________________
#
#    DAKOTA: Design Analysis Kit for Optimization and Terascale Applications
#    Copyright (c) 2010, Sandia National Laboratories.
#    This software is distributed under the GNU Lesser General Public License.
#    For more information, see the README file in the top Dakota directory.
#    _______________________________________________________________________

# Wrapper script dakota.sh to help manage binary and library paths
# when running dakota.  Assume dakota is installed alongside this
# script and libraries are in the same directory and/or ../lib.

script_name=`basename ${0}`

# get the path to this wrapper script
# assume DAKOTA lives in same directory
execpath=`dirname ${0}`

if [ ! -e "${execpath}/dakota" ]; then
  echo "Error in ${script_name}"
  echo "  Could not find dakota binary in ${execpath}"
  exit 1
elif [ ! -x "${execpath}/dakota" ]; then
  echo "Error in ${script_name}"
  echo "  dakota binary in ${execpath} is not executable."
  exit 1
fi

libpaths="${execpath}:${execpath}/../lib"

#echo "Prepending library path with ${libpaths}"
if [ `uname` = "Darwin" ]; then 
  DYLD_LIBRARY_PATH="${libpaths}:${DYLD_LIBRARY_PATH}"
  export DYLD_LIBRARY_PATH
else
  LD_LIBRARY_PATH="${libpaths}:${LD_LIBRARY_PATH}"
  export LD_LIBRARY_PATH
fi

#echo "Appending PATH with ${execpath}:${execpath}/../test:."
PATH="$PATH:${execpath}:${execpath}/../test:."
export PATH

#echo "Launching ${execpath}/dakota with args: $@"
"${execpath}/dakota" "$@"
