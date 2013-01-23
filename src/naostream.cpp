    /**
    *
    * This example demonstrates how to get images from the robot remotely and how
    * to display them on your screen using opencv.
    *
    * Copyright Aldebaran Robotics
    */

    #include <alproxies/alvideodeviceproxy.h>
    #include <alvision/alimage.h>
#include <alvision/alvisiondefinitions.h>
#include <alerror/alerror.h>
#include <alproxies/almotionproxy.h>

// OpenCV includes.
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/calib3d/calib3d.hpp>
#include <opencv2/features2d/features2d.hpp>

#include <iostream>
#include <string>
#include <time.h>
#include "inputsource.hpp"

#define RED cv::Scalar( 0, 0, 255 )
#define EPSILON 0.0001
#define THRESHOLD 0.5
#define VERBOSE 1

typedef std::vector<cv::KeyPoint> KeyPointVector;

class VisualOdometry
{
    InputSource *inputSource;
    cv::Matx33d K;
    cv::Mat distortionCoeffs;

    bool DecomposeEtoRandT( cv::Matx33d &E, cv::Mat &R1, cv::Mat &R2, cv::Mat &t );
    cv::Mat_<double> LinearLSTriangulation( cv::Point3d u1, cv::Matx34d P1, cv::Point3d u2, cv::Matx34d P2 );
    cv::Matx31d IterativeLinearLSTriangulation(cv::Point3d u1, cv::Matx34d P1, cv::Point3d u2, cv::Matx34d P2);
    double solveScale(std::vector<cv::Point2f> imagepoints_normalized,
                      std::vector<cv::Point3f> objectpoints_normalized,
                      cv::Matx34d RTMatrix);
    double determineFundamentalMatrix(std::vector<cv::Point2f> &current_points,
                                      std::vector<cv::Point2f> &previous_points,
                                      std::vector<cv::Point2f> &previous_points_inliers,
                                      std::vector<cv::Point2f> &current_points_inliers,
                                      std::vector<cv::DMatch> &matches,
                                      cv::Matx33d &F);
public:
    VisualOdometry(InputSource *source);
    ~VisualOdometry();
    bool MainLoop();

    bool validConfig;
};

/**
 * Determine fundamental matrix, and while we're at it, find the inliers for both current and previous
 * points (and store them in new vectors), update matches, and calculate mean displacement.
 **/
double VisualOdometry::determineFundamentalMatrix(std::vector<cv::Point2f> &previous_points,
                                                  std::vector<cv::Point2f> &current_points,
                                                  std::vector<cv::Point2f> &previous_points_inliers,
                                                  std::vector<cv::Point2f> &current_points_inliers,
                                                  std::vector<cv::DMatch> &matches,
                                                  cv::Matx33d &F)
{
    double minVal, maxVal;
    std::vector<uchar> status( matches.size() );
    std::vector<cv::DMatch> good_matches;
    double mean_distance = 0.0;

    cv::minMaxIdx( previous_points, &minVal, &maxVal );

    // Find the fundamental matrix
    F = cv::findFundamentalMat( previous_points,
                                current_points,
                                status,
                                cv::FM_RANSAC, 0.006 * maxVal,
                                0.99 );

    // Reject outliers and calculate mean distance at the same time! Update matches as well
    for ( int i = 0; i < previous_points.size(); i++ ) {
        if( status[i] ) {
            current_points_inliers.push_back( current_points[i] );
            previous_points_inliers.push_back( previous_points[i] );

            // Needed for displaying inliers
            good_matches.push_back( matches[i] );

            mean_distance += cv::norm( current_points[i] - previous_points[i] );
        }
    }

    // These are matches after removing outliers
    matches = good_matches;

    // Distance calculation
    mean_distance /= (double)matches.size();
    return mean_distance;
}

bool VisualOdometry::DecomposeEtoRandT( cv::Matx33d &E, cv::Mat &R1, cv::Mat &R2, cv::Mat &t ) {
    //Using HZ E decomposition
    cv::SVD svd(E, cv::SVD::MODIFY_A);

    //check if first and second singular values are the same (as they should be)
    double singular_values_ratio = fabs( svd.w.at<double>( 0 ) / svd.w.at<double>( 1 ) );
    if ( singular_values_ratio > 1.0 ) {
        singular_values_ratio = 1.0/singular_values_ratio; // flip ratio to keep it [0,1]
    }
    if ( singular_values_ratio < 0.7 ) {
        std::cout << "singular values are too far apart\n" << std::endl;
        std::cout << svd.w << std::endl;

        return false;
    }

    cv::Matx33d W(  0, -1,  0,	//HZ 9.513
                    1,  0,  0,
                    0,  0,  1 );
    cv::Matx33d Wt( 0,  1,  0,
                   -1,  0,  0,
                    0,  0,  1 );

    R1 = svd.u * cv::Mat(W) * svd.vt; //HZ 9.19
    R2 = svd.u * cv::Mat(Wt) * svd.vt; //HZ 9.19
    t = svd.u.col(2); //u3

    return true;
}

