/**
* @file CameraCalibrator.h
*
* This file implements a module that can provide a semiautomatic camera calibration.
*
* @author Alexander Härtl
*/

#include "CameraCalibrator.h"
#include "Tools/Debugging/DebugDrawings.h"
#include "Tools/Math/Geometry.h"
#include "Tools/Math/Transformation.h"
#include "Tools/Streams/InStreams.h"
#include "Tools/Settings.h"
#include <limits>

using namespace std;

MAKE_MODULE(CameraCalibrator, Cognition Infrastructure)

CameraCalibrator::CameraCalibrator() :
  state(Idle), lastFetchedPoint(-1, -1), currentCamera(CameraInfo::lower), currentPoint(-1, -1)
{
  states[Idle] = std::bind(&CameraCalibrator::idle, this);
  states[Accumulate] = std::bind(&CameraCalibrator::accumulate, this);
  states[Optimize] = std::bind(&CameraCalibrator::optimize, this);
}

CameraCalibrator::~CameraCalibrator()
{
  if(optimizer)
  {
    delete optimizer;
  }
}

void CameraCalibrator::idle()
{
  // Doo nothing.
}

void CameraCalibrator::accumulate()
{
  if(theCameraInfo.camera != currentCamera)
    return;

  if(currentPoint == lastFetchedPoint)
  {
    return;
  }

  if(samples.size())
  {
    if((samples.back().pointInImage - currentPoint).abs() < 10)   // Move last point
      samples.pop_back();
  }

  lastFetchedPoint = currentPoint;

  // store all necessary information in the sample
  Vector2<> pointOnField;
  if(!Transformation::imageToRobot(currentPoint.x, currentPoint.y, theCameraMatrix, theCameraInfo, pointOnField))
  {
    OUTPUT(idText, text, "MEEEK! Point not on field!" << (theCameraInfo.camera == CameraInfo::upper ? " Upper " : " Lower "));
    return;
  }
  Sample sample;
  sample.pointInImage = currentPoint;
  sample.pointOnField = pointOnField; // for drawing
  sample.torsoMatrix = theTorsoMatrix;
  sample.headYaw = theFilteredJointData.angles[JointData::HeadYaw];
  sample.headPitch = theFilteredJointData.angles[JointData::HeadPitch];
  sample.cameraInfo = theCameraInfo;

  samples.push_back(sample);
}

void CameraCalibrator::optimize()
{
  if(!optimizer)
  {
    vector<float> initialParameters;
    initialParameters.resize(numOfParameterTranslations);
    // since the parameters for the robot pose are correction parameters, an empty RobotPose is used instead of theRobotPose
    translateParameters(nextCameraCalibration, RobotPose(), initialParameters);
    optimizer = new GaussNewtonOptimizer<Sample, CameraCalibrator>(initialParameters, samples, *this, &CameraCalibrator::computeErrorParameterVector);
    successiveConvergations = 0;
    framesToWait = 0;
  }
  else
  {
    // only do an iteration after some frames have passed
    if(framesToWait <= 0)
    {
      framesToWait = numOfFramesToWait;
      const float delta = optimizer->iterate();
      OUTPUT(idText, text, "CameraCalibrator: delta = " << delta);

      // the camera calibration is refreshed from the current optimizer state
      RobotPose robotPose;
      const vector<float> currentParameters = optimizer->getParameters();
      translateParameters(currentParameters, nextCameraCalibration, robotPose);

      if(abs(delta) < terminationCriterion)
      {
        ++successiveConvergations;
      }
      if(successiveConvergations >= minSuccessiveConvergations)
      {
        state = Idle;
        OUTPUT_TEXT("CameraCalibrator: converged!");
        OUTPUT_TEXT("RobotPoseCorrection: "
                    << currentParameters[robotPoseCorrectionX] * 1000.0f << " "
                    << currentParameters[robotPoseCorrectionY] * 1000.0f << " "
                    << currentParameters[robotPoseCorrectionRot]);
        currentRobotPose.translation.x += currentParameters[robotPoseCorrectionX] * 1000.0f;
        currentRobotPose.translation.y += currentParameters[robotPoseCorrectionY] * 1000.0f;
        currentRobotPose.rotation = normalize(currentRobotPose.rotation + currentParameters[robotPoseCorrectionRot]);
        state = Idle;
        delete optimizer;
        optimizer = nullptr;
      }
    }
    --framesToWait;
  }
}

