/**
 * Morry lam
 */

#define LEFT    65361
#define UP      65362
#define RIGHT   65363
#define DOWN    65364
#define ESC     27

// Aldebaran includes.
#include <alproxies/alvideodeviceproxy.h>
#include <alvision/alimage.h>
#include <alvision/alvisiondefinitions.h>
#include <alerror/alerror.h>
#include <alproxies/almotionproxy.h>

// Opencv includes.
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/calib3d/calib3d.hpp>
#include <opencv2/features2d/features2d.hpp>

#include <iostream>
#include <string>
#include <time.h>
#include <stdio.h>
#include <math.h>
#include <boost/thread.hpp>
#include <fstream>

#include "inputsource.hpp"

using namespace boost;

class NaoController {
    void sweep();
    void keyboard();
    NaoInput *naoInput;
    AL::ALMotionProxy *motProxy;

public:
    NaoController(std::string robotIp);
    NaoController(std::string robotIp, cv::Mat &cameraMatrix, cv::Mat &distCoeffs);

    void cameraCalibration();
    void showImages();
    void recordDataSet();
};

static std::string matrixToString(cv::Mat matrix)
{
    std::ostringstream out;

    for(int i=0; i<matrix.rows; i++)
    {
        for(int j=0; j<matrix.cols; j++)
        {
            out << matrix.at<double>(i,j) << "\t";
        }
        out << "\n";
    }
    return out.str();
}

static double computeReprojectionErrors( const std::vector<std::vector<cv::Point3f> >& objectPoints,
                                         const std::vector<std::vector<cv::Point2f> >& imagePoints,
                                         const std::vector<cv::Mat>& rvecs,
                                         const std::vector<cv::Mat>& tvecs,
                                         const cv::Mat& cameraMatrix,
                                         const cv::Mat& distCoeffs,
                                         std::vector<float>& perViewErrors)
{
    std::vector<cv::Point2f> imagePoints2;
    int i, totalPoints = 0;
    double totalErr = 0, err;
    perViewErrors.resize(objectPoints.size());

    for( i = 0; i < (int)objectPoints.size(); ++i )
    {
        projectPoints( cv::Mat(objectPoints[i]), rvecs[i], tvecs[i], cameraMatrix,
                       distCoeffs, imagePoints2);
        err = norm(cv::Mat(imagePoints[i]), cv::Mat(imagePoints2), CV_L2);

        int n = (int)objectPoints[i].size();
        perViewErrors[i] = (float) std::sqrt(err*err/n);
        totalErr        += err*err;
        totalPoints     += n;
    }

    return std::sqrt(totalErr/totalPoints);
}

NaoController::NaoController(const std::string robotIp)
{
    this->naoInput = new NaoInput(robotIp);
    // this->naoInput = new NaoInput(robotIp, cameraMatrix, ...
    this->motProxy = naoInput->motProxy;
}

NaoController::NaoController(std::string robotIp, cv::Mat &cameraMatrix, cv::Mat &distCoeffs)
{
    this->naoInput = new NaoInput(robotIp, "", AL::kTopCamera, cameraMatrix, distCoeffs);
    // this->naoInput = new NaoInput(robotIp, cameraMatrix, ...
    this->motProxy = naoInput->motProxy;
}

