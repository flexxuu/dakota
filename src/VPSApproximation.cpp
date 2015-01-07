/*  _______________________________________________________________________

    DAKOTA: Design Analysis Kit for Optimization and Terascale Applications
    Copyright 2014 Sandia Corporation.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Dakota directory.
    _______________________________________________________________________ */

//- Class:	 VPSApproximation
//- Description: Class implementation for Voronoi Piecewise Surrogate Approximation
//- Owner:       Mohamed Ebeida
//- Checked by:
//- Version:
//-------------------------------------------------------------------------
#include "VPSApproximation.hpp"
#include "dakota_data_types.hpp"
#include "DakotaIterator.hpp"
#include "DakotaResponse.hpp"
//#include "NPSOLOptimizer.hpp"
#ifdef HAVE_NCSU
#include "NCSUOptimizer.hpp"
#endif
#include "ProblemDescDB.hpp"

#ifdef HAVE_OPTPP
#include "SNLLOptimizer.hpp"
using OPTPP::NLPFunction;
using OPTPP::NLPGradient;
#endif

#include "Teuchos_LAPACK.hpp"
#include "Teuchos_SerialDenseSolver.hpp"
#include "Teuchos_SerialDenseHelpers.hpp"

//#define DEBUG
//#define DEBUG_FULL

//#define DEBUG_TEST_FUNCTION


namespace Dakota {

using Teuchos::rcp;

typedef Teuchos::SerialDenseSolver<int, Real>    RealSolver;
typedef Teuchos::SerialSpdDenseSolver<int, Real> RealSpdSolver;

//initialization of statics
VPSApproximation* VPSApproximation::VPSinstance(NULL);

VPSApproximation::
VPSApproximation(const ProblemDescDB& problem_db,
		       const SharedApproxData& shared_data):
  Approximation(BaseConstructor(), problem_db, shared_data),
  surrogateOrder(problem_db.get_int("model.surrogate.surrogate_order")),
  _disc_min_grad(problem_db.get_real("model.surrogate.discont_grad_threshold"))
{
    std::cout << "*** VPS:: Initializing, Surrogate order " << surrogateOrder << std::endl;
    
    if (_disc_min_grad < 1E-10)
    {
        _disc_min_grad = DBL_MAX;
        std::cout << "*** VPS: Discontinuity gradient threshold = " << _disc_min_grad << std::endl;
    }
    else
    {
        std::cout << "*** VPS: Discontinuity gradient threshold = " << _disc_min_grad << std::endl;
    }
}



int VPSApproximation::min_coefficients() const
{
  // min number of samples required to build the network is equal to
  // the number of design variables + 1

  // Note: Often this is too few samples.  It is better to have about
  // O(n^2) samples, where 'n' is the number of variables.

  return sharedDataRep->numVars + 1;
}


int VPSApproximation::num_constraints() const
{ return (approxData.anchor()) ? 1 : 0; }


void VPSApproximation::build()
{
    _num_inserted_points = 0;
    
    _vps_order = surrogateOrder;
  
    // base class implementation checks data set against min required
    Approximation::build();

  
    size_t j, offset = 0, num_v = sharedDataRep->numVars;
    numObs = approxData.points(); // number of points
    
  
    std::cout<< "VPS::    Number of sample points = " << numObs << std::endl;
    _num_inserted_points = numObs;
  
    std::cout<< "VPS::    Number of dimensions = " << num_v << std::endl;
    _n_dim = num_v;
    
    std::cout<< "VPS::    Surrogate Order = " << _vps_order << std::endl;
  
    _xmin = new double[_n_dim];
    _xmax = new double[_n_dim];

    for (size_t idim = 0; idim < _n_dim; idim++)
    {
        _xmin[idim] = DBL_MAX;
        _xmax[idim] = -DBL_MAX;
    }
    
    _sample_points = new double*[_num_inserted_points];
    _fval = new double[_num_inserted_points];
    
    
    // process currentPoints from approx data
    for (size_t ipoint = 0; ipoint < _num_inserted_points; ipoint++)
    {
        const RealVector& c_vars = approxData.continuous_variables(ipoint);
        _sample_points[ipoint] = new double[_n_dim];
        for (size_t idim = 0; idim < _n_dim; idim++)
        {
            _sample_points[ipoint][idim] = c_vars[idim];
            if (_sample_points[ipoint][idim] < _xmin[idim]) _xmin[idim] = _sample_points[ipoint][idim];
            if (_sample_points[ipoint][idim] > _xmax[idim]) _xmax[idim] = _sample_points[ipoint][idim];
        }
        
#ifdef DEBUG_TEST_FUNCTION
        _fval[ipoint] = f_test(_sample_points[ipoint]);
        _xmin[0] = 0.0; _xmin[1] = 0.0;
        _xmax[0] = 1.0; _xmax[1] = 1.0;
#else
        // response from approxData
        _fval[ipoint] = approxData.response_function(ipoint);
#endif
        
    }
        
    _diag = 0.0;
    for (size_t idim = 0; idim < _n_dim; idim++)
    {
        double DX = _xmax[idim] - _xmin[idim];
#ifndef DEBUG_TEST_FUNCTION
        _xmax[idim]+= 0.5 * DX;
        _xmin[idim]-= 0.5 * DX;
        DX = _xmax[idim] - _xmin[idim];
#endif   
        _diag += DX * DX;
    }
    _diag = sqrt(_diag);

    // default domain is for the Herbie Function
    

    // Build a VPS surrogate model using the sampled data
    VPS_execute();
}



    Real VPSApproximation::value(const Variables& vars)
    {
        
        VPSmodel_apply(vars.continuous_variables(),false,false);
        return approxValue;
    }



    const RealVector& VPSApproximation::gradient(const Variables& vars)
    {
        VPSmodel_apply(vars.continuous_variables(),false,true);
        return approxGradient;
    }


    Real VPSApproximation::prediction_variance(const Variables& vars)
    {
        VPSmodel_apply(vars.continuous_variables(),true,false);
        return approxVariance;
    }



    void VPSApproximation::VPSmodel_build()
    {
        
    }

    void VPSApproximation::VPSmodel_apply(const RealVector& approx_pt, bool variance_flag, bool gradients_flag)
    {
        double* x = new double[_n_dim];
    
        for (size_t idim = 0; idim < _n_dim; idim++)
        {
            x[idim] = approx_pt[idim];
        }
        approxValue = evaluate_surrogate(x);
        delete[] x;
    }
    
    
    
    void VPSApproximation::initiate_random_number_generator(unsigned long x)
    {
        //assert(sizeof (double) >= 54) ;
        
        cc = 1.0 / 9007199254740992.0; // inverse of 2^53rd power
        size_t i;
        size_t qlen = indx = sizeof Q / sizeof Q[0];
        for (i = 0; i < qlen; i++) Q[i] = 0;
        
        double c = 0.0, zc = 0.0,	/* current CSWB and SWB `borrow` */
        zx = 5212886298506819.0 / 9007199254740992.0,	/* SWB seed1 */
        zy = 2020898595989513.0 / 9007199254740992.0;	/* SWB seed2 */
        
        size_t j;
        double s, t;	 /* Choose 32 bits for x, 32 for y */
        if (x == 0) x = 123456789; /* default seeds */
        unsigned long y = 362436069; /* default seeds */
        
        /* Next, seed each Q[i], one bit at a time, */
        for (i = 0; i < qlen; i++)
        { /* using 9th bit from Cong+Xorshift */
            s = 0.0;
            t = 1.0;
            for (j = 0; j < 52; j++)
            {
                t = 0.5 * t; /* make t=.5/2^j */
                x = 69069 * x + 123;
                y ^= (y << 13);
                y ^= (y >> 17);
                y ^= (y << 5);
                if (((x + y) >> 23) & 1) s = s + t; /* change bit of s, maybe */
            }	 /* end j loop */
            Q[i] = s;
        } /* end i seed loop, Now generate 10^9 dUNI's: */
    }
    