void CameraCalibrator::update(CameraCalibration& cameraCalibration)
{
  RobotCameraMatrix robotCameraMatrix(theRobotDimensions, theFilteredJointData.angles[JointData::HeadYaw],
                                      theFilteredJointData.angles[JointData::HeadPitch], cameraCalibration, theCameraInfo.camera == CameraInfo::upper);
  theCameraMatrix.computeCameraMatrix(theTorsoMatrix, robotCameraMatrix, cameraCalibration);

  nextCameraCalibration = cameraCalibration;

  // Select camera for current point selection
  MODIFY("module:CameraCalibrator:currentCamera", currentCamera);

  // this is the interface to the ImageView of the simulator
  MODIFY("module:CameraCalibrator:point", currentPoint);

  MODIFY_ONCE("module:CameraCalibrator:robotPose", currentRobotPose);

  processManualControls();
  states[state]();
  draw();

  cameraCalibration = nextCameraCalibration;
}

void CameraCalibrator::processManualControls()
{
  DEBUG_RESPONSE_ONCE("module:CameraCalibrator:collectPoints",
  {
    if(state == Idle)
    {
      state = Accumulate;
    }
  });
  DEBUG_RESPONSE_ONCE("module:CameraCalibrator:undo",
  {
    if(state == Accumulate && !samples.empty())
    {
      samples.pop_back();
    }
  });
  DEBUG_RESPONSE_ONCE("module:CameraCalibrator:clear",
  {
    if(state == Idle || state == Accumulate)
    {
      samples.clear();
    }
  });
  DEBUG_RESPONSE_ONCE("module:CameraCalibrator:optimize",
  {
    if(!(samples.size() > numOfParameterTranslations))
    {
      OUTPUT_TEXT("CameraCalibrator: Error! Too few samples!");
    }
    else
    {
      state = Optimize;
    }
  });
  DEBUG_RESPONSE_ONCE("module:CameraCalibrator:stop", state = Idle;);
}

void CameraCalibrator::draw()
{
  DECLARE_DEBUG_DRAWING("module:CameraCalibrator:drawFieldLines", "drawingOnImage");
  DECLARE_DEBUG_DRAWING("module:CameraCalibrator:drawSamples", "drawingOnImage");
  DECLARE_DEBUG_DRAWING("module:CameraCalibrator:points", "drawingOnImage",
  {
    DRAWTEXT("module:CameraCalibrator:points", 10, -10, 40,
    !(samples.size() > numOfParameterTranslations) ? ColorRGBA::red : ColorRGBA::green,
    "Points collected: " << (unsigned)samples.size());
  });

  COMPLEX_DRAWING("module:CameraCalibrator:drawFieldLines", drawFieldLines(););
  COMPLEX_DRAWING("module:CameraCalibrator:drawSamples", drawSamples(););
}

void CameraCalibrator::drawFieldLines()
{
  const Pose2D robotPoseInv = currentRobotPose.invert();
  for(vector<FieldDimensions::LinesTable::Line>::const_iterator i = theFieldDimensions.fieldLines.lines.begin(); i != theFieldDimensions.fieldLines.lines.end(); ++i)
  {
    FieldDimensions::LinesTable::Line lineOnField(*i);
    lineOnField.corner = robotPoseInv + lineOnField.corner;
    Geometry::Line lineInImage;
    if(projectLineOnFieldIntoImage(Geometry::Line(lineOnField.corner, lineOnField.length), theCameraMatrix, theCameraInfo, lineInImage))
    {
      LINE("module:CameraCalibrator:drawFieldLines", lineInImage.base.x, lineInImage.base.y, (lineInImage.base + lineInImage.direction).x, (lineInImage.base + lineInImage.direction).y, 1, Drawings::ps_solid, ColorRGBA::black);
    }
  }
}

