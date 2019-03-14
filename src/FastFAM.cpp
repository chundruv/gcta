/*
   GCTA: a tool for Genome-wide Complex Trait Analysis

   FastFAM regression

   Depends on the class of genotype

   Developed by Zhili Zheng<zhilizheng@outlook.com>

   This file is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   A copy of the GNU General Public License is attached along with this program.
   If not, see <http://www.gnu.org/licenses/>.
*/
#include "FastFAM.h"
#include "StatLib.h"
#include <cmath>
#include <algorithm>
#include <Eigen/SparseCholesky>
#include <Eigen/PardisoSupport>
#include <Eigen/IterativeLinearSolvers>
#include <sstream>
#include <iterator>
#include "utils.hpp"
#include "Logger.h"
#include "ThreadPool.h"
#include "omp.h"
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <iomanip>
#include "Covar.h"

#include <iostream>

struct InvItem{
    int32_t row;
    int32_t col;
    double val;
};


using std::to_string;
using Eigen::Matrix;

map<string, string> FastFAM::options;
map<string, double> FastFAM::options_d;
vector<string> FastFAM::processFunctions;

FastFAM::FastFAM(Geno *geno){
    //Eigen::setNbThreads(THREADS.getThreadCount() + 1);
    this->geno = geno;
    num_indi = geno->pheno->count_keep();

    double VG;
    double VR;
    bool flag_est_GE = true;
    if(options.find("G") != options.end()){
        VG = std::stod(options["G"]);
        VR = std::stod(options["E"]);
        flag_est_GE = false;
    }

    vector<string> ids;
    geno->pheno->get_pheno(ids, phenos);
    if(ids.size() != num_indi){
        LOGGER.e(0, "Phenotype is not equal, this shall be a flag bug");
    }

    // read covar
    vector<uint32_t> remain_index, remain_index_covar;
    bool has_covar = false;
    Covar covar;
    if(covar.getCommonSampleIndex(ids, remain_index, remain_index_covar)){
        has_covar = true;
        LOGGER.i(0, to_string(remain_index.size()) + " overlapped individuals with non-missing data to be included from the covariate file(s).");
    }else{
        remain_index.resize(ids.size());
        std::iota(remain_index.begin(), remain_index.end(), 0);
    }

    vector<string> remain_ids(remain_index.size());
    std::transform(remain_index.begin(), remain_index.end(), remain_ids.begin(), [&ids](size_t pos){return ids[pos];});

    // read fam
    string ffam_file = "";
    fam_flag = true;
    if(options.find("grmsparse_file") != options.end()){
        ffam_file = options["grmsparse_file"];
    }else{
        fam_flag = false;
    }

    // index in ids when merge to spare fam
    vector<uint32_t> remain_index_fam;
    SpMat fam;

    if(fam_flag){
        readFAM(ffam_file, fam, remain_ids, remain_index_fam);
    }else{
        remain_index_fam.resize(remain_ids.size());
        std::iota(remain_index_fam.begin(), remain_index_fam.end(), 0);
    }

    int n_remain_index_fam = remain_index_fam.size();

    //reorder phenotype, covar
    vector<double> remain_phenos(n_remain_index_fam);
    vector<string> remain_ids_fam(n_remain_index_fam);
    vector<uint32_t> total_remain_index(n_remain_index_fam);
    for(int i = 0; i != n_remain_index_fam; i++){
        uint32_t temp_index = remain_index[remain_index_fam[i]];
        remain_phenos[i] = phenos[temp_index];
        remain_ids_fam[i] = ids[temp_index];
        total_remain_index[i] = temp_index;
    }
    //fix pheno keep
    geno->pheno->filter_keep_index(total_remain_index);
    geno->init_keep();
    num_indi = geno->pheno->count_keep();
    LOGGER.i(0, "After matching all the files, " + to_string(remain_phenos.size()) + " individuals to be included in the analysis.");

    vector<double> remain_covar;
    vector<uint32_t> remain_inds_index;
    if(has_covar){
        covar.getCovarX(remain_ids_fam, remain_covar, remain_inds_index);
        remain_covar.resize(remain_covar.size() + n_remain_index_fam);
        std::fill(remain_covar.end() - n_remain_index_fam, remain_covar.end(), 1.0);
    }

    // standerdize the phenotype, and condition the covar
    phenoVec = Map<VectorXd> (remain_phenos.data(), remain_phenos.size());
    // condition the covar
    if(has_covar){
        MatrixXd concovar = Map<Matrix<double, Dynamic, Dynamic, Eigen::ColMajor>>(remain_covar.data(), remain_phenos.size(), 
                remain_covar.size() / remain_phenos.size());

        /*
        std::ofstream covar_w(options["out"] + "_aln_covar.txt"), pheno_w(options["out"] + "_aln_phen.txt"), pheno_w2(options["out"] + "_adj_phen.txt");
        pheno_w << phenoVec << std::endl;
        */

        conditionCovarReg(phenoVec, concovar);
        if(options.find("save_pheno") != options.end()){
            std::ofstream pheno_w((options["out"] + ".cphen").c_str());
            if(!pheno_w) LOGGER.e(0, "failed to write " + options["out"]+".cphen");
            for(int k = 0; k < remain_ids_fam.size(); k++){
                pheno_w << remain_ids_fam[k] << "\t" << phenoVec[k] << std::endl;
            }
            pheno_w.close();
        }

        /*
        pheno_w2 << phenoVec << std::endl;
        covar_w.close();
        pheno_w.close();
        pheno_w2.close();
        */
    }

    // Center
    double phenoVec_mean = phenoVec.mean();
    phenoVec -= VectorXd::Ones(phenoVec.size()) * phenoVec_mean;
    //LOGGER << "DEBUG: samples size: " << geno->pheno->count_keep() << std::endl;

    if(fam_flag){
        //LOGGER << "DEBUG: pheno vector size: " << phenoVec.size() << std::endl;
        double Vpheno = phenoVec.array().square().sum() / (phenoVec.size() - 1);
        //phenoVec /= pheno_sd;

        //LOGGER.i(0, "DEBUG: conditioned Pheno (first 5)");

        //for(int i = 0; i < 5; i++){
            //LOGGER.i(0, to_string(phenoVec[i]));
        //}

        if(options.find("inv_file") == options.end()){
            vector<double> Aij;
            vector<double> Zij;

            if(flag_est_GE){
                LOGGER.i(0, "Estimating the genetic variance (Vg) by HE regression...");
                if(options["rel_only"] == "yes"){
                    LOGGER.i(0, "Use related pairs only.");
                    for(int k = 0; k < fam.outerSize(); ++k){
                        for(SpMat::InnerIterator it(fam, k); it; ++it){
                            if(it.row() < it.col()){
                                Aij.push_back(it.value());
                                Zij.push_back(phenoVec[it.row()] * phenoVec[it.col()]);
                            }
                        }
                    }

                    VG = HEreg(Zij, Aij, fam_flag);
                }else{
                    VG = HEreg(fam, phenoVec, fam_flag);
                }
                LOGGER.i(2, "Vp = " + to_string(Vpheno));
                if(fam_flag){
                    VR = Vpheno - VG;
                    LOGGER.i(2, "Ve = " + to_string(VR));
                    LOGGER.i(2, "Heritablity = " + to_string(VG/Vpheno));
                }
            }
            if(!fam_flag){
                LOGGER.w(0, "The estimate of Vg is not statistically significant. "
                        "This is likely because the number of relatives is not large enough. "
                        "\nPerforming simple regression via removing --grm-sparse instead...");
 
                fam.resize(0, 0);
                V_inverse.resize(0, 0);
                if(options.find("save_inv") != options.end()){
                    std::ofstream inv_id((options["out"]+".grm.id").c_str());
                    if(!inv_id) LOGGER.e(0, "failed to write " + options["out"]+".grm.id");
                    inv_id << "--fastGWA" << std::endl;
                    inv_id.close();
                }
                return;
            }

            inverseFAM(fam, VG, VR);
            if(options.find("save_inv") != options.end()){
                LOGGER.i(0, "Saving inverse of V for further analysis, use --load-inv for further analysis");
                std::ofstream inv_id((options["out"]+".grm.id").c_str());
                if(!inv_id) LOGGER.e(0, "failed to write " + options["out"]+".grm.id");
                for(int k = 0; k < remain_ids_fam.size(); k++){
                    inv_id << remain_ids_fam[k] << std::endl;
                }

                FILE * inv_out = fopen((options["out"]+".grm.inv").c_str(), "wb");
                InvItem item;
                for(int k = 0; k < V_inverse.outerSize(); ++k){
                    for(SpMat::InnerIterator it(V_inverse, k); it; ++it){
                        item.row = it.row();
                        item.col = it.col();
                        item.val = it.value();
                        if(fwrite(&item, sizeof(item), 1, inv_out) != 1){
                            LOGGER.e(0, "can't write to [" + options["out"] + ".grm.inv]");
                        }
                    }
                }
                fclose(inv_out);

                LOGGER.i(0, "The inverse has been saved to [" + options["out"] + ".grm.inv]");
            }
        }else{
            string id_file = options["inv_file"] + ".grm.id";
            std::ifstream h_id(id_file.c_str());
            if(!h_id){
                LOGGER.e(0, "can't read file [" + id_file + "].");
            }
            string line;
            uint32_t cur_index = 0;
            getline(h_id, line);
            if(line == "--fastGWA") {
                fam_flag = false;
                LOGGER.w(0, "The estimate of Vg is not statistically significant. "
                        "This is likely because the number of relatives is not large enough. "
                        "\nPerforming simple regression via removing --grm-sparse instead...");
 
                fam.resize(0, 0);
                V_inverse.resize(0, 0);
                return;
            }
            // go to the original position
            h_id.clear(); 
            h_id.seekg(0);
            while(getline(h_id, line)){
                if(line != remain_ids_fam[cur_index]){
                    LOGGER.e(0, "samples are not same from line " + to_string(cur_index + 1) + " in [" + id_file + "].");
                }
                cur_index++;
            }
            h_id.close();
            if(cur_index == remain_ids_fam.size()){
                LOGGER.i(0, to_string(cur_index) + " samples are checked identical in inverse V [" + id_file + "].");
            }else{
                LOGGER.e(0, "Empty file or lines not consistent in inverse V [" + id_file + "].");
            }

            V_inverse.resize(phenoVec.size(), phenoVec.size());
            string in_name = options["inv_file"] + ".grm.inv";
            LOGGER.i(0, "Loading inverse of V from " + in_name + "...");
            LOGGER.ts("LOAD_INV");
            FILE * in_file = fopen(in_name.c_str(), "rb");
            if(!in_file){
                LOGGER.e(0, "can't open the file.");
            }
            fseek(in_file, 0L, SEEK_END);
            size_t file_size = ftell(in_file);
            rewind(in_file);

            InvItem item;
            size_t cur_pos = 0;
            while(cur_pos < file_size){
                if(fread(&item, sizeof(item), 1, in_file) == 1){
                    V_inverse.insert(item.row, item.col) = item.val;
                    cur_pos += sizeof(item);
                }else{
                    LOGGER.e(0, "can't read file in pos: " + to_string(cur_pos));
                }
            }
            fclose(in_file);

            V_inverse.finalize();
            V_inverse.makeCompressed();
            LOGGER.i(0, "Inverse of V loaded in " + to_string(LOGGER.tp("LOAD_INV")) + " seconds.");
        }
    }
}