    double VPSApproximation::generate_a_random_number()
    {
        /* Takes 14 nanosecs, Intel Q6600,2.40GHz */
        int i, j;
        double t; /* t: first temp, then next CSWB value */
        /* First get zy as next lag-2 SWB */
        t = zx - zy - zc;
        zx = zy;
        if (t < 0)
        {
            zy = t + 1.0;
            zc = cc;
        }
        else
        {
            zy = t;
            zc = 0.0;
        }
        
        /* Then get t as the next lag-1220 CSWB value */
        if (indx < 1220)
            t = Q[indx++];
        else
        { /* refill Q[n] via Q[n-1220]-Q[n-1190]-c, */
            for (i = 0; i < 1220; i++)
            {
                j = (i < 30) ? i + 1190 : i - 30;
                t = Q[j] - Q[i] + c; /* Get next CSWB element */
                if (t > 0)
                {
                    t = t - cc;
                    c = cc;
                }
                else
                {
                    t = t - cc + 1.0;
                    c = 0.0;
                }
                Q[i] = t;
            }	 /* end i loop */
            indx = 1;
            t = Q[0]; /* set indx, exit 'else' with t=Q[0] */
        } /* end else segment; return t-zy mod 1 */
        
        return ((t < zy) ? 1.0 + (t - zy) : t - zy);
    }

    
    bool VPSApproximation::trim_line_using_Hyperplane(size_t num_dim,                               // number of dimensions
                                                  double* st, double *end,                      // line segmenet end points
                                                  double* qH, double* nH)                // a point on the hyperplane and it normal
    {
        double dotv(0.0), dote(0.0);
        for (size_t idim = 0; idim < num_dim; idim++)
        {
            double dxv = qH[idim] - st[idim];
            double dxe = end[idim] - st[idim];
            dotv += dxv * nH[idim];
            dote += dxe * nH[idim];
        }
        
        if (fabs(dote) < 1E-10) // cos theta_n < 1E-10
        {
            // line is either degenerate or parallel to Hyperplane
            return false;
        }
        
        if (fabs(dote) < fabs(dotv))
        {
            // line lie on one side of the Hyperplane
            if (dotv < 0.0)
            {
                for (size_t idim = 0; idim < num_dim; idim++) end[idim] = st[idim];
            }
            return false;
        }
        
        double u = dotv / dote;
        if (u > 0.0 && u < 1.0)
        {
            for (size_t idim = 0; idim < num_dim; idim++)
            {
                if (dote > 0.0) end[idim] = st[idim] + u * (end[idim] - st[idim]);
                else st[idim] = st[idim] + u * (end[idim] - st[idim]);
            }
            return true;
        }
        
        if (u < 0.0 && dote > 0.0)
        {
            for (size_t idim = 0; idim < num_dim; idim++) end[idim] = st[idim];
            return false;
        }
        
        if (u > 1.0 && dote < 0.0)
        {
            for (size_t idim = 0; idim < num_dim; idim++) end[idim] = st[idim];
            return false;
        }
        return false;
    }
    
    //////////////////////////////////////////////////////////////
    // VPS METHODS
    //////////////////////////////////////////////////////////////
    
    bool VPSApproximation::VPS_execute()
    {
        initiate_random_number_generator(1234567890);
        
        clock_t start_time, end_time; double cpu_time, total_time(0.0);
        
        start_time = clock();
        
        _num_GMRES = 0;
        
        _vps_dfar = new double[_num_inserted_points];
        _vps_ext_neighbors = new size_t*[_num_inserted_points];
        
        // retrive powers of the polynomial expansion
        retrieve_permutations(_vps_num_poly_terms, _vps_t, _n_dim, _vps_order, false, true, _vps_order);
        //std::cout<< "VPS::    Number of poly terms = " << _vps_num_poly_terms << std::endl;
        
        // update neighbors
        //std::cout << "updating neighbors!" << std::endl;
        _sample_neighbors = new size_t*[_num_inserted_points];
        _sample_vsize = new double[_num_inserted_points];
        for (size_t isample = 0; isample < _num_inserted_points; isample++)
        {
            _sample_neighbors[isample] = new size_t[_num_inserted_points];
            _sample_neighbors[isample][0] = 0;
            retrieve_neighbors(isample, false);
        }
        
        // initiate extended neighbors with seed neighbors
        //std::cout << "extending neighbors!" << std::endl;
        _vps_ext_neighbors = new size_t*[_num_inserted_points];
        for (size_t isample = 0; isample < _num_inserted_points; isample++)
        {
            size_t num_neighbors = 0;
            if (_sample_neighbors[isample] != 0) num_neighbors = _sample_neighbors[isample][0];
            _vps_ext_neighbors[isample] = new size_t[num_neighbors + 1];
            _vps_ext_neighbors[isample][0] = num_neighbors;
            for (size_t ineighbor = 1; ineighbor <= num_neighbors; ineighbor++)
            {
                _vps_ext_neighbors[isample][ineighbor] = _sample_neighbors[isample][ineighbor];
            }
        }
        
        // extend neighbors for all points to match the desired max power per cell
        //std::cout << "adjusting extending neighbors!" << std::endl;
        VPS_adjust_extend_neighbors_of_all_points();
        
        //std::cout << "retrieving coefficients!" << std::endl;
        VPS_retrieve_poly_coefficients_for_all_points();
        
        end_time = clock();
        cpu_time = ((double) (end_time - start_time)) / CLOCKS_PER_SEC; total_time += cpu_time;
        
        std::cout << "VPS::    Number of polynomial coeffcients = " << std::fixed << _vps_num_poly_terms << std::endl;
        std::cout << "VPS::    Number of GMRES solves = " << std::fixed << _num_GMRES << std::endl;
        std::cout << "VPS::    VPS Surrogate built in " << std::fixed << cpu_time << " seconds." << std::endl;
        
#ifdef DEBUG_TEST_FUNCTION
        std::vector<double> contours;
        for (size_t i = 0; i < 10; i++) contours.push_back(0.1 * i);
        //isocontouring("test_function_surrogate.ps", true, true, contours);
        isocontouring_solid("surrogate.ps", false, true, contours);
        isocontouring_solid("test_function.ps", true, false, contours);
        plot_neighbors();
#endif
        
        
        return true;
    }
    
    double VPSApproximation::evaluate_surrogate(double* x)
    {
        size_t iclosest = _num_inserted_points;
        double dmin = DBL_MAX;
        for (size_t ipoint = 0; ipoint < _num_inserted_points; ipoint++)
        {
            double dd = 0.0;
            for (size_t idim = 0; idim < _n_dim; idim++)
            {
                double dx = x[idim] - _sample_points[ipoint][idim];
                dd += dx * dx;
            }
            if (dd < dmin)
            {
                dmin = dd;
                iclosest = ipoint;
            }
        }
        
        // shift origin and scale
        for (size_t idim = 0; idim < _n_dim; idim++) x[idim] = (x[idim] - _sample_points[iclosest][idim]) / _vps_dfar[iclosest];
        
        double f_VPS = _fval[iclosest];
        for (size_t i = 0; i < _vps_num_poly_terms; i++)
        {
            double ci = _vps_c[iclosest][i];
            double yi = vec_pow_vec(_n_dim, x, _vps_t[i]);
            f_VPS += ci * yi;
        }
        
        // shift origin and scale back to origin
        for (size_t idim = 0; idim < _n_dim; idim++) x[idim] = (x[idim] * _vps_dfar[iclosest]) + _sample_points[iclosest][idim];
        
        return f_VPS;
    }
    
    void VPSApproximation::retrieve_neighbors(size_t ipoint, bool update_point_neighbors)
    {
        if (_num_inserted_points == 1)
        {
            _sample_vsize[0] = 0.5 * _diag;
            return;
        }
        
        size_t* old_neighbors = _sample_neighbors[ipoint];
        
        size_t* tmp_neighbors = new size_t[_num_inserted_points];
        
        double* tmp_pnt = new double[_n_dim];    // end of spoke
        double* qH = new double[_n_dim];         // mid-ppint
        double* nH = new double[_n_dim];         // normal vector
        
        _sample_vsize[ipoint] = 0.0;
        size_t num_neighbors(0), num_misses(0), max_misses(10);
        
        while (num_misses < max_misses)
        {
            // sample point uniformly from a unit sphere
            double sf = 0.0;
            for (size_t idim = 0; idim < _n_dim; idim++)
            {
                double sum(0.0);
                // select 12 random numbers from 0.0 to 1.0
                for (size_t i = 0; i < 12; i++) sum += generate_a_random_number();
                sum -= 6.0;
                tmp_pnt[idim] = sum;
                sf += tmp_pnt[idim] * tmp_pnt[idim];
            }
            sf = 1.0 / sqrt(sf);
            for (size_t idim = 0; idim < _n_dim; idim++) tmp_pnt[idim] *= sf;
            
            // scale line spoke to extend outside bounding box
            for (size_t idim = 0; idim < _n_dim; idim++)
            {
                tmp_pnt[idim] *= _diag;
                tmp_pnt[idim] += _sample_points[ipoint][idim];
            }
            
            // trim line spoke with domain boundaries
            double t_end(1.0);
            for (size_t idim = 0; idim < _n_dim; idim++)
            {
                if (tmp_pnt[idim] > _xmax[idim])
                {
                    double t = (_xmax[idim] - _sample_points[ipoint][idim]) / (tmp_pnt[idim] - _sample_points[ipoint][idim]);
                    if (t < t_end) t_end = t;
                }
                if (tmp_pnt[idim] < _xmin[idim])
                {
                    double t = (_sample_points[ipoint][idim] - _xmin[idim]) / (_sample_points[ipoint][idim] - tmp_pnt[idim]);
                    if (t < t_end) t_end = t;
                }
            }
            for (size_t idim = 0; idim < _n_dim; idim++) tmp_pnt[idim] = _sample_points[ipoint][idim] + t_end * (tmp_pnt[idim] - _sample_points[ipoint][idim]);
            
            // trim spoke using Voronoi faces
            size_t ineighbor(ipoint);
            for (size_t jpoint = 0; jpoint < _num_inserted_points; jpoint++)
            {
                if (jpoint == ipoint) continue;
             
                // trim line spoke via hyperplane between
                double norm(0.0);
                for (size_t idim = 0; idim < _n_dim; idim++)
                {
                    qH[idim] = 0.5 * (_sample_points[ipoint][idim] + _sample_points[jpoint][idim]);
                    nH[idim] =  _sample_points[jpoint][idim] -  _sample_points[ipoint][idim];
                    
                    norm+= nH[idim] * nH[idim];
                }
                norm = 1.0 / sqrt(norm);
                for (size_t idim = 0; idim < _n_dim; idim++) nH[idim] *= norm;
                
                if (trim_line_using_Hyperplane(_n_dim, _sample_points[ipoint], tmp_pnt, qH, nH))
                {
                    ineighbor = jpoint;
                }
            }
            
            double dst = 0.0;
            for (size_t idim = 0; idim < _n_dim; idim++)
            {
                double dx = _sample_points[ipoint][idim] - tmp_pnt[idim];
                dst += dx * dx;
            }
            dst = sqrt(dst);
            if (dst > _sample_vsize[ipoint]) _sample_vsize[ipoint] = dst;
            
            if (ineighbor == ipoint) continue; // boundary neighbor
            
            // check if new neighbor is found
            bool new_neighbor(true);
            for (size_t i = 0; i < num_neighbors; i++)
            {
                if (tmp_neighbors[i] == ineighbor)
                {
                    new_neighbor = false;
                    break;
                }
            }
            
            if (!new_neighbor)
            {
                // old neighbor = a miss
                num_misses++;
                continue;
            }
            
            double h(0.0);
            for (size_t idim = 0; idim < _n_dim; idim++)
            {
                double dx = _sample_points[ipoint][idim] - _sample_points[ineighbor][idim];
                h += dx * dx;
            }
            h = sqrt(h);
            double grad = fabs(_fval[ipoint] - _fval[ineighbor]) / h;
            
            if (grad > _disc_min_grad) continue; // gradient of two point exceeds threshold
            
            // a hit
            num_misses = 0;
            tmp_neighbors[num_neighbors] = ineighbor;
            num_neighbors++;
            
        } // end of spoke loop
        
        if (old_neighbors != 0)
        {
            delete[] old_neighbors;
        }
        
        _sample_neighbors[ipoint] = new size_t[num_neighbors + 1];
        _sample_neighbors[ipoint][0] = num_neighbors;
        for (size_t i = 0; i < num_neighbors; i++)  _sample_neighbors[ipoint][i + 1] = tmp_neighbors[i];
        
        delete[] tmp_pnt;
        delete[] qH;
        delete[] nH;
        
        if (update_point_neighbors)
        {
            for (size_t i = 0; i < num_neighbors; i++)
            {
                retrieve_neighbors(tmp_neighbors[i], false);
            }
        }
        delete[] tmp_neighbors;
    }
    
