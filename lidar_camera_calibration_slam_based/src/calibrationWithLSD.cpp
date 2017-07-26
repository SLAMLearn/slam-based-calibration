#include "ros/ros.h"
#include "lidar_camera_calibration_slam_based/keyframeMsg.h"
#include <iostream>
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <Eigen/Dense>
#include <ceres/ceres.h>
#include "sophus/so3.hpp"
#include "sophus/se3.hpp"
#include <fstream>
#include <string>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/core/eigen.hpp>
#define COMPILE_C
#include "MIToolbox/CalculateProbability.h"
#include "MIToolbox/MutualInformation.h"
#include <chrono>

using namespace Eigen;
using DepthImage = MatrixXd *;
using PointCloud = MatrixXd *;

struct InputPointDense
{
	float idepth;
	float idepth_var;
	unsigned char color[4];
};
struct KeyFrame
{
  double time;
  DepthImage depth;
	DepthImage color;
  //Color image
};

std::vector<KeyFrame> keyFrames;

uint32_t image_w = 0, image_h = 0;
Matrix<double,3,3> cameraK = MatrixXd::Zero(3,3);
int frame_count = 0;
#define NUM_FRAMES_COUNT_LIMIT 30
void keyFrameCallback(const lidar_camera_calibration_slam_based::keyframeMsg& msg)
{
	ROS_INFO("new key frame!!!");
	frame_count++;
  image_w = msg.width; image_h = msg.height;
	cameraK(0,0) = msg.fx; cameraK(1,1) = msg.fy; cameraK(0,2) = msg.cx; cameraK(1,2) = msg.cy; cameraK(2,2) = 1.0;
  if(msg.pointcloud.size() == 0)
  {
    ROS_ERROR("empty pointcloud!!!");
    return;
  }

  InputPointDense *inputPoints = (InputPointDense *)msg.pointcloud.data();
	// new InputPointDense[image_w*image_h];
  // std::copy(&msg.pointcloud[0], &msg.pointcloud[0 + size(InputPointDense) * image_w * image_h], inputPoints);

  // create a new depthImage
  MatrixXd *depth = new MatrixXd(image_h, image_w);
	MatrixXd *color = new MatrixXd(image_h, image_w);

  for(int i=0;i<image_h;i++){
    for(int j=0;j<image_w;j++){
      (*depth)(i,j) = 1.0 / inputPoints[i*image_w + j].idepth;
			(*color)(i,j) = inputPoints[i*image_w + j].color[0] / 255.0;
    }
  }
  // delete[] inputPoints;

  KeyFrame k;
  k.time = msg.time; k.depth = depth; k.color = color;
  keyFrames.push_back(k);
	// std::cout<<*depth<<std::endl;
}

int findCorrespondingLidarScan(double timestamp, std::vector<double> &time_scans, int start = 0);

int findCorrespondingLidarScan(double timestamp, std::vector<double> &time_scans, int start)
{
	start = start>0 ? start : 0;
	for(int i=start;i<time_scans.size()-1;i++)
	{
		if(timestamp > time_scans[i] && timestamp < time_scans[i+1]){
			return i;
		}
	}
	return -1;
}

bool loadTimestampsIntoVector(
		const std::string& filename, std::vector<double>* timestamp_vec){

  std::ifstream import_file(filename, std::ios::in);
  if (!import_file) {
    return false;
  }

  timestamp_vec->clear();
  std::string line;
  while (std::getline(import_file, line)) {
    std::stringstream line_stream(line);

    std::string timestamp_string = line_stream.str();
    std::tm t = {};
    t.tm_year = std::stoi(timestamp_string.substr(0, 4)) - 1900;
    t.tm_mon  = std::stoi(timestamp_string.substr(5, 2)) - 1;
    t.tm_mday = std::stoi(timestamp_string.substr(8, 2));
    t.tm_hour = std::stoi(timestamp_string.substr(11, 2));
    t.tm_min  = std::stoi(timestamp_string.substr(14, 2));
    t.tm_sec  = std::stoi(timestamp_string.substr(17, 2));
    t.tm_isdst = -1;

    static const uint64_t kSecondsToNanoSeconds = 1e9;
    time_t time_since_epoch = mktime(&t);

    double timestamp = (double)time_since_epoch +
                         (double)std::stoi(timestamp_string.substr(20, 9)) / kSecondsToNanoSeconds;
    timestamp_vec->push_back(timestamp);
  }

  std::cout << "Timestamps: " << std::endl
            << timestamp_vec->front() << " " << timestamp_vec->back()
            << std::endl;

  return true;
}

