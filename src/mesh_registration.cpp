#include "mesh_registration.h"

namespace newmeshreg {

Mesh_registration::Mesh_registration(){
    MESHES.resize(2, newresampler::Mesh());
    FEAT = std::make_shared<featurespace>();
}

void Mesh_registration::run_multiresolutions(int levels, double sigma, const std::string& parameters) {

    parse_reg_options(parameters);

    if(levels != 0) _resolutionlevels = levels;
    if(_verbose) std::cout << "Starting multiresolution with " << _resolutionlevels << " levels." << std::endl;

    for(int i = 0; i < _resolutionlevels; i++)
    {
        level = i + 1;
        if(_verbose) std::cout << "Initialising level " << level << std::endl;

        fix_parameters_for_level(i);
        initialize_level(i);
        evaluate();
        if(isrigid) rigidcf.reset();
    }

    transform(_outdir);
    saveSPH_reg(_outdir);
    save_transformed_data(sigma, _outdir);
}

void Mesh_registration::set_input(const newresampler::Mesh& M) {
    MESHES[0] = M;
    recentre(MESHES[0]);
    true_rescale(MESHES[0], RAD);
}

void Mesh_registration::set_input(const std::string &M) {
    MESHES[0].load(M);
    recentre(MESHES[0]);
    true_rescale(MESHES[0], RAD);
}

void Mesh_registration::set_inputs(const std::string& s) {
    std::vector<std::string> meshlist = read_ascii_list(s);
    newresampler::Mesh tmp;
    MESHES.clear();
    for (int i = 0; i < meshlist.size(); i++) {
        if (_verbose) std::cout << i << " " << meshlist[i] << std::endl;
        tmp.load(meshlist[i]);
        MESHES.push_back(tmp);
    }
}

void Mesh_registration::set_reference(const newresampler::Mesh &M) {
    MESHES[1] = M;
    recentre(MESHES[1]);
    true_rescale(MESHES[1], RAD);
}

void Mesh_registration::set_reference(const std::string& M) {
    MESHES[1].load(M);
    recentre(MESHES[1]);
    true_rescale(MESHES[1], RAD);
}

void Mesh_registration::set_template(const std::string &M) {
    TEMPLATE.load(M);
    recentre(TEMPLATE);
    true_rescale(TEMPLATE, RAD);
}

void Mesh_registration::set_anatomical(const std::string &M1, const std::string &M2) {
    _anat = true;
    in_anat.load(M1);
    ref_anat.load(M2);
}

void Mesh_registration::set_transformed(const std::string &M) {
    transformed_mesh.load(M);
    true_rescale(MESHES[1], RAD);
    _initialise = true;
}

void Mesh_registration::set_input_cfweighting(const std::string& E) {
    IN_CFWEIGHTING = std::make_shared<newresampler::Mesh>(MESHES[0]);
    IN_CFWEIGHTING->load(E, false, false);
    true_rescale(*IN_CFWEIGHTING, RAD);
    _incfw = true;
}

void Mesh_registration::set_reference_cfweighting(const std::string& E) {
    REF_CFWEIGHTING = std::make_shared<newresampler::Mesh>(MESHES[1]);
    REF_CFWEIGHTING->load(E, false, false);
    true_rescale(*REF_CFWEIGHTING, RAD);
    _refcfw = true;
}

void Mesh_registration::set_training(const std::string& s, bool concat) {
    DATAlist = read_ascii_list(s);
    _concattraining = concat;
    _usetraining = true;
}

void Mesh_registration::initialize_level(int current_lvl) {

    check();

    if (_usetraining)
        FEAT = std::make_shared<featurespace>(CMfile_in, DATAlist);
    else
        FEAT = std::make_shared<featurespace>(CMfile_in, CMfile_ref);

    FEAT->set_smoothing_parameters({_sigma_in[current_lvl], _sigma_ref[current_lvl]});
    FEAT->set_cutthreshold(_threshold); // will also generate exclusion masks at the same mesh resolution as datagrid
    FEAT->logtransform(_logtransform);// if true logtransforms AND normalises
    FEAT->varnorm(_varnorm);// variance normalises
    FEAT->intensitynormalize(_IN, _cut); // matches the intensities of the source to the target (will rescale all to the top feature of the target if scale is true)
    FEAT->resamplingmethod(_dataInterpolator);
    FEAT->is_sparse(_issparse);
    SPH_orig = FEAT->initialize(_genesis[current_lvl], MESHES, _exclude);  // downsamples and smooths data, creates and exclusion mask if exclude is true
    SPHin_CFWEIGHTING = downsample_cfweighting(_sigma_in[current_lvl], SPH_orig, IN_CFWEIGHTING, FEAT->get_input_excl());
    SPHref_CFWEIGHTING = downsample_cfweighting(_sigma_ref[current_lvl], SPH_orig, REF_CFWEIGHTING, FEAT->get_reference_excl());

    if(cost[current_lvl] == "RIGID" || cost[current_lvl] == "AFFINE")
    {
        rigidcf = std::make_shared<Rigid_cost_function>(SPH_orig, SPH_orig, FEAT);
        rigidcf->set_parameters(PARAMETERS);
        if(_simval[current_lvl] != 1) rigidcf->set_simmeasure(1);
        rigidcf->Initialize();
        isrigid = true;
    }

    if(cost[current_lvl] == "DISCRETE")
    {
        isrigid = false;
        if(_simval[current_lvl] == 3) std::cout << " warning NMI similarity measure does not take into account cost function weights or exclusion values" << std::endl;
        bool multivariate;
        if(FEAT->get_dim() == 1)
        {
            multivariate = false;
            if (_simval[current_lvl] == 4)
                throw MeshregException(
                      "MeshREG ERROR:: simval option 4 (alphaMI) is not suitable for univariate costfunctions");
        }
        else
            multivariate = true;

        PARAMETERS.insert(parameterPair("multivariate",multivariate));

        model = std::shared_ptr<SRegDiscreteModel>(new NonLinearSRegDiscreteModel(PARAMETERS));

        if (_usetraining) model->set_L1path(_L1path);
        if(_debug) model->set_debug();
        model->set_featurespace(FEAT, _concattraining);
        model->set_meshspace(SPH_orig, SPH_orig);
        newresampler::Mesh CONTROL = newresampler::make_mesh_from_icosa(_gridres[current_lvl]);
        newresampler::recentre(CONTROL);
        newresampler::true_rescale(CONTROL, RAD);

        if(_anat)
        {
            std::vector<std::vector<int>> ANAT_face_neighbourhood;
            std::vector<std::map<int,double>> ANAT_to_CP_baryweights;
            newresampler::Mesh aICO = resample_anatomy(CONTROL, ANAT_to_CP_baryweights, ANAT_face_neighbourhood, current_lvl);
            newresampler::Mesh ANAT_target = newresampler::surface_resample(ref_anat,MESHES[1],aICO);
            model->set_anatomical_meshspace(aICO, ANAT_target, aICO, ANAT_orig);
            model->set_anatomical_neighbourhood(ANAT_to_CP_baryweights, ANAT_face_neighbourhood);
        }
        else
        {
            if (_regmode == 5) throw MeshregException("STRAINS based regularisation requires anatomical meshes");
            else if (_regmode == 4) throw MeshregException("You have specified angular based penalisation of anatomical warp, for which you require anatomical meshes");
        }

        model->Initialize(CONTROL);
    }
}

newresampler::Mesh Mesh_registration::resample_anatomy(const newresampler::Mesh& control_grid,
                                                    std::vector<std::map<int,double>>& baryweights,
                                                    std::vector<std::vector<int>>& ANAT_to_CPgrid_neighbours,
                                                    int current_lvl) {

    if (in_anat.nvertices() != MESHES[0].nvertices() || ref_anat.nvertices() != MESHES[1].nvertices())
        throw MeshregException("MeshREG ERROR:: input/reference anatomical mesh resolution is inconsistent with input/reference spherical mesh resolution.");

    newresampler::Mesh ANAT_ico = control_grid;

    // save face neighbourhood relationships during retesselation
    std::vector<std::vector<int>> FACE_neighbours, FACE_neighbours_tmp;
    std::vector<int> tmp;

    if((_anatres[current_lvl] - _gridres[current_lvl]) > 0)
    {
        for(int i = 0; i < (_anatres[current_lvl] - _gridres[current_lvl]); i++)
        {
            newresampler::retessellate(ANAT_ico, FACE_neighbours_tmp);
            if(i > 0)
            {
                ANAT_to_CPgrid_neighbours.clear();
                for(unsigned int j=0;j<FACE_neighbours.size();j++)
                {
                    ANAT_to_CPgrid_neighbours.push_back(tmp);
                    for(unsigned int k=0;k<FACE_neighbours[j].size();k++)
                    {
                        ANAT_to_CPgrid_neighbours[j].insert(ANAT_to_CPgrid_neighbours[j].begin(),
                                FACE_neighbours_tmp[FACE_neighbours[j][k]].begin(),
                                FACE_neighbours_tmp[FACE_neighbours[j][k]].end());
                    }
                }
                FACE_neighbours=ANAT_to_CPgrid_neighbours;
            }
            else
            {
                FACE_neighbours=FACE_neighbours_tmp;
            }
            FACE_neighbours_tmp.clear();
        }

        if (ANAT_to_CPgrid_neighbours.empty())
            ANAT_to_CPgrid_neighbours = FACE_neighbours;
            //i.e. for one increase in resolution use result directly from restesselation
    }
    else
    {
        for (int i = 0; i < control_grid.ntriangles(); i++)
        {
            ANAT_to_CPgrid_neighbours.push_back(tmp);
            ANAT_to_CPgrid_neighbours[i].push_back(i);
        }
    }

    true_rescale(ANAT_ico,RAD);

    // now get barycentric weights
    baryweights.resize(ANAT_ico.nvertices(),std::map<int,double>());

    for(int i = 0; i < ANAT_to_CPgrid_neighbours.size(); i++)
    {
        int id0 = control_grid.get_triangle(i).get_vertex_no(0);
        int id1 = control_grid.get_triangle(i).get_vertex_no(1);
        int id2 = control_grid.get_triangle(i).get_vertex_no(2);
        newresampler::Point v0 = control_grid.get_triangle_vertex(i,0);
        newresampler::Point v1 = control_grid.get_triangle_vertex(i,1);
        newresampler::Point v2 = control_grid.get_triangle_vertex(i,2);

        for(const int& j : ANAT_to_CPgrid_neighbours[i])
        {
            for(int k = 0; k < 3; k++)
            {
                const newresampler::Point& ci = ANAT_ico.get_triangle_vertex(j,k);
                int id = ANAT_ico.get_triangle(j).get_vertex_no(k);
                baryweights[id] = calc_barycentric_weights(v0, v1, v2, ci, id0, id1, id2);
            }
        }
    }

    ANAT_orig = newresampler::surface_resample(in_anat,MESHES[0],ANAT_ico);

    return ANAT_ico;
}

NEWMAT::Matrix Mesh_registration::downsample_cfweighting(double sigma,
                                                         const newresampler::Mesh& SPH,
                                                         std::shared_ptr<newresampler::Mesh> CFWEIGHTING,
                                                         std::shared_ptr<newresampler::Mesh> EXCL) {
    NEWMAT::Matrix newdata;

    if(EXCL)
    {
        if (!CFWEIGHTING)
            CFWEIGHTING = std::make_shared<newresampler::Mesh>(*EXCL);

        newdata = newresampler::nearest_neighbour_interpolation(*CFWEIGHTING, SPH, EXCL).get_pvalues();
    }
    else if(CFWEIGHTING)
    {
        newdata = newresampler::nearest_neighbour_interpolation(*CFWEIGHTING, SPH, EXCL).get_pvalues();
    }
    else
    {
        newdata.ReSize(1, SPH.nvertices());
        newdata = 1;
    }

    return newdata;
}

//---MAIN FUNCTION---//
void Mesh_registration::evaluate() {
    //Initialise deformation mesh
    SPH_reg = project_CPgrid(SPH_orig,SPH_reg);
    // first project data grid through any predefined transformation or,
    // from transformation from previous resolution level.

    if(isrigid)
    {
        rigidcf->update_source(SPH_reg);
        SPH_reg = rigidcf->run();
    }
    else
    {
        run_discrete_opt(SPH_reg);
    }

    if(_verbose) std::cout << "Exit main algorithm." << std::endl;
}

void Mesh_registration::transform(const std::string& filename) {

    barycentric_mesh_interpolation(MESHES[0], SPH_orig, SPH_reg);
    MESHES[0].save(filename + "sphere.reg" + _surfformat);
}

void Mesh_registration::save_transformed_data(double sigma, const std::string& filename) {

    if(_verbose) std::cout << "Saving and transforming data." << std::endl;

    std::string path = filename + "transformed_and_reprojected" + _dataformat;

    std::shared_ptr<MISCMATHS::BFMatrix> DATA, DATAREF;
    std::shared_ptr<newresampler::Mesh> IN_EXCL, REF_EXCL;

    if(_usetraining)
    {
        set_data(CMfile_in,DATA,MESHES[0]);

        newresampler::Mesh tmp;

        if (sigma < 1e-3)
            tmp = newresampler::metric_resample(MESHES[0], MESHES[1], IN_EXCL);
        else
            tmp = newresampler::smooth_data(MESHES[0], MESHES[1], sigma, IN_EXCL);

        DATA = std::make_shared<MISCMATHS::FullBFMatrix>(tmp.get_pvalues());
    }
    else
    {
        // binarize costfunction weights for use as exclusion masks during resampling
        set_data(CMfile_in,DATA,MESHES[0]);
        set_data(CMfile_ref,DATAREF,MESHES[1]);

        if(_exclude)
        { // no longer necessary as EXCL masks aren't downsampled anymore? Could save from initialisation
            IN_EXCL= std::make_shared<newresampler::Mesh>(create_exclusion(MESHES[0],_threshold[0],_threshold[1]));
            REF_EXCL= std::make_shared<newresampler::Mesh>(create_exclusion(MESHES[1],_threshold[0],_threshold[1]));
        }
        if(_IN)
        { // INTENSITY NORMALIZE
            if(_verbose) std::cout << "Intensity normalise." << std::endl;
            multivariate_histogram_normalization(*DATA,*DATAREF,IN_EXCL,REF_EXCL);
        }

        MESHES[0].set_pvalues(DATA->AsMatrix());
        newresampler::Mesh tmp;

        if (sigma < 1e-3)
            tmp = newresampler::metric_resample(MESHES[0], MESHES[1], IN_EXCL);
        else
            tmp = newresampler::smooth_data(MESHES[0], MESHES[1], sigma, IN_EXCL);

        DATA = std::make_shared<MISCMATHS::FullBFMatrix>(tmp.get_pvalues());
    }

    newresampler::Mesh TRANSFORMED = MESHES[1];
    std::shared_ptr<MISCMATHS::FullBFMatrix > pin = std::dynamic_pointer_cast<MISCMATHS::FullBFMatrix>(DATA);

    if(pin)
    {
      TRANSFORMED.set_pvalues(DATA->AsMatrix());
      TRANSFORMED.save(path);
    }
    else
        DATA->Print(path);

    if(_anat)
    {
        newresampler::Mesh ANAT_TRANS = newresampler::project_mesh(MESHES[0], MESHES[1],ref_anat);
        ANAT_TRANS.save(_outdir + "anat.reg.surf");

        in_anat.estimate_normals();
        ANAT_TRANS.estimate_normals();
        if(_verbose) std::cout << "Calculate strains." << std::endl;
        newresampler::Mesh STRAINSmesh = calculate_strains(2, in_anat, ANAT_TRANS);
        STRAINSmesh.save(_outdir + "STRAINS.func");
    }
}

//---PROJECT TRANSFORMATION FROM PREVIOUS LEVEL TO UPSAMPLED SOURCE---//
newresampler::Mesh Mesh_registration::project_CPgrid(newresampler::Mesh SPH_in, newresampler::Mesh REG, int num) {
    // num indices which warp for group registration

    if(level == 1)
    {
        if(transformed_mesh.nvertices() > 0)
        { // project into alignment with transformed mesh
            if (transformed_mesh == MESHES[num]) std::cout << " WARNING:: transformed mesh has the same coordinates as the input mesh " << std::endl;
            else
            {
                barycentric_mesh_interpolation(SPH_in, MESHES[num], transformed_mesh);
                if (model) model->warp_CPgrid(MESHES[num], transformed_mesh);
                // for tri clique model control grid is continously deformed
            }
        }
    }
    else
    {   // following first round always start by projecting Control and data grids through warp at previous level
        // PROJECT CPgrid into alignment with warp from previous level
        newresampler::Mesh icotmp = newresampler::make_mesh_from_icosa(REG.get_resolution());
        true_rescale(icotmp,RAD);
        // project datagrid though warp defined for the high resolution meshes (the equivalent to if registration is run one level at a time )
        newresampler::Mesh inorig = MESHES[0], incurrent = MESHES[0];
        barycentric_mesh_interpolation(incurrent,icotmp,REG);
        barycentric_mesh_interpolation(SPH_in,inorig,incurrent);
        if(model) model->warp_CPgrid(inorig, incurrent);
        if(_debug) incurrent.save(_outdir + "sphere.regLR.Res" + std::to_string(level) + ".surf");
    }

    unfold(SPH_in);

    return SPH_in;
}

// iterates over discrete optimisations
void Mesh_registration::run_discrete_opt(newresampler::Mesh& source) {

    newresampler::Mesh controlgrid = model->get_CPgrid(),
                       targetmesh = model->get_TARGET(),
                       sourcetmp = source;
    int _itersforlevel = boost::get<int>(PARAMETERS.find("iters")->second);
    int numNodes = model->getNumNodes(), iter = 1;
    double energy = 0, newenergy = 0;

    while(iter <= _itersforlevel)
    {
        NEWMAT::Matrix CombinedWeight;
        // resample and combine the reference cost function weighting with the source if provided
        if(_incfw && _refcfw)
        {
            NEWMAT::Matrix ResampledRefWeight = SPHref_CFWEIGHTING;
            targetmesh.set_pvalues(ResampledRefWeight);
            ResampledRefWeight = newresampler::metric_resample(targetmesh, source).get_pvalues();

            CombinedWeight = combine_costfunction_weighting(SPHin_CFWEIGHTING, ResampledRefWeight);
        }
        else
        {
            CombinedWeight.resize(1, source.nvertices());
            CombinedWeight = 1;
        }

        // elementwise multiplication
        model->reset_meshspace(source); // source mesh is updated and control point grids are reset
        model->setupCostFunctionWeighting(CombinedWeight);

        model->setupCostFunction();

        int* Labels = model->getLabeling();
        if(_verbose) std::cout << "Run optimisation." << std::endl;

        if(_discreteOPT == "FastPD")
        {
            model->computeUnaryCosts();
            model->computePairwiseCosts();
            FPD::FastPD opt(model, 100);
            newenergy = opt.run();
            opt.getLabeling(Labels);
        }
        else
        {
#ifdef HAS_HOCR
            Reduction HOCR_mode;

            if(_discreteOPT=="HOCR")
                HOCR_mode = HOCR;
            else if (_discreteOPT == "ELC")
                HOCR_mode = ELC_HOCR;
            else if (_discreteOPT == "ELC_approx")
                HOCR_mode = ELC_APPROX;
            else
                throw MeshregException("discrete optimisation mode is not available");

            newenergy = Fusion::optimize(model, HOCR_mode, _verbose);
#endif
        }

        if(iter > 1 && ((iter - 1) % 2 == 0) && (energy - newenergy < 0.001))
        {
            if(_verbose)
                std::cout << iter << " level has converged. newenergy " << newenergy
                <<  " energy " << energy <<  " energy-newenergy " <<  energy-newenergy << std::endl;
            break;
        }
        else
        {
            if(_verbose)
                std::cout <<  "newenergy " << newenergy <<  " energy " << energy
                <<  " energy-newenergy " <<  energy-newenergy << std::endl;
        }

        //Get initial energy
        //get labelling choices from the FastPD optimiser
        model->applyLabeling();
        // apply these choices in order to deform the CP grid
        newresampler::Mesh transformed_controlgrid = model->get_CPgrid();
        // use the control point updates to warp the source mesh
        newresampler::barycentric_mesh_interpolation(source, controlgrid, transformed_controlgrid);
        controlgrid = transformed_controlgrid;
        // higher order frameowrk continuous deforms the CP grid whereas the original FW resets the grid each time
        unfold(transformed_controlgrid);
        model->reset_CPgrid(transformed_controlgrid); // source mesh is updated and control point grids are reset
        unfold(source);
        energy = newenergy;
        iter++;
    }
}

void Mesh_registration::parse_reg_options(const std::string &parameters)
{
    std::string title = "msm configuration parameters";
    std::string examples;
    Utilities::OptionParser options(title,examples);

    std::vector<std::string> costdefault;
    Utilities::Option<std::vector<std::string>> optimizer(std::string("--opt"),costdefault,
                                               std::string("optimisation method. Choice of: RIGID, DISCRETE (default)"),
                                     false,Utilities::requires_argument);

    std::vector<int> intdefault;
    Utilities::Option<std::vector<int>> simval(std::string("--simval"), intdefault,
                                    std::string("code for determining which similarty measure is used to assess cost during registration: options are 1) SSD; 2) pearsons correlation (default); 3) NMI;)"),
                               false, Utilities::requires_argument);

    Utilities::Option<std::vector<int>> iterations(std::string("--it"), intdefault,
                                        std::string("number of iterations at each resolution (default -–it=3,3,3)"),
                                    false, Utilities::requires_argument);

    std::vector<float> floatdefault;
    Utilities::Option<std::vector<float>> sigma_in(std::string("--sigma_in"),floatdefault,
                                        std::string("smoothing parameter for input image (default --sigma_in=2,2,2)"),
                                    false, Utilities::requires_argument);

    Utilities::Option<std::vector<float>> sigma_ref(std::string("--sigma_ref"),  floatdefault,
                                         std::string("Sigma parameter - smoothing parameter for reference image  (set equal to sigma_in by default)"),
                                     false, Utilities::requires_argument);

    Utilities::Option<std::vector<float>> lambda(std::string("--lambda"),  floatdefault,
                                      std::string("Lambda parameter - controls contribution of regulariser "),
                                 false, Utilities::requires_argument);

    Utilities::Option<std::vector<int>> datagrid(std::string("--datagrid"),intdefault,
                                      std::string("DATA grid resolution (default --datagrid=5,5,5). If parameter = 0 then the native mesh is used."),
                                 false, Utilities::requires_argument);

    Utilities::Option<std::vector<int>> cpgrid(std::string("--CPgrid"),intdefault,
                                    std::string("Control point grid resolution (default --CPgrid=2,3,4)"),
                               false, Utilities::requires_argument);

    Utilities::Option< std::vector<int>> sampgrid(std::string("--SGgrid"),intdefault,
                                  std::string("Sampling grid resolution (default = 2 levels higher than the control point grid)"),
                                  false, Utilities::requires_argument);
    Utilities::Option<std::vector<int>> anatgrid(std::string("--anatgrid"),intdefault,
                                  std::string("Anatomical grid resolution (default = 2 levels higher than the control point grid)"),
                                  false, Utilities::requires_argument);

    Utilities::Option< std::vector<int>> alpha_knn(std::string("--aKNN"),intdefault,
                                        std::string("Number of neighbours for estimation of kNN graph and alpha entropy measure (default --aKNN=5,5,5)"),
                                   false, Utilities::requires_argument);

    std::vector<float> cutthresholddefault(2,0); cutthresholddefault[1] = 0.0001;
    Utilities::Option< std::vector<float>> cutthreshold(std::string("--cutthr"),cutthresholddefault,
                                             std::string("Upper and lower thresholds for defining cut vertices (default --cutthr=0,0)"),
                                        false, Utilities::requires_argument);

    Utilities::Option<std::string> meshinterpolationmethod(std::string("--mInt"), "BARY",
                                                std::string("Method used for mesh interpolations, options: TPS or BARY (default)"),
                                           false,Utilities::requires_argument);

    Utilities::Option<std::string> datainterpolationmethod(std::string("--dInt"), "ADAP_BARY",
                                                std::string("Method used for data interpolations, options: GAUSSIAN or ADAP_BARY (default)"),
                                           false,Utilities::requires_argument);
#ifdef HAS_HOCR
    Utilities::Option<int> regulariseroption(std::string("--regoption"), 1,
                                  std::string("Choose option for regulariser form lambda*weight*pow(cost,rexp). Where cost can be PAIRWISE or TRI-CLIQUE based. Options are: 1) PAIRWISE - penalising diffences in rotations of neighbouring points (default); 2) TRI_CLIQUE Angle deviation penalty (for spheres); 3) TRI_CLIQUE: Strain-based (for spheres);  4) TRI_CLIQUE Angle deviation penalty (for anatomy); 5) TRI_CLIQUE: Strain-based (for anatomy)"),
                                  false, Utilities::requires_argument);

    Utilities::Option<std::string> doptimizer(std::string("--dopt"),"FastPD",
                                   std::string("discrete optimisation implementation. Choice of: FastPD (default), HOCR (will reduce to QBPO for pairwise), ELC, ELC_approx"),
                              false,Utilities::requires_argument,false);

    Utilities::Option<bool> tricliquelikeihood(std::string("--triclique"), false,
                                    std::string("estimate similarity for triangular patches (rather than circular)"),
                                    false, Utilities::no_argument);

    Utilities::Option<float> shear(std::string("--shearmod"), 0.4,
                        std::string("shear modulus (default 0.4); for use with --regoptions 3 "),
                        false,Utilities::requires_argument);

    Utilities::Option<float> bulk(std::string("--bulkmod"), 1.6,
                       std::string("bulk mod (default 1.6); for use with --regoptions 3 "),
                       false,Utilities::requires_argument);

    Utilities::Option<float> grouplambda(std::string("--glambda_pairs"), 1,
                              std::string("scaling for pairwise term in greoup alignment"),
                              false,Utilities::requires_argument,false);
    Utilities::Option<float> kexponent(std::string("--k_exponent"), 2,
                            std::string("exponent inside strain equation (default 2)"),
                            false, Utilities::requires_argument);
#endif
    Utilities::Option<float> expscaling(std::string("--scaleexp"), 1,
                             std::string("Scaling for weight exponent (default 1.0)"),
                             false,Utilities::requires_argument,false);

    Utilities::Option<float> regulariserexp(std::string("--regexp"), 2.0,
                                 std::string("Regulariser exponent 'rexp' (default 2.0)"),
                                 false,Utilities::requires_argument);

    Utilities::Option<bool> distweight(std::string("--weight"), false,
                            std::string("weight regulariser cost using areal distortion weighting"),
                            false, Utilities::no_argument);

    Utilities::Option<bool> anorm(std::string("--anorm"), false,
                       std::string("norm regulariser cost using mean angle (for HCP compatibility)"),
                       false, Utilities::no_argument);

    Utilities::Option<bool> rescale_labels(std::string("--rescaleL"), false,
                                std::string("rescale label grid rather than using barycentres"),
                                false, Utilities::no_argument);

    Utilities::Option<float> maxdist(std::string("--maxdist"), 4,
                          std::string("Set areal distortion threshold (default 4); for use with --regoptions 2 "),
                          false,Utilities::requires_argument,false);

    Utilities::Option<float> pottsenergy(std::string("--potts"),0.0,
                              std::string("Use potts model for mrf regulariser (strain only thus far). Supply threshold value"),
                              false, Utilities::requires_argument, false);

    Utilities::Option<float> controlptrange(std::string("--cprange"), 1.0,
                                 std::string("Range (as % control point spacing) of data samples (default 1) "),
                                 false,Utilities::requires_argument,false);

    Utilities::Option<bool> logtransform(std::string("--log"), false,
                              std::string("log transform and normalise the data"),
                              false, Utilities::no_argument);

    Utilities::Option<bool> intensitynormalize(std::string("--IN"), false,
                                    std::string("Normalize intensity ranges using histogram matching "),
                                    false, Utilities::no_argument);

    Utilities::Option<bool> intensitynormalizewcut(std::string("--INc"), false,
                                        std::string("Normalize intensity ranges using histogram matching excluding cut"),
                                        false, Utilities::no_argument);

    Utilities::Option<bool> variancenormalize(std::string("--VN"), false,
                                   std::string("Variance normalize data "),
                                   false, Utilities::no_argument);

    Utilities::Option<bool> scaleintensity(std::string("--scale"), false,
                                std::string("Scale intensity ranges of a features in multivariate data to be equal to that of the first (useful for multimodal contrasts)"),
                                false, Utilities::no_argument);

    Utilities::Option<bool> exclude(std::string("--excl"), false,
                         std::string("Ignore the cut when resampling the data"),
                         false, Utilities::no_argument);

    Utilities::Option<bool> quartet(std::string("-Q"), false,
                         std::string("Estimate quartet low rank cost for group reg"),
                         false, Utilities::no_argument,false);

    //---AFFINE OPTIONS---//
    Utilities::Option<float> affinestepsize(std::string("--stepsize"), 0.01,
                                 std::string("gradient stepping for affine optimisation (default 0.01)"),
                                 false, Utilities::requires_argument);

    Utilities::Option<float> gradsampling(std::string("--gradsampling"), 0.5,
                               std::string("Determines the finite distance spacing for the affine gradient calculation (default 0.5)"),
                               false,Utilities::requires_argument);

    Utilities::Option<int> threads(std::string("--numthreads"), 1,
                        std::string("number of threads for tbb (default 1)"),
                        false,Utilities::requires_argument);

    try {
        // must include all wanted options here (the order determines how
        // the help message is printed)
        options.add(optimizer);
        options.add(simval);
        options.add(iterations);
        options.add(sigma_in);
        options.add(sigma_ref);
        options.add(lambda);
        options.add(datagrid);
        options.add(cpgrid);
        options.add(sampgrid);
        options.add(anatgrid);
        options.add(alpha_knn);
        options.add(cutthreshold);
        options.add(meshinterpolationmethod);
        options.add(datainterpolationmethod);
#ifdef HAS_HOCR
        options.add(regulariseroption);
        options.add(doptimizer);
        options.add(tricliquelikeihood);
        options.add(shear);
        options.add(bulk);
        options.add(grouplambda);
        options.add(kexponent);
#endif
        options.add(expscaling);
        options.add(regulariserexp);
        options.add(distweight);
        options.add(anorm);
        options.add(rescale_labels);
        options.add(maxdist);
        options.add(pottsenergy);
        options.add(controlptrange);
        options.add(logtransform);
        options.add(intensitynormalize);
        options.add(intensitynormalizewcut);
        options.add(variancenormalize);
        options.add(scaleintensity);
        options.add(exclude);
        options.add(quartet);
        options.add(affinestepsize);
        options.add(gradsampling);
        options.add(threads);

        if(parameters=="usage")
        {
            options.usage();
            exit(2);
        }

        if (!parameters.empty())
            options.parse_config_file(parameters);
    }
    catch(Utilities::X_OptionError& e)
    {
        options.usage();
        std::cerr << "\n" << e.what() << "\n";
        exit(EXIT_FAILURE);
    }
    catch(std::exception &e)
    {
        std::cerr << "\n" << e.what() << "\n";
        exit(EXIT_FAILURE);
    }

    if(!options.check_compulsory_arguments())
    {
        options.usage();
        exit(2);
    }

    if(parameters.empty())
    {
        // if no config is supplied use sulc config (as of Sept 2014) as default
        cost.resize(3,"DISCRETE");
        cost.insert(cost.begin(),"RIGID");
        _resolutionlevels=cost.size();
        _lambda.resize(4,0); _lambda[1] = 0.1; _lambda[2] = 0.2; _lambda[3] = 0.3;
        _simval.resize(4,2); _simval[0] = 1;
        _sigma_in.resize(4,2); _sigma_in[2] = 3;_sigma_in[3] = 2;
        _sigma_ref.resize(4,2); _sigma_ref[2] = 1.5; _sigma_ref[3] = 1;
        _iters.resize(4,3); _iters[0] = 50;
        _alpha_kNN.resize(cost.size(),5);
        _gridres.resize(4,0); _gridres[1] = 2; _gridres[2] = 3; _gridres[3] = 4;
        _anatres.resize(4,0); _anatres[1] = 4; _anatres[2] = 5; _anatres[3] = 6;
        _genesis.resize(4,4); _genesis[2] = 5; _genesis[3] = 6;
        _sampres.resize(4,0); _sampres[1] = 4; _sampres[2] = 5; _sampres[3] = 6;
    }
    else
    {
        cost = optimizer.value();
        _lambda = lambda.value();

        // now check for assignments and else set defaults
        if (simval.set()) _simval = simval.value();
        else _simval.resize(cost.size(), 2);

        if (iterations.set()) _iters = iterations.value();
        else _iters.resize(cost.size(), 3);

        if (sigma_in.set()) _sigma_in = sigma_in.value();
        else _sigma_in.resize(cost.size(), 2);

        if (sigma_ref.set()) _sigma_ref = sigma_ref.value();
        else _sigma_ref = _sigma_in;

        if (datagrid.set()) _genesis = datagrid.value();
        else _genesis.resize(cost.size(), 5);

        if (cpgrid.set()) _gridres = cpgrid.value();
        else
        {
            _gridres.resize(cost.size(), 2);
            for (unsigned int i = 1; i < cost.size(); i++) _gridres[i] = _gridres[i - 1] + 1;
        }

        if (anatgrid.set()) _anatres = anatgrid.value();
        else
        {
            _anatres.resize(cost.size(), 2);
            for (unsigned int i = 0; i < cost.size(); i++) _anatres[i] = _gridres[i] + 2;
        }

        if (sampgrid.set()) _sampres = sampgrid.value();
        else
        {
            _sampres.resize(cost.size());
            for (unsigned int i = 0; i < cost.size(); i++) _sampres[i] = _gridres[i] + 2;
        }

        if (alpha_knn.set()) _alpha_kNN = alpha_knn.value();
        else _alpha_kNN.resize(cost.size(), 5);
    }

    _resolutionlevels = cost.size();
    _logtransform = logtransform.value();
    _scale = scaleintensity.value();
#ifdef HAS_HOCR
    if(grouplambda.set())
    {
        _set_group_lambda=true;
        _pairwiselambda=grouplambda.value();
    }
    _regmode=regulariseroption.value();
    _discreteOPT=doptimizer.value();
    _tricliquelikeihood=tricliquelikeihood.value();
    _shearmod=shear.value();
    _bulkmod=bulk.value();
    _k_exp=kexponent.value();
#else
    _regmode=1;
_discreteOPT="FastPD";
#endif
    if(intensitynormalizewcut.set())
    {
        _IN=intensitynormalizewcut.value();
        _cut=true;
    }
    else
    {
        _IN=intensitynormalize.value();
        _cut=false;
    }
    _varnorm=variancenormalize.value();
    _exclude=exclude.value();
    _quartet=quartet.value();
    _meshInterpolator=meshinterpolationmethod.value();
    _dataInterpolator=datainterpolationmethod.value();
    _weight=distweight.value();
    _regoption2norm=anorm.value();
    _potts=pottsenergy.value();
    _threshold=cutthreshold.value();
    _regscaling=expscaling.value();
    _regexp=regulariserexp.value();
    _maxdist=maxdist.value();
    _cprange=controlptrange.value();
    _affinestepsize=affinestepsize.value();
    _affinegradsampling=gradsampling.value();
    _numthreads=threads.value();
    _rescale_labels=rescale_labels.value();

    if(_verbose)
    {
        std::cout << "cost.size() " << cost.size() << " "; for(const auto& e : cost) std::cout << e << ' ';
        std::cout << " iters "; for(const auto& e : _iters) std::cout << e << ' ';
        std::cout << "lambda "; for(const auto& e : _lambda) std::cout << e << ' ';
        std::cout << " sigmain "; for(const auto& e : _sigma_in) std::cout << e << ' ';
        std::cout << " sigmaref "; for(const auto& e : _sigma_ref) std::cout << e << ' ';
        std::cout << " datagrid "; for(const auto& e : _genesis) std::cout << e << ' ';
        std::cout << " cp gridres "; for(const auto& e : _gridres) std::cout << e << ' ';
        std::cout << " sp grid "; for(const auto& e : _sampres) std::cout << e << ' ';
        std::cout << " alpha_knn "; for(const auto& e : _alpha_kNN) std::cout << e << ' ';
        std::cout << " in parse options " << " _simval "; for(const auto& e : _simval) std::cout << e << ' ';
             std::cout << " _scale " << _scale << " mesh interpolator " << _meshInterpolator <<
             " discrete implementation " << _discreteOPT << " regoption " <<  _regmode <<
             " potts " << _potts << std::endl;
    }

    if (_regmode > 1 && _discreteOPT == "FastPD")
        throw MeshregException("MeshREG ERROR:: you cannot run higher order clique regularisers with fastPD ");
    if ((int) _threshold.size() != 2)
        throw MeshregException("MeshREG ERROR:: the cut threshold does not contain a limit for upper and lower threshold (too few inputs)");
    if ((int) _simval.size() != _resolutionlevels)
        throw MeshregException("MeshREG ERROR:: config file parameter list lengths are inconsistent: --simval");
    if ((int) _iters.size() != _resolutionlevels)
        throw MeshregException("MeshREG ERROR:: config file  parameter list lengths are inconsistent:--it");
    if ((int) _sigma_in.size() != _resolutionlevels)
        throw MeshregException("MeshREG ERROR:: config file  parameter list lengths are inconsistent: --sigma_in");
    if ((int) _sigma_ref.size() != _resolutionlevels)
        throw MeshregException("MeshREG ERROR:: config file  parameter list lengths are inconsistent:--sigma_ref");
    if ((int) cost.size() != _resolutionlevels)
        throw MeshregException("MeshREG ERROR:: config file parameter list lengths are inconsistent:--opt");
    if ((int) _lambda.size() != _resolutionlevels)
        throw MeshregException("MeshREG ERROR:: config file  parameter list lengths are inconsistent:--lambda");
    if ((int) _genesis.size() != _resolutionlevels)
        throw MeshregException("MeshREG ERROR:: config file  parameter list lengths are inconsistent:--datagrid");
    if ((int) _gridres.size() != _resolutionlevels)
        throw MeshregException("MeshREG ERROR:: config file parameter list lengths are inconsistent:--CPgrid");
    if ((int) _sampres.size() != _resolutionlevels)
        throw MeshregException("MeshREG ERROR:: config file parameter list lengths are inconsistent:--SGres");
    if ((int) _alpha_kNN.size() != _resolutionlevels)
        throw MeshregException("MeshREG ERROR:: config file parameter list lengths are inconsistent:--aKNN");
}

void Mesh_registration::fix_parameters_for_level(int i) {

    PARAMETERS.clear();

    PARAMETERS.insert(parameterPair("lambda", _lambda[i]));
    PARAMETERS.insert(parameterPair("lambda_pairs", _pairwiselambda));
    PARAMETERS.insert(parameterPair("set_lambda_pairs", _set_group_lambda));
    PARAMETERS.insert(parameterPair("iters", _iters[i]));
    PARAMETERS.insert(parameterPair("simmeasure", _simval[i]));
    PARAMETERS.insert(parameterPair("sigma_in", _sigma_in[i]));
    PARAMETERS.insert(parameterPair("CPres", _gridres[i]));
    PARAMETERS.insert(parameterPair("SGres", _sampres[i]));
    PARAMETERS.insert(parameterPair("anatres", _anatres[i]));
    PARAMETERS.insert(parameterPair("quartet", _quartet));
    PARAMETERS.insert(parameterPair("regularisermode", _regmode));
    PARAMETERS.insert(parameterPair("TriLikelihood", _tricliquelikeihood));
    PARAMETERS.insert(parameterPair("rescalelabels", _rescale_labels));
    PARAMETERS.insert(parameterPair("maxdistortion", _maxdist));
    PARAMETERS.insert(parameterPair("shearmodulus", _shearmod));
    PARAMETERS.insert(parameterPair("bulkmodulus", _bulkmod));
    PARAMETERS.insert(parameterPair("pottsthreshold", _potts));
    PARAMETERS.insert(parameterPair("range", _cprange));
    PARAMETERS.insert(parameterPair("exponent", _regexp));
    PARAMETERS.insert(parameterPair("weight", _weight));
    PARAMETERS.insert(parameterPair("anorm", _regoption2norm));
    PARAMETERS.insert(parameterPair("scaling", _regscaling));
    PARAMETERS.insert(parameterPair("kNN", _alpha_kNN[i]));
    PARAMETERS.insert(parameterPair("verbosity", _verbose));
    PARAMETERS.insert(parameterPair("outdir", _outdir));
    PARAMETERS.insert(parameterPair("stepsize", _affinestepsize));
    PARAMETERS.insert(parameterPair("gradsampling", _affinegradsampling));
    PARAMETERS.insert(parameterPair("numthreads", _numthreads));
    PARAMETERS.insert(parameterPair("kexponent", _k_exp));
}

void Mesh_registration::check() {

    if (((MESHES[0].get_coord(0)).norm() - RAD) > 1e-5)
        throw MeshregException("Reg_config ERROR:: input mesh radius has not been normalised to RAD=100");

    if (IN_CFWEIGHTING && (IN_CFWEIGHTING->get_coord(0).norm() - RAD) > 1e-5)
        throw MeshregException("Reg_config ERROR:: input exclusion mesh radius has not been normalised to RAD=100");

    if (REF_CFWEIGHTING && (REF_CFWEIGHTING->get_coord(0).norm() - RAD) > 1e-5)
        throw MeshregException(
                "Reg_config ERROR::reference exclusion mesh radius has not been normalised to RAD=100");
}

void Mesh_registration::set_output_format(const std::string& type) {

    if (type == "GIFTI")
    {
        _surfformat = ".surf.gii";
        _dataformat = ".func.gii";
    }
    else if (type == "ASCII" || type == "ASCII_MAT")
    {
        _surfformat = ".asc";
        if (type == "ASCII")
            _dataformat = ".dpv";
        else
            _dataformat = ".txt"; // for multivariate
    } else
    {
        _surfformat = ".vtk";
        _dataformat = ".txt";
    }
}

// each Matrix can have a different number of rows, for example if the user supplies a
// costfuncion weighting mask for multivariate features only on the reference,
// and also sets exclusion weighting then the reference weighting will be multivariate
// and the source weighting will be univariate - this combines into one mask on the
// source grid (therefore resampling much be reimplemented at every registration step)
NEWMAT::Matrix Mesh_registration::combine_costfunction_weighting(const NEWMAT::Matrix& sourceweight,
                                                          const NEWMAT::Matrix& resampledtargetweight) {

    NEWMAT::Matrix NEW;
    int nrows;
    if (sourceweight.Nrows() >= resampledtargetweight.Nrows())
    {
        NEW = sourceweight;
        nrows = resampledtargetweight.Nrows();
    }
    else
    {
        NEW = resampledtargetweight;
        nrows = sourceweight.Nrows();
    }

    for (int i = 1; i <= NEW.Ncols(); i++)
        for (int j = 1; j <= nrows; j++)
            NEW(j, i) = (sourceweight(j, i) + resampledtargetweight(j, i)) / 2.0;

    return NEW;
}

std::vector<std::string> Mesh_registration::read_ascii_list(const std::string& filename) {

    std::vector<std::string> list;

    std::ifstream fs(filename);
    std::string tmp;

    if(fs)
    {
        fs >> tmp;
        do
        {
            list.push_back(tmp);
            fs >> tmp;
        }
        while(!fs.eof());
    }
    else
        throw MeshregException("Error reading ascii file");

    return list;
}

} //namespace newmeshreg
