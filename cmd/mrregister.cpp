/*
 Copyright 2012 Brain Research Institute, Melbourne, Australia

 Written by David Raffelt, 23/02/2012.

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
//#define REGISTRATION_GRADIENT_DESCENT_DEBUG

#include "command.h"
#include "image.h"
#include "filter/reslice.h"
#include "interp/cubic.h"
#include "transform.h"
#include "registration/linear.h"
#include "registration/syn.h"
#include "registration/metric/syn_demons.h"
#include "registration/metric/mean_squared.h"
#include "registration/metric/difference_robust.h"
#include "registration/metric/difference_robust_4D.h"
#include "registration/metric/cross_correlation.h"
#include "registration/metric/mean_squared_4D.h"
#include "registration/transform/affine.h"
#include "registration/transform/rigid.h"
#include "dwi/directions/predefined.h"
#include "math/SH.h"
#include "image/average_space.h"


using namespace MR;
using namespace App;

const char* transformation_choices[] = { "rigid", "affine", "syn", "rigid_affine", "rigid_syn", "affine_syn", "rigid_affine_syn", NULL };


void usage ()
{
  AUTHOR = "David Raffelt (david.raffelt@florey.edu.au) & Max Pietsch (maximilian.pietsch@kcl.ac.uk)";

  DESCRIPTION
      + "Register two images together using a rigid, affine or a symmetric diffeomorphic (SyN) transformation model."

      + "By default this application will perform an affine, followed by SyN registration."

      + "FOD registration (with apodised point spread reorientation) will be performed by default if the number of volumes "
        "in the 4th dimension equals the number of coefficients in an antipodally symmetric spherical harmonic series (e.g. 6, 15, 28 etc). "
        "The -no_reorientation option can be used to force reorientation off if required."

      + "SyN estimates both the warp and it's inverse. These are each split into two warps to achieve a symmetric transformation (i.e "
        "both the moving and template image are warped towards a 'middle ground'. See Avants (2008) Med Image Anal. 12(1): 26–41.) "
        "By default this application will save all four warps (so that subsequent registrations can be initialised with the output warps) "
        "Warps are saved in a single 5D file, with the 5th dimension defining the warp type. (These can be visualised by switching volume "
        "groups in MRview)."

      + "By default the affine transformation will be saved in the warp image header (use mrinfo to view). To save the affine transform "
        "separately as a text file, use the -affine option.";


  ARGUMENTS
    + Argument ("image1", "input image 1 ('moving')").type_image_in ()
    + Argument ("image2", "input image 2 ('template')").type_image_in ();


  OPTIONS
  + Option ("type", "the registration type. Valid choices are: "
                             "rigid, affine, syn, rigid_affine, rigid_syn, affine_syn, rigid_affine_syn (Default: affine_syn)")
    + Argument ("choice").type_choice (transformation_choices)

  + Option ("transformed", "image1 after registration transformed to the space of image2")
    + Argument ("image").type_image_out ()

  + Option ("transformed_midway", "image1 and image2 after registration transformed "
        "to the midway space")
    + Argument ("image1_transformed").type_image_out ()
    + Argument ("image2_transformed").type_image_out ()

  + Option ("mask1", "a mask to define the region of image1 to use for optimisation.")
    + Argument ("filename").type_image_in ()

  + Option ("mask2", "a mask to define the region of image2 to use for optimisation.")
    + Argument ("filename").type_image_in ()

  + Registration::rigid_options

  + Registration::affine_options

  + Registration::syn_options

  + Registration::fod_options

  + DataType::options();
}

typedef double value_type;

void load_image (const std::string filename, const size_t num_vols, Image<value_type>& image) {
  auto header = Header::open (filename);
  if (num_vols > 1 && num_vols != (size_t)header.size(3)) {
    auto temp_image = Image<value_type>::open (filename).with_direct_io (Stride::contiguous_along_axis (3));
    header.datatype() = DataType::from_command_line (DataType::Float32);
    header.size(3) = num_vols;
    header.stride(0) = 2;
    header.stride(1) = 3;
    header.stride(2) = 4;
    header.stride(3) = 1;
    image = Image<value_type>::scratch (header);
    for (auto i = Loop (image)(image, temp_image); i; ++i)
      image.value() = temp_image.value();
  } else {
    image = Image<value_type>::open (filename).with_direct_io (Stride::contiguous_along_axis (3));
  }
}



void run ()
{
  #ifdef REGISTRATION_GRADIENT_DESCENT_DEBUG
    std::remove("/tmp/gddebug/log.txt");
  #endif
  const auto im1_header = Header::open (argument[0]);
  const auto im2_header = Header::open (argument[1]);

  if (im1_header.ndim() != im2_header.ndim())
    throw Exception ("input images do not have the same number of dimensions");

  Image<value_type> im1_image;
  Image<value_type> im2_image;

  bool do_reorientation = true;
  auto opt = get_options ("noreorientation");
  if (opt.size())
    do_reorientation = false;
  Eigen::MatrixXd directions_cartesian;

  if (im2_header.ndim() > 4) {
    throw Exception ("image dimensions larger than 4 are not supported");
  }
  else if (im2_header.ndim() == 4) {
    if (im1_header.size(3) != im2_header.size(3))
      throw Exception ("input images do not have the same number of volumes in the 4th dimension");

    value_type val = (std::sqrt (float (1 + 8 * im2_header.size(3))) - 3.0) / 4.0;
    if (!(val - (int)val) && do_reorientation && im2_header.size(3) > 1) {
        CONSOLE ("SH series detected, performing FOD registration");
        // Only load as many SH coefficients as required
        int lmax = 4;
        if (Math::SH::LforN(im2_header.size(3)) < 4)
          lmax = Math::SH::LforN(im2_header.size(3));
        auto opt = get_options ("lmax");
        if (opt.size()) {
          lmax = opt[0][0];
          if (lmax % 2)
            throw Exception ("the input lmax must be even");
        }
        INFO ("registering FOD images using lmax = " + str(lmax));
        int num_SH = Math::SH::NforL (lmax);
        if (num_SH > im2_header.size(3))
            throw Exception ("not enough SH coefficients within input image for desired lmax");
        load_image (argument[0], num_SH, im1_image);
        load_image (argument[1], num_SH, im2_image);

        opt = get_options ("directions");
        if (opt.size())
          directions_cartesian = Math::SH::spherical2cartesian (load_matrix (opt[0][0])).transpose();
        else
          directions_cartesian = Math::SH::spherical2cartesian (DWI::Directions::electrostatic_repulsion_60()).transpose();
    }
    else {
      do_reorientation = false;
      load_image (argument[0], im1_header.size(3), im1_image);
      load_image (argument[1], im2_header.size(3), im2_image);
    }
  }
  else {
    do_reorientation = false;
    load_image (argument[0], 1, im1_image);
    load_image (argument[1], 1, im2_image);
  }




  // Will currently output whatever lmax was used during registration TODO
  opt = get_options ("transformed");
  Image<value_type> im1_transformed;
  if (opt.size()){
    Header transformed_header (im2_header);
    transformed_header.datatype() = DataType::from_command_line (DataType::Float32);
    im1_transformed = Image<value_type>::create (opt[0][0], transformed_header);
  }

  std::string im1_midway_transformed_path;
  std::string im2_midway_transformed_path;
  opt = get_options ("transformed_midway");
  if (opt.size()){
    im1_midway_transformed_path = str(opt[0][0]);
    im2_midway_transformed_path = str(opt[0][1]);
  }

  opt = get_options ("type");
  bool do_rigid  = false;
  bool do_affine = false;
  bool do_syn = false;
  int registration_type = 5;
  if (opt.size())
    registration_type = opt[0][0];
  switch (registration_type) {
    case 0:
      do_rigid = true;
      break;
    case 1:
      do_affine = true;
      break;
    case 2:
      do_syn = true;
      break;
    case 3:
      do_rigid = true;
      do_affine = true;
      break;
    case 4:
      do_rigid = true;
      do_syn = true;
      break;
    case 5:
      do_affine = true;
      do_syn = true;
      break;
    case 6:
      do_rigid = true;
      do_affine = true;
      do_syn = true;
      break;
    default:
      break;
  }

  opt = get_options ("mask2");
  Image<value_type> im2_mask;
  if (opt.size ())
    im2_mask = Image<value_type>::open(opt[0][0]);

  opt = get_options ("mask1");
  Image<value_type> im1_mask;
  if (opt.size ())
    im1_mask = Image<value_type>::open(opt[0][0]);



  // ****** RIGID REGISTRATION OPTIONS *******
  Registration::Linear rigid_registration;

  opt = get_options ("rigid");
  bool output_rigid = false;
  std::string rigid_filename;
  if (opt.size()) {
    if (!do_rigid)
      throw Exception ("rigid transformation output requested when no rigid registration is requested");
    output_rigid = true;
    rigid_filename = std::string (opt[0][0]);
  }

  Registration::Transform::Rigid rigid;
  opt = get_options ("rigid_init");
  bool init_rigid_set = false;
  if (opt.size()) {
    init_rigid_set = true;
    transform_type rigid_transform = load_transform (opt[0][0]);
    rigid.set_transform (rigid_transform);
    rigid_registration.set_init_type (Registration::Transform::Init::none);
  }

  opt = get_options ("rigid_centre");
  if (opt.size()) {
    if (init_rigid_set)
      throw Exception ("options -rigid_init and -rigid_centre are mutually exclusive");
    switch ((int)opt[0][0]){
      case 0:
        rigid_registration.set_init_type (Registration::Transform::Init::mass);
        break;
      case 1:
        rigid_registration.set_init_type (Registration::Transform::Init::geometric);
        break;
      case 2:
        rigid_registration.set_init_type (Registration::Transform::Init::moments);
        break;
      case 3:
        rigid_registration.set_init_type (Registration::Transform::Init::none);
        break;
      default:
        break;
    }
  }

  opt = get_options ("rigid_scale");
  if (opt.size ()) {
    if (!do_rigid)
      throw Exception ("the rigid multi-resolution scale factors were input when no rigid registration is requested");
    rigid_registration.set_scale_factor (parse_floats (opt[0][0]));
  }

  opt = get_options ("rigid_niter");
  if (opt.size ()) {
    if (!do_rigid)
      throw Exception ("the number of rigid iterations have been input when no rigid registration is requested");
    rigid_registration.set_max_iter (parse_ints (opt[0][0]));
  }

  opt = get_options ("rigid_metric");
  Registration::LinearMetricType rigid_metric = Registration::Diff;
  if (opt.size()) {
    switch ((int)opt[0][0]){
      case 0:
        rigid_metric = Registration::Diff;
        break;
      case 1:
        rigid_metric = Registration::NCC;
        break;
      default:
        break;
    }
  }

  opt = get_options ("rigid_global_search");
  if (opt.size())
      rigid_registration.use_global_search(true);

  // ****** AFFINE REGISTRATION OPTIONS *******
  Registration::Linear affine_registration;

  opt = get_options ("affine");
  bool output_affine = false;
  std::string affine_filename;
  if (opt.size()) {
   if (!do_affine)
     throw Exception ("affine transformation output requested when no affine registration is requested");
   output_affine = true;
   affine_filename = std::string (opt[0][0]);
  }

  opt = get_options ("affine_1tomidway");
  bool output_affine_1tomid = false;
  std::string affine_1tomid_filename;
  if (opt.size()) {
   if (!do_affine)
     throw Exception ("midway affine transformation output requested when no affine registration is requested");
   output_affine_1tomid = true;
   affine_1tomid_filename = std::string (opt[0][0]);
  }

  opt = get_options ("affine_2tomidway");
  bool output_affine_2tomid = false;
  std::string affine_2tomid_filename;
  if (opt.size()) {
   if (!do_affine)
     throw Exception ("midway affine transformation output requested when no affine registration is requested");
   output_affine_2tomid = true;
   affine_2tomid_filename = std::string (opt[0][0]);
  }

  Registration::Transform::Affine affine;
  opt = get_options ("affine_init");
  bool init_affine_set = false;
  if (opt.size()) {
    if (init_rigid_set)
      throw Exception ("you cannot initialise registrations with both a rigid and affine transformation");
    if (do_rigid)
      throw Exception ("you cannot initialise with -affine_init since a rigid registration is being performed");

    init_affine_set = true;
    transform_type init_affine = load_transform (opt[0][0]);
    affine.set_transform (init_affine);
    affine_registration.set_init_type (Registration::Transform::Init::none);
  }

  opt = get_options ("affine_centre");
  if (opt.size()) {
    if (init_affine_set)
      throw Exception ("options -affine_init and -affine_centre are mutually exclusive");
    switch ((int)opt[0][0]){
      case 0:
        affine_registration.set_init_type (Registration::Transform::Init::mass);
        break;
      case 1:
        affine_registration.set_init_type (Registration::Transform::Init::geometric);
        break;
      case 2:
        affine_registration.set_init_type (Registration::Transform::Init::moments);
        break;
      case 3:
        affine_registration.set_init_type (Registration::Transform::Init::none);
        break;
      default:
        break;
    }
  }

  opt = get_options ("affine_scale");
  if (opt.size ()) {
    if (!do_affine)
      throw Exception ("the affine multi-resolution scale factors were input when no affine registration is requested");
    affine_registration.set_scale_factor (parse_floats (opt[0][0]));
  }

  opt = get_options ("affine_repetitions");
  if (opt.size ()) {
    if (!do_affine)
      throw Exception ("the affine repetition factors were input when no affine registration is requested");
    affine_registration.set_gradient_descent_repetitions (parse_ints (opt[0][0]));
  }

  opt = get_options ("affine_loop_density");
  if (opt.size ()) {
    if (!do_affine)
      throw Exception ("the affine sparsity factor was input when no affine registration is requested");
    affine_registration.set_loop_density (parse_floats (opt[0][0]));
  }

  opt = get_options ("affine_metric");
  Registration::LinearMetricType affine_metric = Registration::Diff;
  if (opt.size()) {
    switch ((int)opt[0][0]){
      case 0:
        affine_metric = Registration::Diff;
        break;
      case 1:
        affine_metric = Registration::NCC;
        break;
      default:
        break;
    }
  }

  opt = get_options ("affine_robust_estimator");
  Registration::LinearRobustMetricEstimatorType affine_estimator = Registration::None;
  if (opt.size()) {
    switch ((int)opt[0][0]){
      case 0:
        affine_estimator = Registration::L1;
        break;
      case 1:
        affine_estimator = Registration::L2;
        break;
      case 2:
        affine_estimator = Registration::LP;
        break;
      default:
        break;
    }
  }

  affine_registration.use_robust_estimate (get_options ("affine_robust_median").size() == 1);

  opt = get_options ("affine_global_search");
  if (opt.size())
      affine_registration.use_global_search(true);

  opt = get_options ("affine_niter");
  if (opt.size ()) {
    if (!do_affine)
      throw Exception ("the number of affine iterations have been input when no affine registration is requested");
    affine_registration.set_max_iter (parse_ints (opt[0][0]));
  }


  // ****** SYN REGISTRATION OPTIONS *******
  Registration::SyN syn_registration;

  opt = get_options ("syn_warp");
  std::string warp_filename;
  if (opt.size()) {
    if (!do_syn)
      throw Exception ("Syn warp output requested when no SyN registration is requested");
    warp_filename = std::string (opt[0][0]);
  }


  opt = get_options ("syn_init");
  bool syn_init = false;
  if (opt.size()) {
    syn_init = true;

    if (!do_syn)
      throw Exception ("the syn initialisation input when no syn registration is requested");

    Image<default_type> input_warps = Image<default_type>::open (opt[0][0]);
    if (input_warps.ndim() != 5)
      throw Exception ("syn initialisation input is not 5D. Input must be from previous syn output");

    syn_registration.initialise (input_warps);

    if (do_affine) {
      WARN ("no affine registration will be performed when initialising with syn non-linear warps");
      do_affine = false;
    }
    if (do_rigid) {
      WARN ("no rigid registration will be performed when initialising with syn non-linear warps");
      do_rigid = false;
    }
    if (init_affine_set)
      WARN ("-affine_init has no effect since the syn init warp also contains the linear transform in the image header");
    if (init_rigid_set)
      WARN ("-rigid_init has no effect since the syn init warp also contains the linear transform in the image header");
  }


  opt = get_options ("syn_scale");
  if (opt.size ()) {
    if (!do_syn)
      throw Exception ("the syn multi-resolution scale factors were input when no syn registration is requested");
    std::vector<default_type> scale_factors = parse_floats (opt[0][0]);
    if (syn_init && (scale_factors.size() > 1)) {
      WARN ("-syn_scale option ignored since only the full resolution will be performed when initialising with syn warp");
    } else {
      syn_registration.set_scale_factor (scale_factors);
    }
  }

  opt = get_options ("syn_niter");
  if (opt.size ()) {
    if (!do_syn)
      throw Exception ("the number of syn iterations have been input when no SyN registration is requested");
    std::vector<int> iterations_per_level = parse_ints (opt[0][0]);
    if (syn_init && iterations_per_level.size() > 1)
      throw Exception ("when initialising the syn registration the max number of iterations can only be defined for a single level");
    else
      syn_registration.set_max_iter (iterations_per_level);
  }

  opt = get_options ("syn_update_smooth");
  if (opt.size()) {
    if (!do_syn)
      throw Exception ("the warp update field smoothing parameter was input when no SyN registration is requested");
    syn_registration.set_update_smoothing (opt[0][0]);
  }

  opt = get_options ("syn_disp_smooth");
  if (opt.size()) {
    if (!do_syn)
      throw Exception ("the displacement field smoothing parameter was input when no SyN registration is requested");
    syn_registration.set_disp_smoothing (opt[0][0]);
  }

  opt = get_options ("syn_grad_step");
  if (opt.size()) {
    if (!do_syn)
      throw Exception ("the initial gradient step size was input when no SyN registration is requested");
    syn_registration.set_init_grad_step (opt[0][0]);
  }


  // ****** RUN RIGID REGISTRATION *******
  if (do_rigid) {
    CONSOLE ("running rigid registration");

    if (im2_image.ndim() == 4) {
      if (rigid_metric == Registration::NCC)
        throw Exception ("cross correlation metric not implemented for data with more than 3 dimensions");
      Registration::Metric::MeanSquared4D<Image<value_type>, Image<value_type>> metric (im1_image, im2_image);
      rigid_registration.run_masked (metric, rigid, im1_image, im2_image, im1_mask, im2_mask);
    } else {
      if (rigid_metric == Registration::NCC) {
        std::vector<size_t> extent(3,3);
        rigid_registration.set_extent(extent);
        Registration::Metric::CrossCorrelation metric;
        rigid_registration.run_masked (metric, rigid, im1_image, im2_image, im1_mask, im2_mask);
      }
      else {
        Registration::Metric::MeanSquared metric;
        rigid_registration.run_masked (metric, rigid, im1_image, im2_image, im1_mask, im2_mask);
      }
    }

    if (output_rigid)
      save_transform (rigid.get_transform(), rigid_filename);
  }

  // ****** RUN AFFINE REGISTRATION *******
  if (do_affine) {
    CONSOLE ("running affine registration");

    if (do_rigid) {
      affine.set_centre (rigid.get_centre());
      affine.set_translation (rigid.get_translation());
      affine.set_matrix (rigid.get_matrix());
      affine_registration.set_init_type (Registration::Transform::Init::none);
    }

    if (do_reorientation)
      affine_registration.set_directions (directions_cartesian);

    if (im2_image.ndim() == 4) {
      if (affine_metric == Registration::NCC)
        throw Exception ("cross correlation metric not implemented for data with more than 3 dimensions");
      else if (affine_metric == Registration::Diff) {
        if (affine_estimator == Registration::None) {
          Registration::Metric::MeanSquared4D<Image<value_type>, Image<value_type>> metric(im1_image, im2_image);
          affine_registration.run_masked (metric, affine, im1_image, im2_image, im1_mask, im2_mask);
        } else if (affine_estimator == Registration::L1) {
          Registration::Metric::L1 estimator;
          Registration::Metric::DifferenceRobust4D<Image<value_type>, Image<value_type>, Registration::Metric::L1> metric(im1_image, im2_image, estimator);
          affine_registration.run_masked (metric, affine, im1_image, im2_image, im1_mask, im2_mask);
        } else if (affine_estimator == Registration::L2) {
          Registration::Metric::L2 estimator;
          Registration::Metric::DifferenceRobust4D<Image<value_type>, Image<value_type>, Registration::Metric::L2> metric(im1_image, im2_image, estimator);
          affine_registration.run_masked (metric, affine, im1_image, im2_image, im1_mask, im2_mask);
        } else if (affine_estimator == Registration::LP) {
          Registration::Metric::LP estimator;
          Registration::Metric::DifferenceRobust4D<Image<value_type>, Image<value_type>, Registration::Metric::LP> metric(im1_image, im2_image, estimator);
          affine_registration.run_masked (metric, affine, im1_image, im2_image, im1_mask, im2_mask);
        } else throw Exception ("FIXME: estimator selection");
      } else throw Exception ("FIXME: metric selection");
    } else { // 3D
      if (affine_metric == Registration::NCC){
        Registration::Metric::CrossCorrelation metric;
        std::vector<size_t> extent(3,3);
        affine_registration.set_extent (extent);
        affine_registration.run_masked (metric, affine, im1_image, im2_image, im1_mask, im2_mask);
      }
      else if (affine_metric == Registration::Diff) {
        if (affine_estimator == Registration::None) {
          Registration::Metric::MeanSquared metric;
          affine_registration.run_masked (metric, affine, im1_image, im2_image, im1_mask, im2_mask);
        } else if (affine_estimator == Registration::L1) {
          Registration::Metric::L1 estimator;
          Registration::Metric::DifferenceRobust<Registration::Metric::L1> metric(estimator);
          affine_registration.run_masked (metric, affine, im1_image, im2_image, im1_mask, im2_mask);
        } else if (affine_estimator == Registration::L2) {
          Registration::Metric::L2 estimator;
          Registration::Metric::DifferenceRobust<Registration::Metric::L2> metric(estimator);
          affine_registration.run_masked (metric, affine, im1_image, im2_image, im1_mask, im2_mask);
        } else if (affine_estimator == Registration::LP) {
          Registration::Metric::LP estimator;
          Registration::Metric::DifferenceRobust<Registration::Metric::LP> metric(estimator);
          affine_registration.run_masked (metric, affine, im1_image, im2_image, im1_mask, im2_mask);
        } else throw Exception ("FIXME: estimator selection");
      } else throw Exception ("FIXME: metric selection");
    }

    if (output_affine)
      save_transform (affine.get_transform(), affine_filename);

    if (output_affine_1tomid)
      save_transform (affine.get_transform_half(), affine_1tomid_filename);

    if (output_affine_2tomid)
      save_transform (affine.get_transform_half_inverse(), affine_2tomid_filename);
  }


  // ****** RUN SYN REGISTRATION *******
  if (do_syn) {
    CONSOLE ("running SyN registration");

    if (do_reorientation)
      syn_registration.set_aPSF_directions (directions_cartesian);

    if (do_affine) {
      syn_registration.run (affine, im1_image, im2_image, im1_mask, im2_mask);
    } else if (do_rigid) {
      syn_registration.run (rigid, im1_image, im2_image, im1_mask, im2_mask);
    } else {
      Registration::Transform::Affine identity_transform;
      syn_registration.run (identity_transform, im1_image, im2_image, im1_mask, im2_mask);
    }

    if (warp_filename.size()) {
      //TODO add affine parameters to comments too?
      Header output_header = syn_registration.get_output_warps_header();
      auto output_warps = Image<float>::create (warp_filename, output_header);
      syn_registration.get_output_warps (output_warps);
    }
  }



  if (im1_transformed.valid()) {
    INFO ("Outputting tranformed input images...");

    if (do_syn) {
      Header deform_header (im1_transformed);
      deform_header.set_ndim(4);
      deform_header.size(3) = 3;
      Image<default_type> deform_field = Image<default_type>::scratch (deform_header);

      Registration::Transform::compose_halfway_transforms (syn_registration.get_im2_linear().inverse(),
                                                           *(syn_registration.get_im2_disp_field_inv()),
                                                           *(syn_registration.get_im1_disp_field()),
                                                           syn_registration.get_im1_linear(),
                                                           deform_field);
      if (im1_image.ndim() == 3) {
        Filter::warp<Interp::Cubic> (im1_image, im1_transformed, deform_field, 0.0);
      } else {
        //TODO warp input lmax. Use more directions for lmax 8
        //TODO use higher number directions for output reorientation
        auto temp_output = Image<default_type>::scratch (im1_transformed);
        Filter::warp<Interp::Cubic> (im1_image, temp_output, deform_field, 0.0);
        if (do_reorientation)
          Registration::Transform::reorient_warp ("reorienting FODs...", temp_output, deform_field, directions_cartesian);
        threaded_copy (temp_output, im1_transformed);
      }

    } else if (do_affine) {
      Filter::reslice<Interp::Cubic> (im1_image, im1_transformed, affine.get_transform(), Adapter::AutoOverSample, 0.0);
      if (do_reorientation)
        Registration::Transform::reorient ("reorienting FODs...", im1_transformed, im1_transformed, affine.get_transform(), directions_cartesian);
    } else {
      Filter::reslice<Interp::Cubic> (im1_image, im1_transformed, rigid.get_transform(), Adapter::AutoOverSample, 0.0);
      if (do_reorientation)
        Registration::Transform::reorient ("reorienting FODs...", im1_transformed, im1_transformed, rigid.get_transform(), directions_cartesian);
    }
  }


  if (!im1_midway_transformed_path.empty() and !im2_midway_transformed_path.empty()) {
    if (do_syn) {
      Image<default_type> im1_deform_field = Image<default_type>::scratch (*(syn_registration.get_im1_disp_field()));
      Registration::Transform::compose_linear_displacement (affine.get_transform_half(), *(syn_registration.get_im1_disp_field()), im1_deform_field);
      Header midway_header (syn_registration.get_midway_header());
      midway_header.datatype() = DataType::from_command_line (DataType::Float32);
      midway_header.set_ndim(im1_image.ndim());
      if (midway_header.ndim() == 4)
        midway_header.size(3) = im1_image.size(3);
      auto im1_midway = Image<default_type>::create (im1_midway_transformed_path, midway_header);

      if (im1_image.ndim() == 3) {
        Filter::warp<Interp::Cubic> (im1_image, im1_midway, im1_deform_field, 0.0);
      } else {
        //TODO warp input lmax. Use more directions for lmax 8
        auto temp_output = Image<default_type>::scratch (midway_header);
        Filter::warp<Interp::Cubic> (im1_image, temp_output, im1_deform_field, 0.0);
        if (do_reorientation)
          Registration::Transform::reorient_warp ("reorienting FODs...", temp_output, im1_deform_field, directions_cartesian);
        threaded_copy (temp_output, im1_midway);
      }

      Image<default_type> im2_deform_field = Image<default_type>::scratch (*(syn_registration.get_im2_disp_field()));
      Registration::Transform::compose_linear_displacement (affine.get_transform_half_inverse(), *(syn_registration.get_im2_disp_field()), im2_deform_field);

      auto im2_midway = Image<default_type>::create (im2_midway_transformed_path, midway_header);

      if (im2_image.ndim() == 3) {
        Filter::warp<Interp::Cubic> (im2_image, im2_midway, im2_deform_field, 0.0);
      } else {
        auto temp_output = Image<default_type>::scratch (midway_header);
        Filter::warp<Interp::Cubic> (im2_image, temp_output, im2_deform_field, 0.0);
        if (do_reorientation)
          Registration::Transform::reorient_warp ("reorienting FODs...", temp_output, im2_deform_field, directions_cartesian);
        threaded_copy (temp_output, im2_midway);
      }

    } else if (do_affine){
      affine_registration.write_transformed_images (im1_image, im2_image, affine, im1_midway_transformed_path, im2_midway_transformed_path, do_reorientation);
    } else {
      rigid_registration.write_transformed_images (im1_image, im2_image, rigid, im1_midway_transformed_path, im2_midway_transformed_path, do_reorientation);

    }
  }
}