void CameraCalibrator::drawSamples()
{
  for(Sample& sample : samples)
  {
    ColorRGBA color = sample.cameraInfo.camera == CameraInfo::upper ? ColorRGBA::orange : ColorRGBA::red;
    Vector2<> pointInImage;
    if(Transformation::robotToImage(sample.pointOnField, theCameraMatrix, theCameraInfo, pointInImage))
    {
      CROSS("module:CameraCalibrator:drawSamples", pointInImage.x, pointInImage.y, 5, 1, Drawings::ps_solid, color);
    }
  }
}

float CameraCalibrator::computeError(const Sample& sample, const CameraCalibration& cameraCalibration, const RobotPose& robotPose, bool inImage) const
{
  // build camera matrix from sample and camera calibration
  const RobotCameraMatrix robotCameraMatrix(theRobotDimensions, sample.headYaw, sample.headPitch, cameraCalibration, sample.cameraInfo.camera == CameraInfo::upper);
  const CameraMatrix cameraMatrix(sample.torsoMatrix, robotCameraMatrix, cameraCalibration);

  if(inImage)
  {
    const Pose2D robotPoseInv = robotPose.invert();
    float minimum = numeric_limits<float>::max();
    for(vector<FieldDimensions::LinesTable::Line>::const_iterator i = theFieldDimensions.fieldLines.lines.begin(); i != theFieldDimensions.fieldLines.lines.end(); ++i)
    {
      FieldDimensions::LinesTable::Line lineOnField(*i);
      // transform the line in robot relative coordinates
      lineOnField.corner = robotPoseInv + lineOnField.corner;
      Geometry::Line lineInImage;
      float distance;
      if(!projectLineOnFieldIntoImage(Geometry::Line(lineOnField.corner, lineOnField.length), cameraMatrix, sample.cameraInfo, lineInImage))
      {
        distance = aboveHorizonError;
      }
      else
      {
        distance = Geometry::getDistanceToEdge(lineInImage, Vector2<>(sample.pointInImage));
      }
      if(distance < minimum)
      {
        minimum = distance;
      }
    }
    return minimum;
  }
  else // on ground
  {
    // project point in image onto ground
    Vector3<> cameraRay(sample.cameraInfo.focalLength, sample.cameraInfo.opticalCenter.x - sample.pointInImage.x, sample.cameraInfo.opticalCenter.y - sample.pointInImage.y);
    cameraRay = cameraMatrix * cameraRay;
    if(cameraRay.z >= 0) // above horizon
    {
      return aboveHorizonError;
    }
    const float scale = cameraMatrix.translation.z / -cameraRay.z;
    cameraRay *= scale;
    Vector2<> pointOnGround(cameraRay.x, cameraRay.y); // point on ground relative to the robot
    pointOnGround = robotPose * pointOnGround; // point on ground in absolute coordinates

    float minimum = numeric_limits<float>::max();
    for(vector<FieldDimensions::LinesTable::Line>::const_iterator i = theFieldDimensions.fieldLines.lines.begin(); i != theFieldDimensions.fieldLines.lines.end(); ++i)
    {
      const Geometry::Line line(i->corner, i->length);
      const float distance = Geometry::getDistanceToEdge(line, pointOnGround);
      if(distance < minimum)
      {
        minimum = distance;
      }
    }
    return minimum;
  }
}

float CameraCalibrator::computeErrorParameterVector(const Sample& sample, const vector<float>& parameters) const
{
  CameraCalibration cameraCalibration = nextCameraCalibration;
  RobotPose robotPose;
  translateParameters(parameters, cameraCalibration, robotPose);

  // the correction parameters for the robot pose are added to theRobotPose
  // in the parameter space the robot pose translation unit is m to keep the order of magnitude similar to the other parameters
  robotPose.translation *= 1000.0f;
  robotPose.translation += currentRobotPose.translation;
  robotPose.rotation += currentRobotPose.rotation;

  return computeError(sample, cameraCalibration, robotPose);
}