//From "Triangulation", Hartley, R.I. and Sturm, P., Computer vision and image understanding, 1997
//
// Arguments:
//     u1 and u2: homogenous image point (u,v,1)
//     P1 and P2: Camera matrices
cv::Mat_<double> VisualOdometry::LinearLSTriangulation( cv::Point3d u1, cv::Matx34d P1, cv::Point3d u2, cv::Matx34d P2 ) {
    //build matrix A for homogenous equation system Ax = 0
    //assume X = (x,y,z,1), for Linear-LS method
    //which turns it into a AX = B system, where A is 4x3, X is 3x1 and B is 4x1
    // cout << "u " << u <<", u1 " << u1 << endl;
    // Matx<double,6,4> A; //this is for the AX=0 case, and with linear dependence..
    // A(0) = u.x*P(2)-P(0);
    // A(1) = u.y*P(2)-P(1);
    // A(2) = u.x*P(1)-u.y*P(0);
    // A(3) = u1.x*P1(2)-P1(0);
    // A(4) = u1.y*P1(2)-P1(1);
    // A(5) = u1.x*P(1)-u1.y*P1(0);
    // Matx43d A; //not working for some reason...
    // A(0) = u.x*P(2)-P(0);
    // A(1) = u.y*P(2)-P(1);
    // A(2) = u1.x*P1(2)-P1(0);
    // A(3) = u1.y*P1(2)-P1(1);
    cv::Matx43d A(  u1.x * P1(2,0) - P1(0,0), u1.x * P1(2,1) - P1(0,1), u1.x * P1(2,2) - P1(0,2),
                    u1.y * P1(2,0) - P1(1,0), u1.y * P1(2,1) - P1(1,1), u1.y * P1(2,2) - P1(1,2),
                    u2.x * P2(2,0) - P2(0,0), u2.x * P2(2,1) - P2(0,1), u2.x * P2(2,2) - P2(0,2),
                    u2.y * P2(2,0) - P2(1,0), u2.y * P2(2,1) - P2(1,1), u2.y * P2(2,2) - P2(1,2)
                 );
    cv::Matx41d B( -( u1.x * P1(2,3) - P1(0,3) ),
                   -( u1.y * P1(2,3) - P1(1,3) ),
                   -( u2.x * P2(2,3) - P2(0,3) ),
                   -( u2.y * P2(2,3) - P2(1,3) )
                 );

    cv::Mat_<double> X;
    cv::solve( A, B, X, cv::DECOMP_SVD );

    return X;
}


/**
From "Triangulation", Hartley, R.I. and Sturm, P., Computer vision and image understanding, 1997
*/
cv::Matx31d VisualOdometry::IterativeLinearLSTriangulation(cv::Point3d u1, cv::Matx34d P1, cv::Point3d u2, cv::Matx34d P2	) {
    double wi1 = 1;
    double wi2 = 1;

    cv::Matx41d X;

    for ( int i = 0; i < 10; i++ ) {
        // Hartley suggests 10 iterations at most
        cv::Mat_<double> X_ = LinearLSTriangulation( u1, P1, u2, P2 );
        X = cv::Matx41d( X_(0), X_(1), X_(2), 1.0 );

        // Recalculate weights
        double p2x1 = cv::Mat_<double>( P1.row( 2 ) * X )(0);
        double p2x2 = cv::Mat_<double>( P2.row( 2 ) * X )(0);

        // Breaking point
        if ( fabs( wi1 - p2x1 ) <= EPSILON && fabs( wi2 - p2x2 ) <= EPSILON ) break;

        wi1 = p2x1;
        wi2 = p2x2;

        // Reweight equations and solve
        cv::Matx43d A( ( u1.x * P1(2,0) - P1(0,0) ) / wi1, ( u1.x * P1(2,1) - P1(0,1) ) / wi1, ( u1.x * P1(2,2) - P1(0,2) ) / wi1,
                       ( u1.y * P1(2,0) - P1(1,0) ) / wi1, ( u1.y * P1(2,1) - P1(1,1) ) / wi1, ( u1.y * P1(2,2) - P1(1,2) ) / wi1,
                       ( u2.x * P2(2,0) - P2(0,0) ) / wi2, ( u2.x * P2(2,1) - P2(0,1) ) / wi2, ( u2.x * P2(2,2) - P2(0,2) ) / wi2,
                       ( u2.y * P2(2,0) - P2(1,0) ) / wi2, ( u2.y * P2(2,1) - P2(1,1) ) / wi2, ( u2.y * P2(2,2) - P2(1,2) ) / wi2
                 );
        cv::Matx41d B( -( u1.x * P1(2,3) - P1(0,3) ) / wi1,
                       -( u1.y * P1(2,3) - P1(1,3) ) / wi1,
                       -( u2.x * P2(2,3) - P2(0,3) ) / wi2,
                       -( u2.y * P2(2,3) - P2(1,3) ) / wi2
                     );

        cv::solve( A, B, X_, cv::DECOMP_SVD );
        X = cv::Matx41d( X_(0), X_(1), X_(2), 1.0 );
    }

    return cv::Matx31d( X(0), X(1), X(2) );
}

