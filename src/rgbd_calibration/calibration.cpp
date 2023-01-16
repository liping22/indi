/*
 *  Copyright (c) 2013-2014, Filippo Basso <bassofil@dei.unipd.it>
 *
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *     1. Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *     2. Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in the
 *        documentation and/or other materials provided with the distribution.
 *     3. Neither the name of the copyright holder(s) nor the
 *        names of its contributors may be used to endorse or promote products
 *        derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 *  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 *  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 *  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <ros/ros.h>
#include <omp.h>
#include <ceres/ceres.h>
#include <ceres/rotation.h>

#include <pcl/visualization/pcl_visualizer.h>
#include <pcl/filters/random_sample.h>
#include <pcl/io/pcd_io.h>

#include <eigen_conversions/eigen_msg.h>

#include <calibration_common/ceres/plane_fit.h>
#include <calibration_common/base/pcl_conversion.h>
#include <kinect/depth/polynomial_matrix_io.h>

#include <rgbd_calibration/checkerboard_views_extractor.h>
#include <rgbd_calibration/plane_based_extrinsic_calibration.h>
#include <rgbd_calibration/depth_undistortion_estimation.h>

#include <rgbd_calibration/calibration.h>

#define RGBD_INFO(id, msg) ROS_INFO_STREAM("RGBD " << id << ": " << msg)

namespace calibration
{

class CheckerboardDistanceConstraint : public Constraint<Checkerboard>
{

public:

  CheckerboardDistanceConstraint (Scalar distance,
                                  const Point3 & from = Point3::Zero())
    : distance_(distance),
      from_(from)
  {
    // Do nothing
  }

  virtual
  ~CheckerboardDistanceConstraint()
  {
    // Do nothing
  }

  inline virtual bool
  isValid (const Checkerboard & checkerboard) const
  {
    return (checkerboard.center() - from_).norm() <= distance_;
  }

  EIGEN_MAKE_ALIGNED_OPERATOR_NEW

private:

  Scalar distance_;
  Point3 from_;

};

void
Calibration::publishData () const
{
  if (not publisher_)
    return;

  publisher_->publishTF(*depth_sensor_);
  publisher_->publishTF(*color_sensor_);

  for (size_t i = 0; i < test_vec_.size(); ++i)
    publisher_->publish(*test_vec_[i]);

  for (size_t i = 0; i < cb_views_vec_.size(); ++i)
    publisher_->publish(*cb_views_vec_[i]);

}

void
Calibration::addData_ (const cv::Mat & image,
                       const PCLCloud3::ConstPtr & cloud,
                       std::vector<RGBDData::ConstPtr> & vec)
{
  RGBDData::Ptr data(boost::make_shared<RGBDData>(data_vec_.size() + 1));
  data->setColorSensor(color_sensor_);
  data->setDepthSensor(depth_sensor_);

  //cv::Mat rectified;
  //color_sensor_->cameraModel()->rectifyImage(image, rectified);
  data->setColorData(image);

  if (ratio_ > 1)
  {
    PCLCloud3::Ptr new_cloud(boost::make_shared<PCLCloud3>());
    new_cloud->resize(cloud->size() / (ratio_ * ratio_));
    new_cloud->header = cloud->header;
    new_cloud->width = cloud->width / ratio_;
    new_cloud->height = cloud->height / ratio_;
    new_cloud->is_dense = cloud->is_dense;

    PCLPoint3 zero(0, 0, 0);
    float nan = std::numeric_limits<float>::quiet_NaN();
    PCLPoint3 bad_point(nan, nan, nan);

    for (Size1 i = 0; i < new_cloud->height; ++i)
    {
      for (Size1 j = 0; j < new_cloud->width; ++j)
      {
        new_cloud->at(j, i) = zero;
        int count = 0;
        for (Size1 di = 0; di < ratio_; ++di)
        {
          for (Size1 dj = 0; dj < ratio_; ++dj)
          {
            const PCLPoint3 & p = cloud->at(j * ratio_ + dj, i * ratio_ + di);
            if (pcl::isFinite(p))
            {
              ++count;
              new_cloud->at(j, i).x += p.x;
              new_cloud->at(j, i).y += p.y;
              new_cloud->at(j, i).z += p.z;
            }
          }
        }
        if (count > 0)
        {
          new_cloud->at(j, i).x /= count;
          new_cloud->at(j, i).y /= count;
          new_cloud->at(j, i).z /= count;
        }
        else
        {
          new_cloud->at(j, i) = bad_point;
          new_cloud->is_dense = false;
        }
      }
    }
    data->setDepthData(*new_cloud);
  }
  else
  {
    data->setDepthData(*cloud);
  }

  vec.push_back(data);
}

void
Calibration::addData (const cv::Mat & image,
                      const PCLCloud3::ConstPtr & cloud)
{
  addData_(image, cloud, data_vec_);
}

//void
//Calibration::addTestData (const cv::Mat & image,
//                          const PCLCloud3::ConstPtr & cloud)
//{
//  addData_(image, cloud, test_vec_);
//}

void
Calibration::perform ()
{
  if (estimate_initial_trasform_ or not color_sensor_->parent())
    estimateInitialTransform();

  if (estimate_depth_und_model_)
  {
    CheckerboardViewsExtraction cb_extractor;
    cb_extractor.setColorSensorPose(color_sensor_->pose());
    cb_extractor.setCheckerboardVector(cb_vec_);
    cb_extractor.setInputData(data_vec_);
    cb_extractor.setOnlyImages(true);
    cb_extractor.extractAll(cb_views_vec_);

    ROS_INFO_STREAM(cb_views_vec_.size());

    for (Size1 i = 0; i < cb_views_vec_.size(); ++i)
    {
      CheckerboardViews & cb_views = *cb_views_vec_[i];
      Checkerboard::Ptr cb = boost::make_shared<Checkerboard>(*cb_views.colorCheckerboard());
      cb->transform(color_sensor_->pose());
      depth_data_vec_.push_back(depth_undistortion_estimation_->addDepthData(cb_views.data()->depthData(), cb));
    }

    ROS_INFO_STREAM("Estimating undistortion map...");
    depth_undistortion_estimation_->estimateLocalModel();
    ROS_INFO_STREAM("Recomputing undistortion map...");
    depth_undistortion_estimation_->estimateLocalModelReverse();
    ROS_INFO_STREAM("Estimating global error correction map...");
    depth_undistortion_estimation_->estimateGlobalModel();

    for (Size1 i = 0; i < cb_views_vec_.size(); ++i)
    {
      CheckerboardViews & cb_views = *cb_views_vec_[i];
      if (depth_data_vec_[i]->plane_extracted_)
        cb_views.setPlaneInliers(depth_data_vec_[i]->estimated_plane_);
      else
        cb_views_vec_[i].reset();
    }

//    PolynomialUndistortionMatrixIO<LocalPolynomial> io;
////    io.write(*local_matrix_->model(), "/tmp/local_matrix.txt");
//    int index = 0;
//    for (Scalar i = 1.0; i < 5.5; i += 0.125, ++index)
//    {
//      Scalar max;
//      std::stringstream ss;
//      ss << "/tmp/matrix_"<< index << ".png";
//      io.writeImageAuto(*local_matrix_->model(), i, ss.str(), max);
//      ROS_INFO_STREAM("Max " << i << ": " << max);
//    }

//    std::vector<std::pair<int, int> > pixels;
//    pixels.push_back(std::make_pair(0, 0));
//    pixels.push_back(std::make_pair(40/ratio_/local_model_->binSize().x(), 480/ratio_/local_model_->binSize().y()));
//    pixels.push_back(std::make_pair(320/ratio_/local_model_->binSize().x(), 240/ratio_/local_model_->binSize().y()));
//    pixels.push_back(std::make_pair(560/ratio_/local_model_->binSize().x(), 400/ratio_/local_model_->binSize().y()));
//
//    for (int i = 0; i < pixels.size(); ++i)
//    {
//      std::cout << "Local Polynomial at (" << pixels[i].first << ", " << pixels[i].second << "): " << local_model_->polynomial(pixels[i].first, pixels[i].second).transpose() << std::endl;
//
//      std::stringstream ss;
//      ss << "/tmp/local_samples_" << pixels[i].first << "_" << pixels[i].second << ".txt";
//
//      std::fstream fs;
//      fs.open(ss.str().c_str(), std::fstream::out);
//      fs << depth_undistortion_estimation_->getLocalSamples(pixels[i].first, pixels[i].second) << std::endl;
//      fs.close();
//    }
//
//    for (int i = 0; i < 4; ++i)
//    {
//      std::cout << "Global Polynomial at (" << i / 2 << ", " << i % 2 << "): " << global_model_->polynomial(i / 2, i % 2).transpose() << std::endl;
//
//      std::stringstream ss;
//      ss << "/tmp/global_samples_" << i / 2 << "_" << i % 2 << ".txt";
//
//      std::fstream fs;
//      fs.open(ss.str().c_str(), std::fstream::out);
//      fs << depth_undistortion_estimation_->getGlobalSamples(i / 2, i % 2) << std::endl;
//      fs.close();
//    }

  }

  estimateTransform(cb_views_vec_);

}

void
Calibration::estimateInitialTransform ()
{
  CheckerboardViewsExtraction cb_extractor;
  cb_extractor.setCheckerboardVector(cb_vec_);
  cb_extractor.setCheckerboardConstraint(boost::make_shared<CheckerboardDistanceConstraint>(2.0));

  std::vector<CheckerboardViews::Ptr> cb_views_vec;

  for (Size1 i = 0; i < data_vec_.size() and cb_views_vec.size() < 10; ++i)
  {
    Size1 index = rand() % data_vec_.size();
    cb_extractor.setInputData(data_vec_[index]);
    cb_extractor.extract(cb_views_vec, true);
  }

  estimateTransform(cb_views_vec);
}

void
Calibration::estimateTransform (const std::vector<CheckerboardViews::Ptr> & cb_views_vec)
{
  PlaneBasedExtrinsicCalibration calib;
  calib.setMainSensor(depth_sensor_);
  calib.setSize(cb_views_vec.size());

  int index = 0;

  for (Size1 i = 0; i < cb_views_vec.size(); ++i)
  {
    if (cb_views_vec[i])
    {
      calib.addData(index, color_sensor_, cb_views_vec[i]->colorCheckerboard());
      calib.addData(index++, depth_sensor_, cb_views_vec[i]->depthPlane());
    }
  }

  calib.setSize(index);
  calib.perform();
}

class TransformError
{
public:

  TransformError(const PinholeCameraModel::ConstPtr & camera_model,
                 const Checkerboard::ConstPtr & checkerboard,
                 const Cloud2 & image_corners,
                 const Plane & depth_plane,
                 const Polynomial<Scalar, 2> & depth_error_function)
    : camera_model_(camera_model),
      checkerboard_(checkerboard),
      image_corners_(image_corners),
      depth_plane_(depth_plane),
      depth_error_function_(depth_error_function)
  {
  }

  template <typename T>
  bool operator ()(const T * const color_sensor_pose,
                   const T * const checkerboard_pose,
                   T * residuals) const
  {
    typename Types<T>::Vector3 color_sensor_r_vec(color_sensor_pose[0], color_sensor_pose[1], color_sensor_pose[2]);
    typename Types<T>::AngleAxis color_sensor_r(color_sensor_r_vec.norm(), color_sensor_r_vec.normalized());
    typename Types<T>::Translation3 color_sensor_t(color_sensor_pose[3], color_sensor_pose[4], color_sensor_pose[5]);

    typename Types<T>::Transform color_sensor_pose_eigen = color_sensor_t * color_sensor_r;

    typename Types<T>::Vector3 checkerboard_r_vec(checkerboard_pose[0], checkerboard_pose[1], checkerboard_pose[2]);
    typename Types<T>::AngleAxis checkerboard_r(checkerboard_r_vec.norm(), checkerboard_r_vec.normalized());
    typename Types<T>::Translation3 checkerboard_t(checkerboard_pose[3], checkerboard_pose[4], checkerboard_pose[5]);

    typename Types<T>::Transform checkerboard_pose_eigen = checkerboard_t * checkerboard_r;

    typename Types<T>::Cloud3 cb_corners(checkerboard_->corners().size());
    cb_corners.container() = checkerboard_pose_eigen * checkerboard_->corners().container().cast<T>();

    typename Types<T>::Plane depth_plane(depth_plane_.normal().cast<T>(), T(depth_plane_.offset()));
    typename Types<T>::Cloud2 reprojected_corners = camera_model_->project3dToPixel2<T>(cb_corners);

    cb_corners.transform(color_sensor_pose_eigen);

    Polynomial<T, 2> depth_error_function(depth_error_function_.coefficients().cast<T>());

    for (Size1 i = 0; i < cb_corners.elements(); ++i)
    {
      residuals[2 * i] = T((reprojected_corners[i] - image_corners_[i].cast<T>()).norm() / 0.5);
      residuals[2 * i + 1] = T(depth_plane.absDistance(cb_corners[i])
                               / ceres::poly_eval(depth_error_function.coefficients(), cb_corners[i].z())); // TODO use line-of-sight error
    }

    return true;
  }

private:

  const PinholeCameraModel::ConstPtr & camera_model_;
  const Checkerboard::ConstPtr & checkerboard_;
  const Cloud2 & image_corners_;
  const Plane & depth_plane_;
  const Polynomial<Scalar, 2> depth_error_function_;

};

void Calibration::optimizeTransform(const std::vector<CheckerboardViews::Ptr> & cb_views_vec)
{
  ceres::Problem problem;
  Eigen::Matrix<Scalar, Eigen::Dynamic, 6, Eigen::DontAlign | Eigen::RowMajor> data(cb_views_vec.size(), 6);

  AngleAxis rotation(color_sensor_->pose().linear());
  Translation3 translation(color_sensor_->pose().translation());

  Eigen::Matrix<Scalar, 1, 6, Eigen::DontAlign | Eigen::RowMajor> transform;
  transform.head<3>() = rotation.angle() * rotation.axis();
  transform.tail<3>() = translation.vector();

  for (size_t i = 0; i < cb_views_vec.size(); ++i)
  {
    const CheckerboardViews & cb_views = *cb_views_vec[i];

    rotation = AngleAxis(cb_views.colorCheckerboard()->pose().linear());
    data.row(i).head<3>() = rotation.angle() * rotation.axis();
    data.row(i).tail<3>() = cb_views.colorCheckerboard()->pose().translation();

    TransformError * error = new TransformError(color_sensor_->cameraModel(),
                                                cb_views.checkerboard(),
                                                cb_views.colorView()->points(),
                                                cb_views.depthPlane()->plane(),
                                                depth_sensor_->depthErrorFunction());

    typedef ceres::AutoDiffCostFunction<TransformError, ceres::DYNAMIC, 6, 6> TransformCostFunction;

    ceres::CostFunction * cost_function = new TransformCostFunction(error, 2 * cb_views.checkerboard()->size());
    problem.AddResidualBlock(cost_function, new ceres::CauchyLoss(1.0), transform.data(), data.row(i).data());
  }

  ceres::Solver::Options options;
  options.linear_solver_type = ceres::SPARSE_SCHUR;
  options.max_num_iterations = 100;
  options.minimizer_progress_to_stdout = true;
  options.num_threads = 8;

  ceres::Solver::Summary summary;
  ceres::Solve(options, &problem, &summary);

  rotation.angle() = transform.head<3>().norm();
  rotation.axis() = transform.head<3>().normalized();
  translation.vector() = transform.tail<3>();

  color_sensor_->setPose(translation * rotation);

}

class ReprojectionError
{
public:

  ReprojectionError (const PinholeCameraModel::ConstPtr & camera_model,
                     const Checkerboard::ConstPtr & checkerboard,
                     const Cloud2 & image_corners)
    : camera_model_(camera_model),
      checkerboard_(checkerboard),
      image_corners_(image_corners)
  {
    // Do nothing
  }

  template <typename T>
    typename Types<T>::Pose toEigen(const T * const q, const T * const t) const
    {
      typename Types<T>::Quaternion rotation(q[0], q[1], q[2], q[3]);
      typename Types<T>::Translation3 translation(t[0], t[1], t[2]);
      return translation * rotation;
    }

  template <typename T>
    bool
	operator () (const T * const checkerboard_pose_q,
               const T * const checkerboard_pose_t,
               T * residuals) const
    {
      typename Types<T>::Pose checkerboard_pose_eigen = toEigen<T>(checkerboard_pose_q, checkerboard_pose_t);

      typename Types<T>::Cloud3 cb_corners(checkerboard_->corners().size());
      cb_corners.container() = checkerboard_pose_eigen * checkerboard_->corners().container().cast<T>();

      typename Types<T>::Cloud2 reprojected_corners(checkerboard_->corners().size());
      for (Size1 i = 0; i < cb_corners.elements(); ++i)
        reprojected_corners[i] = camera_model_->project3dToPixel2<T>(cb_corners[i]);

      Eigen::Map<Eigen::Matrix<T, 2, Eigen::Dynamic> > residual_map(residuals, 2, cb_corners.elements());
      residual_map = (reprojected_corners.container() - image_corners_.container().cast<T>()) / (0.5 * std::sqrt(T(cb_corners.elements())));

      return true;
    }

private:

  const PinholeCameraModel::ConstPtr & camera_model_;
  const Checkerboard::ConstPtr & checkerboard_;
  Cloud2 image_corners_;
};

class TransformDistortionError
{
public:

  TransformDistortionError(const PinholeCameraModel::ConstPtr & camera_model,
		  	  	  	  	   const KinectDepthCameraModel::ConstPtr & depth_camera_model,
                           const Checkerboard::ConstPtr & checkerboard,
                           const Cloud3 & depth_points,
                           const Indices & plane_indices,
                           const Polynomial<Scalar, 2> & depth_error_function,
                           const Size2 & images_size)
    : camera_model_(camera_model),
      depth_camera_model_(depth_camera_model),
      checkerboard_(checkerboard),
      depth_points_(depth_points),
      plane_indices_(plane_indices),
      depth_error_function_(depth_error_function),
      images_size_(images_size)
  {
    // Do nothing
  }

  template <typename T>
    typename Types<T>::Pose toEigen(const T * const q, const T * const t) const
    {
      typename Types<T>::Quaternion rotation(q[0], q[1], q[2], q[3]);
      typename Types<T>::Translation3 translation(t[0], t[1], t[2]);
      return translation * rotation;
    }

  template <typename T>
    bool operator ()(const T * const color_sensor_pose_q,
                     const T * const color_sensor_pose_t,
                     const T * const global_undistortion,
                     const T * const checkerboard_pose_q,
                     const T * const checkerboard_pose_t,
                     const T * const delta,
                     T * residuals) const
    {
      typename Types<T>::Pose color_sensor_pose_eigen = toEigen<T>(color_sensor_pose_q, color_sensor_pose_t);
      typename Types<T>::Pose checkerboard_pose_eigen = toEigen<T>(checkerboard_pose_q, checkerboard_pose_t);

      const int DEGREE = MathTraits<GlobalPolynomial>::Degree;
      const int MIN_DEGREE = MathTraits<GlobalPolynomial>::MinDegree;
      const int SIZE = MathTraits<GlobalPolynomial>::Size;//DEGREE - MIN_DEGREE + 1;
      typedef MathTraits<GlobalPolynomial>::Coefficients Coefficients;

      Size1 index = 0;

      Coefficients c1, c2, c3;
      for (Size1 i = 0 ; i < DEGREE - MIN_DEGREE + 1; ++i)
        c1[i] = global_undistortion[index++];
      for (Size1 i = 0 ; i < DEGREE - MIN_DEGREE + 1; ++i)
        c2[i] = global_undistortion[index++];
      for (Size1 i = 0 ; i < DEGREE - MIN_DEGREE + 1; ++i)
        c3[i] = global_undistortion[index++];

      Polynomial<T, DEGREE, MIN_DEGREE> p1(c1);
      Polynomial<T, DEGREE, MIN_DEGREE> p2(c2);
      Polynomial<T, DEGREE, MIN_DEGREE> p3(c3);

      Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic> A(SIZE, SIZE);
      Eigen::Matrix<T, Eigen::Dynamic, 1> b(SIZE, 1);
      for (int i = 0; i < SIZE; ++i)
      {
        T x(i + 1);
        T y = p2.evaluate(x) + p3.evaluate(x) - p1.evaluate(x);
        T tmp(1.0);
        for (int j = 0; j < MIN_DEGREE; ++j)
          tmp *= x;
        for (int j = 0; j < SIZE; ++j)
        {
          A(i, j) = tmp;
          tmp *= x;
        }
        b[i] = y;
      }

      Eigen::Matrix<T, Eigen::Dynamic, 1> x = A.colPivHouseholderQr().solve(b);

      GlobalModel::Data::Ptr global_data = boost::make_shared<GlobalModel::Data>(Size2(2, 2));
      for (int i = 0; i < 3 * MathTraits<GlobalPolynomial>::Size; ++i)
        global_data->container().data()[0 * MathTraits<GlobalPolynomial>::Size + i] = global_undistortion[i];
      for (int i = 0; i < SIZE; ++i)
        global_data->container().data()[3 * MathTraits<GlobalPolynomial>::Size + i] = x[i];

      GlobalModel::Ptr global_model = boost::make_shared<GlobalModel>(images_size_);
      global_model->setMatrix(global_data);

      GlobalMatrixEigen global(global_model);

      typename Types<T>::Cloud3 depth_points(depth_points_.size());
      depth_points.container() = depth_points_.container().cast<T>();

      const cv::Matx33d & K = depth_camera_model_->intrinsicMatrix();

      for (int j = 0; j < depth_points.size().y(); ++j)
      {
        for (int i = 0; i < depth_points.size().x(); ++i)
        {
          T z = depth_points(i, j).z();
          typename Types<T>::Point2 normalized_pixel((i - (K(0, 2) + delta[2])) / (K(0, 0) * delta[0]),
                                                     (j - (K(1, 2) + delta[3])) / (K(1, 1) * delta[1]));

          depth_points(i, j) = z * depth_camera_model_->undistort2d_<T>(normalized_pixel).homogeneous();

          /*T z = depth_points(i, j).z();
          depth_points(i, j).x() = (i - (depth_camera_model_->cx() + depth_camera_model_->Tx() + delta[2])) * depth_points(i, j).z() / (depth_camera_model_->fx() * delta[0]);
          depth_points(i, j).y() = (j - (depth_camera_model_->cy() + depth_camera_model_->Ty() + delta[3])) * depth_points(i, j).z() / (depth_camera_model_->fy() * delta[1]);*/
        }
      }

      global.undistort(depth_points);

      typename Types<T>::Cloud3 cb_corners(checkerboard_->corners().size());
      cb_corners.container() = color_sensor_pose_eigen * checkerboard_pose_eigen * checkerboard_->corners().container().cast<T>();

      Polynomial<T, 2> depth_error_function(depth_error_function_.coefficients().cast<T>());
      typename Types<T>::Plane cb_plane = Types<T>::Plane::Through(cb_corners(0, 0), cb_corners(0, 1), cb_corners(1, 0));

      Eigen::Map<Eigen::Matrix<T, 3, Eigen::Dynamic> > residual_map_dist(residuals, 3, plane_indices_.size());
      for (Size1 i = 0; i < plane_indices_.size(); ++i)
      {
        Line line(Point3::Zero(), depth_points[plane_indices_[i]].normalized());
        residual_map_dist.col(i) = (line.intersectionPoint(cb_plane) - depth_points[plane_indices_[i]]) /
            (std::sqrt(T(plane_indices_.size())) * ceres::poly_eval(depth_error_function.coefficients(), depth_points[plane_indices_[i]].z()));
      }