bool loadVeloPointCloud(int frameId, MatrixXd &pc)
{
	std::string filename =  (std::string("0000000000") + std::to_string(frameId));
  std::string path = \
	"/home/sundw/workspace/data/2011_09_30/2011_09_30_drive_0028_sync/velodyne_points/data/"\
	 + filename.substr(filename.size()-10) \
	 + ".bin";

	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (!input) {
		std::cout << "Could not open pointcloud file.\n";
		return false;
	}

	std::streamsize size = input.tellg();
	input.seekg(0, std::ios::beg);

	float *buffer = new float[size/sizeof(float)];
	if(!input.read((char *)buffer, size))
	{
		std::cout<<"error read"<<std::endl;
	}
	input.close();
	Map<MatrixXf> _pc(buffer, 4, (int)(size/sizeof(float)/4));
	pc = _pc.cast<double>();
	delete[] buffer;
  return true;
}

struct singlePointCloudMICost{
	singlePointCloudMICost(PointCloud pc, DepthImage depth, DepthImage color):_pc(pc), _depth(depth), _color(color){};
	PointCloud _pc;
	DepthImage _depth;
	DepthImage _color;
	// template <typename T>
	bool operator()(const double* const xi_cam_velo, double* residuals) const
	{
		typedef Matrix<double,6,1> Vector6d;
		Vector6d xi;
		for (int i=0;i<6;i++) xi(i) = double(xi_cam_velo[i]);// = Map<const Vector6d>(xi_cam_velo,6,1);
		MatrixXd T_cam_velo = Sophus::SE3d::exp(xi).matrix();
		MatrixXd _pc_homo(4, _pc->cols());
		_pc_homo << _pc->topRows(3), MatrixXd::Ones(1, _pc->cols());
		// std::cout<<T_cam_velo.topRows(3)<<std::endl;
		// std::cout<<cameraK<<std::endl;
		// std::cout<<_pc->leftCols(10)<<std::endl;
		// std::cout<<_pc_homo.leftCols(10)<<std::endl;
		// project this PointCloud to camera plane
		MatrixXd imagePoints = cameraK * (T_cam_velo.topRows(3) * _pc_homo);
		// std::cout<<imagePoints.leftCols(10)<<std::endl;

		int num_point = imagePoints.cols();
		double *X = new double[num_point], *Y = new double[num_point], *Xpt = X, *Ypt = Y;
		// MatrixXd depth_gt = MatrixXd::Zero(480, 640);
		for(int i=0;i<num_point;i++)
		{
			Vector3d p = imagePoints.col(i);
			double z = p[2];
			double u = p[0] / z, v = p[1] / z;
			if(u<0 || u>image_w || v<0 || v>image_h || z<0){
				// out of camera plane
				continue;
			}
			// this point is on the camera plane
			double image_color = (*_color)((int)(v), (int)(u));
			*Xpt = (*_pc)(3, i) * 255.0; *Ypt = image_color * 255.0;
			Xpt++; Ypt++;

			// depth_gt((int)(v), int(u)) = z / 30.0;

			// double image_depth = (*_depth)((int)(v), (int)(u));
			// if(image_depth > 0 && image_depth < 100.0){
			// 	// find a corresponding point, add depth to random varibles X and Y
			// 	*Xpt = z; *Ypt = image_depth;
			// 	Xpt++; Ypt++;
			// }
		}
		// cv::Mat image;
		// cv::eigen2cv(depth_gt, image);
		// cv::imshow(windowName, image / 1.0 );                   // Show our image inside it.
		if(Xpt-X > 1000){
			auto prob = discAndCalcJointProbability(X,Y,(Xpt-X));
			residuals[0] = double(1.0 / mi(prob));
			freeJointProbabilityState(prob);
			// ;
		}
		else{
			residuals[0] = double(100.0);
		}
		delete[] X;
		delete[] Y;
	}
};