    void VPSApproximation::VPS_adjust_extend_neighbors_of_all_points()
    {
        for (size_t ipoint = 0; ipoint < _num_inserted_points; ipoint++)
        {
            while (_vps_ext_neighbors[ipoint][0] < 2 * _vps_num_poly_terms && _vps_ext_neighbors[ipoint][0] < _num_inserted_points - 1)
            {
                VPS_extend_neighbors(ipoint);
            }
        }
        
        for (size_t ipoint = 0; ipoint < _num_inserted_points; ipoint++)
        {
            _vps_dfar[ipoint] = 0.0;
            for (size_t i = 1; i <= _vps_ext_neighbors[ipoint][0]; i++)
            {
                size_t neighbor = _vps_ext_neighbors[ipoint][i];
                double dst = 0.0;
                for (size_t idim = 0; idim < _n_dim; idim++)
                {
                    double dx = _sample_points[ipoint][idim] - _sample_points[neighbor][idim];
                    dst += dx * dx;
                }
                dst = sqrt(dst);
                if (dst > _vps_dfar[ipoint]) _vps_dfar[ipoint] = dst;
            }
        }
    }
    
    void VPSApproximation::VPS_extend_neighbors(size_t ipoint)
    {
        // initiate tmp_neighbors with old extended neighbors
        size_t num_ext_neigbors = _vps_ext_neighbors[ipoint][0];
        size_t* tmp_neighbors = new size_t[_num_inserted_points];
        for (size_t i = 1; i <= _vps_ext_neighbors[ipoint][0]; i++) tmp_neighbors[i - 1] = _vps_ext_neighbors[ipoint][i];
        
        for (size_t i = 1; i <= _vps_ext_neighbors[ipoint][0]; i++)
        {
            size_t neighbor = _vps_ext_neighbors[ipoint][i];
            for (size_t j = 1; j <= _sample_neighbors[neighbor][0]; j++) // extend neighbors of ipoint using direct neighbors of neighbor
            {
                size_t ext_neighbor = _sample_neighbors[neighbor][j];
                if (ext_neighbor == ipoint) continue;
                bool new_neighbor = true;
                for (size_t k= 0; k < num_ext_neigbors; k++)
                {
                    if (ext_neighbor == tmp_neighbors[k])
                    {
                        new_neighbor = false;
                        break;
                    }
                }
                if (new_neighbor)
                {
                    tmp_neighbors[num_ext_neigbors] = ext_neighbor;
                    num_ext_neigbors++;
                }
            }
        }
        
        if (_vps_ext_neighbors[ipoint] != 0) delete[] _vps_ext_neighbors[ipoint];
        _vps_ext_neighbors[ipoint] = new size_t[num_ext_neigbors + 1];
        _vps_ext_neighbors[ipoint][0] = num_ext_neigbors;
        for (size_t i = 0; i < num_ext_neigbors; i++)
        {
            _vps_ext_neighbors[ipoint][i + 1] = tmp_neighbors[i];
        }
        delete[] tmp_neighbors;
    }
    
    void VPSApproximation::VPS_retrieve_poly_coefficients_for_all_points()
    {
        _vps_c = new double*[_num_inserted_points];
        for (size_t ipoint = 0; ipoint < _num_inserted_points; ipoint++)
        {
            _vps_c[ipoint] = new double[_vps_num_poly_terms];

            VPS_retrieve_poly_coefficients(ipoint);
        }
    }
    
    void VPSApproximation::VPS_retrieve_poly_coefficients(size_t ipoint)
    {
        double** A = new double*[_vps_num_poly_terms];
        double** LD = new double*[_vps_num_poly_terms];
        double* b = new double[_vps_num_poly_terms];
        
        double* x_neighbor = new double[_n_dim];
        
        for (size_t q = 0; q < _vps_num_poly_terms; q++)
        {
            A[q] = new double[_vps_num_poly_terms];
            LD[q] = new double[_vps_num_poly_terms];
        }
        
        for (size_t q = 0; q < _vps_num_poly_terms; q++)
        {
            b[q] = 0.0;
            for (size_t i = 0; i < _vps_num_poly_terms; i++)
            {
                A[q][i] = 0.0;
                LD[q][i] = 0.0;
            }
        }

        
        for (size_t j = 1; j <= _vps_ext_neighbors[ipoint][0]; j++) // loop over neighbors
        {
            size_t neighbor = _vps_ext_neighbors[ipoint][j];
            // shift origin
            for (size_t idim = 0; idim < _n_dim; idim++) x_neighbor[idim] = (_sample_points[neighbor][idim] - _sample_points[ipoint][idim]) / _vps_dfar[ipoint];
            
            for (size_t q = 0; q < _vps_num_poly_terms; q++)
            {
                double yq = vec_pow_vec(_n_dim, x_neighbor, _vps_t[q]);
                b[q]+= (_fval[neighbor] - _fval[ipoint]) * yq;
                for (size_t i = 0; i < _vps_num_poly_terms; i++)
                {
                    double yi = vec_pow_vec(_n_dim, x_neighbor, _vps_t[i]);
                    A[q][i] += yi * yq;
                }
            }
        }
               
        if (Cholesky(_vps_num_poly_terms, A, LD))
            Cholesky_solver(_vps_num_poly_terms, LD, b, _vps_c[ipoint]);
        else
        {
            GMRES(_vps_num_poly_terms, A, b, _vps_c[ipoint], 1E-6);
            _num_GMRES++;
        }

        for (size_t q = 0; q < _vps_num_poly_terms; q++)
        {
            delete[] A[q];
            delete[] LD[q];
        }
        
        delete[] A;
        delete[] LD;
        delete[] b;
        delete[] x_neighbor;
    }

    void VPSApproximation::VPS_destroy_global_containers()
    {
        delete[] _xmin;
        delete[] _xmax;
        delete[] _fval;
        delete[] _vps_dfar;
        delete[] _sample_vsize;
        
        for (size_t ipoint = 0; ipoint < _num_inserted_points; ipoint++)
        {
            delete[] _sample_points[ipoint];
            delete[] _sample_neighbors[ipoint];
            delete[] _vps_c[ipoint];
            delete[] _vps_ext_neighbors[ipoint];
        }
        delete[] _sample_points;
        delete[] _sample_neighbors;
        delete[] _vps_c;
        delete[] _vps_ext_neighbors;
        
        for (size_t i = 0; i < _vps_num_poly_terms; i++)
        {
            delete[] _vps_t[i];
        }
        delete[] _vps_t;
    }
    