//      Scalar alpha = std::acos(depth_plane.normal().dot(cb_plane.normal()));
//      if (alpha > M_PI_2)
//        alpha = M_PI - alpha;
//      residual_map_dist *= std::exp(alpha);

      return true;
    }

private:

  const PinholeCameraModel::ConstPtr & camera_model_;
  const KinectDepthCameraModel::ConstPtr & depth_camera_model_;
  const Checkerboard::ConstPtr & checkerboard_;

  const Cloud3 depth_points_;
  const Indices plane_indices_;

  const Polynomial<Scalar, 2> depth_error_function_;
  const Size2 images_size_;

};

typedef ceres::NumericDiffCostFunction<ReprojectionError, ceres::CENTRAL, ceres::DYNAMIC, 4, 3> ReprojectionCostFunction;

typedef ceres::NumericDiffCostFunction<TransformDistortionError, ceres::CENTRAL, ceres::DYNAMIC, 4, 3,
    3 * MathTraits<GlobalPolynomial>::Size, 4, 3, 4> TransformDistortionCostFunction;

void
Calibration::optimizeAll (const std::vector<CheckerboardViews::Ptr> & cb_views_vec)
{
  ceres::Problem problem;
  Eigen::Matrix<Scalar, Eigen::Dynamic, 7, Eigen::DontAlign | Eigen::RowMajor> data(cb_views_vec.size(), 7);

  //AngleAxis rotation(color_sensor_->pose().linear());
  Quaternion rotation(color_sensor_->pose().linear());
  Translation3 translation(color_sensor_->pose().translation());

  Eigen::Matrix<Scalar, 1, 7 , Eigen::DontAlign | Eigen::RowMajor> transform;
  //transform.head<3>() = rotation.angle() * rotation.axis();
  transform[0] = rotation.w();
  transform[1] = rotation.x();
  transform[2] = rotation.y();
  transform[3] = rotation.z();
  transform.tail<3>() = translation.vector();

  double delta[4] = {1.0, 1.0, 0.0, 0.0};

  for (size_t i = 0; i < cb_views_vec.size(); ++i)
  {
    const CheckerboardViews & cb_views = *cb_views_vec[i];

    /*rotation = AngleAxis(cb_views.colorCheckerboard()->pose().linear());
    data.row(i).head<3>() = rotation.angle() * rotation.axis();*/

    Quaternion rotation = Quaternion(cb_views.colorCheckerboard()->pose().linear());
    data.row(i)[0] = rotation.w();
    data.row(i)[1] = rotation.x();
    data.row(i)[2] = rotation.y();
    data.row(i)[3] = rotation.z();
    data.row(i).tail<3>() = cb_views.colorCheckerboard()->pose().translation();

    TransformDistortionError * error;
    ceres::CostFunction * cost_function;

    error = new TransformDistortionError(color_sensor_->cameraModel(),
									     depth_sensor_->cameraModel(),
									     cb_views.checkerboard(),
									     PCLConversion<Scalar>::toPointMatrix(*cb_views.depthView()->data()),
									     cb_views.depthView()->points(),
									     depth_sensor_->depthErrorFunction(),
									     global_model_->imageSize());

    cost_function = new TransformDistortionCostFunction(error,
    		                                            ceres::DO_NOT_TAKE_OWNERSHIP,
														3 * cb_views.depthView()->points().size());

    problem.AddResidualBlock(cost_function,
                             NULL,//new ceres::CauchyLoss(1.0),
                             transform.data(),
                             transform.data() + 4,
                             global_matrix_->model()->dataPtr(),
                             data.row(i).data(),
                             data.row(i).data() + 4,
                             delta);

    ReprojectionError * repr_error = new ReprojectionError(color_sensor_->cameraModel(),
                                                           cb_views.checkerboard(),
                                                           cb_views.colorView()->points());
    ceres::CostFunction * repr_cost_function = new ReprojectionCostFunction(repr_error,
                                                                            ceres::DO_NOT_TAKE_OWNERSHIP,
                                                                            2 * cb_views.checkerboard()->size());

    problem.AddResidualBlock(repr_cost_function,
                             NULL,//new ceres::CauchyLoss(1.0),
                             data.row(i).data(),
                             data.row(i).data() + 4);

    problem.SetParameterization(data.row(i).data(), new ceres::QuaternionParameterization());
  }

  problem.SetParameterization(transform.data(), new ceres::QuaternionParameterization());

  ceres::Solver::Options options;
  options.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;
  options.max_num_iterations = 20;
  options.minimizer_progress_to_stdout = true;
  options.num_threads = 8;

//  problem.SetParameterBlockConstant(delta);

  ceres::Solver::Summary summary;
  ceres::Solve(options, &problem, &summary);

  rotation = Quaternion(transform[0], transform[1], transform[2], transform[3]);
  translation.vector() = transform.tail<3>();

  color_sensor_->setPose(translation * rotation);


  const int DEGREE = MathTraits<GlobalPolynomial>::Degree;
  const int MIN_DEGREE = MathTraits<GlobalPolynomial>::MinDegree;
  const int SIZE = DEGREE - MIN_DEGREE + 1;
  typedef MathTraits<GlobalPolynomial>::Coefficients Coefficients;

  GlobalPolynomial p1(global_matrix_->model()->polynomial(0, 0));
  GlobalPolynomial p2(global_matrix_->model()->polynomial(0, 1));
  GlobalPolynomial p3(global_matrix_->model()->polynomial(1, 0));

  Eigen::Matrix<Scalar, Eigen::Dynamic, Eigen::Dynamic> A(SIZE, SIZE);
  Eigen::Matrix<Scalar, Eigen::Dynamic, 1> b(SIZE, 1);
  for (int i = 0; i < SIZE; ++i)
  {
    Scalar x(i + 1);
    Scalar y =  p2.evaluate(x) + p3.evaluate(x) - p1.evaluate(x);
    Scalar tmp(1.0);
    for (int j = 0; j < MIN_DEGREE; ++j)
      tmp *= x;
    for (int j = 0; j < SIZE; ++j)
    {
      A(i, j) = tmp;
      tmp *= x;
    }
    b[i] = y;
  }

  Eigen::Matrix<Scalar, Eigen::Dynamic, 1> x = A.colPivHouseholderQr().solve(b);

  global_matrix_->model()->polynomial(1, 1) = x;

  depth_sensor_->cameraModel();
  depth_intrinsics_[0] = depth_intrinsics_[0] * delta[0];
  depth_intrinsics_[1] = depth_intrinsics_[1] * delta[1];
  depth_intrinsics_[2] = depth_intrinsics_[2] + delta[2];
  depth_intrinsics_[3] = depth_intrinsics_[3] + delta[3];
}

