// Copyright 2013, Ji Zhang, Carnegie Mellon University
// Further contributions copyright (c) 2016, Southwest Research Institute
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from this
//    software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// This is an implementation of the algorithm described in the following paper:
//   J. Zhang and S. Singh. LOAM: Lidar Odometry and Mapping in Real-time.
//     Robotics: Science and Systems Conference (RSS). Berkeley, CA, July 2014.

#include <pcl/filters/filter.h>

#include "loam_velodyne/LaserOdometry.h"
#include "loam_velodyne/common.h"
#include "math_utils.h"

namespace loam
{

  using std::sin;
  using std::cos;
  using std::asin;
  using std::atan2;
  using std::sqrt;
  using std::fabs;
  using std::pow;


  LaserOdometry::LaserOdometry(float scanPeriod, uint16_t ioRatio, size_t maxIterations):
    BasicLaserOdometry(scanPeriod, maxIterations),
    _ioRatio(ioRatio)
  {
    _initFrame = "/camera_init";
    _odomFrame = "/laser_odom";
    _loamOdomTopic = "/laser_odom_to_init";
    _lidarFrame = "/camera";
    _outputTransforms = true;

    // initialize odometry and odometry tf messages
    _laserOdometryMsg.header.frame_id = _initFrame;
    _laserOdometryMsg.child_frame_id  = _odomFrame;

    _laserOdometryTrans.frame_id_       = _initFrame;
    _laserOdometryTrans.child_frame_id_ = _odomFrame;
  }


  bool LaserOdometry::setup(ros::NodeHandle &node, ros::NodeHandle &privateNode)
  {
    // fetch laser odometry params
    float fParam;
    int iParam;
    bool bParam;
    std::string sParam;

    if (node.getParam("scanPeriod", fParam))
    {
      if (fParam <= 0)
      {
        ROS_ERROR("Invalid scanPeriod parameter: %f (expected > 0)", fParam);
        return false;
      }
      else
      {
        setScanPeriod(fParam);
        ROS_DEBUG("Set scanPeriod: %g", fParam);
      }
    }

    if (privateNode.getParam("ioRatio", iParam))
    {
      if (iParam < 1)
      {
        ROS_ERROR("Invalid ioRatio parameter: %d (expected >= 1)", iParam);
        return false;
      }
      else
      {
        _ioRatio = iParam;
        ROS_DEBUG("Set ioRatio: %d", iParam);
      }
    }

    if (privateNode.getParam("maxIterationsOdom", iParam))
    {
      if (iParam < 1)
      {
        ROS_ERROR("Invalid maxIterationsOdom parameter: %d (expected >= 1)", iParam);
        return false;
      }
      else
      {
        setMaxIterations(iParam);
        ROS_DEBUG("Set maxIterationsOdom: %d", iParam);
      }
    }

    if (privateNode.getParam("deltaTAbortOdom", fParam))
    {
      if (fParam <= 0)
      {
        ROS_ERROR("Invalid deltaTAbortOdom parameter: %f (expected > 0)", fParam);
        return false;
      }
      else
      {
        setDeltaTAbort(fParam);
        ROS_DEBUG("Set deltaTAbortOdom: %g", fParam);
      }
    }

    if (privateNode.getParam("deltaRAbortOdom", fParam))
    {
      if (fParam <= 0)
      {
        ROS_ERROR("Invalid deltaRAbortOdom parameter: %f (expected > 0)", fParam);
        return false;
      }
      else
      {
        setDeltaRAbort(fParam);
        ROS_DEBUG("Set deltaRAbortOdom: %g", fParam);
      }
    }

    if (node.getParam("initFrame", sParam)) {
      _initFrame = sParam;
      _laserOdometryMsg.header.frame_id = _initFrame;
      _laserOdometryTrans.frame_id_ = _initFrame;
      ROS_DEBUG("Set initial frame name to: %s", sParam.c_str());
    }

    if (node.getParam("odomFrame", sParam)) {
      _odomFrame = sParam;
      _laserOdometryMsg.child_frame_id  = _odomFrame;
      _laserOdometryTrans.child_frame_id_ = _odomFrame;
      ROS_DEBUG("Set odometry frame name to: %s", sParam.c_str());
    }

    if (node.getParam("loamOdomTopic", sParam)) {
      _loamOdomTopic = sParam;
      ROS_DEBUG("Set loam odometry topic name to: %s", sParam.c_str());
    }

    if (node.getParam("lidarFrame", sParam)) {
      _lidarFrame = sParam;
      ROS_DEBUG("Set lidar frame name to: %s", sParam.c_str());
    }

    if (node.getParam("outputTransforms", bParam)) {
      _outputTransforms = bParam;
      ROS_DEBUG("Set outputTransforms param to: %d", bParam);
    }

    // advertise laser odometry topics
    _pubLaserCloudCornerLast = node.advertise<sensor_msgs::PointCloud2>("laser_cloud_corner_last", 2);
    _pubLaserCloudSurfLast = node.advertise<sensor_msgs::PointCloud2>("laser_cloud_surf_last", 2);
    _pubLaserCloudFullRes = node.advertise<sensor_msgs::PointCloud2>("velodyne_cloud_3", 2);
    _pubLaserOdometry = node.advertise<nav_msgs::Odometry>(_loamOdomTopic, 5);

    // subscribe to scan registration topics
    _subCornerPointsSharp = node.subscribe<sensor_msgs::PointCloud2>
      ("laser_cloud_sharp", 2, &LaserOdometry::laserCloudSharpHandler, this);

    _subCornerPointsLessSharp = node.subscribe<sensor_msgs::PointCloud2>
      ("laser_cloud_less_sharp", 2, &LaserOdometry::laserCloudLessSharpHandler, this);

    _subSurfPointsFlat = node.subscribe<sensor_msgs::PointCloud2>
      ("laser_cloud_flat", 2, &LaserOdometry::laserCloudFlatHandler, this);

    _subSurfPointsLessFlat = node.subscribe<sensor_msgs::PointCloud2>
      ("laser_cloud_less_flat", 2, &LaserOdometry::laserCloudLessFlatHandler, this);

    _subLaserCloudFullRes = node.subscribe<sensor_msgs::PointCloud2>
      ("velodyne_cloud_2", 2, &LaserOdometry::laserCloudFullResHandler, this);

    _subImuTrans = node.subscribe<sensor_msgs::PointCloud2>
      ("imu_trans", 5, &LaserOdometry::imuTransHandler, this);

    return true;
  }

