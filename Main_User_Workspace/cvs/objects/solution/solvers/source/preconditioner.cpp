/*
* LEGAL NOTICE
* This computer software was prepared by Battelle Memorial Institute,
* hereinafter the Contractor, under Contract No. DE-AC05-76RL0 1830
* with the Department of Energy (DOE). NEITHER THE GOVERNMENT NOR THE
* CONTRACTOR MAKES ANY WARRANTY, EXPRESS OR IMPLIED, OR ASSUMES ANY
* LIABILITY FOR THE USE OF THIS SOFTWARE. This notice including this
* sentence must appear on any copies of this computer software.
* 
* EXPORT CONTROL
* User agrees that the Software will not be shipped, transferred or
* exported into any country or used in any manner prohibited by the
* United States Export Administration Act or any other applicable
* export laws, restrictions or regulations (collectively the "Export Laws").
* Export of the Software may require some form of license or other
* authority from the U.S. Government, and failure to obtain such
* export control license may result in criminal liability under
* U.S. laws. In addition, if the Software is identified as export controlled
* items under the Export Laws, User represents and warrants that User
* is not a citizen, or otherwise located within, an embargoed nation
* (including without limitation Iran, Syria, Sudan, Cuba, and North Korea)
*     and that User is not otherwise prohibited
* under the Export Laws from receiving the Software.
* 
* Copyright 2011 Battelle Memorial Institute.  All Rights Reserved.
* Distributed as open-source under the terms of the Educational Community 
* License version 2.0 (ECL 2.0). http://www.opensource.org/licenses/ecl2.php
* 
* For further details, see: http://www.globalchange.umd.edu/models/gcam/
*
*/


/*! 
* \file preconditioner.cpp
* \ingroup objects
* \brief Preconditioner source file.
* \author Robert Link
*/

#include "util/base/include/definitions.h"
#include <string>
#include <iomanip>
#include <xercesc/dom/DOMNode.hpp>
#include <xercesc/dom/DOMNodeList.hpp>

#include "solution/solvers/include/solver_component.h"
#include "solution/solvers/include/preconditioner.hpp"
#include "solution/util/include/calc_counter.h"
#include "marketplace/include/marketplace.h"
#include "containers/include/world.h"
#include "solution/util/include/solution_info.h"
#include "solution/util/include/solution_info_set.h"
#include "solution/util/include/solver_library.h"
#include "util/base/include/util.h"
#include "util/logger/include/ilogger.h"
#include "util/base/include/xml_helper.h"
#include "solution/util/include/solution_info_filter_factory.h"
// TODO: this filter is hard coded here since it is the default, is this ok?
#include "solution/util/include/solvable_solution_info_filter.h"

#include "util/base/include/timer.h"

using namespace std;
using namespace xercesc;

//! Default Constructor. Constructs the base class. 
Preconditioner::Preconditioner( Marketplace* marketplaceIn, World* worldIn, CalcCounter* calcCounterIn ) :
  SolverComponent( marketplaceIn, worldIn, calcCounterIn ),
  mItmax(2),
  mPriceIncreaseFac(0.25),
  mPriceDecreaseFac(0.1),
  mLargePrice(1.0e6),
  mFTOL(util::getSmallNumber())
{
}

//! Init method.
void Preconditioner::init() {
    if( !mSolutionInfoFilter.get() ) {
        // note we are hard coding this as the default
        mSolutionInfoFilter.reset( new SolvableSolutionInfoFilter() );
    }
}

//! Get the name of the SolverComponent
const string& Preconditioner::getXMLNameStatic() {
    const static string SOLVER_NAME = "preconditioner-solver-component";
    return SOLVER_NAME;
}

//! Get the name of the SolverComponent
const string& Preconditioner::getXMLName() const {
    return getXMLNameStatic();
}

bool Preconditioner::XMLParse( const DOMNode* aNode ) {
    // assume we were passed a valid node.
    assert( aNode );
    
    // get the children of the node.
    DOMNodeList* nodeList = aNode->getChildNodes();
    
    // loop through the children
    for ( unsigned int i = 0; i < nodeList->getLength(); ++i ){
        DOMNode* curr = nodeList->item( i );
        string nodeName = XMLHelper<string>::safeTranscode( curr->getNodeName() );
        
        if( nodeName == "#text" ) {
            continue;
        }
        else if( nodeName == "max-iterations" || nodeName == "itmax") {
            mItmax = XMLHelper<unsigned int>::getValue( curr );
        }
        else if( nodeName == "price-increase-fac" ) {
            mPriceIncreaseFac = XMLHelper<double>::getValue( curr );
        }
        else if( nodeName == "price-decrease-fac" ) {
            mPriceDecreaseFac = XMLHelper<double>::getValue( curr );
        }
        else if( nodeName == "large-price-thresh") {
            mLargePrice = XMLHelper<double>::getValue(curr);
        }
        else if( nodeName == "ftol") {
            mFTOL = XMLHelper<double>::getValue(curr);
        }
        else if( nodeName == "solution-info-filter" ) {
            mSolutionInfoFilter.reset(
                SolutionInfoFilterFactory::createSolutionInfoFilterFromString( XMLHelper<string>::getValue( curr ) ) );
        }
        else if( SolutionInfoFilterFactory::hasSolutionInfoFilter( nodeName ) ) {
            mSolutionInfoFilter.reset( SolutionInfoFilterFactory::createAndParseSolutionInfoFilter( nodeName, curr ) );
        }
        else {
            ILogger& mainLog = ILogger::getLogger( "main_log" );
            mainLog.setLevel( ILogger::WARNING );
            mainLog << "Unrecognized text string: " << nodeName << " found while parsing "
                << getXMLNameStatic() << "." << endl;
        }
    }
    return true;
}

