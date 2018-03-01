// COLMAP - Structure-from-Motion and Multi-View Stereo.
// Copyright (C) 2017  Johannes L. Schoenberger <jsch at inf.ethz.ch>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "base/undistortion.h"

#include <fstream>

#include "base/pose.h"
#include "base/warp.h"
#include "util/misc.h"

namespace colmap {
namespace {

template <typename Derived>
void WriteMatrix(const Eigen::MatrixBase<Derived>& matrix,
                 std::ofstream* file) {
  typedef typename Eigen::MatrixBase<Derived>::Index index_t;
  for (index_t r = 0; r < matrix.rows(); ++r) {
    for (index_t c = 0; c < matrix.cols() - 1; ++c) {
      *file << matrix(r, c) << " ";
    }
    *file << matrix(r, matrix.cols() - 1) << std::endl;
  }
}

// Write projection matrix P = K * [R t] to file and prepend given header.
void WriteProjectionMatrix(const std::string& path, const Camera& camera,
                           const Image& image, const std::string& header) {
  CHECK_EQ(camera.ModelId(), PinholeCameraModel::model_id);

  std::ofstream file(path, std::ios::trunc);
  CHECK(file.is_open()) << path;

  Eigen::Matrix3d calib_matrix = Eigen::Matrix3d::Identity();
  calib_matrix(0, 0) = camera.FocalLengthX();
  calib_matrix(1, 1) = camera.FocalLengthY();
  calib_matrix(0, 2) = camera.PrincipalPointX();
  calib_matrix(1, 2) = camera.PrincipalPointY();

  const Eigen::Matrix3x4d proj_matrix = calib_matrix * image.ProjectionMatrix();

  if (!header.empty()) {
    file << header << std::endl;
  }

  WriteMatrix(proj_matrix, &file);
}

void WriteCOLMAPCommands(const bool geometric,
                         const std::string& workspace_path,
                         const std::string& workspace_format,
                         const std::string& pmvs_option_name,
                         const std::string& output_prefix,
                         const std::string& indent, std::ofstream* file) {
  if (geometric) {
    *file << indent << "$COLMAP_EXE_PATH/dense_stereo \\" << std::endl;
    *file << indent << "  --workspace_path " << workspace_path << " \\"
          << std::endl;
    *file << indent << "  --workspace_format " << workspace_format << " \\"
          << std::endl;
    if (workspace_format == "PMVS") {
      *file << indent << "  --pmvs_option_name " << pmvs_option_name << " \\"
            << std::endl;
    }
    *file << indent << "  --DenseStereo.max_image_size 2000 \\" << std::endl;
    *file << indent << "  --DenseStereo.geom_consistency true" << std::endl;
  } else {
    *file << indent << "$COLMAP_EXE_PATH/dense_stereo \\" << std::endl;
    *file << indent << "  --workspace_path " << workspace_path << " \\"
          << std::endl;
    *file << indent << "  --workspace_format " << workspace_format << " \\"
          << std::endl;
    if (workspace_format == "PMVS") {
      *file << indent << "  --pmvs_option_name " << pmvs_option_name << " \\"
            << std::endl;
    }
    *file << indent << "  --DenseStereo.max_image_size 2000 \\" << std::endl;
    *file << indent << "  --DenseStereo.geom_consistency false" << std::endl;
  }

  *file << indent << "$COLMAP_EXE_PATH/dense_fuser \\" << std::endl;
  *file << indent << "  --workspace_path " << workspace_path << " \\"
        << std::endl;
  *file << indent << "  --workspace_format " << workspace_format << " \\"
        << std::endl;
  if (workspace_format == "PMVS") {
    *file << indent << "  --pmvs_option_name " << pmvs_option_name << " \\"
          << std::endl;
  }
  if (geometric) {
    *file << indent << "  --input_type geometric \\" << std::endl;
  } else {
    *file << indent << "  --input_type photometric \\" << std::endl;
  }
  *file << indent << "  --output_path "
        << JoinPaths(workspace_path, output_prefix + "fused.ply") << std::endl;

  *file << indent << "$COLMAP_EXE_PATH/dense_mesher \\" << std::endl;
  *file << indent << "  --input_path "
        << JoinPaths(workspace_path, output_prefix + "fused.ply") << " \\"
        << std::endl;
  *file << indent << "  --output_path "
        << JoinPaths(workspace_path, output_prefix + "meshed.ply") << std::endl;
}

Eigen::Vector2d SelectPointOnRay(const Camera& camera,
                                 const Eigen::Vector2d& origin,
                                 const Eigen::Vector2d& target,
                                 double max_length, double max_angle,
                                 double max_horizontal_angle,
                                 double max_vertical_angle) {
  const Eigen::Vector2d diff = target - origin;
  const Eigen::Vector2d dir = diff.normalized();

  double l = 0.0, r = std::min(max_length, diff.norm());
  for (int i = 0; i < 32; ++i) {
    double m = (r + l) / 2.0;
    const Eigen::Vector2d world_point = camera.ImageToWorld(origin + dir * m);
    const double vertical_angle = std::atan(world_point[1]),
                 horizontal_angle = std::atan(world_point[0]),
                 full_angle = std::atan(world_point.norm());
    const bool m_valid = vertical_angle < max_vertical_angle &&
                         horizontal_angle < max_horizontal_angle &&
                         full_angle < max_angle;

    if (m_valid) {
      l = m;
    } else {
      r = m;
    }
  }
  return origin + dir * l;
}

}  // namespace

COLMAPUndistorter::COLMAPUndistorter(const UndistortCameraOptions& options,
                                     const Reconstruction& reconstruction,
                                     const std::string& image_path,
                                     const std::string& output_path)
    : options_(options),
      image_path_(image_path),
      output_path_(output_path),
      reconstruction_(reconstruction) {}

void COLMAPUndistorter::Run() {
  PrintHeading1("Image undistortion");

  CreateDirIfNotExists(JoinPaths(output_path_, "images"));
  CreateDirIfNotExists(JoinPaths(output_path_, "sparse"));
  CreateDirIfNotExists(JoinPaths(output_path_, "stereo"));
  CreateDirIfNotExists(JoinPaths(output_path_, "stereo/depth_maps"));
  CreateDirIfNotExists(JoinPaths(output_path_, "stereo/normal_maps"));
  CreateDirIfNotExists(JoinPaths(output_path_, "stereo/consistency_graphs"));
  reconstruction_.CreateImageDirs(JoinPaths(output_path_, "images"));
  reconstruction_.CreateImageDirs(JoinPaths(output_path_, "stereo/depth_maps"));
  reconstruction_.CreateImageDirs(
      JoinPaths(output_path_, "stereo/normal_maps"));
  reconstruction_.CreateImageDirs(
      JoinPaths(output_path_, "stereo/consistency_graphs"));

  ThreadPool thread_pool;
  std::vector<std::future<void>> futures;
  futures.reserve(reconstruction_.NumRegImages());
  for (size_t i = 0; i < reconstruction_.NumRegImages(); ++i) {
    futures.push_back(
        thread_pool.AddTask(&COLMAPUndistorter::Undistort, this, i));
  }

  for (size_t i = 0; i < futures.size(); ++i) {
    if (IsStopped()) {
      break;
    }

    std::cout << StringPrintf("Undistorting image [%d/%d]", i + 1,
                              futures.size())
              << std::endl;

    futures[i].get();
  }

  std::cout << "Writing reconstruction..." << std::endl;
  Reconstruction undistorted_reconstruction = reconstruction_;
  UndistortReconstruction(options_, &undistorted_reconstruction);
  undistorted_reconstruction.Write(JoinPaths(output_path_, "sparse"));

  std::cout << "Writing configuration..." << std::endl;
  WritePatchMatchConfig();
  WriteFusionConfig();

  std::cout << "Writing scripts..." << std::endl;
  WriteScript(false);
  WriteScript(true);

  GetTimer().PrintMinutes();
}

void COLMAPUndistorter::Undistort(const size_t reg_image_idx) const {
  const image_t image_id = reconstruction_.RegImageIds().at(reg_image_idx);
  const Image& image = reconstruction_.Image(image_id);
  const Camera& camera = reconstruction_.Camera(image.CameraId());

  const std::string output_image_path =
      JoinPaths(output_path_, "images", image.Name());

  Bitmap distorted_bitmap;
  const std::string input_image_path = JoinPaths(image_path_, image.Name());
  if (!distorted_bitmap.Read(input_image_path)) {
    std::cerr << "ERROR: Cannot read image at path " << input_image_path
              << std::endl;
    return;
  }

  Bitmap undistorted_bitmap;
  Camera undistorted_camera;
  UndistortImage(options_, distorted_bitmap, camera, &undistorted_bitmap,
                 &undistorted_camera);

  undistorted_bitmap.Write(output_image_path);
}

void COLMAPUndistorter::WritePatchMatchConfig() const {
  const auto path = JoinPaths(output_path_, "stereo/patch-match.cfg");
  std::ofstream file(path, std::ios::trunc);
  CHECK(file.is_open()) << path;
  for (const auto image_id : reconstruction_.RegImageIds()) {
    const auto& image = reconstruction_.Image(image_id);
    file << image.Name() << std::endl;
    file << "__auto__, 20" << std::endl;
  }
}

void COLMAPUndistorter::WriteFusionConfig() const {
  const auto path = JoinPaths(output_path_, "stereo/fusion.cfg");
  std::ofstream file(path, std::ios::trunc);
  CHECK(file.is_open()) << path;
  for (const auto image_id : reconstruction_.RegImageIds()) {
    const auto& image = reconstruction_.Image(image_id);
    file << image.Name() << std::endl;
  }
}

void COLMAPUndistorter::WriteScript(const bool geometric) const {
  const std::string path =
      JoinPaths(output_path_, geometric ? "run-colmap-geometric.sh"
                                        : "run-colmap-photometric.sh");
  std::ofstream file(path, std::ios::trunc);
  CHECK(file.is_open()) << path;

  file << "# You must set $COLMAP_EXE_PATH to " << std::endl
       << "# the directory containing the COLMAP executables." << std::endl;
  WriteCOLMAPCommands(geometric, ".", "COLMAP", "option-all", "", "", &file);
}

PMVSUndistorter::PMVSUndistorter(const UndistortCameraOptions& options,
                                 const Reconstruction& reconstruction,
                                 const std::string& image_path,
                                 const std::string& output_path)
    : options_(options),
      image_path_(image_path),
      output_path_(output_path),
      reconstruction_(reconstruction) {}

void PMVSUndistorter::Run() {
  PrintHeading1("Image undistortion (CMVS/PMVS)");

  CreateDirIfNotExists(JoinPaths(output_path_, "pmvs"));
  CreateDirIfNotExists(JoinPaths(output_path_, "pmvs/txt"));
  CreateDirIfNotExists(JoinPaths(output_path_, "pmvs/visualize"));
  CreateDirIfNotExists(JoinPaths(output_path_, "pmvs/models"));

  ThreadPool thread_pool;
  std::vector<std::future<void>> futures;
  futures.reserve(reconstruction_.NumRegImages());
  for (size_t i = 0; i < reconstruction_.NumRegImages(); ++i) {
    futures.push_back(
        thread_pool.AddTask(&PMVSUndistorter::Undistort, this, i));
  }

  for (size_t i = 0; i < futures.size(); ++i) {
    if (IsStopped()) {
      thread_pool.Stop();
      std::cout << "WARNING: Stopped the undistortion process. Image point "
                   "locations and camera parameters for not yet processed "
                   "images in the Bundler output file is probably wrong."
                << std::endl;
      break;
    }

    std::cout << StringPrintf("Undistorting image [%d/%d]", i + 1,
                              futures.size())
              << std::endl;

    futures[i].get();
  }

  std::cout << "Writing bundle file..." << std::endl;
  Reconstruction undistorted_reconstruction = reconstruction_;
  UndistortReconstruction(options_, &undistorted_reconstruction);
  const std::string bundle_path = JoinPaths(output_path_, "pmvs/bundle.rd.out");
  undistorted_reconstruction.ExportBundler(bundle_path,
                                           bundle_path + ".list.txt");

  std::cout << "Writing visibility file..." << std::endl;
  WriteVisibilityData();

  std::cout << "Writing option file..." << std::endl;
  WriteOptionFile();

  std::cout << "Writing scripts..." << std::endl;
  WritePMVSScript();
  WriteCMVSPMVSScript();
  WriteCOLMAPScript(false);
  WriteCOLMAPScript(true);
  WriteCMVSCOLMAPScript(false);
  WriteCMVSCOLMAPScript(true);

  GetTimer().PrintMinutes();
}

void PMVSUndistorter::Undistort(const size_t reg_image_idx) const {
  const std::string output_image_path = JoinPaths(
      output_path_, StringPrintf("pmvs/visualize/%08d.jpg", reg_image_idx));
  const std::string proj_matrix_path =
      JoinPaths(output_path_, StringPrintf("pmvs/txt/%08d.txt", reg_image_idx));

  const image_t image_id = reconstruction_.RegImageIds().at(reg_image_idx);
  const Image& image = reconstruction_.Image(image_id);
  const Camera& camera = reconstruction_.Camera(image.CameraId());

  Bitmap distorted_bitmap;
  const std::string input_image_path = JoinPaths(image_path_, image.Name());
  if (!distorted_bitmap.Read(input_image_path)) {
    std::cerr << StringPrintf("ERROR: Cannot read image at path %s",
                              input_image_path.c_str())
              << std::endl;
    return;
  }

  Bitmap undistorted_bitmap;
  Camera undistorted_camera;
  UndistortImage(options_, distorted_bitmap, camera, &undistorted_bitmap,
                 &undistorted_camera);

  undistorted_bitmap.Write(output_image_path);
  WriteProjectionMatrix(proj_matrix_path, undistorted_camera, image, "CONTOUR");
}

void PMVSUndistorter::WriteVisibilityData() const {
  const auto path = JoinPaths(output_path_, "pmvs/vis.dat");
  std::ofstream file(path, std::ios::trunc);
  CHECK(file.is_open()) << path;

  file << "VISDATA" << std::endl;
  file << reconstruction_.NumRegImages() << std::endl;

  const std::vector<image_t>& reg_image_ids = reconstruction_.RegImageIds();

  for (size_t i = 0; i < reg_image_ids.size(); ++i) {
    const image_t image_id = reg_image_ids[i];
    const Image& image = reconstruction_.Image(image_id);
    std::unordered_set<image_t> visible_image_ids;
    for (point2D_t point2D_idx = 0; point2D_idx < image.NumPoints2D();
         ++point2D_idx) {
      const Point2D& point2D = image.Point2D(point2D_idx);
      if (point2D.HasPoint3D()) {
        const Point3D& point3D = reconstruction_.Point3D(point2D.Point3DId());
        for (const TrackElement track_el : point3D.Track().Elements()) {
          if (track_el.image_id != image_id) {
            visible_image_ids.insert(track_el.image_id);
          }
        }
      }
    }

    std::vector<image_t> sorted_visible_image_ids(visible_image_ids.begin(),
                                                  visible_image_ids.end());
    std::sort(sorted_visible_image_ids.begin(), sorted_visible_image_ids.end());

    file << i << " " << visible_image_ids.size();
    for (const image_t visible_image_id : sorted_visible_image_ids) {
      file << " " << visible_image_id;
    }
    file << std::endl;
  }
}

void PMVSUndistorter::WritePMVSScript() const {
  const auto path = JoinPaths(output_path_, "run-pmvs.sh");
  std::ofstream file(path, std::ios::trunc);
  CHECK(file.is_open()) << path;

  file << "# You must set $PMVS_EXE_PATH to " << std::endl
       << "# the directory containing the CMVS-PMVS executables." << std::endl;
  file << "$PMVS_EXE_PATH/pmvs2 pmvs/ option-all" << std::endl;
}

void PMVSUndistorter::WriteCMVSPMVSScript() const {
  const auto path = JoinPaths(output_path_, "run-cmvs-pmvs.sh");
  std::ofstream file(path, std::ios::trunc);
  CHECK(file.is_open()) << path;

  file << "# You must set $PMVS_EXE_PATH to " << std::endl
       << "# the directory containing the CMVS-PMVS executables." << std::endl;
  file << "$PMVS_EXE_PATH/cmvs pmvs/" << std::endl;
  file << "$PMVS_EXE_PATH/genOption pmvs/" << std::endl;
  file << "find pmvs/ -iname \"option-*\" | sort | while read file_name"
       << std::endl;
  file << "do" << std::endl;
  file << "    option_name=$(basename \"$file_name\")" << std::endl;
  file << "    if [ \"$option_name\" = \"option-all\" ]; then" << std::endl;
  file << "        continue" << std::endl;
  file << "    fi" << std::endl;
  file << "    $PMVS_EXE_PATH/pmvs2 pmvs/ $option_name" << std::endl;
  file << "done" << std::endl;
}

void PMVSUndistorter::WriteCOLMAPScript(const bool geometric) const {
  const std::string path =
      JoinPaths(output_path_, geometric ? "run-colmap-geometric.sh"
                                        : "run-colmap-photometric.sh");
  std::ofstream file(path, std::ios::trunc);
  CHECK(file.is_open()) << path;

  file << "# You must set $COLMAP_EXE_PATH to " << std::endl
       << "# the directory containing the COLMAP executables." << std::endl;
  WriteCOLMAPCommands(geometric, "pmvs", "PMVS", "option-all", "option-all-",
                      "", &file);
}

void PMVSUndistorter::WriteCMVSCOLMAPScript(const bool geometric) const {
  const std::string path =
      JoinPaths(output_path_, geometric ? "run-cmvs-colmap-geometric.sh"
                                        : "run-cmvs-colmap-photometric.sh");
  std::ofstream file(path, std::ios::trunc);
  CHECK(file.is_open()) << path;

  file << "# You must set $PMVS_EXE_PATH to " << std::endl
       << "# the directory containing the CMVS-PMVS executables" << std::endl;
  file << "# and you must set $COLMAP_EXE_PATH to " << std::endl
       << "# the directory containing the COLMAP executables." << std::endl;
  file << "$PMVS_EXE_PATH/cmvs pmvs/" << std::endl;
  file << "$PMVS_EXE_PATH/genOption pmvs/" << std::endl;
  file << "find pmvs/ -iname \"option-*\" | sort | while read file_name"
       << std::endl;
  file << "do" << std::endl;
  file << "    workspace_path=$(dirname \"$file_name\")" << std::endl;
  file << "    option_name=$(basename \"$file_name\")" << std::endl;
  file << "    if [ \"$option_name\" = \"option-all\" ]; then" << std::endl;
  file << "        continue" << std::endl;
  file << "    fi" << std::endl;
  file << "    rm -rf \"$workspace_path/stereo\"" << std::endl;
  WriteCOLMAPCommands(geometric, "pmvs", "PMVS", "$option_name",
                      "$option_name-", "    ", &file);
  file << "done" << std::endl;
}

void PMVSUndistorter::WriteOptionFile() const {
  const auto path = JoinPaths(output_path_, "pmvs/option-all");
  std::ofstream file(path, std::ios::trunc);
  CHECK(file.is_open()) << path;

  file << "# Generated by COLMAP - all images, no clustering." << std::endl;

  file << "level 1" << std::endl;
  file << "csize 2" << std::endl;
  file << "threshold 0.7" << std::endl;
  file << "wsize 7" << std::endl;
  file << "minImageNum 3" << std::endl;
  file << "CPU " << std::thread::hardware_concurrency() << std::endl;
  file << "setEdge 0" << std::endl;
  file << "useBound 0" << std::endl;
  file << "useVisData 1" << std::endl;
  file << "sequence -1" << std::endl;
  file << "maxAngle 10" << std::endl;
  file << "quad 2.0" << std::endl;

  file << "timages " << reconstruction_.NumRegImages();
  for (size_t i = 0; i < reconstruction_.NumRegImages(); ++i) {
    file << " " << i;
  }
  file << std::endl;

  file << "oimages 0" << std::endl;
}

CMPMVSUndistorter::CMPMVSUndistorter(const UndistortCameraOptions& options,
                                     const Reconstruction& reconstruction,
                                     const std::string& image_path,
                                     const std::string& output_path)
    : options_(options),
      image_path_(image_path),
      output_path_(output_path),
      reconstruction_(reconstruction) {}

void CMPMVSUndistorter::Run() {
  PrintHeading1("Image undistortion (CMP-MVS)");

  ThreadPool thread_pool;
  std::vector<std::future<void>> futures;
  futures.reserve(reconstruction_.NumRegImages());
  for (size_t i = 0; i < reconstruction_.NumRegImages(); ++i) {
    futures.push_back(
        thread_pool.AddTask(&CMPMVSUndistorter::Undistort, this, i));
  }

  for (size_t i = 0; i < futures.size(); ++i) {
    if (IsStopped()) {
      break;
    }

    std::cout << StringPrintf("Undistorting image [%d/%d]", i + 1,
                              futures.size())
              << std::endl;

    futures[i].get();
  }

  GetTimer().PrintMinutes();
}

void CMPMVSUndistorter::Undistort(const size_t reg_image_idx) const {
  const std::string output_image_path =
      JoinPaths(output_path_, StringPrintf("%05d.jpg", reg_image_idx + 1));
  const std::string proj_matrix_path =
      JoinPaths(output_path_, StringPrintf("%05d_P.txt", reg_image_idx + 1));

  const image_t image_id = reconstruction_.RegImageIds().at(reg_image_idx);
  const Image& image = reconstruction_.Image(image_id);
  const Camera& camera = reconstruction_.Camera(image.CameraId());

  Bitmap distorted_bitmap;
  const std::string input_image_path = JoinPaths(image_path_, image.Name());
  if (!distorted_bitmap.Read(input_image_path)) {
    std::cerr << "ERROR: Cannot read image at path " << input_image_path
              << std::endl;
    return;
  }

  Bitmap undistorted_bitmap;
  Camera undistorted_camera;
  UndistortImage(options_, distorted_bitmap, camera, &undistorted_bitmap,
                 &undistorted_camera);

  undistorted_bitmap.Write(output_image_path);
  WriteProjectionMatrix(proj_matrix_path, undistorted_camera, image, "CONTOUR");
}

StereoImageRectifier::StereoImageRectifier(
    const UndistortCameraOptions& options, const Reconstruction& reconstruction,
    const std::string& image_path, const std::string& output_path,
    const std::vector<std::pair<image_t, image_t>>& stereo_pairs)
    : options_(options),
      image_path_(image_path),
      output_path_(output_path),
      stereo_pairs_(stereo_pairs),
      reconstruction_(reconstruction) {}

void StereoImageRectifier::Run() {
  PrintHeading1("Stereo rectification");

  ThreadPool thread_pool;
  std::vector<std::future<void>> futures;
  futures.reserve(stereo_pairs_.size());
  for (const auto& stereo_pair : stereo_pairs_) {
    futures.push_back(thread_pool.AddTask(&StereoImageRectifier::Rectify, this,
                                          stereo_pair.first,
                                          stereo_pair.second));
  }

  for (size_t i = 0; i < futures.size(); ++i) {
    if (IsStopped()) {
      break;
    }

    std::cout << StringPrintf("Rectifying image pair [%d/%d]", i + 1,
                              futures.size())
              << std::endl;

    futures[i].get();
  }

  GetTimer().PrintMinutes();
}

void StereoImageRectifier::Rectify(const image_t image_id1,
                                   const image_t image_id2) const {
  const Image& image1 = reconstruction_.Image(image_id1);
  const Image& image2 = reconstruction_.Image(image_id2);
  const Camera& camera1 = reconstruction_.Camera(image1.CameraId());
  const Camera& camera2 = reconstruction_.Camera(image2.CameraId());

  const std::string image_name1 = StringReplace(image1.Name(), "/", "-");
  const std::string image_name2 = StringReplace(image2.Name(), "/", "-");

  const std::string stereo_pair_name =
      StringPrintf("%s-%s", image_name1.c_str(), image_name2.c_str());

  CreateDirIfNotExists(JoinPaths(output_path_, stereo_pair_name));

  const std::string output_image1_path =
      JoinPaths(output_path_, stereo_pair_name, image_name1);
  const std::string output_image2_path =
      JoinPaths(output_path_, stereo_pair_name, image_name2);

  Bitmap distorted_bitmap1;
  const std::string input_image1_path = JoinPaths(image_path_, image1.Name());
  if (!distorted_bitmap1.Read(input_image1_path)) {
    std::cerr << "ERROR: Cannot read image at path " << input_image1_path
              << std::endl;
    return;
  }

  Bitmap distorted_bitmap2;
  const std::string input_image2_path = JoinPaths(image_path_, image2.Name());
  if (!distorted_bitmap2.Read(input_image2_path)) {
    std::cerr << "ERROR: Cannot read image at path " << input_image2_path
              << std::endl;
    return;
  }

  Eigen::Vector4d qvec;
  Eigen::Vector3d tvec;
  ComputeRelativePose(image1.Qvec(), image1.Tvec(), image2.Qvec(),
                      image2.Tvec(), &qvec, &tvec);

  Bitmap undistorted_bitmap1;
  Bitmap undistorted_bitmap2;
  Camera undistorted_camera;
  Eigen::Matrix4d Q;
  RectifyAndUndistortStereoImages(
      options_, distorted_bitmap1, distorted_bitmap2, camera1, camera2, qvec,
      tvec, &undistorted_bitmap1, &undistorted_bitmap2, &undistorted_camera,
      &Q);

  undistorted_bitmap1.Write(output_image1_path);
  undistorted_bitmap2.Write(output_image2_path);

  const auto Q_path = JoinPaths(output_path_, stereo_pair_name, "Q.txt");
  std::ofstream Q_file(Q_path, std::ios::trunc);
  CHECK(Q_file.is_open()) << Q_path;
  WriteMatrix(Q, &Q_file);
}

Camera UndistortCamera(const UndistortCameraOptions& options,
                       const Camera& camera) {
  CHECK_GE(options.blank_pixels, 0);
  CHECK_LE(options.blank_pixels, 1);
  CHECK_GT(options.min_scale, 0.0);
  CHECK_LE(options.min_scale, options.max_scale);
  CHECK_NE(options.max_image_size, 0);
  CHECK_LT(options.max_fov, 180.0);
  CHECK_GT(options.max_fov, 0.0);
  CHECK_LE(options.max_vertical_fov, 180.0);
  CHECK_GT(options.max_vertical_fov, 0.0);
  CHECK_LE(options.max_horizontal_fov, 180.0);
  CHECK_GT(options.max_horizontal_fov, 0.0);

  Camera undistorted_camera;
  undistorted_camera.SetModelId(PinholeCameraModel::model_id);
  undistorted_camera.SetWidth(camera.Width());
  undistorted_camera.SetHeight(camera.Height());

  if (options.camera_model_override.size()) {
    const int camera_model_id =
        CameraModelNameToId(options.camera_model_override);
    undistorted_camera.SetModelId(camera_model_id);
    undistorted_camera.SetParamsFromString(
        options.camera_model_override_params);
    CHECK(undistorted_camera.VerifyParams());
    return undistorted_camera;
  }

  // Estimate maximal valid radius for radial distortion based on monothonicity
  const double max_fov = DegToRad(options.max_fov);
  const double max_horizontal_fov = DegToRad(options.max_horizontal_fov);
  const double max_vertical_fov = DegToRad(options.max_vertical_fov);

  double max_valid_fov_half = max_fov / 2.0;
  Eigen::Vector2d principal_point = Eigen::Vector2d::Zero();
  const Eigen::Vector2d image_size(camera.Width(), camera.Height());
  double max_valid_radius = image_size.norm();
  const Eigen::Vector2d corners[] = {
      Eigen::Vector2d(0, 0), Eigen::Vector2d(camera.Width(), 0),
      Eigen::Vector2d(camera.Height(), camera.Width()),
      Eigen::Vector2d(0, camera.Height())};
  if (camera.PrincipalPointIdxs().size() == 2) {
    principal_point =
        Eigen::Vector2d(camera.PrincipalPointX(), camera.PrincipalPointY());

    double max_radius = 0.0;
    Eigen::Vector2d corner_dir;
    for (int i = 0; i < 4; ++i) {
      const Eigen::Vector2d diff = corners[i] - principal_point;
      const double norm = diff.norm();
      if (norm > max_radius) {
        max_radius = norm;
        corner_dir = diff.normalized();
      }
    }

    double max_valid_fov_half = 0.0;
    for (int i = 1; i < max_radius; ++i) {
      const Eigen::Vector2d world_point =
          camera.ImageToWorld(i * corner_dir + principal_point);
      const double phi = std::atan(world_point.norm());
      if (phi <= max_valid_fov_half || 2.0 * phi > max_fov) break;
      max_valid_fov_half = phi;
      max_valid_radius = i;
    }
  }

  // Copy focal length parameters, or estimate them
  if (options.estimate_focal_length_from_fov) {
    // Estimate focal length preserving diagonal, horizontal and vertical
    // field-of-view
    double focal_diagonal_fov =
        image_size.norm() / 2.0 / std::tan(max_valid_fov_half);
    for (int i = 0; i < 2; ++i) {
      const Eigen::Vector2d cornerA = SelectPointOnRay(
          camera, principal_point, corners[i], max_valid_radius, max_valid_fov_half,
          max_horizontal_fov / 2.0, max_vertical_fov / 2.0);
      const Eigen::Vector2d cornerB = SelectPointOnRay(
          camera, principal_point, corners[i + 2], max_valid_radius,
          max_valid_fov_half, max_horizontal_fov / 2.0, max_vertical_fov / 2.0);
      const double fov = std::atan(camera.ImageToWorld(cornerA).norm()) +
                         std::atan(camera.ImageToWorld(cornerB).norm());
      focal_diagonal_fov = std::max(
          focal_diagonal_fov, image_size.norm() / 2.0 / std::tan(fov / 2.0));
    }

    const Eigen::Vector2d left(
        std::max(0.0, principal_point[0] - max_valid_radius),
        principal_point[1]),
        right(std::min(image_size[0], principal_point[0] + max_valid_radius),
              principal_point[1]);
    const double horizontal_fov =
        std::atan(std::abs(camera.ImageToWorld(left)[0])) +
        std::atan(std::abs(camera.ImageToWorld(right)[0]));
    const double focal_horizontal_fov =
        image_size[0] / 2.0 /
        std::tan(std::min(max_horizontal_fov, horizontal_fov) / 2.0);

    const Eigen::Vector2d top(
        principal_point[0],
        std::max(0.0, principal_point[1] - max_valid_radius)),
        bottom(principal_point[0],
               std::min(image_size[1], principal_point[1] + max_valid_radius));
    const double vertical_fov =
        std::atan(std::abs(camera.ImageToWorld(top)[1])) +
        std::atan(std::abs(camera.ImageToWorld(bottom)[1]));
    const double focal_vertical_fov =
        image_size[1] / 2.0 /
        std::tan(std::min(max_vertical_fov, vertical_fov) / 2.0);

    const double focal = std::max(
        focal_diagonal_fov, std::max(focal_horizontal_fov, focal_vertical_fov));
    undistorted_camera.SetFocalLengthX(focal);
    undistorted_camera.SetFocalLengthY(focal);
  } else {
    const std::vector<size_t>& focal_length_idxs = camera.FocalLengthIdxs();
    CHECK_LE(focal_length_idxs.size(), 2)
        << "Not more than two focal length parameters supported.";
    if (focal_length_idxs.size() == 1) {
      undistorted_camera.SetFocalLengthX(camera.FocalLength());
      undistorted_camera.SetFocalLengthY(camera.FocalLength());
    } else if (focal_length_idxs.size() == 2) {
      undistorted_camera.SetFocalLengthX(camera.FocalLengthX());
      undistorted_camera.SetFocalLengthY(camera.FocalLengthY());
    }
  }

  // Copy principal point parameters.
  undistorted_camera.SetPrincipalPointX(camera.PrincipalPointX());
  undistorted_camera.SetPrincipalPointY(camera.PrincipalPointY());

  // Scale the image such the the boundary of the undistorted image.
  if (camera.ModelId() != SimplePinholeCameraModel::model_id &&
      camera.ModelId() != PinholeCameraModel::model_id) {

    // Determine min, max coordinates along top / bottom image border.

    double left_min_x = std::numeric_limits<double>::max();
    double left_max_x = std::numeric_limits<double>::lowest();
    double right_min_x = std::numeric_limits<double>::max();
    double right_max_x = std::numeric_limits<double>::lowest();

    for (size_t y = 0; y < camera.Height(); ++y) {
      // Left border.
      const Eigen::Vector2d border_point1 = SelectPointOnRay(
          camera, principal_point, Eigen::Vector2d(0.5, y + 0.5),
          max_valid_radius, max_valid_fov_half, max_horizontal_fov / 2.0,
          max_vertical_fov / 2.0);
      const Eigen::Vector2d world_point1 = camera.ImageToWorld(border_point1);
      const Eigen::Vector2d undistorted_point1 =
          undistorted_camera.WorldToImage(world_point1);
      left_min_x = std::min(left_min_x, undistorted_point1(0));
      left_max_x = std::max(left_max_x, undistorted_point1(0));
      // Right border.
      const Eigen::Vector2d border_point2 = SelectPointOnRay(
          camera, principal_point,
          Eigen::Vector2d(camera.Width() - 0.5, y + 0.5), max_valid_radius,
          max_valid_fov_half, max_horizontal_fov / 2.0, max_vertical_fov / 2.0);
      const Eigen::Vector2d world_point2 = camera.ImageToWorld(border_point2);
      const Eigen::Vector2d undistorted_point2 =
          undistorted_camera.WorldToImage(world_point2);
      right_min_x = std::min(right_min_x, undistorted_point2(0));
      right_max_x = std::max(right_max_x, undistorted_point2(0));
    }

    // Determine min, max coordinates along left / right image border.

    double top_min_y = std::numeric_limits<double>::max();
    double top_max_y = std::numeric_limits<double>::lowest();
    double bottom_min_y = std::numeric_limits<double>::max();
    double bottom_max_y = std::numeric_limits<double>::lowest();

    for (size_t x = 0; x < camera.Width(); ++x) {
      // Top border.
      const Eigen::Vector2d border_point1 = SelectPointOnRay(
          camera, principal_point, Eigen::Vector2d(x + 0.5, 0.5),
          max_valid_radius, max_valid_fov_half, max_horizontal_fov / 2.0,
          max_vertical_fov / 2.0);
      const Eigen::Vector2d world_point1 = camera.ImageToWorld(border_point1);
      const Eigen::Vector2d undistorted_point1 =
          undistorted_camera.WorldToImage(world_point1);
      top_min_y = std::min(top_min_y, undistorted_point1(1));
      top_max_y = std::max(top_max_y, undistorted_point1(1));
      // Bottom border.
      const Eigen::Vector2d border_point2 = SelectPointOnRay(
          camera, principal_point,
          Eigen::Vector2d(x + 0.5, camera.Height() - 0.5), max_valid_radius,
          max_valid_fov_half, max_horizontal_fov / 2.0, max_vertical_fov / 2.0);
      const Eigen::Vector2d world_point2 = camera.ImageToWorld(border_point2);
      const Eigen::Vector2d undistorted_point2 =
          undistorted_camera.WorldToImage(world_point2);
      bottom_min_y = std::min(bottom_min_y, undistorted_point2(1));
      bottom_max_y = std::max(bottom_max_y, undistorted_point2(1));
    }

    const double cx = undistorted_camera.PrincipalPointX();
    const double cy = undistorted_camera.PrincipalPointY();

    // Scale such that undistorted image contains all pixels of distorted image
    const double min_scale_x =
        std::min(cx / (cx - left_min_x),
                 (camera.Width() - 0.5 - cx) / (right_max_x - cx));
    const double min_scale_y =
        std::min(cy / (cy - top_min_y),
                 (camera.Height() - 0.5 - cy) / (bottom_max_y - cy));

    // Scale such that there are no blank pixels in undistorted image
    const double max_scale_x =
        std::max(cx / (cx - left_max_x),
                 (camera.Width() - 0.5 - cx) / (right_min_x - cx));
    const double max_scale_y =
        std::max(cy / (cy - top_max_y),
                 (camera.Height() - 0.5 - cy) / (bottom_min_y - cy));

    // Interpolate scale according to blank_pixels.
    double scale_x = 1.0 / (min_scale_x * options.blank_pixels +
                            max_scale_x * (1.0 - options.blank_pixels));
    double scale_y = 1.0 / (min_scale_y * options.blank_pixels +
                            max_scale_y * (1.0 - options.blank_pixels));

    // Clip the scaling factors.
    scale_x = Clip(scale_x, options.min_scale, options.max_scale);
    scale_y = Clip(scale_y, options.min_scale, options.max_scale);

    // Scale undistorted camera dimensions.
    undistorted_camera.SetWidth(static_cast<size_t>(
        std::max(1.0, scale_x * undistorted_camera.Width())));
    undistorted_camera.SetHeight(static_cast<size_t>(
        std::max(1.0, scale_y * undistorted_camera.Height())));

    // Scale the principal point according to the new dimensions of the image.
    undistorted_camera.SetPrincipalPointX(
        undistorted_camera.PrincipalPointX() *
        static_cast<double>(undistorted_camera.Width()) / camera.Width());
    undistorted_camera.SetPrincipalPointY(
        undistorted_camera.PrincipalPointY() *
        static_cast<double>(undistorted_camera.Height()) / camera.Height());
  }

  if (options.max_image_size > 0) {
    const double max_image_scale_x =
        options.max_image_size /
        static_cast<double>(undistorted_camera.Width());
    const double max_image_scale_y =
        options.max_image_size /
        static_cast<double>(undistorted_camera.Height());
    const double max_image_scale =
        std::min(max_image_scale_x, max_image_scale_y);
    if (max_image_scale < 1.0) {
      undistorted_camera.Rescale(max_image_scale);
    }
  }

  return undistorted_camera;
}

void UndistortImage(const UndistortCameraOptions& options,
                    const Bitmap& distorted_bitmap,
                    const Camera& distorted_camera, Bitmap* undistorted_bitmap,
                    Camera* undistorted_camera) {
  CHECK_EQ(distorted_camera.Width(), distorted_bitmap.Width());
  CHECK_EQ(distorted_camera.Height(), distorted_bitmap.Height());

  *undistorted_camera = UndistortCamera(options, distorted_camera);
  undistorted_bitmap->Allocate(static_cast<int>(undistorted_camera->Width()),
                               static_cast<int>(undistorted_camera->Height()),
                               distorted_bitmap.IsRGB());
  distorted_bitmap.CloneMetadata(undistorted_bitmap);

  WarpImageBetweenCameras(distorted_camera, *undistorted_camera,
                          distorted_bitmap, undistorted_bitmap);
}

void UndistortReconstruction(const UndistortCameraOptions& options,
                             Reconstruction* reconstruction) {
  const auto distorted_cameras = reconstruction->Cameras();
  for (auto& camera : distorted_cameras) {
    reconstruction->Camera(camera.first) =
        UndistortCamera(options, camera.second);
  }

  for (const auto& distorted_image : reconstruction->Images()) {
    auto& image = reconstruction->Image(distorted_image.first);
    const auto& distorted_camera = distorted_cameras.at(image.CameraId());
    const auto& undistorted_camera = reconstruction->Camera(image.CameraId());
    for (point2D_t point2D_idx = 0; point2D_idx < image.NumPoints2D();
         ++point2D_idx) {
      auto& point2D = image.Point2D(point2D_idx);
      point2D.SetXY(undistorted_camera.WorldToImage(
          distorted_camera.ImageToWorld(point2D.XY())));
    }
  }
}

void RectifyStereoCameras(const Camera& camera1, const Camera& camera2,
                          const Eigen::Vector4d& qvec,
                          const Eigen::Vector3d& tvec, Eigen::Matrix3d* H1,
                          Eigen::Matrix3d* H2, Eigen::Matrix4d* Q) {
  CHECK(camera1.ModelId() == SimplePinholeCameraModel::model_id ||
        camera1.ModelId() == PinholeCameraModel::model_id);
  CHECK(camera2.ModelId() == SimplePinholeCameraModel::model_id ||
        camera2.ModelId() == PinholeCameraModel::model_id);

  // Compute the average rotation between the first and the second camera.
  Eigen::AngleAxisd rvec(
      Eigen::Quaterniond(qvec(0), qvec(1), qvec(2), qvec(3)));
  rvec.angle() *= -0.5;

  Eigen::Matrix3d R2 = rvec.toRotationMatrix();
  Eigen::Matrix3d R1 = R2.transpose();

  // Determine the translation, such that it coincides with the X-axis.
  Eigen::Vector3d t = R2 * tvec;

  Eigen::Vector3d x_unit_vector(1, 0, 0);
  if (t.transpose() * x_unit_vector < 0) {
    x_unit_vector *= -1;
  }

  const Eigen::Vector3d rotation_axis = t.cross(x_unit_vector);

  Eigen::Matrix3d R_x;
  if (rotation_axis.norm() < std::numeric_limits<double>::epsilon()) {
    R_x = Eigen::Matrix3d::Identity();
  } else {
    const double angle = std::acos(std::abs(t.transpose() * x_unit_vector) /
                                   (t.norm() * x_unit_vector.norm()));
    R_x = Eigen::AngleAxisd(angle, rotation_axis.normalized());
  }

  // Apply the X-axis correction.
  R1 = R_x * R1;
  R2 = R_x * R2;
  t = R_x * t;

  // Determine the intrinsic calibration matrix.
  Eigen::Matrix3d K = Eigen::Matrix3d::Identity();
  K(0, 0) = std::min(camera1.MeanFocalLength(), camera2.MeanFocalLength());
  K(1, 1) = K(0, 0);
  K(0, 2) = camera1.PrincipalPointX();
  K(1, 2) = (camera1.PrincipalPointY() + camera2.PrincipalPointY()) / 2;

  // Compose the homographies.
  *H1 = K * R1 * camera1.CalibrationMatrix().inverse();
  *H2 = K * R2 * camera2.CalibrationMatrix().inverse();

  // Determine the inverse projection matrix that transforms disparity values
  // to 3D world coordinates: [x, y, disparity, 1] * Q = [X, Y, Z, 1] * w.
  *Q = Eigen::Matrix4d::Identity();
  (*Q)(3, 0) = -K(1, 2);
  (*Q)(3, 1) = -K(0, 2);
  (*Q)(3, 2) = K(0, 0);
  (*Q)(2, 3) = -1 / t(0);
  (*Q)(3, 3) = 0;
}

void RectifyAndUndistortStereoImages(
    const UndistortCameraOptions& options, const Bitmap& distorted_image1,
    const Bitmap& distorted_image2, const Camera& distorted_camera1,
    const Camera& distorted_camera2, const Eigen::Vector4d& qvec,
    const Eigen::Vector3d& tvec, Bitmap* undistorted_image1,
    Bitmap* undistorted_image2, Camera* undistorted_camera,
    Eigen::Matrix4d* Q) {
  CHECK_EQ(distorted_camera1.Width(), distorted_image1.Width());
  CHECK_EQ(distorted_camera1.Height(), distorted_image1.Height());
  CHECK_EQ(distorted_camera2.Width(), distorted_image2.Width());
  CHECK_EQ(distorted_camera2.Height(), distorted_image2.Height());

  *undistorted_camera = UndistortCamera(options, distorted_camera1);
  undistorted_image1->Allocate(static_cast<int>(undistorted_camera->Width()),
                               static_cast<int>(undistorted_camera->Height()),
                               distorted_image1.IsRGB());
  distorted_image1.CloneMetadata(undistorted_image1);

  undistorted_image2->Allocate(static_cast<int>(undistorted_camera->Width()),
                               static_cast<int>(undistorted_camera->Height()),
                               distorted_image2.IsRGB());
  distorted_image2.CloneMetadata(undistorted_image2);

  Eigen::Matrix3d H1;
  Eigen::Matrix3d H2;
  RectifyStereoCameras(*undistorted_camera, *undistorted_camera, qvec, tvec,
                       &H1, &H2, Q);

  WarpImageWithHomographyBetweenCameras(H1.inverse(), distorted_camera1,
                                        *undistorted_camera, distorted_image1,
                                        undistorted_image1);
  WarpImageWithHomographyBetweenCameras(H2.inverse(), distorted_camera2,
                                        *undistorted_camera, distorted_image2,
                                        undistorted_image2);
}

}  // namespace colmap