void CameraCalibrator::translateParameters(const vector<float>& parameters, CameraCalibration& cameraCalibration, RobotPose& robotPose) const
{
  ASSERT(parameters.size() == numOfParameterTranslations || parameters.size() == numOfParametersLowerCamera);

  cameraCalibration.lowerCameraTiltCorrection = parameters[cameraTiltCorrection];
  cameraCalibration.lowerCameraRollCorrection = parameters[cameraRollCorrection];
  cameraCalibration.bodyTiltCorrection = parameters[bodyTiltCorrection];
  cameraCalibration.bodyRollCorrection = parameters[bodyRollCorrection];

  robotPose.translation.x = parameters[robotPoseCorrectionX];
  robotPose.translation.y = parameters[robotPoseCorrectionY];
  robotPose.rotation = parameters[robotPoseCorrectionRot];

  cameraCalibration.upperCameraRollCorrection = parameters[upperCameraX];
  cameraCalibration.upperCameraTiltCorrection = parameters[upperCameraY];
  cameraCalibration.upperCameraPanCorrection = parameters[upperCameraZ];
}

void CameraCalibrator::translateParameters(const CameraCalibration& cameraCalibration, const RobotPose& robotPose, vector<float>& parameters) const
{
  ASSERT(parameters.size() == numOfParameterTranslations || parameters.size() == numOfParametersLowerCamera);

  parameters[cameraTiltCorrection] = cameraCalibration.lowerCameraTiltCorrection;
  parameters[cameraRollCorrection] = cameraCalibration.lowerCameraRollCorrection;
  parameters[bodyTiltCorrection] = cameraCalibration.bodyTiltCorrection;
  parameters[bodyRollCorrection] = cameraCalibration.bodyRollCorrection;

  parameters[robotPoseCorrectionX] = robotPose.translation.x;
  parameters[robotPoseCorrectionY] = robotPose.translation.y;
  parameters[robotPoseCorrectionRot] = robotPose.rotation;

  parameters[upperCameraX] = cameraCalibration.upperCameraRollCorrection;
  parameters[upperCameraY] = cameraCalibration.upperCameraTiltCorrection;
  parameters[upperCameraZ] = cameraCalibration.upperCameraPanCorrection;
}

bool CameraCalibrator::projectLineOnFieldIntoImage(const Geometry::Line& lineOnField, const CameraMatrix& cameraMatrix,
    const CameraInfo& cameraInfo, Geometry::Line& lineInImage) const
{
  const float& f = cameraInfo.focalLength;
  const Pose3D cameraMatrixInv = cameraMatrix.invert();

  // TODO more elegant solution directly using the direction of the line?

  // start and end point of the line
  Vector2<> p1 = lineOnField.base;
  Vector2<> p2 = p1 + lineOnField.direction;
  Vector3<> p1Camera(p1.x, p1.y, 0);
  Vector3<> p2Camera(p2.x, p2.y, 0);

  // points are transformed into camera coordinates
  p1Camera = cameraMatrixInv * p1Camera;
  p2Camera = cameraMatrixInv * p2Camera;

  // handle the case that points can lie behind the camera plane
  const bool p1Behind = p1Camera.x < cameraInfo.focalLength;
  const bool p2Behind = p2Camera.x < cameraInfo.focalLength;
  if(p1Behind && p2Behind)
  {
    return false;
  }
  else if(!p1Behind && !p2Behind)
  {
    // both rays can be simply intersected with the image plane
    p1Camera /= (p1Camera.x / f);
    p2Camera /= (p2Camera.x / f);
  }
  else
  {
    // if one point lies behind the camera and the other in front, there must be an intersection of the connective line with the image plane
    const Vector3<> direction = p1Camera - p2Camera;
    const float scale = (f - p1Camera.x) / direction.x;
    const Vector3<> intersection = p1Camera + direction * scale;
    if(p1Behind)
    {
      p1Camera = intersection;
      p2Camera /= (p2Camera.x / f);
    }
    else
    {
      p2Camera = intersection;
      p1Camera /= (p1Camera.x / f);
    }
  }
  const Vector2<> p1Result(cameraInfo.opticalCenter.x - p1Camera.y, cameraInfo.opticalCenter.y - p1Camera.z);
  const Vector2<> p2Result(cameraInfo.opticalCenter.x - p2Camera.y, cameraInfo.opticalCenter.y - p2Camera.z);
  lineInImage.base = p1Result;
  lineInImage.direction = p2Result - p1Result;
  return true;
}

