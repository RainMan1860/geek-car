#pragma once

#include <librealsense2/rs.hpp>
#include <memory>
#include <opencv2/opencv.hpp>

#include "cyber/class_loader/class_loader.h"
#include "cyber/component/component.h"
#include "modules/sensors/proto/sensors.pb.h"

namespace apollo {
namespace sensors {
namespace realsense {

using apollo::cyber::Component;
using apollo::cyber::Writer;
using apollo::sensors::Image;

class RealsenseComponent : public Component<> {
 public:
  bool Init() override;
  bool Proc() override;

 private:
  bool OnImage(cv::Mat dst);
  bool OnPose(rs2::rs2_pose pose_data);
  std::shared_ptr<Writer<Image>> image_writer_ = nullptr;
  std::shared_ptr<Writer<Pose>> pose_writer_ = nullptr;
  // realsense device
  std::shared_ptr<rs2::device> device_;
  // sensor include imu and camera;
  rs2::sensor sensor_;
  // pipe
  rs2::pipeline pipe_;
  // Create a configuration for configuring the pipeline with a non default
  // profile
  rs2::config cfg_;
  cv::Mat intrinsicsL;
  cv::Mat distCoeffsL;
  cv::Mat map1_;
  cv::Mat map2_;

  uint64_t pose_counter_ = 0;
  uint64_t frame_counter_ = 0;
};

CYBER_REGISTER_COMPONENT(RealsenseComponent)
}  // namespace realsense
}  // namespace sensors
}  // namespace apollo