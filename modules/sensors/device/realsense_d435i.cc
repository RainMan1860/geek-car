/******************************************************************************
 * MIT License

 * Copyright (c) 2019 Geekstyle

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
******************************************************************************/

#include "modules/sensors/device/realsense_d435i.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "pcl/common/transforms.h"
#include "pcl/filters/passthrough.h"

#include "modules/common/global_gflags.h"

namespace apollo {
namespace sensors {
namespace device {

using pcl_ptr = pcl::PointCloud<pcl::PointXYZ>::Ptr;

bool D435I::Init() {
  // 1. Init Device
  DeviceConfig();
  // 2. Channel Writer Config
  InitChannelWriter();

  // 3. Concurrent object pool for point cloud
  point_cloud_pool_.reset(new CCObjectPool<PointCloud>(pool_size_));
  point_cloud_pool_->ConstructAll();
  for (int i = 0; i < pool_size_; i++) {
    auto point_cloud = point_cloud_pool_->GetObject();
    if (point_cloud == nullptr) {
      AERROR << "Failed to Getobject, i: " << i;
      return false;
    }
    point_cloud->mutable_point()->Reserve(point_size_);
  }

  // Thread to handle frames
  async_result_ = cyber::Async(&RealsenseComponent::Run, this);

  // Thread to get point cloud from frame queue, and publish
  std::thread(&RealsenseComponent::PublishPointCloud, this).detach();
  AINFO << "Device Realsense D435I Init succussfuly";
  return true;
}

void D435I::DeviceConfig() {
  // Add desired streams to configuration
  cfg.enable_stream(RS2_STREAM_COLOR, FLAGS_color_image_width,
                    FLAGS_color_image_height, RS2_FORMAT_BGR8,
                    FLAGS_color_image_frequency);
  // Use a configuration object to request only depth from the pipeline
  cfg.enable_stream(RS2_STREAM_DEPTH, FLAGS_color_image_width,
                    FLAGS_color_image_height, RS2_FORMAT_Z16,
                    FLAGS_color_image_frequency);
  // Add streams of gyro and accelerometer to configuration
  cfg.enable_stream(RS2_STREAM_ACCEL, RS2_FORMAT_MOTION_XYZ32F);
  cfg.enable_stream(RS2_STREAM_GYRO, RS2_FORMAT_MOTION_XYZ32F);

  // Instruct pipeline to start streaming with the requested configuration
  auto profile = pipe.start(cfg);
  auto sensor = profile.get_device().first<rs2::depth_sensor>();

  // Set the device to High Accuracy preset of the D400 stereoscopic cameras
  if (sensor && sensor.is<rs2::depth_stereo_sensor>()) {
    sensor.set_option(RS2_OPTION_VISUAL_PRESET,
                      RS2_RS400_VISUAL_PRESET_HIGH_ACCURACY);
  }
}

void D435I::InitChannelWriter() {
  // Channel Writer flags
  if (FLAGS_publish_color_image) {
    color_image_writer_ = node_->CreateWriter<Image>(FLAGS_color_image_channel);
  }

  // Point cloud channel
  if (FLAGS_publish_point_cloud) {
    point_cloud_writer_ =
        node_->CreateWriter<PointCloud>(FLAGS_point_cloud_channel);
  }

  // acc channel
  if (FLAGS_publish_acc) {
    acc_writer_ = node_->CreateWriter<Acc>(FLAGS_acc_channel);
  }

  // gyro channel
  if (FLAGS_publish_gyro) {
    gyro_writer_ = node_->CreateWriter<Gyro>(FLAGS_gyro_channel);
  }

  // compreessed image channel
  if (FLAGS_publish_compressed_color_image) {
    compressed_image_writer_ =
        node_->CreateWriter<Image>(FLAGS_compressed_color_image_channel);
  }
}

void D435I::Run() {
  while (!apollo::cyber::IsShutdown()) {
    rs2::frameset frames;
    // Wait for all configured streams to produce a frame
    frames = pipe.wait_for_frames();

    if (FLAGS_publish_color_image) {
      rs2::frame color_frame = frames.get_color_frame();
      OnColorImage(color_frame);
    }

    if (FLAGS_publish_acc) {
      rs2::motion_frame accel_frame = frames.first_or_default(RS2_STREAM_ACCEL);
      OnAcc(accel_frame);
    }

    if (FLAGS_publish_gyro) {
      rs2::motion_frame gyro_frame = frames.first_or_default(RS2_STREAM_GYRO);
      OnGyro(gyro_frame);
    }

    if (FLAGS_publish_point_cloud) {
      // Calculate the angle
      if (FLAGS_enable_point_cloud_transform) {
        auto angle = algo_.get_theta();
        AINFO << "CALCULATED ANGLE X:" << angle.x << " Z:" << angle.z;

        transform = Eigen::Matrix4f::Identity();
        ::Eigen::Vector3d ea0(0, angle.x, angle.z);
        ::Eigen::Matrix3d R;
        R = ::Eigen::AngleAxisd(ea0[0], ::Eigen::Vector3d::UnitZ()) *
            ::Eigen::AngleAxisd(ea0[1], ::Eigen::Vector3d::UnitY()) *
            ::Eigen::AngleAxisd(ea0[2], ::Eigen::Vector3d::UnitX());
        std::cout << "ROTATION :" << R << std::endl << std::endl;
        Eigen::MatrixXf RR = R.cast<float>();
        transform(0, 0) = RR(0, 0);
        transform(0, 1) = RR(0, 1);
        transform(0, 2) = RR(0, 2);
        transform(1, 0) = RR(1, 0);
        transform(1, 1) = RR(1, 1);
        transform(1, 2) = RR(1, 2);
        transform(2, 0) = RR(2, 0);
        transform(2, 1) = RR(2, 1);
        transform(2, 2) = RR(2, 2);
        std::cout << "TRANSFORM:" << transform << std::endl << std::endl;
      }

      rs2::frame depth_frame = frames.get_depth_frame();
      OnPointCloud(depth_frame);
    }
  }
}

void RealsenseComponent::OnColorImage(const rs2::frame &color_frame) {
  // Creating OpenCV Matrix from a color image
  cv::Mat mat(cv::Size(FLAGS_color_image_width, FLAGS_color_image_height),
              CV_8UC3, const_cast<void *>(color_frame.get_data()),
              cv::Mat::AUTO_STEP);
  AINFO << "FRAME NUMBER:" << color_frame.get_frame_number();
  auto image_proto = std::make_shared<Image>();
  image_proto->set_frame_no(color_frame.get_frame_number());
  image_proto->set_height(mat.rows);
  image_proto->set_width(mat.cols);
  // encoding /**< 16-bit linear depth values. The depth is meters is equal to
  // depth scale * pixel value. */
  image_proto->set_encoding(
      rs2_format_to_string(color_frame.get_profile().format()));

  image_proto->set_measurement_time(color_frame.get_timestamp());
  auto m_size = mat.rows * mat.cols * mat.elemSize();
  image_proto->set_data(mat.data, m_size);
  color_image_writer_->Write(image_proto);

  if (FLAGS_publish_compressed_color_image) {
    OnCompressedImage(color_frame, mat);
  }
}

void D435I::OnPointCloud(rs2::frame depth_frame) {
  rs2::threshold_filter thr_filter(
      static_cast<float>(FLAGS_point_cloud_min_distance),
      static_cast<float>(FLAGS_point_cloud_max_distance));

  depth_frame = thr_filter.process(depth_frame);

  filtered_data.enqueue(depth_frame);
}

void D435I::PublishPointCloud() {
  while (!apollo::cyber::IsShutdown()) {
    AINFO << "ENTERED PublishPointCloud METHOD";
    // Declare pointcloud object, for calculating pointclouds and texture
    // mappings
    rs2::pointcloud pc;
    // We want the points object to be persistent so we can display the last
    // cloud when a frame drops
    rs2::points points;

    rs2::frame depth_frame;
    filtered_data.poll_for_frame(&depth_frame);
    if (!depth_frame) {
      AINFO << "FRAME QUEUE IS EMPTY, WAIT FOR ENQUEUE;";
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      continue;
    }
    // Generate the pointcloud and texture mappings
    points = pc.calculate(depth_frame);

    auto t1 = Time::Now().ToSecond();
    auto pcl_points = points_to_pcl(points);
    auto t2 = Time::Now().ToSecond();
    AINFO << "time for realsense point to point cloud:" << t2 - t1;

    pcl_ptr cloud_(new pcl::PointCloud<pcl::PointXYZ>);

    if (FLAGS_enable_point_cloud_transform) {
      // Apply an affine transform defined by an Eigen Transform.
      pcl::transformPointCloud(*pcl_points, *cloud_, transform);
    } else {
      *cloud_ = *pcl_points;
    }

    auto sp = points.get_profile().as<rs2::video_stream_profile>();

    std::shared_ptr<PointCloud> point_cloud_out =
        point_cloud_pool_->GetObject();
    if (point_cloud_out == nullptr) {
      AWARN << "point cloud pool return nullptr, will be create new.";
      point_cloud_out = std::make_shared<PointCloud>();
      point_cloud_out->mutable_point()->Reserve(point_size_);
    }
    if (point_cloud_out == nullptr) {
      AWARN << "point cloud out is nullptr";
      return;
    }
    point_cloud_out->Clear();

    point_cloud_out->set_is_dense(false);
    point_cloud_out->set_measurement_time(Time::Now().ToSecond());
    point_cloud_out->set_width(sp.width());
    point_cloud_out->set_height(sp.height());

    for (size_t i = 0; i < (*cloud_).size(); i++) {
      if ((*cloud_)[i].z) {
        apollo::sensors::PointXYZIT *p = point_cloud_out->add_point();
        p->set_x((*cloud_)[i].x);
        p->set_y((*cloud_)[i].y);
        p->set_z((*cloud_)[i].z);
        // p->set_intensity(0);
        // p->set_timestamp(depth_frame.get_timestamp());
      }
    }

    auto tt = Time::Now().ToSecond();
    AINFO << "Time for point cloud from collect to publish :" << tt - t1;

    point_cloud_writer_->Write(point_cloud_out);
  }
}

void D435I::OnCompressedImage(const rs2::frame &f, cv::Mat raw_image) {
  std::vector<int> param = std::vector<int>(2);
  param[0] = CV_IMWRITE_JPEG_QUALITY;
  param[1] = FLAGS_compress_rate;
  cv::Mat tmp_mat;
  cv::cvtColor(raw_image, tmp_mat, cv::COLOR_RGB2BGR);
  std::vector<uchar> data_encode;
  cv::imencode(".jpeg", tmp_mat, data_encode, param);
  std::string str_encode(data_encode.begin(), data_encode.end());

  auto compressedimage = std::make_shared<CompressedImage>();
  compressedimage->set_frame_no(f.get_frame_number());
  compressedimage->set_format("jpeg");
  compressedimage->set_measurement_time(f.get_timestamp());
  compressedimage->set_data(str_encode);

  compressed_image_writer_->Write(compressedimage);
}

}  // namespace device
}  // namespace sensors
}  // namespace apollo