void FastFAM::initMarkerVars(){
    num_marker = geno->marker->count_extract();
    if(beta)delete[] beta;
    if(se) delete[] se;
    if(p) delete[] p;
    beta = new float[num_marker];
    se = new float[num_marker];
    p = new float[num_marker];
}

FastFAM::~FastFAM(){
    if(beta)delete[] beta;
    if(se) delete[] se;
    if(p) delete[] p;
}


void FastFAM::conditionCovarReg(VectorXd &pheno, MatrixXd &covar){
    MatrixXd t_covar = covar.transpose();
    VectorXd beta = (t_covar * covar).ldlt().solve(t_covar * pheno);
    //LOGGER.i(0, "DEBUG: condition betas:");
    //LOGGER << beta << std::endl;
    //VectorXd beta = covar.jacobiSvd(Eigen::ComputeThinU | Eigen::ComputeThinV).solve(pheno);
    pheno -= covar * beta;
    //double pheno_mean = pheno.mean();
    //pheno -= (VectorXd::Ones(pheno.size())) * pheno_mean;
}

double FastFAM::HEreg(const Ref<const SpMat> fam, const Ref<const VectorXd> pheno, bool &isSig){
    int num_covar = 1;
    int num_component = 1;
    int col_X = num_covar + num_component;
    MatrixXd XtX = MatrixXd::Zero(col_X, col_X);
    VectorXd XtY = VectorXd::Zero(col_X);
    double SSy = 0;

    uint64_t size = fam.cols() * fam.rows();
    XtX(0, 0) = size;

    for(int i = 1; i < fam.cols(); i++){
        double temp_pheno = pheno[i];
        auto fam_block = fam.block(0, i, i, 1);
        auto pheno_block = pheno.head(i) * temp_pheno;
        SSy += pheno_block.dot(pheno_block);
        XtY[0] += pheno_block.sum();
        XtY[1] += (pheno_block.transpose() * fam_block)[0];
        XtX(0,1) += fam_block.sum();
        XtX(1,1) += (fam_block.cwiseProduct(fam_block)).sum();
    }

    //MatrixXd XtXi = XtX.selfadjointView<Eigen::Upper>().inverse();
    XtX(1,0) = XtX(0,1);
    LOGGER << "XtX:" << endl;
    LOGGER << XtX << endl;

    Eigen::FullPivLU<MatrixXd> lu(XtX);
    if(lu.rank() < XtX.rows()){
        LOGGER.w(0, "the XtX matrix is invertable.");
        isSig = false;
        return std::numeric_limits<double>::quiet_NaN();
    }

    MatrixXd XtXi = lu.inverse();

    VectorXd betas = XtXi * XtY;
    LOGGER << "beta:" << endl;
    LOGGER << betas << endl;

    double sse = (SSy - betas.dot(XtY)) / (size - col_X);
    LOGGER << "SSE: " << sse << endl;

    VectorXd SDs = sse * XtXi.diagonal();
    LOGGER << "SD: " << SDs << endl;

    double hsq = betas[betas.size() - 1];
    double SD = SDs[SDs.size() - 1];
    double Zsq = hsq * hsq / SD;
    double p = StatLib::pchisqd1(Zsq);

    LOGGER.i(2, "Vg = " + to_string(hsq) + ", se = " + to_string(sqrt(SD)) +  ", P = " + to_string(p));

    if(p > 0.05){
        isSig = false;
    }else{
        isSig = true;
    }
    return hsq;
}