    void VPSApproximation::retrieve_permutations(size_t &m, size_t** &perm, size_t num_dim, size_t upper_bound, bool include_origin, bool force_sum_constraint, size_t sum_constraint)
    {
        size_t* t = new size_t[num_dim];
        for (size_t idim = 0; idim < num_dim; idim++) t[idim] = 0;
        
        // count output
        m = 0;
        bool origin(true);
        size_t k_dim(num_dim - 1);
        while (true)
        {
            while (t[k_dim] <= upper_bound)
            {
                bool valid(true);
                
                if (!include_origin && origin)
                {
                    valid = false; origin = false;
                }
                if (force_sum_constraint)
                {
                    size_t s_const(0);
                    for (size_t idim = 0; idim < num_dim; idim++) s_const += t[idim];
                    
                    if (s_const > upper_bound)
                    {
                        valid = false; origin = false;
                    }
                }
                if (valid) m++;
                
                t[k_dim]++; // move to the next enumeration
            }
            
            
            size_t kk_dim(k_dim - 1);
            
            bool done(false);
            while (true)
            {
                t[kk_dim]++;
                if (t[kk_dim] > upper_bound)
                {
                    t[kk_dim] = 0;
                    if (kk_dim == 0)
                    {
                        done = true;
                        break;
                    }
                    kk_dim--;
                }
                else break;
            }
            if (done) break;
            t[k_dim] = 0;
        }
        
        //std::cout<< "Number of permutations = " << m << std::endl;
        
        perm = new size_t*[m];
        for (size_t i = 0; i < m; i++)
        {
            perm[i] = new size_t[num_dim];
            for (size_t idim = 0; idim < num_dim; idim++) perm[i][idim] = 0;
        }
        
        // Taxi Counter to evaluate the surrogate
        origin = true;
        for (size_t idim = 0; idim < num_dim; idim++) t[idim] = 0;
        k_dim = num_dim - 1;
        m = 0; // index of alpha
        
        while (true)
        {
            while (t[k_dim] <= upper_bound)
            {
                bool valid(true);
                
                if (!include_origin && origin)
                {
                    valid = false; origin = false;
                }
                if (force_sum_constraint)
                {
                    size_t s_const(0);
                    for (size_t idim = 0; idim < num_dim; idim++) s_const += t[idim];
                    
                    if (s_const > upper_bound)
                    {
                        valid = false; origin = false;
                    }
                }
                if (valid)
                {
                    // store t in t_alpha of counter
                    for (size_t idim = 0; idim < num_dim; idim++) perm[m][idim] = t[idim];
                    
                    //for (size_t idim = 0; idim < num_dim; idim++) std::cout << perm[m][idim] << " ";
                    //std::cout << std::endl;
                    
                    m++;
                }
                
                t[k_dim]++; // move to the next enumeration
            }
            
            
            size_t kk_dim(k_dim - 1);
            
            bool done(false);
            while (true)
            {
                t[kk_dim]++;
                if (t[kk_dim] > upper_bound)
                {
                    t[kk_dim] = 0;
                    if (kk_dim == 0)
                    {
                        done = true;
                        break;
                    }
                    kk_dim--;
                }
                else break;
            }
            if (done) break;
            t[k_dim] = 0;
        }
        
        delete[] t;
    }
    
    double VPSApproximation::vec_pow_vec(size_t num_dim, double* vec_a, size_t* vec_b)
    {
        double ans = 1.0;
        for (size_t idim = 0; idim < num_dim; idim++)
        {
            ans *= pow(vec_a[idim], (int) vec_b[idim]);
        }
        return ans;
    }
    
    bool VPSApproximation::Cholesky(int n, double** A, double** LD)
    {
        for (int i = 0; i < n; i++)
        {
            for (int j = 0; j <= i; j++)
            {
                double sum = 0.0;
                for (int k = 0; k < j; k++) sum += LD[i][k] * LD[j][k];
                
                if (i == j)
                {
                    if (A[i][i] < sum + 1E-8)
                    {
                        return false;
                    }
                    LD[i][j] = sqrt(A[i][i] - sum);
                }
                else        LD[i][j] = 1.0 / LD[j][j] * (A[i][j] - sum);
                
                LD[j][i] = LD[i][j];
            }
        }
        return true;
    }
    
    void VPSApproximation::Cholesky_solver(int n, double** LD, double* b, double* x)
    {
        double* y = new double[n];
        for(int i = 0; i < n; i++)
        {
            double sum = 0.;
            for(int k = 0; k < i; k++) sum+= LD[i][k] * y[k];
            y[i] = (b[i] - sum) / LD[i][i];
        }
        for(int i = n - 1; i >= 0; i--)
        {
            double sum = 0.;
            for(int k = i + 1; k < n; ++k) sum += LD[i][k] * x[k];
            x[i] = (y[i] - sum) / LD[i][i];
        }
        delete[] y;
    }
    
    void VPSApproximation::GMRES(size_t n, double** A, double* b, double* x, double eps)
    {
        size_t im = 0;
        
        for (size_t i = 0; i < n; i++) x[i] = 0.0;
        
        double* r = new double[n];
        
        double* rr = new double[n + 1];
        double* bh = new double[n + 1];
        double* c = new double[n + 1];
        double* s = new double[n + 1];
        
        double* w = new double[n];
        
        double** v = new double*[n + 1];
        for (size_t i = 0; i < n + 1; i++) v[i] = new double[n];
        double** HT = new double*[n];
        double** RT = new double*[n];
        double** QT = new double*[n];
        for (im = 0; im < n; im++)
        {
            HT[im] = new double[im + 2];
            RT[im] = new double[im + 1];
            QT[im] = new double[im + 2];
        }
        
        double ro = 0.0;
        double rNorm = 0.0;
        
        // Calculating residual:
        double beta = 0.0;
        for (size_t i = 0; i < n; i++)
        {
            r[i] = b[i];
            beta += r[i] * r[i];
        }
        beta = sqrt(beta);
        
        rNorm = beta;
        ro = beta;
        
        for (size_t i = 0; i < n + 1; i++)
        {
            rr[i] = 0.0;
            bh[i] = 0.0;
            c[i] = 0.0;
            s[i] = 0.0;
        }
        rr[0] = 1.0;
        bh[0] = beta;
        
        // Calculating vo:
        v[0] = new double[n];
        for (size_t i = 0; i < n; i++)
        {
            v[0][i] = r[i] / beta;
        }
        
        for (im = 0; im < n; im++)
        {
            // Calculating w = A v_i :
            
            double wNorm = 0.0;
            for (size_t i = 0; i < n; i++)
            {
                w[i] = 0.0;
                for (size_t j = 0; j < n; j++)
                {
                    w[i] += A[i][j] * v[im][j];
                }
            }
            
            // Updating HT:
            for (size_t ik = 0; ik <= im; ik++)
            {
                HT[im][ik] = 0.0;
                for (size_t j = 0; j < n; j++)
                {
                    HT[im][ik] += v[ik][j] * w[j];
                }
                for (size_t j = 0; j < n; j++)
                {
                    w[j] -= HT[im][ik] * v[ik][j];
                }
            }
            
            wNorm = 0.0;
            for (size_t i = 0; i < n; i++)
            {
                wNorm += w[i] * w[i];
            }
            wNorm = sqrt(wNorm);
            HT[im][im + 1] = wNorm;
            
            for (size_t i = 0; i < n; i++)
            {
                v[im + 1][i] = w[i] / wNorm;
            }
            
            // Updating RT:
            RT[im][0] = HT[im][0];
            for (size_t ik = 1; ik <= im; ik++)
            {
                double gamma = c[ik - 1] * RT[im][ik - 1] + s[ik - 1] * HT[im][ik];
                RT[im][ik] = -s[ik - 1] * RT[im][ik - 1] + c[ik - 1] * HT[im][ik];
                RT[im][ik - 1] = gamma;
            }
            
            double delta = sqrt(pow(RT[im][im], 2) + pow(HT[im][im + 1], 2));
            c[im] = RT[im][im] / delta;
            s[im] = HT[im][im + 1] / delta;
            RT[im][im] = c[im] * RT[im][im] + s[im] * HT[im][im + 1];
            
            
            // Updating QT:
            QT[im][im + 1] = HT[im][im + 1] / RT[im][im];
            size_t ik = im;
            while (true)
            {
                double sum = 0.0;
                for (size_t j = 0; j < im; j++)
                {
                    if (ik >= j + 2)
                        continue;
                    
                    sum += QT[j][ik] * RT[im][j];
                }
                QT[im][ik] = (HT[im][ik] - sum) / RT[im][im];
                if (ik == 0) break;
                ik--;
            }
            
            // Calculating new Norm:
            rNorm = 0.0;
            for (size_t i = 0; i < im + 2; i++)
            {
                rr[i] -= QT[im][0] * QT[im][i];
                rNorm += rr[i] * rr[i];
            }
            rNorm = sqrt(rNorm);
            
            if (rNorm < eps * ro && rNorm < eps)
                break;
        }
        
        // Calculating y and x :
        if (im == n) im--;
        
        double* yy = new double[im + 1];
        for (size_t j = 0; j < im + 1; j++) yy[j] = beta * QT[j][0];
        
        
        size_t nn = im + 1;
        double* y = new double[nn];
        y[nn - 1] = yy[nn - 1] / RT[nn - 1][nn - 1];
        
        size_t i = nn - 2;
        while (true)
        {
            double ss = yy[i];
            for (size_t j = i + 1; j < nn; j++)
            {
                ss -= RT[j][i] * y[j];
            }
            y[i] = ss / RT[i][i];
            if (i == 0) break;
            i--;
        }
        for (size_t i = 0; i < n; i++)
        {
            for (size_t j = 0; j < nn; j++)
            {
                x[i] += v[j][i] * y[j];
            }
        }
        
        delete[] yy;
        delete[] y;
        
        delete[] r;
        delete[] rr;
        delete[] bh;
        delete[] c;
        delete[] s;
        
        delete[] w;
        
        for (size_t im = 0; im < n + 1; im++) delete[] v[im];
        delete[] v;
        
        for (size_t im = 0; im < n; im++)
        {
            delete[] HT[im];
            delete[] RT[im];
            delete[] QT[im];
        }
        delete[] HT;
        delete[] RT;
        delete[] QT;
    }
    
