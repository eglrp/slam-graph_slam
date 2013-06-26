#include "extended_sparse_optimizer.hpp"

#include <limits>
#include <graph_slam/vertex_se3_gicp.hpp>

#include <g2o/core/factory.h>
#include <g2o/core/optimization_algorithm_factory.h>
#include <g2o/core/block_solver.h>
#include <g2o/core/optimization_algorithm_gauss_newton.h>
#include <g2o/solvers/pcg/linear_solver_pcg.h>
#include <g2o/solvers/cholmod/linear_solver_cholmod.h>
#include <g2o/solvers/csparse/linear_solver_csparse.h>
#include <g2o/solvers/dense/linear_solver_dense.h>


namespace graph_slam 
{
    
ExtendedSparseOptimizer::ExtendedSparseOptimizer() : SparseOptimizer(), next_vertex_id(0), initialized(false), 
        odometry_pose_last_vertex(Eigen::Isometry3d::Identity()), odometry_covariance_last_vertex(Matrix6d::Identity())
{
    setupOptimizer();
}

ExtendedSparseOptimizer::~ExtendedSparseOptimizer()
{
}

void ExtendedSparseOptimizer::setupOptimizer()
{
    typedef g2o::BlockSolver< g2o::BlockSolverTraits<-1, -1> >  SlamBlockSolver;
    //typedef g2o::LinearSolverPCG<SlamBlockSolver::PoseMatrixType> SlamLinearSolver;
    typedef g2o::LinearSolverCSparse<SlamBlockSolver::PoseMatrixType> SlamLinearSolver;
    //typedef g2o::LinearSolverCholmod<SlamBlockSolver::PoseMatrixType> SlamLinearSolver;
    //typedef g2o::LinearSolverDense<SlamBlockSolver::PoseMatrixType> SlamLinearSolver;
    
    // allocating the optimizer
    SlamLinearSolver* linearSolver = new SlamLinearSolver();
    SlamBlockSolver* blockSolver = new SlamBlockSolver(linearSolver);
    //g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(blockSolver);
    g2o::OptimizationAlgorithmGaussNewton* solver = new g2o::OptimizationAlgorithmGaussNewton(blockSolver);
    
    setAlgorithm(solver);
}

void ExtendedSparseOptimizer::updateGICPConfiguration(const GICPConfiguration& gicp_config)
{
    this->gicp_config = gicp_config;
    
    for(g2o::HyperGraph::EdgeSet::iterator it = _edges.begin(); it != _edges.end(); it++)
    {
        graph_slam::EdgeSE3_GICP* edge = dynamic_cast<graph_slam::EdgeSE3_GICP*>(*it);
        if(edge)
            edge->setGICPConfiguration(gicp_config);
    }
}

bool ExtendedSparseOptimizer::addVertex(const base::samples::RigidBodyState& rigid_body_state, envire::Pointcloud* point_cloud, bool delayed_icp_update)
{
    if(next_vertex_id == std::numeric_limits<uint64_t>::max())
    {
        // this should not happen under normal circumstances
        throw std::runtime_error("Can't add any new vertex. Max id count has been reached.");
        return false;
    }
    
    // get odometry pose and covariance
    Eigen::Isometry3d odometry_pose(rigid_body_state.getTransform().matrix());
    Matrix6d odometry_covariance = combineToPoseCovariance(rigid_body_state.cov_position, rigid_body_state.cov_orientation);
    
    // create new vertex
    graph_slam::VertexSE3_GICP* vertex = new graph_slam::VertexSE3_GICP();
    vertex->setId(next_vertex_id);
    
    // attach point cloud to vertex
    vertex->attachPointCloud(point_cloud, gicp_config.point_cloud_density);
    
    // added vertex to the graph
    if(!g2o::SparseOptimizer::addVertex(vertex))
    {
        std::cerr << "failed to add a new vertex." << std::endl;
        delete vertex;
        return false;
    }
    
    if(next_vertex_id == 0)
    {
        // set first vertex fixed
        vertex->setFixed(true);
        
        // set odometry pose as inital pose
        vertex->setEstimate(odometry_pose);
    }
    else
    {
        graph_slam::VertexSE3_GICP *source_vertex = dynamic_cast<graph_slam::VertexSE3_GICP*>(this->vertex(next_vertex_id-1));
        Eigen::Isometry3d odometry_pose_delta = odometry_pose_last_vertex.inverse() * odometry_pose;
        Matrix6d odometry_covariance_delta = odometry_covariance_last_vertex.inverse() * odometry_covariance;
        
        // set pose of the source vertex times odometry delta as inital pose
        vertex->setEstimate(source_vertex->estimate() * odometry_pose_delta);
        
        // create an odometry based edge between the last and the new vertex
        g2o::EdgeSE3* odometry_edge = new g2o::EdgeSE3();
        odometry_edge->vertices()[0] = source_vertex;
        odometry_edge->vertices()[1] = vertex;
        
        odometry_edge->setMeasurement(odometry_pose_delta);
        odometry_edge->setInformation(odometry_covariance_delta.inverse());

        if(!g2o::SparseOptimizer::addEdge(odometry_edge))
        {
            std::cerr << "failed to add a new odometry based edge." << std::endl;
            g2o::SparseOptimizer::removeVertex(vertex);
            delete odometry_edge;
            delete vertex;
            return false;
        }
        
        // create an icp based edge between the last and the new vertex
        graph_slam::EdgeSE3_GICP* icp_edge = new graph_slam::EdgeSE3_GICP();
        icp_edge->setSourceVertex(source_vertex);
        icp_edge->setTargetVertex(vertex);

        if(!icp_edge->setMeasurementFromGICP(delayed_icp_update))
            throw std::runtime_error("compute transformation using gicp failed!");
        
        if(!g2o::SparseOptimizer::addEdge(icp_edge))
        {
            std::cerr << "failed to add a new icp based edge." << std::endl;
            g2o::SparseOptimizer::removeEdge(odometry_edge);
            g2o::SparseOptimizer::removeVertex(vertex);
            delete icp_edge;
            delete vertex;
            return false;
        }
        edges_to_add.insert(icp_edge);
    }
    
    vertices_to_add.insert(vertex);
    odometry_pose_last_vertex = odometry_pose;
    odometry_covariance_last_vertex = odometry_covariance;
    
    next_vertex_id++;
    return true;
}

int ExtendedSparseOptimizer::optimize(int iterations, bool online)
{
    // update hessian matrix
    if(vertices_to_add.size())
    {
        if(initialized)
        {
            if(!updateInitialization(vertices_to_add, edges_to_add))
                throw std::runtime_error("update optimization failed!");
        }
        else
        {
            if(!initializeOptimization())
                throw std::runtime_error("initialize optimization failed!");
            initialized = true;
        }
        vertices_to_add.clear();
        edges_to_add.clear();
    }
    
    
    // do optimization
    return g2o::SparseOptimizer::optimize(iterations, online);
}

bool ExtendedSparseOptimizer::updateEnvireTransformations()
{
    unsigned err_counter = 0;
    for(VertexIDMap::const_iterator it = _vertices.begin(); it != _vertices.end(); it++)
    {
        graph_slam::VertexSE3_GICP *vertex = static_cast<graph_slam::VertexSE3_GICP*>(it->second);
        if(vertex && !vertex->updateEnvireTransformation())
            err_counter++;
    }
    return !err_counter;
}
    
}