double FastFAM::HEreg(vector<double> &Zij, vector<double> &Aij, bool &isSig){
    Map<VectorXd> ZVec(Zij.data(), Zij.size());
    Map<VectorXd> AVec(Aij.data(), Aij.size());

    double Zmean = ZVec.mean();
    double Amean = AVec.mean();
    ZVec -= (VectorXd::Ones(ZVec.size())) * Zmean;
    AVec -= (VectorXd::Ones(AVec.size())) * Amean;

    double A2v = (AVec.transpose() * AVec)(0, 0);
    if(A2v < 1e-6){
        LOGGER.e(0, "can't solve the regression");
    }
    double AZ = (AVec.transpose() * ZVec)(0, 0);
    double hsq = (1.0 / A2v) * AZ;

    VectorXd RZVec = ZVec - AVec * hsq;

    double delta = RZVec.array().square().sum() / (RZVec.size() - 2);
    double se = sqrt(delta / A2v);

    double z = hsq / se;

    double p = StatLib::pchisqd1(z * z);

    LOGGER.i(2, "Vg = " + to_string(hsq) + ", se = " + to_string(se) +  ", P = " + to_string(p));

    if(p > 0.05){
        isSig = false;
    }else{
        isSig = true;
    }

    return hsq;
}
    

void FastFAM::readFAM(string filename, SpMat& fam, const vector<string> &ids, vector<uint32_t> &remain_index){
    LOGGER.i(0, "Reading the sparse GRM file from [" + filename + "]...");
    uint32_t num_indi = ids.size();
    vector<string> sublist = Pheno::read_sublist(filename + ".grm.id");
    vector<uint32_t> fam_index;
    vector_commonIndex(sublist, ids, fam_index, remain_index);
    //LOGGER.i(0, "DEBUG: " + to_string(fam_index.size()) + " subjects remained");

    //Fix index order to outside, that fix the phenotype, covar order
    vector<size_t> index_list_order = sort_indexes(remain_index);
    vector<uint32_t> ordered_fam_index(remain_index.size(), 0);
    vector<uint32_t> ordered_remain_index(remain_index.size(), 0);
    std::transform(index_list_order.begin(), index_list_order.end(), ordered_fam_index.begin(), [&fam_index](size_t pos){
            return fam_index[pos];});
    std::transform(index_list_order.begin(), index_list_order.end(), ordered_remain_index.begin(), [&remain_index](size_t pos){
            return remain_index[pos];});
    remain_index = ordered_remain_index;

    std::ifstream pair_list((filename + ".grm.sp").c_str());
    if(!pair_list){
        LOGGER.e(0, "can't read [" + filename + ".grm.sp]");
    }

    string line;
    int line_number = 0;
    int last_length = 0;

    vector<uint32_t> id1;
    vector<uint32_t> id2;
    vector<double> grm;

    vector<uint32_t> num_elements(num_indi, 0);

    map<uint32_t, uint32_t> map_index;
    for(uint32_t index = 0; index != ordered_fam_index.size(); index++){
        map_index[ordered_fam_index[index]] = index;
    }


    while(std::getline(pair_list, line)){
        line_number++;
        std::istringstream line_buf(line);
        std::istream_iterator<string> begin(line_buf), end;
        vector<string> line_elements(begin, end);

        uint32_t tmp_id1 = (std::stoi(line_elements[0]));
        uint32_t tmp_id2 = (std::stoi(line_elements[1]));
        if(map_index.find(tmp_id1) != map_index.end() &&
                map_index.find(tmp_id2) != map_index.end()){
            tmp_id1 = map_index[tmp_id1];
            tmp_id2 = map_index[tmp_id2];

            double tmp_grm = std::stod(line_elements[2]);
            id1.push_back(tmp_id1);
            id2.push_back(tmp_id2);
            num_elements[tmp_id2] += 1;
            grm.push_back(tmp_grm);
            if(tmp_id1 != tmp_id2){
                id1.push_back(tmp_id2);
                id2.push_back(tmp_id1);
                num_elements[tmp_id1] += 1;
                grm.push_back(tmp_grm);
            }
        }
    }
    pair_list.close();

    auto sorted_index = sort_indexes(id2, id1);

    fam.resize(ordered_fam_index.size(), ordered_fam_index.size());
    fam.reserve(num_elements);

    for(auto index : sorted_index){
        fam.insertBackUncompressed(id1[index], id2[index]) = grm[index];
    }
    fam.finalize();
    fam.makeCompressed();

}

