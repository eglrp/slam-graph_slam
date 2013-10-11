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

#include <envire/maps/Pointcloud.hpp>
#include <envire/maps/MLSGrid.hpp>

namespace graph_slam 
{
    
ExtendedSparseOptimizer::ExtendedSparseOptimizer() : SparseOptimizer()
{
    initValues();
    setupOptimizer();
    env.reset(new envire::Environment);
}

ExtendedSparseOptimizer::~ExtendedSparseOptimizer()
{
}

void ExtendedSparseOptimizer::initValues()
{
    next_vertex_id = 0;
    initialized = false;
    odometry_pose_last_vertex = Eigen::Isometry3d::Identity();
    odometry_covariance_last_vertex = Matrix6d::Identity();
    last_vertex = NULL;
    new_edges_added = false;
    use_mls = false;
    use_vertex_grid = false;
    map_update_necessary = false;
}

void ExtendedSparseOptimizer::clear()
{
    initValues();
    env.reset(new envire::Environment);
    vertex_grid.reset();
    vertices_to_add.clear();
    edges_to_add.clear();
    cov_graph.clear();

    SparseOptimizer::clear();
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

    // setup cov graph
    SlamLinearSolver* covLinearSolver = new SlamLinearSolver();
    SlamBlockSolver* covBlockSolver = new SlamBlockSolver(covLinearSolver);
    g2o::OptimizationAlgorithmGaussNewton* covSolver = new g2o::OptimizationAlgorithmGaussNewton(covBlockSolver);
    cov_graph.setAlgorithm(covSolver);
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

bool ExtendedSparseOptimizer::addVertex(const envire::TransformWithUncertainty& transformation, std::vector<Eigen::Vector3d>& pointcloud, 
                                        const Eigen::Affine3d& sensor_origin, bool delayed_icp_update)
{
    if(next_vertex_id == std::numeric_limits<int>::max())
    {
        // this should not happen under normal circumstances
        throw std::runtime_error("Can't add any new vertex. Max id count has been reached.");
        return false;
    }
    
    // get odometry pose and covariance
    Eigen::Isometry3d odometry_pose(transformation.getTransform().matrix());
    Matrix6d odometry_covariance = switchEnvireG2oCov(transformation.getCovariance());
    
    // check for nan values
    if(is_nan(odometry_pose.matrix()))
    {
        throw std::runtime_error("Odometry pose matrix contains not numerical entries!");
        return false;
    }
    else if(is_nan(odometry_covariance))
    {
        throw std::runtime_error("Odometry covaraince matrix contains not numerical entries!");
        return false;
    }
    
    // create new vertex
    graph_slam::VertexSE3_GICP* vertex = new graph_slam::VertexSE3_GICP();
    vertex->setId(next_vertex_id);
    
    // attach point cloud to vertex
    envire::Pointcloud* envire_pointcloud = new envire::Pointcloud();
    envire_pointcloud->vertices = pointcloud;
    envire_pointcloud->setSensorOrigin(sensor_origin);
    vertex->attachPointCloud(envire_pointcloud);
    
    // added vertex to the graph
    if(!g2o::SparseOptimizer::addVertex(vertex))
    {
        std::cerr << "failed to add a new vertex." << std::endl;
        delete vertex;
        delete envire_pointcloud;
        return false;
    }
    
    if(next_vertex_id == 0 || last_vertex == NULL)
    {
        // set first vertex fixed
        vertex->setFixed(true);
        
        // set odometry pose as inital pose
        vertex->setEstimate(odometry_pose);

        // do inital update of the map if the first fixed vertex is available
        map_update_necessary = true;
    }
    else
    {
        Eigen::Isometry3d odometry_pose_delta = odometry_pose_last_vertex.inverse() * odometry_pose;
        Matrix6d odometry_covariance_delta = odometry_covariance_last_vertex.inverse() * odometry_covariance;
        
        // set pose of the source vertex times odometry delta as inital pose
        vertex->setEstimate(last_vertex->estimate() * odometry_pose_delta);
 
        // create an edge between the last and the new vertex
        graph_slam::EdgeSE3_GICP* edge = new graph_slam::EdgeSE3_GICP();
        edge->setSourceVertex(last_vertex);
        edge->setTargetVertex(vertex);
        edge->setGICPConfiguration(gicp_config);
        
        edge->setMeasurement(odometry_pose_delta);
        edge->setInformation((Matrix6d::Identity() + odometry_covariance_delta).inverse());

        if(!edge->setMeasurementFromGICP(delayed_icp_update))
            throw std::runtime_error("compute transformation using gicp failed!");
        
        if(!g2o::SparseOptimizer::addEdge(edge))
        {
            std::cerr << "failed to add a new edge." << std::endl;
            g2o::SparseOptimizer::removeVertex(vertex);
            delete edge;
            delete vertex;
            delete envire_pointcloud;
            return false;
        }
        edges_to_add.insert(edge);
    }
    
    // add pointcloud to environment
    envire::FrameNode* framenode = new envire::FrameNode();
    framenode->setTransform(Eigen::Affine3d(vertex->estimate().matrix()));
    env->addChild(env->getRootNode(), framenode);
    env->setFrameNode(envire_pointcloud, framenode);
    if(use_mls)
        env->addInput(projection.get(), envire_pointcloud);

    vertices_to_add.insert(vertex);
    odometry_pose_last_vertex = odometry_pose;
    odometry_covariance_last_vertex = odometry_covariance;
    last_vertex = vertex;
    
    next_vertex_id++;
    return true;
}

bool ExtendedSparseOptimizer::removeVertex(int vertex_id)
{
    graph_slam::VertexSE3_GICP *vertex = dynamic_cast<graph_slam::VertexSE3_GICP*>(this->vertex(vertex_id));
    if(vertex && isHandledByOptimizer(vertex))
    {
        removePointcloudFromVertex(vertex_id);
        //TODO
        
    }
    return false;
}

bool ExtendedSparseOptimizer::removePointcloudFromVertex(int vertex_id)
{
    graph_slam::VertexSE3_GICP *vertex = dynamic_cast<graph_slam::VertexSE3_GICP*>(this->vertex(vertex_id));
    if(vertex && isHandledByOptimizer(vertex) && vertex->hasPointcloudAttached())
    {
        // remove pointcloud from vertex
        envire::EnvironmentItem::Ptr envire_item = vertex->getEnvirePointCloud();
        envire::Pointcloud* envire_pointcloud = dynamic_cast<envire::Pointcloud*>(envire_item.get());
        vertex->detachPointCloud();

        // remove pointcloud from envire
        if(use_mls)
            env->removeInput(projection.get(), envire_pointcloud);
        envire::FrameNode* fn = envire_pointcloud->getFrameNode();
        env->detachItem(fn, true);
        return true;
    }
    return false;
}

void ExtendedSparseOptimizer::setupMaxVertexGrid(unsigned max_vertices_per_cell, double grid_size_x, double grid_size_y, double cell_resolution)
{
    if(vertex_grid.use_count() == 0)
    {
        vertex_grid.reset(new VertexGrid(grid_size_x, grid_size_y, cell_resolution, max_vertices_per_cell));
        use_vertex_grid = true;
    }
    vertex_grid->setMaxVerticesPerCell(max_vertices_per_cell);
}

void ExtendedSparseOptimizer::removeVerticesFromGrid()
{
    if(!use_vertex_grid)
        return;
    std::vector<int> vertices;
    vertex_grid->removeVertices(vertices);
    for(unsigned i = 0; i < vertices.size(); i++)
    {
        if(!removePointcloudFromVertex(vertices[i]))
            std::cerr << "couldn't remove vertex with id " << vertices[i] << std::endl;
        else
            std::cerr << "removed vertex with id " << vertices[i] << std::endl;
    }
}

void ExtendedSparseOptimizer::findEdgeCandidates()
{
    g2o::SparseBlockMatrix<Eigen::MatrixXd> spinv;
    VertexContainer vc;
    for(g2o::OptimizableGraph::VertexContainer::const_iterator it = _activeVertices.begin(); it != _activeVertices.end(); it++)
    {
        graph_slam::VertexSE3_GICP *vertex = dynamic_cast<graph_slam::VertexSE3_GICP*>(*it);
        if(vertex && vertex->hasPointcloudAttached() && isHandledByOptimizer(vertex))
            vc.push_back(vertex);
    }
    cov_graph.computeMarginals(spinv, vc);

    for(g2o::OptimizableGraph::VertexContainer::const_iterator it = _activeVertices.begin(); it != _activeVertices.end(); it++)
    {
        graph_slam::VertexSE3_GICP *vertex = dynamic_cast<graph_slam::VertexSE3_GICP*>(*it);
        if(vertex && vertex->hasPointcloudAttached() && !vertex->getEdgeSearchState().has_run)
        {
            findEdgeCandidates(vertex->id(), spinv);
        }
        // TODO add a check for vertecies, if the pose has significantly changed
    }
}

void ExtendedSparseOptimizer::findEdgeCandidates(int vertex_id, const g2o::SparseBlockMatrix<Eigen::MatrixXd>& spinv)
{
    graph_slam::VertexSE3_GICP *source_vertex = dynamic_cast<graph_slam::VertexSE3_GICP*>(this->vertex(vertex_id));
    Matrix6d source_covariance;
    if(source_vertex && source_vertex->hasPointcloudAttached() && getVertexCovariance(source_covariance, source_vertex, spinv))
    {
        for(g2o::OptimizableGraph::VertexContainer::iterator it = _activeVertices.begin(); it != _activeVertices.end(); it++)
        {
            graph_slam::VertexSE3_GICP *target_vertex = dynamic_cast<graph_slam::VertexSE3_GICP*>(*it);
            if(target_vertex && target_vertex->hasPointcloudAttached() && (vertex_id < target_vertex->id()-1 || vertex_id > target_vertex->id()+1))
            {
                // check if vertecies have already an edge
                unsigned equal_edges = 0;
                for(g2o::HyperGraph::EdgeSet::const_iterator sv_edge = source_vertex->edges().begin(); sv_edge != source_vertex->edges().end(); sv_edge++)
                {
                    equal_edges += target_vertex->edges().count(*sv_edge);
                }
                
                // there should never be more than one edge between two vertecies
                assert(equal_edges <= 1);
                
                Matrix6d target_covariance;
                if(equal_edges == 0 && getVertexCovariance(target_covariance, target_vertex, spinv))
                {
                    // try to add a new edge
                    Eigen::Matrix3d position_covariance = source_covariance.topLeftCorner<3,3>() + target_covariance.topLeftCorner<3,3>();
                    
                    double mahalanobis_distance = computeMahalanobisDistance<double, 3>(source_vertex->estimate().translation(), 
                                                                            position_covariance, 
                                                                            target_vertex->estimate().translation());
                    double euclidean_distance = (target_vertex->estimate().translation() - source_vertex->estimate().translation()).norm();
                    double distance = mahalanobis_distance > euclidean_distance ? euclidean_distance : mahalanobis_distance;
                    
                    if(distance <= gicp_config.max_sensor_distance)
                    {
                        source_vertex->addEdgeCandidate(target_vertex->id(), distance);
                        target_vertex->addEdgeCandidate(source_vertex->id(), distance);
                        new_edges_added = true;
                    }
                }
            }
        }
        // save search pose
        source_vertex->setEdgeSearchState(true, source_vertex->estimate());
    }
}

void ExtendedSparseOptimizer::tryBestEdgeCandidates(unsigned count)
{
    if(!new_edges_added)
        return;

    unsigned edge_candidates_tested = 0;
    while(edge_candidates_tested < count)
    {
        // get vertex with highest missing edge error
        double vertex_error = 0.0;
        graph_slam::VertexSE3_GICP* source_vertex = NULL;
        for(g2o::OptimizableGraph::VertexContainer::iterator it = _activeVertices.begin(); it != _activeVertices.end(); it++)
        {
            graph_slam::VertexSE3_GICP* vertex = dynamic_cast<graph_slam::VertexSE3_GICP*>(*it);
            if(vertex && vertex->hasPointcloudAttached() && vertex->getMissingEdgesError() > vertex_error)
            {
                vertex_error = vertex->getMissingEdgesError();
                source_vertex = vertex;
            }
        }
        
        if(vertex_error == 0.0)
        {
            new_edges_added = false;
            return;
        }
        
        VertexSE3_GICP::EdgeCandidate candidate;
        int target_id;
        if(source_vertex && source_vertex->getBestEdgeCandidate(candidate, target_id))
        {
            graph_slam::VertexSE3_GICP *target_vertex = dynamic_cast<graph_slam::VertexSE3_GICP*>(this->vertex(target_id));

            if(!target_vertex || !target_vertex->hasPointcloudAttached())
            {
                source_vertex->removeEdgeCandidate(target_id);
                continue;
            }
            
            graph_slam::EdgeSE3_GICP* edge = new graph_slam::EdgeSE3_GICP();
            edge->setSourceVertex(source_vertex);
            edge->setTargetVertex(target_vertex);
            edge->setGICPConfiguration(gicp_config);

            if(!edge->setMeasurementFromGICP())
                throw std::runtime_error("compute transformation using gicp failed!");
            
            // add the new edge to the graph if the icp allignment was successful
            if(edge->hasValidGICPMeasurement())
            {
                if(!g2o::SparseOptimizer::addEdge(edge))
                {
                    std::cerr << "failed to add a new edge." << std::endl;
                    delete edge;
                }
                else if(_verbose)
                    std::cerr << "Added new edge between vertex " << source_vertex->id() << " and " << target_vertex->id() 
                                << ". Mahalanobis distance was " << candidate.mahalanobis_distance << ", edge error was " << candidate.error << std::endl;
                
                edges_to_add.insert(edge);
                source_vertex->removeEdgeCandidate(target_id);
                target_vertex->removeEdgeCandidate(source_vertex->id());
            }
            else
            {
                delete edge;
                source_vertex->updateEdgeCandidate(target_id, true);
                target_vertex->updateEdgeCandidate(source_vertex->id(), true);
            }
            edge_candidates_tested++;
        }
    }
}

int ExtendedSparseOptimizer::optimize(int iterations, bool online)
{
    if(activeVertices().size() == 0 && vertices_to_add.size() < 2)
    {
        // nothing to optimize
        return 0;
    }

    int err = -1;
    if(vertices_to_add.size() || edges_to_add.size())
    {
        // Update the cov graph, which provides the local covariances
        // This is a hack, since the covariances provided by this otimizer are in the space of the updates
        for(g2o::HyperGraph::VertexSet::const_iterator it = vertices_to_add.begin(); it != vertices_to_add.end(); it++)
        {
            g2o::VertexSE3* v = new g2o::VertexSE3();
            v->setId((*it)->id());
            v->setFixed(dynamic_cast<g2o::VertexSE3*>(*it)->fixed());
            cov_graph.addVertex(v);
        }
        for(g2o::HyperGraph::EdgeSet::const_iterator it = edges_to_add.begin(); it != edges_to_add.end(); it++)
        {
            g2o::EdgeSE3* e = new g2o::EdgeSE3();
            e->vertices()[0] = cov_graph.vertex((*it)->vertices()[0]->id());
            e->vertices()[1] = cov_graph.vertex((*it)->vertices()[1]->id());
            e->setMeasurement(Eigen::Isometry3d::Identity());
            e->setInformation(dynamic_cast<g2o::EdgeSE3*>(*it)->information());
            cov_graph.addEdge(e);
        }
        cov_graph.initializeOptimization();
        cov_graph.optimize(iterations);

        // update hessian matrix
        if(initialized)
        {
            if(!updateInitialization(vertices_to_add, edges_to_add))
                throw std::runtime_error("update optimization failed!");

            // do optimization
            err = g2o::SparseOptimizer::optimize(iterations, online);
        }
        else
        {
            if(!initializeOptimization())
                throw std::runtime_error("initialize optimization failed!");
            initialized = true;

            // do optimization
            err = g2o::SparseOptimizer::optimize(iterations, false);
        }

        // add new vertecies to grid
        if(use_vertex_grid)
        {
            for(g2o::HyperGraph::VertexSet::const_iterator it = vertices_to_add.begin(); it != vertices_to_add.end(); it++)
            {
                graph_slam::VertexSE3_GICP *vertex = dynamic_cast<graph_slam::VertexSE3_GICP*>(*it);
                if(vertex)
                    vertex_grid->addVertex(vertex->id(), vertex->estimate().translation());
            }
        }

        vertices_to_add.clear();
        edges_to_add.clear();
    }
    else
    {
        // do optimization
        err = g2o::SparseOptimizer::optimize(iterations, online);
    }
    map_update_necessary = true;

    return err;
}

void ExtendedSparseOptimizer::setMLSMapConfiguration(bool use_mls, double grid_size_x, double grid_size_y, double cell_resolution_x, double cell_resolution_y, double min_z, double max_z)
{
    if(use_mls && projection.get() == 0)
    {
        double grid_count_x = grid_size_x / cell_resolution_x;
        double grid_count_y = grid_size_y / cell_resolution_y;
        envire::MultiLevelSurfaceGrid* mls = new envire::MultiLevelSurfaceGrid(grid_count_x, grid_count_y, cell_resolution_x, cell_resolution_y, -0.5 * grid_size_x, -0.5 * grid_size_y);
        projection.reset(new envire::MLSProjection());
        projection->setAreaOfInterest(-0.5 * grid_size_x, 0.5 * grid_size_x, -0.5 * grid_size_y, 0.5 * grid_size_y, min_z, max_z);
        env->attachItem(mls);
        envire::FrameNode *fn = new envire::FrameNode();
        env->getRootNode()->addChild(fn);
        mls->setFrameNode(fn);
        env->addOutput(projection.get(), mls);
        this->use_mls = true;
    }
    else if(use_mls && projection.get() != 0)
    {
        this->use_mls = true;
    }
    else if(!use_mls && projection.get() != 0)
    {
        envire::MultiLevelSurfaceGrid* mls = env->getOutput<envire::MultiLevelSurfaceGrid*>(projection.get());
        mls->clear();
        this->use_mls = false;
    }
}

bool ExtendedSparseOptimizer::updateEnvire()
{
    // nothing to do
    if(!map_update_necessary)
        return true;

    // update framenodes
    unsigned err_counter = 0;
    g2o::SparseBlockMatrix<Eigen::MatrixXd> spinv;
    VertexContainer vc;
    for(VertexIDMap::const_iterator it = _vertices.begin(); it != _vertices.end(); it++)
    {
        graph_slam::VertexSE3_GICP *vertex = dynamic_cast<graph_slam::VertexSE3_GICP*>(it->second);
        if(!vertex->hasPointcloudAttached() || !isHandledByOptimizer(vertex))
            continue;
        vc.push_back(vertex);
    }
    if(!vc.empty())
        cov_graph.computeMarginals(spinv, vc);

    for(VertexIDMap::const_iterator it = _vertices.begin(); it != _vertices.end(); it++)
    {
        graph_slam::VertexSE3_GICP *vertex = dynamic_cast<graph_slam::VertexSE3_GICP*>(it->second);
        if(vertex)
        {
            if(!vertex->hasPointcloudAttached())
                continue;
            envire::CartesianMap* map = dynamic_cast<envire::CartesianMap*>(vertex->getEnvirePointCloud().get());
            if(map)
            {
                envire::FrameNode* framenode = map->getFrameNode();
                if(framenode)
                {
                    framenode->setTransform(getEnvireTransformWithUncertainty(vertex, &spinv));
                    continue;
                }
            }
        }
        err_counter++;
    }

    if(use_mls)
    {
        envire::MultiLevelSurfaceGrid* mls = env->getOutput<envire::MultiLevelSurfaceGrid*>(projection.get());
        mls->clear();
        projection->updateAll();
    }

    return !err_counter;
}

bool ExtendedSparseOptimizer::getVertexCovariance(Matrix6d& covariance, const g2o::OptimizableGraph::Vertex* vertex, const g2o::SparseBlockMatrix<Eigen::MatrixXd>& spinv)
{
    if(vertex && isHandledByOptimizer(vertex))
    {
        if(vertex->hessianIndex() >= (int)spinv.blockCols().size())
            return false;
        const Eigen::MatrixXd* vertex_cov = spinv.block(vertex->hessianIndex(), vertex->hessianIndex());
        if(!vertex_cov)
            return false;
        covariance = Matrix6d(*vertex_cov);
        return true;
    }
    return false;
}

bool ExtendedSparseOptimizer::getVertexCovariance(Matrix6d& covariance, const g2o::OptimizableGraph::Vertex* vertex)
{
    if(vertex && isHandledByOptimizer(vertex))
    {
        g2o::SparseBlockMatrix<Eigen::MatrixXd> spinv;
        cov_graph.computeMarginals(spinv, vertex);
        return getVertexCovariance(covariance, vertex, spinv);
    }
    return false;
}

envire::TransformWithUncertainty ExtendedSparseOptimizer::getEnvireTransformWithUncertainty(const g2o::OptimizableGraph::Vertex* vertex, const g2o::SparseBlockMatrix<Eigen::MatrixXd>* spinv)
{
    envire::TransformWithUncertainty transform = envire::TransformWithUncertainty::Identity();
    const graph_slam::VertexSE3_GICP *vertex_se3 = dynamic_cast<const graph_slam::VertexSE3_GICP*>(vertex);
    if(vertex_se3)
        transform.setTransform(Eigen::Affine3d(vertex_se3->estimate().matrix()));
    Matrix6d covariance;
    if(spinv)
    {
        if(getVertexCovariance(covariance, vertex, *spinv))
            transform.setCovariance(switchEnvireG2oCov(covariance));
    }
    else
    {
        if(getVertexCovariance(covariance, vertex))
            transform.setCovariance(switchEnvireG2oCov(covariance));
    }
    return transform;
}

bool ExtendedSparseOptimizer::adjustOdometryPose(const base::samples::RigidBodyState& odometry_pose, base::samples::RigidBodyState& adjusted_odometry_pose) const
{
    if(!last_vertex)
        return false;
    
    Eigen::Isometry3d adjusted_pose = last_vertex->estimate() * (odometry_pose_last_vertex.inverse() * Eigen::Isometry3d(odometry_pose.getTransform().matrix()));
    adjusted_odometry_pose.initUnknown();
    adjusted_odometry_pose.position = adjusted_pose.translation();
    adjusted_odometry_pose.orientation = Eigen::Quaterniond(adjusted_pose.linear());
    // TODO handle also covariance
    
    return true;
}

void ExtendedSparseOptimizer::dumpGraphViz(std::ostream& os)
{
    os << "graph G {" << std::endl;
    os << "label = \"Vertices: " << _activeVertices.size() << ", Edges: " << _activeEdges.size() << "\";" << std::endl;
    os << "overlap = scale;" << std::endl;
    for(g2o::OptimizableGraph::VertexContainer::iterator it = _activeVertices.begin(); it != _activeVertices.end(); it++)
    {
        graph_slam::VertexSE3_GICP *vertex = dynamic_cast<graph_slam::VertexSE3_GICP*>(*it);
        if(vertex)
        {
            const Eigen::Isometry3d& pose = vertex->estimate();
            os << "  v" << vertex->id() << " [pos=\"" << pose.translation().x() << "," << pose.translation().y() << "\"";
            if(!vertex->hasPointcloudAttached())
                os << ", style=dashed";
            os << "];" << std::endl;
        }
    }
    for(g2o::OptimizableGraph::EdgeContainer::iterator edge = _activeEdges.begin(); edge != _activeEdges.end(); edge++)
    {
        os << "  v" << (*edge)->vertices()[0]->id() << " -- " << "v" << (*edge)->vertices()[1]->id() << " ";
        os << "[";

        graph_slam::EdgeSE3_GICP* edge_icp = dynamic_cast<graph_slam::EdgeSE3_GICP*>(*edge);
        if(edge_icp)
        {
            if(edge_icp->hasValidGICPMeasurement())
                os << "label=" << 0.01 * std::floor(edge_icp->getICPFitnessScore() * 100) << ", fontsize=10, ";

            // set edge color
            if(!edge_icp->hasValidGICPMeasurement())
                os << "color=red";
            else if((*edge)->vertices()[0]->id()+1 != (*edge)->vertices()[1]->id())
                os << "color=blue";
        }

        os << "];" << std::endl;
    }
    os << "}" << std::endl;
}
    
}