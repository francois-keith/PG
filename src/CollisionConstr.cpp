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

// associated header
#include "CollisionConstr.h"

// include
// SCD
#include <SCD/CD/CD_Pair.h>

// PG
#include "ConfigStruct.h"
#include "PGData.h"
#include "FillSparse.h"

namespace pg
{


SCD::Matrix4x4 toSCD(const sva::PTransformd& t)
{
  SCD::Matrix4x4 m;
  const Eigen::Matrix3d& rot = t.rotation();
  const Eigen::Vector3d& tran = t.translation();

  for(int i = 0; i < 3; ++i)
  {
    for(int j = 0; j < 3; ++j)
    {
      m(i,j) = rot(j,i);
    }
  }

  m(0,3) = tran(0);
  m(1,3) = tran(1);
  m(2,3) = tran(2);

  return m;
}


// return the pair signed squared distance
double distance(SCD::CD_Pair* pair)
{
  return pair->getDistance();
}


// return the pair signed squared distance and the closest point position in world frame
std::tuple<double, Eigen::Vector3d, Eigen::Vector3d>
closestPoints(SCD::CD_Pair* pair)
{
  using namespace Eigen;

  SCD::Point3 pb1Tmp, pb2Tmp;
  double dist = pair->getClosestPoints(pb1Tmp, pb2Tmp);

  Vector3d T_0_p1(pb1Tmp[0], pb1Tmp[1], pb1Tmp[2]);
  Vector3d T_0_p2(pb2Tmp[0], pb2Tmp[1], pb2Tmp[2]);

  return std::make_tuple(dist, T_0_p1, T_0_p2);
}


/*
 *                             EnvCollisionConstr
 */


EnvCollisionConstr::EnvCollisionConstr(PGData* pgdata, const std::vector<EnvCollision>& cols)
  : roboptim::DifferentiableSparseFunction(pgdata->pbSize(), int(cols.size()), "EnvCollision")
  , pgdata_(pgdata)
  , nrNonZero_(0)
{
  cols_.reserve(cols.size());
  for(const EnvCollision& sc: cols)
  {
    rbd::Jacobian jac(pgdata_->mb(), sc.bodyId);
    Eigen::MatrixXd jacMat(1, jac.dof());
    cols_.push_back({pgdata_->multibody().bodyIndexById(sc.bodyId),
                     sc.bodyT, new SCD::CD_Pair(sc.bodyHull, sc.envHull),
                     jac, jacMat});
    nrNonZero_ += jac.dof();
  }
}


EnvCollisionConstr::~EnvCollisionConstr() throw()
{
  /// @todo try to use unique_ptr instead
  for(auto& cd: cols_)
  {
    delete cd.pair;
  }
}


void EnvCollisionConstr::impl_compute(result_t& res, const argument_t& x) const throw()
{
  pgdata_->x(x);
  int i = 0;
  for(const CollisionData& cd: cols_)
  {
    sva::PTransformd X_0_b(pgdata_->mbc().bodyPosW[cd.bodyIndex]);

    cd.pair->operator[](0)->setTransformation(toSCD(cd.bodyT*X_0_b));

    res(i) = distance(cd.pair);
    ++i;
  }
}


void EnvCollisionConstr::impl_jacobian(jacobian_t& jac, const argument_t& x) const throw()
{
  pgdata_->x(x);
  jac.reserve(nrNonZero_);

  int i = 0;
  for(CollisionData& cd: cols_)
  {
    sva::PTransformd X_0_b(pgdata_->mbc().bodyPosW[cd.bodyIndex]);

    cd.pair->operator[](0)->setTransformation(toSCD(cd.bodyT*X_0_b));

    double dist;
    Eigen::Vector3d T_0_p, pEnv;
    std::tie(dist, T_0_p, pEnv) = closestPoints(cd.pair);

    Eigen::Vector3d dist3d(T_0_p - pEnv);
    Eigen::Vector3d T_b_p(X_0_b.rotation()*(T_0_p - X_0_b.translation()));
    cd.jac.point(T_b_p);

    double coef = std::copysign(2., dist);
    const Eigen::MatrixXd& jacMat = cd.jac.jacobian(pgdata_->mb(), pgdata_->mbc());
    cd.jacMat.noalias() = coef*dist3d.transpose()*jacMat.block(3, 0, 3, cd.jac.dof());
    fullJacobianSparse(pgdata_->mb(), cd.jac, cd.jacMat, jac, {i, pgdata_->qParamsBegin()});
    ++i;
  }
}


/*
 *                             EnvCollisionConstr
 */


SelfCollisionConstr::SelfCollisionConstr(PGData* pgdata, const std::vector<SelfCollision>& cols)
  : roboptim::DifferentiableSparseFunction(pgdata->pbSize(), int(cols.size()), "EnvCollision")
  , pgdata_(pgdata)
  , nrNonZero_(0)
{
  cols_.reserve(cols.size());
  for(const SelfCollision& sc: cols)
  {
    rbd::Jacobian jac1(pgdata_->mb(), sc.body1Id);
    Eigen::MatrixXd jac1Mat(1, jac1.dof());
    Eigen::SparseMatrix<double, Eigen::RowMajor> jac1MatFull(outputSize(),
                                                             pgdata_->pbSize());

    jac1MatFull.reserve(jac1.dof());

    rbd::Jacobian jac2(pgdata_->mb(), sc.body2Id);
    Eigen::MatrixXd jac2Mat(1, jac2.dof());
    Eigen::SparseMatrix<double, Eigen::RowMajor> jac2MatFull(outputSize(),
                                                             pgdata_->pbSize());
    jac2MatFull.reserve(jac2.dof());

    cols_.push_back({pgdata_->multibody().bodyIndexById(sc.body1Id),
                     sc.body1T, jac1, jac1Mat, jac1MatFull,
                     pgdata_->multibody().bodyIndexById(sc.body2Id),
                     sc.body2T, jac2, jac2Mat, jac2MatFull,
                     new SCD::CD_Pair(sc.body1Hull, sc.body2Hull)});
    // not true, but better to ask bigger
    nrNonZero_ += jac1.dof() + jac2.dof();
  }
}


SelfCollisionConstr::~SelfCollisionConstr() throw()
{
  /// @todo try to use unique_ptr instead
  for(auto& cd: cols_)
  {
    delete cd.pair;
  }
}


void SelfCollisionConstr::impl_compute(result_t& res, const argument_t& x) const throw()
{
  pgdata_->x(x);
  int i = 0;
  for(const CollisionData& cd: cols_)
  {
    sva::PTransformd X_0_b1(pgdata_->mbc().bodyPosW[cd.body1Index]);
    sva::PTransformd X_0_b2(pgdata_->mbc().bodyPosW[cd.body2Index]);

    cd.pair->operator[](0)->setTransformation(toSCD(cd.body1T*X_0_b1));
    cd.pair->operator[](1)->setTransformation(toSCD(cd.body2T*X_0_b2));

    res(i) = distance(cd.pair);
    ++i;
  }
}


void SelfCollisionConstr::impl_jacobian(jacobian_t& jac, const argument_t& x) const throw()
{
  pgdata_->x(x);
  jac.reserve(nrNonZero_);

  int i = 0;
  for(CollisionData& cd: cols_)
  {
    sva::PTransformd X_0_b1(pgdata_->mbc().bodyPosW[cd.body1Index]);
    sva::PTransformd X_0_b2(pgdata_->mbc().bodyPosW[cd.body2Index]);

    cd.pair->operator[](0)->setTransformation(toSCD(cd.body1T*X_0_b1));
    cd.pair->operator[](1)->setTransformation(toSCD(cd.body2T*X_0_b2));

    double dist;
    Eigen::Vector3d T_0_p1, T_0_p2;
    std::tie(dist, T_0_p1, T_0_p2) = closestPoints(cd.pair);

    Eigen::Vector3d dist3d(T_0_p1 - T_0_p2);
    Eigen::Vector3d T_b_p1(X_0_b1.rotation()*(T_0_p1 - X_0_b1.translation()));
    Eigen::Vector3d T_b_p2(X_0_b2.rotation()*(T_0_p2 - X_0_b2.translation()));

    cd.jac1.point(T_b_p1);
    cd.jac2.point(T_b_p2);

    double coef = std::copysign(2., dist);
    const Eigen::MatrixXd& jac1Mat = cd.jac1.jacobian(pgdata_->mb(), pgdata_->mbc());
    const Eigen::MatrixXd& jac2Mat = cd.jac2.jacobian(pgdata_->mb(), pgdata_->mbc());

    cd.jac1Mat.noalias() = coef*dist3d.transpose()*jac1Mat.block(3, 0, 3, cd.jac1.dof());
    cd.jac2Mat.noalias() = coef*dist3d.transpose()*jac2Mat.block(3, 0, 3, cd.jac2.dof());

    updateFullJacobianSparse(pgdata_->mb(), cd.jac1, cd.jac1Mat, cd.jac1MatFull, {i, pgdata_->qParamsBegin()});
    updateFullJacobianSparse(pgdata_->mb(), cd.jac2, cd.jac2Mat, cd.jac2MatFull, {i, pgdata_->qParamsBegin()});
    jac += cd.jac1MatFull - cd.jac2MatFull;
    ++i;
  }
}


} // pg