void FastFAM::inverseFAM(SpMat& fam, double VG, double VR){
    LOGGER.i(0, "Inverting the variance-covarinace matrix (This may take a long time).");
    LOGGER.i(0, string("Inverse method: ") + options["inv_method"]);
    LOGGER.i(0, "DEUBG: Inverse Threads " + to_string(Eigen::nbThreads()));
    LOGGER.ts("INVERSE_FAM");
    SpMat eye(fam.rows(), fam.cols());
    LOGGER.i(0, "FAM " + to_string(fam.rows()) + " * " + to_string(fam.cols()));
    eye.setIdentity();

    // V
    fam *= VG;
    fam += eye * VR;

    // inverse
    if(options["inv_method"] == "ldlt"){
        Eigen::SimplicialLDLT<SpMat> solver;
        solver.compute(fam);
        if(solver.info() != Eigen::Success){
            LOGGER.e(0, "can't inverse the FAM");
        }
        V_inverse = solver.solve(eye);
    }else if(options["inv_method"] == "cg"){
        Eigen::ConjugateGradient<SpMat> solver;
        solver.compute(fam);
        if(solver.info() != Eigen::Success){
            LOGGER.e(0, "can't inverse the FAM");
        }
        V_inverse = solver.solve(eye);
    }else if(options["inv_method"] == "llt"){
        Eigen::SimplicialLLT<SpMat> solver;
        solver.compute(fam);
        if(solver.info() != Eigen::Success){
            LOGGER.e(0, "can't inverse the FAM");
        }
        V_inverse = solver.solve(eye);
    }else if(options["inv_method"] == "pardiso1"){
        /*
        Eigen::PardisoLLT<SpMat> solver;
        solver.compute(fam);
        if(solver.info() != Eigen::Success){
            LOGGER.e(0, "can't inverse the FAM");
        }
        V_inverse = solver.solve(eye);
        */
    }else if(options["inv_method"] == "tcg"){
        Eigen::ConjugateGradient<SpMat, Eigen::Lower|Eigen::Upper> solver;
        solver.compute(fam);
        if(solver.info() != Eigen::Success){
            LOGGER.e(0, "can't inverse the FAM");
        }
        V_inverse = solver.solve(eye);
    }else if(options["inv_method"] == "lscg"){
        Eigen::LeastSquaresConjugateGradient<SpMat> solver;
        solver.compute(fam);
        if(solver.info() != Eigen::Success){
            LOGGER.e(0, "can't inverse the FAM");
        }
        V_inverse = solver.solve(eye);
    }else{
        LOGGER.e(0, "Unknown inverse methods");
    }


    //solver.setTolerance(1e-3);
    ///solver.setMaxIterations(10);;

   //LOGGER.i(0, "# iteations: " + to_string(solver.iterations()));
    //LOGGER.i(0, "# error: " + to_string(solver.error()));

    LOGGER.i(0, "Inverted in " + to_string(LOGGER.tp("INVERSE_FAM")) + " seconds");
}