  void LaserOdometry::reset()
  {
    _newCornerPointsSharp = false;
    _newCornerPointsLessSharp = false;
    _newSurfPointsFlat = false;
    _newSurfPointsLessFlat = false;
    _newLaserCloudFullRes = false;
    _newImuTrans = false;
  }

  void LaserOdometry::laserCloudSharpHandler(const sensor_msgs::PointCloud2ConstPtr& cornerPointsSharpMsg)
  {
    _timeCornerPointsSharp = cornerPointsSharpMsg->header.stamp;

    cornerPointsSharp()->clear();
    pcl::fromROSMsg(*cornerPointsSharpMsg, *cornerPointsSharp());
    std::vector<int> indices;
    pcl::removeNaNFromPointCloud(*cornerPointsSharp(), *cornerPointsSharp(), indices);
    _newCornerPointsSharp = true;
  }

  void LaserOdometry::laserCloudLessSharpHandler(const sensor_msgs::PointCloud2ConstPtr& cornerPointsLessSharpMsg)
  {
    _timeCornerPointsLessSharp = cornerPointsLessSharpMsg->header.stamp;

    cornerPointsLessSharp()->clear();
    pcl::fromROSMsg(*cornerPointsLessSharpMsg, *cornerPointsLessSharp());
    std::vector<int> indices;
    pcl::removeNaNFromPointCloud(*cornerPointsLessSharp(), *cornerPointsLessSharp(), indices);
    _newCornerPointsLessSharp = true;
  }

  void LaserOdometry::laserCloudFlatHandler(const sensor_msgs::PointCloud2ConstPtr& surfPointsFlatMsg)
  {
    _timeSurfPointsFlat = surfPointsFlatMsg->header.stamp;

    surfPointsFlat()->clear();
    pcl::fromROSMsg(*surfPointsFlatMsg, *surfPointsFlat());
    std::vector<int> indices;
    pcl::removeNaNFromPointCloud(*surfPointsFlat(), *surfPointsFlat(), indices);
    _newSurfPointsFlat = true;
  }

  void LaserOdometry::laserCloudLessFlatHandler(const sensor_msgs::PointCloud2ConstPtr& surfPointsLessFlatMsg)
  {
    _timeSurfPointsLessFlat = surfPointsLessFlatMsg->header.stamp;

    surfPointsLessFlat()->clear();
    pcl::fromROSMsg(*surfPointsLessFlatMsg, *surfPointsLessFlat());
    std::vector<int> indices;
    pcl::removeNaNFromPointCloud(*surfPointsLessFlat(), *surfPointsLessFlat(), indices);
    _newSurfPointsLessFlat = true;
  }

