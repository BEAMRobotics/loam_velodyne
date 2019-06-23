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

#include "loam_velodyne/TransformMaintenance.h"

namespace loam {

TransformMaintenance::TransformMaintenance() {
  _mapOdomTopic = "/aft_mapped_to_init";
  _loamOdomTopic = "/laser_odom_to_init";
  _lidarOdomTopic = "/integrated_to_init";
  _lidarFrame = "/camera";
  _initFrame = "/camera_init";

  // initialize odometry and odometry tf messages.
  _laserOdometry2.header.frame_id = _initFrame;
  _laserOdometry2.child_frame_id = _lidarFrame;

  _laserOdometryTrans2.frame_id_ = _initFrame;
  _laserOdometryTrans2.child_frame_id_ = _lidarFrame;
}

bool TransformMaintenance::setup(ros::NodeHandle &node,
                                 ros::NodeHandle &privateNode) {

  std::string sParam;
  if (privateNode.getParam("loamOdomTopic", sParam)) {
    _loamOdomTopic = sParam;
    ROS_DEBUG("Set loam odometry topic name to: %s", sParam.c_str());
  }

  if (privateNode.getParam("mapOdomTopic", sParam)) {
    _mapOdomTopic = sParam;
    ROS_DEBUG("Set map odometry topic name to: %s", sParam.c_str());
  }

  if (privateNode.getParam("lidarOdomTopic", sParam)) {
    _lidarOdomTopic = sParam;
    ROS_DEBUG("Set lidar odometry topic name to: %s", sParam.c_str());
  }

  if (privateNode.getParam("initFrame", sParam)) {
    _initFrame = sParam;
    _laserOdometry2.header.frame_id = _initFrame;
    _laserOdometryTrans2.frame_id_ = _initFrame;
    ROS_DEBUG("Set initial frame name to: %s", sParam.c_str());
  }

  if (privateNode.getParam("lidarFrame", sParam)) {
    _lidarFrame = sParam;
    _laserOdometry2.child_frame_id = _lidarFrame;
    _laserOdometryTrans2.child_frame_id_ = _lidarFrame;
    ROS_DEBUG("Set lidar frame name to: %s", sParam.c_str());
  }

  // advertise integrated laser odometry topic
  _pubLaserOdometry2 = node.advertise<nav_msgs::Odometry>(_lidarOdomTopic, 5);

  // subscribe to laser odometry and mapping odometry topics
  _subLaserOdometry = node.subscribe<nav_msgs::Odometry>(
      _loamOdomTopic, 5, &TransformMaintenance::laserOdometryHandler, this);

  _subOdomAftMapped = node.subscribe<nav_msgs::Odometry>(
      _mapOdomTopic, 5, &TransformMaintenance::odomAftMappedHandler, this);

  return true;
}

void TransformMaintenance::laserOdometryHandler(
    const nav_msgs::Odometry::ConstPtr &laserOdometry) {
  double roll, pitch, yaw;
  geometry_msgs::Quaternion geoQuat = laserOdometry->pose.pose.orientation;
  tf::Matrix3x3(tf::Quaternion(geoQuat.z, -geoQuat.x, -geoQuat.y, geoQuat.w))
      .getRPY(roll, pitch, yaw);

  updateOdometry(-pitch, -yaw, roll, laserOdometry->pose.pose.position.x,
                 laserOdometry->pose.pose.position.y,
                 laserOdometry->pose.pose.position.z);

  transformAssociateToMap();

  geoQuat = tf::createQuaternionMsgFromRollPitchYaw(
      transformMapped()[2], -transformMapped()[0], -transformMapped()[1]);

  std::vector<double> pose_covariance(6, 0);
  ros::param::get("lidarOdomCov", pose_covariance);
  _laserOdometry2.header.stamp = laserOdometry->header.stamp;
  _laserOdometry2.pose.pose.orientation.x = -geoQuat.y;
  _laserOdometry2.pose.pose.orientation.y = -geoQuat.z;
  _laserOdometry2.pose.pose.orientation.z = geoQuat.x;
  _laserOdometry2.pose.pose.orientation.w = geoQuat.w;
  _laserOdometry2.pose.pose.position.x = transformMapped()[3];
  _laserOdometry2.pose.pose.position.y = transformMapped()[4];
  _laserOdometry2.pose.pose.position.z = transformMapped()[5];
  _laserOdometry2.pose.covariance[0] = pose_covariance[0];
  _laserOdometry2.pose.covariance[7] = pose_covariance[1];
  _laserOdometry2.pose.covariance[14] = pose_covariance[2];
  _laserOdometry2.pose.covariance[21] = pose_covariance[3];
  _laserOdometry2.pose.covariance[28] = pose_covariance[4];
  _laserOdometry2.pose.covariance[35] = pose_covariance[5];
  _pubLaserOdometry2.publish(_laserOdometry2);

  bool outputTransform;
  ros::param::get("outputTransforms", outputTransform);
  if (outputTransform) {
    _laserOdometryTrans2.stamp_ = laserOdometry->header.stamp;
    _laserOdometryTrans2.setRotation(
        tf::Quaternion(-geoQuat.y, -geoQuat.z, geoQuat.x, geoQuat.w));
    _laserOdometryTrans2.setOrigin(tf::Vector3(
        transformMapped()[3], transformMapped()[4], transformMapped()[5]));
    _tfBroadcaster2.sendTransform(_laserOdometryTrans2);
  }
}

void TransformMaintenance::odomAftMappedHandler(
    const nav_msgs::Odometry::ConstPtr &odomAftMapped) {
  double roll, pitch, yaw;
  geometry_msgs::Quaternion geoQuat = odomAftMapped->pose.pose.orientation;
  tf::Matrix3x3(tf::Quaternion(geoQuat.z, -geoQuat.x, -geoQuat.y, geoQuat.w))
      .getRPY(roll, pitch, yaw);

  updateMappingTransform(
      -pitch, -yaw, roll, odomAftMapped->pose.pose.position.x,
      odomAftMapped->pose.pose.position.y, odomAftMapped->pose.pose.position.z,

      odomAftMapped->twist.twist.angular.x,
      odomAftMapped->twist.twist.angular.y,
      odomAftMapped->twist.twist.angular.z,

      odomAftMapped->twist.twist.linear.x, odomAftMapped->twist.twist.linear.y,
      odomAftMapped->twist.twist.linear.z);
}

} // end namespace loam