void FastFAM::calculate_gwa(uint64_t *buf, int num_marker){
/*
        std::ofstream pheno_w2(options["out"] + "_gwa_phen.txt");
        pheno_w2 << phenoVec << std::endl;
        pheno_w2.close();
        LOGGER << "LOOP: " << num_finished_marker << std::endl;
*/
        //MatrixXd dealGeno(num_indi, num_marker);
    #pragma omp parallel for schedule(dynamic)
    for(int cur_marker = 0; cur_marker < num_marker; cur_marker++){
        double *w_buf = new double[num_indi];
        Map< VectorXd > xMat(w_buf, num_indi);
        MatrixXd XMat_V;

        geno->makeMarkerX(buf, cur_marker, w_buf, true, false);
        //dealGeno.col(cur_marker) = xMat;

        MatrixXd tMat_V = xMat.transpose();

        double xMat_V_x = 1.0 / (tMat_V * xMat)(0, 0);
        double xMat_V_p = (tMat_V * phenoVec)(0, 0);

        double temp_beta =  xMat_V_x * xMat_V_p;
        double temp_se = sqrt(xMat_V_x);
        double temp_z = temp_beta / temp_se;

        uint32_t cur_raw_marker = num_finished_marker + cur_marker;

        beta[cur_raw_marker] = temp_beta; //* geno->RDev[cur_raw_marker]; 
        se[cur_raw_marker] = temp_se;
        p[cur_raw_marker] = StatLib::pchisqd1(temp_z * temp_z); 
        delete[] w_buf;
    }

    /*
    std::ofstream o_geno(options["out"] + "_geno.txt");
        o_geno << dealGeno << std::endl;
        o_geno.close();
    */
     

    num_finished_marker += num_marker;
    if(num_finished_marker % 30000 == 0){
        LOGGER.i(2, to_string(num_finished_marker) + " markers finished"); 
    }

}