bool VisualOdometry::MainLoop() {
    // Declare neccessary storage variables
    cv::Mat current_descriptors, previous_descriptors;
    KeyPointVector current_keypoints, previous_keypoints;
    std::vector<cv::DMatch> matches;
    cv::Matx41d robotPosition (0.0, 0.0, 0.0, 1.0);

    // Create brisk detector
    cv::BRISK brisk(60, 4, 1.0f);
    brisk.create("BRISK");

    // Get the previous frame
    Frame current_frame;
    Frame previous_frame;
    inputSource->getFrame( previous_frame );

    // Detect features for the firstm time
    brisk.detect( previous_frame.img, previous_keypoints );    
    brisk.compute( previous_frame.img, previous_keypoints, previous_descriptors );

    // Ready matcher and corresponding iterator object
    cv::FlannBasedMatcher matcher( new cv::flann::LshIndexParams( 20, 10, 2 ) );
    std::vector<cv::DMatch>::iterator match_it;

    // Use frame-to-frame initially.
    bool epnp = false;


    // Storage for 3d points and corresponding descriptors
    std::vector<cv::Point3f> total_3D_pointcloud;
    KeyPointVector total_2D_keypoints;
    cv::Mat total_3D_descriptors;
    cv::Mat total_2D_descriptors;

    while ( (char) cv::waitKey( 30 ) == -1 ) {
        // Retrieve an image
        if ( !inputSource->getFrame( current_frame ) ) {
            std::cout << "Can not read the next frame." << std::endl;
            break;
        }
        if ( !current_frame.img.data ) {
            std::cerr << "No image found." << std::endl;
            return false;
        }

        // Detect features
        brisk.detect( current_frame.img, current_keypoints );

        // TODO : What if zero features found?
        if ( previous_keypoints.size() == 0 ) {
            continue;
        }

        // Find descriptors for these features
        brisk.compute( current_frame.img, current_keypoints, current_descriptors );



        if (epnp)
        {
            // CASE 1: SolvePnP
            matcher.match( current_descriptors, total_3D_descriptors, matches );

            // determine correct keypoints and corresponding 3d positions
            std::vector<cv::Point2f> imagepoints;
            std::vector<cv::Point3f> objectpoints;

            cv::Point2f cp;
            cv::Point3f op;
            for ( match_it = matches.begin(); match_it != matches.begin() + current_descriptors.rows; match_it++ ) {
                cp = current_keypoints[match_it->queryIdx].pt;
                op = total_3D_pointcloud[match_it->trainIdx];

                imagepoints.push_back(cp);
                objectpoints.push_back(op);
            }

            cv::Mat rvec, tvec, inliers;
            cv::solvePnPRansac(objectpoints, imagepoints, K, distortionCoeffs, rvec, tvec,
                               false, 100, 8.0, 100, inliers);

            // Construct matrix [I|0]
            cv::Matx34d P1( 1, 0, 0, 0,
                            0, 1, 0, 0,
                            0, 0, 1, 0 );

            // Construct matrix [R|t]
            cv::Matx33d R;
            cv::Rodrigues(rvec, R);
            cv::Matx34d P2;
            cv::hconcat(R, tvec, P2);

            std::cout << P2 << std::endl;

            //////////////////////////////////
            // Triangulate any (yet) unknown points
            matcher.match( current_descriptors, total_2D_descriptors, matches );
            std::vector<cv::Point2f> matching_2D_points, current_points;

            for ( match_it = matches.begin(); match_it != matches.begin() + current_descriptors.rows; match_it++ ) {
                current_points.push_back( current_keypoints[match_it->queryIdx].pt );
                matching_2D_points.push_back( total_2D_keypoints[match_it->trainIdx].pt );
            }

            cv::Matx33d fundamental;
            std::vector<cv::Point2f> previous_points_inliers, current_points_inliers;

            double mean_dist = determineFundamentalMatrix(matching_2D_points,
                                                          current_points,
                                                          previous_points_inliers,
                                                          current_points_inliers,
                                                          matches,
                                                          fundamental);

            //// Order of rotation must be roll pitch yaw for this to work
            //double roll = atan2(R(1,0), R(0,0));
            //double pitch = atan2(-R(2,0), sqrt( pow(R(2,1), 2) + pow(R(2,2), 2) ) );
            //double yaw = atan2(R(2,1), R(2,2));

            cv::Mat X ( 4, matches.size(), CV_64F, cv::Scalar( 1 ) );

            for ( int m = 0; m < (int) matches.size(); m++ ) {
                cv::Point3f previous_point_homogeneous( previous_points_inliers[m].x,
                                                        previous_points_inliers[m].y,
                                                        1 );
                cv::Point3f current_point_homogeneous( current_points_inliers[m].x,
                                                       current_points_inliers[m].y,
                                                       1 );

                cv::Matx31d X_a = IterativeLinearLSTriangulation(
                    previous_point_homogeneous,	P1,
                    current_point_homogeneous, P2 );

                X.at<double>(0,m) = X_a(0);
                X.at<double>(1,m) = X_a(1);
                X.at<double>(2,m) = X_a(2);
            }

            // Add to all_descriptors and 3d point cloud


        } else {
            // CASE 0: frame-to-frame

            // Match descriptor vectors using FLANN matcher
            matcher.match( current_descriptors, previous_descriptors, matches );

            // Calculation of centroid by looping over matches
            cv::Point2f current_centroid(0,0);
            cv::Point2f previous_centroid(0,0);
            std::vector<cv::Point2f> current_points_normalized, previous_points_normalized;
            cv::Point2f cp;
            cv::Point2f pp;

            for ( match_it = matches.begin(); match_it != matches.begin() + current_descriptors.rows; match_it++ ) {
                cp = current_keypoints[match_it->queryIdx].pt;
                pp = previous_keypoints[match_it->trainIdx].pt;

                current_centroid.x += cp.x;
                current_centroid.y += cp.y;
                current_points_normalized.push_back( cp );

                previous_centroid.x += pp.x;
                previous_centroid.y += pp.y;
                previous_points_normalized.push_back( pp );
            }

            // Normalize the centroids
            int matchesSize = matches.size();
            current_centroid.x /= matchesSize;
            current_centroid.y /= matchesSize;
            previous_centroid.x /= matchesSize;
            previous_centroid.y /= matchesSize;

            double current_scaling = 0;
            double previous_scaling = 0;

            // Translate points to have (0,0) as centroid
            for ( size_t i = 0; i < matches.size(); i++ ) {
                current_points_normalized[i] -= current_centroid;
                previous_points_normalized[i] -= previous_centroid;

                current_scaling += cv::norm( current_points_normalized[i] );
                previous_scaling += cv::norm( previous_points_normalized[i] );
            }

            // Enforce mean distance sqrt( 2 ) from origin (0,0)
            current_scaling  = sqrt( 2.0 ) * (double) matches.size() / current_scaling;
            previous_scaling = sqrt( 2.0 ) * (double) matches.size() / previous_scaling;

            // Compute transformation matrices
            cv::Matx33d current_T( current_scaling, 0,               -current_scaling * current_centroid.x,
                                   0,               current_scaling, -current_scaling * current_centroid.y,
                                   0,               0,               1
                                 );

            cv::Matx33d previous_T ( previous_scaling, 0,                -previous_scaling * previous_centroid.x,
                                     0,                previous_scaling, -previous_scaling * previous_centroid.y,
                                     0,                0,                1
                                   );

            // Scale points
            for ( size_t i = 0; i < matches.size(); i++ ) {
                previous_points_normalized[i] *= previous_scaling;
                current_points_normalized[i]  *= current_scaling;
            }

            // Find the fundamental matrix and reject outliers and calc distance between matches
            cv::Matx33d F;
            std::vector<cv::Point2f> current_points_normalized_inliers, previous_points_normalized_inliers;

            double mean_distance = determineFundamentalMatrix(previous_points_normalized,
                                                              current_points_normalized,
                                                              previous_points_normalized_inliers,
                                                              current_points_normalized_inliers,
                                                              matches,
                                                              F);

            // Scale up again
            F = current_T.t() * F * previous_T;

    #if VERBOSE
            std::cout << "Matches before pruning: " << matchesSize << ". " <<
                         "Matches after: " << matches.size() << "\n" <<
                         "Mean displacement: " << mean_distance << std::endl;
    #endif

            // Draw only inliers
            cv::Mat img_matches;
            cv::drawMatches(
                current_frame.img, current_keypoints, previous_frame.img, previous_keypoints,
                matches, img_matches, cv::Scalar::all( -1 ), cv::Scalar::all( -1 ),
                std::vector<char>(), cv::DrawMatchesFlags::NOT_DRAW_SINGLE_POINTS
                );

            imshow( "Good Matches", img_matches );
            imwrite("some.png", img_matches);

            // If displacement is not sufficiently large, skip this image.
            if (mean_distance < THRESHOLD)
            {
                if (mean_distance < 0.00001)
                {
                    // what the fuck just happened
                    std::cout << "wut 0 mean_distance wut" << std::endl;
                }
    #if VERBOSE
                std::cout << "Displacement not sufficiently large, skipping frame." << std::endl;
    #endif
                continue;
            }

            // Compute essential matrix
            cv::Matx33d E (cv::Mat(K.t() * F * K));

            // Estimation of projection matrix
            cv::Mat R1, R2, t;
            if( !DecomposeEtoRandT( E, R1, R2, t ) ) {
                return false;
            }

            // Check correctness(!(cv::Mat(visualOdometry->K).empty()))
            if ( cv::determinant(R1) < 0 ) R1 = -R1;
            if ( cv::determinant(R2) < 0 ) R2 = -R2;

            cv::Mat possible_projections[4];
            cv::hconcat( R1,  t, possible_projections[0] );
            cv::hconcat( R1, -t, possible_projections[1] );
            cv::hconcat( R2,  t, possible_projections[2] );
            cv::hconcat( R2, -t, possible_projections[3] );

            // Construct matrix [I|0]
            cv::Matx34d P1( 1, 0, 0, 0,
                            0, 1, 0, 0,
                            0, 0, 1, 0 );
            cv::Matx34d P2;

            int max_inliers = 0;
            std::vector<cv::Point3f> best_X;
            cv::Matx34d best_transform;

            // Loop over possible candidates
            for ( int i = 0 ; i < 4; i++ ) {
                P2 = possible_projections[i];

                std::vector<cv::Point3f> X;

                int num_inliers = 0;

                // TODO replace by iterator?
                for ( int m = 0; m < matches.size(); m++ ) {

                    cv::Point3f current_point_homogeneous( current_keypoints[matches[m].queryIdx].pt.x,
                                                           current_keypoints[matches[m].queryIdx].pt.y,
                                                           1 );
                    cv::Point3f previous_point_homogeneous( previous_keypoints[matches[m].trainIdx].pt.x,
                                                            previous_keypoints[matches[m].trainIdx].pt.y,
                                                            1 );

                    cv::Matx31d X_a = IterativeLinearLSTriangulation(
                        previous_point_homogeneous,	P1,
                        current_point_homogeneous, P2 );

                    X.push_back( cv::Point3f( X_a(0), X_a(1), X_a(2) ));

                    if ( X_a(0) > 0 ) {
                        num_inliers++;
                    }
                }
                if ( num_inliers > max_inliers ) {
                    max_inliers = num_inliers;

                    // update best_X
                    best_X = X;
                    best_transform = cv::Mat(P2).clone();
                }
            }
    #if VERBOSE
            //std::cout << best_X << std::endl;
            std::cout << best_transform << "\n" << std::endl;
    #endif

            // SOLVE THEM SCALE ISSUES for m = 1;
            double scale = solveScale(current_points_normalized, best_X, best_transform);
            std::cout << "Scale current: " << scale << std::endl;

            scale = solveScale(previous_points_normalized, best_X, best_transform);
            std::cout << "Scale previous: " << scale << std::endl;

            // Update total points/cloud
            total_3D_pointcloud = best_X;
            for (int matchnr = 0; matchnr < matches.size(); matchnr++)
            {
                total_3D_descriptors.push_back( current_descriptors.at<uchar>(matchnr) );
            }

            // TODO BE SMART

            //cv::Matx44d transformationMatrix( best_transform(0,0),best_transform(0,1),best_transform(0,2),best_transform(0,3),
            //                                  best_transform(1,0),best_transform(1,1),best_transform(1,2),best_transform(1,3),
            //                                  best_transform(2,0),best_transform(2,1),best_transform(2,2),best_transform(2,3),
            //                                  0, 0, 0, 1 );

            cv::Matx44d transformationMatrix;
            cv::vconcat( best_transform, cv::Matx14d(0, 0, 0, 1), transformationMatrix );

            robotPosition = transformationMatrix * robotPosition;
            robotPosition(0,0) /= robotPosition(3,0);
            robotPosition(1,0) /= robotPosition(3,0);
            robotPosition(2,0) /= robotPosition(3,0);
            robotPosition(3,0) /= robotPosition(3,0);

            std::cout << robotPosition.t() << std::endl;

            // Assign current values to the previous ones, for the next iteration
            previous_keypoints = current_keypoints;
            previous_frame = current_frame;
            previous_descriptors = current_descriptors;
        }
    }
    // Main loop successful.
    return true;
}