void NaoController::cameraCalibration()
{
    cv::Size imageSize = cv::Size(640, 480);
    cv::Mat imgHeader = cv::Mat(imageSize, CV_8UC1);
    cv::namedWindow("images");

    // calibration params and variables
    bool calibrated = false;
    bool found;

    // calibration points storage
    std::vector<std::vector<cv::Point2f> > finalImagePoints;

    // board dimension and size
    cv::Size boardSize = cv::Size(8,5);
    float squareSize = 0.027;

    // fill a vector of vectors with 3d-points
    std::vector<std::vector<cv::Point3f> > objectPoints(1);
    for( int i = 0; i < boardSize.height; ++i )
        for( int j = 0; j < boardSize.width; ++j )
            objectPoints[0].push_back(cv::Point3f(float( j*squareSize ), float( i*squareSize ), 0));

    // camera matrix for intrinsic and extrinsic parameters
    cv::Mat cameraMatrix, distCoeffs;
    cameraMatrix = cv::Mat::eye(3, 3, CV_64F);
    cameraMatrix.at<double>(0,0) = 1.0;
    distCoeffs = cv::Mat::zeros(8, 1, CV_64F);

    Frame frame;

    /** Main loop. Exit when pressing ESC.*/
    while ((char) cv::waitKey(30) != 27)
    {
        frame = naoInput->getNextFrame();
        imgHeader = frame.img;
        //std::cout << frame.img.size().height << std::endl;

        std::vector<cv::Mat> rvecs, tvecs;
        std::vector<cv::Point2f> pointBuf;

        found = cv::findChessboardCorners( imgHeader,
                                           boardSize, 
                                           pointBuf,
                                           CV_CALIB_CB_FAST_CHECK | CV_CALIB_CB_NORMALIZE_IMAGE);

        if (found)
        {
            // improve the found corners' coordinate accuracy for chessboard
            cv::cornerSubPix( imgHeader,
                              pointBuf,
                              cv::Size(11,11),
                              cv::Size(-1,-1),
                              cv::TermCriteria( CV_TERMCRIT_EPS+CV_TERMCRIT_ITER, 30, 0.1 ));

            // size of chessboard squares, compute real points in the object
            std::vector<std::vector<cv::Point2f> > imagePoints;
            imagePoints.push_back(pointBuf);

            // Draw the corners.
            cv::drawChessboardCorners( imgHeader, boardSize, cv::Mat(pointBuf), found );

            // If c is pressed, add the current chessboard to the image points
            if (cv::waitKey(30) == 'c')
            {
                finalImagePoints.push_back(pointBuf);
                objectPoints.resize(finalImagePoints.size(), objectPoints[0]);

                // Perform calibration based on old values, if possible.
                double rms;
                try
                {
                    if(calibrated)
                    {
                        // keep updating
                        rms = cv::calibrateCamera(objectPoints,
                                                  finalImagePoints,
                                                  imageSize,
                                                  cameraMatrix,
                                                  distCoeffs,
                                                  rvecs,
                                                  tvecs,
                                                  CV_CALIB_USE_INTRINSIC_GUESS|CV_CALIB_FIX_K4|CV_CALIB_FIX_K5);
                    } else {
                        // estimate parameters for the first time
                        rms = cv::calibrateCamera(objectPoints,
                                                  finalImagePoints,
                                                  imageSize,
                                                  cameraMatrix,
                                                  distCoeffs,
                                                  rvecs,
                                                  tvecs,
                                                  CV_CALIB_FIX_K4|CV_CALIB_FIX_K5);
                    }
                    std::cout << "Re-projection error reported by calibrateCamera: "<< rms << std::endl;
                } catch(cv::Exception e) {
                    // Probably due to wrong distortion coefficients (cam movement?)
                }
                calibrated = (cv::checkRange(cameraMatrix) && cv::checkRange(distCoeffs));

                std::vector<float> reprojErrs;
                double totalAvgErr;
                totalAvgErr = computeReprojectionErrors(objectPoints,
                                                        finalImagePoints,
                                                        rvecs,
                                                        tvecs,
                                                        cameraMatrix,
                                                        distCoeffs,
                                                        reprojErrs);
                std::cout << "Avg re projection error = "  << totalAvgErr << std::endl;

            }
        }
        cv::imshow("images", imgHeader);
    }

    while ((char) cv::waitKey(20) != 27)
    {
        frame = naoInput->getNextFrame();
        imgHeader = frame.img;
        undistortImage(imgHeader, cameraMatrix, distCoeffs);
        cv::imshow("images", imgHeader);
    }

    saveSettings(cameraMatrix, distCoeffs);
}

/**
  * Get images and save these, annotated with timestamp and 3d pose estimation.
  */
void NaoController::recordDataSet()
{
    AL::ALValue jointName = "Body";
    AL::ALValue stiffness = 1.0f;
    motProxy->stiffnessInterpolation(jointName, stiffness, 0.1f);

    this_thread::sleep(posix_time::seconds(2));

    motProxy->walkTo(0.1, 0.0, 0.0);

    AL::ALValue headPitchName = "HeadPitch";
    AL::ALValue headPitchAngle = 0.0;
    AL::ALValue headPitchSpeed = 0.5;
    motProxy->setAngles(headPitchName, headPitchAngle, headPitchSpeed);

    thread keyboardThread (bind(&NaoController::keyboard, this));
    thread sweepThread (bind(&NaoController::sweep, this));

    keyboardThread.join();
    sweepThread.join();

}