    double VPSApproximation::f_test(double* x)
    {
        // step function in sphere
        double h = 0.0;
        for (size_t idim = 0; idim < _n_dim; idim++)
        {
            double dx = x[idim];
            h += dx * dx;
        }
        h = sqrt(h);
        if (h < 0.5) return 1.0;
        return 0.0;
    }
    
    void VPSApproximation::isocontouring(std::string file_name, bool plot_test_function, bool plot_surrogate, std::vector<double> contours)
    {
        std::fstream file(file_name.c_str(), std::ios::out);
        file << "%!PS-Adobe-3.0" << std::endl;
        file << "72 72 scale     % one unit = one inch" << std::endl;
        
        double xmin(_xmin[0]);
        double ymin(_xmin[1]);
        double Lx(_xmax[0] - _xmin[0]);
        double Ly(_xmax[1] - _xmin[0]);
        
        double scale_x, scale_y, scale;
        double shift_x, shift_y;
        
        scale_x = 6.5 / Lx;
        scale_y = 9.0 / Ly;
        
        if (scale_x < scale_y)
        {
            scale = scale_x;
            shift_x = 1.0 - xmin * scale;
            shift_y = 0.5 * (11.0 - Ly * scale) - ymin * scale;
        }
        else
        {
            scale = scale_y;
            shift_x = 0.5 * (8.5 - Lx * scale) - xmin * scale;
            shift_y = 1.0 - ymin * scale;
        }
        file << shift_x << " " << shift_y << " translate" << std::endl;
        
        
        file << "/redseg      % stack: x1 y1 x2 y2" << std::endl;
        file << "{newpath" << std::endl;
        file << " moveto" << std::endl;
        file << " lineto" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " grestore" << std::endl;
        file << " 1 0 0 setrgbcolor" << std::endl;
        file << " 0.01 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/greenseg      % stack: x1 y1 x2 y2" << std::endl;
        file << "{newpath" << std::endl;
        file << " moveto" << std::endl;
        file << " lineto" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 1 0 setrgbcolor" << std::endl;
        file << " 0.01 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        
        file << "/blueseg      % stack: x1 y1 x2 y2" << std::endl;
        file << "{newpath" << std::endl;
        file << " moveto" << std::endl;
        file << " lineto" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 0 1 setrgbcolor" << std::endl;
        file << " 0.02 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/blackquad      % stack: x1 y1 x2 y2 x3 y3 x4 y4" << std::endl;
        file << "{newpath" << std::endl;
        file << " moveto" << std::endl;
        file << " lineto" << std::endl;
        file << " lineto" << std::endl;
        file << " lineto" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 0 0 setrgbcolor" << std::endl;
        file << " 0.02 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/circ    % stack: x y r" << std::endl;
        file << "{0 360 arc" << std::endl;
        file << " closepath" << std::endl;
        file << " 0.002 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/blackfcirc    % stack: x y r" << std::endl;
        file << "{0 360 arc" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " 0 0 0 setrgbcolor" << std::endl;
        file << " fill" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 0 0 setrgbcolor" << std::endl;
        file << " 0.0 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/redfcirc    % stack: x y r" << std::endl;
        file << "{0 360 arc" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " 1 0 0 setrgbcolor" << std::endl;
        file << " fill" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 0 0 setrgbcolor" << std::endl;
        file << " 0.0 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/bluefcirc    % stack: x y r" << std::endl;
        file << "{0 360 arc" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " 0 0 1 setrgbcolor" << std::endl;
        file << " fill" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 0 0 setrgbcolor" << std::endl;
        file << " 0.0 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/greenfcirc    % stack: x y r" << std::endl;
        file << "{0 360 arc" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " 0 1 0 setrgbcolor" << std::endl;
        file << " fill" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 0 0 setrgbcolor" << std::endl;
        file << " 0.0 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        
        file << "/quad_white      % stack: x1 y1 x2 y2 x3 y3 x4 y4" << std::endl;
        file << "{newpath" << std::endl;
        file << " moveto" << std::endl;
        file << " lineto" << std::endl;
        file << " lineto" << std::endl;
        file << " lineto" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " 1.0 setgray fill" << std::endl;
        file << " grestore" << std::endl;
        file << "} def" << std::endl;
        
        file << "/quad_bold      % stack: x1 y1 x2 y2 x3 y3 x4 y4" << std::endl;
        file << "{newpath" << std::endl;
        file << " moveto" << std::endl;
        file << " lineto" << std::endl;
        file << " lineto" << std::endl;
        file << " lineto" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 0 0 setrgbcolor" << std::endl;
        file << " 0.01 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        double* xx = new double[2];
        double sx = 0.001 * (_xmax[0] - _xmin[0]);
        double sy = 0.001 * (_xmax[1] - _xmin[1]);
        for (size_t i = 0; i < 1000; i++)
        {
            double xo = _xmin[0] + i * sx;
            for (size_t j = 0; j < 1000; j++)
            {
                for (size_t ifunc = 0; ifunc < 2; ifunc++)
                {
                    double fo(0.0), f1(0.0), f2(0.0), f3(0.0);
                
                    double yo = _xmin[1] + j * sy;
                    xx[0] = xo; xx[1] = yo;
                    if (plot_test_function && ifunc != 1)  fo = f_test(xx);
                    else if (plot_surrogate) fo = evaluate_surrogate(xx);
                    xx[0] = xo+sx; xx[1] = yo;
                    if (plot_test_function && ifunc != 1)  f1 = f_test(xx);
                    else if (plot_surrogate) f1 = evaluate_surrogate(xx);
                    xx[0] = xo + sx; xx[1] = yo + sy;
                    if (plot_test_function && ifunc != 1)  f2 = f_test(xx);
                    else if (plot_surrogate) f2 = evaluate_surrogate(xx);
                    xx[0] = xo; xx[1] = yo + sy;
                    if (plot_test_function && ifunc != 1)  f3 = f_test(xx);
                    else if (plot_surrogate) f3 = evaluate_surrogate(xx);
                
                    size_t num_isocontours = contours.size();
                    for (size_t icont = 0; icont < num_isocontours; icont++)
                    {
                        double contour = contours[icont];
                    
                        size_t num_points(0);
                        double x1, y1, x2, y2;

                        if ((fo > contour && f1 < contour) || (fo < contour && f1 > contour))
                        {
                            double h = sx * (contour - fo) / (f1 - fo);
                            x1 = xo + h;
                            y1 = yo; num_points++;
                        }
                        if ((f1 > contour && f2 < contour) || (f1 < contour && f2 > contour))
                        {
                            double h = sy * (contour - f1) / (f2 - f1);
                            if (num_points == 0)
                            {
                                x1 = xo + sx;
                                y1 = yo + h;
                            }
                            else
                            {
                                x2 = xo + sx;
                                y2 = yo + h;
                            }
                            num_points++;
                        }
                        if ((f2 > contour && f3 < contour) || (f2 < contour && f3 > contour))
                        {
                            double h = sx * (contour - f2) / (f3 - f2);
                            if (num_points == 0)
                            {
                                x1 = xo + sx - h;
                                y1 = yo + sy;
                            }
                            else
                            {
                                x2 = xo + sx - h;
                                y2 = yo + sy;
                            }
                            num_points++;
                        }
                        if ((f3 > contour && fo < contour) || (f3 < contour && fo > contour))
                        {
                            double h = sy * (contour - f3) / (fo - f3);
                            if (num_points == 0)
                            {
                                x1 = xo;
                                y1 = yo + sy - h;
                            }
                            else
                            {
                                x2 = xo;
                                y2 = yo + sy - h;
                            }
                            num_points++;
                        }
                    
                        if (num_points == 2)
                        {
                            if (ifunc == 0)
                            {
                                file << "newpath" << std::endl;
                                file << x1 * scale << " " << y1 * scale << " moveto" << std::endl;
                                file << x2 * scale << " " << y2 * scale << " lineto" << std::endl;
                                file << "closepath" << std::endl;
                                file << "gsave" << std::endl;
                                file << "grestore" << std::endl;
                                file << "0 0 0" << " setrgbcolor" << std::endl;
                                file << "0.02 setlinewidth" << std::endl;
                                file << "stroke" << std::endl;
                            }
                            else
                            {
                                double gs = icont * 1.0 / num_isocontours;
                                file << "newpath" << std::endl;
                                file << x1 * scale << " " << y1 * scale << " moveto" << std::endl;
                                file << x2 * scale << " " << y2 * scale << " lineto" << std::endl;
                                file << "closepath" << std::endl;
                                file << "gsave" << std::endl;
                                file << "grestore" << std::endl;
                                if (gs < 0.5)
                                {
                                    file << 1.0 - 2 * gs << " " << 2 * gs << " " << 0.0 << " setrgbcolor" << std::endl;
                                }
                                else
                                {
                                    file << 0.0 << " " << 2.0 - 2 * gs << " " << 2 * gs - 1.0 << " setrgbcolor" << std::endl;
                                }
                                file << "0.02 setlinewidth" << std::endl;
                                file << "stroke" << std::endl;
                            }
                        }
                    }
                }
            }
        }
        
        delete[] xx;
        
        double DX = _xmax[0] - _xmin[0];
        double DY = _xmax[1] - _xmin[1];
        
        // plot domain boundaries
        file << (_xmin[0] - DX) * scale << "  "  << _xmin[1]        * scale << "  ";
        file << (_xmax[0] + DX) * scale << "  "  <<  _xmin[1]       * scale << "  ";
        file << (_xmax[0] + DX) * scale << "  "  << (_xmin[1] - DY) * scale << "  ";
        file << (_xmin[0] - DX) * scale << "  "  << (_xmin[1] - DY) * scale << "  ";
        file << "quad_white"      << std::endl;
        
        file << (_xmin[0] - DX) * scale << "  "  <<  _xmax[1]        * scale << "  ";
        file << (_xmax[0] + DX) * scale << "  "  <<  _xmax[1]        * scale << "  ";
        file << (_xmax[0] + DX) * scale << "  "  <<  (_xmax[1] + DY) * scale << "  ";
        file << (_xmin[0] - DX) * scale << "  "  <<  (_xmax[1] + DY) * scale << "  ";
        file << "quad_white"      << std::endl;
        
        file <<  _xmax[0]       * scale << "  "  <<  (_xmin[1] - DY) * scale << "  ";
        file << (_xmax[0] + DX) * scale << "  "  <<  (_xmin[1] - DY) * scale << "  ";
        file << (_xmax[0] + DX) * scale << "  "  <<  (_xmax[1] + DY) * scale << "  ";
        file <<  _xmax[0]       * scale << "  "  <<  (_xmax[1] + DY) * scale << "  ";
        file << "quad_white"      << std::endl;
        
        file << (_xmin[0] - DX) * scale << "  "  <<  (_xmin[1] - DY) * scale << "  ";
        file <<  _xmin[0]       * scale << "  "  <<  (_xmin[1] - DY) * scale << "  ";
        file <<  _xmin[0]       * scale << "  "  <<  (_xmax[1] + DY) * scale << "  ";
        file << (_xmin[0] - DX) * scale << "  "  <<  (_xmax[1] + DY) * scale << "  ";
        file << "quad_white"      << std::endl;
        
        
        // plot domain boundaries
        file << _xmin[0] * scale << "  " << _xmin[1] * scale << "  ";
        file << _xmax[0] * scale << "  " << _xmin[1] * scale << "  ";
        file << _xmax[0] * scale << "  " << _xmax[1] * scale << "  ";
        file << _xmin[0] * scale << "  " << _xmax[1] * scale << "  ";
        file << "quad_bold"      << std::endl;
        
        file << "showpage" << std::endl;
        
    }
    
