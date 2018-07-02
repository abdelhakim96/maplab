#include <memory>

#include <vi-map/vi-map-serialization.h>
#include <minkindr_conversions/kindr_xml.h>

#include "voxblox_ros_interface/voxblox_bag_importer.h"

namespace voxblox {

VoxbloxBagImporter::VoxbloxBagImporter(const ros::NodeHandle& nh,
                                       const ros::NodeHandle& nh_private)
    : nh_(nh),
      nh_private_(nh_private),
      integrate_every_nth_message_(1),
      tsdf_server_(nh, nh_private),
      message_index_(0) {}

void VoxbloxBagImporter::setSubsampling(int integrate_every_nth_message) {
  integrate_every_nth_message_ = integrate_every_nth_message;
}

bool VoxbloxBagImporter::setupRosbag(const std::string& filename) {
  try {
    bag_.open(filename, rosbag::bagmode::Read);
  } catch (const std::exception& ex) {  // NOLINT
    ROS_ERROR_STREAM("Couldn't open rosbag at path " << filename);
    return false;
  }

 return true;
}

bool VoxbloxBagImporter::setupPointcloudSensor(
    const std::string& pointcloud_topic,
    const std::string& camchain_namespace) {
  pointcloud_topic_ = pointcloud_topic;
  XmlRpc::XmlRpcValue T_imu_pointcloud_xml;
  CHECK(nh_private_.getParam(camchain_namespace + "/cam0/T_cam_imu",
                             T_imu_pointcloud_xml))
      << "please provide a path to the pointcloud sensor extrinsic calibration "
         "file";
  kindr::minimal::xmlRpcToKindr(T_imu_pointcloud_xml, &T_I_P_);

  return true;
}

bool VoxbloxBagImporter::setupStereoSensor(
    const std::string& cam0_topic, const std::string& cam1_topic,
    const std::string& camchain_namespace) {
  cam0_topic_ = cam0_topic;
  cam1_topic_ = cam1_topic;

  XmlRpc::XmlRpcValue T_imu_cam0_xml;
  CHECK(nh_private_.getParam(camchain_namespace + "cam0/T_cam_imu",
                             T_imu_cam0_xml))
      << "please provide a path to the stereo camera extrinsic calibration "
         "file";
  kindr::minimal::xmlRpcToKindr(T_imu_cam0_xml, &T_I_C0_);

  XmlRpc::XmlRpcValue T_imu_cam1_xml;
  CHECK(nh_private_.getParam(camchain_namespace + "cam1/T_cam_imu",
                             T_imu_cam1_xml))
      << "please provide a path to the stereo camera extrinsic calibration "
         "file";
  kindr::minimal::xmlRpcToKindr(T_imu_cam1_xml, &T_I_C1_);

  return true;
}

bool VoxbloxBagImporter::setupMap(const std::string& map_path) {
  const bool result =
      vi_map::serialization::loadMapFromFolder(map_path, &vi_map_);
  if (result != true) {
    return false;
  }

  // Figure out the mission ID. We always just take the first mission in the
  // thing.
  vi_map::MissionIdList mission_ids;
  vi_map_.getAllMissionIds(&mission_ids);
  if (mission_ids.size() < 1) {
    return false;
  }
  mission_id_ = mission_ids[0];

  // Fill in min and max timestamps for future pose lookups.
  landmark_triangulation::VertexToTimeStampMap vertex_to_time_map;
  pose_interpolator_.getVertexToTimeStampMap(
      vi_map_, mission_id_, &vertex_to_time_map, &min_timestamp_ns_,
      &max_timestamp_ns_);
  if (vertex_to_time_map.empty()) {
    return false;
  }
  return true;
}

bool VoxbloxBagImporter::lookupTransformInMap(int64_t timestamp_ns,
                                              voxblox::Transformation* T_G_I) {
  if (timestamp_ns < min_timestamp_ns_ || timestamp_ns > max_timestamp_ns_) {
    return false;
  }

  // Interpolate poses at resource timestamp.
  // You have to feed a vector in, no way to just look up one pose. :(

  aslam::TransformationVector poses_M_I;
  const landmark_triangulation::PoseInterpolator pose_interpolator;

  Eigen::Matrix<int64_t, 1, Eigen::Dynamic> resource_timestamps(1);
  resource_timestamps << timestamp_ns;
  size_t index = 0;

  pose_interpolator.getPosesAtTime(vi_map_, mission_id_, resource_timestamps,
                                   &poses_M_I);

  const aslam::Transformation& T_G_M =
      vi_map_.getMissionBaseFrameForMission(mission_id_).get_T_G_M();
  const aslam::Transformation& T_M_I = poses_M_I[index];

  // TODO(helenol): check WTF this mistake was!
  *T_G_I = (T_G_M * T_M_I).cast<FloatingPoint>()/* * T_I_C_*/;

  return true;
}

void VoxbloxBagImporter::pointcloudCallback(
    sensor_msgs::PointCloud2Ptr pointcloud) {
  if (message_index_++ % integrate_every_nth_message_ != 0) {
    return;
  }

  voxblox::Transformation T_G_I;
  const int64_t timestamp_ns = pointcloud->header.stamp.toNSec();
  if (!lookupTransformInMap(timestamp_ns, &T_G_I)) {
    return;
  }

  voxblox::Transformation T_G_C = T_G_I * T_I_P_;
  const bool is_freespace_pointcloud = false;

  tsdf_server_.processPointCloudMessageAndInsert(pointcloud, T_G_C,
                                                 is_freespace_pointcloud);
}

void VoxbloxBagImporter::run() {
  // TODO(helenol): read bag here!!!!
  return;
 }

void VoxbloxBagImporter::visualize() { tsdf_server_.generateMesh(); }

size_t VoxbloxBagImporter::numMessages() const { return message_index_; }
int VoxbloxBagImporter::getSubsampling() const {
  return integrate_every_nth_message_;
}

void VoxbloxBagImporter::save(const std::string& output_path) {
  tsdf_server_.saveMap(output_path);
}

}  // namespace voxblox