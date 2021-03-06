// This file is part of PG.
//
// PG is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// PG is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with PG.  If not, see <http://www.gnu.org/licenses/>.

#pragma once

// include
// std
#include <cassert>
#include <vector>

// boost
#include <boost/math/constants/constants.hpp>

// Eigen
#include <Eigen/Core>

// RBDyn
#include <RBDyn/MultiBody.h>

// PG
#include "FK.h"
#include "ID.h"

namespace pg
{

template<typename Type>
class PGData
{
public:
  typedef typename Type::scalar_t scalar_t;
  typedef typename Type::construct_f construct_f;

public:
  struct ForceData
  {
    int bodyIndex;
    std::vector<sva::PTransform<scalar_t>> points;
    std::vector<sva::ForceVec<scalar_t>> forces;
    double mu;
  };

  struct EllipseData
  {
    int bodyIndex;  //Each ellipse is defined relatively to a Surface of a Body
    scalar_t x;     //x coord of the center
    scalar_t y;     //y coord of the center
    scalar_t theta; //Angle between the x-axis and the first axis of the ellipse
    scalar_t r1;    //First radius
    scalar_t r2;    //Second radius
    std::string print()
    {
      std::stringstream result;
      result << "ellipse = Ellipse((" << this->x << ", " << this->y << "), ";
      result << 2*this->r1 << ", " << 2*this->r2 << ", " << 180*this->theta/boost::math::constants::pi<double>() << ")\n";
      return result.str();
    }
  };

public:
  PGData(const rbd::MultiBody& mb, const Eigen::Vector3d& gravity);

  void x(const Eigen::VectorXd& x);

  void forces(std::vector<ForceData> fd);
  void ellipses(std::vector<EllipseData> ed);
  void update();

  const std::vector<std::vector<scalar_t> >& q()
  {
    return q_;
  }

  const FK<scalar_t>& fk() const
  {
    return fk_;
  }

  const ID<scalar_t>& id()
  {
    if(xStamp_ != idStamp_)
    {
      id_.run(mb_, fk_.bodyPosW(), fk_.parentToSon(), forcesB_);
      idStamp_ = xStamp_;
    }
    return id_;
  }

  const rbd::MultiBody& multibody() const
  {
    return mb_;
  }

  int pbSize() const
  {
    return mb_.nrParams() + nrForcePoints_*3 + int(ellipseDatas_.size())*5;
  }

  int forceParamsBegin() const
  {
    return mb_.nrParams();
  }

  int ellipseParamsBegin() const
  {
    return mb_.nrParams() + nrForcePoints_*3;
  }

  int nrForcePoints() const
  {
    return nrForcePoints_;
  }

  const std::vector<ForceData>& forceDatas() const
  {
    return forceDatas_;
  }

  const std::vector<EllipseData>& ellipseDatas() const
  {
    return ellipseDatas_;
  }


  const Eigen::Vector3d& gravity() const
  {
    return gravity_;
  }

  std::size_t xStamp() const
  {
    return xStamp_;
  }

private:
  rbd::MultiBody mb_;
  Eigen::Vector3d gravity_;

  Eigen::VectorXd x_;
  std::vector<std::vector<scalar_t>> q_;

  std::vector<ForceData> forceDatas_;
  int nrForcePoints_;
  std::vector<sva::ForceVec<scalar_t>> forcesB_;

  std::vector<EllipseData> ellipseDatas_;

  FK<scalar_t> fk_;
  ID<scalar_t> id_;

  std::size_t xStamp_;
  std::size_t idStamp_;
};


// inline


template<typename Type>
PGData<Type>::PGData(const rbd::MultiBody& mb, const Eigen::Vector3d& gravity)
  : mb_(mb)
  , gravity_(gravity)
  , x_(mb.nrParams())
  , q_(mb.nrJoints())
  , nrForcePoints_(0)
  , forcesB_(mb.nrBodies())
  , fk_(mb)
  , id_(mb, gravity)
  , xStamp_(1)
  , idStamp_(1)
{
  x_.setZero();
  for(int i = 0; i < mb.nrJoints(); ++i)
  {
    q_[i].resize(mb.joint(i).params());
  }
  for(int i = 0; i < mb.nrBodies(); ++i)
  {
    forcesB_[i] = sva::ForceVec<scalar_t>(Eigen::Vector6<scalar_t>::Zero());
  }
}


template<typename Type>
void PGData<Type>::x(const Eigen::VectorXd& x)
{
  assert(x.size() == x_.size());

  Eigen::VectorXd xNorm = x;
  if(mb_.joint(0).type() == rbd::Joint::Free)
  {
    xNorm.head(4) /= xNorm.head(4).norm();
  }

  if(x_ != xNorm)
  {
    x_ = xNorm;
    ++xStamp_;
    update();
  }
}


template<typename Type>
void PGData<Type>::forces(std::vector<ForceData> fd)
{
  forceDatas_ = std::move(fd);
  nrForcePoints_ = 0;
  for(const ForceData& fd: forceDatas_)
  {
    nrForcePoints_ += int(fd.points.size());
  }

  x_.setZero(pbSize());
  ++xStamp_;
}


template<typename Type>
void PGData<Type>::ellipses(std::vector<EllipseData> ed)
{
  ellipseDatas_ = std::move(ed);
  x_.setZero(pbSize());
  ++xStamp_;
}


template<typename Type>
void PGData<Type>::update()
{
  int xPos = 0;
  for(int i = 0; i < mb_.nrJoints(); ++i)
  {
    for(int j = 0; j < mb_.joint(i).params(); ++j)
    {
      construct_f()(int(x_.size()), xPos, x_[xPos], q_[i][j]);
      ++xPos;
    }
  }
  fk_.init(pbSize());
  fk_.run(mb_, q_);

  scalar_t zero(0., Eigen::VectorXd::Zero(x_.size()));
  Eigen::Vector3<scalar_t> zeroVec3(zero, zero, zero);
  for(ForceData& fd: forceDatas_)
  {
    forcesB_[fd.bodyIndex] = sva::ForceVec<scalar_t>(Eigen::Vector6<scalar_t>::Zero());
    for(std::size_t i = 0; i < fd.forces.size(); ++i)
    {
      sva::ForceVec<scalar_t>& fv = fd.forces[i];
      const sva::PTransform<scalar_t>& pt = fd.points[i];
      Eigen::Vector3<scalar_t> forceAd;
      construct_f()(int(x_.size()), xPos + 0, x_[xPos + 0], forceAd(0));
      construct_f()(int(x_.size()), xPos + 1, x_[xPos + 1], forceAd(1));
      construct_f()(int(x_.size()), xPos + 2, x_[xPos + 2], forceAd(2));
      fv = sva::ForceVec<scalar_t>(zeroVec3, forceAd);
      forcesB_[fd.bodyIndex] = forcesB_[fd.bodyIndex] + pt.transMul(fv);
      xPos += 3;
    }
  }

  for(EllipseData& ed: ellipseDatas_)
  {
    construct_f()(int(x_.size()), xPos + 0, x_[xPos + 0], ed.x);
    construct_f()(int(x_.size()), xPos + 1, x_[xPos + 1], ed.y);
    construct_f()(int(x_.size()), xPos + 2, x_[xPos + 2], ed.theta);
    construct_f()(int(x_.size()), xPos + 3, x_[xPos + 3], ed.r1);
    construct_f()(int(x_.size()), xPos + 4, x_[xPos + 4], ed.r2);
    xPos += 5;
  }
}

} // namespace pg