void FastFAM::calculate_fam(uint64_t *buf, int num_marker){
    // Memory fam_size * 2 * 4 + (N * 8 * 2 ) * thread_num + M * 3 * 8  B
    //int num_thread = THREADS.getThreadCount() + 1; 
    #pragma omp parallel for schedule(dynamic)
    for(int cur_marker = 0; cur_marker < num_marker; cur_marker++){
        double *w_buf = new double[num_indi];
        Map< VectorXd > xMat(w_buf, num_indi);
        MatrixXd XMat_V;

        geno->makeMarkerX(buf, cur_marker, w_buf, true, false);
        // Xt * V-1
        MatrixXd xMat_V = xMat.transpose() * V_inverse;
        // 
        double xMat_V_x = 1.0 / (xMat_V * xMat)(0, 0);
        double xMat_V_p = (xMat_V * phenoVec)(0, 0);

        double temp_beta =  xMat_V_x * xMat_V_p;
        double temp_se = sqrt(xMat_V_x);
        double temp_z = temp_beta / temp_se;

        uint32_t cur_raw_marker = num_finished_marker + cur_marker;

        beta[cur_raw_marker] = temp_beta; //* geno->RDev[cur_raw_marker]; 
        se[cur_raw_marker] = temp_se;
        p[cur_raw_marker] = StatLib::pchisqd1(temp_z * temp_z); 
        delete[] w_buf;
    }
/*
    int num_thread = omp_get_max_threads();
    int num_marker_part = (num_marker + num_thread - 1) / num_thread;
    #pragma omp parallel for
    for(int index = 0; index <= num_thread; index++){
        if(index != num_thread){
            reg_thread(buf, index * num_marker_part, (index + 1) * num_marker);
        }else{
            reg_thread(buf, (num_thread - 1) * num_marker_part, num_marker);
        }
        //THREADS.AddJob(std::bind(&FastFAM::reg_thread, this, buf, index * num_marker_part, (index + 1) * num_marker_part));
    }

    //THREADS.WaitAll();
*/
    num_finished_marker += num_marker;
    if(num_finished_marker % 30000 == 0){
        LOGGER.i(2, to_string(num_finished_marker) + " markers finished"); 
    }
}
/*
void FastFAM::reg_thread(uint8_t *buf, int from_marker, int to_marker){
    //Eigen::setNbThreads(1);
    double *w_buf = new double[num_indi];
    Map< VectorXd > xMat(w_buf, num_indi);
    MatrixXd XMat_V;
    for(int cur_marker = from_marker; cur_marker < to_marker; cur_marker++){
        geno->makeMarkerX(buf, cur_marker, w_buf, true, false);
        // Xt * V-1
        MatrixXd xMat_V = xMat.transpose() * V_inverse;
        // 
        double xMat_V_x = 1.0 / (xMat_V * xMat)(0, 0);
        double xMat_V_p = (xMat_V * phenoVec)(0, 0);
        
        double temp_beta =  xMat_V_x * xMat_V_p;
        double temp_se = sqrt(xMat_V_x);
        double temp_z = temp_beta / temp_se;

        uint32_t cur_raw_marker = num_finished_marker + cur_marker;

        beta[cur_raw_marker] = temp_beta; //* geno->RDev[cur_raw_marker]; 
        se[cur_raw_marker] = temp_se;
        {
            std::lock_guard<std::mutex> lock(chisq_lock);
            p[cur_raw_marker] = StatLib::pchisqd1(temp_z * temp_z); 
        } 
    }
    delete[] w_buf;
}
*/


