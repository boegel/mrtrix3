/*
    Copyright 2008 Brain Research Institute, Melbourne, Australia

    Written by David Raffelt

    This file is part of MRtrix.

    MRtrix is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    MRtrix is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with MRtrix.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "command.h"
#include "progressbar.h"
#include "datatype.h"
#include "dwi/tractography/scalar_file.h"

using namespace MR;
using namespace App;

void usage ()
{
  DESCRIPTION
  + "compare two track scalar files for differences, within specified tolerance. ";

  ARGUMENTS
  + Argument ("tsf1", "a track scalar file.").type_file_in ()
  + Argument ("tsg2", "another track scalar file.").type_file_in ()
  + Argument ("tolerance", "the maximum distance to consider acceptable").type_float (0.0);
}

typedef float value_type;

void run ()
{
  float tol = argument[2];

  DWI::Tractography::Properties properties1, properties2;
  DWI::Tractography::ScalarReader<value_type> reader1 (argument[0], properties1);
  DWI::Tractography::ScalarReader<value_type> reader2 (argument[1], properties2);

  DWI::Tractography::check_properties_match (properties1, properties2, "scalar", false);

  std::vector<value_type> tck_scalar1;
  std::vector<value_type> tck_scalar2;
  while (reader1 (tck_scalar1)) {
    if (!reader2 (tck_scalar2))
      break;
    if (tck_scalar1.size() != tck_scalar2.size())
      throw Exception ("track scalar length mismatch - test FAILED");

    for (size_t i = 0; i < tck_scalar1.size(); ++i) {
      if (std::abs (tck_scalar1[i] - tck_scalar2[i]) > tol)
        throw Exception ("track scalar files \"" + str(argument[0]) + "\" and \"" + str(argument[1]) + "\" do not match within specified precision of " + str(tol)
            + " (" + str(cdouble (tck_scalar1[i])) + " vs " + str(cdouble (tck_scalar2[i])) + ")");
    }
  }
  CONSOLE ("track scalar files checked OK");
}

