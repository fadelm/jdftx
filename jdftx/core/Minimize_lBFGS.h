/*-------------------------------------------------------------------
Copyright 2014 Ravishankar Sundararaman

This file is part of JDFTx.

JDFTx is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

JDFTx is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with JDFTx.  If not, see <http://www.gnu.org/licenses/>.
-------------------------------------------------------------------*/

#ifndef JDFTX_CORE_MINIMIZE_LBFGS_H
#define JDFTX_CORE_MINIMIZE_LBFGS_H

#include <memory>
#include <list>
#include <stack>

//! @addtogroup Algorithms
//! @{

//! L-BFGS algorithm following \cite LBFGS
template<typename Vector> double Minimizable<Vector>::lBFGS(const MinimizeParams& p)
{	
	Vector g, Kg; //gradient and preconditioned gradient
	double E = sync(compute(&g, &Kg)); //get initial energy and gradient
	
	EdiffCheck ediffCheck(p.nEnergyDiff, p.energyDiffThreshold); //list of past energies

	double alpha = 0.; //step size (note BFGS always tries alpha=alphaTstart first, which is recommended to be 1)
	double linminTest = 0.;
	const char* knormName = p.maxThreshold ? "grad_max" : "|grad|_K";

	//History of variable and residual changes:
	struct History
	{	Vector s; //change in variable (= alpha d)
		Vector Ky; //change in preconditioned residual (= Kg - KgPrev)
		double rho; //= 1/dot(s,y)
	};
	std::list< std::shared_ptr<History> > history;
	double gamma = 0.; //scaling: set to dot(s,y)/dot(y,Ky) each iteration
	
	//Select the linmin method:
	Linmin linmin = getLinmin(p);
	
	//Iterate until convergence, max iteration count or kill signal
	int iter=0;
	for(iter=0; !killFlag; iter++)
	{	
		if(report(iter)) //optional reporting/processing
		{	E = sync(compute(&g, &Kg)); //update energy and gradient if state was modified
			fprintf(p.fpLog, "%s\tState modified externally: resetting history.\n", p.linePrefix);
			fflush(p.fpLog);
			history.clear();
		}
		
		double gKnorm = sync(dot(g,Kg));
		double knormValue = p.maxThreshold ? p.maxCalculator(&Kg) : sqrt(gKnorm/p.nDim);
		fprintf(p.fpLog, "%sIter: %3d  %s: ", p.linePrefix, iter, p.energyLabel);
		fprintf(p.fpLog, p.energyFormat, E);
		fprintf(p.fpLog, "  %s: %10.3le", knormName, knormValue);
		if(alpha) fprintf(p.fpLog, "  alpha: %10.3le", alpha);
		if(linminTest) fprintf(p.fpLog, "  linmin: %10.3le", linminTest);
		fprintf(p.fpLog, "  t[s]: %9.2lf", clock_sec());
		
		//Check stopping conditions:
		fprintf(p.fpLog, "\n"); fflush(p.fpLog);
		int nConverged = 0;
		ostringstream ossConverged;
		if(fabs(knormValue) < p.knormThreshold)
		{	ossConverged << knormName << "<" << std::scientific << p.knormThreshold;
			nConverged++;
		}
		if(ediffCheck.checkConvergence(E))
		{	if(nConverged) ossConverged << ", ";
			ossConverged << "|Delta " << p.energyLabel << "|<"
				<< std::scientific << p.energyDiffThreshold
				<< " for " << p.nEnergyDiff << " iters";
			nConverged++;
		}
		if(nConverged >= (p.convergeAll ? 2 : 1))
		{	fprintf(p.fpLog, "%sConverged (%s).\n", p.linePrefix, ossConverged.str().c_str());
			fflush(p.fpLog); return E;
		}
		if(!std::isfinite(gKnorm))
		{	fprintf(p.fpLog, "%s|grad|_K=%le. Stopping ...\n", p.linePrefix, gKnorm);
			fflush(p.fpLog); return E;
		}
		if(!std::isfinite(E))
		{	fprintf(p.fpLog, "%sE=%le. Stopping ...\n", p.linePrefix, E);
			fflush(p.fpLog); return E;
		}
		if(iter>=p.nIterations) break;
		
		//Prepare container that will be committed to history below: (and avoid copies)
		auto h = std::make_shared<History>();
		Vector& d = h->s;
		
		//Compute search direction:
		d = clone(Kg);
		std::stack<double> a; //alpha in the reference renamed to 'a' here to not clash with step size
		for(auto h=history.rbegin(); h!=history.rend(); h++)
		{	a.push( (*h)->rho * sync(dot((*h)->s, d)) );
			axpy(-a.top(), (*h)->Ky, d);
		}
		if(gamma) d *= gamma; //scaling (available after first iteration)
		for(auto h=history.begin(); h!=history.end(); h++)
		{	double b = (*h)->rho * sync(dot((*h)->Ky, d));
			axpy(a.top()-b, (*h)->s, d);
			a.pop();
		}
		d *= -1;
		if((int)history.size() == p.history) history.pop_front(); //if full, make room for the upcoming history entry (do this here to free memory earlier)
		constrain(d); //restrict search direction to allowed subspace
		
		//Line minimization
		Vector y = clone(g); h->Ky = clone(Kg); //store previous gradients before linmin changes it (these will later be converted to y = g-gPrev)
		double alphaT = std::min(p.alphaTstart, safeStepSize(d));
		if(!linmin(*this, p, d, alphaT, alpha, E, g, Kg))
		{	if(p.abortOnFailedStep) die("%s\tStep failed: aborting.\n\n", p.linePrefix);
			//linmin failed:
			fprintf(p.fpLog, "%s\tUndoing step.\n", p.linePrefix);
			step(d, -alpha);
			E = sync(compute(&g, &Kg));
			if(history.size())
			{	//Failed, but not along the gradient direction:
				fprintf(p.fpLog, "%s\tStep failed: resetting history.\n", p.linePrefix);
				fflush(p.fpLog);
				history.clear();
				gamma = 0.;
				linminTest = 0.;
				continue;
			}
			else
			{	//Failed along the gradient direction
				fprintf(p.fpLog, "%s\tStep failed along negative gradient direction.\n", p.linePrefix);
				fprintf(p.fpLog, "%sProbably at roundoff error limit. (Stopping)\n", p.linePrefix);
				fflush(p.fpLog);
				return E;
			}
		}
		
		//Update history:
		linminTest = sync(dot(g,d))/sqrt(sync(dot(g,g))*sync(dot(d,d)));
		d *= alpha; //d -> alpha * d, which is change of state, and it resides in h->s
		h->Ky *= -1; axpy(1., Kg, h->Ky); //Ky = K(g-gPrev)
		y *= -1; axpy(1., g, y); //y = g-gPrev
		double ydots = sync(dot(y, h->s));
		h->rho = 1./ydots;
		gamma = ydots / sync(dot(y, h->Ky));
		history.push_back(h);
	}
	fprintf(p.fpLog, "%sNone of the convergence criteria satisfied after %d iterations.\n", p.linePrefix, iter);
	return E;
}

//! @}
#endif //JDFTX_CORE_MINIMIZE_LBFGS_H