void FastFAM::output(string filename){
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double MAF_L_THRESH = 0.00001;
    const double MAF_U_THRESH = 0.99999;
    if(options.find("save_bin") == options.end()){
        std::ofstream out(filename.c_str());
        if(options.find("no_marker") == options.end()){
            vector<string> header{"CHR", "SNP", "POS", "A1", "A2", "AF1", "beta", "se", "p"};
            //std::copy(header.begin(), header.end(), std::ostream_iterator<string>(out, "\t"));
            string header_string = boost::algorithm::join(header, "\t");
            out << header_string << std::endl;
            for(int index = 0; index != num_marker; index++){
                double af = geno->AFA1[index];
                if(af > MAF_L_THRESH && af < MAF_U_THRESH){
                    out << geno->marker->get_marker(geno->marker->getExtractIndex(index)) << "\t" <<
                        af << "\t" << beta[index] << "\t" << se[index] << "\t" << p[index] << std::endl;
                }else{
                    out << geno->marker->get_marker(geno->marker->getExtractIndex(index)) << "\t" <<
                        af << "\t" << nan << "\t" << nan << "\t" << nan << std::endl;
                }
            }
        }else{
            vector<string> header{"AF1", "beta", "se", "p"};
            //std::copy(header.begin(), header.end(), std::ostream_iterator<string>(out, "\t"));
            string header_string = boost::algorithm::join(header, "\t");
            out << header_string << std::endl;
            for(int index = 0; index != num_marker; index++){
                double af = geno->AFA1[index];
                if(af > MAF_L_THRESH && af < MAF_U_THRESH){
                    out << af << "\t" << beta[index] << "\t" << se[index] << "\t" << p[index] << std::endl;
                }else{
                    out << af << "\t" << nan << "\t" << nan << "\t" << nan << std::endl;
                }
            }
            LOGGER.i(0, "No SNP information saved, " + to_string(num_marker) + " SNPs saved");
        }
        out.close();
        LOGGER.i(0, "The association results have been saved to [" + filename +"].");
    }else{
        if(options.find("no_marker") == options.end()){
            std::ofstream out((filename + ".snp").c_str());
            for(int index = 0; index != num_marker; index++){
                out << geno->marker->get_marker(geno->marker->getExtractIndex(index)) << std::endl;
            }
            out.close();
            LOGGER.i(0, "The SNP inf of association results has been saved to [" + filename + ".snp].");
        }else{
            LOGGER.i(0, "No SNP information saved, " + to_string(num_marker) + " SNPs saved");
        }

        float * afa1 = new float[num_marker];
        for(int index = 0; index != num_marker; index++){
            double af = geno->AFA1[index];
            afa1[index] = af;
            if(af < MAF_L_THRESH || af > MAF_U_THRESH){
                beta[index] = nan;
                se[index] = nan;
                p[index] = nan;
            }
        }

        FILE * h_out = fopen((filename + ".bin").c_str(), "wb");
        if(!h_out){LOGGER.e(0,  "can't open [" + filename + ".bin] to write.");}
        if(fwrite(afa1, sizeof(float), num_marker, h_out) != num_marker){
            LOGGER.e(0, "can't write AF to [" + filename + ".bin].");
        }
        delete[] afa1;

        if(fwrite(beta, sizeof(float), num_marker, h_out) != num_marker){
            LOGGER.e(0, "can't write beta to [" + filename + ".bin].");
        }
         if(fwrite(se, sizeof(float), num_marker, h_out) != num_marker){
            LOGGER.e(0, "can't write se to [" + filename + ".bin].");
        }
        if(fwrite(p, sizeof(float), num_marker, h_out) != num_marker){
            LOGGER.e(0, "can't write p to [" + filename + ".bin].");
        }
        fclose(h_out);
        LOGGER.i(0, "The association results have been saved to [" + filename + ".bin] in binary format.");
    }
        
}