void NaoController::keyboard()
{
    bool doBreak = false;
    int lastCase = 0;
    while (!doBreak)
    {
        // basic keyboardinterface
        int c = cv::waitKey(500);
        if( c != lastCase || c == -1)
        {
            switch(c)
            {
            case ESC: // esc key
                motProxy->setWalkTargetVelocity(0.0, 0.0, 0.0, 1.0);
                doBreak = true;
                break;
            case RIGHT: // right arrow
                motProxy->setWalkTargetVelocity(0.0, 0.0, -0.8, 1.0);
                break;
            case LEFT: // left arrow
                motProxy->setWalkTargetVelocity(0.0, 0.0, 0.8, 1.0);
                break;
            case UP: // up arrow
                motProxy->setWalkTargetVelocity(0.8, 0.0, 0.0, 1.0);
                break;
            case DOWN: // down arrow
                motProxy->setWalkTargetVelocity(-0.8, 0.0, 0.0, 1.0);
                break;
            default:
                motProxy->setWalkTargetVelocity(0.0, 0.0, 0.0, 0.0);
                break;
            }
        }
        this_thread::sleep(posix_time::milliseconds(10));
    }
    std::cout << "keyboard" << std::endl;
}

void NaoController::sweep()
{
    cv::Size imageSize = cv::Size(640, 480);
    cv::Mat imgHeader = cv::Mat(imageSize, CV_8UC1);
    cv::namedWindow("images");

    AL::ALValue topCamName = "CameraTop";
    int space = 1; // world coordinates
    std::vector<float> camPosition;
    std::vector<float> initialCamPosition = motProxy->getPosition(topCamName, space, true);

    int counter = 1;
    std::ofstream odometryFile;
    odometryFile.open("images/odometry.txt");

    AL::ALValue headYawName = "HeadYaw";
    AL::ALValue headYawAngles =  AL::ALValue::array(-1.5f, 1.5f);
    AL::ALValue headYawTimes =  AL::ALValue::array(5, 10);
    motProxy->post.angleInterpolation(headYawName, headYawAngles, headYawTimes, true);

    Frame frame;

    while(cv::waitKey(30) != ESC)
    {        
        // get imagedata, show feed
        frame = naoInput->getNextFrame();
        imgHeader = frame.img;
        camPosition = frame.camPosition;

        // find relative positionvector
        for(size_t i = 0; i < camPosition.size(); ++i)
        {
          if(i != 0)
            odometryFile << " ";
          odometryFile << (camPosition[i] - initialCamPosition[i]);
        }
        odometryFile << std::endl;

        char filename[30];
        int length = sprintf(filename,
                             "./images/image_%.4d.png",
                             counter++);

        cv::imshow("images", imgHeader);
        try {
            cv::imwrite(filename, imgHeader);
        }
        catch (std::runtime_error &e) {
            std::cerr << "Failed to write to file " << filename << ": " << e.what() << std::endl;
        }


        this_thread::sleep(posix_time::milliseconds(30));

        // Perform sweep, get images
        bool sweep = true;
        AL::ALValue taskList = motProxy->getTaskList();
        std::string angleInterpolation("angleInterpolation");
        for(int i=0; i<taskList.getSize(); i++)
        {
            std::string motionName = taskList[i][0];
            if(!motionName.compare(angleInterpolation))
            {
                sweep = false;
                break;
            }
        }
        if (sweep)
        {
            motProxy->post.angleInterpolation(headYawName, headYawAngles, headYawTimes, true);
        }
    }
    std::cout << "sweep" << std::endl;
    odometryFile.close();
}

/*
std::ostream& operator<<(std::ostream &strm, const NaoController &naoCam)
{

    strm << matrixToString(naoCam.cameraMatrix);
    strm << "\n";
    strm << matrixToString(naoCam.distCoeffs);

    return strm;
}
*/

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "Usage: '" << argv[0] << " robotIp'" << std::endl;
        return 1;
    }

    const std::string robotIp(argv[1]);
    NaoController *naoCam;
    cv::Mat cameraMatrix, distCoeffs;

    try {
        loadSettings(cameraMatrix, distCoeffs);
        naoCam = new NaoController(robotIp, cameraMatrix, distCoeffs);
    } catch (std::runtime_error e) {
        std::cout << "Failed to load file config" << std::endl;
        naoCam = new NaoController(robotIp);
    }

    //naoCam.cameraCalibration();
    //std::cout << naoCam;
    naoCam->recordDataSet();

    return 0;

    if (argc < 3)
    {
        std::cerr << "Usage 'inputsource method argument'" << std::endl;
        return 1;
    }

    cv::namedWindow("images");

    const std::string method(argv[1]);
    if(!method.compare("offline"))
    {
        const std::string foldername(argv[2]);
        FileInput input (foldername);
        cv::imshow("images", input.getNextFrame().img);
    } else {
        const std::string robotIp(argv[2]);
        NaoInput input (robotIp);
        cv::imshow("images", input.getNextFrame().img);
    }
    return 0;
}