double VisualOdometry::solveScale(std::vector<cv::Point2f> imagepoints_normalized,
                                  std::vector<cv::Point3f> objectpoints_normalized,
                                  cv::Matx34d RTMatrix) {

    cv::Mat A (2 * imagepoints_normalized.size(), 1, CV_32F, 0.0f);
    cv::Mat b (2 * imagepoints_normalized.size(), 1, CV_32F, 0.0f);

    cv::Matx13f r1 (RTMatrix(0,0), RTMatrix(0,1), RTMatrix(0,2));
    cv::Matx13f r2 (RTMatrix(1,0), RTMatrix(1,1), RTMatrix(1,2));
    cv::Matx13f r3 (RTMatrix(2,0), RTMatrix(2,1), RTMatrix(2,2));
    cv::Mat temp1;
    cv::Mat temp2;

    double norm_t = cv::norm(RTMatrix.col(3));
    cv::Matx31f t (RTMatrix(0,3) / norm_t,
                   RTMatrix(1,3) / norm_t,
                   RTMatrix(2,3) / norm_t);

    cv::Point2f ip;
    cv::Point3f op;

    for (int i=0; i < imagepoints_normalized.size(); i++ ) {

        ip = imagepoints_normalized[i];
        op = objectpoints_normalized[i];
        cv::Matx31f objectpoint_mat( op.x, op.y, op.z );

        cv::subtract(r1, r3 * ip.x, temp1);
        cv::subtract(r2, r3 * ip.y, temp2);

        // Method 1
        A.at<float>(i*2,1) = t(2) * ip.x - t(0);
        b.at<float>(i*2,1) = ((cv::Mat)(temp1 * cv::Mat(objectpoint_mat))).at<float>(0,0);

        // Method 2
        A.at<float>(i*2+1,1) = t(2) * ip.y - t(1);
        b.at<float>(i*2+1,1) = ((cv::Mat)(temp2 * cv::Mat(objectpoint_mat))).at<float>(0,0);

        // Together, these comprise method 3
    }
    A = (A.t() * A).inv() * A.t();

    double s = ((cv::Mat)(A * b)).at<double>(0,0);

    return s;
}

VisualOdometry::VisualOdometry(InputSource *source){
    this->inputSource = source;

    // Load calibrationmatrix K (and distortioncoefficients while we're at it).
    this->validConfig = loadSettings( K, distortionCoeffs);
}

VisualOdometry::~VisualOdometry(){
    delete this->inputSource;
}


int main( int argc, char* argv[] ) {
    if ( argc < 3 ) {
        std::cerr << "Usage" << argv[0] << " '(-n robotIp|-f folderName)'" << std::endl;
        return 1;
    }

    VisualOdometry *visualOdometry;
    InputSource *inputSource;

    if ( std::string(argv[1]) == "-n" ) {
        const std::string robotIp( argv[2] );
        inputSource = new NaoInput( robotIp );
    } else if ( std::string(argv[1]) == "-f" ) {
        const std::string folderName(argv[2]);
        inputSource = new FileInput( folderName );
    } else {
        std::cout << "Wrong use of command line arguments." << std::endl;
        return 1;
    }

    visualOdometry = new VisualOdometry( inputSource );
    if (visualOdometry->validConfig)
    {
        visualOdometry->MainLoop();
    }
}