int main(int argc, char **argv)
{
  ros::init(argc, argv, "calibrationWithLSD");

  ros::NodeHandle n;

  ros::Subscriber sub = n.subscribe("/lsd_slam/keyframes", 1000, keyFrameCallback);

  while(ros::ok()){
    ros::spinOnce();
		if(frame_count>NUM_FRAMES_COUNT_LIMIT) {break;}
  }

  // start calibrating
	std::vector<double> timestamp_vec;
	loadTimestampsIntoVector("/home/sundw/workspace/data/2011_09_30/2011_09_30_drive_0028_sync/velodyne_points/timestamps_start.txt", &timestamp_vec);
	// cv::namedWindow( "image", cv::WINDOW_AUTOSIZE );// Create a window for display.
	// cv::namedWindow( "lsd_depth", cv::WINDOW_AUTOSIZE );// Create a window for display.
	// cv::namedWindow( "velo", cv::WINDOW_AUTOSIZE );// Create a window for display.
	// cv::namedWindow( "velo_error", cv::WINDOW_AUTOSIZE );// Create a window for display.
	// double loss1 = 0.0, loss2 = 0.0;
	ceres::Problem problem;
	double T_cam_velo_xi[6] = {-0.632169, 0.137709, 0.036695, 1.20717, -1.21912, 1.20154};
	double T_cam_velo_xi_error[6] = {-0.532169, 0.237709, 0.046695, 1.10717, -1.11912, 1.30154};
	double result[6] = {-0.532169, 0.237709, 0.046695, 1.10717, -1.11912, 1.30154};

	for(auto kF : keyFrames){
		auto timestamp = kF.time;
		auto depth = kF.depth;
		auto color = kF.color;

		int frameId = findCorrespondingLidarScan(timestamp, timestamp_vec);
		if(frameId<0){ROS_ERROR("can not find corresponding scan!!!"); exit(1);}
		MatrixXd *velo_pointCloud = new MatrixXd();
		loadVeloPointCloud(frameId, (*velo_pointCloud));
		// singlePointCloudMICost pcc(velo_pointCloud, depth);

		problem.AddResidualBlock(new ceres::NumericDiffCostFunction<singlePointCloudMICost, ceres::RIDDERS, 1, 6> (new singlePointCloudMICost ( velo_pointCloud, depth, color )), nullptr, result);
	}
	ceres::Solver::Options options;
	// options.use_nonmonotonic_steps = true;
	options.linear_solver_type = ceres::DENSE_NORMAL_CHOLESKY;
	options.minimizer_progress_to_stdout = true;
	ceres::Solver::Summary summary;

	std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
	ceres::Solve ( options, &problem, &summary );
	std::chrono::steady_clock::time_point t2 = std::chrono::steady_clock::now();

	std::chrono::duration<double> time_used = std::chrono::duration_cast<std::chrono::duration<double>>( t2-t1 );
	std::cout<<"solve time cost = "<<time_used.count()<<" seconds. "<<std::endl;

	std::cout<<summary.BriefReport() <<std::endl;
	std::cout<<"estimated T_cam3_velo : ";
	for ( auto a:result ) std::cout<<a<<" "; std::cout<<std::endl;
	return 0;
}