    void VPSApproximation::isocontouring_solid(std::string file_name, bool plot_test_function, bool plot_surrogate, std::vector<double> contours)
    {
        std::fstream file(file_name.c_str(), std::ios::out);
        file << "%!PS-Adobe-3.0" << std::endl;
        file << "72 72 scale     % one unit = one inch" << std::endl;
        
        double xmin(_xmin[0]);
        double ymin(_xmin[1]);
        double Lx(_xmax[0] - _xmin[0]);
        double Ly(_xmax[1] - _xmin[0]);
        
        double scale_x, scale_y, scale;
        double shift_x, shift_y;
        
        scale_x = 6.5 / Lx;
        scale_y = 9.0 / Ly;
        
        if (scale_x < scale_y)
        {
            scale = scale_x;
            shift_x = 1.0 - xmin * scale;
            shift_y = 0.5 * (11.0 - Ly * scale) - ymin * scale;
        }
        else
        {
            scale = scale_y;
            shift_x = 0.5 * (8.5 - Lx * scale) - xmin * scale;
            shift_y = 1.0 - ymin * scale;
        }
        file << shift_x << " " << shift_y << " translate" << std::endl;
        
        
        file << "/redseg      % stack: x1 y1 x2 y2" << std::endl;
        file << "{newpath" << std::endl;
        file << " moveto" << std::endl;
        file << " lineto" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " grestore" << std::endl;
        file << " 1 0 0 setrgbcolor" << std::endl;
        file << " 0.01 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/greenseg      % stack: x1 y1 x2 y2" << std::endl;
        file << "{newpath" << std::endl;
        file << " moveto" << std::endl;
        file << " lineto" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 1 0 setrgbcolor" << std::endl;
        file << " 0.01 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        
        file << "/blueseg      % stack: x1 y1 x2 y2" << std::endl;
        file << "{newpath" << std::endl;
        file << " moveto" << std::endl;
        file << " lineto" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 0 1 setrgbcolor" << std::endl;
        file << " 0.02 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/blackquad      % stack: x1 y1 x2 y2 x3 y3 x4 y4" << std::endl;
        file << "{newpath" << std::endl;
        file << " moveto" << std::endl;
        file << " lineto" << std::endl;
        file << " lineto" << std::endl;
        file << " lineto" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 0 0 setrgbcolor" << std::endl;
        file << " 0.02 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/circ    % stack: x y r" << std::endl;
        file << "{0 360 arc" << std::endl;
        file << " closepath" << std::endl;
        file << " 0.002 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/blackfcirc    % stack: x y r" << std::endl;
        file << "{0 360 arc" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " 0 0 0 setrgbcolor" << std::endl;
        file << " fill" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 0 0 setrgbcolor" << std::endl;
        file << " 0.0 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/redfcirc    % stack: x y r" << std::endl;
        file << "{0 360 arc" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " 1 0 0 setrgbcolor" << std::endl;
        file << " fill" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 0 0 setrgbcolor" << std::endl;
        file << " 0.0 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/bluefcirc    % stack: x y r" << std::endl;
        file << "{0 360 arc" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " 0 0 1 setrgbcolor" << std::endl;
        file << " fill" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 0 0 setrgbcolor" << std::endl;
        file << " 0.0 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/greenfcirc    % stack: x y r" << std::endl;
        file << "{0 360 arc" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " 0 1 0 setrgbcolor" << std::endl;
        file << " fill" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 0 0 setrgbcolor" << std::endl;
        file << " 0.0 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        
        file << "/quad_white      % stack: x1 y1 x2 y2 x3 y3 x4 y4" << std::endl;
        file << "{newpath" << std::endl;
        file << " moveto" << std::endl;
        file << " lineto" << std::endl;
        file << " lineto" << std::endl;
        file << " lineto" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " 1.0 setgray fill" << std::endl;
        file << " grestore" << std::endl;
        file << "} def" << std::endl;
        
        file << "/quad_bold      % stack: x1 y1 x2 y2 x3 y3 x4 y4" << std::endl;
        file << "{newpath" << std::endl;
        file << " moveto" << std::endl;
        file << " lineto" << std::endl;
        file << " lineto" << std::endl;
        file << " lineto" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 0 0 setrgbcolor" << std::endl;
        file << " 0.01 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        std::vector<double> poly_x;
        std::vector<double> poly_y;
        
        contours.push_back(100.00);
        
        size_t num_cells(1000);
        double* xx = new double[2];
        double sx = 1.0 * (_xmax[0] - _xmin[0]) / num_cells;
        double sy = 1.0 * (_xmax[1] - _xmin[1]) / num_cells;
        for (size_t i = 0; i < num_cells; i++)
        {
            double xo = _xmin[0] + i * sx;
            for (size_t j = 0; j < num_cells; j++)
            {
                double fo(0.0), f1(0.0), f2(0.0), f3(0.0);
                
                double yo = _xmin[1] + j * sy;
                xx[0] = xo; xx[1] = yo;
                if (plot_test_function)  fo = f_test(xx);
                else                     fo = evaluate_surrogate(xx);
                xx[0] = xo+sx; xx[1] = yo;
                if (plot_test_function)  f1 = f_test(xx);
                else                     f1 = evaluate_surrogate(xx);
                xx[0] = xo + sx; xx[1] = yo + sy;
                if (plot_test_function)  f2 = f_test(xx);
                else                     f2 = evaluate_surrogate(xx);
                xx[0] = xo; xx[1] = yo + sy;
                if (plot_test_function)  f3 = f_test(xx);
                else                     f3 = evaluate_surrogate(xx);
                
                //std::cout << "fo = " << fo << " , f1 = " << f1 << " , f2 = " << f2 << " , f3 = " << f3 << std::endl;
                
                size_t num_isocontours = contours.size();
                for (size_t icont = 0; icont < num_isocontours; icont++)
                {
                    double contour = contours[icont];
                    double contour_m = - 1000.00;
                    if (icont > 0) contour_m = contours[icont - 1];
                    
                    //std::cout<< "contour_m = " << contour_m << " , contour = " << contour << std::endl;
                    
                    poly_x.clear(); poly_y.clear();
                    
                    // moving right
                    if (fo >= contour_m - 1E-10 && fo < contour + 1E-10)
                    {
                        poly_x.push_back(xo);
                        poly_y.push_back(yo);
                        if ((fo > contour && f1 < contour) || (fo < contour && f1 > contour))
                        {
                            double h = sx * (contour - fo) / (f1 - fo);
                            poly_x.push_back(xo + h);
                            poly_y.push_back(yo);
                        }
                        else if ((fo > contour_m && f1 < contour_m) || (fo < contour_m && f1 > contour_m))
                        {
                            double h = sx * (contour_m - fo) / (f1 - fo);
                            poly_x.push_back(xo + h);
                            poly_y.push_back(yo);
                        }
                    }
                    else if ((fo > contour_m && f1 < contour_m) || (fo < contour_m && f1 > contour_m))
                    {
                        double hm = sx * (contour_m - fo) / (f1 - fo);
                        double h = hm;
                        if ((fo > contour && f1 < contour) || (fo < contour && f1 > contour))
                        {
                            h = sx * (contour - fo) / (f1 - fo);
                        }
                        if (h < hm)
                        {
                            double tmp = h; h = hm; hm = tmp;
                        }
                        poly_x.push_back(xo + hm);
                        poly_y.push_back(yo);
                        
                        if (h - hm > 1E-10)
                        {
                            poly_x.push_back(xo + h);
                            poly_y.push_back(yo);
                        }
                    }
                    else if ((fo > contour && f1 < contour) || (fo < contour && f1 > contour))
                    {
                        double h = sx * (contour - fo) / (f1 - fo);
                        poly_x.push_back(xo + h);
                        poly_y.push_back(yo);
                    }
                    
                    // moving up
                    if (f1 >= contour_m - 1E-10 && f1 < contour + 1E-10)
                    {
                        poly_x.push_back(xo + sx);
                        poly_y.push_back(yo);
                        if ((f1 > contour && f2 < contour) || (f1 < contour && f2 > contour))
                        {
                            double h = sy * (contour - f1) / (f2 - f1);
                            poly_x.push_back(xo + sx);
                            poly_y.push_back(yo + h);
                        }
                        else if ((f1 > contour_m && f2 < contour_m) || (f1 < contour_m && f2 > contour_m))
                        {
                            double h = sy * (contour_m - f1) / (f2 - f1);
                            poly_x.push_back(xo + sx);
                            poly_y.push_back(yo + h);
                        }

                    }
                    else if ((f1 > contour_m && f2 < contour_m) || (f1 < contour_m && f2 > contour_m))
                    {
                        double hm = sy * (contour_m - f1) / (f2 - f1);
                        double h = hm;
                        if ((f1 > contour && f2 < contour) || (f1 < contour && f2 > contour))
                        {
                            h = sy * (contour - f1) / (f2 - f1);
                        }
                        if (h < hm)
                        {
                            double tmp = h; h = hm; hm = tmp;
                        }
                        poly_x.push_back(xo + sx);
                        poly_y.push_back(yo + hm);

                        if (h - hm > 1E-10)
                        {
                            poly_x.push_back(xo + sx);
                            poly_y.push_back(yo + h);
                        }
                    }
                    else if ((f1 > contour && f2 < contour) || (f1 < contour && f2 > contour))
                    {
                        double h = sy * (contour - f1) / (f2 - f1);
                        poly_x.push_back(xo + sx);
                        poly_y.push_back(yo + h);
                    }
                    
                    // moving left
                    if (f2 >= contour_m - 1E-10 && f2 < contour + 1E-10)
                    {
                        poly_x.push_back(xo + sx);
                        poly_y.push_back(yo + sy);
                        if ((f2 > contour && f3 < contour) || (f2 < contour && f3 > contour))
                        {
                            double h = sx * (contour - f2) / (f3 - f2);
                            poly_x.push_back(xo + sx - h);
                            poly_y.push_back(yo + sy);
                        }
                        else if ((f2 > contour_m && f3 < contour_m) || (f2 < contour_m && f3 > contour_m))
                        {
                            double h = sx * (contour_m - f2) / (f3 - f2);
                            poly_x.push_back(xo + sx - h);
                            poly_y.push_back(yo + sy);
                        }
                    }
                    else if ((f2 > contour_m && f3 < contour_m) || (f2 < contour_m && f3 > contour_m))
                    {
                        double hm = sx * (contour_m - f2) / (f3 - f2);
                        double h = hm;
                        if ((f2 > contour && f3 < contour) || (f2 < contour && f3 > contour))
                        {
                            h = sx * (contour - f2) / (f3 - f2);
                        }
                        if (h < hm)
                        {
                            double tmp = h; h = hm; hm = tmp;
                        }
                        poly_x.push_back(xo + sx - hm);
                        poly_y.push_back(yo + sy);
                        
                        if (h - hm > 1E-10)
                        {
                            poly_x.push_back(xo + sx - h);
                            poly_y.push_back(yo + sy);
                        }
                    }
                    else if ((f2 > contour && f3 < contour) || (f2 < contour && f3 > contour))
                    {
                        double h = sx * (contour - f2) / (f3 - f2);
                        poly_x.push_back(xo + sx - h);
                        poly_y.push_back(yo + sy);
                    }
                    
                    // moving down
                    if (f3 >= contour_m - 1E-10 && f3 < contour + 1E-10)
                    {
                        poly_x.push_back(xo);
                        poly_y.push_back(yo + sy);
                        if ((f3 > contour && fo < contour) || (f3 < contour && fo > contour))
                        {
                            double h = sy * (contour - f3) / (fo - f3);
                            poly_x.push_back(xo);
                            poly_y.push_back(yo + sy - h);
                        }
                        else if ((f3 > contour_m && fo < contour_m) || (f3 < contour_m && fo > contour_m))
                        {
                            double h = sy * (contour_m - f3) / (fo - f3);
                            poly_x.push_back(xo);
                            poly_y.push_back(yo + sy - h);
                        }
                    }
                    else if ((f3 > contour_m && fo < contour_m) || (f3 < contour_m && fo > contour_m))
                    {
                        double hm = sy * (contour_m - f3) / (fo - f3);
                        double h = hm;
                        if ((f3 > contour && fo < contour) || (f3 < contour && fo > contour))
                        {
                            h = sy * (contour - f3) / (fo - f3);
                        }
                        if (h < hm)
                        {
                            double tmp = h; h = hm; hm = tmp;
                        }
                        poly_x.push_back(xo);
                        poly_y.push_back(yo + sy - hm);
                        
                        if (h - hm > 1E-10)
                        {
                            poly_x.push_back(xo);
                            poly_y.push_back(yo + sy - h);
                        }
                    }
                    else if ((f3 > contour && fo < contour) || (f3 < contour && fo > contour))
                    {
                        double h = sy * (contour - f3) / (fo - f3);
                        poly_x.push_back(xo);
                        poly_y.push_back(yo + sy - h);
                    }
                    
                    
                    size_t num_corners(poly_x.size());
                    if (num_corners > 1)
                    {
                        double gs = 1.0 - icont * 1.0 / num_isocontours;
                        file << "newpath" << std::endl;
                        file << poly_x[0] * scale << " " << poly_y[0] * scale << " moveto" << std::endl;
                        //std::cout<< "*** x = " <<  poly_x[0] << ", y = " << poly_y[0] << std::endl;
                        for (size_t icorner = 1; icorner < num_corners; icorner++)
                        {
                            file << poly_x[icorner] * scale << " " << poly_y[icorner] * scale << " lineto" << std::endl;
                            //std::cout << "*** x = " <<  poly_x[icorner] << ", y = " << poly_y[icorner] << std::endl;
                        }
                        //std::cout << std::endl;
                        
                        file << "closepath" << std::endl;
                        file << "gsave" << std::endl;
                        file << "grestore" << std::endl;
                        if (gs < 0.5)
                        {
                            file << 1.0 - 2 * gs << " " << 2 * gs << " " << 0.0 << " setrgbcolor" << std::endl;
                        }
                        else
                        {
                            file << 0.0 << " " << 2.0 - 2 * gs << " " << 2 * gs - 1.0 << " setrgbcolor" << std::endl;
                        }
                        file << " fill" << std::endl;
                    }
                }
            }
        }
        delete[] xx;
        
        double DX = _xmax[0] - _xmin[0];
        double DY = _xmax[1] - _xmin[1];
        
        // plot domain boundaries
        file << (_xmin[0] - DX) * scale << "  "  << _xmin[1]        * scale << "  ";
        file << (_xmax[0] + DX) * scale << "  "  <<  _xmin[1]       * scale << "  ";
        file << (_xmax[0] + DX) * scale << "  "  << (_xmin[1] - DY) * scale << "  ";
        file << (_xmin[0] - DX) * scale << "  "  << (_xmin[1] - DY) * scale << "  ";
        file << "quad_white"      << std::endl;
        
        file << (_xmin[0] - DX) * scale << "  "  <<  _xmax[1]        * scale << "  ";
        file << (_xmax[0] + DX) * scale << "  "  <<  _xmax[1]        * scale << "  ";
        file << (_xmax[0] + DX) * scale << "  "  <<  (_xmax[1] + DY) * scale << "  ";
        file << (_xmin[0] - DX) * scale << "  "  <<  (_xmax[1] + DY) * scale << "  ";
        file << "quad_white"      << std::endl;
        
        file <<  _xmax[0]       * scale << "  "  <<  (_xmin[1] - DY) * scale << "  ";
        file << (_xmax[0] + DX) * scale << "  "  <<  (_xmin[1] - DY) * scale << "  ";
        file << (_xmax[0] + DX) * scale << "  "  <<  (_xmax[1] + DY) * scale << "  ";
        file <<  _xmax[0]       * scale << "  "  <<  (_xmax[1] + DY) * scale << "  ";
        file << "quad_white"      << std::endl;
        
        file << (_xmin[0] - DX) * scale << "  "  <<  (_xmin[1] - DY) * scale << "  ";
        file <<  _xmin[0]       * scale << "  "  <<  (_xmin[1] - DY) * scale << "  ";
        file <<  _xmin[0]       * scale << "  "  <<  (_xmax[1] + DY) * scale << "  ";
        file << (_xmin[0] - DX) * scale << "  "  <<  (_xmax[1] + DY) * scale << "  ";
        file << "quad_white"      << std::endl;
        
        
        // plot domain boundaries
        file << _xmin[0] * scale << "  " << _xmin[1] * scale << "  ";
        file << _xmax[0] * scale << "  " << _xmin[1] * scale << "  ";
        file << _xmax[0] * scale << "  " << _xmax[1] * scale << "  ";
        file << _xmin[0] * scale << "  " << _xmax[1] * scale << "  ";
        file << "quad_bold"      << std::endl;
        
        file << "showpage" << std::endl;
        
    }
    
    
    void VPSApproximation::plot_neighbors()
    {
        std::stringstream ss;
        ss << "vps_neighbors.ps";
        std::fstream file(ss.str().c_str(), std::ios::out);
        file << "%!PS-Adobe-3.0" << std::endl;
        file << "72 72 scale     % one unit = one inch" << std::endl;
        
        double xmin(_xmin[0]);
        double ymin(_xmin[1]);
        double Lx(_xmax[0] - _xmin[0]);
        double Ly(_xmax[1] - _xmin[0]);
        
        double scale_x, scale_y, scale;
        double shift_x, shift_y;
        
        scale_x = 6.5 / Lx;
        scale_y = 9.0 / Ly;
        
        if (scale_x < scale_y)
        {
            scale = scale_x;
            shift_x = 1.0 - xmin * scale;
            shift_y = 0.5 * (11.0 - Ly * scale) - ymin * scale;
        }
        else
        {
            scale = scale_y;
            shift_x = 0.5 * (8.5 - Lx * scale) - xmin * scale;
            shift_y = 1.0 - ymin * scale;
        }
        file << shift_x << " " << shift_y << " translate" << std::endl;
        
        
        file << "/redseg      % stack: x1 y1 x2 y2" << std::endl;
        file << "{newpath" << std::endl;
        file << " moveto" << std::endl;
        file << " lineto" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " grestore" << std::endl;
        file << " 1 0 0 setrgbcolor" << std::endl;
        file << " 0.01 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/greenseg      % stack: x1 y1 x2 y2" << std::endl;
        file << "{newpath" << std::endl;
        file << " moveto" << std::endl;
        file << " lineto" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 1 0 setrgbcolor" << std::endl;
        file << " 0.01 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        
        file << "/blueseg      % stack: x1 y1 x2 y2" << std::endl;
        file << "{newpath" << std::endl;
        file << " moveto" << std::endl;
        file << " lineto" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 0 1 setrgbcolor" << std::endl;
        file << " 0.005 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/blackquad      % stack: x1 y1 x2 y2 x3 y3 x4 y4" << std::endl;
        file << "{newpath" << std::endl;
        file << " moveto" << std::endl;
        file << " lineto" << std::endl;
        file << " lineto" << std::endl;
        file << " lineto" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 0 0 setrgbcolor" << std::endl;
        file << " 0.02 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/circ    % stack: x y r" << std::endl;
        file << "{0 360 arc" << std::endl;
        file << " closepath" << std::endl;
        file << " 0.002 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/blackfcirc    % stack: x y r" << std::endl;
        file << "{0 360 arc" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " 0 0 0 setrgbcolor" << std::endl;
        file << " fill" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 0 0 setrgbcolor" << std::endl;
        file << " 0.0 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/redfcirc    % stack: x y r" << std::endl;
        file << "{0 360 arc" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " 1 0 0 setrgbcolor" << std::endl;
        file << " fill" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 0 0 setrgbcolor" << std::endl;
        file << " 0.0 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/bluefcirc    % stack: x y r" << std::endl;
        file << "{0 360 arc" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " 0 0 1 setrgbcolor" << std::endl;
        file << " fill" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 0 0 setrgbcolor" << std::endl;
        file << " 0.0 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        file << "/greenfcirc    % stack: x y r" << std::endl;
        file << "{0 360 arc" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " 0 1 0 setrgbcolor" << std::endl;
        file << " fill" << std::endl;
        file << " grestore" << std::endl;
        file << " 0 0 0 setrgbcolor" << std::endl;
        file << " 0.0 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        
        file << "/quad_white      % stack: x1 y1 x2 y2 x3 y3 x4 y4" << std::endl;
        file << "{newpath" << std::endl;
        file << " moveto" << std::endl;
        file << " lineto" << std::endl;
        file << " lineto" << std::endl;
        file << " lineto" << std::endl;
        file << " closepath" << std::endl;
        file << " gsave" << std::endl;
        file << " 1.0 setgray fill" << std::endl;
        file << " grestore" << std::endl;
        file << "} def" << std::endl;
        
        file << "/quad_bold      % stack: x1 y1 x2 y2 x3 y3 x4 y4" << std::endl;
        file << "{newpath" << std::endl;
        file << " moveto" << std::endl;
        file << " lineto" << std::endl;
        file << " lineto" << std::endl;
        file << " lineto" << std::endl;
        file << " closepath" << std::endl;
        file << " 0.01 setlinewidth" << std::endl;
        file << " stroke" << std::endl;
        file << "} def" << std::endl;
        
        for (size_t isample = 0; isample < _num_inserted_points; isample++)
        {
            size_t num_neighbors = 0;
            if (_sample_neighbors[isample] != 0) num_neighbors = _sample_neighbors[isample][0];
            
            for (size_t i = 1; i <= num_neighbors; i++)
            {
                size_t neighbor = _sample_neighbors[isample][i];
                // draw a line between isample and neighbor
                file << _sample_points[isample][0] * scale << "  " << _sample_points[isample][1] * scale << "  ";
                file << _sample_points[neighbor][0] * scale << "  " << _sample_points[neighbor][1] * scale << "  ";
                file << "blueseg"     << std::endl;
            }
        }
        
        double s(0.002 * _diag);
        for (size_t index = 0; index < _num_inserted_points; index++)
        {
            // plot vertex
            file << _sample_points[index][0] * scale << "  " << _sample_points[index][1] * scale << "  " << s * scale << " ";
            file << "blackfcirc"     << std::endl; // non-failure disk
        }
        
        double DX = _xmax[0] - _xmin[0];
        double DY = _xmax[1] - _xmin[1];
        
        // plot domain boundaries
        file << (_xmin[0] - DX) * scale << "  "  << _xmin[1]        * scale << "  ";
        file << (_xmax[0] + DX) * scale << "  "  <<  _xmin[1]       * scale << "  ";
        file << (_xmax[0] + DX) * scale << "  "  << (_xmin[1] - DY) * scale << "  ";
        file << (_xmin[0] - DX) * scale << "  "  << (_xmin[1] - DY) * scale << "  ";
        file << "quad_white"      << std::endl;
        
        file << (_xmin[0] - DX) * scale << "  "  <<  _xmax[1]        * scale << "  ";
        file << (_xmax[0] + DX) * scale << "  "  <<  _xmax[1]        * scale << "  ";
        file << (_xmax[0] + DX) * scale << "  "  <<  (_xmax[1] + DY) * scale << "  ";
        file << (_xmin[0] - DX) * scale << "  "  <<  (_xmax[1] + DY) * scale << "  ";
        file << "quad_white"      << std::endl;
        
        file <<  _xmax[0]       * scale << "  "  <<  (_xmin[1] - DY) * scale << "  ";
        file << (_xmax[0] + DX) * scale << "  "  <<  (_xmin[1] - DY) * scale << "  ";
        file << (_xmax[0] + DX) * scale << "  "  <<  (_xmax[1] + DY) * scale << "  ";
        file <<  _xmax[0]       * scale << "  "  <<  (_xmax[1] + DY) * scale << "  ";
        file << "quad_white"      << std::endl;
        
        file << (_xmin[0] - DX) * scale << "  "  <<  (_xmin[1] - DY) * scale << "  ";
        file <<  _xmin[0]       * scale << "  "  <<  (_xmin[1] - DY) * scale << "  ";
        file <<  _xmin[0]       * scale << "  "  <<  (_xmax[1] + DY) * scale << "  ";
        file << (_xmin[0] - DX) * scale << "  "  <<  (_xmax[1] + DY) * scale << "  ";
        file << "quad_white"      << std::endl;
        
        
        // plot domain boundaries
        file << _xmin[0] * scale << "  " << _xmin[1] * scale << "  ";
        file << _xmax[0] * scale << "  " << _xmin[1] * scale << "  ";
        file << _xmax[0] * scale << "  " << _xmax[1] * scale << "  ";
        file << _xmin[0] * scale << "  " << _xmax[1] * scale << "  ";
        file << "quad_bold"      << std::endl;
        
        file << "showpage" << std::endl;

        
    }

    
    



} // namespace Dakota