void
Calibration::optimize ()
{
  ROS_INFO("Optimizing...\n");

  if (estimate_depth_und_model_)
  {
    std::vector<CheckerboardViews::Ptr> und_cb_views_vec;

    // Create locally undistorted clouds and views
#pragma omp parallel for
    for (Size1 i = 0; i < cb_views_vec_.size(); ++i)
    {
      if (not cb_views_vec_[i])
        continue;

      const CheckerboardViews & cb_views = *cb_views_vec_[i];
      const DepthUndistortionEstimation::DepthData & depth_data = *depth_data_vec_[i];

      CheckerboardViews::Ptr und_cb_views = boost::make_shared<CheckerboardViews>(cb_views);

      RGBDData::Ptr und_data = boost::make_shared<RGBDData>(*cb_views.data());
      und_data->setDepthData(*depth_data.undistorted_cloud_);

//      std::stringstream ss;
//      ss <<  "/tmp/und_cloud_" << i <<  ".pcd";
//      pcl::PCDWriter writer;
//      writer.write(ss.str(), *depth_data.undistorted_cloud_);
//
//      ss.str("");
//      ss <<  "/tmp/cloud_" << i <<  ".pcd";
//      writer.write(ss.str(), *depth_data.cloud_);

      und_cb_views->setId(cb_views.id() + "_undistorted");
      und_cb_views->setData(und_data);
      und_cb_views->setPlaneInliers(depth_data.estimated_plane_.indices_, depth_data.estimated_plane_.std_dev_);

#pragma omp critical
      und_cb_views_vec.push_back(und_cb_views);
    }

    optimizeAll(und_cb_views_vec);
  }
  else
  {
    optimizeTransform(cb_views_vec_);
  }

//  PolynomialUndistortionMatrixIO<GlobalPolynomial> io;
//  //io.write(*global_matrix_->model(), "/tmp/opt_global_matrix.txt");
//
//  int index = 0;
//  for (Scalar i = 1.0; i < 5.5; i += 0.5, ++index)
//  {
//    Scalar max;
//    std::stringstream ss;
//    ss << "/tmp/g_matrix_"<< index << ".png";
//    io.writeImageAuto(*global_matrix_->model(), i, ss.str(), max);
//    ROS_INFO_STREAM("Max " << i << ": " << max);
//  }

//  const Size1 size = test_vec_.size();
//
//  for (Size1 i = 0; i < size; ++i)
//  {
//    const RGBDData::ConstPtr & test_data = test_vec_[i];
//
//    PCLCloud3::Ptr depth_data = boost::make_shared<PCLCloud3>(*test_data->depthData());
//    UndistortionPCL und;
//    und.setModel(depth_sensor_->undistortionModel());
//    und.undistort(*depth_data);
//    RGBDData::Ptr new_test_data = boost::make_shared<RGBDData>(*test_data);
//    new_test_data->setDepthData(*depth_data);
//    new_test_data->setId(10000 + test_data->id());
//    test_vec_.push_back(new_test_data);
//
//  }

//  std::ofstream transform_file;
//  transform_file.open("/tmp/camera_pose.yaml");
//  geometry_msgs::Pose pose_msg;
//  tf::poseEigenToMsg(color_sensor_->pose(), pose_msg);
//  transform_file << pose_msg;
//  transform_file.close();

}

} /* namespace calibration */