/*! \brief Market Preconditioner
  
 * \details The preconditioner is intended to run before any other
 *          solver, and just once per period.  The goal is to filter
 *          out any price inputs that are in ranges that will cause the
 *          Newton-type solvers to flip out (e.g. by providing
 *          misleading derivatives).  We use two heuristics to adjust
 *          prices:
 *          1) supply < epsilon && demand > supply: increase
 *            prices by the increase-price-increment until one of these
 *            conditions is no longer true.
 *          2) supply > demand: decrease prices by the
 *             decrease-price-increment until supply changes
 *             (probably a decrease) by at least 10% of its original
 *             value, or until demand > supply
 * \param aSolutionSet Initial set of objects with information on each market which may be filtered.
 * \param aPeriod Model period.
 */
SolverComponent::ReturnCode Preconditioner::solve( SolutionInfoSet& aSolutionSet, const int aPeriod )
{
    // Setup Logging.
    ILogger& solverLog = ILogger::getLogger( "solver_log" );
    solverLog.setLevel( ILogger::NOTICE );
    ILogger& worstMarketLog = ILogger::getLogger( "worst_market_log" );
    worstMarketLog.setLevel( ILogger::NOTICE );

    const SolutionInfo* maxred = aSolutionSet.getWorstSolutionInfo();
    addIteration(maxred->getName(), maxred->getRelativeED());
    worstMarketLog << "###Preconditioner-strt: " << *maxred << std::endl;

    
    // If all markets are solved, then return with success code.
    if( aSolutionSet.isAllSolved() ){
        solverLog << "Preconditioner:  All solvable markets are solved.  Returning.\n\n";
        return SolverComponent::SUCCESS;
    }

    // for now we'll just record this in the "bisect timer", since
    // this solver is intended as a functional replacement for
    // bisection
    Timer& bisectTimer = TimerRegistry::getInstance().getTimer( TimerRegistry::BISECT );
    bisectTimer.start();
    
    // need to do bracketing first, does this need to be before or after startMethod?
    solverLog << "Solution set before Preconditioning: " << endl << aSolutionSet << endl;
    
    
    startMethod();
    
    worstMarketLog << "Market Name, X, XL, XR, ED, EDL, EDR, RED, bracketed, supply, demand" << endl;
    solverLog << "Preconditioning routine starting" << endl; 

    aSolutionSet.updateSolvable( mSolutionInfoFilter.get() );
    
    if( aSolutionSet.getNumSolvable() == 0 ) {
        solverLog << "Exiting Preconditioning early due to empty solvable set." << endl;
        return SUCCESS;
    }
    
    // set up some scratch data
    int nmkt = aSolutionSet.getNumSolvable();
    std::vector<SolutionInfo> solvable = aSolutionSet.getSolvableSet();

    solverLog << "Preconditioning " << nmkt << " markets.\n";

    for(int pass=0; pass<mItmax; ++pass) {
        solverLog << "pass " << pass << "\n";
        solverLog << "p0      \tp1      \tpold     \t pnew    \tsold    \tdold    \tName\n";

        int nchg = 0;
        for(int i=0; i<nmkt; ++i) {
            double oldprice = solvable[i].getPrice();
            double oldsply  = solvable[i].getSupply();
            double olddmnd  = solvable[i].getDemand();
            double newprice = oldprice;
            bool chg = false;
            double lb,ub;       // only used for normal markets, but need to be declared up here.

            if(!util::isValidNumber(oldprice) || !util::isValidNumber(oldsply) ||
               !util::isValidNumber(olddmnd) || fabs(oldprice) > 1.0e16) {
                // something is badly messed up.  Reset price to something
                // benign and hope for the best.
                newprice = std::max(1.0, fabs(solvable[i].getLowerBoundSupplyPrice()));
                solvable[i].setPrice(newprice);
                chg = true;
                ++nchg;
            }
            else {
                switch(solvable[i].getType()) {
                case IMarketType::NORMAL:
                case IMarketType::TRIAL_VALUE:
                    // select a lower bound just a bit above the bottom of the supply curve.
                    lb = solvable[i].getLowerBoundSupplyPrice();
                    lb += 1.0e-5 * std::max(1.0, fabs(lb));
                    // select an upper bound just a bit below the top of the supply curve.
                    ub = solvable[i].getUpperBoundSupplyPrice();
                    ub -= 1.0e-5 * std::max(1.0, fabs(ub));
                    if(oldprice < lb &&
                       oldsply < olddmnd && olddmnd > mFTOL
                        ) {
                        // price is below the bottom of the supply curve,
                        // and there is excess demand: set new price a bit
                        // above the bottom of the curve.
                        newprice = lb + 0.01*std::max(1.0, fabs(lb)); 
                        // sometimes the range of valid prices is really
                        // narrow and the above can actually overshoot.
                        if(newprice >= solvable[i].getUpperBoundSupplyPrice())
                            newprice = 0.5*(solvable[i].getLowerBoundSupplyPrice() + solvable[i].getUpperBoundSupplyPrice());
                        solvable[i].setPrice(newprice);
                        chg = true;
                        ++nchg;
                    }
                    else if(oldprice <= 0.0 &&
                            oldsply < olddmnd
                        ) {
                        // we have a negative price, but there is some
                        // demand, and supply is less than demand.
                        // This clause catches all the land categories
                        // for which we have no bounds on the supply
                        // curve (supply is determined by the land
                        // allocator), but for which price probably
                        // shouldn't be negative.
                        newprice = 0.001;
                        solvable[i].setPrice(newprice);
                        chg = true;
                        ++nchg;
                    } 
                    else if (oldprice > ub &&
                             oldsply > olddmnd) {
                        // price is above the top of the supply curve,
                        // and there little demand.  Set new price at
                        // the top of the supply curve.  We are
                        // conservative about this adjustment because
                        // it frequently happens that the supply of
                        // certain resources runs out, and the
                        // clearing price will be above the top of the
                        // supply curve.
                        newprice = ub;
                        solvable[i].setPrice(newprice);
                        chg = true;
                        ++nchg;
                    }
                    break; 
                case IMarketType::PRICE:
                    // price markets are solving a consistency
                    // condition, but we want to be a bit more
                    // conservative with them than with demand markets
                    // because large changes in prices can have
                    // far-reaching effects.  If we have a negative
                    // trial price and the model price is positive,
                    // then adjust the trial price minimally into
                    // positive territory.  Let the other solvers do
                    // the rest.  This adjustment is not limited to
                    // the second pass.
                    if(olddmnd > 0.0 && oldprice <= 0.0) {
                        newprice = std::min(0.1, olddmnd);
                        solvable[i].setPrice(newprice);
                        chg = true;
                        ++nchg;
                    }
                    break;
                case IMarketType::DEMAND:
                    // These markets all set their supply equal to
                    // their input.  In principle we should precondition
                    // them by setting the input value ("price")
                    // to the current "demand" value.  However in practice there
                    // are situations where the demand value gets inflated to some
                    // ridiculous range (a negative price makes it's way to a final
                    // demand) and not adjusting the input value ("price") is
                    // actually beneficial to allowing the solution mechanism to
                    // quickly walk back to a reasonable range.  Perhaps we can
                    // add this heuristic back in when we can better characterize
                    // when it would be safe to do so.  Also note we should ignore these
                    // on the first pass because changes to the normal markets might
                    // greatly affect the trial values.

                    // Never intentionally set a trial demand or trial value
                    // to something less than zero.  If a trial demand
                    // is less than zero, set it to a small positive
                    // number.
                    if(pass>0) {
                        if(olddmnd <= 0.0) {
                            newprice = util::getSmallNumber();
                            solvable[i].setPrice(newprice);
                            chg = true;
                            ++nchg;
                        }
                        
                    } 
                    break; 
                default:
                    // no action for tax markets, etc.
                    chg = false;
                }
            }
            char marker = chg ? '*' : ' ';
            solverLog << std::setw(8) << solvable[i].getLowerBoundSupplyPrice() << "\t"
                      << std::setw(8) << solvable[i].getUpperBoundSupplyPrice() << "\t"
                      << std::setw(8) << oldprice << "\t"
                      << marker << std::setw(8) << newprice << "\t"
                      << std::setw(8) << oldsply << "\t"
                      << std::setw(8) << olddmnd << "\t"
                      << solvable[i].getName() << "\n";
            if(nchg==0 && pass > 2)
                // no additional effect from further passes.
                break;
        }

        marketplace->nullSuppliesAndDemands( aPeriod );
#if GCAM_PARALLEL_ENABLED
        world->calc(aPeriod, world->getGlobalFlowGraph());
#else
        world->calc(aPeriod);
#endif
    addIteration(maxred->getName(), maxred->getRelativeED());
    worstMarketLog << "###Preconditioner-" << pass << ": " << *maxred << std::endl; 
    } // end of loop over two passes
    bisectTimer.stop();

    maxred = aSolutionSet.getWorstSolutionInfo();
    addIteration(maxred->getName(), maxred->getRelativeED());
    worstMarketLog << "###Preconditioner-end: " << *maxred << std::endl;
    
    // Report exit conditions.  Technically it's possible that the
    // last re-evaluation could have "fixed" the last remaining
    // troublesome markets, so we ought to test that here, but
    // honestly it doesn't matter that much, so we are just going to
    // bag it.
    solverLog.setLevel( ILogger::NOTICE );
    solverLog << "Exiting Preconditioner due to reaching the maximum number of iterations." << endl;
    return SUCCESS;
}
