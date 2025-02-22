#include "DiscreteModel.h"

using namespace std;

namespace newmeshreg {

void SRegDiscreteModel::set_parameters(myparam& PAR) {
    myparam::iterator it;
    it=PAR.find("CPres"); m_CPres = boost::get<int>(it->second);
    it=PAR.find("SGres"); m_SGres = boost::get<int>(it->second);
    it=PAR.find("simmeasure"); m_simmeasure = boost::get<int>(it->second);
    it=PAR.find("regularisermode"); m_regoption = boost::get<int>(it->second);
    it=PAR.find("multivariate"); m_multivariate = boost::get<bool>(it->second);
    it=PAR.find("verbosity"); m_verbosity = boost::get<bool>(it->second);
    it=PAR.find("outdir"); m_outdir = boost::get<string>(it->second);
    it=PAR.find("TriLikelihood"); m_triclique = boost::get<bool>(it->second);
    it=PAR.find("rescalelabels"); m_rescalelabels = boost::get<bool>(it->second);
    it=PAR.find("quartet"); _estquartet = boost::get<bool>(it->second);
    if(m_regoption == 1) _pairwise = true;
}

void SRegDiscreteModel::Initialize(const newresampler::Mesh& CONTROLGRID) {

    double MVDmax = 0.0;
    MVD = 0.0;
    int tot = 0;
    m_CPgrid = CONTROLGRID;

    //---SET LOW RES DEFORMATION GRID & INITIALISE ASSOCIATED MRF PARAMS---//
    m_num_nodes = m_CPgrid.nvertices();
    m_num_pairs = 0;
    initLabeling();

    //---CALCULATE (MEAN) VERTEX SEPARATIONS FOR EACH VERTEX
    NEWMAT::ColumnVector vMAXmvd;
    vMAXmvd.ReSize(m_CPgrid.nvertices());
    vMAXmvd = 0;

    for (int k = 0; k < m_CPgrid.nvertices(); k++)
    {
        newresampler::Point CP = m_CPgrid.get_coord(k);
        for (auto it = m_CPgrid.nbegin(k); it != m_CPgrid.nend(k); it++)
        {
            double dist = 2 * RAD * asin((CP-m_CPgrid.get_coord(*it)).norm() / (2 * RAD));
            MVD += (CP-m_CPgrid.get_coord(*it)).norm();
            tot++;
            if(dist > vMAXmvd(k+1)) vMAXmvd(k+1) = dist;
            if(vMAXmvd(k+1) > MVDmax) MVDmax = vMAXmvd(k+1);
        }
    }
    MVD /= tot;

    m_maxs_dist = 0.5 * m_CPgrid.calculate_MaxVD();

    //---INITIALIZE COSTFCT---//
    costfct->set_meshes(m_TARGET, m_SOURCE, m_CPgrid);
    vector<vector<double>> orig_angles = m_CPgrid.get_face_angles();
    costfct->set_initial_angles(orig_angles);
    costfct->set_spacings(vMAXmvd, MVDmax);

    m_iter = 1;
}

void SRegDiscreteModel::initialize_cost_function(bool MV, int sim, myparam& P) {
  costfct = std::shared_ptr<SRegDiscreteCostFunction>(new UnivariateNonLinearSRegDiscreteCostFunction());
  costfct->set_parameters(P);
}

void SRegDiscreteModel::Initialize_sampling_grid() {
    //---LABELS USING HIGHER RES GRID---//
    m_samplinggrid = newresampler::make_mesh_from_icosa(m_SGres);
    true_rescale(m_samplinggrid,RAD);
    // find the first centroid with 6 neighbours
    for (int i = 0; i < m_samplinggrid.nvertices(); i++)
        if (m_samplinggrid.get_total_neighbours(i) == 6)
        {
            m_centroid = i;
            break;
        }
    label_sampling_grid(m_centroid,m_maxs_dist,m_samplinggrid);
}

void SRegDiscreteModel::label_sampling_grid(int centroid, double dist, newresampler::Mesh& Grid) {

    m_samples.clear();
    m_barycentres.clear();
    vector<int> getneighbours, newneighbours;
    int label = 1;
    NEWMAT::ColumnVector found(Grid.nvertices()), found_tr(Grid.ntriangles());
    found = 0; found_tr = 0;

    centre = Grid.get_coord(centroid);
    getneighbours.push_back(centroid);
    m_samples.push_back(centre);
    m_barycentres.push_back(centre);

    // searches for neighbours of the cnetroid that are within the max sampling distance
    while(!getneighbours.empty())
    {
        for(int& getneighbour : getneighbours)
        {
            for(auto j = Grid.nbegin(getneighbour); j != Grid.nend(getneighbour); j++)
            {
                newresampler::Point sample = Grid.get_coord(*j);
                if((sample-centre).norm() <= dist && (found(*j+1)==0) && *j != centroid)
                {
                    m_samples.push_back(Grid.get_coord(*j));  // pt-centroid equals deformation vector
                    newneighbours.push_back(*j);
                    found(*j+1) = 1;
                }
            }

            for(auto j = Grid.tIDbegin(getneighbour); j != Grid.tIDend(getneighbour); j++)
            {
                newresampler::Point v1 = Grid.get_triangle_vertex(*j,0),
                                    v2 = Grid.get_triangle_vertex(*j,1),
                                    v3 = Grid.get_triangle_vertex(*j,2);
                newresampler::Point bary((v1.X + v2.X + v3.X) / 3,
                                         (v1.Y + v2.Y + v3.Y) / 3,
                                         (v1.Z + v2.Z + v3.Z) / 3);
                bary.normalize();
                bary = bary * RAD;

                if((bary - centre).norm() <= dist && (bary - centre).norm() > 0 && found_tr(*j+1) == 0)
                {
                    for (auto& m_barycentre: m_barycentres)
                        if (abs(1 - (((bary - centre) | (m_barycentre - centre)) /
                                     ((bary - centre).norm() * (m_barycentre - centre).norm()))) < 1e-2)
                            found_tr(*j + 1) = 1;

                    if(!found_tr(*j+1))
                    {
                        m_barycentres.push_back(bary);
                        Grid.set_pvalue(Grid.get_triangle(*j).get_vertex_no(0),label);
                        label++;
                    }
                    found_tr(*j+1) = 1;
                }
            }
        }
        getneighbours = newneighbours;
        newneighbours.clear();
    }
}

vector<newresampler::Point> SRegDiscreteModel::rescale_sampling_grid() {

    vector<newresampler::Point> newlabels(m_samples.size());

    if(m_verbosity) cout << " resample labels " << m_scale << " length scale " << (centre-m_samples[1]).norm() << endl;

    if(m_scale >= 0.25)
    {
        #pragma omp parallel for
        for (int i = 0; i < m_samples.size(); i++)
        {
            newresampler::Point newsample = centre + (centre - m_samples[i]) * m_scale;
            newsample.normalize();
            newlabels[i] = newsample * 100;
        }
    }
    else
    {
        m_scale = 1;
        newlabels = m_samples;
    }
    m_scale *= 0.8;
    return newlabels;
}

void NonLinearSRegDiscreteModel::estimate_pairs() {

    int pair = 0;

    for (int i = 0; i < m_CPgrid.nvertices(); i++) // estimate the total number of edge pairs
        m_num_pairs += m_CPgrid.get_total_neighbours(i);

    m_num_pairs /= 2;
    pairs = new int[m_num_pairs * 2];

    for (int i = 0; i < m_CPgrid.nvertices(); i++)
        for (auto j = m_CPgrid.nbegin(i); j != m_CPgrid.nend(i); j++)
            if (*j > i)
            {
                int node_ids[2] = {i, *j};
                std::sort(std::begin(node_ids), std::end(node_ids));
                pairs[2 * pair] = node_ids[0];
                pairs[2 * pair + 1] = node_ids[1];
                pair++;
            }
}

void NonLinearSRegDiscreteModel::estimate_triplets() {

    m_num_triplets = m_CPgrid.ntriangles();
    triplets = new int[m_num_triplets * 3];

    #pragma omp parallel for
    for(int i = 0; i < m_CPgrid.ntriangles(); i++)
    {
        int node_ids[3] = {m_CPgrid.get_triangle(i).get_vertex_no(0),
                           m_CPgrid.get_triangle(i).get_vertex_no(1),
                           m_CPgrid.get_triangle(i).get_vertex_no(2) };
        std::sort(std::begin(node_ids), std::end(node_ids));
        triplets[3 * i] = node_ids[0];
        triplets[3 * i + 1] = node_ids[1];
        triplets[3 * i + 2] = node_ids[2];
    }
}

void NonLinearSRegDiscreteModel::Initialize(const newresampler::Mesh& CONTROLGRID) {

    SRegDiscreteModel::Initialize(CONTROLGRID);
    m_scale = 1;
    if (_pairwise)
        estimate_pairs();
    else
        estimate_triplets();

    //---INITIALIAZE LABEL GRID---//
    Initialize_sampling_grid();
    get_rotations(m_ROT);  // enables rotation of sampling grid onto every CP

    //---INITIALIZE NEIGHBOURHOODS---//
    m_inputtree = std::make_shared<newresampler::Octree>(m_TARGET);
}

void NonLinearSRegDiscreteModel::get_rotations(vector<NEWMAT::Matrix>& ROT) {

    // rotates sampling grid to each control point
    ROT.clear();
    ROT.resize(m_CPgrid.nvertices());
    newresampler::Point ci = m_samplinggrid.get_coord(m_centroid);

    #pragma omp parallel for
    for (int k = 0; k < m_CPgrid.nvertices(); k++)
        ROT[k] = estimate_rotation_matrix(ci, m_CPgrid.get_coord(k));
}

void NonLinearSRegDiscreteModel::setupCostFunction() {

    resetLabeling(); // initialise label array to zero
    //---use geodesic distances---//
    costfct->reset_CPgrid(m_CPgrid);

    if(m_iter == 1)
    {
        costfct->initialize_regulariser();
        costfct->set_octrees(m_inputtree);
    }

    costfct->reset_anatomical(m_outdir, m_iter);
    get_rotations(m_ROT);
    // instead of recalulating the source->CP neighbours, these are now constant
    // (as source is moving with CPgrid) we we just need to recalculate
    // the rotations of the label grid to the cp vertices

    if(m_debug)
    {
        m_SOURCE.save(m_outdir + "SOURCE-" + std::to_string(m_iter) + ".surf");
        m_SOURCE.save(m_outdir + "SOURCE-" + std::to_string(m_iter) + ".func");
        if (m_iter == 1) m_TARGET.save(m_outdir + "TARGET.surf");
        m_CPgrid.save(m_outdir + "CPgrid-" + std::to_string(m_iter) + ".surf");
    }

    //---set samples (labels vary from iter to iter)---//
    if (m_rescalelabels)
        m_labels = rescale_sampling_grid();
    else if (m_iter % 2 == 0)
        m_labels = m_samples;
    else
        m_labels = m_barycentres;

    m_num_labels = m_labels.size();

    costfct->set_labels(m_labels,m_ROT);
    if(m_verbosity) cout << " initialize cost function " << m_iter <<  endl;

    costfct->initialize(m_num_nodes,m_num_labels,m_num_pairs,m_num_triplets,0);
    costfct->get_source_data();

    if(_pairwise) costfct->setPairs(pairs);
    else costfct->setTriplets(triplets);
    m_iter++;
}

void NonLinearSRegDiscreteModel::applyLabeling(int* dlabels) {

    // rotate sampling points to overlap with control point transform grid point to new position given by label
    if (dlabels)
    {
        #pragma omp parallel for
        for (int i = 0; i < m_CPgrid.nvertices(); i++)
            m_CPgrid.set_coord(i, m_ROT[i] * m_labels[dlabels[i]]);
    }
}

} //namespace newmeshreg