  void LaserOdometry::laserCloudFullResHandler(const sensor_msgs::PointCloud2ConstPtr& laserCloudFullResMsg)
  {
    _timeLaserCloudFullRes = laserCloudFullResMsg->header.stamp;

    laserCloud()->clear();
    pcl::fromROSMsg(*laserCloudFullResMsg, *laserCloud());
    std::vector<int> indices;
    pcl::removeNaNFromPointCloud(*laserCloud(), *laserCloud(), indices);
    _newLaserCloudFullRes = true;
  }

  void LaserOdometry::imuTransHandler(const sensor_msgs::PointCloud2ConstPtr& imuTransMsg)
  {
    _timeImuTrans = imuTransMsg->header.stamp;

    pcl::PointCloud<pcl::PointXYZ> imuTrans;
    pcl::fromROSMsg(*imuTransMsg, imuTrans);
    updateIMU(imuTrans);
    _newImuTrans = true;
  }

  void LaserOdometry::spin()
  {
    ros::Rate rate(100);
    bool status = ros::ok();

    // loop until shutdown
    while (status)
    {
      ros::spinOnce();

      // try processing new data
      process();

      status = ros::ok();
      rate.sleep();
    }
  }

  bool LaserOdometry::hasNewData()
  {
    return _newCornerPointsSharp && _newCornerPointsLessSharp && _newSurfPointsFlat &&
      _newSurfPointsLessFlat && _newLaserCloudFullRes && _newImuTrans &&
      fabs((_timeCornerPointsSharp - _timeSurfPointsLessFlat).toSec()) < 0.005 &&
      fabs((_timeCornerPointsLessSharp - _timeSurfPointsLessFlat).toSec()) < 0.005 &&
      fabs((_timeSurfPointsFlat - _timeSurfPointsLessFlat).toSec()) < 0.005 &&
      fabs((_timeLaserCloudFullRes - _timeSurfPointsLessFlat).toSec()) < 0.005 &&
      fabs((_timeImuTrans - _timeSurfPointsLessFlat).toSec()) < 0.005;
  }

  void LaserOdometry::process()
  {
    if (!hasNewData())
      return;// waiting for new data to arrive...

    reset();// reset flags, etc.
    BasicLaserOdometry::process();
    publishResult();
  }

  void LaserOdometry::publishResult()
  {
    // publish odometry transformations
    geometry_msgs::Quaternion geoQuat = tf::createQuaternionMsgFromRollPitchYaw(transformSum().rot_z.rad(),
                                                                               -transformSum().rot_x.rad(),
                                                                               -transformSum().rot_y.rad());

    _laserOdometryMsg.header.stamp            = _timeSurfPointsLessFlat;
    _laserOdometryMsg.pose.pose.orientation.x = -geoQuat.y;
    _laserOdometryMsg.pose.pose.orientation.y = -geoQuat.z;
    _laserOdometryMsg.pose.pose.orientation.z = geoQuat.x;
    _laserOdometryMsg.pose.pose.orientation.w = geoQuat.w;
    _laserOdometryMsg.pose.pose.position.x    = transformSum().pos.x();
    _laserOdometryMsg.pose.pose.position.y    = transformSum().pos.y();
    _laserOdometryMsg.pose.pose.position.z    = transformSum().pos.z();
    _pubLaserOdometry.publish(_laserOdometryMsg);

    if(_outputTransforms){
    _laserOdometryTrans.stamp_ = _timeSurfPointsLessFlat;
    _laserOdometryTrans.setRotation(tf::Quaternion(-geoQuat.y, -geoQuat.z, geoQuat.x, geoQuat.w));
    _laserOdometryTrans.setOrigin(tf::Vector3(transformSum().pos.x(), transformSum().pos.y(), transformSum().pos.z()));
    _tfBroadcaster.sendTransform(_laserOdometryTrans);
    }

    // publish cloud results according to the input output ratio
    if (_ioRatio < 2 || frameCount() % _ioRatio == 1)
    {
      ros::Time sweepTime = _timeSurfPointsLessFlat;
      publishCloudMsg(_pubLaserCloudCornerLast, *lastCornerCloud(), sweepTime, _lidarFrame);
      publishCloudMsg(_pubLaserCloudSurfLast, *lastSurfaceCloud(), sweepTime, _lidarFrame);

      transformToEnd(laserCloud());  // transform full resolution cloud to sweep end before sending it
      publishCloudMsg(_pubLaserCloudFullRes, *laserCloud(), sweepTime, _lidarFrame);
    }
  }

} // end namespace loam