int FastFAM::registerOption(map<string, vector<string>>& options_in){
    int returnValue = 0;
    //DEBUG: change to .fastFAM
    options["out"] = options_in["out"][0] + ".fastFAM.assoc";

    string curFlag = "--fastFAM";
    if(options_in.find(curFlag) != options_in.end()){
        processFunctions.push_back("fast_fam");
        returnValue++;
        options_in.erase(curFlag);
    }

    curFlag = "--grm-sparse";
    if(options_in.find(curFlag) != options_in.end()){
        if(options_in[curFlag].size() == 1){
            options["grmsparse_file"] = options_in[curFlag][0];
        }else{
            LOGGER.e(0, curFlag + "can't deal with 0 or > 1 files");
        }
        options_in.erase(curFlag);
    }

    curFlag = "--ge";
    if(options_in.find(curFlag) != options_in.end()){
        if(options_in[curFlag].size() == 2){
            options["G"] = options_in[curFlag][0];
            options["E"] = options_in[curFlag][1];
        }else{
            LOGGER.e(0, curFlag + " can't handle other than 2 numbers");
        }
        options_in.erase(curFlag);
    }

    /*
    curFlag = "--qcovar";
    if(options_in.find(curFlag) != options_in.end()){
        if(options_in[curFlag].size() == 1){
            options["concovar"] = options_in[curFlag][0];
        }else{
            LOGGER.e(0, curFlag + "can't deal with covar other than 1");
        }
    }
    */

    options["inv_method"] = "ldlt";
    vector<string> flags = {"--cg", "--ldlt", "--llt", "--pardiso", "--tcg", "--lscg"};
    for(auto curFlag : flags){
        if(options_in.find(curFlag) != options_in.end()){
            boost::erase_all(curFlag, "--");
            options["inv_method"] = curFlag;
            options_in.erase(curFlag);
        }
    }

    curFlag = "--save-inv";
    if(options_in.find(curFlag) != options_in.end()){
        options["save_inv"] = "yes";
        options_in.erase(curFlag);
    }

    curFlag = "--save-bin";
    if(options_in.find(curFlag) != options_in.end()){
        options["save_bin"] = "yes";
        //options_in.erase(curFlag);
    }

    curFlag = "--no-marker";
    if(options_in.find(curFlag) != options_in.end()){
        options["no_marker"] = "yes";
        //options_in.erase(curFlag);
    }


    curFlag = "--load-inv";
    if(options_in.find(curFlag) != options_in.end()){
        if(options_in[curFlag].size() == 1){
            options["inv_file"] = options_in[curFlag][0];
        }else{
            LOGGER.e(0, "can't load multiple --load-inv files");
        }
        options_in.erase(curFlag);
    }

    curFlag = "--save-pheno";
    if(options_in.find(curFlag) != options_in.end()){
        options["save_pheno"] == "yes";
        options_in.erase(curFlag);
    }

    curFlag = "--rel-only";
    if(options_in.find(curFlag) != options_in.end()){
        options["rel_only"] = "yes";
        options_in.erase(curFlag);
    }else{
        options["rel_only"] = "no";
    }

    return returnValue;
}

void FastFAM::processMain(){
    vector<function<void (uint64_t *, int)>> callBacks;
    //THREADS.JoinAll();
    for(auto &process_function : processFunctions){
        if(process_function == "fast_fam"){
            Pheno pheno;
            Marker marker;
            Geno geno(&pheno, &marker);
            FastFAM ffam(&geno);
            bool freqed = geno.filterMAF();
            ffam.initMarkerVars();

            if(options.find("save_inv") != options.end()){
                LOGGER.i(0, "Use --load-inv to load the inversed file for fastFAM");
                return;
            }
            //Eigen::setNbThreads(1);
            if(!freqed){
                callBacks.push_back(bind(&Geno::freq64, &geno, _1, _2));
            }
            if(options.find("grmsparse_file") != options.end() && ffam.fam_flag){
                LOGGER.i(0, "\nRunning fastFAM...");
                callBacks.push_back(bind(&FastFAM::calculate_fam, &ffam, _1, _2));
            }else{
                LOGGER.i(0, "\nRunning GWAS...");
                callBacks.push_back(bind(&FastFAM::calculate_gwa, &ffam, _1, _2));
            }
            geno.loop_64block(marker.get_extract_index(), callBacks);

            ffam.output(options["out"]);
        }
    }
}


